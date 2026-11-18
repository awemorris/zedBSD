/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A2: the memory the GPU and the driver share.
 *
 * The stage maps the CPU's window on the VRAM (its first 256 MiB, without
 * the firmware's picture, which the console keeps using through its own
 * mapping) and the doorbell page, sets up the VRAM pools the later stages
 * allocate from, and builds context 0's flat table that maps system memory
 * into the GPU's address space.  It then sets up both memory hubs the same
 * way: the table and its range, the frame buffer's aperture, the default
 * pages, the translation and cache controls, contexts 1 to 15 for the
 * four-level tables of later stages, and the invalidation engines; turns
 * on the host data path's flush rule; and drops every cached translation
 * of context 0.  Last it prepares the interrupt ring, which stage A3 turns
 * on.
 *
 * The display keeps scanning the firmware's picture all along, through the
 * media hub's frame-buffer aperture, so the stage reads the plane's and the
 * timing generator's underflow bits before and after: the setup must not
 * starve the display.
 *
 * Every CPU write to VRAM goes through the host data path, which caches
 * writes; the driver flushes it before the GPU reads what the CPU wrote,
 * and drops its read cache before the CPU reads what the GPU wrote.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/dma.h>
#include <drivers/pci/pci-pcat.h>
#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/* The page size of the GPU's tables, and the bytes of one table entry. */
#define MEMORY_PAGE_BYTES		4096U
#define MEMORY_ENTRY_BYTES		8U

/* The contexts of a hub and the invalidation engines it has. */
#define MEMORY_CONTEXTS			16U
#define MEMORY_ENGINES			18U

/*
 * How long the CPU waits for an invalidation engine (its lock, then its
 * done bit), counted in register reads of about a microsecond each.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_device.c (the default usec_timeout
 * of 100000) and gmc_v10_0.c (gmc_v10_0_flush_gpu_tlb: a 1 us step).
 */
#define MEMORY_INVALIDATE_POLLS		100000U

/*
 * How long the display runs before its underflow bits are read again: a
 * few frames at any refresh rate the firmware sets.
 */
#define MEMORY_SETTLE_US		100000U

/* The entries of the system-memory table read back after it is written. */
#define MEMORY_CHECKED_ENTRIES		4U

/*
 * The start of the system-memory table's reach that is never handed out,
 * so that a GPU address of 0 never names a buffer.
 */
#define MEMORY_GART_GUARD_BYTES		(1024U * 1024U)

/*
 * The underflow bits of the display chain: the plane's three status bits
 * (30:28) and its clear bit (31), and the timing generator's input
 * underflow status (bit 10) and clear bit (12).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_STATUS,
 * HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_CLEAR,
 * ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_OCCURRED_STATUS,
 * ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_CLEAR) and
 * drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c
 * (hubp2_clear_underflow).
 */
#define MEMORY_PLANE_UNDERFLOW_SHIFT	28U
#define MEMORY_PLANE_UNDERFLOW_MASK	0x00000007U
#define MEMORY_PLANE_UNDERFLOW_CLEAR	0x80000000U
#define MEMORY_TIMING_UNDERFLOW		0x00000400U
#define MEMORY_TIMING_UNDERFLOW_CLEAR	0x00001000U

/* The driver's own bit, above the plane's three, that stands for the timing generator's bit. */
#define MEMORY_UNDERFLOW_TIMING_BIT	8U

/*
 * One hub register the stage reads back after the setup, with the value it
 * wrote.
 */
struct memory_check {
	enum amdrdna_hub_register which;
	unsigned instance;
	uint32_t wrote;
};

/*
 * The names the log gives the two hubs, in the order of enum amdrdna_hub.
 */
static const char *const hub_names[AMDRDNA_HUB_COUNT] = {
	"gfx",
	"mm"
};

static const struct amdrdna_ip *hub_block(struct amdrdna_device *device, enum amdrdna_hub hub);
static uint32_t hub_read(struct amdrdna_device *device, enum amdrdna_hub hub, enum amdrdna_hub_register which, unsigned instance);
static void hub_write(struct amdrdna_device *device, enum amdrdna_hub hub, enum amdrdna_hub_register which, unsigned instance, uint32_t value);
static int memory_blocks(struct amdrdna_device *device);
static uint32_t memory_underflow(struct amdrdna_device *device);
static void memory_underflow_clear(struct amdrdna_device *device);
static void memory_grow(struct amdrdna_device *device);
static int memory_windows(struct amdrdna_device *device);
static int memory_pools(struct amdrdna_device *device);
static int memory_table(struct amdrdna_device *device);
static volatile uint8_t *memory_cpu(struct amdrdna_device *device, uint64_t offset);
static void memory_mark_hubs(struct amdrdna_device *device);
static void memory_hub_setup(struct amdrdna_device *device, enum amdrdna_hub hub);
static void memory_hub_contexts(struct amdrdna_device *device, enum amdrdna_hub hub);
static int memory_hub_check(struct amdrdna_device *device, enum amdrdna_hub hub);
static void memory_restore_contexts(struct amdrdna_device *device);
static uint64_t memory_table_base(struct amdrdna_device *device);

/*
 * Runs stage A2: maps the VRAM window and the doorbells, builds the pools
 * and the system-memory table, sets up both memory hubs and the host data
 * path, and prepares the interrupt ring.
 *
 * Returns 0 when every step reported ok and the registers read back as
 * written, or the error of the step that failed; a failure after the hubs
 * were touched gives their contexts back the words the firmware left.
 */
int
amdrdna_a2_memory(
	struct amdrdna_device *device)
{
	uint32_t underflow;
	uint32_t word;
	int error;

	/* Says the stage starts. */
	amdrdna_mark("A2 begin");

	/* The blocks the stage writes must all be listed. */
	error = memory_blocks(device);
	if (error != 0)
		return error;

	/* Reads the display's underflow bits; a set bit left from before is cleared and read again. */
	underflow = memory_underflow(device);
	if (underflow != 0U) {
		memory_underflow_clear(device);
		kern_usleep_range(MEMORY_SETTLE_US, MEMORY_SETTLE_US + MEMORY_SETTLE_US / 4U);
		word = memory_underflow(device);
		amdrdna_mark("A2 underflow 0x%x before the stage, cleared, then 0x%x", (unsigned)underflow, (unsigned)word);
	}

	/* Records the bits the display shows before the stage writes anything. */
	device->underflow_before = memory_underflow(device);

	/* Grows the VRAM window to cover the VRAM when it can (a failure keeps the window as it is). */
	memory_grow(device);

	/* Maps the CPU's VRAM window and the doorbell page. */
	error = memory_windows(device);
	if (error != 0)
		return error;

	/* Builds the VRAM pools and the system-memory table's address space. */
	error = memory_pools(device);
	if (error != 0)
		return error;

	/* Builds the system-memory table in VRAM, every entry unused, and reads it back. */
	error = memory_table(device);
	if (error != 0)
		return error;

	/* Marks how the firmware left the hubs. */
	memory_mark_hubs(device);

	/* Sets up the graphics hub, then the media hub. */
	memory_hub_setup(device, AMDRDNA_HUB_GRAPHICS);
	memory_hub_setup(device, AMDRDNA_HUB_MEDIA);

	/* Makes a flush of the host data path also drop its read cache, and flushes it once. */
	word = amdrdna_block_read(device, AMDRDNA_BLOCK_HDP, AMDRDNA_HDP_WINDOW_MAIN, AMDRDNA_HDP_REG_MISC);
	word |= AMDRDNA_HDP_MISC_FLUSH_DROPS;
	amdrdna_block_write(device, AMDRDNA_BLOCK_HDP, AMDRDNA_HDP_WINDOW_MAIN, AMDRDNA_HDP_REG_MISC, word);
	amdrdna_host_path_flush(device);

	/* Sends a faulting access of the graphics hub to the default page instead of stopping its client. */
	word = hub_read(device, AMDRDNA_HUB_GRAPHICS, AMDRDNA_HUB_FAULT_CONTROL, 0U);
	word = amdrdna_hub_fault_default_word(word);
	hub_write(device, AMDRDNA_HUB_GRAPHICS, AMDRDNA_HUB_FAULT_CONTROL, 0U, word);

	/* The same on the media hub. */
	word = hub_read(device, AMDRDNA_HUB_MEDIA, AMDRDNA_HUB_FAULT_CONTROL, 0U);
	word = amdrdna_hub_fault_default_word(word);
	hub_write(device, AMDRDNA_HUB_MEDIA, AMDRDNA_HUB_FAULT_CONTROL, 0U, word);

	/* Lets the doorbell BAR reach the engines. */
	word = amdrdna_block_read(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_DOORBELL_APERTURE);
	word |= AMDRDNA_DOORBELL_APERTURE_ON;
	amdrdna_block_write(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_DOORBELL_APERTURE, word);

