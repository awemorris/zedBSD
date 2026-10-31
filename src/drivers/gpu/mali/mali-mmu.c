/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The memory the Mali GPU reaches, and its address spaces.
 *
 * The GPU on this SoC does not snoop the processor's caches, so every
 * buffer and every translation table page is written through an uncached
 * view of RAM, and the GPU reads what the processor wrote without cache
 * maintenance.  Each address space keeps its table pages until it is
 * destroyed; a change of the tables of a slot that is translating is
 * followed by a flush of that slot's view, so the GPU forgets an old
 * translation it may have cached.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/mali/mali-private.h"
#include "drivers/gpu/mali/mali-regs.h"

/* How long a register poll sleeps before the next read, in milliseconds (a tick at least). */
#define MALI_MMU_POLL_MS 1U

/* How long a slot may stay busy with an earlier command (100 ms). */
#define SLOT_WAIT_MS		100U

/*
 * The smallest region the MMU locks, as a power of two, and the field of
 * the lock address register that holds the region's width.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_mmu_regs.h
 * (AS_LOCK_REGION_MIN_SIZE); panthor_mmu.c (pack_region_range: the width
 * is the base-two logarithm of the region minus one, in the low bits).
 */
#define LOCK_REGION_MIN_SHIFT	15U
#define LOCK_ADDRESS_BITS	64U

static int table_allocate(void *context, uint64_t *physical, uint64_t **table);
static uint64_t *table_view(void *context, uint64_t physical);
static int send_command(struct mali_device *device, unsigned slot, uint32_t command);
static int wait_slot_idle(struct mali_device *device, unsigned slot);
static uint64_t lock_region(uint64_t gpu_address, uint64_t bytes);
static int place_buffer(struct mali_device *device, struct mali_address_space *space, uint64_t *next, uint64_t end, struct mali_buffer *buffer, uint64_t bytes, unsigned flags);

/* The table memory of every address space: the space's own pages. */
static const struct mali_pt_memory table_memory = {
	table_allocate,
	table_view,
};

/*
 * Allocates a buffer of whole pages below the GPU's reach, zeroed, with
 * its uncached view.
 */
int
mali_buffer_alloc(
	struct mali_buffer *buffer,
	uint64_t bytes)
{
	void *view;
	uint64_t rounded;
	int error;

	/* Starts empty, so a failure leaves nothing to free. */
	kern_memset(buffer, 0, sizeof(*buffer));
	if (bytes == 0U)
		return EINVAL;

	/* Whole pages, so a mapping of the run maps nothing else. */
	rounded = (bytes + MALI_PAGE_BYTES - 1U) & ~(MALI_PAGE_BYTES - 1U);

	/* The run, below the address the GPU can name. */
	error = kern_pmem_alloc_limited((size_t)rounded, (size_t)MALI_PAGE_BYTES, MALI_DMA_LIMIT, 0U, &buffer->run);
	if (error != 0)
		return error;

	/* The uncached view the processor reads and writes the run through. */
	error = kern_pmem_map_uncached(&buffer->run, &view);
	if (error != 0) {
		(void)kern_pmem_free(&buffer->run);
		kern_memset(buffer, 0, sizeof(*buffer));
		return error;
	}

	/* New memory holds zeroes, not an earlier owner's bytes. */
	buffer->view = view;
	buffer->bytes = rounded;
	kern_memset((void *)(uintptr_t)buffer->view, 0, (size_t)rounded);

	/* Succeeded: the buffer is ready to be mapped. */
	return 0;
}

/*
 * Frees a buffer's view and its run.  The caller has made sure the GPU
 * no longer reads it.
 */
void
mali_buffer_free(
	struct mali_buffer *buffer)
{
	/* A buffer that was never allocated has nothing to free. */
	if (buffer->view == NULL)
		return;

	/* The view first, then the run. */
	(void)kern_pmem_unmap_uncached((void *)(uintptr_t)buffer->view, (size_t)buffer->bytes);
	(void)kern_pmem_free(&buffer->run);
	kern_memset(buffer, 0, sizeof(*buffer));
}

/*
 * Makes the root table of an address space that will be bound to a slot.
 *
 * The space is as wide as the GPU's virtual addresses, at most 48 bits.
 */
int
mali_as_init(
	struct mali_device *device,
	struct mali_address_space *space,
	unsigned slot)
{
	unsigned bits;
	int error;

	/* Refuses a slot the GPU does not have. */
	if (slot >= 32U || (device->address_spaces & (1U << slot)) == 0U)
		return ENODEV;

	/* Starts without tables, bound to the slot but not yet translating. */
	kern_memset(space, 0, sizeof(*space));
	space->slot = slot;

	/* The width the GPU reports, kept inside what the builder makes. */
	bits = device->virtual_bits;
	if (bits > 48U)
		bits = 48U;
	if (bits < 32U)
		bits = 32U;

	/* The root table. */
	error = mali_pt_init(&space->tables, bits, &table_memory, space);
	if (error != 0)
		return error;

	/* Succeeded: the space is empty and ready for mappings. */
	return 0;
}

