/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Register access and forcewake.
 *
 * Most GT registers sit in a power domain that sleeps unless forcewake holds
 * it awake; a read of a sleeping register returns all-ones and a write to it
 * is dropped without any error.  Every access path here therefore states who
 * holds the register's domain:
 *
 *   held   the caller holds the domain across a sequence of accesses; the
 *          access refuses (and records) a register whose domain is asleep
 *   auto   the access takes and releases the domain around itself
 *   raw    no bookkeeping at all, for the init and reset paths that hold
 *          every domain they touch
 *
 * The bus access and the forcewake request and acknowledge registers are
 * reached through an operations table, so the same logic runs against a
 * mock register file in the host tests.
 */

#ifndef DRIVERS_GPU_I915_MMIO_H
#define DRIVERS_GPU_I915_MMIO_H

#include <stdint.h>

struct i915_trace;

/*
 * The forcewake domains the driver wakes.
 *
 * Gen12 gives the media engines domains of their own: a video engine
 * register reached while only RENDER and GT are held is not covered.  Gen9
 * has RENDER, GT and one MEDIA domain that covers every media engine; a
 * device uses only the domains of its own generation's map.
 */
enum i915_forcewake_domain {
	I915_FORCEWAKE_RENDER = 0,
	I915_FORCEWAKE_GT,
	I915_FORCEWAKE_MEDIA_VDBOX0,
	I915_FORCEWAKE_MEDIA_VDBOX2,
	I915_FORCEWAKE_MEDIA_VEBOX0,
	I915_FORCEWAKE_MEDIA_GEN9,
	I915_FORCEWAKE_DOMAIN_COUNT
};

/* How many times a wake request polls its acknowledge before giving up. */
#define I915_FORCEWAKE_ACK_POLLS	4096U

/*
 * One register range that belongs to a forcewake domain.
 *
 * A register in no range is always on.
 */
struct i915_mmio_range {
	uint32_t start;
	uint32_t end;
	int domain;
};

/*
 * The bus end of register access.
 *
 * The real device reaches the mapped BAR; a host test substitutes a
 * register file.  None of these take forcewake into account.
 */
struct i915_mmio_ops {
	/* Reads one register. */
	uint32_t (*read32)(void *context, uint32_t offset);

	/* Writes one register. */
	void (*write32)(void *context, uint32_t offset, uint32_t value);

	/* Asks a domain to wake (nonzero) or allows it to sleep (zero). */
	void (*forcewake_request)(void *context, int domain, int wake);

	/* Reports nonzero once the domain acknowledges that it is awake. */
	int (*forcewake_ack)(void *context, int domain);
};

/*
 * The register block of one device and the forcewake state that guards it.
 *
 * It lives inside the device from the register mapping to the device stop.
 * The forcewake counts are protected by the caller: the device start and
 * the request worker are the only users, and they never run at once.
 */
struct i915_mmio {
	/* The bus access and the context it is given. */
	const struct i915_mmio_ops *ops;
	void *context;

	/* Where register access failures are recorded; may be NULL. */
	struct i915_trace *trace;

	/* The register ranges that need forcewake, in ascending order. */
	const struct i915_mmio_range *ranges;
	unsigned range_count;

	/*
	 * How many holders each domain has.  The domain is woken when its
	 * count leaves zero and allowed to sleep when it returns to zero.
	 */
	int forcewake_count[I915_FORCEWAKE_DOMAIN_COUNT];

	/* Nonzero while the domain has acknowledged its wake request. */
	int forcewake_awake[I915_FORCEWAKE_DOMAIN_COUNT];

	/* How many wake requests were never acknowledged. */
	uint32_t forcewake_ack_timeouts;

	/*
	 * How many times a domain was woken (its count left zero), for the
	 * telemetry, which reads it from another thread without the caller's
	 * protection; a reader may see a value one wake old.
	 */
	volatile uint32_t forcewake_wakes;

	/* How many held-accessor reads and writes found their domain asleep (refused); read like forcewake_wakes. */
	volatile uint32_t forcewake_refusals;

	/* Nonzero once a release without a matching hold was attempted. */
	int forcewake_underflow;

	/*
	 * Nonzero while one caller steers replicated registers to a single
	 * instance; a second steer inside that section would redirect the
	 * first caller's accesses.
	 */
	int mcr_locked;

	/* The instance the steering section targets. */
	uint32_t mcr_steer;

	/*
	 * The graphics version whose register layout the block has (9 or 12;
	 * 0 until the device start sets it).  Paths that only differ by a
	 * register, such as the GGTT flush, branch on it.
	 */
	int graphics_ver;
};

/*
 * The mapped register window a real device's register access reaches.
 */
struct i915_mmio_window {
	volatile uint8_t *base;
	unsigned long size;
};

void drv_i915_mmio_init(struct i915_mmio *mmio, const struct i915_mmio_ops *ops, void *context, const struct i915_mmio_range *ranges, unsigned range_count, struct i915_trace *trace);
const struct i915_mmio_ops *drv_i915_mmio_window_ops(void);
const struct i915_mmio_range *drv_i915_mmio_gen12_ranges(unsigned *count);
const struct i915_mmio_range *drv_i915_mmio_gen11_ranges(unsigned *count);
const struct i915_mmio_range *drv_i915_mmio_gen9_ranges(unsigned *count);
int drv_i915_mmio_domain_of(const struct i915_mmio *mmio, uint32_t offset);

int drv_i915_forcewake_get(struct i915_mmio *mmio, int domain);
int drv_i915_forcewake_put(struct i915_mmio *mmio, int domain);
int drv_i915_forcewake_held(const struct i915_mmio *mmio, int domain);

uint32_t drv_i915_read32(struct i915_mmio *mmio, uint32_t offset);
void drv_i915_write32(struct i915_mmio *mmio, uint32_t offset, uint32_t value);
uint32_t drv_i915_read32_auto(struct i915_mmio *mmio, uint32_t offset);
void drv_i915_write32_auto(struct i915_mmio *mmio, uint32_t offset, uint32_t value);
uint32_t drv_i915_raw_read32(struct i915_mmio *mmio, uint32_t offset);
void drv_i915_raw_write32(struct i915_mmio *mmio, uint32_t offset, uint32_t value);
void drv_i915_posting_read32(struct i915_mmio *mmio, uint32_t offset);
void drv_i915_rmw32(struct i915_mmio *mmio, uint32_t offset, uint32_t mask, uint32_t value);
void drv_i915_write32_masked(struct i915_mmio *mmio, uint32_t offset, uint32_t masked_word);

int drv_i915_mcr_lock(struct i915_mmio *mmio, uint32_t steer);
void drv_i915_mcr_unlock(struct i915_mmio *mmio);
int drv_i915_mcr_locked(const struct i915_mmio *mmio);

#endif
