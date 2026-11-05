/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The V3D 7.1 engine's identification (stage V2), page table (V3) and
 * interrupts (V4), and the cache, MMU and reset services the job runner
 * uses.
 *
 * The functions keep the names and contracts of the Pi 4's v3d-hardware.c,
 * which a Pi 5 kernel does not build: a single worker owns MMU and cache
 * commands, and interrupts only acknowledge and latch.  What differs on 7.1
 * is the identification, the two interrupt lines (the hub's and the
 * core's), the interrupt bits, and the reset, which goes through the power
 * sequencer before the power manager.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/v3d/v3d_regs.h (hub
 * identification 0x008-0x014, MMU 0x1000-0x1238, interrupts 0x050-0x064,
 * L2T 0x024-0x038), v3d_mmu.c lines 25-80, v3d_gem.c lines 20-37 and
 * 124-150, v3d_irq.c lines 24-34 and 249-307, v3d_drv.c lines 375-395.
 */

#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/v3d71-private.h"
#include "drivers/platform/rpi5/rpi5-serror.h"

/* Page-table and scratch storage stay in low CPU RAM, inside the 36 bits a PTE names. */
#define ENGINE_LOW_LIMIT 0x3fffffffULL
#define ENGINE_TABLE_BYTES (BCM2711_V3D_PAGE_ENTRIES * 4U)

/* The physical address bits a 24-bit page number of 4 KiB pages names. */
#define ENGINE_TABLE_PHYSICAL_BITS 36U

/* A short register-only retry avoids a scheduler tick for ordinary MMU completion. */
#define ENGINE_FAST_READS 1024U
#define ENGINE_WAIT_STEP_US 10U
#define ENGINE_WAIT_STEPS 10000U

/* The longest sleep for an engine event before the caller looks again, in milliseconds. */
#define ENGINE_EVENT_WAIT_MS 1U

/* The hub registers this file uses. */
#define HUB_IDENT0 0x008U
#define HUB_INT_STS 0x050U
#define HUB_INT_CLR 0x058U
#define HUB_INT_MSK_STS 0x05cU
#define HUB_INT_MSK_SET 0x060U
#define HUB_INT_MSK_CLR 0x064U
#define HUB_MMUC_CONTROL 0x1000U
#define HUB_MMU_CTL 0x1200U
#define HUB_MMU_PT_PA_BASE 0x1204U
#define HUB_MMU_VIO_ID 0x122cU
#define HUB_MMU_ILLEGAL_ADDR 0x1230U
#define HUB_MMU_VIO_ADDR 0x1234U
#define HUB_MMU_DEBUG_INFO 0x1238U

/* The core registers this file uses. */
#define CORE_SLCACTL 0x024U
#define CORE_L2TCACTL 0x030U
#define CORE_L2TFLSTA 0x034U
#define CORE_L2TFLEND 0x038U
#define CORE_INT_STS 0x050U
#define CORE_INT_CLR 0x058U
#define CORE_INT_MSK_STS 0x05cU
#define CORE_INT_MSK_SET 0x060U
#define CORE_INT_MSK_CLR 0x064U

/*
 * The MMU's configuration: translation on, and both an abort and an
 * interrupt for an invalid entry, a write to a read-only page and an
 * address past the capability.  The same word as the Pi 4's.
 */
#define MMU_CONFIGURATION 0x060d0c01U

/*
 * Whether the shader state record is the draw-index form: set by
 * identification from the revision (10 and later) before any job runs, and
 * never changed by a reset, which keeps the same identification.
 */
bool bcm2712_v3d_draw_index_record;

static int identify(struct bcm2712_v3d *v3d);
static int allocate_tables(struct bcm2711_v3d *engine);
static int bind_tables(struct bcm2711_v3d *engine);
static int flush_translation(struct bcm2711_v3d *engine);
static int wait_clear(struct bcm2711_v3d *engine, volatile uint8_t *window, uint32_t offset, uint32_t mask);
static int worker_admit(struct bcm2711_v3d *engine);
static void record_failure(struct bcm2711_v3d *engine, int error);
static void prepare_sources(struct bcm2711_v3d *engine);
static int open_sources(struct bcm2712_v3d *v3d);
static bool core_service(void *owner);

/*
 * Starts the MMU and serviced interrupts after the engine is powered.
 *
 * Runs stages V2 (identification), V3 (page table and MMU) and V4
 * (interrupts).  The persistent engine owns allocations and IRQ arguments
 * for the kernel's lifetime.  Any refusal after MMU publication retains the
 * table and scratch references.
 */
int
bcm2711_v3d_hardware_start(
	struct bcm2711_v3d *engine)
{
	struct bcm2712_v3d *v3d;
	struct bcm2711_v3d_hardware *hardware;
	bool allowed;
	int error;

	/* No engine register is touched before the power-up finished. */
	v3d = bcm2712_v3d_of(engine);
	hardware = &engine->hardware;
	if (!engine->present || !engine->power.ready)
		return ENODEV;
	if (hardware->initialized)
		return EBUSY;
	if (!engine->irq.registered || !v3d->hub_irq.registered)
		return ENODEV;

	/* Establishes the guard before publishing any IRQ-side argument. */
	spin_init(&hardware->guard, LOCK_RANK_DEVICE, "bcm2712-v3d");
	waitq_init(&hardware->events_queue, "bcm2712-v3d-events");
	hardware->initialized = true;
	engine->irq.owner = engine;
	engine->irq.service = core_service;
	v3d->hub_irq.owner = engine;
	v3d->hub_irq.service = bcm2712_v3d_hub_service;

	/* Identification is the first read of the powered engine. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V2");
	if (!allowed)
		return ECANCELED;
	error = identify(v3d);
	(void)drv_rpi5_serror_check("v3d: identifying the engine");
	if (error != 0)
		return error;

	/* Sets the whole-cache L2T range and masks every source before the table goes in. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V3");
	if (!allowed)
		return ECANCELED;
	kern_mmio_write32(engine->core.mapped + CORE_L2TFLSTA, 0);
	kern_mmio_write32(engine->core.mapped + CORE_L2TFLEND, UINT32_MAX);
	prepare_sources(engine);

	/* Creates and cleans both owners before permitting a hardware table fetch. */
	error = allocate_tables(engine);
	if (error != 0)
		return error;
	error = bind_tables(engine);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V3 MMU ctl %08x table %llx scratch %llx",
		kern_mmio_read32(engine->hub.mapped + HUB_MMU_CTL),
		(unsigned long long)hardware->pages->memory.paddr,
		(unsigned long long)hardware->scratch->memory.paddr);

	/* All callbacks, source masks and MMU translations exist before GIC delivery. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V4");
	if (!allowed)
		return ECANCELED;
	error = open_sources(v3d);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* Publishes worker admission only after serviced source masks were confirmed. */
	hardware->ready = true;
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V4 IRQ hub %d core %d open, pending hub %x core %x",
		v3d->hub_irq.irq,
		engine->irq.irq,
		kern_mmio_read32(engine->hub.mapped + HUB_INT_STS),
		kern_mmio_read32(engine->core.mapped + CORE_INT_STS));

	/* Succeeded: the worker can now map resources and run its first job. */
	return 0;
}

