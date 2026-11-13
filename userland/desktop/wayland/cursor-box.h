/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cursor's box (cursor-box.c, ws213-p004; the design is
 * plan/ws213/phase001/phase.md 6.1 and 6.2).
 *
 * The box holds the pointer's place as the input thread last wrote it:
 * the input thread adds each motion to it, and the main thread's warps
 * (the pointer carried off an output that goes, an absolute device's
 * place) replace it.  It also keeps the outputs' rectangles the motions
 * are computed over, so that a place and the rectangles it was computed
 * with are always written in one section.  The main thread's own view of
 * the pointer (server->pointer_x) follows the box through the input
 * thread's events and may be behind it.
 *
 * One mutex (the cursor lock of the design) guards it; its holder calls
 * nothing that waits, prints or enters Vulkan (design 4.6).
 */

#ifndef KWL_CURSOR_BOX_H
#define KWL_CURSOR_BOX_H

#include "plane.h"

#include <pthread.h>
#include <stdint.h>

/* The cursor plane's images of a client's cursor (design 4.7), which the main thread writes and the cursor thread reads. */
#define KWL_CURSOR_COPIES	2U

struct kwl_import;

/*
 * The cursor thread's counts of the KWL PERF lines (ws213-p001 M0, p004):
 * presents, moves held for the next refresh, images copied, a present's
 * parts in cycles (the copies' fences asked, the acquire, the copy's
 * recording and submission, the present itself and its longest), the
 * longest present from its start to its end and the longest time between
 * two presents while the pointer moved, in microseconds.  The cursor thread
 * adds to the box's copy; the main thread takes it at each window.
 */
struct kwl_cursor_stats {
	uint32_t presents;
	uint32_t held;
	uint32_t copies;
	uint64_t fence_cycles;
	uint64_t acquire_cycles;
	uint64_t copy_cycles;
	uint64_t present_cycles;
	uint64_t present_max_cycles;
	uint64_t present_max_us;
	uint64_t present_gap_max_us;
};

/*
 * The pointer's place after one write of the box, as the writer reports it
 * to the main thread: whether the place changed, whether it crossed to
 * another output, the place, and the warp count it was computed under (a
 * motion computed before a later warp is not taken as the pointer's
 * place).  event_us is the evdev time of the report that moved it and
 * write_us when the box was written (kwl_microseconds), the two ends of
 * the box's lag (design 6.4).  The input thread adds where the lag went
 * (ws213-p004, the KWL CURSOR_LAG line): when its pass began to poll, when
 * the poll returned and when the read that gave the report returned; 0
 * for a write that is not the input thread's.
 */
struct kwl_cursor_motion {
	unsigned moved;
	unsigned crossed;
	int32_t x;
	int32_t y;
	unsigned output;
	uint64_t warp;
	uint64_t event_us;
	uint64_t write_us;
	uint64_t poll_entered_us;
	uint64_t poll_returned_us;
	uint64_t read_us;
};

/*
 * One output's panel as the absolute devices see it (WS217): its physical
 * width and height, and the angle the picture is shown at on it (clockwise
 * degrees, rotate.h), which the output's logical rectangle is turned by.
 */
struct kwl_cursor_physical {
	uint32_t width;
	uint32_t height;
	unsigned rotation;
};

/*
 * What the pointer's place is computed over (WS217, plan/ws217/phase001
 * 6.6): the outputs' logical rectangles of the plane (no size: not shown),
 * each output's panel and angle, and the output an absolute pointer maps
 * onto.  The main thread makes it (kwl_output_geometry) and gives it to
 * the box whole, so that a place is never computed over the rectangles of
 * one moment and the angles of another.
 */
struct kwl_cursor_geometry {
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	unsigned output_count;
	struct kwl_cursor_physical physical[KWL_PLANE_SLOTS];
	unsigned absolute_slot;
};

/* One axis of an absolute device's report: the value and the range it lies in. */
struct kwl_cursor_axis {
	int32_t value;
	int32_t minimum;
	int32_t maximum;
};

