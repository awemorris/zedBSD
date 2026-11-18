/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The record of the display register writes of a device's start.
 *
 * From the first register write of a Gen12 device until the display shows
 * its first picture, every write that reaches a display register is kept
 * in a fixed ring, in the order the writes reached the hardware: the
 * register access (mmio.c) hands each write over while the record is open.
 * A long run of writes to consecutive registers (a firmware program) is
 * kept as one entry with its length and the sum of its values, and the
 * writes to the display interrupt registers (acknowledges and masks) are
 * only counted.  A name can be put between the writes, to tell which part
 * of the display code wrote what follows.
 *
 * The record is closed by whoever reports it, and closes itself once its
 * ring is full or the device has written many registers without the record
 * being reported, so a device whose display never shows a picture stops
 * paying for it.  Two digests of the record (one that depends on the order
 * of the writes and one that does not) let two starts be compared without
 * the whole list.
 */

#ifndef DRIVERS_GPU_I915_MMIO_RECORD_H
#define DRIVERS_GPU_I915_MMIO_RECORD_H

#include <stdint.h>

struct i915_mmio;

/* How many entries the ring keeps. */
#define I915_MMIO_RECORD_ENTRIES	4096U

/* How many writes to consecutive registers become one run entry. */
#define I915_MMIO_RECORD_RUN		16U

/*
 * How many register writes of any kind the device may make before the record
 * closes itself without being reported.
 */
#define I915_MMIO_RECORD_WRITE_LIMIT	262144U

/*
 * One entry of the record.
 *
 * A write keeps its register and value (count 1).  A run keeps its first
 * register, how many consecutive registers it wrote, and the sum of the
 * values.  A mark (count 0) keeps the name put between the writes.
 */
struct i915_mmio_record_entry {
	uint32_t offset;
	uint32_t value;
	uint32_t count;
	const char *mark;
};

/*
 * What the record holds, in a few numbers.
 *
 * The order digest changes when two writes swap places; the set digest
 * changes only when a write is added, missing or has another value.
 */
struct i915_mmio_record_digest {
	/* The register writes kept, a run counting each of its writes. */
	uint32_t writes;

	/* The entries the ring holds, and how many of them are runs. */
	uint32_t entries;
	uint32_t runs;

	/* The writes to the display interrupt registers, counted only. */
	uint32_t interrupt_writes;

	/* The display writes that came after the ring was full. */
	uint32_t dropped;

	/* The digests of the kept writes. */
	uint32_t order_digest;
	uint32_t set_digest;

	/* Nonzero when the record closed itself before it was reported. */
	int closed_early;
};

/*
 * Nonzero while the record is open.  The register access reads it on every
 * write and hands the write over only while it is set; it is cleared once,
 * when the record closes.
 */
extern volatile int drv_i915_mmio_record_on;

void drv_i915_mmio_record_write(const struct i915_mmio *mmio, uint32_t offset, uint32_t value);
void drv_i915_mmio_record_mark(const char *name);
void drv_i915_mmio_record_close(struct i915_mmio_record_digest *digest);
unsigned drv_i915_mmio_record_entries(struct i915_mmio_record_entry *out, unsigned max);
void drv_i915_mmio_record_log(int whole);
void drv_i915_mmio_record_reset(void);

#endif
