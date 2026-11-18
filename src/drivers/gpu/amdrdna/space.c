/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU's address spaces: the page tables of each session, the VRAM its
 * work uses, and the runs of that work on the rings.
 *
 * Every session (and every check of the driver's own) has an address
 * space of four levels of page tables in VRAM the CPU writes through its
 * window, and an identifier the interrupt controller attaches to the page
 * faults of the space's work.  Buffers of VRAM (backings) are mapped into
 * a space at addresses from 4 GiB on.  A run hands the space one of the
 * graphics hub's contexts 1 to 15 for as long as it keeps it
 * (context-pool.c), drops the context's cached translations on the ring
 * when needed, has the ring run the space's indirect buffer in that
 * context and waits for the fence after it.  A page fault of a space's
 * work, or a run that does not end in time, loses the space: it runs
 * nothing more, and its session is told.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/* A page, a table's entries and the bytes of one entry. */
#define SPACE_PAGE_BYTES		4096U
#define SPACE_TABLE_ENTRIES		512U
#define SPACE_ENTRY_BYTES		8U

/* The level of the leaf tables (the root is level 0). */
#define SPACE_LEAF_LEVEL		(AMDRDNA_VM_LEVELS - 1U)

/*
 * The addresses a space hands out: from 4 GiB (nothing is mapped below,
 * so a small bad address faults) to 64 TiB, below the hole that starts at
 * 2^47; and the boundary of every mapping and backing (64 KiB, the hubs'
 * larger page).
 */
#define SPACE_FIRST_ADDRESS		(4ULL << 30)
#define SPACE_LAST_ADDRESS		(64ULL << 40)
#define SPACE_MAPPING_ALIGN		(64ULL << 10)

/*
 * The largest run one page entry's fragment field names (2^9 pages, 2
 * MiB), and the boundary a backing's VRAM and addresses are put on: the
 * largest power of two not above its size, from 64 KiB to that run, so
 * the fragments of a large backing reach the hubs' large translations.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c
 * (gmc_v10_0_sw_init: amdgpu_vm_adjust_size with the default fragment
 * size 9) and amdgpu_vm.c (amdgpu_vm_adjust_size).
 */
#define SPACE_FRAGMENT_MOST		9U
#define SPACE_LARGE_ALIGN		(2ULL << 20)

/*
 * The invalidation engine each command ring drops a context's
 * translations on: the graphics ring engine 0, the compute queues of the
 * first compute engine's pipes 0 to 3 engines 4 to 7 (the scheduler's
 * queue has 1, the first copy engine 8).  Engines 2 and 3 are the
 * firmware's and 17 the CPU's.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c
 * (amdgpu_gmc_allocate_vm_inv_eng: the engines handed to the rings in
 * order from the mask 0x1fff3).
 */
#define SPACE_ENGINE_GRAPHICS		0U
#define SPACE_ENGINE_COMPUTE_FIRST	4U

/*
 * What a wave scan covers: two shader engines of two arrays each, the
 * instances (workgroup processors and their SIMDs) 0 .. 31 of an array, 20 wave
 * slots each; and the most waves marked.
 */
#define SPACE_SHADER_ENGINES		2U
#define SPACE_SHADER_ARRAYS		2U
#define SPACE_WAVE_INSTANCES		32U
#define SPACE_WAVE_SLOTS		20U
#define SPACE_WAVES_MARKED		24U

/* The most words a run puts on its ring: a sync, a drop, the indirect buffer and a timed run's two timestamps. */
#define SPACE_RUN_COMMANDS		64U

/* How long the work of a run may take before the space is lost and its queue reset (the waits of another space's runs take twice it). */
#define SPACE_RUN_MS			2000U

/* How many runs of system memory say where they are. */
#define SPACE_SYSTEM_MARKS		8U

/* How often a run waits for a context before giving up. */
#define SPACE_GRANT_TRIES		32U

/*
 * How many runs of system memory have said where they are; only the first
 * SPACE_SYSTEM_MARKS do.  Counted without a lock: a count off by one only
 * changes how many lines are shown.
 */
static unsigned space_system_marks;

static struct amdrdna_ring *space_ring(struct amdrdna_device *device, unsigned tracked);
static void space_passed(struct amdrdna_device *device, uint32_t *passed);
static unsigned space_commands(struct amdrdna_space *space, struct amdrdna_ring *ring, unsigned tracked, const struct amdrdna_context_grant *grant, uint64_t address, uint32_t words, uint32_t *commands);
static int space_table_create(struct amdrdna_device *device, unsigned level, struct amdrdna_table **result);
static void space_table_free(struct amdrdna_device *device, struct amdrdna_table *table, unsigned level);
static void space_table_write(struct amdrdna_device *device, struct amdrdna_table *table, unsigned index, uint64_t entry);
static int space_leaf(struct amdrdna_space *space, uint64_t address, struct amdrdna_table **leaf);
static void space_clear(struct amdrdna_space *space, uint64_t address, uint64_t bytes);
static void space_changed(struct amdrdna_space *space);
static void space_lose(struct amdrdna_space *space, int error);
static uint32_t space_wave_read(struct amdrdna_device *device, uint32_t wave, uint32_t index);
static void space_hang(struct amdrdna_space *space, const struct amdrdna_space_ticket *ticket);
static bool space_dropped_locked(struct amdrdna_device *device, const struct amdrdna_space_ticket *ticket);
static void space_reset(struct amdrdna_device *device, unsigned tracked, unsigned context);
static uint64_t space_table_word(struct amdrdna_device *device, uint64_t vram);
static uint64_t space_fragment(uint64_t start, uint64_t bytes, uint64_t address, uint64_t physical);
static uint64_t space_align(uint64_t bytes);
static bool space_quiet_locked(struct amdrdna_space *space);
static uint32_t space_hub_read(struct amdrdna_device *device, enum amdrdna_hub_register which, unsigned instance);

/*
 * Prepares the address spaces' lock, the context pool and the
 * identifiers at the attach, before any interrupt can name a space.
 */
void
amdrdna_spaces_init(
	struct amdrdna_device *device)
{
	struct amdrdna_spaces *spaces;

	/* The lock, the pool and the identifiers. */
	spaces = &device->spaces;
	spin_init(&spaces->lock, LOCK_RANK_DEVICE, "amdrdna spaces");
	(void)mutex_init(&spaces->submit_lock, LOCK_RANK_DEVICE, "amdrdna submit");
	amdrdna_context_pool_init(&spaces->pool);
	amdrdna_space_ids_init(&spaces->ids);

	/* No space, nothing running yet. */
	spaces->list = NULL;
	spaces->count = 0U;
	spaces->held = 0U;
	spaces->ready = false;
	spaces->faulted = false;
	spaces->runs = 0U;
	spaces->lost = 0U;

	/* No ring has a run or a reset yet. */
	kern_memset(spaces->last_space, 0, sizeof(spaces->last_space));
	kern_memset(spaces->last_sequence, 0, sizeof(spaces->last_sequence));
	kern_memset(spaces->resets_of, 0, sizeof(spaces->resets_of));
	kern_memset(spaces->dropped_after, 0, sizeof(spaces->dropped_after));
	kern_memset(spaces->dropped_last, 0, sizeof(spaces->dropped_last));
}

/* Lets the address spaces run work once the rings run (stage A5s). */
void
amdrdna_spaces_start(
	struct amdrdna_device *device)
{
	unsigned long irq;

	/* ready lets runs be submitted from now on. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	device->spaces.ready = true;

	spin_unlock_irqrestore(&device->spaces.lock, irq);
}

/*
 * Stops every new run after a fault of the device or before its stop:
 * a run asked for from now on is refused with EIO.
 */
void
amdrdna_spaces_fault(
	struct amdrdna_device *device)
{
	unsigned long irq;

	/* faulted refuses every later run. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	device->spaces.faulted = true;

	spin_unlock_irqrestore(&device->spaces.lock, irq);
}

/*
 * Lets the address spaces run work again after a reset of the GPU, which
 * took the hubs' contexts and VRAM: with no space left, every context is
 * free again and runs are taken.  A space still there holds tables the
 * reset lost, so the spaces stay refused then.
 *
 * Returns 0, or EBUSY when a space is still there.
 */
int
amdrdna_spaces_restart(
	struct amdrdna_device *device)
{
	struct amdrdna_spaces *spaces;
	unsigned long irq;
	unsigned count;

	/* The contexts free again and the runs taken, only without a space. */
	spaces = &device->spaces;
	irq = spin_lock_irqsave(&spaces->lock);

	count = spaces->count;
	if (count == 0U) {
		amdrdna_context_pool_init(&spaces->pool);
		spaces->faulted = false;
		spaces->ready = true;
	}

	spin_unlock_irqrestore(&spaces->lock, irq);

	/* Reports spaces the reset left without their tables. */
	if (count != 0U) {
		amdrdna_mark("reset: %u address spaces remain; they run nothing more", count);
		return EBUSY;
	}

	/* Succeeded: runs are taken again. */
	return 0;
}

/*
 * Creates an address space for a session (session names it for the
 * report of a loss; NULL for a check of the driver's own): an identifier,
 * an empty root table and the free addresses.
 *
 * Returns 0, ENOSPC when no identifier is free or the VRAM has no page for
 * the root table, or ENOMEM.
 */
int
amdrdna_space_create(
	struct amdrdna_device *device,
	void *session,
	struct amdrdna_space **result)
{
	struct amdrdna_space *space;
	unsigned long irq;
	uint32_t id;
	int error;

	/* The record. */
	*result = NULL;
	space = kern_calloc(1U, sizeof(*space));
	if (space == NULL)
		return ENOMEM;

	/* The free addresses. */
	space->addresses = kern_malloc(sizeof(*space->addresses));
	if (space->addresses == NULL) {
		kern_free(space);
		return ENOMEM;
	}

	/* Every address from 4 GiB to 64 TiB is free. */
	amdrdna_pool_init(space->addresses, SPACE_FIRST_ADDRESS, SPACE_LAST_ADDRESS);
	(void)amdrdna_pool_give(space->addresses, SPACE_FIRST_ADDRESS, SPACE_LAST_ADDRESS - SPACE_FIRST_ADDRESS);

	/* The root table, empty. */
	error = space_table_create(device, 0U, &space->root);
	if (error != 0) {
		kern_free(space->addresses);
		kern_free(space);
		return error;
	}

	/* Describes the space: no context yet, its tables at their first generation. */
	(void)mutex_init(&space->lock, LOCK_RANK_DEVICE, "amdrdna space");
	space->device = device;
	space->root_word = space_table_word(device, space->root->vram);
	space->generation = 1U;
	space->context = 0U;
	space->session = session;

	/* Takes an identifier and lists the space, so its page faults find it. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	error = amdrdna_space_id_take(&device->spaces.ids, &id);
	if (error == 0) {
		space->id = id;
		space->next = device->spaces.list;
		device->spaces.list = space;
		device->spaces.count++;
	}

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* No identifier is free. */
	if (error != 0) {
		space_table_free(device, space->root, 0U);
		kern_free(space->addresses);
		kern_free(space);
		return error;
	}

	/* Succeeded: the space is the caller's. */
	*result = space;
	return 0;
}

/*
 * Destroys an address space once its session is done with it: its
 * context goes back to the pool, its identifier is free again, and its
 * tables and record are freed.
 *
 * A space whose context may still run work (it was lost and its work
 * never ended) keeps its context out of the pool and its tables in VRAM
 * for good, since the GPU may still read them; true is then returned and
 * the record stays.
 */
bool
amdrdna_space_destroy(
	struct amdrdna_space *space)
{
	struct amdrdna_device *device;
	struct amdrdna_space **link;
	unsigned long irq;
	bool quiet;

	/* Takes the space off the list and gives its context back, or holds it. */
	device = space->device;
	irq = spin_lock_irqsave(&device->spaces.lock);

	for (link = &device->spaces.list; *link != NULL; link = &(*link)->next) {
		if (*link == space) {
			*link = space->next;
			device->spaces.count--;
			break;
		}
	}

	/* A space whose work ended gives its context and identifier back; another holds its context for good. */
	quiet = space_quiet_locked(space);
	if (quiet) {
		amdrdna_context_release(&device->spaces.pool, space->context, space->id);
		amdrdna_space_id_give(&device->spaces.ids, space->id);
	} else {
		amdrdna_context_hold(&device->spaces.pool, space->context);
		device->spaces.held++;
	}

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* A space that may still run keeps everything the GPU may read. */
	if (!quiet) {
		amdrdna_mark("space %u kept: its work in context %u did not end", (unsigned)space->id, space->context);
		return true;
	}

	/* The tables, the free addresses and the record. */
	space_table_free(device, space->root, 0U);
	kern_free(space->addresses);
	kern_free(space);

	/* Succeeded: the space is gone. */
	return false;
}

/*
 * Maps a backing into an address space at a free address on a 64 KiB
 * boundary, readable, writable and executable, and reports the address.
 *
 * Returns 0, ENOSPC when the space has no free addresses of the size or
 * the VRAM had no page for a table, or ENOMEM when a table's record could
 * not be had (nothing stays mapped).
 */
int
amdrdna_space_map(
	struct amdrdna_space *space,
	const struct amdrdna_backing *backing,
	uint64_t *address)
{
	struct amdrdna_device *device;
	struct amdrdna_table *leaf;
	uint64_t access;
	uint64_t system_access;
	uint64_t start;
	uint64_t page;
	uint64_t physical;
	uint64_t fragment;
	uint64_t align;
	uint64_t entry;
	unsigned index[AMDRDNA_VM_LEVELS];
	int error;

	/* The bits of every page: present, readable, writable, executable, non-coherent VRAM. */
	device = space->device;
	access = AMDRDNA_PAGE_PRESENT | AMDRDNA_PAGE_READ | AMDRDNA_PAGE_WRITE | AMDRDNA_PAGE_EXECUTE;
	access |= AMDRDNA_PAGE_TYPE_NONCOHERENT << AMDRDNA_PAGE_TYPE_SHIFT;

	/*
	 * A page of system memory: present, readable, writable, executable, in
	 * system memory, snooped (coherent with the CPU's caches) and not kept
	 * in the GPU's caches, as the system-memory table maps its pages.
	 */
	system_access = AMDRDNA_PAGE_PRESENT | AMDRDNA_PAGE_READ | AMDRDNA_PAGE_WRITE | AMDRDNA_PAGE_EXECUTE;
	system_access |= AMDRDNA_PAGE_SYSTEM | AMDRDNA_PAGE_COHERENT;
	system_access |= AMDRDNA_PAGE_TYPE_UNCACHED << AMDRDNA_PAGE_TYPE_SHIFT;

	/* Takes the addresses on the backing's boundary and writes one entry per page, under the space's lock. */
	align = space_align(backing->bytes);
	mutex_lock(&space->lock);

	error = amdrdna_pool_take(space->addresses, backing->bytes, align, &start);
	if (error != 0) {
		mutex_unlock(&space->lock);
		return ENOSPC;
	}

	/* One entry per page of the backing. */
	for (page = 0U; page < backing->bytes; page += SPACE_PAGE_BYTES) {
		/* The leaf table of the page, made when missing. */
		error = space_leaf(space, start + page, &leaf);
		if (error != 0)
			break;

		/*
		 * The page's entry: its system page's bus address and the system
		 * bits, or its VRAM's physical address, the bits and the run of
		 * contiguous VRAM it lies in.
		 */
		amdrdna_vm_split(start + page, index);
		if (backing->system) {
			entry = amdrdna_vm_leaf_entry(backing->ram.paddr + page, system_access);
		} else {
			physical = backing->vram + page + device->frame_buffer_physical;
			fragment = space_fragment(start, backing->bytes, start + page, physical);
			entry = amdrdna_vm_leaf_entry(physical, access | fragment);
		}
		space_table_write(device, leaf, index[SPACE_LEAF_LEVEL], entry);
		leaf->count++;
	}

	/* A table that could not be had undoes the pages mapped so far. */
	if (error != 0) {
		space_clear(space, start, page);
		(void)amdrdna_pool_give(space->addresses, start, backing->bytes);
		mutex_unlock(&space->lock);
		return error;
	}

	mutex_unlock(&space->lock);

	/* The GPU reads the tables as written; the next run drops the context's translations. */
	amdrdna_host_path_flush(device);
	space_changed(space);

	/* Succeeded: the backing is at the address. */
	*address = start;
	return 0;
}

/*
 * Unmaps a run of addresses of an address space and frees them.  The
 * caller has made sure no work of the space reads them any more; the
 * next run drops the context's stale translations.
 */
void
amdrdna_space_unmap(
	struct amdrdna_space *space,
	uint64_t address,
	uint64_t bytes)
{
	uint64_t whole;
	int error;

	/* The run in whole 64 KiB steps, as it was mapped. */
	whole = (bytes + SPACE_MAPPING_ALIGN - 1U) & ~(SPACE_MAPPING_ALIGN - 1U);

	/* Clears the entries and frees the addresses, under the space's lock. */
	mutex_lock(&space->lock);

	space_clear(space, address, whole);
	error = amdrdna_pool_give(space->addresses, address, whole);

	mutex_unlock(&space->lock);

	/* Addresses the list cannot take back stay unused for good. */
	if (error != 0)
		amdrdna_mark("space %u: addresses at 0x%llx stay unused (%d)", (unsigned)space->id, (unsigned long long)address, error);

	/* The GPU reads the cleared tables; the next run drops the stale translations. */
	amdrdna_host_path_flush(space->device);
	space_changed(space);
}

/*
 * Runs words dwords of commands at address of an address space on a
 * tracked ring (AMDRDNA_TRACKED_*) and waits up to milliseconds for them
 * to end: amdrdna_space_submit() and amdrdna_space_wait().
 *
 * Returns 0, or the error of either.
 */
int
amdrdna_space_run(
	struct amdrdna_space *space,
	unsigned tracked,
	uint64_t address,
	uint32_t words,
	unsigned milliseconds)
{
	struct amdrdna_space_ticket ticket;
	int error;

	/* The run on its ring. */
	error = amdrdna_space_submit(space, tracked, address, words, &ticket);
	if (error != 0)
		return error;

	/* Its end. */
	error = amdrdna_space_wait(space, &ticket, milliseconds);
	if (error != 0)
		return error;

	/* Succeeded: the run ended. */
	return 0;
}

/*
 * Puts words dwords of commands at address of an address space on a
 * tracked ring (AMDRDNA_TRACKED_*) without waiting for them; ticket names
 * the run for amdrdna_space_wait() and amdrdna_space_passed().
 *
 * The power side hears that the GPU works, and the CPU's writes to VRAM
 * are made visible.  Work another space put on the ring and the GPU has
 * not finished is waited for first, at most twice the run time
 * SPACE_RUN_MS: a queue reset after a run that does not end drops what is
 * queued behind it, so one session's hang never takes another's work with
 * it.  The space gets a hub context; when the context's translations are
 * stale they are dropped on the ring before the commands (the context
 * pointed at the space's root table, the drop waited for, the interrupt
 * controller told which space the context's faults belong to).  The
 * commands run as an indirect buffer in the context, and the ring writes
 * its fence after them.
 *
 * Returns 0, EINVAL for a bad buffer, ENOTSUP for the copy engine, ENODEV
 * for a ring that does not run, EIO for a space that is lost or a device
 * that failed, EBUSY when the ring is full, ETIMEDOUT when no context
 * became free.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c (amdgpu_vm_flush: the pipeline
 * sync, the drop, then the space's identifier for the context) and
 * amdgpu_ib.c (amdgpu_ib_schedule).
 */
int
amdrdna_space_submit(
	struct amdrdna_space *space,
	unsigned tracked,
	uint64_t address,
	uint32_t words,
	struct amdrdna_space_ticket *ticket)
{
	uint32_t commands[SPACE_RUN_COMMANDS];
	uint32_t passed[AMDRDNA_TRACKED_RINGS];
	struct amdrdna_context_request request;
	struct amdrdna_context_grant grant;
	struct amdrdna_device *device;
	struct amdrdna_ring *ring;
	struct amdrdna_ring *waited;
	unsigned long irq;
	uint32_t sequence;
	uint32_t other_sequence;
	unsigned count;
	unsigned tries;
	bool submitted;
	bool other;
	int error;

	/* No run yet. */
	kern_memset(ticket, 0, sizeof(*ticket));

	/* A command ring that runs (the copy engine reads other packets), and a buffer of whole dwords. */
	device = space->device;
	if (tracked >= AMDRDNA_TRACKED_COPY)
		return ENOTSUP;
	ring = space_ring(device, tracked);
	if (ring == NULL)
		return ENODEV;
	if (words == 0U || (address & 3U) != 0U)
		return EINVAL;

	/* Another space's newest run on the ring, when it is still there. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	other = (device->spaces.last_space[tracked] != 0U && device->spaces.last_space[tracked] != space->id);
	other_sequence = device->spaces.last_sequence[tracked];

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* Waits for it to end, or for a reset to drop it; a run that is not done by then is queued behind it all the same. */
	if (other)
		(void)amdrdna_ring_wait(device, ring, other_sequence, SPACE_RUN_MS * 2U);

	/* Tells the power side the GPU works, so the graphics clock is raised for it. */
	amdrdna_power_busy(device);

	/* The GPU must read what the CPU wrote to VRAM. */
	amdrdna_host_path_flush(device);

	/* Gets a context and submits the run, waiting for a context when none is free; no queue is reset meanwhile. */
	submitted = false;
	sequence = 0U;
	error = 0;
	for (tries = 0U; tries < SPACE_GRANT_TRIES; tries++) {
		mutex_lock(&device->spaces.submit_lock);
		irq = spin_lock_irqsave(&device->spaces.lock);

		/* A failed device or a lost space runs nothing (said, since the session only sees EIO). */
		if (!device->spaces.ready ||
		    device->spaces.faulted ||
		    space->lost) {
			spin_unlock_irqrestore(&device->spaces.lock, irq);
			mutex_unlock(&device->spaces.submit_lock);
			amdrdna_mark("space %u: run on ring %u refused (ready %d faulted %d lost %d)", (unsigned)space->id, tracked,
			    (int)device->spaces.ready, (int)device->spaces.faulted, (int)space->lost);
			return EIO;
		}

		/* What the run asks of the pool, and the rings' fences. */
		request.owner = space->id;
		request.held = space->context;
		request.table = space->root_word;
		request.generation = space->generation;
		request.ring = tracked;
		space_passed(device, passed);
		error = amdrdna_context_grant(&device->spaces.pool, &request, passed, &grant);
		if (error != 0) {
			spin_unlock_irqrestore(&device->spaces.lock, irq);
			mutex_unlock(&device->spaces.submit_lock);
			return error;
		}

		/* No context is free: waits for one without the locks, then asks again. */
		if (grant.wait) {
			spin_unlock_irqrestore(&device->spaces.lock, irq);
			mutex_unlock(&device->spaces.submit_lock);
			waited = space_ring(device, grant.wait_ring);
			if (waited != NULL)
				(void)amdrdna_ring_wait(device, waited, grant.wait_sequence, SPACE_RUN_MS);
			continue;
		}

		/* Builds the commands and puts them on the ring. */
		count = space_commands(space, ring, tracked, &grant, address, words, commands);
		error = amdrdna_ring_submit(device, ring, commands, count, true, &sequence);
		if (error == 0) {
			amdrdna_context_submitted(&device->spaces.pool, grant.context, &request, sequence, grant.flush);
			space->context = grant.context;
			device->spaces.runs++;
			device->spaces.last_space[tracked] = space->id;
			device->spaces.last_sequence[tracked] = sequence;
			ticket->resets = device->spaces.resets_of[tracked];
			submitted = true;
		}

		spin_unlock_irqrestore(&device->spaces.lock, irq);
		mutex_unlock(&device->spaces.submit_lock);
		break;
	}

	/* Reports a ring that was full. */
	if (error != 0)
		return error;

	/* Reports a run that never got a context. */
	if (!submitted)
		return ETIMEDOUT;

	/* Succeeded: the ring runs it; the ticket names it. */
	ticket->tracked = tracked;
	ticket->sequence = sequence;
	ticket->context = grant.context;
	ticket->issued = true;
	return 0;
}

/*
 * Waits up to milliseconds for a run of an address space to end.  A run
 * that does not end loses the space, and its queue is reset so the other
 * sessions go on (a queue that cannot be reset fails the device); a run
 * a reset dropped, or of a space that was lost, failed too.  A ticket of
 * no run has nothing to wait for.
 *
 * Returns 0 once the run ended (the CPU then reads what it wrote), or EIO.
 */
int
amdrdna_space_wait(
	struct amdrdna_space *space,
	const struct amdrdna_space_ticket *ticket,
	unsigned milliseconds)
{
	struct amdrdna_device *device;
	struct amdrdna_ring *ring;
	unsigned long irq;
	uint32_t resets;
	uint32_t after;
	uint32_t last;
	bool dropped;
	bool lost;
	int error;

	/* No run, nothing to wait for. */
	if (!ticket->issued)
		return 0;

	/* The ring the run is on. */
	device = space->device;
	ring = space_ring(device, ticket->tracked);
	if (ring == NULL)
		return EIO;

	/* Waits for the fence after the run; a run that does not end hangs the queue. */
	error = amdrdna_ring_wait(device, ring, ticket->sequence, milliseconds);
	if (error != 0) {
		space_hang(space, ticket);
		return EIO;
	}

	/* A run a reset dropped, or of a lost space, did not do its work. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	dropped = space_dropped_locked(device, ticket);
	lost = space->lost;
	resets = device->spaces.resets_of[ticket->tracked];
	after = device->spaces.dropped_after[ticket->tracked];
	last = device->spaces.dropped_last[ticket->tracked];

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* Reports a run that did not do its work, and says why (the session only sees EIO). */
	if (dropped || lost) {
		amdrdna_mark("space %u: run %u on ring %u did no work (dropped %d: resets %u then %u, dropped %u..%u; lost %d)",
		    (unsigned)space->id, (unsigned)ticket->sequence, ticket->tracked, (int)dropped, (unsigned)ticket->resets,
		    (unsigned)resets, (unsigned)after, (unsigned)last, (int)lost);
		return EIO;
	}

	/* The CPU reads what the GPU wrote to VRAM, not what the host path held. */
	amdrdna_host_path_drop(device);

	/* Succeeded: the run ended. */
	return 0;
}

/*
 * Reports whether a run of an address space is no longer running: its
 * ring's fence passed it (it ended, or a reset dropped it).  A ticket of
 * no run has passed.  Nothing waits.
 */
bool
amdrdna_space_passed(
	struct amdrdna_space *space,
	const struct amdrdna_space_ticket *ticket)
{
	struct amdrdna_ring *ring;
	uint32_t fence;

	/* No run has passed. */
	if (!ticket->issued)
		return true;

	/* A ring that does not run any more runs nothing. */
	ring = space_ring(space->device, ticket->tracked);
	if (ring == NULL)
		return true;

	/* The fence against the run's value (the difference handles the wrap). */
	fence = amdrdna_ring_fence(space->device, ring);
	if ((int32_t)(fence - ticket->sequence) < 0)
		return false;

	/* Succeeded: the run is no longer running. */
	return true;
}

/* Reports whether a ticket's run is the newest on its ring (the ring's last run stamps are its own then). */
bool
amdrdna_space_newest(
	struct amdrdna_space *space,
	const struct amdrdna_space_ticket *ticket)
{
	struct amdrdna_ring *ring;
	uint32_t newest;

	/* No run, or a ring that does not run, is no newest run. */
	if (!ticket->issued)
		return false;
	ring = space_ring(space->device, ticket->tracked);
	if (ring == NULL)
		return false;

	/* The ring's newest submission. */
	newest = amdrdna_ring_sequence(ring);
	if (newest != ticket->sequence)
		return false;

	/* Succeeded: nothing was put on the ring after it. */
	return true;
}

/*
 * Reports whether no work of an address space can still run: it holds
 * no context, or every ring has passed the work of its context.
 */
bool
amdrdna_space_quiet(
	struct amdrdna_space *space)
{
	unsigned long irq;
	bool quiet;

	/* Read under the spaces' lock. */
	irq = spin_lock_irqsave(&space->device->spaces.lock);

	quiet = space_quiet_locked(space);

	spin_unlock_irqrestore(&space->device->spaces.lock, irq);

	/* Succeeded: the answer. */
	return quiet;
}

/*
 * Keeps an address space after a run of the video engine in it did not
 * end: the engine may still read and write what the space maps, so its
 * tables and its memory stay until the engine is seen to halt
 * (amdrdna_space_video_release) or the GPU is reset.
 */
void
amdrdna_space_video_hold(
	struct amdrdna_space *space)
{
	unsigned long irq;

	/* Set under the spaces' lock, where the space's quiet is read. */
	irq = spin_lock_irqsave(&space->device->spaces.lock);

	space->video_held = true;

	spin_unlock_irqrestore(&space->device->spaces.lock, irq);
}

/*
 * Lets go of the spaces kept after runs of the video engine that did not
 * end, once the engine was seen to halt: nothing of it runs in them any
 * more, so their memory goes back as any other space's.  A space already
 * destroyed while kept stays held until the GPU is reset.  Returns how
 * many spaces were let go.
 */
unsigned
amdrdna_space_video_release(
	struct amdrdna_device *device)
{
	struct amdrdna_space *space;
	unsigned long irq;
	unsigned count;

	/* Cleared under the spaces' lock, where the spaces' quiet is read. */
	count = 0U;
	irq = spin_lock_irqsave(&device->spaces.lock);

	for (space = device->spaces.list; space != NULL; space = space->next) {
		if (space->video_held) {
			space->video_held = false;
			count++;
		}
	}

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* Succeeded: the count. */
	return count;
}

/* Reports whether an address space was lost. */
bool
amdrdna_space_lost(
	struct amdrdna_space *space)
{
	unsigned long irq;
	bool lost;

	/* Read under the spaces' lock. */
	irq = spin_lock_irqsave(&space->device->spaces.lock);

	lost = space->lost;

	spin_unlock_irqrestore(&space->device->spaces.lock, irq);

	/* Succeeded: the answer. */
	return lost;
}

/*
 * Makes every later run on the graphics ring write the GPU's timestamp
 * counter at its start and at its end (on), or not, for the executor's
 * timing.
 */
void
amdrdna_spaces_stamp(
	struct amdrdna_device *device,
	bool on)
{
	unsigned long irq;

	/* Set under the spaces' lock, which every run's commands are built under. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	device->spaces.stamped = on;

	spin_unlock_irqrestore(&device->spaces.lock, irq);
}

/*
 * Marks the waves still on the shader arrays when a run did not end: each
 * valid wave's place, status, program counter, the instruction there and
 * its lane mask (at most SPACE_WAVES_MARKED of them), so that the
 * instruction the waves wait at is known.
 *
 * Procedure referenced: Linux v6.12
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (wave_read_ind: the index word
 * with the wave and the register, then the data word;
 * gfx_v10_0_read_wave_data: the registers read; the array selected first
 * through GRBM_GFX_INDEX and every array selected again after) and
 * drivers/gpu/drm/amd/amdgpu/amdgpu_debugfs.c (amdgpu_debugfs_wave_read:
 * the shader engine, the array and the instance selected).
 */
void
amdrdna_spaces_mark_waves(
	struct amdrdna_device *device)
{
	uint32_t engine;
	uint32_t array;
	uint32_t instance;
	uint32_t wave;
	uint32_t select;
	uint32_t status;
	uint32_t pc_low;
	uint32_t pc_high;
	uint32_t instruction;
	uint32_t exec;
	uint32_t identity;
	uint32_t marked;
	uint32_t seen;

	/* Every engine, array, instance and wave slot, read in turn. */
	marked = 0U;
	seen = 0U;
	for (engine = 0U; engine < SPACE_SHADER_ENGINES; engine++) {
		for (array = 0U; array < SPACE_SHADER_ARRAYS; array++) {
			for (instance = 0U; instance < SPACE_WAVE_INSTANCES; instance++) {
				/* The array and the instance the window reads. */
				select = instance << AMDRDNA_GC_SELECT_INSTANCE_SHIFT;
				select |= array << AMDRDNA_GC_SELECT_ARRAY_SHIFT;
				select |= engine << AMDRDNA_GC_SELECT_ENGINE_INDEX_SHIFT;
				amdrdna_block_write(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_ARRAY_SELECT, select);

				/* Each wave slot: its status, and the rest of a valid one. */
				for (wave = 0U; wave < SPACE_WAVE_SLOTS; wave++) {
					status = space_wave_read(device, wave, AMDRDNA_WAVE_STATUS);
					if ((status & AMDRDNA_WAVE_STATUS_VALID) == 0U)
						continue;
					seen++;
					if (marked == SPACE_WAVES_MARKED)
						continue;

					/* Reads where the wave is and what it runs. */
					pc_low = space_wave_read(device, wave, AMDRDNA_WAVE_PC_LOW);
					pc_high = space_wave_read(device, wave, AMDRDNA_WAVE_PC_HIGH);
					instruction = space_wave_read(device, wave, AMDRDNA_WAVE_INSTRUCTION);
					exec = space_wave_read(device, wave, AMDRDNA_WAVE_EXEC);
					identity = space_wave_read(device, wave, AMDRDNA_WAVE_IDENTITY);
					amdrdna_mark("wave se%u sa%u i%u w%u status 0x%08x pc 0x%x%08x inst 0x%08x exec 0x%08x id 0x%08x",
						     engine,
						     array,
						     instance,
						     wave,
						     status,
						     pc_high,
						     pc_low,
						     instruction,
						     exec,
						     identity);
					marked++;
				}
			}
		}
	}

	/* Every array again, as the rest of the driver writes them. */
	select = AMDRDNA_GC_SELECT_ALL_INSTANCES | AMDRDNA_GC_SELECT_ALL_ARRAYS | AMDRDNA_GC_SELECT_ALL_ENGINES;
	amdrdna_block_write(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_ARRAY_SELECT, select);

	/* Says how many there were. */
	amdrdna_mark("waves valid %u, marked %u", seen, marked);
}

/*
 * Marks the graphics block's status words, which say which units still
 * work: when a run did not end, and with the executor's timing.
 */
void
amdrdna_spaces_mark_status(
	struct amdrdna_device *device)
{
	uint32_t status;
	uint32_t status2;
	uint32_t engine0;
	uint32_t engine1;
	uint32_t status3;
	uint32_t cp_busy;
	uint32_t cp_status;
	uint32_t clip;
	uint32_t setup;

	/* The words, read as they are. */
	status = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_BLOCK_STATUS);
	status2 = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_BLOCK_STATUS2);
	engine0 = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_ENGINE0_STATUS);
	engine1 = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_ENGINE1_STATUS);
	status3 = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_BLOCK_STATUS3);
	cp_busy = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_CP_BUSY);
	cp_status = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_CP_STATUS);
	clip = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_CLIP_STATUS);
	setup = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_SETUP_STATUS);

	/* Says them. */
	amdrdna_mark("gc status 0x%08x status2 0x%08x status3 0x%08x se0 0x%08x se1 0x%08x cp 0x%08x/0x%08x pa 0x%08x/0x%08x",
		     status,
		     status2,
		     status3,
		     engine0,
		     engine1,
		     cp_busy,
		     cp_status,
		     clip,
		     setup);
}