	/* Drops context 0's cached translations, on the media hub first. */
	error = amdrdna_tlb_flush(device, AMDRDNA_HUB_MEDIA, 0U);
	if (error == 0)
		error = amdrdna_tlb_flush(device, AMDRDNA_HUB_GRAPHICS, 0U);
	if (error != 0) {
		amdrdna_mark("A2 fail: context 0 translations not dropped (%d)", error);
		memory_restore_contexts(device);
		return error;
	}

	/* Reads the main words of both hubs back. */
	error = memory_hub_check(device, AMDRDNA_HUB_GRAPHICS);
	if (error == 0)
		error = memory_hub_check(device, AMDRDNA_HUB_MEDIA);
	if (error != 0) {
		memory_restore_contexts(device);
		return error;
	}

	/* The table now translates; the later stages may map buffers through it. */
	device->memory_ready = true;

	/* Prepares the interrupt ring, leaving it off. */
	error = amdrdna_ih_prepare(device);
	if (error != 0) {
		amdrdna_mark("A2 fail: interrupt ring not prepared (%d)", error);
		memory_restore_contexts(device);
		return error;
	}

	/* Lets the display run a few frames, then reads its underflow bits again. */
	kern_usleep_range(MEMORY_SETTLE_US, MEMORY_SETTLE_US + MEMORY_SETTLE_US / 4U);
	device->underflow_after = memory_underflow(device);

	/* Marks the result. */
	amdrdna_mark("A2 ok bar0=%lluMiB pool=2 cpu=%lluMiB gpu=%lluMiB fb=0x%llx-0x%llx gart=0x%llx+%lluMiB table=+0x%llx ih=ready underflow=%u",
	    (unsigned long long)((device->window_from + device->window.size) >> 20),
	    (unsigned long long)(device->cpu_pool.free_bytes >> 20),
	    (unsigned long long)(device->gpu_pool.free_bytes >> 20),
	    (unsigned long long)device->frame_buffer_base,
	    (unsigned long long)device->frame_buffer_end,
	    (unsigned long long)device->gart_start,
	    (unsigned long long)(AMDRDNA_GART_BYTES >> 20),
	    (unsigned long long)device->gart_table,
	    (unsigned)device->underflow_after);

	/* Succeeded: the memory is shared and the interrupt ring is ready. */
	return 0;
}

/*
 * Undoes stage A2 for the shutdown of the machine: stops the interrupt
 * ring and gives both hubs' contexts back the words the firmware left.
 * The display keeps the frame-buffer aperture it reads through.
 */
void
amdrdna_a2_stop(
	struct amdrdna_device *device)
{
	/* Nothing was set up when the stage never got as far. */
	if (!device->memory_ready)
		return;

	/* Stops the interrupt ring. */
	amdrdna_ih_stop(device);

	/* Gives the contexts back their words from before the stage. */
	memory_restore_contexts(device);

	/* Marks the undo, for the log of the reboot. */
	amdrdna_mark("shutdown: A2 undone, interrupt ring off, hub contexts restored");
}

/*
 * Copies the system-memory table's entries (entries of them) out of VRAM
 * before a reset of the GPU, which takes VRAM as lost.
 *
 * Returns 0, or EBUSY before stage A2 built the table.
 */
int
amdrdna_gart_save(
	struct amdrdna_device *device,
	uint64_t *copy,
	uint64_t entries)
{
	volatile uint8_t *table;
	uint64_t entry;

	/* The table must exist. */
	if (device->gart_table == 0U)
		return EBUSY;

	/* Drops what the host path read before, then reads every entry through the window. */
	amdrdna_host_path_drop(device);
	table = memory_cpu(device, device->gart_table);
	for (entry = 0U; entry < entries; entry++)
		copy[entry] = kern_mmio_read64(table + entry * MEMORY_ENTRY_BYTES);

	/* Succeeded: the entries are copied. */
	return 0;
}

/*
 * Sets stage A2's memory up again after a reset of the GPU, keeping what
 * the stage took: the system-memory table's entries written back from
 * the copy taken before the reset (entries of them; every entry unused
 * without one), both memory hubs set up as the stage does with their
 * words recorded again, the host data path's flush rule, the default
 * page for faults, the doorbells, context 0's translations dropped, the
 * hubs' words read back, and the interrupt ring set up again, off.
 *
 * Returns 0, or the error of the step that failed.
 */
int
amdrdna_a2_restart(
	struct amdrdna_device *device,
	const uint64_t *copy,
	uint64_t entries)
{
	volatile uint8_t *table;
	uint64_t unused;
	uint64_t entry;
	uint32_t word;
	int error;

	/* Says the setup starts. */
	amdrdna_mark("A2 again after the reset, %llu table entries kept", (unsigned long long)entries);

	/* The table's entries, from the copy or unused. */
	table = memory_cpu(device, device->gart_table);
	unused = amdrdna_gart_unused_entry(device->dummy_page.dma.device_address);
	for (entry = 0U; entry < AMDRDNA_GART_BYTES / MEMORY_PAGE_BYTES; entry++) {
		/* One entry: the kept one, else unused. */
		if (copy != NULL && entry < entries)
			kern_mmio_write64(table + entry * MEMORY_ENTRY_BYTES, copy[entry]);
		else
			kern_mmio_write64(table + entry * MEMORY_ENTRY_BYTES, unused);
	}
	amdrdna_host_path_flush(device);
	amdrdna_host_path_drop(device);

	/* How the reset left the hubs; their context words are the ones to give back from now on. */
	memory_mark_hubs(device);

	/* Sets up the graphics hub, then the media hub. */
	memory_hub_setup(device, AMDRDNA_HUB_GRAPHICS);
	memory_hub_setup(device, AMDRDNA_HUB_MEDIA);

	/* The host data path's flush also drops its read cache, and is flushed once. */
	word = amdrdna_block_read(device, AMDRDNA_BLOCK_HDP, AMDRDNA_HDP_WINDOW_MAIN, AMDRDNA_HDP_REG_MISC);
	word |= AMDRDNA_HDP_MISC_FLUSH_DROPS;
	amdrdna_block_write(device, AMDRDNA_BLOCK_HDP, AMDRDNA_HDP_WINDOW_MAIN, AMDRDNA_HDP_REG_MISC, word);
	amdrdna_host_path_flush(device);

	/* A faulting access of either hub goes to the default page. */
	word = hub_read(device, AMDRDNA_HUB_GRAPHICS, AMDRDNA_HUB_FAULT_CONTROL, 0U);
	word = amdrdna_hub_fault_default_word(word);
	hub_write(device, AMDRDNA_HUB_GRAPHICS, AMDRDNA_HUB_FAULT_CONTROL, 0U, word);
	word = hub_read(device, AMDRDNA_HUB_MEDIA, AMDRDNA_HUB_FAULT_CONTROL, 0U);
	word = amdrdna_hub_fault_default_word(word);
	hub_write(device, AMDRDNA_HUB_MEDIA, AMDRDNA_HUB_FAULT_CONTROL, 0U, word);

	/* The doorbell BAR reaches the engines. */
	word = amdrdna_block_read(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_DOORBELL_APERTURE);
	word |= AMDRDNA_DOORBELL_APERTURE_ON;
	amdrdna_block_write(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_DOORBELL_APERTURE, word);

	/* Context 0's cached translations dropped, on the media hub first. */
	error = amdrdna_tlb_flush(device, AMDRDNA_HUB_MEDIA, 0U);
	if (error == 0)
		error = amdrdna_tlb_flush(device, AMDRDNA_HUB_GRAPHICS, 0U);
	if (error != 0) {
		amdrdna_mark("A2 fail: context 0 translations not dropped after the reset (%d)", error);
		return error;
	}

	/* The hubs' main words read back. */
	error = memory_hub_check(device, AMDRDNA_HUB_GRAPHICS);
	if (error == 0)
		error = memory_hub_check(device, AMDRDNA_HUB_MEDIA);
	if (error != 0)
		return error;

	/* The interrupt ring set up again, left off. */
	error = amdrdna_ih_restart(device);
	if (error != 0) {
		amdrdna_mark("A2 fail: interrupt ring not set up after the reset (%d)", error);
		return error;
	}

	/* Marks the result. */
	word = memory_underflow(device);
	amdrdna_mark("A2 ok again gart=0x%llx table=+0x%llx underflow=%u",
	    (unsigned long long)device->gart_start,
	    (unsigned long long)device->gart_table,
	    (unsigned)word);

	/* Succeeded: the memory is shared again. */
	return 0;
}

