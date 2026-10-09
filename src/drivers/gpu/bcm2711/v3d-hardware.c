/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* A single worker owns MMU/cache commands; IRQs only acknowledge and latch. */
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/buffer.h"

/* Owned interrupt sources of the fixed, single-core 4.2 engine. */
#define ENGINE_CORE_EVENTS 0xa7U
#define ENGINE_HUB_EVENTS 0x3aU
#define ENGINE_CORE_FAULT 0x20U
#define ENGINE_HUB_FAULTS 0x38U

/* Page-table and scratch storage deliberately stay in low, native CPU RAM. */
#define ENGINE_LOW_LIMIT 0x3fffffffULL
#define ENGINE_TABLE_BYTES (BCM2711_V3D_PAGE_ENTRIES * 4U)

/* Bounds every cache and MMU wait without borrowing an IRQ spin guard. */
#define ENGINE_WAIT_STEP_US 10U
#define ENGINE_WAIT_STEPS 10000U

static int identify(struct bcm2711_v3d *engine);
static int allocate_tables(struct bcm2711_v3d *engine);
static int bind_tables(struct bcm2711_v3d *engine);
static int flush_translation(struct bcm2711_v3d *engine);
static int wait_clear(volatile uint8_t *window, uint32_t offset, uint32_t mask);
static int worker_admit(struct bcm2711_v3d *engine);
static void record_failure(struct bcm2711_v3d *engine, int error);
static void prepare_sources(struct bcm2711_v3d *engine);
static int open_sources(struct bcm2711_v3d *engine);
static bool service_sources(void *owner);

/*
 * Starts the MMU and serviced interrupts after native power is established.
 * The persistent engine owns allocations and IRQ arguments for kernel lifetime.
 * Any refusal after MMU publication retains the table and scratch references.
 */
int
bcm2711_v3d_hardware_start(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	bool allowed;
	int error;

	/* No engine register is touched without the provider's completed startup. */
	hardware = &engine->hardware;
	if (!engine->present || !engine->power.ready)
		return ENODEV;
	if (hardware->initialized)
		return EBUSY;
	if (engine->hub.size < 0x123cU || engine->core.size < 0x940U)
		return ENOTSUP;
	if (!engine->irq.registered)
		return ENODEV;

	/* Establishes the guard before publishing any IRQ-side argument. */
	spin_init(&hardware->guard, LOCK_RANK_DEVICE, "bcm2711-v3d");
	hardware->initialized = true;
	engine->irq.owner = engine;
	engine->irq.service = service_sources;

	/* Identification is the first native read of the powered render engine. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V3");
	if (!allowed)
		return ECANCELED;
	error = identify(engine);
	if (error != 0)
		return error;

	/* Only 4.2's L2T range exists here; older L2C/GCA registers are omitted. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V4");
	if (!allowed)
		return ECANCELED;
	kern_mmio_write32(engine->core.mapped + 0x34, 0);
	kern_mmio_write32(engine->core.mapped + 0x38, UINT32_MAX);
	prepare_sources(engine);
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V4 cache range and IRQ masks ready");

	/* Creates and cleans both owners before permitting a hardware table fetch. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V5");
	if (!allowed)
		return ECANCELED;
	error = allocate_tables(engine);
	if (error != 0)
		return error;
	error = bind_tables(engine);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* All callbacks, source masks and MMU translations exist before GIC delivery. */
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V5 MMU table %llx scratch %llx",
		(unsigned long long)hardware->pages->memory.paddr,
		(unsigned long long)hardware->scratch->memory.paddr);
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V6");
	if (!allowed)
		return ECANCELED;
	error = open_sources(engine);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* Publishes worker admission only after serviced source masks were confirmed. */
	hardware->ready = true;
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V6 core/hub IRQ ready");

	/* Succeeded: the worker can now map resources and publish its first job. */
	return 0;
}

/*
 * Quiets and joins IRQ services before the worker changes power or MMU state.
 * Owners remain allocated, including when a failed ASB stop cannot prove idle.
 */
