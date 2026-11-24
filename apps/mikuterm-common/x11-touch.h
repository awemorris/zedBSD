/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pure state machine that turns touch contacts on an X11 tab into
 * pointer motion, button clicks, and wheel notches.
 */

#ifndef MIKUTERM_X11_TOUCH_H
#define MIKUTERM_X11_TOUCH_H

#include <stdint.h>

/* Input touch event kinds. */
#define X11_TOUCH_DOWN		0U
#define X11_TOUCH_MOVE		1U
#define X11_TOUCH_UP		2U
#define X11_TOUCH_CANCEL	3U

/* Touch gesture modes: per-window tab (wheel) vs rooted server tab (pan/pinch). */
#define X11_TOUCH_MODE_WINDOW	0U
#define X11_TOUCH_MODE_ROOT	1U

/* Output pointer action kinds. */
#define X11_TOUCH_MOTION	1U
#define X11_TOUCH_BUTTON	2U
#define X11_TOUCH_WHEEL		3U
#define X11_TOUCH_PAN		4U
#define X11_TOUCH_PINCH		5U

/*
 * One pointer action produced by x11_touch_take: motion to (x, y),
 * a button press or release, whole wheel notches, or root pan/pinch.
 */
struct x11_touch_action {
	unsigned kind;
	unsigned button;
	int pressed;
	int32_t x;
	int32_t y;
	int notches;
	int32_t dx;
	int32_t dy;
	double scale_ratio;
};

/*
 * Per-tab touch gesture state: tracks up to two active contacts,
 * single-finger drag threshold, and two-finger scroll/pan/pinch/right-tap state.
 */
struct x11_touch {
	unsigned mode;
	int active[2];
	int32_t id[2];
	int32_t start_x[2];
	int32_t start_y[2];
	int32_t x[2];
	int32_t y[2];
	int pressed_left;
	int two_finger;
	int two_done;
	int moved_far;
	int two_pinch_active;
	int32_t two_last_avg_x;
	int32_t two_last_avg_y;
	int32_t wheel_accum_y;
	double two_pinch_dist;
	uint64_t two_start_time_us;
	int32_t last_x;
	int32_t last_y;
};

unsigned x11_touch_take(struct x11_touch *touch, unsigned kind, int32_t id, int32_t x, int32_t y, uint64_t time_us, struct x11_touch_action *out, unsigned room);

#endif