/*
 * Reads the sequence the IRQ handlers advance after latching events.
 */
uint64_t
bcm2711_v3d_hardware_event_sequence(
	struct bcm2711_v3d *engine)
{
	unsigned long enabled;
	uint64_t sequence;

	/* The handlers advance it under the same guard. */
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	sequence = waitq_sequence(&engine->hardware.events_queue);

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* Succeeded: the caller checks the events after this observation. */
	return sequence;
}

/*
 * Sleeps until an IRQ comes after the observed sequence, or for a millisecond at most.
 */
void
bcm2711_v3d_hardware_event_wait(
	struct bcm2711_v3d *engine,
	uint64_t observed)
{
	unsigned long enabled;
	uint64_t deadline;

	/* At most a millisecond (a tick at least). */
	deadline = sched_ticks() + kern_ms_to_ticks(ENGINE_EVENT_WAIT_MS);

	/* Sleeps on the queue unless an IRQ came since the observation. */
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	(void)waitq_sleep(&engine->hardware.events_queue, &engine->hardware.guard, observed, deadline, 0U);

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);
}

/*
 * Quiets and joins IRQ services before the worker changes power or MMU state.
 */
void
bcm2711_v3d_hardware_mask(
	struct bcm2711_v3d *engine)
{
	struct bcm2712_v3d *v3d;
	struct bcm2711_v3d_hardware *hardware;
	unsigned long enabled;