/*
 * Flushes the host data path's write cache, so the GPU, the security
 * processor and the display read what the CPU wrote to VRAM.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_hdp.c (amdgpu_hdp_generic_flush: write
 * 0, then read the VRAM size word to post the write).
 */
void
amdrdna_host_path_flush(
	struct amdrdna_device *device)
{
	/* Orders the CPU's earlier writes before the flush. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Writes the flush and posts it with a read. */
	amdrdna_block_write(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_HOST_PATH_FLUSH, 0U);
	(void)amdrdna_read(device, AMDRDNA_NBIO_REG_VRAM_SIZE_MIB);
}

/*
 * Drops the host data path's read cache, so the CPU reads what the GPU
 * wrote to VRAM.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/hdp_v5_0.c (hdp_v5_0_invalidate_hdp: write 1,
 * read back).
 */
void
amdrdna_host_path_drop(
	struct amdrdna_device *device)
{
	/* Writes the drop and reads it back. */
	amdrdna_block_write(device, AMDRDNA_BLOCK_HDP, AMDRDNA_HDP_WINDOW_MAIN, AMDRDNA_HDP_REG_READ_DROP, 1U);
	(void)amdrdna_block_read(device, AMDRDNA_BLOCK_HDP, AMDRDNA_HDP_WINDOW_MAIN, AMDRDNA_HDP_REG_READ_DROP);
}

/*
 * Drops every cached translation of one context on one hub, from the CPU,
 * through invalidation engine 17.
 *
 * The media hub's engine is taken through its lock first and given back
 * after: the hub may lose an engine's done bit across a power-gating
 * cycle otherwise.  The graphics hub's request is read back once before
 * the done bit is polled, so a done bit left from before is not taken for
 * this request's.  Returns 0, or ETIMEDOUT when the lock or the done bit
 * did not come.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c (gmc_v10_0_flush_gpu_tlb, the
 * path before the kernel queue runs).
 */
int
amdrdna_tlb_flush(
	struct amdrdna_device *device,
	enum amdrdna_hub hub,
	unsigned context)
{
	uint32_t request;
	uint32_t word;
	unsigned poll;
	bool locked;
	bool done;

	/* The request for the context. */
	request = amdrdna_invalidate_word(context);

	/* The GPU must see the table as the CPU last wrote it. */
	amdrdna_host_path_flush(device);

	/* Runs the request on the CPU's engine, one request at a time. */
	mutex_lock(&device->memory_lock);

	/* Takes the media hub's engine lock: the lock word reads 1 once it is the CPU's. */
	locked = false;
	if (hub == AMDRDNA_HUB_MEDIA) {
		for (poll = 0U; poll < MEMORY_INVALIDATE_POLLS; poll++) {
			/* Stops once the lock is taken. */
			word = hub_read(device, hub, AMDRDNA_HUB_INVALIDATE_LOCK, AMDRDNA_HUB_CPU_ENGINE);
			if ((word & AMDRDNA_HUB_LOCK_TAKEN) != 0U) {
				locked = true;
				break;
			}
		}
	}

	/* Writes the request; the graphics hub's is read back once before polling. */
	hub_write(device, hub, AMDRDNA_HUB_INVALIDATE_REQUEST, AMDRDNA_HUB_CPU_ENGINE, request);
	if (hub == AMDRDNA_HUB_GRAPHICS)
		(void)hub_read(device, hub, AMDRDNA_HUB_INVALIDATE_REQUEST, AMDRDNA_HUB_CPU_ENGINE);

	/* Waits for the context's done bit. */
	done = false;
	for (poll = 0U; poll < MEMORY_INVALIDATE_POLLS; poll++) {
		/* Stops once the engine reports the context done. */
		word = hub_read(device, hub, AMDRDNA_HUB_INVALIDATE_DONE, AMDRDNA_HUB_CPU_ENGINE);
		if ((word & (1U << context)) != 0U) {
			done = true;
			break;
		}
	}

	/* Gives the media hub's engine lock back. */
	if (locked)
		hub_write(device, hub, AMDRDNA_HUB_INVALIDATE_LOCK, AMDRDNA_HUB_CPU_ENGINE, 0U);

	mutex_unlock(&device->memory_lock);

	/* Reports a lock that never came. */
	if (hub == AMDRDNA_HUB_MEDIA && !locked) {
		amdrdna_mark("%s hub: invalidation engine lock not taken", hub_names[hub]);
		return ETIMEDOUT;
	}

	/* Reports a request the engine did not finish. */
	if (!done) {
		amdrdna_mark("%s hub: context %u invalidation not done", hub_names[hub], context);
		return ETIMEDOUT;
	}

	/* Succeeded: the context's translations are dropped. */
	return 0;
}

/*
 * Maps a buffer of system memory into the GPU's address space through the
 * system-memory table, at an address aligned to alignment, and records
 * that address in the buffer.
 *
 * Returns 0, EBUSY before stage A2 built the table, or ENOMEM when the
 * table's reach has no free run of the size.
 */
int
amdrdna_gart_map(
	struct amdrdna_device *device,
	struct amdrdna_system_buffer *buffer,
	uint64_t alignment)
{
	volatile uint8_t *table;
	uint64_t bytes;
	uint64_t offset;
	uint64_t page;
	uint64_t entry;
	int error;

	/* The table must exist. */
	if (device->gart_table == 0U)
		return EBUSY;

	/* The run the buffer needs: whole pages, aligned at least to a page. */
	bytes = ((uint64_t)buffer->dma.size + MEMORY_PAGE_BYTES - 1U) & ~((uint64_t)MEMORY_PAGE_BYTES - 1U);
	if (alignment < MEMORY_PAGE_BYTES)
		alignment = MEMORY_PAGE_BYTES;

	/* Takes the run from the table's reach. */
	mutex_lock(&device->memory_lock);

	error = amdrdna_pool_take(&device->gart_space, bytes, alignment, &offset);

	mutex_unlock(&device->memory_lock);

	/* Reports a reach without room. */
	if (error != 0)
		return error;

	/* Writes one entry per page of the buffer. */
	table = memory_cpu(device, device->gart_table);
	for (page = 0U; page < bytes / MEMORY_PAGE_BYTES; page++) {
		/* The page's bus address in a present system entry. */
		entry = amdrdna_gart_system_entry(buffer->dma.device_address + page * MEMORY_PAGE_BYTES);
		kern_mmio_write64(table + (offset / MEMORY_PAGE_BYTES + page) * MEMORY_ENTRY_BYTES, entry);
	}

	/* Records where the GPU sees the buffer. */
	buffer->gpu_address = device->gart_start + offset;

	/* Drops stale translations of context 0 on both hubs once the hubs translate. */
	if (device->memory_ready) {
		error = amdrdna_tlb_flush(device, AMDRDNA_HUB_MEDIA, 0U);
		if (error == 0)
			error = amdrdna_tlb_flush(device, AMDRDNA_HUB_GRAPHICS, 0U);
		if (error != 0)
			return error;
	}

	/* Succeeded: the GPU reaches the buffer at its recorded address. */
	return 0;
}

/*
 * Handles a page fault a memory hub reported: reads and clears the hub's
 * latched fault so the next one is recorded, and logs the first faults.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c (gmc_v10_0_process_interrupt: on
 * the graphics hub of GC 10.1 the status word is read twice, the first
 * read waiting for it to be updated).
 */
void
amdrdna_vm_fault(
	struct amdrdna_device *device,
	const struct amdrdna_interrupt *interrupt)
{
	struct amdrdna_vm_fault fault;
	enum amdrdna_hub hub;
	uint32_t latched;
	uint32_t word;

	/* The media hub reports as its own client; every other client is the graphics hub. */
	hub = AMDRDNA_HUB_GRAPHICS;
	if (interrupt->client == AMDRDNA_CLIENT_MEDIA_HUB)
		hub = AMDRDNA_HUB_MEDIA;

	/* Reads the latched fault, twice on the graphics hub. */
	if (hub == AMDRDNA_HUB_GRAPHICS)
		(void)hub_read(device, hub, AMDRDNA_HUB_FAULT_STATUS, 0U);
	latched = hub_read(device, hub, AMDRDNA_HUB_FAULT_STATUS, 0U);

	/* Clears it, so the hub records the next one. */
	word = hub_read(device, hub, AMDRDNA_HUB_FAULT_CONTROL, 0U);
	word = amdrdna_hub_fault_rearm_word(word);
	hub_write(device, hub, AMDRDNA_HUB_FAULT_CONTROL, 0U, word);

	/* Decodes it and counts it. */
	amdrdna_vm_fault_decode(interrupt, latched, &fault);
	device->interrupts.faults++;

	/*
	 * The address space the fault belongs to loses its work.  Only the
	 * video engine runs in the media hub's contexts, outside the pool of
	 * the graphics hub's: its run fails instead.
	 */
	if (hub == AMDRDNA_HUB_MEDIA) {
		amdrdna_vcn_media_fault(device);
	} else {
		amdrdna_space_fault(device, &fault, interrupt->context);
	}

	/*
	 * Logs the first faults only, so a flood does not fill the log.  The
	 * access bits are the interrupt's (retry, read, write, execute from
	 * the top); more faults behind this one show in the status word.
	 */
	if (device->interrupts.faults > 16U)
		return;
	amdrdna_mark("vm fault %s hub ctx=%u space=%u at 0x%llx access=0x%02x status=0x%08x unit=0x%x walker=%u denied=0x%x",
	    hub_names[fault.hub],
	    (unsigned)interrupt->context,
	    (unsigned)fault.space_id,
	    (unsigned long long)fault.fault_address,
	    (unsigned)fault.access_bits,
	    (unsigned)fault.latched,
	    fault.unit,
	    fault.walker_error,
	    fault.denied);
}