/*
 * Reports how many ticks of the GPU's timestamp counter the last run on
 * the graphics ring took, from the command processor's start to its end,
 * and the counter at its start; 0 and 0 when runs are not stamped.  The
 * caller ran that run and nothing has run on the ring since (the node's
 * lock keeps the executor's runs in turn).
 */
uint64_t
amdrdna_space_run_ticks(
	struct amdrdna_space *space,
	uint64_t *started)
{
	struct amdrdna_device *device;
	struct amdrdna_ring *ring;
	uint64_t start;
	uint64_t end;

	/* Nothing was stamped without the timing. */
	device = space->device;
	*started = 0U;
	if (!device->spaces.stamped)
		return 0U;

	/* Nor without the graphics ring. */
	ring = space_ring(device, AMDRDNA_TRACKED_GRAPHICS);
	if (ring == NULL)
		return 0U;

	/* The counter at the run's start and at its end. */
	amdrdna_ring_stamps(device, ring, &start, &end);
	*started = start;

	/* A pair that does not go forward was not this run's. */
	if (end < start)
		return 0U;

	/* Succeeded: the ticks between. */
	return end - start;
}

/*
 * Takes a page fault a hub reported to the address space it belongs to:
 * the space the identifier names, or, without one, the space the faulting
 * context is handed to.  The space is lost and its session told; a fault
 * of the kernel's context or of no known space is left to the log.
 */
