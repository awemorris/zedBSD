/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The trace of a window's frames (the diagnostic KL_TRACE_FRAMES=1,
 * ws210-p014, see trace.h): the inputs gathered for each frame, the
 * frames awaiting their callbacks, and their lines.
 */

#include "trace.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* How far (milliseconds) the compositor's time of a callback may be behind its arrival and still be read as the same clock. */
#define TRACE_BEHIND_MS	10000U

/* The longest line told. */
#define TRACE_LINE	768U

static void trace_gather(struct keiui_trace_inputs *into, const struct keiui_trace_inputs *from);
static uint64_t trace_since(uint64_t earlier, uint64_t later);
static uint64_t trace_compositor_us(uint64_t arrival_us, uint32_t time_ms);
static void trace_tell(struct keiui_trace_frame *frame);

/*
 * Notes an input of the window that arrived at arrival_us: one queued, or
 * one merged into a motion queued before it (merged).
 */
void
keiui_trace_input(
	struct keiui_trace *trace,
	uint64_t arrival_us,
	int merged)
{
	struct keiui_trace_inputs *inputs;

	/* The first input since the last frame begun starts the wait it is drawn after. */
	inputs = &trace->waiting;
	if (inputs->count == 0U && inputs->merged == 0U)
		inputs->first_us = arrival_us;

	/* One input more, queued or merged. */
	if (merged)
		inputs->merged++;
	else
		inputs->count++;
	inputs->last_us = arrival_us;
}

/*
 * Notes the compositor's time (microseconds of the monotonic clock) of the
 * window's newest input.
 */
void
keiui_trace_stamp(
	struct keiui_trace *trace,
	uint64_t time_us)
{
	/* The newest input's time, as the compositor told it. */
	trace->waiting.event_us = time_us;
}

/*
 * Notes an answer of kl_window_frame_wait at now_us: a wait (the pace's)
 * starts when the first one since the last frame is not 0; an answer of 0
 * lets the frame be drawn, with the inputs that came until then and the
 * pointer at (x, y).
 */
void
keiui_trace_answer(
	struct keiui_trace *trace,
	int wait,
	uint64_t now_us,
	double x,
	double y)
{
	/* Told to wait: the first time since the last frame is when the wait began. */
	if (wait != 0) {
		if (trace->waited_us == 0U)
			trace->waited_us = now_us;
		return;
	}

	/* Told to draw: the frame shows the inputs so far, from now. */
	trace->ready_us = now_us;
	trace->x = (int)x;
	trace->y = (int)y;
	trace_gather(&trace->drawn, &trace->waiting);
	memset(&trace->waiting, 0, sizeof(trace->waiting));
}

/*
 * Takes a slot for a frame about to be shown at now_us.  When every slot
 * awaits a callback, the oldest frame is told without its answer and its
 * slot is taken; its callback, left in the slot, is the caller's to
 * destroy before it asks for the new one's.
 */
struct keiui_trace_frame *
keiui_trace_open(
	struct keiui_trace *trace,
	uint64_t now_us)
{
	struct keiui_trace_frame *frame;
	void *callback;
	unsigned index;

	/* A free slot, or the oldest frame's. */
	frame = NULL;
	for (index = 0U; index < KEIUI_TRACE_FRAMES; index++) {
		if (!trace->slots[index].used) {
			frame = &trace->slots[index];
			break;
		}

		/* A slot in use is the one taken when its frame is the oldest. */
		if (frame == NULL || trace->slots[index].number < frame->number)
			frame = &trace->slots[index];
	}

	/* The oldest frame, its answer not come, is told as it stands. */
	if (frame->used)
		trace_tell(frame);

	/* The slot for the new frame, numbered, from its showing's start; the old callback stays for the caller. */
	callback = frame->callback;
	memset(frame, 0, sizeof(*frame));
	frame->callback = callback;
	frame->used = 1;
	trace->frames++;
	frame->number = trace->frames;
	frame->present_us = now_us;

	/* Succeeded: the slot. */
	return frame;
}