/*
 * Maps a buffer at a GPU address of a space, and flushes the slot's view
 * of the run when the slot is translating.
 */
int
mali_as_map(
	struct mali_device *device,
	struct mali_address_space *space,
	uint64_t gpu_address,
	struct mali_buffer *buffer,
	unsigned flags)
{
	int error;

	/* Writes the entries of every page of the run. */
	error = mali_pt_map(&space->tables, gpu_address, buffer->run.paddr, buffer->bytes, flags);
	if (error != 0)
		return error;

	/* The address the buffer is seen at from now on. */
	buffer->gpu_address = gpu_address;

	/* A slot already translating may hold old entries of the range. */
	if (space->enabled) {
		error = mali_as_flush(device, space, gpu_address, buffer->bytes);
		if (error != 0)
			return error;
	}

	/* Succeeded: the GPU sees the buffer at the address. */
	return 0;
}

/*
 * Binds a space to its slot: the tables, the memory attributes and the
 * translation mode, then the update command.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_mmu.c
 * (panthor_mmu_as_enable, panthor_vm_active).
 */
int
mali_as_enable(
	struct mali_device *device,
	struct mali_address_space *space)
{
	uint64_t memory_attributes;
	uint64_t configuration;
	uint32_t base;
	int error;

	/* Slot 0 non-cacheable (everything the processor shares), slot 1 write-back. */
	memory_attributes = MALI_MEMATTR_UNCACHED << (8U * MALI_MEMATTR_SLOT_UNCACHED);
	memory_attributes |= MALI_MEMATTR_WRITEBACK << (8U * MALI_MEMATTR_SLOT_WRITEBACK);

	/* AArch64 tables of 4 KiB pages, as wide as the space, walked uncached. */
	configuration = MALI_TRANSCFG_AARCH64_4K;
	configuration |= (uint64_t)(MALI_TRANSCFG_INPUT_BASE - space->tables.virtual_bits) << MALI_TRANSCFG_INPUT_SHIFT;
	configuration |= MALI_TRANSCFG_WALK_UNCACHED;

	/* Writes the three registers of the slot. */
	base = MALI_AS_BASE + space->slot * MALI_AS_STRIDE;
	mali_write64(device, base + MALI_AS_TRANSLATION_TABLE, space->tables.root_physical);
	mali_write64(device, base + MALI_AS_MEMORY_ATTRIBUTES, memory_attributes);
	mali_write64(device, base + MALI_AS_TRANSLATION_CONFIG, configuration);

	/* Makes the slot take them. */
	error = send_command(device, space->slot, MALI_AS_COMMAND_UPDATE);
	if (error != 0) {
		kern_logf("mali: address space %u not enabled (status %08x)\n",
			  space->slot,
			  (unsigned)mali_read32(device, base + MALI_AS_STATUS));
		return error;
	}

	/* From now on a change of the tables needs a flush. */
	space->enabled = 1;

	/* Succeeded: the slot translates through the space. */
	return 0;
}

/*
 * Stops a slot translating: its caches are flushed and it is set to
 * translate nothing.
 */
void
mali_as_disable(
	struct mali_device *device,
	struct mali_address_space *space)
{
	uint32_t base;

	/* What the GPU wrote through the slot goes to memory first. */
	(void)mali_gpu_flush_caches(device, MALI_CACHE_CLEAN | MALI_CACHE_INVALIDATE, MALI_CACHE_CLEAN | MALI_CACHE_INVALIDATE, MALI_CACHE_INVALIDATE);

	/* No tables and the mode that translates nothing. */
	base = MALI_AS_BASE + space->slot * MALI_AS_STRIDE;
	mali_write64(device, base + MALI_AS_TRANSLATION_TABLE, 0U);
	mali_write64(device, base + MALI_AS_MEMORY_ATTRIBUTES, 0U);
	mali_write64(device, base + MALI_AS_TRANSLATION_CONFIG, MALI_TRANSCFG_UNMAPPED);
	(void)send_command(device, space->slot, MALI_AS_COMMAND_UPDATE);
	space->enabled = 0;
}

/*
 * Makes a translating slot forget what it cached of a range of its
 * space: the region is locked, the GPU's caches are cleaned and
 * invalidated, and the region is unlocked.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_mmu.c
 * (panthor_vm_lock_region, panthor_vm_unlock_region).
 */
