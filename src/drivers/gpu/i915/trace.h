/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The device execution trace.
 *
 * A fixed ring of structured records of what the driver actually did --
 * resources taken and given back, DMA mapped and unmapped, steps that ran,
 * operations that failed -- kept apart from what the source merely calls, so
 * a missing log line is never read as "the code did not run".  The ring is
 * dumped on demand instead of flooding the console, and every record it
 * overwrote before a dump is counted.
 */

#ifndef DRIVERS_GPU_I915_TRACE_H
#define DRIVERS_GPU_I915_TRACE_H

#include <stdint.h>

/* What one trace record reports. */
enum i915_trace_op {
	I915_TRACE_ENTRY = 0,
	I915_TRACE_EXIT,
	I915_TRACE_ACQUIRE,
	I915_TRACE_RELEASE,
	I915_TRACE_MAP,
	I915_TRACE_UNMAP,
	I915_TRACE_SYNC_DEVICE,
	I915_TRACE_SYNC_CPU,
	I915_TRACE_WORK_ENQUEUE,
	I915_TRACE_WORK_BEGIN,
	I915_TRACE_WORK_END,
	I915_TRACE_WORK_CANCEL,
	I915_TRACE_UNIMPLEMENTED,
	I915_TRACE_FAIL,
	I915_TRACE_NOTE
};

/*
 * How many records the ring holds.
 *
 * The ring is a static object, never an automatic one (a kernel thread stack
 * is 16 KiB), and the dump prints only the newest 256, so a larger ring only
 * costs .bss -- which once pushed the kernel past what the UEFI loader could
 * place.
 */
#define I915_TRACE_CAPACITY	1024U

/*
 * One thing the driver did.
 *
 * The name is a static string that is never freed.
 */
struct i915_trace_record {
	/* The record's position in the whole trace; a gap means records were overwritten. */
	uint32_t sequence;

	/* The start phase the record belongs to, or zero. */
	uint16_t stage;

	/* What happened, one of enum i915_trace_op. */
	uint16_t op;

	/* The operation or resource the record names. */
	const char *what;

	/* Two values the operation produced; their meaning depends on the operation. */
	uint64_t argument0;
	uint64_t argument1;
};

/*
 * The trace of one device.
 *
 * It lives from the device start to the device free and is written only by
 * the thread that runs the device start and, later, the request worker.
 */
struct i915_trace {
	struct i915_trace_record records[I915_TRACE_CAPACITY];

	/* The slot the next record is written to. */
	uint32_t next;

	/* How many records were ever written. */
	uint32_t sequence;

	/* How many records were overwritten before anyone read them. */
	uint32_t dropped;

	/* Nonzero once the ring has wrapped at least once. */
	int wrapped;
};

void drv_i915_trace_init(struct i915_trace *trace);
void drv_i915_trace_record(struct i915_trace *trace, uint16_t stage, uint16_t op, const char *what, uint64_t argument0, uint64_t argument1);
uint32_t drv_i915_trace_count(const struct i915_trace *trace);
uint32_t drv_i915_trace_snapshot(const struct i915_trace *trace, struct i915_trace_record *records, uint32_t max);
const char *drv_i915_trace_op_name(uint16_t op);

#endif