/*
 * Notes a frame's showing, returned at now_us with error (0 when shown):
 * the inputs it shows and its wait, its parts (none: the whole frame,
 * whole_area pixels) and the presenter's times (NULL: none).  A frame not
 * shown, or awaiting no callback, is told at once.
 */
void
keiui_trace_shown(
	struct keiui_trace *trace,
	struct keiui_trace_frame *frame,
	const struct keiui_present_stamps *stamps,
	const struct kl_rect *parts,
	unsigned count,
	uint64_t whole_area,
	int error,
	uint64_t now_us)
{
	unsigned index;

	/* A window never told it may draw: the frame shows every input so far, its drawing's start not known. */
	if (trace->ready_us == 0U) {
		trace_gather(&trace->drawn, &trace->waiting);
		memset(&trace->waiting, 0, sizeof(trace->waiting));
	}

	/* The inputs, the pointer and the wait of the frame; the next frame starts anew. */
	frame->inputs = trace->drawn;
	frame->x = trace->x;
	frame->y = trace->y;
	frame->waited_us = trace->waited_us;
	frame->ready_us = trace->ready_us;
	memset(&trace->drawn, 0, sizeof(trace->drawn));
	trace->waited_us = 0U;
	trace->ready_us = 0U;

	/* The parts drawn, and their pixels. */
	frame->parts = count;
	frame->area = whole_area;
	if (count != 0U)
		frame->area = 0U;
	for (index = 0U; index < count; index++)
		frame->area += (uint64_t)parts[index].width * (uint64_t)parts[index].height;

	/* The presenter's times, and how the showing ended. */
	if (stamps != NULL)
		frame->stamps = *stamps;
	frame->shown_us = now_us;
	frame->error = error;

	/* Not shown, or no answer to wait for: told now. */
	if (error != 0 || frame->callback == NULL) {
		trace_tell(frame);
		frame->used = 0;
	}
}

/*
 * Notes the answer of a frame's callback, come at now_us with the
 * compositor's time_ms, and tells the frame.  The callback is gone.
 */
void
keiui_trace_done(
	struct keiui_trace_frame *frame,
	uint64_t now_us,
	uint32_t time_ms)
{
	/* The answer. */
	frame->callback = NULL;
	frame->done = 1;
	frame->done_us = now_us;
	frame->done_ms = time_ms;

	/* Told, and the slot free. */
	trace_tell(frame);
	frame->used = 0;
}

/*
 * Tells every frame still awaiting its callback, the callbacks gone (the
 * window closes).
 */
void
keiui_trace_flush(
	struct keiui_trace *trace)
{
	unsigned index;

	/* Each frame in a slot, as it stands. */
	for (index = 0U; index < KEIUI_TRACE_FRAMES; index++) {
		if (!trace->slots[index].used)
			continue;

		/* Told without its answer, the slot free. */
		trace->slots[index].callback = NULL;
		trace_tell(&trace->slots[index]);
		trace->slots[index].used = 0;
	}
}

/*
 * Writes a frame's line into a buffer of size bytes and reports its
 * length: the absolute times (_at, microseconds of the monotonic clock,
 * the clock of the compositor's KWL LAT at_us) and the steps' durations
 * (_us).  The latency is from the newest input the frame shows to the
 * compositor's answer (its time, else the answer's arrival).
 */