int
mali_as_flush(
	struct mali_device *device,
	struct mali_address_space *space,
	uint64_t gpu_address,
	uint64_t bytes)
{
	uint32_t base;
	int error;

	/* Nothing to forget for an empty range or a slot that is not translating. */
	if (bytes == 0U || !space->enabled)
		return 0;

	/* Locks the smallest aligned region that covers the range. */
	base = MALI_AS_BASE + space->slot * MALI_AS_STRIDE;
	mali_write64(device, base + MALI_AS_LOCK_ADDRESS, lock_region(gpu_address, bytes));
	error = send_command(device, space->slot, MALI_AS_COMMAND_LOCK);
	if (error != 0)
		return error;

	/* The caches lose what they held of the old translation. */
	error = mali_gpu_flush_caches(device, MALI_CACHE_CLEAN | MALI_CACHE_INVALIDATE, MALI_CACHE_CLEAN | MALI_CACHE_INVALIDATE, MALI_CACHE_INVALIDATE);

	/* Unlocks the region also after a failed flush, so the slot does not stay stalled. */
	(void)send_command(device, space->slot, MALI_AS_COMMAND_UNLOCK);
	if (error != 0)
		return error;

	/* Succeeded: the slot reads the new tables. */
	return 0;
}

/*
 * Allocates a buffer and maps it at the next free place of the
 * microcontroller's part for the driver's objects.
 */
int
mali_mcu_alloc(
	struct mali_device *device,
	struct mali_buffer *buffer,
	uint64_t bytes,
	unsigned flags)
{
	int error;

	/* The next place after the firmware's sections in the shared region. */
	error = place_buffer(device, &device->mcu, &device->mcu_next, MALI_MCU_KERNEL_END, buffer, bytes, flags);
	if (error != 0)
		return error;

	/* Succeeded: the firmware sees the buffer at its address. */
	return 0;
}

/*
 * Allocates a buffer and maps it at the next free place of the driver's
 * group's space.
 */
int
mali_group_alloc(
	struct mali_device *device,
	struct mali_buffer *buffer,
	uint64_t bytes,
	unsigned flags)
{
	int error;

	/* The next place of the group's part. */
	error = place_buffer(device, &device->group, &device->group_next, MALI_GROUP_SPACE_END, buffer, bytes, flags);
	if (error != 0)
		return error;

	/* Succeeded: the group's streams see the buffer at its address. */
	return 0;
}

/*
 * Removes a run of a space's mappings, and flushes the slot's view of
 * the run when the slot is translating.  The tables stay.
 */
int
mali_as_unmap(
	struct mali_device *device,
	struct mali_address_space *space,
	uint64_t gpu_address,
	uint64_t bytes)
{
	int error;

	/* Clears the entries of every page of the run. */
	error = mali_pt_unmap(&space->tables, gpu_address, bytes);
	if (error != 0)
		return error;

	/* A slot that translates may still hold the old entries. */
	if (space->enabled) {
		error = mali_as_flush(device, space, gpu_address, bytes);
		if (error != 0)
			return error;
	}

	/* Succeeded: the GPU no longer reaches the run. */
	return 0;
}

/*
 * Frees every table page of a space that no slot translates through any
 * more.
 */
void
mali_as_destroy(
	struct mali_address_space *space)
{
	unsigned index;

	/* Each page's view, then its run. */
	for (index = 0; index < space->table_count; index++) {
		(void)kern_pmem_unmap_uncached(space->table_views[index], (size_t)MALI_PT_PAGE_BYTES);
		(void)kern_pmem_free(&space->table_runs[index]);
	}

	/* Nothing is left of it. */
	kern_memset(space, 0, sizeof(*space));
}

/* Hands the builder one zeroed table page of the space, with its uncached view. */
static int
table_allocate(
	void *context,
	uint64_t *physical,
	uint64_t **table)
{
	struct mali_address_space *space;
	struct kern_pmem *run;
	void *view;
	int error;

	/* Refuses a table past the pages the space keeps. */
	space = context;
	if (space->table_count >= MALI_TABLE_PAGES_MAX)
		return ENOSPC;

	/* One page, aligned to its size, that the GPU can reach. */
	run = &space->table_runs[space->table_count];
	error = kern_pmem_alloc_limited((size_t)MALI_PT_PAGE_BYTES, (size_t)MALI_PT_PAGE_BYTES, MALI_DMA_LIMIT, 0U, run);
	if (error != 0)
		return error;

	/* Its uncached view, which the builder writes and the GPU's walk reads. */
	error = kern_pmem_map_uncached(run, &view);
	if (error != 0) {
		(void)kern_pmem_free(run);
		return error;
	}

	/* An empty table maps nothing. */
	kern_memset(view, 0, (size_t)MALI_PT_PAGE_BYTES);
	space->table_views[space->table_count] = view;
	space->table_count++;

	/* Succeeded: the builder owns the table until the space is destroyed. */
	*physical = run->paddr;
	*table = view;
	return 0;
}