void
bcm2711_v3d_hardware_mask(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	unsigned long enabled;

	/* Stops delivery before joining any handler that already obtained the guard. */
	hardware = &engine->hardware;
	if (!hardware->initialized)
		return;
	kern_irq_mask(engine->irq.irq);
	enabled = spin_lock_irqsave(&hardware->guard);

	/* A failed power reset leaves native register accessibility unproved. */
	if (engine->power.ready) {
		kern_mmio_write32(engine->core.mapped + 0x60, UINT32_MAX);
		kern_mmio_write32(engine->hub.mapped + 0x60, UINT32_MAX);
		kern_mmio_write32(engine->core.mapped + 0x58, ENGINE_CORE_EVENTS);
		kern_mmio_write32(engine->hub.mapped + 0x58, ENGINE_HUB_EVENTS);
	}

	/* Waiting handlers observe the closed admission before touching engine IO. */
	hardware->irq_live = false;

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Submission remains closed throughout the worker's subsequent reset. */
	hardware->ready = false;
}

/*
 * Resets native power, restores translation state and reopens serviced IRQs.
 * The serialized worker stops submissions before calling; success alone allows
 * it to retire old DMA references.  Failed reset keeps all references retained.
 */
int
bcm2711_v3d_hardware_reset(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	unsigned long enabled;
	bool busy;
	uint32_t old_hub[4];
	uint32_t old_core[3];
	uint32_t index;
	int error;

	/* Requires the original MMU allocations, never a replacement under active DMA. */
	hardware = &engine->hardware;
	if (!hardware->initialized || !hardware->mmu_published)
		return ENODEV;
	if (hardware->resets == UINT64_MAX)
		return EOVERFLOW;
	enabled = spin_lock_irqsave(&hardware->guard);

	busy = hardware->job_busy;

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Global recovery cannot race a worker that still owns a native job call. */
	if (busy)
		return EBUSY;

	/* Recovery must return the same native engine, not a newly assumed register layout. */
	kern_memcpy(old_hub, hardware->hub_ident, sizeof(old_hub));
	kern_memcpy(old_core, hardware->core_ident, sizeof(old_core));

	/* Joins every native service before the provider can close the engine domain. */
	bcm2711_v3d_hardware_mask(engine);
	error = bcm2711_v3d_power_reset(engine);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* Reads the now-powered identifiers again before restoring translation storage. */
	error = identify(engine);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* A changed hub identity invalidates the old worker's hardware assumptions. */
	for (index = 0; index < 4; index++) {
		if (old_hub[index] != hardware->hub_ident[index]) {
			record_failure(engine, ENODEV);
			return ENODEV;
		}
	}

	/* The sole core must likewise match the actual identifiers captured at first startup. */
	for (index = 0; index < 3; index++) {
		if (old_core[index] != hardware->core_ident[index]) {
			record_failure(engine, ENODEV);
			return ENODEV;
		}
	}

	/* Restores the entire cache range before publishing the retained page table. */
	prepare_sources(engine);
	kern_mmio_write32(engine->core.mapped + 0x34, 0);
	kern_mmio_write32(engine->core.mapped + 0x38, UINT32_MAX);
	error = bind_tables(engine);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* Old events cannot complete jobs admitted under the next reset generation. */
	enabled = spin_lock_irqsave(&hardware->guard);

	kern_memset(&hardware->events, 0, sizeof(hardware->events));
	hardware->faulted = false;

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Only completed provider and MMU restoration permits fresh submission. */
	hardware->resets++;
	error = open_sources(engine);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* Reset restoration has established the retained mappings and serviced IRQ sources. */
	hardware->ready = true;

	/* Succeeded: the old execution stopped and the same mappings are usable again. */
	return 0;
}

/*
 * Cleans changed PTEs and invalidates both levels of cached translation.
 * The worker serializes edits against every job and retains unmapped buffers
 * until success or a subsequently proved native reset.
 */
