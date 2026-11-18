/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scroll of a part of a window, and the overlay scroll bar, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_SCROLL_H
#define KEILAND_SCROLL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kl_canvas;
struct kl_rect;
struct kl_theme;

/*
 * The scroll (scroll.c): the state of one part of a window whose content is
 * larger than the part, which an application keeps and draws its content at
 * (x, y) of.
 *
 * The wheel and the keys glide the content to where they send it (the
 * distance left shrinks by e every KL_SCROLL_GLIDE_US); a finger drags it
 * and lets it fly on with libkeiland's scroller (the inertia and the
 * rubber band past an end are the scroller's).  Outside a finger's hold
 * the position stays within 0..content-viewport on each axis that
 * scrolls.  The scroll bars show while the content moves and fade out
 * over KL_SCROLL_FADE_US after it stops.
 *
 * A program whose ends are not 0..content-viewport (Terminal's scrollback
 * runs to a negative position, KL_VERSION 61) gives them and the rubber
 * band's size with kl_scroll_set_bounds; kl_scroll_set_size goes back to
 * the ends of the sizes.  A program that moves its view by other means
 * (keys, new output) hands the place over with kl_scroll_move_to without a
 * glide, and the next finger drags on from there.
 *
 * The fields are read by the application (x and y above all); they are
 * written only through the calls.  Nothing here draws but
 * kl_scroll_draw_bars, so a program that draws its own content with
 * Vulkan (Terminal, Notes) uses the same scroll without the canvas.
 */
struct kl_scroller;

/* The axes a scroll moves along. */
#define KL_SCROLL_X		1U
#define KL_SCROLL_Y		2U

/* How quickly a glide closes on its target (the time constant), and how long the bars take to fade. */
#define KL_SCROLL_GLIDE_US	70000U
#define KL_SCROLL_FADE_US	1000000U

/*
 * The track of a touch pad's two-finger scrolling (KL_VERSION 40): its last
 * moves and their times, from which the velocity is worked out when the
 * fingers lift, so that the content flies on.  The samples older than
 * KL_AXIS_TRACK_WINDOW_US at the lift do not count, and fingers that rested
 * longer than KL_AXIS_TRACK_REST_US before lifting throw nothing.  It is
 * plain data a caller keeps; kl_scroller keeps one for a touch pad's fingers
 * (KL_VERSION 41), so a program needs none of its own.
 */
#define KL_AXIS_TRACK_SAMPLES	16U
#define KL_AXIS_TRACK_WINDOW_US	100000U
#define KL_AXIS_TRACK_REST_US	60000U

struct kl_axis_track {
	unsigned count;
	unsigned next;
	double dx[KL_AXIS_TRACK_SAMPLES];
	double dy[KL_AXIS_TRACK_SAMPLES];
	uint64_t us[KL_AXIS_TRACK_SAMPLES];
};

void kl_axis_track_reset(struct kl_axis_track *track);
void kl_axis_track_add(struct kl_axis_track *track, double dx, double dy, uint64_t now_us);
void kl_axis_track_velocity(const struct kl_axis_track *track, uint64_t now_us, double *vx, double *vy);

struct kl_scroll {
	/* The axes it moves along, the position drawn, and the sizes of the content and of the part that shows it. */
	unsigned axes;
	double x;
	double y;
	double content_width;
	double content_height;
	double viewport_width;
	double viewport_height;

	/* A glide of the wheel or the keys: where it started, where it goes, and when it started. */
	int gliding;
	double from_x;
	double from_y;
	double to_x;
	double to_y;
	uint64_t glide_us;

	/* The finger's scroller, whether it owns the content (a finger holds it or it flies on), and whether the finger has lifted. */
	struct kl_scroller *scroller;
	int touched;
	int released;

	/* When the content last moved (for the bars), 0 before it ever moved. */
	uint64_t moved_us;

	/*
	 * The ends kl_scroll_set_bounds gave (bounded is 1 from then until
	 * kl_scroll_set_size), in place of 0..content-viewport, and the size
	 * the rubber band past them is measured against (KL_VERSION 61).
	 */
	int bounded;
	double minimum_x;
	double maximum_x;
	double minimum_y;
	double maximum_y;
	double band_width;
	double band_height;
};