/* Finds the uncached view of one of the space's table pages from its physical address. */
static uint64_t *
table_view(
	void *context,
	uint64_t physical)
{
	struct mali_address_space *space;
	unsigned index;

	/* Looks through the pages the space handed out. */
	space = context;
	for (index = 0U; index < space->table_count; index++) {
		if (space->table_runs[index].paddr == physical)
			return space->table_views[index];
	}

	/* A page the space never handed out. */
	return NULL;
}

/* Issues one command to a slot, after its last one, and waits for it to finish. */
static int
send_command(
	struct mali_device *device,
	unsigned slot,
	uint32_t command)
{
	uint32_t base;
	int error;

	/* The slot takes a command only when idle. */
	error = wait_slot_idle(device, slot);
	if (error != 0)
		return error;

	/* The command, then the wait for it to end. */
	base = MALI_AS_BASE + slot * MALI_AS_STRIDE;
	mali_write32(device, base + MALI_AS_COMMAND, command);
	error = wait_slot_idle(device, slot);
	if (error != 0)
		return error;

	/* Succeeded: the command is done. */
	return 0;
}

/*
 * Polls a slot's status until no command is active; ETIMEDOUT when it
 * stays busy.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_mmu.c (wait_ready).
 */
static int
wait_slot_idle(
	struct mali_device *device,
	unsigned slot)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t status;
	uint32_t base;

	/* Reads, then sleeps a tick, until idle or the time is spent. */
	base = MALI_AS_BASE + slot * MALI_AS_STRIDE;
	deadline = sched_ticks() + kern_ms_to_ticks(SLOT_WAIT_MS) + 1U;
	for (;;) {
		/* An idle slot takes the next command. */
		status = mali_read32(device, base + MALI_AS_STATUS);
		if ((status & MALI_AS_STATUS_ACTIVE) == 0U)
			return 0;

		/* The time is spent. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* A millisecond before the next read. */
		sched_sleep(now + kern_ms_to_ticks(MALI_MMU_POLL_MS));
	}

	/* Succeeded in waiting, but the slot stayed busy. */
	kern_logf("mali: address space %u stays busy (status %08x)\n", slot, (unsigned)status);
	return ETIMEDOUT;
}

/*
 * Builds the lock address of the smallest naturally aligned power-of-two
 * region, at least the MMU's smallest, that covers a range: the region's
 * base with the logarithm of its size, minus one, in the low bits.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_mmu.c (pack_region_range).
 */
static uint64_t
lock_region(
	uint64_t gpu_address,
	uint64_t bytes)
{
	uint64_t last;
	uint64_t differing;
	unsigned width;

	/* The highest bit in which the first and last addresses differ decides the size. */
	last = gpu_address + bytes - 1U;
	differing = gpu_address ^ last;
	width = 0U;
	while (width < LOCK_ADDRESS_BITS && (differing >> width) != 0U)
		width++;

	/* At least the smallest region the MMU locks. */
	if (width < LOCK_REGION_MIN_SHIFT)
		width = LOCK_REGION_MIN_SHIFT;

	/* The region's aligned base and its encoded width. */
	gpu_address &= ~((1ULL << (width - 1U)) - 1U);

	/* Succeeded: the register's value. */
	return gpu_address | (uint64_t)(width - 1U);
}

/* Allocates a buffer and maps it at the next free place of a part of a space, bounded by end. */
static int
place_buffer(
	struct mali_device *device,
	struct mali_address_space *space,
	uint64_t *next,
	uint64_t end,
	struct mali_buffer *buffer,
	uint64_t bytes,
	unsigned flags)
{
	uint64_t address;
	int error;

	/* The memory first. */
	error = mali_buffer_alloc(buffer, bytes);
	if (error != 0)
		return error;

	/* Refuses a buffer the part has no room left for. */
	address = *next;
	if (address > end || buffer->bytes > end - address) {
		mali_buffer_free(buffer);
		return ENOSPC;
	}

	/* Maps it there; a failed map takes back the entries it wrote before the run is freed. */
	error = mali_as_map(device, space, address, buffer, flags);
	if (error != 0) {
		(void)mali_pt_unmap(&space->tables, address, buffer->bytes);
		mali_buffer_free(buffer);
		return error;
	}

	/* The next buffer goes after this one. */
	*next = address + buffer->bytes;

	/* Succeeded: the buffer is mapped. */
	return 0;
}