int
bcm2711_v3d_hardware_pages_sync(
	struct bcm2711_v3d *engine)
{
	int error;

	/* Faulted or reset domains cannot safely execute a translation command. */
	error = worker_admit(engine);
	if (error != 0)
		return error;

	/* The DMA table uses a cached CPU view, so every changed entry reaches RAM first. */
	kern_dcache_clean_range(engine->hardware.pages->address, ENGINE_TABLE_BYTES);
	kern_io_write_barrier();
	error = flush_translation(engine);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* A concurrent IRQ fault cannot make successful translation polling imply retirement. */
	error = worker_admit(engine);
	if (error != 0)
		return error;

	/* Succeeded: stale translations can no longer borrow a removed physical page. */
	return 0;
}

/*
 * Invalidates L2T then slice caches before bin, render or compute execution.
 * The worker also serializes cache clean; hardware stalls L2T access during
 * this invalidate, so a separate completion wait is not required.
 */
int
bcm2711_v3d_hardware_invalidate(
	struct bcm2711_v3d *engine)
{
	int error;

	/* Only an initialized, non-faulted engine accepts worker commands. */
	error = worker_admit(engine);
	if (error != 0)
		return error;

	/* Invalidating outer storage first prevents inner caches from refilling stale data. */
	kern_mmio_write32(engine->core.mapped + 0x30, 1);
	kern_mmio_write32(engine->core.mapped + 0x24, 0x0f0f0f0f);
	kern_io_write_barrier();

	/* Reports any fault delivered during this worker command before admitting a job. */
	error = worker_admit(engine);
	if (error != 0)
		return error;

	/* Succeeded: later command fetches observe RAM cleaned by their resource owners. */
	return 0;
}

/*
 * Drains TMU write combiners and cleans L2T before reporting output completion.
 * Each wait is bounded and occurs outside the IRQ guard.
 */
int
bcm2711_v3d_hardware_clean(
	struct bcm2711_v3d *engine)
{
	int error;

	/* The worker never overlaps this cache operation with invalidate or reset. */
	error = worker_admit(engine);
	if (error != 0)
		return error;

	/* Write combiners must finish before their resulting L2T lines are cleaned. */
	kern_mmio_write32(engine->core.mapped + 0x30, 0x100);
	error = wait_clear(engine->core.mapped, 0x30, 0x100);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* Cleans the full initialized L2T range and waits for actual drain completion. */
	kern_mmio_write32(engine->core.mapped + 0x30, 5);
	error = wait_clear(engine->core.mapped, 0x30, 1);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* Orders later CPU/output observation after completed native cache maintenance. */
	kern_io_read_barrier();

	/* A simultaneous native fault takes precedence over a cleared clean-command bit. */
	error = worker_admit(engine);
	if (error != 0)
		return error;

	/* Succeeded: GPU cache contents no longer hide completed TMU stores from RAM. */
	return 0;
}

/*
 * Consumes IRQ event latches while preserving the sticky engine fault state.
 * The worker handles fault recovery before any completion bits in this snapshot.
 */
void
bcm2711_v3d_hardware_events(
	struct bcm2711_v3d *engine,
	struct bcm2711_v3d_events *events)
{
	struct bcm2711_v3d_hardware *hardware;
	unsigned long enabled;

	/* Initialization refusal supplies no native events and no IRQ guard to acquire. */
	hardware = &engine->hardware;
	kern_memset(events, 0, sizeof(*events));
	if (!hardware->initialized)
		return;

	/* Captures and consumes both banks together, including a simultaneous fault. */
	enabled = spin_lock_irqsave(&hardware->guard);

	*events = hardware->events;
	hardware->events.core = 0;
	hardware->events.hub = 0;

	spin_unlock_irqrestore(&hardware->guard, enabled);
}

