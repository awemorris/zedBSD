/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch screen of Notes (ws081-p013, plan/ws081/design.md section 5):
 * the pen (and the pointer) writes, the fingers move the page.  One finger
 * scrolls a page zoomed past the window, gliding on after a flick and
 * stretching past its edges (libkeiland's kl_scroll); two fingers zoom
 * about the place between them; a double tap zooms in twice about the
 * tapped place, or back to the whole page; a tap on the toolbar presses its
 * button.
 *
 * While writing with a finger is on (the toolbar's Finger, ws081-p015),
 * one finger on the page writes instead, as the pointer does, and two
 * fingers scroll and zoom: a second finger that comes while the first has
 * only just begun (NOTES_TOUCH_WRITE_GRACE_MS, NOTES_TOUCH_WRITE_SLOP)
 * takes the line back and the two scroll and zoom; a later one is left
 * alone.  Off, a finger never writes.
 *
 * A palm resting on the screen while the pen writes is not a finger: the
 * touch screen lifts a contact it does not trust (HID Confidence 0, the
 * kernel's touch state machine), and Notes takes no new finger while the
 * pen is near the window and for NOTES_TOUCH_PALM_MS after it leaves; a
 * finger already down when the pen comes near is cancelled, and every
 * finger down then is left alone until it lifts.
 *
 * Nothing here speaks Wayland or Vulkan: the window queues the wl_touch
 * events, the main loop hands them here, tells when the pen comes and
 * goes, and asks where the page is for each frame.
 */

#ifndef NOTES_TOUCH_H
#define NOTES_TOUCH_H

#include <stdint.h>

#include <keiland/keiland.h>

/* The kinds of touch input the window queues. */
#define NOTES_TOUCH_DOWN	0U
#define NOTES_TOUCH_MOTION	1U
#define NOTES_TOUCH_UP		2U
#define NOTES_TOUCH_CANCEL	3U

/* How many fingers Notes follows at once (the rest are left alone). */
#define NOTES_TOUCH_FINGERS	10U

/* How long after the pen leaves the window a new finger is still taken for a palm, in milliseconds. */
#define NOTES_TOUCH_PALM_MS	500U

/* How far in the fingers may zoom, at most, as a multiple of the whole page's scale. */
#define NOTES_TOUCH_ZOOM_MAX	4.0f

/* The largest page picture the zoom may ask for, in pixels on its longer side. */
#define NOTES_TOUCH_PICTURE_MAX	4096.0f

/* How many toolbar taps wait for the main loop at most. */
#define NOTES_TOUCH_TAPS	4U

/* The kinds of a writing finger's events: it touches, moves, lifts, or its line is taken back. */
#define NOTES_TOUCH_WRITE_BEGIN		0U
#define NOTES_TOUCH_WRITE_MOTION	1U
#define NOTES_TOUCH_WRITE_END		2U
#define NOTES_TOUCH_WRITE_ABORT		3U

/* How many of a writing finger's events wait for the main loop at most. */
#define NOTES_TOUCH_WRITES	256U

/* How long after a writing finger touched a second finger still takes its line back, in milliseconds. */
#define NOTES_TOUCH_WRITE_GRACE_MS	250U

/* How far a writing finger may have moved and a second finger still take its line back, in pixels. */
#define NOTES_TOUCH_WRITE_SLOP		12.0f

/*
 * One touch input: its kind (NOTES_TOUCH_*), the finger (wl_touch's id),
 * where in the window (surface pixels; not for UP and CANCEL), the
 * compositor's time (milliseconds of CLOCK_MONOTONIC, the low 32 bits; not
 * for CANCEL), and when the window read it (microseconds of the same
 * clock, notes_touch_clock).
 */
struct notes_touch_event {
	unsigned type;
	int32_t id;
	float x;
	float y;
	uint32_t time;
	uint64_t arrival;
};

/*
 * One event of a writing finger: its kind (NOTES_TOUCH_WRITE_*), where in
 * the window (surface pixels) and when (milliseconds of CLOCK_MONOTONIC,
 * the low 32 bits, as the pointer's events carry).
 */
struct notes_touch_write {
	unsigned kind;
	float x;
	float y;
	uint32_t time_ms;
};

/*
 * One finger down: its id, and whether the gestures follow it (a finger
 * taken for a palm is not followed).
 */
struct notes_touch_finger {
	int32_t id;
	int used;
	int followed;
};

/*
 * The fingers and the page they move.
 *
 * The layout (notes_touch_layout) gives the window's size, the toolbar's
 * band, the margin, the page's size in points and the scale at which the
 * whole page fits; zoom multiplies that scale, and the scroll (kl_scroll,
 * ws090-p015, its ends and rubber band set for the zoomed page and the
 * room) holds the page's scroll (pixels, from its top left edge less the
 * margin) on the axes where the zoomed page is larger than the room
 * (scroll_* as it last gave it).  view_* is where the page is drawn (its top left in pixels, and
 * pixels per point).
 *
 * pen_near says the pen is over the window, pen_left_us when it last left.
 * pressed says the scroll holds the fingers' touch, moving that it owns
 * the page's place (from a touch until the page rests), dragging that the
 * fingers' drag scrolls, and caught that the touch caught a gliding page;
 * toolbar that the first finger touched the toolbar (it only taps), base_*
 * the drag's offset when the scroll was last pressed.  While two fingers
 * zoom (pinching), anchor_* is the page's point, in points, that stays
 * between them, pinch_zoom the zoom and pinch_ratio their distance's ratio
 * when the zoom began; zooming is what the frame asks (the page's picture
 * may be stretched rather than drawn again).  bounds_* are the scroll's
 * ends and room as last set.
 *
 * write_mode says one finger writes (the toolbar's Finger); writing that a
 * finger writes now: writer_id is that finger, writer_down_us when it
 * touched, writer_x and writer_y its last place, writer_time_us the time
 * of its last event, and writer_path how far it has moved in all (pixels).
 * writes are its events not yet taken, oldest first.
 */
struct notes_touch {
	struct kl_gesture *gesture;
	struct kl_scroll scroll;
	struct notes_touch_finger fingers[NOTES_TOUCH_FINGERS];
	unsigned followed;

	/* The pen, for telling a palm from a finger. */
	int pen_near;
	uint64_t pen_left_us;

	/* The fingers' touch and drag. */
	int pressed;
	int moving;
	int dragging;
	int caught;
	int toolbar;
	double base_x;
	double base_y;

	/* The zoom, and two fingers' zoom under way. */
	float zoom;
	int pinching;
	int zooming;
	float anchor_x;
	float anchor_y;
	float pinch_zoom;
	double pinch_ratio;

	/* The toolbar taps not yet taken, oldest first. */
	float tap_x[NOTES_TOUCH_TAPS];
	float tap_y[NOTES_TOUCH_TAPS];
	unsigned tap_count;

	/* Writing with a finger. */
	int write_mode;
	int writing;
	int32_t writer_id;
	uint64_t writer_down_us;
	uint64_t writer_time_us;
	float writer_x;
	float writer_y;
	float writer_path;
	struct notes_touch_write writes[NOTES_TOUCH_WRITES];
	unsigned write_count;

	/* The layout. */
	float width;
	float height;
	float top;
	float margin;
	float page_width;
	float page_height;
	float fit_scale;

	/* The page's scroll as the scroll last gave it, and where the page is drawn. */
	double scroll_x;
	double scroll_y;
	float view_x;
	float view_y;
	float view_scale;

	/* The scroll's ends and room as last set. */
	double bounds_x;
	double bounds_y;
	double bounds_width;
	double bounds_height;
};

/* The touch screen (touch.c). */
int notes_touch_open(struct notes_touch *touch);
void notes_touch_close(struct notes_touch *touch);
void notes_touch_layout(struct notes_touch *touch, uint32_t width, uint32_t height, float top, float margin, float page_width, float page_height, float fit_scale);
void notes_touch_event(struct notes_touch *touch, const struct notes_touch_event *event);
void notes_touch_pen(struct notes_touch *touch, int near, uint64_t now);
int notes_touch_tick(struct notes_touch *touch, uint64_t now);
void notes_touch_view(const struct notes_touch *touch, float *x, float *y, float *scale);
int notes_touch_take_tap(struct notes_touch *touch, float *x, float *y);
void notes_touch_write_mode(struct notes_touch *touch, int on, uint64_t now);
int notes_touch_take_write(struct notes_touch *touch, struct notes_touch_write *write);
void notes_touch_top(struct notes_touch *touch);
uint64_t notes_touch_clock(void);

#endif