size_t
keiui_trace_line(
	const struct keiui_trace_frame *frame,
	char *line,
	size_t size)
{
	const struct keiui_present_stamps *stamps;
	uint64_t done_event_us;
	uint64_t answered_us;
	int length;

	/* The compositor's time of the answer, else its arrival (0 without one). */
	done_event_us = 0U;
	if (frame->done)
		done_event_us = trace_compositor_us(frame->done_us, frame->done_ms);
	answered_us = done_event_us;
	if (answered_us == 0U)
		answered_us = frame->done_us;

	/* One line. */
	stamps = &frame->stamps;
	length = snprintf(
		line,
		size,
		"KL TRACE frame=%llu pid=%ld in=%u merged=%u in_at=%llu in_last_at=%llu in_ev_at=%llu x=%d y=%d"
		" waited_at=%llu ready_at=%llu paced_us=%llu draw_us=%llu parts=%u area=%llu"
		" present_at=%llu copy_us=%llu acquire_us=%llu submit_us=%llu queue_us=%llu fence_us=%llu present_us=%llu"
		" shown_at=%llu done_at=%llu done_ev_at=%llu lat_us=%llu ev_lat_us=%llu error=%d\n",
		(unsigned long long)frame->number,
		(long)getpid(),
		frame->inputs.count,
		frame->inputs.merged,
		(unsigned long long)frame->inputs.first_us,
		(unsigned long long)frame->inputs.last_us,
		(unsigned long long)frame->inputs.event_us,
		frame->x,
		frame->y,
		(unsigned long long)frame->waited_us,
		(unsigned long long)frame->ready_us,
		(unsigned long long)trace_since(frame->waited_us, frame->ready_us),
		(unsigned long long)trace_since(frame->ready_us, frame->present_us),
		frame->parts,
		(unsigned long long)frame->area,
		(unsigned long long)frame->present_us,
		(unsigned long long)trace_since(stamps->started_us, stamps->copied_us),
		(unsigned long long)trace_since(stamps->copied_us, stamps->acquired_us),
		(unsigned long long)trace_since(stamps->acquired_us, stamps->submitted_us),
		(unsigned long long)trace_since(stamps->submitted_us, stamps->queued_us),
		(unsigned long long)trace_since(stamps->queued_us, stamps->finished_us),
		(unsigned long long)trace_since(frame->present_us, frame->shown_us),
		(unsigned long long)frame->shown_us,
		(unsigned long long)frame->done_us,
		(unsigned long long)done_event_us,
		(unsigned long long)trace_since(frame->inputs.last_us, answered_us),
		(unsigned long long)trace_since(frame->inputs.event_us, answered_us),
		frame->error);
	if (length < 0)
		return 0U;

	/* A line cut to the buffer is as long as the buffer holds. */
	if ((size_t)length >= size && size != 0U)
		return size - 1U;

	/* Succeeded: the line's length. */
	return (size_t)length;
}

/* Adds the inputs of from to those of into (from being the later). */
static void
trace_gather(
	struct keiui_trace_inputs *into,
	const struct keiui_trace_inputs *from)
{
	/* No input to add. */
	if (from->count == 0U && from->merged == 0U)
		return;

	/* None before: the later ones alone. */
	if (into->count == 0U && into->merged == 0U) {
		*into = *from;
		return;
	}

	/* Both: from the first of the earlier to the last of the later. */
	into->count += from->count;
	into->merged += from->merged;
	into->last_us = from->last_us;
	if (from->event_us != 0U)
		into->event_us = from->event_us;
}

/* Reports the microseconds from earlier to later, 0 when either is not known or they are out of order. */
static uint64_t
trace_since(
	uint64_t earlier,
	uint64_t later)
{
	/* A time not known. */
	if (earlier == 0U || later == 0U)
		return 0U;

	/* Out of order. */
	if (later < earlier)
		return 0U;

	/* The time between them. */
	return later - earlier;
}

/*
 * Reports the compositor's time of an answer (milliseconds of the
 * monotonic clock, the low 32 bits) in microseconds of the full clock, by
 * how far it is behind the answer's arrival; 0 for another clock's.
 */
static uint64_t
trace_compositor_us(
	uint64_t arrival_us,
	uint32_t time_ms)
{
	uint64_t arrival_ms;
	uint32_t behind;

	/* How far the compositor's time is behind the arrival, modulo 2^32 milliseconds. */
	arrival_ms = arrival_us / 1000U;
	behind = (uint32_t)arrival_ms - time_ms;

	/* Another clock, or a time ahead. */
	if (behind > TRACE_BEHIND_MS)
		return 0U;

	/* The compositor's time on the full clock. */
	return (arrival_ms - behind) * 1000U;
}

/* Tells a frame's line on the standard error, in one write. */
static void
trace_tell(
	struct keiui_trace_frame *frame)
{
	char line[TRACE_LINE];
	size_t length;

	/* The line. */
	length = keiui_trace_line(frame, line, sizeof(line));
	if (length == 0U)
		return;

	/* Written whole. */
	(void)fwrite(line, 1U, length, stderr);
}