/* Records actual identification and rejects unsupported layouts before MMU publication. */
static int
identify(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	uint32_t index;
	uint32_t layout;
	uint32_t cores;
	uint32_t widths;
	uint32_t version;

	/* Reads only identification from the already powered domain. */
	hardware = &engine->hardware;
	for (index = 0; index < 4; index++)
		hardware->hub_ident[index] = kern_mmio_read32(engine->hub.mapped + 8U + index * 4U);

	/* Captures all core identifiers for the user's later physical comparison. */
	for (index = 0; index < 3; index++)
		hardware->core_ident[index] = kern_mmio_read32(engine->core.mapped + index * 4U);

	/* The native layout, not a simulator fixture's entire identifier, selects 4.2. */
	layout = hardware->hub_ident[1];
	version = (layout & 15U) * 10U + ((layout >> 4) & 15U);
	cores = (layout >> 8) & 15U;
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V3 hub %08x %08x %08x %08x",
		hardware->hub_ident[0],
		hardware->hub_ident[1],
		hardware->hub_ident[2],
		hardware->hub_ident[3]);
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V3 core %08x %08x %08x",
		hardware->core_ident[0],
		hardware->core_ident[1],
		hardware->core_ident[2]);
	if (version != 42U || cores != 1U)
		return ENOTSUP;
	if ((hardware->hub_ident[2] & 0x100U) == 0)
		return ENOTSUP;
	if ((layout & 0x20000U) == 0 || (layout & 0x10000U) != 0)
		return ENOTSUP;
	if ((hardware->core_ident[0] & 0xffffffU) != 0x443356U)
		return ENOTSUP;

	/* PT PFNs encode at most 36 physical bits and this owner reserves 32-bit VAs. */
	widths = kern_mmio_read32(engine->hub.mapped + 0x1238);
	hardware->physical_bits = 30U + ((widths >> 8) & 15U);
	hardware->virtual_bits = 30U + ((widths >> 4) & 15U);
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V3 version %u cores %u PA %u VA %u",
		version,
		cores,
		hardware->physical_bits,
		hardware->virtual_bits);
	if (hardware->physical_bits < 30U || hardware->physical_bits > 36U)
		return ENOTSUP;
	if (hardware->virtual_bits != 32U)
		return ENOTSUP;

	/* Succeeded: register layout and address widths match the private hardware owner. */
	return 0;
}

/* Acquires zeroed table and fault scratch owners before any hardware can fetch them. */
static int
allocate_tables(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	int error;

	/* Allocates the complete 4 MiB level, including an unmapped virtual page zero. */
	hardware = &engine->hardware;
	error = bcm2711_buffer_create(
		ENGINE_TABLE_BYTES,
		ENGINE_LOW_LIMIT,
		BCM2711_V3D_PAGE_BYTES,
		&hardware->pages);
	if (error != 0)
		return error;

	/* Missing scratch storage prevents publication and permits safe table unwind. */
	error = bcm2711_buffer_create(
		BCM2711_V3D_PAGE_BYTES,
		ENGINE_LOW_LIMIT,
		BCM2711_V3D_PAGE_BYTES,
		&hardware->scratch);
	if (error != 0) {
		bcm2711_buffer_release(hardware->pages);
		hardware->pages = NULL;
		return error;
	}

	/* Both allocations belong to the persistent engine and are still unexposed. */
	return 0;
}

/* Installs native PFNs and enables bounded page-entry cache/TLB maintenance. */
static int
bind_tables(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	uint32_t table_page;
	uint32_t scratch_page;
	uint32_t observed;
	int error;

	/* Publishes only RAM initialized by the CPU, before any MMU enable write. */
	hardware = &engine->hardware;
	kern_dcache_clean_range(hardware->pages->address, ENGINE_TABLE_BYTES);
	kern_dcache_clean_range(hardware->scratch->address, BCM2711_V3D_PAGE_BYTES);
	kern_io_write_barrier();
	table_page = (uint32_t)(hardware->pages->memory.paddr >> 12);
	scratch_page = (uint32_t)(hardware->scratch->memory.paddr >> 12);

	/* References become permanent before the first register can authorize fetching. */
	hardware->mmu_published = true;
	kern_mmio_write32(engine->hub.mapped + 0x1204, table_page);
	observed = kern_mmio_read32(engine->hub.mapped + 0x1204);
	if (observed != table_page)
		return EIO;

	/* Readback proves the native PFN before enabling this translation configuration. */
	kern_mmio_write32(engine->hub.mapped + 0x1200, 0x060d0c01);
	kern_mmio_write32(engine->hub.mapped + 0x1230, scratch_page | 0x80000000U);
	kern_mmio_write32(engine->hub.mapped + 0x1000, 1);
	error = flush_translation(engine);
	if (error != 0)
		return error;

	/* Succeeded: translation storage is visible and both hardware caches are fresh. */
	return 0;
}