/*
 * Lets every context of both hubs raise an interrupt for each kind of
 * fault (on), or stops it.  Stage A3 turns them on with the ring.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c (amdgpu_gmc_set_vm_fault_masks).
 */
void
amdrdna_fault_interrupts(
	struct amdrdna_device *device,
	bool on)
{
	enum amdrdna_hub hub;
	unsigned context;
	uint32_t word;

	/* Rewrites the control word of every context of every hub. */
	for (hub = AMDRDNA_HUB_GRAPHICS; hub < AMDRDNA_HUB_COUNT; hub++) {
		for (context = 0U; context < MEMORY_CONTEXTS; context++) {
			/* Sets or clears the context's interrupt bits. */
			word = hub_read(device, hub, AMDRDNA_HUB_CONTEXT_CONTROL, context);
			word = amdrdna_hub_fault_interrupt_word(word, on);
			hub_write(device, hub, AMDRDNA_HUB_CONTEXT_CONTROL, context, word);
		}
	}
}

/*
 * Reports the CPU's view of a VRAM offset inside the mapped window.  The
 * offset must lie in the CPU's pool, which the window maps.
 */
volatile uint8_t *
amdrdna_vram_cpu(
	struct amdrdna_device *device,
	uint64_t offset)
{
	volatile uint8_t *address;

	/* The window's own translation. */
	address = memory_cpu(device, offset);

	/* Succeeded: where the CPU reads and writes the offset. */
	return address;
}

/*
 * Takes an aligned run of VRAM from the CPU's pool (cpu true) or from the
 * pool only the GPU reaches, and reports its VRAM offset.  The run is the
 * caller's for the GPU's lifetime.
 *
 * Returns 0, EBUSY before stage A2 built the pools, or ENOMEM.
 */
int
amdrdna_vram_take(
	struct amdrdna_device *device,
	bool cpu,
	uint64_t bytes,
	uint64_t alignment,
	uint64_t *offset)
{
	struct amdrdna_pool *pool;
	int error;

	/* The pools exist once the stage that builds them reported ok. */
	if (!device->memory_ready)
		return EBUSY;

	/* The pool the caller asked for. */
	pool = &device->gpu_pool;
	if (cpu)
		pool = &device->cpu_pool;

	/* Takes the run, one taker at a time. */
	mutex_lock(&device->memory_lock);

	error = amdrdna_pool_take(pool, bytes, alignment, offset);

	mutex_unlock(&device->memory_lock);

	/* Reports a pool without room. */
	if (error != 0)
		return ENOMEM;

	/* Succeeded: the run is the caller's. */
	return 0;
}

/*
 * Gives a run of VRAM taken with amdrdna_vram_take() back to its pool.
 * A run the pool cannot take back (its list of free runs is full) stays
 * taken for good, which is only a loss of room.
 */
void
amdrdna_vram_give(
	struct amdrdna_device *device,
	bool cpu,
	uint64_t offset,
	uint64_t bytes)
{
	struct amdrdna_pool *pool;
	int error;

	/* The pool the run came from. */
	pool = &device->gpu_pool;
	if (cpu)
		pool = &device->cpu_pool;

	/* Gives the run back, one caller at a time. */
	mutex_lock(&device->memory_lock);

	error = amdrdna_pool_give(pool, offset, bytes);

	mutex_unlock(&device->memory_lock);

	/* Says when the run could not be taken back. */
	if (error != 0)
		amdrdna_mark("vram: %lluKiB at 0x%llx stay taken (%d)", (unsigned long long)(bytes >> 10), (unsigned long long)offset, error);
}

/*
 * Reports the bus address the CPU reaches a VRAM offset of the CPU's pool
 * at: the window's physical address plus the offset's place in it.
 */
uint64_t
amdrdna_vram_bus(
	struct amdrdna_device *device,
	uint64_t offset)
{
	uint64_t bus;

	/* The window's mapping starts at window_from in VRAM. */
	bus = device->window.physical_address + (offset - device->window_from);

	/* Succeeded: the bus address. */
	return bus;
}

/*
 * Reports the absolute dword index of one instance of a named register of
 * a memory hub, for a ring command that writes or waits on it; 0 when the
 * block table does not list the hub.
 */
uint32_t
amdrdna_hub_register(
	struct amdrdna_device *device,
	enum amdrdna_hub hub,
	enum amdrdna_hub_register which,
	unsigned instance)
{
	const struct amdrdna_ip *block;
	uint32_t index;

	/* The hub's block and the register's index in it. */
	block = hub_block(device, hub);
	index = amdrdna_hub_index(block, hub, which, instance);

	/* Succeeded: the index, 0 for none. */
	return index;
}

/*
 * Points a context of a memory hub at an address space's root table from
 * the CPU: the word a context takes (the rings write the same into the
 * graphics hub's contexts).  The caller drops the context's translations
 * after it (amdrdna_tlb_flush()).
 */
void
amdrdna_hub_context_set(
	struct amdrdna_device *device,
	enum amdrdna_hub hub,
	unsigned context,
	uint64_t root_word)
{
	/* The root table's word, low half then high half. */
	hub_write(device, hub, AMDRDNA_HUB_TABLE_BASE_LOW, context, (uint32_t)root_word);
	hub_write(device, hub, AMDRDNA_HUB_TABLE_BASE_HIGH, context, (uint32_t)(root_word >> 32));
}

/*
 * Gives the VRAM memory training would have overwritten back to the
 * pools, once training has run or been skipped: every part of the
 * training range that no other kept range covers goes to the pool whose
 * bounds hold it.
 *
 * Returns 0, or ENOMEM when the scratch pool cannot be had.
 */
int
amdrdna_training_range_release(
	struct amdrdna_device *device)
{
	struct amdrdna_pool *scratch;
	struct amdrdna_pool *pool;
	const struct amdrdna_range *range;
	const struct amdrdna_range *training;
	uint64_t start;
	uint64_t end;
	unsigned index;
	unsigned span;
	unsigned side;
	int error;

	/* Finds the training range; a layout without one has nothing to give. */
	training = NULL;
	for (index = 0U; index < device->layout.reserved_count; index++) {
		/* Stops at the training range. */
		if (device->layout.reserved[index].reason == AMDRDNA_RANGE_TRAINING) {
			training = &device->layout.reserved[index];
			break;
		}
	}

	/* Without one, nothing was held for training. */
	if (training == NULL)
		return 0;

	/* A pool of its own to cut the other kept ranges out of the training range. */
	scratch = kern_malloc(sizeof(*scratch));
	if (scratch == NULL)
		return ENOMEM;

	/* Gives each pool the part of the training range inside its bounds and outside every other kept range. */
	error = 0;
	mutex_lock(&device->memory_lock);

	for (side = 0U; side < 2U; side++) {
		/* The CPU's pool first, then the GPU's. */
		pool = &device->cpu_pool;
		if (side == 1U)
			pool = &device->gpu_pool;

		/* The training range clipped to the pool's bounds. */
		start = training->start;
		end = training->end;
		if (start < pool->start)
			start = pool->start;
		if (end > pool->end)
			end = pool->end;
		if (start >= end)
			continue;

		/* The clipped range without the other kept ranges. */
		amdrdna_pool_init(scratch, start, end);
		(void)amdrdna_pool_give(scratch, start, end - start);
		for (index = 0U; index < device->layout.reserved_count; index++) {
			/* Holds every kept range but the training range. */
			range = &device->layout.reserved[index];
			if (range == training)
				continue;
			amdrdna_pool_hold(scratch, range->start, range->end - range->start);
		}

		/* Gives what is left to the pool. */
		for (span = 0U; span < scratch->span_count; span++) {
			/* Gives one run; a run the pool cannot take is reported. */
			error = amdrdna_pool_give(pool, scratch->spans[span].start, scratch->spans[span].end - scratch->spans[span].start);
			if (error != 0)
				break;
		}

		/* Stops at a refused run. */
		if (error != 0)
			break;
	}

