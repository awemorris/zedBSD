/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private, persistent MMU/cache/IRQ ownership for the single V3D 4.2 core. */
#ifndef KERN_DRIVERS_GPU_BCM2711_V3D_HARDWARE_H
#define KERN_DRIVERS_GPU_BCM2711_V3D_HARDWARE_H

#include <stdbool.h>
#include <stdint.h>
#include <kern/lock.h>
#include <kern/waitq.h>

struct bcm2711_buffer;
struct bcm2711_v3d;

/* The IRQ guard protects these latches; a serialized worker consumes them. */
struct bcm2711_v3d_events {
	uint32_t core;
	uint32_t hub;

	/* Arrival time diagnoses completion-to-worker delay; it never controls retirement. */
	uint64_t arrival_tick;
	uint32_t fault_client;
	uint32_t fault_address;
	int error;
};

/*
 * The engine retains both allocations once the MMU can fetch them.
 * The single worker owns cache/MMU commands and ready/reset counters.  guard
 * protects IRQ admission, faulted, and event latches; no waits occur under it.
 * initialized is published before installing the persistent IRQ owner.
 */
struct bcm2711_v3d_hardware {
	struct spinlock guard;
	struct bcm2711_buffer *pages;
	struct bcm2711_buffer *scratch;
	struct bcm2711_v3d_events events;
	/* The IRQ handler advances this queue under guard after latching events; the worker sleeps on it. */
	struct wait_queue events_queue;
	uint32_t hub_ident[4];
	uint32_t core_ident[3];
	uint32_t physical_bits;
	uint32_t virtual_bits;
	uint64_t resets;
	/* Serialized maintenance diagnostics count actual yielding waits, never IRQ work. */
	uint64_t maintenance_yields;

	/*
	 * The time the jobs ran from launch to retirement, in nanoseconds,
	 * summed over every job that ended.  Only
	 * bcm2711_v3d_busy_add raises it, atomically; hw.gputelemetry reads it
	 * without a lock.  It lives as long as the engine and is never reset.
	 */
	volatile uint64_t busy_ns;
	/* Textures are converted by the texture formatting unit; without it (v3d.tfu=0) they use the CPU. */
	bool tfu_uif_ready;
	/* The performance counters have their sources and run (cleared by a reset). */
	bool counters_ready;
	bool initialized;
	bool ready;
	bool mmu_published;
	bool irq_live;
	bool faulted;
	bool job_busy;
	/*
	 * The render executor runs a pass's native jobs without the controller
	 * mutex (guard).  While it is open no translation may change: a view
	 * whose last reference ends waits as delayed, and a new mapping waits.
	 * Only the executor, holding the mutex, opens it.
	 */
	bool window;
};

int bcm2711_v3d_hardware_start(struct bcm2711_v3d *engine);
void bcm2711_v3d_hardware_mask(struct bcm2711_v3d *engine);
int bcm2711_v3d_hardware_reset(struct bcm2711_v3d *engine);
int bcm2711_v3d_hardware_pages_sync(struct bcm2711_v3d *engine, uint32_t address, uint64_t bytes);
int bcm2711_v3d_hardware_invalidate(struct bcm2711_v3d *engine);
int bcm2711_v3d_hardware_clean(struct bcm2711_v3d *engine);
void bcm2711_v3d_hardware_events(struct bcm2711_v3d *engine, struct bcm2711_v3d_events *events);

/* Reads the event queue's sequence before looking at the events, so a later IRQ ends the next wait at once. */
uint64_t bcm2711_v3d_hardware_event_sequence(struct bcm2711_v3d *engine);

/* Sleeps until an IRQ after the observed sequence, or until the next scheduler tick at the latest. */
void bcm2711_v3d_hardware_event_wait(struct bcm2711_v3d *engine, uint64_t observed);

#endif