void
amdrdna_space_fault(
	struct amdrdna_device *device,
	const struct amdrdna_vm_fault *fault,
	unsigned context)
{
	struct amdrdna_space *space;
	struct amdrdna_space *found;
	unsigned long irq;
	void *session;
	bool first;

	/* Finds the space and marks it lost, once. */
	found = NULL;
	first = false;
	session = NULL;
	irq = spin_lock_irqsave(&device->spaces.lock);

	for (space = device->spaces.list; space != NULL; space = space->next) {
		/* The identifier names the space. */
		if (fault->space_id != 0U && space->id == fault->space_id) {
			found = space;
			break;
		}

		/* Without one, the context's owner is the space. */
		if (fault->space_id == 0U &&
		    context != 0U &&
		    space->context == context &&
		    device->spaces.pool.slots[context].owner == space->id) {
			found = space;
			break;
		}
	}

	/* The space counts the fault and is lost, the first time with its session noted. */
	if (found != NULL) {
		found->faults++;
		if (!found->lost) {
			found->lost = true;
			found->lost_error = EFAULT;
			device->spaces.lost++;
			session = found->session;
			first = true;
		}
	}

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* Says which space lost its work, the first time. */
	if (!first)
		return;
	amdrdna_mark("space %u lost: page fault at 0x%llx in context %u",
	    (unsigned)found->id,
	    (unsigned long long)fault->fault_address,
	    context);

	/* Tells the session; its later work is refused. */
	if (session != NULL)
		amdrdna_node_session_lost(device, session, EIO);
}