	/* Stops delivery on both lines before joining any handler that already obtained the guard. */
	v3d = bcm2712_v3d_of(engine);
	hardware = &engine->hardware;
	if (!hardware->initialized)
		return;
	kern_irq_mask(engine->irq.irq);
	kern_irq_mask(v3d->hub_irq.irq);
	enabled = spin_lock_irqsave(&hardware->guard);

	/* A failed power reset leaves register accessibility unproved, so only a powered engine is written. */
	if (engine->power.ready) {
		kern_mmio_write32(engine->core.mapped + CORE_INT_MSK_SET, UINT32_MAX);
		kern_mmio_write32(engine->hub.mapped + HUB_INT_MSK_SET, UINT32_MAX);
		kern_mmio_write32(engine->core.mapped + CORE_INT_CLR, BCM2712_V3D_CORE_EVENTS);
		kern_mmio_write32(engine->hub.mapped + HUB_INT_CLR, BCM2712_V3D_HUB_EVENTS);
	}

	/* Waiting handlers observe the closed admission before touching engine IO. */
	hardware->irq_live = false;

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Submission remains closed throughout the worker's subsequent reset. */
	hardware->ready = false;
}

/*
 * Resets the engine, restores translation state and reopens serviced IRQs.
 *
 * The serialized worker stops submissions before calling; success alone
 * allows it to retire old DMA references.  A failed reset keeps all
 * references retained.
 */
int
bcm2711_v3d_hardware_reset(
	struct bcm2711_v3d *engine)
{
	struct bcm2712_v3d *v3d;
	struct bcm2711_v3d_hardware *hardware;
	unsigned long enabled;
	bool busy;
	uint32_t old_hub[4];
	uint32_t old_core[3];
	uint32_t error_status;
	uint32_t index;
	int error;

	/* Requires the original MMU allocations, never a replacement under active DMA. */
	v3d = bcm2712_v3d_of(engine);
	hardware = &engine->hardware;
	if (!hardware->initialized || !hardware->mmu_published)
		return ENODEV;
	if (hardware->resets == UINT64_MAX)
		return EOVERFLOW;
	enabled = spin_lock_irqsave(&hardware->guard);

	busy = hardware->job_busy;

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Global recovery cannot race a worker that still owns a job. */
	if (busy)
		return EBUSY;

