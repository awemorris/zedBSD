/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The device execution trace (see trace.h).
 */

#include "trace.h"

#include <stddef.h>

/*
 * Empties the trace.
 */
void
drv_i915_trace_init(
	struct i915_trace *trace)
{
	uint32_t index;

	/* Starts the ring empty and unwrapped. */
	trace->next = 0U;
	trace->sequence = 0U;
	trace->dropped = 0U;
	trace->wrapped = 0;

	/* Clears every slot so a dump never shows a stale name. */
	for (index = 0U; index < I915_TRACE_CAPACITY; index++) {
		trace->records[index].sequence = 0U;
		trace->records[index].stage = 0U;
		trace->records[index].op = I915_TRACE_NOTE;
		trace->records[index].what = NULL;
		trace->records[index].argument0 = 0U;
		trace->records[index].argument1 = 0U;
	}
}

/*
 * Appends one record, overwriting the oldest once the ring is full.
 */
void
drv_i915_trace_record(
	struct i915_trace *trace,
	uint16_t stage,
	uint16_t op,
	const char *what,
	uint64_t argument0,
	uint64_t argument1)
{
	struct i915_trace_record *record;

	/* Counts the record this one overwrites, which nobody read. */
	record = &trace->records[trace->next];
	if (trace->wrapped != 0)
		trace->dropped++;

	/* Fills the slot. */
	record->sequence = trace->sequence;
	record->stage = stage;
	record->op = op;
	record->what = what;
	record->argument0 = argument0;
	record->argument1 = argument1;

	/* Advances to the next slot, wrapping at the end of the ring. */
	trace->sequence++;
	trace->next++;
	if (trace->next == I915_TRACE_CAPACITY) {
		trace->next = 0U;
		trace->wrapped = 1;
	}
}

/*
 * Reports how many records the ring currently holds.
 */
uint32_t
drv_i915_trace_count(
	const struct i915_trace *trace)
{
	/* A wrapped ring is full. */
	if (trace->wrapped != 0)
		return I915_TRACE_CAPACITY;

	/* Succeeded: the ring holds every record written so far. */
	return trace->next;
}

/*
 * Copies up to max records, oldest first, and reports how many were copied.
 */
uint32_t
drv_i915_trace_snapshot(
	const struct i915_trace *trace,
	struct i915_trace_record *records,
	uint32_t max)
{
	uint32_t count;
	uint32_t start;
	uint32_t index;

	/* Limits the copy to what the ring holds and what the caller has room for. */
	count = drv_i915_trace_count(trace);
	if (count > max)
		count = max;

	/* The oldest record sits at the write slot once the ring has wrapped. */
	start = 0U;
	if (trace->wrapped != 0)
		start = trace->next;

	/* Copies the records in the order they were written. */
	for (index = 0U; index < count; index++)
		records[index] = trace->records[(start + index) % I915_TRACE_CAPACITY];

	/* Succeeded: reports how many records the caller received. */
	return count;
}

/*
 * Names a trace operation for a dump.
 */
const char *
drv_i915_trace_op_name(
	uint16_t op)
{
	/* Maps each operation to the word the dump prints. */
	switch (op) {
	case I915_TRACE_ENTRY:
		return "entry";
	case I915_TRACE_EXIT:
		return "exit";
	case I915_TRACE_ACQUIRE:
		return "acquire";
	case I915_TRACE_RELEASE:
		return "release";
	case I915_TRACE_MAP:
		return "map";
	case I915_TRACE_UNMAP:
		return "unmap";
	case I915_TRACE_SYNC_DEVICE:
		return "sync_for_device";
	case I915_TRACE_SYNC_CPU:
		return "sync_for_cpu";
	case I915_TRACE_WORK_ENQUEUE:
		return "work_enqueue";
	case I915_TRACE_WORK_BEGIN:
		return "work_begin";
	case I915_TRACE_WORK_END:
		return "work_end";
	case I915_TRACE_WORK_CANCEL:
		return "work_cancel";
	case I915_TRACE_UNIMPLEMENTED:
		return "unimplemented";
	case I915_TRACE_FAIL:
		return "fail";
	case I915_TRACE_NOTE:
		return "note";
	default:
		return "?";
	}
}