/*
 * Takes a command error a ring reported for a context (a privileged
 * register or instruction, or a bad opcode): the space the context is
 * handed to is lost and its session told; its run is then given up when
 * its time is up (the queue is reset).  An error of the kernel's context
 * or of no known space is left to the log.
 *
 * The CP's exceptions lose the session at once; the queue's reset follows.
 */
void
amdrdna_space_command_error(
	struct amdrdna_device *device,
	unsigned context,
	unsigned source)
{
	struct amdrdna_space *space;
	struct amdrdna_space *found;
	unsigned long irq;
	void *session;
	bool first;

	/* The kernel's context has no space. */
	if (context == 0U)
		return;

	/* Finds the context's owner and marks it lost, once. */
	found = NULL;
	first = false;
	session = NULL;
	irq = spin_lock_irqsave(&device->spaces.lock);

	for (space = device->spaces.list; space != NULL; space = space->next) {
		if (space->context == context && device->spaces.pool.slots[context].owner == space->id) {
			found = space;
			break;
		}
	}
	if (found != NULL && !found->lost) {
		found->lost = true;
		found->lost_error = EIO;
		device->spaces.lost++;
		session = found->session;
		first = true;
	}

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* Says which space lost its work, the first time, and tells the session. */
	if (!first)
		return;
	amdrdna_mark("space %u lost: command error 0x%x in context %u", (unsigned)found->id, source, context);
	if (session != NULL)
		amdrdna_node_session_lost(device, session, EIO);
}