	/* Records the engine's error status before the reset clears it. */
	error_status = 0;
	if (engine->power.ready)
		error_status = kern_mmio_read32(engine->core.mapped + 0xf20U);
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "reset: error status %08x", error_status);

	/* Recovery must return the same engine, not a newly assumed register layout. */
	kern_memcpy(old_hub, hardware->hub_ident, sizeof(old_hub));
	kern_memcpy(old_core, hardware->core_ident, sizeof(old_core));

	/* Joins every service before the sequencer and the power manager reset the engine. */
	bcm2711_v3d_hardware_mask(engine);
	error = bcm2712_v3d_power_reset(v3d);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}
	hardware->counters_ready = false;

	/* Reads the identifiers again before restoring translation storage. */
	error = identify(v3d);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* A changed hub identity invalidates the worker's hardware assumptions. */
	for (index = 0; index < 4; index++) {
		/* Refuses an engine whose hub identifies differently. */
		if (old_hub[index] != hardware->hub_ident[index]) {
			record_failure(engine, ENODEV);
			return ENODEV;
		}
	}

	/* The sole core must likewise match the identifiers captured at first startup. */
	for (index = 0; index < 3; index++) {
		/* Refuses a core that identifies differently. */
		if (old_core[index] != hardware->core_ident[index]) {
			record_failure(engine, ENODEV);
			return ENODEV;
		}
	}

	/* Restores the whole cache range before publishing the retained page table. */
	prepare_sources(engine);
	kern_mmio_write32(engine->core.mapped + CORE_L2TFLSTA, 0);
	kern_mmio_write32(engine->core.mapped + CORE_L2TFLEND, UINT32_MAX);
	error = bind_tables(engine);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* Old events cannot complete jobs admitted after the reset. */
	enabled = spin_lock_irqsave(&hardware->guard);

	kern_memset(&hardware->events, 0, sizeof(hardware->events));
	hardware->faulted = false;

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Only a completed power and MMU restoration permits fresh submission. */
	hardware->resets++;
	error = open_sources(v3d);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}
	hardware->ready = true;

	/* Succeeded: the old execution stopped and the same mappings are usable again. */
	return 0;
}

/*
 * Cleans changed PTEs and invalidates both levels of cached translation.
 */
int
bcm2711_v3d_hardware_pages_sync(
	struct bcm2711_v3d *engine,
	uint32_t address,
	uint64_t bytes)
{
	uint32_t *entries;
	size_t first;
	size_t count;
	int error;

	/* Bounds the changed GPU span before deriving its CPU page-table range. */
	if (bytes == 0 || bytes > 0x100000000ULL - address)
		return EINVAL;
	if ((address & (BCM2711_V3D_PAGE_BYTES - 1U)) != 0)
		return EINVAL;
	if ((bytes & (BCM2711_V3D_PAGE_BYTES - 1U)) != 0)
		return EINVAL;

	/* Faulted or reset engines cannot safely execute a translation command. */
	error = worker_admit(engine);
	if (error != 0)
		return error;

	/* Publishes only changed PTEs, then flushes the MMU's caches. */
	entries = engine->hardware.pages->address;
	first = address / BCM2711_V3D_PAGE_BYTES;
	count = (size_t)(bytes / BCM2711_V3D_PAGE_BYTES);
	kern_dcache_clean_range(entries + first, count * sizeof(*entries));
	kern_io_write_barrier();
	error = flush_translation(engine);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* A concurrent fault cannot make a finished flush imply retirement. */
	error = worker_admit(engine);
	if (error != 0)
		return error;

	/* Succeeded: stale translations can no longer borrow a removed physical page. */
	return 0;
}

/*
 * Invalidates L2T then the slice caches before bin, render or compute execution.
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

	/* Flushes L2T first, then every slice cache, so inner caches cannot refill stale data. */
	kern_mmio_write32(engine->core.mapped + CORE_L2TCACTL, 1);
	kern_mmio_write32(engine->core.mapped + CORE_SLCACTL, 0x0f0f0f0f);
	kern_io_write_barrier();

	/* Reports any fault delivered during this command before admitting a job. */
	error = worker_admit(engine);
	if (error != 0)
		return error;

	/* Succeeded: later fetches observe RAM cleaned by their resource owners. */
	return 0;
}

/*
 * Drains TMU write combiners and cleans L2T before reporting output completion.
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
	kern_mmio_write32(engine->core.mapped + CORE_L2TCACTL, 0x100);
	error = wait_clear(engine, engine->core.mapped, CORE_L2TCACTL, 0x100);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* Cleans the full L2T range (flush with the clean mode) and waits for the drain. */
	kern_mmio_write32(engine->core.mapped + CORE_L2TCACTL, 5);
	error = wait_clear(engine, engine->core.mapped, CORE_L2TCACTL, 1);
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* Orders later CPU observation after the finished cache maintenance. */
	kern_io_read_barrier();

	/* A simultaneous fault takes precedence over a cleared command bit. */
	error = worker_admit(engine);
	if (error != 0)
		return error;

	/* Succeeded: GPU caches no longer hide completed TMU stores from RAM. */
	return 0;
}

/*
 * Consumes IRQ event latches while preserving the sticky fault state.
 */
void
bcm2711_v3d_hardware_events(
	struct bcm2711_v3d *engine,
	struct bcm2711_v3d_events *events)
{
	struct bcm2711_v3d_hardware *hardware;
	unsigned long enabled;

	/* An engine that never initialized has no events and no guard. */
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

/*
 * Quiets the hub's interrupt sources and captures its faults.
 *
 * Runs on the hub's line: texture formatting completion, MMU faults and
 * memory-protection violations.  Never allocates or waits.
 */
bool
bcm2712_v3d_hub_service(
	void *owner)
{
	struct bcm2711_v3d *engine;
	struct bcm2711_v3d_hardware *hardware;
	uint32_t hub;
	uint32_t control;
	unsigned long enabled;

	/* A handler arriving during reset observes the closed admission before touching the engine. */
	engine = owner;
	hardware = &engine->hardware;
	enabled = spin_lock_irqsave(&hardware->guard);

	if (!hardware->irq_live) {
		spin_unlock_irqrestore(&hardware->guard, enabled);
		return false;
	}

	/* Reads the hub's owned sources; an empty read is not this line's event. */
	hub = kern_mmio_read32(engine->hub.mapped + HUB_INT_STS) & BCM2712_V3D_HUB_EVENTS;
	if (hub == 0) {
		spin_unlock_irqrestore(&hardware->guard, enabled);
		return false;
	}

	/* Acknowledges the sources and latches them for the worker. */
	kern_mmio_write32(engine->hub.mapped + HUB_INT_CLR, hub);
	hardware->events.hub |= hub;
	hardware->events.arrival_tick = clock_ticks();
	waitq_wake_all(&hardware->events_queue);

	/* Faults close the engine until the worker proves a complete reset. */
	if ((hub & BCM2712_V3D_HUB_FAULTS) != 0) {
		hardware->faulted = true;
		hardware->events.error = EIO;
		hardware->events.fault_client = kern_mmio_read32(engine->hub.mapped + HUB_MMU_VIO_ID);
		hardware->events.fault_address = kern_mmio_read32(engine->hub.mapped + HUB_MMU_VIO_ADDR);
		control = kern_mmio_read32(engine->hub.mapped + HUB_MMU_CTL);
		kern_mmio_write32(engine->hub.mapped + HUB_MMU_CTL, control);
		kern_mmio_write32(engine->core.mapped + CORE_INT_MSK_SET, UINT32_MAX);
		kern_mmio_write32(engine->hub.mapped + HUB_INT_MSK_SET, UINT32_MAX);
		hardware->irq_live = false;
	}

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Succeeded: every owned source was quieted without pretending DMA was retired. */
	return true;
}

/* Records the identification and refuses an engine that is not a single-core 7.1. */
static int
identify(
	struct bcm2712_v3d *v3d)
{
	struct bcm2711_v3d *engine;
	struct bcm2711_v3d_hardware *hardware;
	uint32_t index;
	uint32_t layout;
	uint32_t cores;
	uint32_t widths;
	uint32_t core_layout;
	uint32_t slices;
	uint32_t qpus_per_slice;

