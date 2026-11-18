/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The trace of a window's frames (the diagnostic KL_TRACE_FRAMES=1,
 * ws210-p014): for each frame, one line on the standard error with the
 * times of the way from the input it shows to the compositor's taking of
 * it (the inputs, the wait for the pace, the drawing, the presenter's
 * steps and the frame callback), in microseconds of the monotonic clock,
 * the clock of the compositor's KWL LAT lines.  Plain C, so the host
 * tests can run it without Wayland or Vulkan.
 */

#ifndef KEILAND_UI_TRACE_H
#define KEILAND_UI_TRACE_H

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* How many frames await their callbacks at most; the oldest is told without its answer when one more is shown. */
#define KEIUI_TRACE_FRAMES	8U

/*
 * The times of the steps of the Vulkan presenter's last frame (present.c),
 * in microseconds of the monotonic clock: its start, the canvas written,
 * the image acquired, the commands submitted, the image queued for the
 * window and the copy finished.  0 for a step not reached.
 */
struct keiui_present_stamps {
	uint64_t started_us;
	uint64_t copied_us;
	uint64_t acquired_us;
	uint64_t submitted_us;
	uint64_t queued_us;
	uint64_t finished_us;
};

/*
 * The inputs of a window gathered for a frame: how many were queued and
 * how many were merged into one queued before (KL_PACING_MOTION), when
 * the first and the last arrived, and the compositor's time of the last
 * (0 when it told none).
 */
struct keiui_trace_inputs {
	unsigned count;
	unsigned merged;
	uint64_t first_us;
	uint64_t last_us;
	uint64_t event_us;
};

/*
 * One frame shown and traced, from its showing until its line is told:
 * the frame callback asked for with it (the caller's, NULL when none is
 * awaited; window.c), whether the slot is in use, the frame's number, the
 * inputs it shows, the pointer's place when it was begun, when the wait for
 * the pace began and ended (the drawing begins then), its parts and their
 * area, the presenter's times, when its showing returned and how, and the
 * callback's answer (when it came, and the compositor's time of it in
 * milliseconds).
 */
struct keiui_trace_frame {
	void *callback;
	int used;
	uint64_t number;
	struct keiui_trace_inputs inputs;
	int x;
	int y;
	uint64_t waited_us;
	uint64_t ready_us;
	unsigned parts;
	uint64_t area;
	uint64_t present_us;
	struct keiui_present_stamps stamps;
	uint64_t shown_us;
	int error;
	uint64_t done_us;
	uint32_t done_ms;
	int done;
};

/*
 * The trace of one window: the frames numbered so far, the inputs not yet
 * in a frame begun (waiting) and those of the frame being drawn (drawn,
 * taken when the window was told it may draw), when the window was first
 * told to wait since the last frame and last told it may draw (0: not),
 * the pointer's place then, and the frames awaiting their callbacks.
 */
struct keiui_trace {
	uint64_t frames;
	struct keiui_trace_inputs waiting;
	struct keiui_trace_inputs drawn;
	uint64_t waited_us;
	uint64_t ready_us;
	int x;
	int y;
	struct keiui_trace_frame slots[KEIUI_TRACE_FRAMES];
};

void keiui_trace_input(struct keiui_trace *trace, uint64_t arrival_us, int merged);
void keiui_trace_stamp(struct keiui_trace *trace, uint64_t time_us);
void keiui_trace_answer(struct keiui_trace *trace, int wait, uint64_t now_us, double x, double y);
struct keiui_trace_frame *keiui_trace_open(struct keiui_trace *trace, uint64_t now_us);
void keiui_trace_shown(struct keiui_trace *trace, struct keiui_trace_frame *frame, const struct keiui_present_stamps *stamps, const struct kl_rect *parts, unsigned count, uint64_t whole_area, int error, uint64_t now_us);
void keiui_trace_done(struct keiui_trace_frame *frame, uint64_t now_us, uint32_t time_ms);
void keiui_trace_flush(struct keiui_trace *trace);
size_t keiui_trace_line(const struct keiui_trace_frame *frame, char *line, size_t size);

#endif