/*
 * Marks what the hub and the tables say about an address of a space, for
 * a check that failed: the context the space holds, that context's table
 * words and control word as the hub reads them, the done word of the
 * graphics ring's invalidation engine, and the entry of each level the
 * address goes through, read back through the CPU's window.
 */
void
amdrdna_space_describe(
	struct amdrdna_space *space,
	uint64_t address)
{
	struct amdrdna_device *device;
	struct amdrdna_table *table;
	unsigned index[AMDRDNA_VM_LEVELS];
	uint64_t entries[AMDRDNA_VM_LEVELS];
	uint32_t base_low;
	uint32_t base_high;
	uint32_t control;
	volatile uint8_t *cpu;
	uint32_t done;
	unsigned context;
	unsigned level;

	/* The context and the hub's words for it. */
	device = space->device;
	context = space->context;
	base_low = space_hub_read(device, AMDRDNA_HUB_TABLE_BASE_LOW, context);
	base_high = space_hub_read(device, AMDRDNA_HUB_TABLE_BASE_HIGH, context);
	control = space_hub_read(device, AMDRDNA_HUB_CONTEXT_CONTROL, context);
	done = space_hub_read(device, AMDRDNA_HUB_INVALIDATE_DONE, SPACE_ENGINE_GRAPHICS);

	/* The entry of each level, as far as the tables go. */
	amdrdna_vm_split(address, index);
	table = space->root;
	for (level = 0U; level < AMDRDNA_VM_LEVELS; level++) {
		/* A level the tables do not reach reads as no entry. */
		entries[level] = 0U;
		if (table == NULL)
			continue;

		/* The entry, then the table below it. */
		cpu = amdrdna_vram_cpu(device, table->vram);
		entries[level] = kern_mmio_read64(cpu + (size_t)index[level] * SPACE_ENTRY_BYTES);
		if (table->below == NULL) {
			table = NULL;
		} else {
			table = table->below[index[level]];
		}
	}

	/* One line for the hub, one for the tables. */
	amdrdna_mark("space %u ctx=%u root=0x%llx hub base=0x%08x%08x control=0x%08x done=0x%08x",
	    (unsigned)space->id,
	    context,
	    (unsigned long long)space->root_word,
	    (unsigned)base_high,
	    (unsigned)base_low,
	    (unsigned)control,
	    (unsigned)done);
	amdrdna_mark("space %u at 0x%llx: 0x%llx 0x%llx 0x%llx 0x%llx",
	    (unsigned)space->id,
	    (unsigned long long)address,
	    (unsigned long long)entries[0],
	    (unsigned long long)entries[1],
	    (unsigned long long)entries[2],
	    (unsigned long long)entries[3]);
}

/*
 * Creates a backing: zeroed VRAM in the CPU's window (cpu true) or in the
 * VRAM only the GPU reaches, in whole 64 KiB steps, held once by the
 * caller.
 *
 * Returns 0, EINVAL for no bytes, ENOSPC when the pool has no run of the
 * size, ENOMEM for the record, or the copy engine's error for VRAM outside
 * the window.
 */
int
amdrdna_backing_create(
	struct amdrdna_device *device,
	uint64_t bytes,
	bool cpu,
	struct amdrdna_backing **result)
{
	struct amdrdna_backing *backing;
	volatile uint8_t *window;
	uint64_t whole;
	uint64_t align;
	uint64_t at;
	int error;

	/* Some bytes, in whole steps. */
	*result = NULL;
	if (bytes == 0U)
		return EINVAL;
	whole = (bytes + SPACE_MAPPING_ALIGN - 1U) & ~(SPACE_MAPPING_ALIGN - 1U);

	/* The record. */
	backing = kern_calloc(1U, sizeof(*backing));
	if (backing == NULL)
		return ENOMEM;

	/* The VRAM, from the pool asked for: on the backing's boundary, or the plain one when no run is free there. */
	align = space_align(whole);
	error = amdrdna_vram_take(device, cpu, whole, align, &backing->vram);
	if (error != 0)
		error = amdrdna_vram_take(device, cpu, whole, SPACE_MAPPING_ALIGN, &backing->vram);
	if (error != 0) {
		kern_free(backing);
		return ENOSPC;
	}

	/* Cleared, so no earlier contents show: through the window, or by the copy engine outside it. */
	if (cpu) {
		window = amdrdna_vram_cpu(device, backing->vram);
		for (at = 0U; at < whole; at += 8U)
			kern_mmio_write64(window + at, 0U);
	} else {
		error = amdrdna_engines_clear(device, backing->vram, whole);
		if (error != 0) {
			amdrdna_vram_give(device, false, backing->vram, whole);
			kern_free(backing);
			return error;
		}
	}

	/* One holder: the caller. */
	backing->bytes = whole;
	backing->asked = bytes;
	backing->refs = 1U;
	backing->cpu = cpu;
	backing->kept = false;

	/* Succeeded: the backing is the caller's. */
	*result = backing;
	return 0;
}