/*
 * One warp of the main thread: given the box's place and output, gives the
 * new ones.  It runs under the cursor lock, so it only computes (plane.c's
 * functions): no output, no allocation.
 */
typedef void kwl_cursor_box_move_fn(const void *context, const struct kwl_plane_rect *outputs, int32_t *x, int32_t *y, unsigned *output);

/*
 * The box shared by the input thread (motions), the main thread (warps,
 * the outputs' rectangles, the cursor's image) and the cursor thread
 * (reads, and its answers).  writes counts every write of the place, so a
 * reader tells a new place from one it has seen; warp counts the main
 * thread's warps only.
 *
 * The main thread also writes what the plane shows (design 6.2): the image
 * (a cursor shape, the arrow, or one of the two copies of a client's
 * cursor), its generation and version, its hotspot, and whether the plane
 * shows the cursor at all (show 0: the cursor is composed, the plane is
 * empty); and asks the cursor thread to park (it answers parked and calls
 * no Vulkan until the park ends) or to stop.  The cursor thread writes
 * whether it failed (it then calls no Vulkan again), how many of its copies
 * read each of the two copies (pins: the main thread writes only a copy
 * with none that the box does not show), and its counts.  copy_wanted is
 * the main thread's note that it found both copies in use: the cursor
 * thread clears it and wakes the main thread when it lets go of a copy's
 * last pin, so the main thread writes the copy then.  sequence counts
 * every change the cursor thread waits for; answered signals the main
 * thread.
 *
 * The last four fields are the geometry the motions are computed over
 * (struct kwl_cursor_geometry, WS217), always written together.
 */
struct kwl_cursor_box {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	pthread_cond_t answered;
	uint64_t sequence;
	const struct kwl_import *image;
	uint64_t image_generation;
	uint32_t image_version;
	int32_t hotspot_x;
	int32_t hotspot_y;
	unsigned show;
	unsigned park;
	unsigned stop;
	unsigned parked;
	unsigned failed;
	const struct kwl_import *copies[KWL_CURSOR_COPIES];
	unsigned pins[KWL_CURSOR_COPIES];
	unsigned copy_wanted;
	struct kwl_cursor_stats stats;
	int32_t x;
	int32_t y;
	unsigned output;
	uint64_t event_us;
	uint64_t write_us;
	uint64_t warp;
	uint64_t writes;
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	unsigned output_count;
	struct kwl_cursor_physical physical[KWL_PLANE_SLOTS];
	unsigned absolute_slot;
};

int kwl_cursor_box_init(struct kwl_cursor_box *box, int32_t x, int32_t y, uint32_t width, uint32_t height);
void kwl_cursor_box_destroy(struct kwl_cursor_box *box);
void kwl_cursor_box_move(struct kwl_cursor_box *box, int64_t dx, int64_t dy, uint64_t event_us, struct kwl_cursor_motion *motion);
void kwl_cursor_box_place(struct kwl_cursor_box *box, int32_t value_x, int32_t minimum_x, int32_t maximum_x, int32_t value_y, int32_t minimum_y, int32_t maximum_y, int64_t dx, int64_t dy, uint64_t event_us, struct kwl_cursor_motion *motion);
void kwl_cursor_box_warp(struct kwl_cursor_box *box, const struct kwl_cursor_geometry *geometry, kwl_cursor_box_move_fn *move, const void *context, struct kwl_cursor_motion *motion);
void kwl_cursor_box_outputs(struct kwl_cursor_box *box, const struct kwl_cursor_geometry *geometry, uint64_t *warp);
void kwl_cursor_box_read(struct kwl_cursor_box *box, int32_t *x, int32_t *y, unsigned *output);
void kwl_cursor_box_take_stats(struct kwl_cursor_box *box, struct kwl_cursor_stats *stats);
int32_t kwl_cursor_clamp(int64_t position, uint32_t size);
void kwl_cursor_geometry_place(const struct kwl_cursor_geometry *geometry, unsigned slot, unsigned direct, unsigned unit, const struct kwl_cursor_axis *across, const struct kwl_cursor_axis *down, int32_t *x, int32_t *y);

#endif
