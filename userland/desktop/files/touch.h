/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch screen of Files (ws081-p010, plan/ws081/design.md section 5).
 *
 * Over the items or the sidebar, a finger (or two) drags to scroll, and a
 * flick glides on (libkeiland's scroller; past either end it stretches and
 * springs back); a touch catches it.  A tap is a click of the left button
 * (two quick taps open, as a double click does); a long press is the right
 * button's click (the context menu) when the finger lifts without moving,
 * and a press of the left button held where it pressed when the finger
 * then moves (dragging the items -- a drag and drop with the finger,
 * ws081-p014 -- or a rubber band).  Anywhere else (the toolbar, the tabs,
 * the fields, a dialog) the first finger is the left button: it presses
 * where it touches, moves the pointer and releases where it lifts.
 *
 * Nothing here speaks Wayland: the window queues the wl_touch events, the
 * main loop hands them here with what is under a finger and the scrolls
 * each round, and takes the scrolls and the pointer's events made here.
 */

#ifndef FM_TOUCH_H
#define FM_TOUCH_H

#include <stdint.h>

#include <keiland/keiland.h>

/* The kinds of touch input the window queues. */
#define FM_TOUCH_DOWN		0U
#define FM_TOUCH_MOTION		1U
#define FM_TOUCH_UP		2U
#define FM_TOUCH_CANCEL		3U

/*
 * A touch pad's two fingers (ws090-p019): a move of the scroll (y as a
 * wheel scrolls, surface pixels; the area under the pointer) and their
 * lift, which libkeiland's scroller turns into the same flight as a
 * finger's on the screen.
 */
#define FM_TOUCH_PAD		4U
#define FM_TOUCH_PAD_STOP	5U

/* What is under a finger: something that is not scrolled, the items (the content), the sidebar. */
#define FM_TOUCH_OTHER		0U
#define FM_TOUCH_CONTENT	1U
#define FM_TOUCH_SIDEBAR	2U
#define FM_TOUCH_AREAS		3U

/* The kinds of pointer event the fingers make: a motion, a button pressed or released. */
#define FM_TOUCH_POINTER_MOTION		0U
#define FM_TOUCH_POINTER_PRESS		1U
#define FM_TOUCH_POINTER_RELEASE	2U

/* The buttons the fingers press. */
#define FM_TOUCH_LEFT		0U
#define FM_TOUCH_RIGHT		1U

/* How many pointer events the fingers make before the main loop takes them, at most. */
#define FM_TOUCH_POINTERS	32U

/*
 * One touch input: its kind (FM_TOUCH_*), the finger (wl_touch's id),
 * where in the window (surface pixels; not for UP and CANCEL), the
 * compositor's time (milliseconds of CLOCK_MONOTONIC, the low 32 bits; not
 * for CANCEL), a down's serial, what is under a down (FM_TOUCH_OTHER,
 * _CONTENT or _SIDEBAR, which the main loop finds), and when the window
 * read it (microseconds of the same clock, fm_touch_clock).
 */
struct fm_touch_event {
	unsigned type;
	int32_t id;
	float x;
	float y;
	uint32_t time;
	uint32_t serial;
	unsigned area;
	uint64_t arrival;
};

/*
 * One pointer event the fingers make: its kind (FM_TOUCH_POINTER_*), the
 * button (FM_TOUCH_LEFT or _RIGHT), where (surface pixels), its time
 * (milliseconds) and the serial of the touch that made it.
 */
struct fm_touch_pointer {
	unsigned kind;
	unsigned button;
	int32_t x;
	int32_t y;
	uint32_t time;
	uint32_t serial;
};

/*
 * One scrolled area as the main loop has it: a token that changes with
 * what it shows (another tab or folder), its scroll (pixels from the top,
 * positive down), how far it scrolls at most, and its height.
 */
struct fm_touch_area {
	const void *token;
	int scroll;
	int largest;
	int height;
};

/*
 * The fingers and the areas they scroll.
 *
 * areas are the scrolled areas as the main loop last gave them (index by
 * FM_TOUCH_CONTENT and _SIDEBAR), and target the one the scroller moves (0
 * for none); scroll is what the fingers last set it to, changed that the
 * main loop has not taken it.  A scroll set elsewhere (the wheel, a key,
 * another folder) is taken over and stops a glide.
 *
 * mode is what the first finger does: nothing yet (TOUCH_MODE_*, in
 * touch.c), play the left button, or its gestures.  pressed says the
 * scroller holds the touch, moving that it owns the target's scroll (from
 * a touch until it rests), dragging that the drag scrolls, held that a long
 * press waits to see whether the finger moves, holding that it moved and
 * the left button is held for it, and caught that the touch caught a glide;
 * first_id is the first finger, last_* its last place, press_* the long
 * press's place, serial the first finger's down serial, base_* the drag's
 * offset when the scroller was last pressed, and repress that the scroll
 * was taken over under a finger and the next tick presses the scroller
 * again.
 */
struct fm_touch {
	struct kl_gesture *gesture;
	struct kl_scroller *scroller;
	struct fm_touch_area areas[FM_TOUCH_AREAS];
	unsigned target;
	int scroll;
	int changed;
	unsigned followed;
	unsigned mode;
	int pressed;
	int moving;
	int dragging;
	int held;
	int holding;
	int caught;
	int32_t first_id;
	double last_x;
	double last_y;
	double press_x;
	double press_y;
	uint32_t serial;
	double base_x;
	double base_y;
	int repress;
	double bounds_largest;
	double bounds_height;

	/* The pointer events made and not yet taken, oldest first. */
	struct fm_touch_pointer pointers[FM_TOUCH_POINTERS];
	unsigned pointer_count;
};

/* The touch screen (touch.c). */
int fm_touch_open(struct fm_touch *touch);
void fm_touch_close(struct fm_touch *touch);
void fm_touch_layout(struct fm_touch *touch, unsigned area, const struct fm_touch_area *state);
void fm_touch_event(struct fm_touch *touch, const struct fm_touch_event *event);
int fm_touch_tick(struct fm_touch *touch, uint64_t now);
int fm_touch_scroll(struct fm_touch *touch, unsigned *area, int *scroll);
int fm_touch_take_pointer(struct fm_touch *touch, struct fm_touch_pointer *pointer);
uint64_t fm_touch_clock(void);

#endif