/*
 * Creates a backing of system memory: a zeroed contiguous run, in whole
 * 64 KiB steps, the CPU writes through its cache and the GPU reads
 * coherently with it, held once by the caller.
 *
 * The run is plain RAM, not the bus's DMA memory: that is the memory
 * below 4 GiB every PCI device shares (the USB network adapter among
 * them) and counts against the caches' budget, and the executor's runs
 * would take megabytes of it.  The GPU reaches all of RAM, at its
 * physical address (the PC's bus maps it one to one).
 *
 * Returns 0, EINVAL for no bytes, or ENOMEM.
 */
int
amdrdna_backing_create_system(
	struct amdrdna_device *device,
	uint64_t bytes,
	struct amdrdna_backing **result)
{
	struct amdrdna_backing *backing;
	uint64_t whole;
	int error;

	UNUSED_PARAMETER(device);

	/* Some bytes, in whole steps. */
	*result = NULL;
	if (bytes == 0U)
		return EINVAL;
	whole = (bytes + SPACE_MAPPING_ALIGN - 1U) & ~(SPACE_MAPPING_ALIGN - 1U);

	/* The record. */
	backing = kern_calloc(1U, sizeof(*backing));
	if (backing == NULL)
		return ENOMEM;

	/* The run, on the mapping boundary. */
	error = kern_pmem_alloc((size_t)whole, SPACE_MAPPING_ALIGN, &backing->ram);
	if (error != 0) {
		kern_free(backing);
		return ENOMEM;
	}

	/* Its view through the direct map. */
	backing->system_cpu = kern_pmem_to_kernel(backing->ram.paddr);
	if (backing->system_cpu == NULL) {
		(void)kern_pmem_free(&backing->ram);
		kern_free(backing);
		return ENOMEM;
	}

	/* Cleared, so no earlier contents show. */
	kern_memset(backing->system_cpu, 0, (size_t)whole);

	/* The first few runs say where they are (whether the GPU reads RAM above 4 GiB shows in their work). */
	if (space_system_marks < SPACE_SYSTEM_MARKS) {
		space_system_marks++;
		amdrdna_mark("system run at physical 0x%llx, %llu KiB", (unsigned long long)backing->ram.paddr, (unsigned long long)(whole >> 10));
	}

	/* One holder: the caller. */
	backing->bytes = whole;
	backing->asked = bytes;
	backing->refs = 1U;
	backing->cpu = true;
	backing->system = true;
	backing->kept = false;

	/* Succeeded: the backing is the caller's. */
	*result = backing;
	return 0;
}

/* Takes one more reference to a backing. */
void
amdrdna_backing_get(
	struct amdrdna_device *device,
	struct amdrdna_backing *backing)
{
	unsigned long irq;

	/* refs counts the holders; the last put gives the VRAM back. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	backing->refs++;

	spin_unlock_irqrestore(&device->spaces.lock, irq);
}

/*
 * Drops one reference to a backing: the last one gives the VRAM back,
 * unless work nobody could stop may still touch it, and frees the record.
 */
void
amdrdna_backing_put(
	struct amdrdna_device *device,
	struct amdrdna_backing *backing)
{
	unsigned long irq;
	uint32_t refs;
	bool kept;

	/* Counts the reference off. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	backing->refs--;
	refs = backing->refs;
	kept = backing->kept;

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* Others still hold it. */
	if (refs != 0U)
		return;

	/* The memory goes back unless it is kept for good: the system run, or the VRAM. */
	if (!kept && backing->system) {
		(void)kern_pmem_free(&backing->ram);
	} else if (!kept) {
		amdrdna_vram_give(device, backing->cpu, backing->vram, backing->bytes);
	}

	/* The record. */
	kern_free(backing);
}

/* Keeps a backing's VRAM for good: GPU work nobody could stop may still touch it. */
void
amdrdna_backing_keep(
	struct amdrdna_device *device,
	struct amdrdna_backing *backing)
{
	unsigned long irq;