	/* Reads the hub's four identification words and the core's three. */
	engine = &v3d->engine;
	hardware = &engine->hardware;
	for (index = 0; index < 4; index++)
		hardware->hub_ident[index] = kern_mmio_read32(engine->hub.mapped + HUB_IDENT0 + index * 4U);
	for (index = 0; index < 3; index++)
		hardware->core_ident[index] = kern_mmio_read32(engine->core.mapped + index * 4U);

	/* Decodes the version (TVER.REV), the core count and the revision from the hub. */
	layout = hardware->hub_ident[1];
	v3d->version = (layout & 15U) * 10U + ((layout >> 4) & 15U);
	cores = (layout >> 8) & 15U;
	v3d->revision = (hardware->hub_ident[3] >> 8) & 0xffU;
	v3d->compat_revision = (hardware->hub_ident[3] >> 16) & 0xffU;

	/* Decodes the QPUs (slices times QPUs per slice) and the VPM (8 KiB units) from the core. */
	core_layout = hardware->core_ident[1];
	slices = (core_layout >> 4) & 15U;
	qpus_per_slice = (core_layout >> 8) & 15U;
	v3d->qpus = slices * qpus_per_slice;
	v3d->vpm_bytes = ((core_layout >> 28) & 15U) * 8192U;

	/* Reads the MMU's address widths. */
	widths = kern_mmio_read32(engine->hub.mapped + HUB_MMU_DEBUG_INFO);
	hardware->physical_bits = 30U + ((widths >> 8) & 15U);
	hardware->virtual_bits = 30U + ((widths >> 4) & 15U);

	/* Shows everything read, before any refusal. */
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V2 hub %08x %08x %08x %08x",
		hardware->hub_ident[0],
		hardware->hub_ident[1],
		hardware->hub_ident[2],
		hardware->hub_ident[3]);
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V2 core %08x %08x %08x",
		hardware->core_ident[0],
		hardware->core_ident[1],
		hardware->core_ident[2]);
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V2 version %u.%u rev %u compat %u cores %u QPUs %u VPM %u KiB PA %u VA %u",
		v3d->version / 10U,
		v3d->version % 10U,
		v3d->revision,
		v3d->compat_revision,
		cores,
		v3d->qpus,
		v3d->vpm_bytes / 1024U,
		hardware->physical_bits,
		hardware->virtual_bits);

	/* Refuses anything but a single-core 7.1 with an MMU. */
	if (v3d->version != 71U || cores != 1U)
		return ENOTSUP;
	if ((hardware->hub_ident[2] & 0x100U) == 0)
		return ENOTSUP;

	/* Requires the 32-bit GPU addresses the page table covers. */
	if (hardware->physical_bits < 32U || hardware->virtual_bits < 32U)
		return ENOTSUP;

	/* Revision 10 and later take the draw-index form of the shader state record. */
	bcm2712_v3d_draw_index_record = false;
	if (v3d->revision >= 10U)
		bcm2712_v3d_draw_index_record = true;

	/* The page table names at most 36 physical bits whatever the MMU could reach. */
	v3d->table_physical_bits = hardware->physical_bits;
	if (v3d->table_physical_bits > ENGINE_TABLE_PHYSICAL_BITS)
		v3d->table_physical_bits = ENGINE_TABLE_PHYSICAL_BITS;

	/* Succeeded: the register layout is the one this driver programs. */
	return 0;
}

/* Acquires zeroed table and fault scratch owners before any hardware can fetch them. */
static int
allocate_tables(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	int error;

	/* Allocates the complete 4 MiB table, including an unmapped virtual page zero. */
	hardware = &engine->hardware;
	error = bcm2711_buffer_create(
		ENGINE_TABLE_BYTES,
		ENGINE_LOW_LIMIT,
		BCM2711_V3D_PAGE_BYTES,
		&hardware->pages);
	if (error != 0)
		return error;

	/* Missing scratch storage prevents publication and permits an unexposed unwind. */
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

	/* Succeeded: both allocations belong to the persistent engine and are still unexposed. */
	return 0;
}