/* Flushes page-entry cache before clearing cached virtual translations. */
static int
flush_translation(
	struct bcm2711_v3d *engine)
{
	uint32_t control;
	int error;

	/* A current PTE cannot be bypassed by the MMU's cached older page entry. */
	kern_mmio_write32(engine->hub.mapped + 0x1000, 3);
	error = wait_clear(engine->hub.mapped, 0x1000, 4);
	if (error != 0)
		return error;

	/* Clears TLB entries only after the page-entry cache flush completed. */
	control = kern_mmio_read32(engine->hub.mapped + 0x1200);
	kern_mmio_write32(engine->hub.mapped + 0x1200, control | 4U);
	error = wait_clear(engine->hub.mapped, 0x1200, 0x80);
	if (error != 0)
		return error;

	/* Succeeded: no earlier mapping remains in either translation cache. */
	return 0;
}

/* Polls a self-clearing command outside spin guards with a finite 100 ms bound. */
static int
wait_clear(
	volatile uint8_t *window,
	uint32_t offset,
	uint32_t mask)
{
	uint32_t attempt;
	uint32_t status;

	/* Allows immediate completion, then one final sample at the requested bound. */
	for (attempt = 0; attempt <= ENGINE_WAIT_STEPS; attempt++) {
		status = kern_mmio_read32(window + offset);
		if ((status & mask) == 0)
			return 0;
		if (attempt == ENGINE_WAIT_STEPS)
			break;
		kern_usleep_range(ENGINE_WAIT_STEP_US, ENGINE_WAIT_STEP_US);
	}

	/* A timeout supplies no proof that an affected DMA reference can be retired. */
	return ETIMEDOUT;
}

/* Checks sticky IRQ faults before any serialized worker command. */
static int
worker_admit(
	struct bcm2711_v3d *engine)
{
	unsigned long enabled;
	bool faulted;

	/* Reset and incomplete boot attempts close submission before native access. */
	if (!engine->hardware.ready || !engine->power.ready)
		return ENODEV;
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	faulted = engine->hardware.faulted;

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* IRQ faults remain visible even after the worker consumed their event bits. */
	if (faulted)
		return EIO;

	/* Succeeded: the sole worker owns a powered, initialized execution domain. */
	return 0;
}

/* Turns an incomplete hardware operation into a sticky fault with retained owners. */
static void
record_failure(
	struct bcm2711_v3d *engine,
	int error)
{
	unsigned long enabled;

	/* No later IRQ may disguise a failed cache/MMU operation as ordinary completion. */
	bcm2711_v3d_hardware_mask(engine);
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	engine->hardware.faulted = true;
	engine->hardware.events.error = error;

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);
}

/* Clears stale owned interrupts while keeping both source banks fully masked. */
static void
prepare_sources(
	struct bcm2711_v3d *engine)
{
	/* Source masking precedes acknowledgement and any later worker publication. */
	kern_mmio_write32(engine->core.mapped + 0x60, UINT32_MAX);
	kern_mmio_write32(engine->hub.mapped + 0x60, UINT32_MAX);
	kern_mmio_write32(engine->core.mapped + 0x58, ENGINE_CORE_EVENTS);
	kern_mmio_write32(engine->hub.mapped + 0x58, ENGINE_HUB_EVENTS);
}