	/* kept makes the last put leave the VRAM taken. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	backing->kept = true;

	spin_unlock_irqrestore(&device->spaces.lock, irq);
}

/* Reports where the CPU reads and writes a backing: in its window, or its system run; NULL for VRAM outside the window. */
volatile uint8_t *
amdrdna_backing_cpu(
	struct amdrdna_device *device,
	const struct amdrdna_backing *backing)
{
	volatile uint8_t *cpu;

	/* Outside the window the CPU does not reach it. */
	if (!backing->cpu)
		return NULL;

	/* A run of system memory is written through the CPU's own view of it. */
	if (backing->system)
		return backing->system_cpu;

	/* The window's view of the VRAM. */
	cpu = amdrdna_vram_cpu(device, backing->vram);

	/* Succeeded: the CPU's address. */
	return cpu;
}

/*
 * Gives the fragment field of the entry of the page at address, VRAM at
 * physical, in a mapping of bytes at start: the largest aligned run of
 * 2^(12 + n) bytes around the page that lies inside the mapping and whose
 * GPU and VRAM addresses agree below the run's size (one translation then
 * covers it), or 0.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c
 * (amdgpu_vm_pte_fragment, amdgpu_vm_ptes_update: every entry of an
 * aligned run of contiguous VRAM carries the run's fragment).
 */
static uint64_t
space_fragment(
	uint64_t start,
	uint64_t bytes,
	uint64_t address,
	uint64_t physical)
{
	uint64_t run;
	uint64_t first;
	unsigned fragment;

	/* Tries the runs from the largest down. */
	for (fragment = SPACE_FRAGMENT_MOST; fragment > 0U; fragment--) {
		/* The run around the page. */
		run = (uint64_t)SPACE_PAGE_BYTES << fragment;
		first = address & ~(run - 1U);

		/* Skips a run whose VRAM is not on the same boundary. */
		if (((address ^ physical) & (run - 1U)) != 0U)
			continue;

		/* Skips a run that reaches outside the mapping. */
		if (first < start)
			continue;
		if (first + run > start + bytes)
			continue;

		/* Succeeded: the run's field. */
		return (uint64_t)fragment << AMDRDNA_PAGE_FRAGMENT_SHIFT;
	}

	/* Only the page itself. */
	return 0U;
}

/*
 * Gives the boundary of a backing of bytes (whole 64 KiB steps): the
 * largest power of two not above it, from 64 KiB to 2 MiB.
 */
static uint64_t
space_align(
	uint64_t bytes)
{
	uint64_t align;

	/* Doubles the boundary while the backing still holds it. */
	align = SPACE_MAPPING_ALIGN;
	while (align < SPACE_LARGE_ALIGN && align * 2U <= bytes)
		align *= 2U;

	/* Succeeded: the boundary. */
	return align;
}

/* Reports the ring a tracked ring stands for, or NULL when it does not run. */
static struct amdrdna_ring *
space_ring(
	struct amdrdna_device *device,
	unsigned tracked)
{
	struct amdrdna_engines *engines;
	unsigned compute;

	/* The graphics ring. */
	engines = &device->engines;
	if (tracked == AMDRDNA_TRACKED_GRAPHICS) {
		if (!engines->graphics_running)
			return NULL;
		return &engines->graphics;
	}

	/* A compute queue the stage attached. */
	if (tracked >= AMDRDNA_TRACKED_COMPUTE && tracked < AMDRDNA_TRACKED_COPY) {
		compute = tracked - AMDRDNA_TRACKED_COMPUTE;
		if (!engines->compute_running || compute >= engines->compute_count)
			return NULL;
		return &engines->compute[compute];
	}

	/* The first copy engine. */
	if (tracked == AMDRDNA_TRACKED_COPY) {
		if (!engines->copy_running)
			return NULL;
		return &engines->copy;
	}

	/* No such ring. */
	return NULL;
}

/* Reads the fence each tracked ring last wrote (0 for one that does not run). */
static void
space_passed(
	struct amdrdna_device *device,
	uint32_t *passed)
{
	struct amdrdna_ring *ring;
	unsigned tracked;

	/* Each tracked ring's fence word. */
	for (tracked = 0U; tracked < AMDRDNA_TRACKED_RINGS; tracked++) {
		ring = space_ring(device, tracked);
		passed[tracked] = 0U;
		if (ring != NULL)
			passed[tracked] = amdrdna_ring_fence(device, ring);
	}
}

/*
 * Builds a run's commands: a wait for the ring's own earlier work when the
 * context still ran some (sync), the drop of the context's translations
 * when they are stale (the context pointed at the space's root table, the
 * hub asked to drop them and waited for, the interrupt controller told the
 * context's space, the graphics ring's fetcher held until the engine
 * caught up), the caches synced, then the indirect buffer in the context.
 * Reports the words.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c (amdgpu_ib_schedule: the memory
 * sync before a job's indirect buffers),
 * gmc_v10_0.c (gmc_v10_0_emit_flush_gpu_tlb,
 * gmc_v10_0_emit_pasid_mapping) and gfx_v10_0.c
 * (gfx_v10_0_ring_emit_pipeline_sync, gfx_v10_0_ring_emit_vm_flush,
 * gfx_v10_0_ring_emit_ib_gfx, gfx_v10_0_ring_emit_ib_compute).
 */
static unsigned
space_commands(
	struct amdrdna_space *space,
	struct amdrdna_ring *ring,
	unsigned tracked,
	const struct amdrdna_context_grant *grant,
	uint64_t address,
	uint32_t words,
	uint32_t *commands)
{
	struct amdrdna_device *device;
	uint64_t fence_address;
	uint32_t fence_sequence;
	uint32_t table_low;
	uint32_t table_high;
	uint32_t request;
	uint32_t done;
	uint32_t identifier;
	uint32_t invalidate;
	uint64_t stamp_address;
	unsigned engine;
	unsigned count;
	bool graphics;
	bool compute;
	bool stamped;
	int error;

	/* The ring's kind and its invalidation engine: the graphics ring or a compute queue. */
	device = space->device;
	if (tracked == AMDRDNA_TRACKED_GRAPHICS) {
		graphics = true;
		compute = false;
		engine = SPACE_ENGINE_GRAPHICS;
	} else {
		graphics = false;
		compute = true;
		engine = SPACE_ENGINE_COMPUTE_FIRST + (tracked - AMDRDNA_TRACKED_COMPUTE);
	}

	/* The ring waits for its own earlier work in the context first: its fence reaching its newest sequence. */
	count = 0U;
	if (grant->sync) {
		fence_address = amdrdna_ring_fence_address(device, ring);
		fence_sequence = amdrdna_ring_sequence(ring);
		count += amdrdna_pm4_wait_memory(commands + count, fence_address, fence_sequence, graphics);
	}

	/* The drop of the context's translations, when they are stale. */
	if (grant->flush) {
		/* The registers of the context and of the ring's invalidation engine. */
		table_low = amdrdna_hub_register(device, AMDRDNA_HUB_GRAPHICS, AMDRDNA_HUB_TABLE_BASE_LOW, grant->context);
		table_high = amdrdna_hub_register(device, AMDRDNA_HUB_GRAPHICS, AMDRDNA_HUB_TABLE_BASE_HIGH, grant->context);
		request = amdrdna_hub_register(device, AMDRDNA_HUB_GRAPHICS, AMDRDNA_HUB_INVALIDATE_REQUEST, engine);
		done = amdrdna_hub_register(device, AMDRDNA_HUB_GRAPHICS, AMDRDNA_HUB_INVALIDATE_DONE, engine);
		identifier = 0U;
		error = amdrdna_block_index(device, AMDRDNA_BLOCK_IH, AMDRDNA_IH_WINDOW_MAIN, AMDRDNA_IH_REG_SPACE_TO_PROCESS + grant->context, &identifier);
		if (error != 0)
			identifier = 0U;

		/* The context points at the space's root table. */
		if (graphics) {
			count += amdrdna_pm4_fetcher_write_register(commands + count, table_low, (uint32_t)space->root_word);
			count += amdrdna_pm4_fetcher_write_register(commands + count, table_high, (uint32_t)(space->root_word >> 32));
		} else {
			count += amdrdna_pm4_write_register(commands + count, table_low, (uint32_t)space->root_word, false);
			count += amdrdna_pm4_write_register(commands + count, table_high, (uint32_t)(space->root_word >> 32), false);
		}

		/* The hub drops the context's translations, and the ring waits for it. */
		invalidate = amdrdna_invalidate_word(grant->context);
		count += amdrdna_pm4_write_and_wait(commands + count, request, done, invalidate, 1U << grant->context, graphics);

		/* The interrupt controller names the context's faults after the space. */
		if (identifier != 0U) {
			if (graphics) {
				count += amdrdna_pm4_fetcher_write_register(commands + count, identifier, space->id);
			} else {
				count += amdrdna_pm4_write_register(commands + count, identifier, space->id, false);
			}
		}

		/* The graphics ring's fetcher waits for the engine before it reads on. */
		if (graphics)
			count += amdrdna_pm4_fetcher_sync(commands + count);
	}

	/*
	 * The GPU's caches dropped and written back, so the indirect buffer and
	 * what it reads come from memory as the CPU last wrote them through the
	 * window (which the level-2 cache does not see).
	 */
	count += amdrdna_pm4_cache_sync(commands + count, false);

	/* Only the graphics ring's runs are timed, and only while the executor's timing asks. */
	stamped = false;
	if (graphics && device->spaces.stamped)
		stamped = true;

	/* A timed run notes the GPU's counter as the command processor starts it. */
	if (stamped) {
		stamp_address = amdrdna_ring_stamp_address(device, ring, false);
		count += amdrdna_pm4_stamp(commands + count, stamp_address, compute);
	}

	/* The indirect buffer, in the context. */
	count += amdrdna_pm4_indirect(commands + count, address, words, grant->context, compute);

	/* And again once everything the buffer started has finished. */
	if (stamped) {
		stamp_address = amdrdna_ring_stamp_address(device, ring, true);
		count += amdrdna_pm4_stamp_done(commands + count, stamp_address, compute);
	}

	/* Succeeded: the words. */
	return count;
}

/*
 * Makes one page table of a level: a page of the CPU's VRAM cleared to no
 * entries, and for a directory the list of the tables below it.
 *
 * Returns 0, ENOSPC when the VRAM has no page for it, or ENOMEM.
 */
static int
space_table_create(
	struct amdrdna_device *device,
	unsigned level,
	struct amdrdna_table **result)
{
	struct amdrdna_table *table;
	volatile uint8_t *cpu;
	unsigned index;
	int error;

	/* The record. */
	*result = NULL;
	table = kern_calloc(1U, sizeof(*table));
	if (table == NULL)
		return ENOMEM;

	/* A directory lists the tables below it. */
	if (level < SPACE_LEAF_LEVEL) {
		table->below = kern_calloc(SPACE_TABLE_ENTRIES, sizeof(*table->below));
		if (table->below == NULL) {
			kern_free(table);
			return ENOMEM;
		}
	}

	/* The table's page of VRAM. */
	error = amdrdna_vram_take(device, true, SPACE_PAGE_BYTES, SPACE_PAGE_BYTES, &table->vram);
	if (error != 0) {
		kern_free(table->below);
		kern_free(table);
		return ENOSPC;
	}

	/* No entry yet. */
	cpu = amdrdna_vram_cpu(device, table->vram);
	for (index = 0U; index < SPACE_TABLE_ENTRIES; index++)
		kern_mmio_write64(cpu + (size_t)index * SPACE_ENTRY_BYTES, 0U);

	/* Succeeded: the table is the caller's. */
	*result = table;
	return 0;
}

/* Frees a page table and every table below it, with their VRAM. */
static void
space_table_free(
	struct amdrdna_device *device,
	struct amdrdna_table *table,
	unsigned level)
{
	unsigned index;

	/* The tables below a directory first. */
	if (table->below != NULL) {
		for (index = 0U; index < SPACE_TABLE_ENTRIES; index++) {
			if (table->below[index] != NULL)
				space_table_free(device, table->below[index], level + 1U);
		}

		/* The list of the tables below. */
		kern_free(table->below);
	}

	/* The table's VRAM and record. */
	amdrdna_vram_give(device, true, table->vram, SPACE_PAGE_BYTES);
	kern_free(table);
}

/* Writes one entry of a page table through the CPU's window. */
static void
space_table_write(
	struct amdrdna_device *device,
	struct amdrdna_table *table,
	unsigned index,
	uint64_t entry)
{
	volatile uint8_t *cpu;

	/* The entry's place in the table's page. */
	cpu = amdrdna_vram_cpu(device, table->vram);
	kern_mmio_write64(cpu + (size_t)index * SPACE_ENTRY_BYTES, entry);
}

/*
 * Finds the leaf table that maps an address of a space, making the
 * missing directories and the leaf on the way.  The caller holds the
 * space's lock.  Returns 0, or space_table_create()'s error.
 */
static int
space_leaf(
	struct amdrdna_space *space,
	uint64_t address,
	struct amdrdna_table **leaf)
{
	struct amdrdna_table *table;
	struct amdrdna_table *below;
	unsigned index[AMDRDNA_VM_LEVELS];
	unsigned level;
	uint64_t entry;
	int error;

	/* The address's index at each level. */
	amdrdna_vm_split(address, index);

	/* Walks down from the root, making each missing table. */
	table = space->root;
	for (level = 0U; level < SPACE_LEAF_LEVEL; level++) {
		below = table->below[index[level]];
		if (below == NULL) {
			/* The missing table, and the directory's entry pointing at it. */
			error = space_table_create(space->device, level + 1U, &below);
			if (error != 0)
				return error;
			table->below[index[level]] = below;
			table->count++;
			entry = space_table_word(space->device, below->vram);
			space_table_write(space->device, table, index[level], entry);
		}

		/* One level down. */
		table = below;
	}