/* Installs the table's and the scratch page's PFNs and enables translation. */
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
	kern_mmio_write32(engine->hub.mapped + HUB_MMU_PT_PA_BASE, table_page);
	observed = kern_mmio_read32(engine->hub.mapped + HUB_MMU_PT_PA_BASE);
	if (observed != table_page)
		return EIO;

	/* Enables translation with its faults, sends illegal accesses to the scratch page and enables the PTE cache. */
	kern_mmio_write32(engine->hub.mapped + HUB_MMU_CTL, MMU_CONFIGURATION);
	kern_mmio_write32(engine->hub.mapped + HUB_MMU_ILLEGAL_ADDR, scratch_page | 0x80000000U);
	kern_mmio_write32(engine->hub.mapped + HUB_MMUC_CONTROL, 1);
	error = flush_translation(engine);
	if (error != 0)
		return error;

	/* Succeeded: translation storage is visible and both hardware caches are fresh. */
	return 0;
}

/* Flushes the page-entry cache before clearing cached virtual translations. */
static int
flush_translation(
	struct bcm2711_v3d *engine)
{
	uint32_t control;
	int error;

	/* Flushes the PTE cache (flush and enable) and waits until it stops flushing. */
	kern_mmio_write32(engine->hub.mapped + HUB_MMUC_CONTROL, 3);
	error = wait_clear(engine, engine->hub.mapped, HUB_MMUC_CONTROL, 4);
	if (error != 0)
		return error;

	/* Clears the TLB only after the PTE cache flush completed. */
	control = kern_mmio_read32(engine->hub.mapped + HUB_MMU_CTL);
	kern_mmio_write32(engine->hub.mapped + HUB_MMU_CTL, control | 4U);
	error = wait_clear(engine, engine->hub.mapped, HUB_MMU_CTL, 0x80);
	if (error != 0)
		return error;

	/* Succeeded: no earlier mapping remains in either translation cache. */
	return 0;
}

/* Polls a self-clearing command briefly before falling back to bounded yielding waits. */
static int
wait_clear(
	struct bcm2711_v3d *engine,
	volatile uint8_t *window,
	uint32_t offset,
	uint32_t mask)
{
	uint32_t attempt;
	uint32_t status;

	/* A bounded register-only burst avoids a scheduler tick for short commands. */
	for (attempt = 0; attempt < ENGINE_FAST_READS; attempt++) {
		/* Stops as soon as the command bit clears. */
		status = kern_mmio_read32(window + offset);
		if ((status & mask) == 0)
			return 0;
	}

	/* Slow hardware still yields, for at most about 100 ms. */
	for (attempt = 0; attempt <= ENGINE_WAIT_STEPS; attempt++) {
		/* Stops as soon as the command bit clears. */
		status = kern_mmio_read32(window + offset);
		if ((status & mask) == 0)
			return 0;
		if (attempt == ENGINE_WAIT_STEPS)
			break;

		/* Counts real yielding waits of the serialized maintenance owner. */
		engine->hardware.maintenance_yields++;
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

	/* Reset and incomplete starts close submission before register access. */
	if (!engine->hardware.ready || !engine->power.ready)
		return ENODEV;
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	faulted = engine->hardware.faulted;

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* IRQ faults remain visible even after the worker consumed their event bits. */
	if (faulted)
		return EIO;

	/* Succeeded: the sole worker owns a powered, initialized engine. */
	return 0;
}

/* Turns an incomplete hardware operation into a sticky fault with retained owners. */
static void
record_failure(
	struct bcm2711_v3d *engine,
	int error)
{
	unsigned long enabled;

	/* No later IRQ may disguise a failed cache or MMU operation as completion. */
	bcm2711_v3d_hardware_mask(engine);
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	engine->hardware.faulted = true;
	engine->hardware.events.error = error;

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);
}