	mutex_unlock(&device->memory_lock);

	/* Frees the scratch pool. */
	kern_free(scratch);

	/* Reports a run the pools did not take. */
	if (error != 0)
		return error;

	/* Succeeded: the training range is ordinary VRAM again. */
	return 0;
}

/* The block table entry of a hub: the graphics block, or the media hub's own. */
static const struct amdrdna_ip *
hub_block(
	struct amdrdna_device *device,
	enum amdrdna_hub hub)
{
	/* The media hub has a block of its own. */
	if (hub == AMDRDNA_HUB_MEDIA)
		return &device->blocks.blocks[AMDRDNA_BLOCK_MMHUB];

	/* The graphics hub lives in the graphics block. */
	return &device->blocks.blocks[AMDRDNA_BLOCK_GC];
}

/* Reads one instance of a named hub register; a register the hub lacks reads as zero. */
static uint32_t
hub_read(
	struct amdrdna_device *device,
	enum amdrdna_hub hub,
	enum amdrdna_hub_register which,
	unsigned instance)
{
	uint32_t index;
	uint32_t value;

	/* Finds the register's index. */
	index = amdrdna_hub_index(hub_block(device, hub), hub, which, instance);
	if (index == 0U)
		return 0U;

	/* Reads it. */
	value = amdrdna_read(device, index);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one instance of a named hub register; a register the hub lacks is not written. */
static void
hub_write(
	struct amdrdna_device *device,
	enum amdrdna_hub hub,
	enum amdrdna_hub_register which,
	unsigned instance,
	uint32_t value)
{
	uint32_t index;

	/* Finds the register's index. */
	index = amdrdna_hub_index(hub_block(device, hub), hub, which, instance);
	if (index == 0U)
		return;

	/* Writes it. */
	amdrdna_write(device, index, value);
}

/* Checks that the block table listed every block and window the stage writes. */
static int
memory_blocks(
	struct amdrdna_device *device)
{
	static const struct {
		enum amdrdna_block block;
		unsigned window;
	} needed[] = {
		{ AMDRDNA_BLOCK_GC, 0U },
		{ AMDRDNA_BLOCK_MMHUB, 0U },
		{ AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST },
		{ AMDRDNA_BLOCK_HDP, AMDRDNA_HDP_WINDOW_MAIN },
		{ AMDRDNA_BLOCK_IH, AMDRDNA_IH_WINDOW_MAIN }
	};
	uint32_t index;
	unsigned item;
	int error;

	/* Each block must be listed with the window. */
	for (item = 0U; item < sizeof(needed) / sizeof(needed[0]); item++) {
		/* Looks the window's first register up. */
		error = amdrdna_block_index(device, needed[item].block, needed[item].window, 0U, &index);
		if (error != 0) {
			amdrdna_mark("A2 fail: no %s window %u", amdrdna_block_name(needed[item].block), needed[item].window);
			return error;
		}
	}

	/* Succeeded: every block the stage writes is known. */
	return 0;
}

/*
 * Reads the underflow bits of the firmware's display chain: the plane's
 * three status bits, and the timing generator's input underflow as
 * MEMORY_UNDERFLOW_TIMING_BIT.  A display nothing is lit on reads 0.
 */
static uint32_t
memory_underflow(
	struct amdrdna_device *device)
{
	const struct amdrdna_ip *display;
	uint32_t index;
	uint32_t word;
	uint32_t underflow;

	/* Nothing lit, nothing to starve. */
	if (!device->gop.lit)
		return 0U;

	/* The plane's status bits. */
	display = &device->blocks.blocks[AMDRDNA_BLOCK_DCN];
	underflow = 0U;
	index = amdrdna_dcn_index(display, AMDRDNA_DCN_PLANE_CONTROL, device->gop.plane);
	if (index != 0U) {
		word = amdrdna_read(device, index);
		underflow = (word >> MEMORY_PLANE_UNDERFLOW_SHIFT) & MEMORY_PLANE_UNDERFLOW_MASK;
	}

	/* The timing generator's input underflow. */
	index = amdrdna_dcn_index(display, AMDRDNA_DCN_COMBINER_INPUT, device->gop.timing);
	if (index != 0U) {
		word = amdrdna_read(device, index);
		if ((word & MEMORY_TIMING_UNDERFLOW) != 0U)
			underflow |= MEMORY_UNDERFLOW_TIMING_BIT;
	}

	/* Succeeded: the underflow bits. */
	return underflow;
}

/* Clears the underflow bits of the firmware's display chain, leaving every other bit as it is. */
static void
memory_underflow_clear(
	struct amdrdna_device *device)
{
	const struct amdrdna_ip *display;
	uint32_t index;
	uint32_t word;

	/* Clears the plane's bits. */
	display = &device->blocks.blocks[AMDRDNA_BLOCK_DCN];
	index = amdrdna_dcn_index(display, AMDRDNA_DCN_PLANE_CONTROL, device->gop.plane);
	if (index != 0U) {
		word = amdrdna_read(device, index);
		amdrdna_write(device, index, word | MEMORY_PLANE_UNDERFLOW_CLEAR);
	}

	/* Clears the timing generator's bit. */
	index = amdrdna_dcn_index(display, AMDRDNA_DCN_COMBINER_INPUT, device->gop.timing);
	if (index != 0U) {
		word = amdrdna_read(device, index);
		amdrdna_write(device, index, word | MEMORY_TIMING_UNDERFLOW_CLEAR);
	}
}

/*
 * Grows the VRAM window (BAR 0) from what the firmware left to cover the
 * VRAM: the sizes from the VRAM's, rounded up to a power of two, down to
 * twice the window, each asked of the PCI host, which moves the doorbell
 * BAR past the window and grows the bridges' windows; the first that
 * works wins.  The layout is planned again for the larger window.  Off
 * with the boot word amdrdna.bar.grow=0; a window that does not grow stays
 * as it was.
 */
static void
memory_grow(
	struct amdrdna_device *device)
{
	struct drv_pci_bar bar;
	uint64_t size;
	uint64_t before;
	unsigned asked;
	int error;

	/* Only when asked (the default) and when the window shows less than the VRAM. */
	asked = amdrdna_parameter_number("amdrdna.bar.grow", 1U);
	if (asked == 0U)
		return;
	before = device->vram_window_bytes;
	if (before >= device->vram_bytes)
		return;

	/* The largest size first: the VRAM's, rounded up to a power of two. */
	size = before;
	while (size < device->vram_bytes)
		size <<= 1;

	/* Each size down to twice the window, until one works. */
	error = ENOSPC;
	while (size > before) {
		error = drv_pci_pcat_grow_bar(device->pci, AMDRDNA_BAR_VRAM, size);
		if (error == 0)
			break;
		size >>= 1;
	}

	/* A window no size could grow stays as it was. */
	if (error != 0) {
		amdrdna_mark("A2 window stays %lluMiB (%d)", (unsigned long long)(before >> 20), error);
		return;
	}

	/* The window's new size, where it is (the doorbells may have moved). */
	kern_memset(&bar, 0, sizeof(bar));
	error = drv_pci_device_bar(device->pci, AMDRDNA_BAR_VRAM, &bar);
	if (error == 0) {
		device->vram_bus = bar.bus_address;
		device->vram_window_bytes = bar.size;
	}

	/* The doorbells' record (their address goes in the mark). */
	error = drv_pci_device_bar(device->pci, AMDRDNA_BAR_DOORBELL, &bar);
	if (error == 0)
		device->doorbell_bytes = bar.size;

	/* The layout for the larger window. */
	error = amdrdna_plan_layout(device);
	amdrdna_mark("A2 window grown %lluMiB -> %lluMiB at 0x%llx, doorbells at 0x%llx, layout cpu=0-%lluMiB gpu=%llu-%lluMiB (%d)",
	    (unsigned long long)(before >> 20),
	    (unsigned long long)(device->vram_window_bytes >> 20),
	    (unsigned long long)device->vram_bus,
	    (unsigned long long)bar.bus_address,
	    (unsigned long long)(device->layout.cpu_pool_end >> 20),
	    (unsigned long long)(device->layout.gpu_pool_start >> 20),
	    (unsigned long long)(device->layout.gpu_pool_end >> 20),
	    error);
}

/*
 * Maps the CPU's window on the VRAM and the doorbell page.
 *
 * The window starts past the firmware's picture when the picture lies in
 * it: the console keeps writing the picture through its own mapping, and
 * a second mapping of other cache attributes must not alias it.
 */
static int
memory_windows(
	struct amdrdna_device *device)
{
	const struct amdrdna_range *range;
	uint64_t window;
	uint64_t from;
	unsigned index;
	int error;

	/* The part of the VRAM BAR the driver uses: at most 256 MiB. */
	window = device->vram_window_bytes;
	if (window > AMDRDNA_CPU_WINDOW_BYTES)
		window = AMDRDNA_CPU_WINDOW_BYTES;

	/* Starts past the picture's kept range when it lies in the window. */
	from = 0U;
	for (index = 0U; index < device->layout.reserved_count; index++) {
		/* Only the picture's range moves the start. */
		range = &device->layout.reserved[index];
		if (range->reason != AMDRDNA_RANGE_GOP)
			continue;
		if (range->start < window && range->end > from)
			from = range->end;
	}

	/* A window wholly taken by the picture leaves nothing to map. */
	if (from >= window) {
		amdrdna_mark("A2 fail: the picture fills the VRAM window");
		return ENOSPC;
	}

	/* Claims the VRAM BAR and maps the window's part past the picture, uncached. */
	error = drv_pci_device_claim_bar(device->pci, AMDRDNA_BAR_VRAM);
	if (error != 0) {
		amdrdna_mark("A2 fail: VRAM window not claimed (%d)", error);
		return error;
	}

	/* The release now gives the claim back. */
	device->window_claimed = true;

	/* Maps the window. */
	error = drv_pci_device_map_bar_region(device->pci,
	    AMDRDNA_BAR_VRAM,
	    from,
	    (size_t)(window - from),
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &device->window);
	if (error != 0) {
		amdrdna_mark("A2 fail: VRAM window not mapped (%d)", error);
		return error;
	}

	/* Records where the mapping starts in VRAM. */
	device->window_from = from;

	/* Claims the doorbell BAR and maps its first page, which holds every doorbell the driver rings. */
	error = drv_pci_device_claim_bar(device->pci, AMDRDNA_BAR_DOORBELL);
	if (error != 0) {
		amdrdna_mark("A2 fail: doorbells not claimed (%d)", error);
		return error;
	}

	/* The release now gives the claim back. */
	device->doorbells_claimed = true;

	/* Maps the doorbell page. */
	error = drv_pci_device_map_bar_region(device->pci,
	    AMDRDNA_BAR_DOORBELL,
	    0U,
	    AMDRDNA_DOORBELL_BYTES,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &device->doorbells);
	if (error != 0) {
		amdrdna_mark("A2 fail: doorbells not mapped (%d)", error);
		return error;
	}

	/* Marks the windows. */
	amdrdna_mark("A2 window +0x%llx-0x%llx at 0x%llx, doorbells %uKiB at 0x%llx",
	    (unsigned long long)from,
	    (unsigned long long)window,
	    (unsigned long long)device->window.physical_address,
	    (unsigned)(AMDRDNA_DOORBELL_BYTES >> 10),
	    (unsigned long long)device->doorbells.physical_address);

	/* Succeeded: the CPU reaches the window and the doorbells. */
	return 0;
}

/*
 * Builds the VRAM pools from the planned layout (the window's mapped part
 * for the CPU, the rest below the top range for the GPU, without the kept
 * ranges) and the address space of the system-memory table.
 */
static int
memory_pools(
	struct amdrdna_device *device)
{
	const struct amdrdna_range *range;
	uint64_t window_end;
	unsigned index;
	int error;

	/* The CPU's pool: the mapped part of the window, without the kept ranges. */
	window_end = device->window_from + device->window.size;
	amdrdna_pool_init(&device->cpu_pool, device->window_from, window_end);
	error = amdrdna_pool_give(&device->cpu_pool, device->window_from, window_end - device->window_from);
	if (error != 0)
		return error;

	/* The GPU's pool: from the window's end to the top range, without the kept ranges. */
	amdrdna_pool_init(&device->gpu_pool, device->layout.gpu_pool_start, device->layout.gpu_pool_end);
	if (device->layout.gpu_pool_end > device->layout.gpu_pool_start) {
		error = amdrdna_pool_give(&device->gpu_pool,
		    device->layout.gpu_pool_start,
		    device->layout.gpu_pool_end - device->layout.gpu_pool_start);
		if (error != 0)
			return error;
	}

	/* Keeps every reserved range out of both pools. */
	for (index = 0U; index < device->layout.reserved_count; index++) {
		/* Holds the range in whichever pool it overlaps. */
		range = &device->layout.reserved[index];
		amdrdna_pool_hold(&device->cpu_pool, range->start, range->end - range->start);
		amdrdna_pool_hold(&device->gpu_pool, range->start, range->end - range->start);
	}

	/* The table's reach, in the GPU's address space, without its guarded start. */
	amdrdna_pool_init(&device->gart_space, 0U, AMDRDNA_GART_BYTES);
	error = amdrdna_pool_give(&device->gart_space, 0U, AMDRDNA_GART_BYTES);
	if (error != 0)
		return error;
	amdrdna_pool_hold(&device->gart_space, 0U, MEMORY_GART_GUARD_BYTES);

	/* Marks the pools. */
	amdrdna_mark("A2 pools cpu=%lluMiB in %u runs, gpu=%lluMiB in %u runs",
	    (unsigned long long)(device->cpu_pool.free_bytes >> 20),
	    device->cpu_pool.span_count,
	    (unsigned long long)(device->gpu_pool.free_bytes >> 20),
	    device->gpu_pool.span_count);

	/* Succeeded: the pools hand out VRAM and table addresses. */
	return 0;
}

/*
 * Builds the system-memory table: places its reach in the GPU's address
 * space, takes the dummy page (system memory) and the scratch page and the
 * table (VRAM the CPU reaches), writes every entry as unused, flushes the
 * host data path and reads a few entries back.
 */
static int
memory_table(
	struct amdrdna_device *device)
{
	volatile uint8_t *table;
	uint64_t unused;
	uint64_t entries;
	uint64_t entry;
	uint64_t read_back;
	uint64_t checked[MEMORY_CHECKED_ENTRIES];
	unsigned item;
	int error;

	/* The frame buffer's last address in the GPU's space. */
	device->frame_buffer_end = device->frame_buffer_base + device->vram_bytes - 1U;

	/* Places the table's reach before or after the frame buffer. */
	error = amdrdna_gart_place(device->frame_buffer_base, device->frame_buffer_end, AMDRDNA_GART_BYTES, &device->gart_start);
	if (error != 0) {
		amdrdna_mark("A2 fail: no room for the system-memory table (%d)", error);
		return error;
	}

	/* The page every unused entry and every faulting access points at. */
	device->dma = drv_pci_device_dma(device->pci);
	if (device->dma == NULL) {
		amdrdna_mark("A2 fail: no DMA device");
		return ENODEV;
	}

	/* Allocates the dummy page. */
	error = drv_dma_alloc_coherent(device->dma, MEMORY_PAGE_BYTES, MEMORY_PAGE_BYTES, &device->dummy_page.dma);
	if (error != 0) {
		amdrdna_mark("A2 fail: dummy page not allocated (%d)", error);
		return error;
	}

	/* Clears it; the driver keeps it for the GPU's lifetime. */
	device->dummy_page.allocated = true;
	kern_memset(device->dummy_page.dma.address, 0, MEMORY_PAGE_BYTES);

	/* The scratch page the hubs fall back to for an access outside every aperture. */
	error = amdrdna_pool_take(&device->cpu_pool, MEMORY_PAGE_BYTES, MEMORY_PAGE_BYTES, &device->scratch_page);
	if (error != 0) {
		amdrdna_mark("A2 fail: no VRAM for the scratch page (%d)", error);
		return error;
	}

	/* The table: one entry per page of its reach, in VRAM the CPU writes. */
	entries = AMDRDNA_GART_BYTES / MEMORY_PAGE_BYTES;
	error = amdrdna_pool_take(&device->cpu_pool, entries * MEMORY_ENTRY_BYTES, MEMORY_PAGE_BYTES, &device->gart_table);
	if (error != 0) {
		amdrdna_mark("A2 fail: no VRAM for the system-memory table (%d)", error);
		device->gart_table = 0U;
		return error;
	}

	/* Writes every entry as unused, pointing at the dummy page. */
	table = memory_cpu(device, device->gart_table);
	unused = amdrdna_gart_unused_entry(device->dummy_page.dma.device_address);
	for (entry = 0U; entry < entries; entry++)
		kern_mmio_write64(table + entry * MEMORY_ENTRY_BYTES, unused);

	/* Makes the writes visible to the GPU, and drops what the host path read before. */
	amdrdna_host_path_flush(device);
	amdrdna_host_path_drop(device);

	/* Reads the first, two middle and the last entries back through the indirect VRAM window. */
	checked[0] = 0U;
	checked[1] = entries / 3U;
	checked[2] = (entries / 3U) * 2U;
	checked[3] = entries - 1U;
	for (item = 0U; item < MEMORY_CHECKED_ENTRIES; item++) {
		/* Compares one entry with the unused word. */
		amdrdna_vram_read(device, device->gart_table + checked[item] * MEMORY_ENTRY_BYTES, &read_back, sizeof(read_back));
		if (read_back != unused) {
			amdrdna_mark("A2 fail: table entry %llu reads 0x%016llx, wrote 0x%016llx",
			    (unsigned long long)checked[item],
			    (unsigned long long)read_back,
			    (unsigned long long)unused);
			return EIO;
		}
	}

	/* Marks the table. */
	amdrdna_mark("A2 table vram=+0x%llx entries=%llu gart=0x%llx-0x%llx scratch=+0x%llx dummy=0x%llx",
	    (unsigned long long)device->gart_table,
	    (unsigned long long)entries,
	    (unsigned long long)device->gart_start,
	    (unsigned long long)(device->gart_start + AMDRDNA_GART_BYTES - 1U),
	    (unsigned long long)device->scratch_page,
	    (unsigned long long)device->dummy_page.dma.device_address);

	/* Succeeded: the table maps nothing yet, and reads back. */
	return 0;
}

/* The CPU's view of a VRAM offset inside the mapped window. */
static volatile uint8_t *
memory_cpu(
	struct amdrdna_device *device,
	uint64_t offset)
{
	volatile uint8_t *base;

	/* The window's mapping starts at window_from in VRAM. */
	base = device->window.address;

	/* Succeeded: the address of the offset. */
	return base + (size_t)(offset - device->window_from);
}

/* Marks how the firmware left the main words of both hubs, and records their context words. */
static void
memory_mark_hubs(
	struct amdrdna_device *device)
{
	enum amdrdna_hub hub;
	unsigned context;

	/* One line per hub. */
	for (hub = AMDRDNA_HUB_GRAPHICS; hub < AMDRDNA_HUB_COUNT; hub++) {
		/* Records every context's word, to give it back on a failure or at shutdown. */
		for (context = 0U; context < MEMORY_CONTEXTS; context++)
			device->context_before[hub][context] = hub_read(device, hub, AMDRDNA_HUB_CONTEXT_CONTROL, context);

		/* Marks the hub's words. */
		amdrdna_mark("A2 %s hub before: l1=0x%08x l2=0x%08x ctx0=0x%08x ctx1=0x%08x aperture=0x%x-0x%x bypass=0x%x/0x%x fault=0x%08x",
		    hub_names[hub],
		    (unsigned)hub_read(device, hub, AMDRDNA_HUB_TRANSLATION_CONTROL, 0U),
		    (unsigned)hub_read(device, hub, AMDRDNA_HUB_CACHE_CONTROL, 0U),
		    (unsigned)device->context_before[hub][0],
		    (unsigned)device->context_before[hub][1],
		    (unsigned)hub_read(device, hub, AMDRDNA_HUB_APERTURE_LOW, 0U),
		    (unsigned)hub_read(device, hub, AMDRDNA_HUB_APERTURE_HIGH, 0U),
		    (unsigned)hub_read(device, hub, AMDRDNA_HUB_BYPASS_BOTTOM, 0U),
		    (unsigned)hub_read(device, hub, AMDRDNA_HUB_BYPASS_TOP, 0U),
		    (unsigned)hub_read(device, hub, AMDRDNA_HUB_FAULT_CONTROL, 0U));
	}

	/* The words are recorded; a failure from here on gives them back. */
	device->contexts_recorded = true;
}

/*
 * Sets up one memory hub: context 0's table and its range, the frame
 * buffer's aperture with the bypass aperture shut, the default and fault
 * pages, the translation and cache controls, context 0 on, the identity
 * aperture shut, contexts 1 to 15, and the invalidation engines' ranges.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c and mmhub_v2_0.c
 * (gfxhub_v2_0_gart_enable and the functions it calls, in that order).
 */
static void
memory_hub_setup(
	struct amdrdna_device *device,
	enum amdrdna_hub hub)
{
	uint64_t base;
	uint64_t last;
	uint64_t scratch;
	uint64_t dummy;
	unsigned engine;
	uint32_t word;

	/* Context 0's table, and the first and last page of its reach. */
	base = memory_table_base(device);
	last = device->gart_start + AMDRDNA_GART_BYTES - 1U;
	hub_write(device, hub, AMDRDNA_HUB_TABLE_BASE_LOW, 0U, (uint32_t)base);
	hub_write(device, hub, AMDRDNA_HUB_TABLE_BASE_HIGH, 0U, (uint32_t)(base >> 32));
	hub_write(device, hub, AMDRDNA_HUB_TABLE_FIRST_LOW, 0U, (uint32_t)(device->gart_start >> AMDRDNA_HUB_PAGE_SHIFT));
	hub_write(device, hub, AMDRDNA_HUB_TABLE_FIRST_HIGH, 0U, (uint32_t)(device->gart_start >> AMDRDNA_HUB_PAGE_HIGH_SHIFT));
	hub_write(device, hub, AMDRDNA_HUB_TABLE_LAST_LOW, 0U, (uint32_t)(last >> AMDRDNA_HUB_PAGE_SHIFT));
	hub_write(device, hub, AMDRDNA_HUB_TABLE_LAST_HIGH, 0U, (uint32_t)(last >> AMDRDNA_HUB_PAGE_HIGH_SHIFT));

	/* The bypass aperture shut, and the frame buffer's aperture. */
	hub_write(device, hub, AMDRDNA_HUB_BYPASS_BASE, 0U, 0U);
	hub_write(device, hub, AMDRDNA_HUB_BYPASS_BOTTOM, 0U, AMDRDNA_HUB_BYPASS_SHUT_BOTTOM);
	hub_write(device, hub, AMDRDNA_HUB_BYPASS_TOP, 0U, 0U);
	hub_write(device, hub, AMDRDNA_HUB_APERTURE_LOW, 0U, (uint32_t)(device->frame_buffer_base >> AMDRDNA_HUB_APERTURE_SHIFT));
	hub_write(device, hub, AMDRDNA_HUB_APERTURE_HIGH, 0U, (uint32_t)(device->frame_buffer_end >> AMDRDNA_HUB_APERTURE_SHIFT));

	/* The scratch page for an access outside every aperture (a VRAM address), and the fault page. */
	scratch = device->scratch_page + device->frame_buffer_physical;
	dummy = device->dummy_page.dma.device_address;
	hub_write(device, hub, AMDRDNA_HUB_DEFAULT_PAGE_LOW, 0U, (uint32_t)(scratch >> AMDRDNA_HUB_PAGE_SHIFT));
	hub_write(device, hub, AMDRDNA_HUB_DEFAULT_PAGE_HIGH, 0U, (uint32_t)(scratch >> AMDRDNA_HUB_PAGE_HIGH_SHIFT));
	hub_write(device, hub, AMDRDNA_HUB_FAULT_PAGE_LOW, 0U, (uint32_t)(dummy >> AMDRDNA_HUB_PAGE_SHIFT));
	hub_write(device, hub, AMDRDNA_HUB_FAULT_PAGE_HIGH, 0U, (uint32_t)(dummy >> AMDRDNA_HUB_PAGE_HIGH_SHIFT));

	/* Retries the read of a page-migration entry. */
	word = hub_read(device, hub, AMDRDNA_HUB_FAULT_CONTROL_2, 0U);
	word = amdrdna_hub_fault_retry_word(word);
	hub_write(device, hub, AMDRDNA_HUB_FAULT_CONTROL_2, 0U, word);

	/* The first-level translation control. */
	word = hub_read(device, hub, AMDRDNA_HUB_TRANSLATION_CONTROL, 0U);
	word = amdrdna_hub_translation_word(word);
	hub_write(device, hub, AMDRDNA_HUB_TRANSLATION_CONTROL, 0U, word);

	/* The second-level cache on. */
	word = hub_read(device, hub, AMDRDNA_HUB_CACHE_CONTROL, 0U);
	word = amdrdna_hub_cache_word(word);
	hub_write(device, hub, AMDRDNA_HUB_CACHE_CONTROL, 0U, word);

	/* Drops whatever the caches hold, once. */
	word = hub_read(device, hub, AMDRDNA_HUB_CACHE_CONTROL_2, 0U);
	word = amdrdna_hub_cache_flush_word(word);
	hub_write(device, hub, AMDRDNA_HUB_CACHE_CONTROL_2, 0U, word);

	/* The cache's bank and fragment sizes and its request mode. */
	word = amdrdna_hub_cache_word_3();
	hub_write(device, hub, AMDRDNA_HUB_CACHE_CONTROL_3, 0U, word);
	word = amdrdna_hub_cache_word_4();
	hub_write(device, hub, AMDRDNA_HUB_CACHE_CONTROL_4, 0U, word);
	word = amdrdna_hub_cache_word_5();
	hub_write(device, hub, AMDRDNA_HUB_CACHE_CONTROL_5, 0U, word);

	/* Context 0 on, with its flat table. */
	word = hub_read(device, hub, AMDRDNA_HUB_CONTEXT_CONTROL, 0U);
	word = amdrdna_hub_system_context_word(word);
	hub_write(device, hub, AMDRDNA_HUB_CONTEXT_CONTROL, 0U, word);

	/* The identity aperture shut: its first page above its last. */
	hub_write(device, hub, AMDRDNA_HUB_IDENTITY_FIRST_LOW, 0U, AMDRDNA_HUB_IDENTITY_SHUT_LOW);
	hub_write(device, hub, AMDRDNA_HUB_IDENTITY_FIRST_HIGH, 0U, AMDRDNA_HUB_IDENTITY_SHUT_HIGH);
	hub_write(device, hub, AMDRDNA_HUB_IDENTITY_LAST_LOW, 0U, 0U);
	hub_write(device, hub, AMDRDNA_HUB_IDENTITY_LAST_HIGH, 0U, 0U);
	hub_write(device, hub, AMDRDNA_HUB_IDENTITY_SHIFT_LOW, 0U, 0U);
	hub_write(device, hub, AMDRDNA_HUB_IDENTITY_SHIFT_HIGH, 0U, 0U);

	/* Contexts 1 to 15. */
	memory_hub_contexts(device, hub);

	/* Every invalidation engine covers the whole space. */
	for (engine = 0U; engine < MEMORY_ENGINES; engine++) {
		/* The engine's range: all of it. */
		hub_write(device, hub, AMDRDNA_HUB_INVALIDATE_RANGE_LOW, engine, AMDRDNA_HUB_RANGE_ALL_LOW);
		hub_write(device, hub, AMDRDNA_HUB_INVALIDATE_RANGE_HIGH, engine, AMDRDNA_HUB_RANGE_ALL_HIGH);
	}
}

/*
 * Sets up contexts 1 to 15 of a hub for the four-level tables of later
 * stages: each one on, its reach the whole 48-bit space, with no table
 * yet (a context is given its root when a session first runs).
 */
static void
memory_hub_contexts(
	struct amdrdna_device *device,
	enum amdrdna_hub hub)
{
	unsigned context;
	uint32_t word;

	/* One context at a time. */
	for (context = 1U; context < MEMORY_CONTEXTS; context++) {
		/* The control word. */
		word = hub_read(device, hub, AMDRDNA_HUB_CONTEXT_CONTROL, context);
		word = amdrdna_hub_user_context_word(word);
		hub_write(device, hub, AMDRDNA_HUB_CONTEXT_CONTROL, context, word);

		/* The reach: from page 0 to the last page of the space. */
		hub_write(device, hub, AMDRDNA_HUB_TABLE_FIRST_LOW, context, 0U);
		hub_write(device, hub, AMDRDNA_HUB_TABLE_FIRST_HIGH, context, 0U);
		hub_write(device, hub, AMDRDNA_HUB_TABLE_LAST_LOW, context, AMDRDNA_HUB_LAST_PAGE_LOW);
		hub_write(device, hub, AMDRDNA_HUB_TABLE_LAST_HIGH, context, AMDRDNA_HUB_LAST_PAGE_HIGH);
	}
}

/*
 * Reads the main words of a hub back and compares them with what the
 * setup wrote.  Returns 0, or EIO for the first word that differs.
 */
static int
memory_hub_check(
	struct amdrdna_device *device,
	enum amdrdna_hub hub)
{
	struct memory_check checks[8];
	uint64_t base;
	uint64_t last;
	uint32_t word;
	unsigned item;

	/* The words to compare and the values the setup wrote. */
	base = memory_table_base(device);
	last = device->gart_start + AMDRDNA_GART_BYTES - 1U;
	checks[0].which = AMDRDNA_HUB_TABLE_BASE_LOW;
	checks[0].instance = 0U;
	checks[0].wrote = (uint32_t)base;
	checks[1].which = AMDRDNA_HUB_TABLE_BASE_HIGH;
	checks[1].instance = 0U;
	checks[1].wrote = (uint32_t)(base >> 32);
	checks[2].which = AMDRDNA_HUB_TABLE_FIRST_LOW;
	checks[2].instance = 0U;
	checks[2].wrote = (uint32_t)(device->gart_start >> AMDRDNA_HUB_PAGE_SHIFT);
	checks[3].which = AMDRDNA_HUB_TABLE_LAST_LOW;
	checks[3].instance = 0U;
	checks[3].wrote = (uint32_t)(last >> AMDRDNA_HUB_PAGE_SHIFT);
	checks[4].which = AMDRDNA_HUB_APERTURE_LOW;
	checks[4].instance = 0U;
	checks[4].wrote = (uint32_t)(device->frame_buffer_base >> AMDRDNA_HUB_APERTURE_SHIFT);
	checks[5].which = AMDRDNA_HUB_APERTURE_HIGH;
	checks[5].instance = 0U;
	checks[5].wrote = (uint32_t)(device->frame_buffer_end >> AMDRDNA_HUB_APERTURE_SHIFT);
	checks[6].which = AMDRDNA_HUB_CONTEXT_CONTROL;
	checks[6].instance = 0U;
	checks[6].wrote = amdrdna_hub_system_context_word(device->context_before[hub][0]);
	checks[7].which = AMDRDNA_HUB_CONTEXT_CONTROL;
	checks[7].instance = 1U;
	checks[7].wrote = amdrdna_hub_user_context_word(device->context_before[hub][1]);

	/* Compares each word. */
	for (item = 0U; item < sizeof(checks) / sizeof(checks[0]); item++) {
		/* Stops at the first word that differs. */
		word = hub_read(device, hub, checks[item].which, checks[item].instance);
		if (word != checks[item].wrote) {
			amdrdna_mark("A2 fail: %s hub word %u.%u reads 0x%08x, wrote 0x%08x",
			    hub_names[hub],
			    (unsigned)checks[item].which,
			    checks[item].instance,
			    (unsigned)word,
			    (unsigned)checks[item].wrote);
			return EIO;
		}
	}

	/* Marks the hub's new main words. */
	amdrdna_mark("A2 %s hub: l1=0x%08x l2=0x%08x ctx0=0x%08x ctx1=0x%08x base=0x%08x%08x",
	    hub_names[hub],
	    (unsigned)hub_read(device, hub, AMDRDNA_HUB_TRANSLATION_CONTROL, 0U),
	    (unsigned)hub_read(device, hub, AMDRDNA_HUB_CACHE_CONTROL, 0U),
	    (unsigned)hub_read(device, hub, AMDRDNA_HUB_CONTEXT_CONTROL, 0U),
	    (unsigned)hub_read(device, hub, AMDRDNA_HUB_CONTEXT_CONTROL, 1U),
	    (unsigned)hub_read(device, hub, AMDRDNA_HUB_TABLE_BASE_HIGH, 0U),
	    (unsigned)hub_read(device, hub, AMDRDNA_HUB_TABLE_BASE_LOW, 0U));

	/* Succeeded: the words read back as written. */
	return 0;
}

/* Gives every context of both hubs the control word the firmware left. */
static void
memory_restore_contexts(
	struct amdrdna_device *device)
{
	enum amdrdna_hub hub;
	unsigned context;

	/* Nothing to give back before the words were recorded. */
	if (!device->contexts_recorded)
		return;

	/* Writes each recorded word back. */
	for (hub = AMDRDNA_HUB_GRAPHICS; hub < AMDRDNA_HUB_COUNT; hub++) {
		for (context = 0U; context < MEMORY_CONTEXTS; context++)
			hub_write(device, hub, AMDRDNA_HUB_CONTEXT_CONTROL, context, device->context_before[hub][context]);
	}
}

/*
 * The word that points a hub at context 0's table: the table's physical
 * VRAM address, present.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c (amdgpu_gmc_pd_addr,
 * amdgpu_gmc_vram_mc2pa: a VRAM address is its offset plus the frame
 * buffer's physical offset).
 */
static uint64_t
memory_table_base(
	struct amdrdna_device *device)
{
	uint64_t base;

	/* The table's physical address with the present bit. */
	base = device->gart_table + device->frame_buffer_physical;
	base |= AMDRDNA_PAGE_PRESENT;

	/* Succeeded: the base word. */
	return base;
}
