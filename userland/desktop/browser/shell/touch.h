/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch screen of the browser's window (ws081-p006,
 * plan/ws081/design.md section 5).
 *
 * A finger (or two, by their centroid) drags the page, and a flick glides
 * on (libkeiland's scroller): past either end of the document the content
 * stretches and springs back (the view's overscroll); a touch catches a
 * glide.  A tap is a click of the primary button (two quick taps are two
 * clicks, which the page reads as a double click); a long press lifted
 * without moving is a click of the secondary button.  A scroll placed
 * otherwise (the wheel, a key, a link, another page) is taken over: a
 * glide stops there, and a finger down drags on from it.
 *
 * Nothing here speaks Wayland or the view: the window queues the wl_touch
 * events, the main loop hands them here with the page's scroll and range
 * each round, and takes the scroll, the overscroll and the pointer's
 * events made here.
 */

#ifndef KEILAND_BROWSER_SHELL_TOUCH_H
#define KEILAND_BROWSER_SHELL_TOUCH_H

#include <stdint.h>

#include <keiland/keiland.h>

/* The kinds of touch input the window queues. */
#define SHELL_TOUCH_DOWN	0U
#define SHELL_TOUCH_MOTION	1U
#define SHELL_TOUCH_UP		2U
#define SHELL_TOUCH_CANCEL	3U

/*
 * A touch pad's two fingers (ws090-p019): a move of the scroll (y as a
 * wheel scrolls, surface pixels) and their lift, which libkeiland's
 * scroller turns into the same flight as a finger's on the screen.
 */
#define SHELL_TOUCH_PAD		4U
#define SHELL_TOUCH_PAD_STOP	5U

/* The kinds of pointer event the fingers make: a motion, a button pressed or released. */
#define SHELL_TOUCH_POINTER_MOTION	0U
#define SHELL_TOUCH_POINTER_PRESS	1U
#define SHELL_TOUCH_POINTER_RELEASE	2U

/* The buttons the fingers press (the DOM's numbers, <browser/browser.h>'s BROWSER_BUTTON_PRIMARY and _SECONDARY). */
#define SHELL_TOUCH_PRIMARY	0
#define SHELL_TOUCH_SECONDARY	2

/* How many pointer events the fingers make before the main loop takes them, at most. */
#define SHELL_TOUCH_POINTERS	32U

/*
 * One touch input: its kind (SHELL_TOUCH_*), the finger (wl_touch's id),
 * where in the window (surface pixels; not for UP and CANCEL), the
 * compositor's time (milliseconds of CLOCK_MONOTONIC, the low 32 bits; not
 * for CANCEL), and when the window read it (microseconds of the same
 * clock, shell_touch_clock).
 */
struct shell_touch_event {
	unsigned type;
	int32_t id;
	float x;
	float y;
	uint32_t time;
	uint64_t arrival;
};

/*
 * One pointer event the fingers make: its kind (SHELL_TOUCH_POINTER_*),
 * the button (SHELL_TOUCH_PRIMARY or _SECONDARY) and where (surface
 * pixels).
 */
struct shell_touch_pointer {
	unsigned kind;
	int button;
	float x;
	float y;
};

/*
 * The fingers and the page's scroll they move.
 *
 * token is the page the scroll belongs to (another page is taken over as a
 * scroll set elsewhere), largest how far it scrolls and height the view's
 * (bounds_* as the scroller last got them).  scroll is where the fingers
 * last put the page (pixels, within the document) and overscroll how far
 * past an end they stretch it (pixels, positive down: the content moved
 * down past the top); changed says the main loop has not taken them.
 *
 * pressed says the scroller holds the touch, moving that it owns the
 * scroll (from a touch until it rests), dragging that the drag scrolls,
 * held that a long press waits for its lift, and caught that the touch
 * caught a glide; followed is how many fingers the gestures follow,
 * base_* the drag's offset when the scroller was last pressed, and
 * repress that the scroll was taken over under a finger and the next tick
 * presses the scroller again.
 */
struct shell_touch {
	struct kl_gesture *gesture;
	struct kl_scroller *scroller;

	/* The page as the main loop last gave it. */
	unsigned long token;
	double largest;
	double height;
	double bounds_largest;
	double bounds_height;

	/* What the fingers set, not yet taken. */
	double scroll;
	double overscroll;
	int changed;

	/* The fingers' touch and drag. */
	unsigned followed;
	int pressed;
	int moving;
	int dragging;
	int held;
	int caught;
	float press_x;
	float press_y;
	double base_x;
	double base_y;
	int repress;

	/* The pointer events made and not yet taken, oldest first. */
	struct shell_touch_pointer pointers[SHELL_TOUCH_POINTERS];
	unsigned pointer_count;
};

/* The touch screen (touch.c). */
int shell_touch_open(struct shell_touch *touch);
void shell_touch_close(struct shell_touch *touch);
void shell_touch_layout(struct shell_touch *touch, unsigned long token, double scroll, double largest, double height);
void shell_touch_event(struct shell_touch *touch, const struct shell_touch_event *event);
int shell_touch_tick(struct shell_touch *touch, uint64_t now);
int shell_touch_scroll(struct shell_touch *touch, double *scroll, double *overscroll);
int shell_touch_take_pointer(struct shell_touch *touch, struct shell_touch_pointer *pointer);
uint64_t shell_touch_clock(void);

#endif