/* Masks every source of both banks and clears the owned ones. */
static void
prepare_sources(
	struct bcm2711_v3d *engine)
{
	/* Masking precedes acknowledgement and any later worker publication. */
	kern_mmio_write32(engine->core.mapped + CORE_INT_MSK_SET, UINT32_MAX);
	kern_mmio_write32(engine->hub.mapped + HUB_INT_MSK_SET, UINT32_MAX);
	kern_mmio_write32(engine->core.mapped + CORE_INT_CLR, BCM2712_V3D_CORE_EVENTS);
	kern_mmio_write32(engine->hub.mapped + HUB_INT_CLR, BCM2712_V3D_HUB_EVENTS);
}

/* Opens the callbacks before unmasking the owned sources and both GIC lines. */
static int
open_sources(
	struct bcm2712_v3d *v3d)
{
	struct bcm2711_v3d *engine;
	unsigned long enabled;
	uint32_t core_mask;
	uint32_t hub_mask;

	/* Publishes IRQ admission and the exact owned masks atomically against service. */
	engine = &v3d->engine;
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	engine->hardware.irq_live = true;
	kern_mmio_write32(engine->core.mapped + CORE_INT_MSK_SET, ~BCM2712_V3D_CORE_EVENTS);
	kern_mmio_write32(engine->hub.mapped + HUB_INT_MSK_SET, ~BCM2712_V3D_HUB_EVENTS);
	kern_mmio_write32(engine->core.mapped + CORE_INT_MSK_CLR, BCM2712_V3D_CORE_EVENTS);
	kern_mmio_write32(engine->hub.mapped + HUB_INT_MSK_CLR, BCM2712_V3D_HUB_EVENTS);
	core_mask = kern_mmio_read32(engine->core.mapped + CORE_INT_MSK_STS);
	hub_mask = kern_mmio_read32(engine->hub.mapped + HUB_INT_MSK_STS);
	if ((core_mask & BCM2712_V3D_CORE_EVENTS) != 0 || (hub_mask & BCM2712_V3D_HUB_EVENTS) != 0) {
		engine->hardware.irq_live = false;
		kern_mmio_write32(engine->core.mapped + CORE_INT_MSK_SET, UINT32_MAX);
		kern_mmio_write32(engine->hub.mapped + HUB_INT_MSK_SET, UINT32_MAX);
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V4 masks did not open (core %08x hub %08x)", core_mask, hub_mask);
		return EIO;
	}

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* The sources now have an owner before the GIC can deliver them. */
	kern_irq_unmask(engine->irq.irq);
	kern_irq_unmask(v3d->hub_irq.irq);

	/* Succeeded: the owned sources are open on both lines. */
	return 0;
}

/* Quiets the core's interrupt sources: bin, render and compute completion and out of memory. */
static bool
core_service(
	void *owner)
{
	struct bcm2711_v3d *engine;
	struct bcm2711_v3d_hardware *hardware;
	uint32_t core;
	unsigned long enabled;

	/* A handler arriving during reset observes the closed admission before touching the engine. */
	engine = owner;
	hardware = &engine->hardware;
	enabled = spin_lock_irqsave(&hardware->guard);

	if (!hardware->irq_live) {
		spin_unlock_irqrestore(&hardware->guard, enabled);
		return false;
	}

	/* Reads the core's owned sources; an empty read is not this line's event. */
	core = kern_mmio_read32(engine->core.mapped + CORE_INT_STS) & BCM2712_V3D_CORE_EVENTS;
	if (core == 0) {
		spin_unlock_irqrestore(&hardware->guard, enabled);
		return false;
	}

	/* Acknowledges the sources and latches them for the worker. */
	kern_mmio_write32(engine->core.mapped + CORE_INT_CLR, core);
	hardware->events.core |= core;
	hardware->events.arrival_tick = clock_ticks();
	waitq_wake_all(&hardware->events_queue);

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Succeeded: the core's sources were quieted and latched. */
	return true;
}