	/* Succeeded: the leaf table. */
	*leaf = table;
	return 0;
}

/*
 * Clears the page entries of a run of a space's addresses (whole pages);
 * the tables stay.  The caller holds the space's lock.
 */
static void
space_clear(
	struct amdrdna_space *space,
	uint64_t address,
	uint64_t bytes)
{
	struct amdrdna_table *table;
	unsigned index[AMDRDNA_VM_LEVELS];
	unsigned level;
	uint64_t page;

	/* Each page of the run. */
	for (page = 0U; page < bytes; page += SPACE_PAGE_BYTES) {
		/* Walks down to the page's leaf; a missing table maps nothing. */
		amdrdna_vm_split(address + page, index);
		table = space->root;
		for (level = 0U; level < SPACE_LEAF_LEVEL; level++) {
			if (table == NULL)
				break;
			table = table->below[index[level]];
		}

		/* No leaf: nothing to clear. */
		if (table == NULL)
			continue;

		/* The entry cleared. */
		space_table_write(space->device, table, index[SPACE_LEAF_LEVEL], 0U);
		if (table->count != 0U)
			table->count--;
	}
}

/* Moves a space's table generation on, so its next run drops the context's translations. */
static void
space_changed(
	struct amdrdna_space *space)
{
	unsigned long irq;

	/* The generation the context pool compares with the context's last drop. */
	irq = spin_lock_irqsave(&space->device->spaces.lock);

	space->generation++;

	spin_unlock_irqrestore(&space->device->spaces.lock, irq);
}

/* Loses a space whose run did not end: it runs nothing more, and its session is told. */
static void
space_lose(
	struct amdrdna_space *space,
	int error)
{
	struct amdrdna_device *device;
	unsigned long irq;
	void *session;
	bool first;

	/* Marks it lost, once. */
	device = space->device;
	first = false;
	session = NULL;
	irq = spin_lock_irqsave(&device->spaces.lock);

	if (!space->lost) {
		space->lost = true;
		space->lost_error = error;
		device->spaces.lost++;
		session = space->session;
		first = true;
	}

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* Says so the first time, and tells the session. */
	if (!first)
		return;
	amdrdna_mark("space %u lost: its run did not end (%d)", (unsigned)space->id, error);
	amdrdna_spaces_mark_status(device);
	amdrdna_spaces_mark_waves(device);
	if (session != NULL)
		amdrdna_node_session_lost(device, session, EIO);
}

/*
 * Takes a run that did not end in time: unless its fence came meanwhile
 * (it ended late, or another waiter's reset dropped it), the space is lost
 * and the run's queue reset, once, with no run submitted meanwhile.  The
 * reset drops every run queued on the ring; the fence then passes them
 * all, and the values from just after the fence then to the newest run
 * are kept, so their waiters know their runs did no work.
 */
static void
space_hang(
	struct amdrdna_space *space,
	const struct amdrdna_space_ticket *ticket)
{
	struct amdrdna_device *device;
	struct amdrdna_ring *ring;
	unsigned long irq;
	uint32_t fence;
	uint32_t newest;

	/* The ring the run is on. */
	device = space->device;
	ring = space_ring(device, ticket->tracked);
	if (ring == NULL)
		return;

	/* One reset at a time, and no run put on a ring meanwhile. */
	mutex_lock(&device->spaces.submit_lock);

	/* A run that passed meanwhile needs no reset (it ended late, or another waiter's reset dropped it). */
	fence = amdrdna_ring_fence(device, ring);
	if ((int32_t)(fence - ticket->sequence) >= 0) {
		mutex_unlock(&device->spaces.submit_lock);
		amdrdna_mark("space %u: run %u on ring %u passed after its time (fence %u)", (unsigned)space->id, (unsigned)ticket->sequence,
		    ticket->tracked, (unsigned)fence);
		return;
	}

	/* The runs the reset drops: after the fence up to the newest. */
	newest = amdrdna_ring_sequence(ring);
	irq = spin_lock_irqsave(&device->spaces.lock);

	device->spaces.dropped_after[ticket->tracked] = fence;
	device->spaces.dropped_last[ticket->tracked] = newest;
	device->spaces.resets_of[ticket->tracked]++;

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* The space is lost, and its queue reset so the other sessions go on. */
	space_lose(space, ETIMEDOUT);
	space_reset(device, ticket->tracked, ticket->context);

	/* What still works after the reset: the units' status and the waves left. */
	amdrdna_spaces_mark_status(device);
	amdrdna_spaces_mark_waves(device);

	mutex_unlock(&device->spaces.submit_lock);
}

/*
 * Reports whether the last reset of a ticket's ring dropped its run: the
 * ring was reset after the run was put on it, and its value lies after
 * the fence at the reset, up to the newest run then.  The caller holds
 * the spaces' lock.
 */
static bool
space_dropped_locked(
	struct amdrdna_device *device,
	const struct amdrdna_space_ticket *ticket)
{
	uint32_t after;
	uint32_t last;

	/* A ring not reset since the run was put on it dropped nothing of it. */
	if (device->spaces.resets_of[ticket->tracked] == ticket->resets)
		return false;

	/* A run the fence had passed at the reset had ended. */
	after = device->spaces.dropped_after[ticket->tracked];
	if ((int32_t)(ticket->sequence - after) <= 0)
		return false;

	/* A run put on the ring after the reset was not dropped by it. */
	last = device->spaces.dropped_last[ticket->tracked];
	if ((int32_t)(ticket->sequence - last) > 0)
		return false;

	/* Succeeded: the reset dropped the run. */
	return true;
}

/*
 * Resets the tracked ring's queue (the graphics queue or a compute queue)
 * after a run of a context did not end, and says how long it took; a
 * queue that cannot be reset fails the device (every session's work then
 * fails).
 */
static void
space_reset(
	struct amdrdna_device *device,
	unsigned tracked,
	unsigned context)
{
	const char *queue;
	uint64_t started;
	uint64_t taken;
	int error;

	/* The reset of the ring's queue, timed. */
	started = clock_milliseconds(NULL);
	if (tracked == AMDRDNA_TRACKED_GRAPHICS) {
		queue = "graphics";
		error = amdrdna_cp_graphics_reset(device, context);
	} else {
		queue = "compute";
		error = amdrdna_cp_compute_reset(device, tracked - AMDRDNA_TRACKED_COMPUTE, context);
	}
	taken = clock_milliseconds(NULL) - started;

	/* Says how it went; a failed one ends the device's work. */
	if (error != 0) {
		amdrdna_mark("%s queue reset for context %u failed (%d) after %llums; the device takes no more work", queue, context, error,
		    (unsigned long long)taken);
		amdrdna_node_device_failed(device, EIO);
		return;
	}

	/* Counted, and said. */
	device->spaces.resets++;
	amdrdna_mark("%s queue reset for context %u in %llums, the queue runs again", queue, context, (unsigned long long)taken);
}

/*
 * The word that points a hub context, or a directory entry, at a table:
 * its physical VRAM address, present.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c (amdgpu_gmc_pd_addr,
 * amdgpu_gmc_get_pde_for_bo) and gmc_v10_0.c (gmc_v10_0_get_vm_pde: a
 * VRAM table's address is its offset plus the frame buffer's physical
 * offset).
 */
static uint64_t
space_table_word(
	struct amdrdna_device *device,
	uint64_t vram)
{
	uint64_t word;

	/* The table's physical address with the present bit. */
	word = amdrdna_vm_table_entry(vram + device->frame_buffer_physical, false);

	/* Succeeded: the word. */
	return word;
}

/*
 * Reports whether no work of a space can still run; the caller holds the
 * spaces' lock.  A space whose context changed hands ran nothing more in
 * it: the context was handed on only once its work had ended.
 */
static bool
space_quiet_locked(
	struct amdrdna_space *space)
{
	uint32_t passed[AMDRDNA_TRACKED_RINGS];
	struct amdrdna_device *device;
	bool idle;

	/* A space the video engine may still run in is never quiet. */
	device = space->device;
	if (space->video_held)
		return false;

	/* A space that never held a context ran nothing. */
	if (space->context == 0U)
		return true;

	/* A context handed on carries nothing of the space's. */
	if (device->spaces.pool.slots[space->context].owner != space->id)
		return true;

	/* Every ring passed the context's work. */
	space_passed(device, passed);
	idle = amdrdna_context_idle(&device->spaces.pool, space->context, passed);
	if (!idle)
		return false;

	/* Succeeded: nothing of the space runs. */
	return true;
}

/* Reads one instance of a named register of the graphics hub. */
static uint32_t
space_hub_read(
	struct amdrdna_device *device,
	enum amdrdna_hub_register which,
	unsigned instance)
{
	uint32_t index;
	uint32_t value;

	/* The register's index, then its value. */
	index = amdrdna_hub_register(device, AMDRDNA_HUB_GRAPHICS, which, instance);
	value = amdrdna_read(device, index);

	/* Succeeded: the value. */
	return value;
}

/* Reads one register of a wave of the selected array through the shader sequencer's window. */
static uint32_t
space_wave_read(
	struct amdrdna_device *device,
	uint32_t wave,
	uint32_t index)
{
	uint32_t word;
	uint32_t value;

	/* The wave and the register, then the data. */
	word = wave | (index << AMDRDNA_GC_WAVE_REGISTER_SHIFT);
	amdrdna_block_write(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_WAVE_INDEX, word);
	value = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_WAVE_DATA);

	/* The register's value. */
	return value;
}