/* Opens the persistent callback before unmasking device sources and GIC delivery. */
static int
open_sources(
	struct bcm2711_v3d *engine)
{
	unsigned long enabled;
	uint32_t core_mask;
	uint32_t hub_mask;

	/* Publishes IRQ admission and exact owned masks atomically against service. */
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	engine->hardware.irq_live = true;
	kern_mmio_write32(engine->core.mapped + 0x60, ~ENGINE_CORE_EVENTS);
	kern_mmio_write32(engine->hub.mapped + 0x60, ~ENGINE_HUB_EVENTS);
	kern_mmio_write32(engine->core.mapped + 0x64, ENGINE_CORE_EVENTS);
	kern_mmio_write32(engine->hub.mapped + 0x64, ENGINE_HUB_EVENTS);
	core_mask = kern_mmio_read32(engine->core.mapped + 0x5c);
	hub_mask = kern_mmio_read32(engine->hub.mapped + 0x5c);
	if ((core_mask & ENGINE_CORE_EVENTS) != 0 || (hub_mask & ENGINE_HUB_EVENTS) != 0) {
		engine->hardware.irq_live = false;
		kern_mmio_write32(engine->core.mapped + 0x60, UINT32_MAX);
		kern_mmio_write32(engine->hub.mapped + 0x60, UINT32_MAX);
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		return EIO;
	}

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* Device sources now have an owner before the GIC can deliver them. */
	kern_irq_unmask(engine->irq.irq);

	/* Succeeded: owned device sources are open and the GIC can deliver to their callback. */
	return 0;
}

/* Quiets both shared IRQ banks and captures faults without allocating or waiting. */
static bool
service_sources(
	void *owner)
{
	struct bcm2711_v3d *engine;
	struct bcm2711_v3d_hardware *hardware;
	uint32_t core;
	uint32_t hub;
	uint32_t control;
	unsigned long enabled;

	/* A handler arriving during reset observes admission before any powered IO. */
	engine = owner;
	hardware = &engine->hardware;
	enabled = spin_lock_irqsave(&hardware->guard);

	if (!hardware->irq_live) {
		spin_unlock_irqrestore(&hardware->guard, enabled);
		return false;
	}

	/* The hub can hold the shared level asserted even with an owned core event. */
	core = kern_mmio_read32(
		engine->core.mapped + 0x50) & ENGINE_CORE_EVENTS;
	hub = kern_mmio_read32(engine->hub.mapped + 0x50) & ENGINE_HUB_EVENTS;
	if (core == 0 && hub == 0) {
		spin_unlock_irqrestore(&hardware->guard, enabled);
		return false;
	}

	/* Acknowledges both banks before the generic IRQ handler retires the GIC. */
	kern_mmio_write32(engine->core.mapped + 0x58, core);
	kern_mmio_write32(engine->hub.mapped + 0x58, hub);
	hardware->events.core |= core;
	hardware->events.hub |= hub;

	/* Faults take precedence over simultaneous queue completion in the worker. */
	if ((core & ENGINE_CORE_FAULT) != 0 || (hub & ENGINE_HUB_FAULTS) != 0) {
		hardware->faulted = true;
		hardware->events.error = EIO;
		if ((hub & ENGINE_HUB_FAULTS) != 0) {
			hardware->events.fault_client = kern_mmio_read32(engine->hub.mapped + 0x122c);
			hardware->events.fault_address = kern_mmio_read32(engine->hub.mapped + 0x1234);
			control = kern_mmio_read32(engine->hub.mapped + 0x1200);
			kern_mmio_write32(engine->hub.mapped + 0x1200, control);
		}

		/* Quarantine remains sticky until the worker proves a complete native reset. */
		kern_mmio_write32(engine->core.mapped + 0x60, UINT32_MAX);
		kern_mmio_write32(engine->hub.mapped + 0x60, UINT32_MAX);
		hardware->irq_live = false;
	}

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Succeeded: every owned source was quieted without pretending DMA was retired. */
	return true;
}