int kl_scroll_init(struct kl_scroll *scroll, unsigned axes);
void kl_scroll_release(struct kl_scroll *scroll);
void kl_scroll_set_size(struct kl_scroll *scroll, double content_width, double content_height, double viewport_width, double viewport_height);
int kl_scroll_set_bounds(struct kl_scroll *scroll, double minimum_x, double maximum_x, double minimum_y, double maximum_y, double band_width, double band_height);
void kl_scroll_wheel(struct kl_scroll *scroll, double dx, double dy, uint64_t now_us);
void kl_scroll_move_to(struct kl_scroll *scroll, double x, double y, int glide, uint64_t now_us);
void kl_scroll_reveal(struct kl_scroll *scroll, const struct kl_rect *rect, uint64_t now_us);
int kl_scroll_key(struct kl_scroll *scroll, uint32_t key, unsigned modifiers, double line, uint64_t now_us);
int kl_scroll_press(struct kl_scroll *scroll, uint64_t now_us);
void kl_scroll_drag(struct kl_scroll *scroll, double dx, double dy);
int kl_scroll_fling(struct kl_scroll *scroll, double vx, double vy, uint64_t now_us);
void kl_scroll_cancel(struct kl_scroll *scroll, uint64_t now_us);
void kl_scroll_axis(struct kl_scroll *scroll, double dx, double dy, unsigned source, uint64_t now_us);
int kl_scroll_axis_stop(struct kl_scroll *scroll, uint64_t now_us);
int kl_scroll_axis_at(struct kl_scroll *scroll, double dx, double dy, unsigned source, uint64_t event_us, uint64_t now_us);
int kl_scroll_axis_stop_at(struct kl_scroll *scroll, uint64_t event_us, uint64_t now_us, double *vx, double *vy);
int kl_scroll_axis_holding(const struct kl_scroll *scroll);
int kl_scroll_step(struct kl_scroll *scroll, uint64_t now_us);
double kl_scroll_limit_x(const struct kl_scroll *scroll);
double kl_scroll_limit_y(const struct kl_scroll *scroll);
int kl_scroll_draw_bars(const struct kl_scroll *scroll, struct kl_canvas *canvas, const struct kl_rect *viewport, const struct kl_theme *theme, uint64_t now_us);

/*
 * The overlay scroll bar (scroll-bar.c): the vertical bar of a view, drawn
 * over its content's right edge the way macOS draws one.  It comes out thin
 * while the content moves, grows thick (with a faint track) while the pointer
 * is near the edge or drags it, and fades a while after the last of these.  A
 * press on the thumb drags the content; a press on the track moves it a page
 * towards the press.
 *
 * The state knows nothing of the window or of drawing: the application
 * tells it what happened (its sizes are in the content's pixels, offset
 * is how far the content is scrolled), asks for the shape to draw with its
 * own canvas (kl_scroll_bar_draw draws it on a kl_canvas), and draws
 * again while kl_scroll_bar_busy says the bar still changes.  The times
 * are microseconds of one clock.
 */
#define KL_SCROLL_BAR_THIN	6
#define KL_SCROLL_BAR_THICK	11
#define KL_SCROLL_BAR_REACH	16
#define KL_SCROLL_BAR_GAP	2
#define KL_SCROLL_BAR_MIN	28
#define KL_SCROLL_BAR_SHOW_US	1000000U
#define KL_SCROLL_BAR_FADE_US	400000U

/*
 * The state of one overlay bar.  active_us is when the content last moved
 * or the pointer last came near or dragged (0 before any); near says the
 * pointer is over the bar's band; dragging and grab (where in the thumb the
 * drag holds it) belong to a press on the thumb.  Zeroed, it is a bar that
 * has not shown yet.
 */
struct kl_scroll_bar {
	uint64_t active_us;
	int near;
	int dragging;
	double grab;
};

/*
 * What to draw now: the track (shown while the bar is thick) and the
 * thumb, in the window's pixels, and the strength of the ink (0 to 1).
 */
struct kl_scroll_bar_shape {
	double track_x;
	double track_y;
	double track_width;
	double track_height;
	double thumb_x;
	double thumb_y;
	double thumb_width;
	double thumb_height;
	double alpha;
	int thick;
};

void kl_scroll_bar_moved(struct kl_scroll_bar *bar, uint64_t now_us);
int kl_scroll_bar_hover(struct kl_scroll_bar *bar, const struct kl_rect *viewport, double content, double x, double y, uint64_t now_us);
int kl_scroll_bar_leave(struct kl_scroll_bar *bar, uint64_t now_us);
int kl_scroll_bar_shape(const struct kl_scroll_bar *bar, const struct kl_rect *viewport, double content, double offset, uint64_t now_us, struct kl_scroll_bar_shape *shape);
int kl_scroll_bar_press(struct kl_scroll_bar *bar, const struct kl_rect *viewport, double content, double offset, double x, double y, uint64_t now_us, double *new_offset);
int kl_scroll_bar_drag(struct kl_scroll_bar *bar, const struct kl_rect *viewport, double content, double y, uint64_t now_us, double *new_offset);
int kl_scroll_bar_release(struct kl_scroll_bar *bar, uint64_t now_us);
int kl_scroll_bar_busy(const struct kl_scroll_bar *bar, uint64_t now_us);
int kl_scroll_bar_draw(const struct kl_scroll_bar *bar, struct kl_canvas *canvas, const struct kl_rect *viewport, double content, double offset, uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif
