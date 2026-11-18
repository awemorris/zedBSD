/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The overlay scroll bar (libkeiui's version 12, ws127-p002): the vertical bar a
 * view draws over its content's right edge, as macOS draws one (the user's
 * choice of 2026-10-02, "B の MacOS 風です").
 *
 * It shows thin while the content moves, thick with a faint track while
 * the pointer is near the edge or drags it, and fades out a while after
 * the last of these.  This file is the bar's state and geometry only: it
 * draws nothing and needs neither the window nor the canvas, so a program
 * with a canvas of its own (Files) uses it as the library's programs do
 * (kl_scroll_bar_draw in scroll.c draws it on a kl_canvas).
 */

#include <keiland/keiland.h>

#include <string.h>

/* How far a press on the track moves the content: this share of the viewport. */
#define SCROLL_BAR_PAGE		0.9

static double scroll_bar_limit(const struct kl_rect *viewport, double content);
static double scroll_bar_track(const struct kl_rect *viewport);
static double scroll_bar_thumb(const struct kl_rect *viewport, double content);
static double scroll_bar_alpha(const struct kl_scroll_bar *bar, uint64_t now_us);

/*
 * Tells the bar the content moved (a wheel, a key, a finger, a jump): it
 * comes out thin, or stays out.
 */
void
kl_scroll_bar_moved(
	struct kl_scroll_bar *bar,
	uint64_t now_us)
{
	/* The bar is active from now; the time zero is kept for "never". */
	bar->active_us = now_us;
	if (bar->active_us == 0U)
		bar->active_us = 1U;
}

/*
 * Tells the bar where the pointer is over the view: near the right edge
 * (within KL_SCROLL_BAR_REACH of it) of content that does not fit, the bar
 * comes out thick.  Returns 1 when the bar's look changed (draw again).
 */
int
kl_scroll_bar_hover(
	struct kl_scroll_bar *bar,
	const struct kl_rect *viewport,
	double content,
	double x,
	double y,
	uint64_t now_us)
{
	double limit;
	int near;

	/* Near: inside the view's height, within reach of its right edge, with something to scroll. */
	near = 0;
	limit = scroll_bar_limit(viewport, content);
	if (limit > 0.0 &&
	    y >= (double)viewport->y &&
	    y < (double)(viewport->y + viewport->height) &&
	    x < (double)(viewport->x + viewport->width) &&
	    x >= (double)(viewport->x + viewport->width - KL_SCROLL_BAR_REACH))
		near = 1;

	/* Nothing changes while the pointer stays where it was. */
	if (near == bar->near) {
		if (near != 0)
			kl_scroll_bar_moved(bar, now_us);
		return 0;
	}

	/* Coming near brings the bar out; going away starts its wait to fade. */
	bar->near = near;
	kl_scroll_bar_moved(bar, now_us);

	/* Succeeded: the bar grew or shrank. */
	return 1;
}

/*
 * Tells the bar the pointer left the view.  Returns 1 when its look
 * changed.
 */
int
kl_scroll_bar_leave(
	struct kl_scroll_bar *bar,
	uint64_t now_us)
{
	/* A pointer that was not near changes nothing. */
	if (bar->near == 0)
		return 0;

	/* The bar shrinks and waits to fade. */
	bar->near = 0;
	kl_scroll_bar_moved(bar, now_us);

	/* Succeeded: the bar shrank. */
	return 1;
}

/*
 * Works out what to draw of the bar of a view (viewport, in the window's
 * pixels) whose content is content pixels tall and scrolled by offset.
 * Returns 1 when something shows (shape filled), 0 when nothing does.
 */
int
kl_scroll_bar_shape(
	const struct kl_scroll_bar *bar,
	const struct kl_rect *viewport,
	double content,
	double offset,
	uint64_t now_us,
	struct kl_scroll_bar_shape *shape)
{
	double limit;
	double track;
	double thumb;
	double width;
	double alpha;

	/* Nothing yet. */
	memset(shape, 0, sizeof(*shape));

	/* Content that fits has no bar. */
	limit = scroll_bar_limit(viewport, content);
	if (limit <= 0.0)
		return 0;

	/* A bar that is not out (or has faded) shows nothing. */
	alpha = scroll_bar_alpha(bar, now_us);
	if (alpha <= 0.0)
		return 0;

	/* Thick while the pointer is near or holds it, thin otherwise. */
	width = (double)KL_SCROLL_BAR_THIN;
	if (bar->near != 0 || bar->dragging != 0) {
		width = (double)KL_SCROLL_BAR_THICK;
		shape->thick = 1;
	}

	/* The track along the right edge, the gap kept from the edges. */
	track = scroll_bar_track(viewport);
	shape->track_x = (double)(viewport->x + viewport->width - KL_SCROLL_BAR_GAP) - width;
	shape->track_y = (double)(viewport->y + KL_SCROLL_BAR_GAP);
	shape->track_width = width;
	shape->track_height = track;

	/* The thumb: as long as the share of the content that shows, placed by the offset. */
	thumb = scroll_bar_thumb(viewport, content);
	if (offset < 0.0)
		offset = 0.0;
	if (offset > limit)
		offset = limit;
	shape->thumb_x = shape->track_x;
	shape->thumb_y = shape->track_y + (track - thumb) * offset / limit;
	shape->thumb_width = width;
	shape->thumb_height = thumb;
	shape->alpha = alpha;

	/* Succeeded: the bar shows. */
	return 1;
}

/*
 * A press of the pointer's button at (x, y).  On the thumb it starts a
 * drag; on the track it moves the content a page towards the press
 * (new_offset).  Returns 1 when the bar took the press (the view must not
 * act on it), 0 when the press is not the bar's.
 */
int
kl_scroll_bar_press(
	struct kl_scroll_bar *bar,
	const struct kl_rect *viewport,
	double content,
	double offset,
	double x,
	double y,
	uint64_t now_us,
	double *new_offset)
{
	struct kl_scroll_bar_shape shape;
	double limit;
	double page;
	int shown;

	/* The offset stays where it is unless the press moves it. */
	*new_offset = offset;

	/* Only a bar that shows, near the pointer, takes a press. */
	shown = kl_scroll_bar_shape(bar, viewport, content, offset, now_us, &shape);
	if (shown == 0 || bar->near == 0)
		return 0;

	/* Only on the band of the edge. */
	if (x < (double)(viewport->x + viewport->width - KL_SCROLL_BAR_REACH))
		return 0;

	/* On the thumb: a drag that holds the thumb where it was pressed. */
	limit = scroll_bar_limit(viewport, content);
	if (y >= shape.thumb_y && y < shape.thumb_y + shape.thumb_height) {
		bar->dragging = 1;
		bar->grab = y - shape.thumb_y;
		kl_scroll_bar_moved(bar, now_us);
		return 1;
	}

	/* On the track above or below it: a page towards the press. */
	page = (double)viewport->height * SCROLL_BAR_PAGE;
	if (y < shape.thumb_y) {
		*new_offset = offset - page;
	} else {
		*new_offset = offset + page;
	}

	/* The new place stays within the content. */
	if (*new_offset < 0.0)
		*new_offset = 0.0;
	if (*new_offset > limit)
		*new_offset = limit;
	kl_scroll_bar_moved(bar, now_us);

	/* Succeeded: the press paged the content. */
	return 1;
}

/*
 * The pointer moved to y with the button held after a press on the thumb:
 * the content follows (new_offset).  Returns 1 while the bar drags, 0 when
 * no drag is on.
 */
int
kl_scroll_bar_drag(
	struct kl_scroll_bar *bar,
	const struct kl_rect *viewport,
	double content,
	double y,
	uint64_t now_us,
	double *new_offset)
{
	double limit;
	double track;
	double thumb;
	double top;

	/* Only a drag the thumb started. */
	*new_offset = 0.0;
	if (bar->dragging == 0)
		return 0;

	/* Content that fits any more stays at the top. */
	limit = scroll_bar_limit(viewport, content);
	if (limit <= 0.0)
		return 1;

	/* The thumb's top under the pointer, within the track, gives the offset. */
	track = scroll_bar_track(viewport);
	thumb = scroll_bar_thumb(viewport, content);
	top = y - bar->grab - (double)(viewport->y + KL_SCROLL_BAR_GAP);
	if (top < 0.0)
		top = 0.0;
	if (top > track - thumb)
		top = track - thumb;
	if (track - thumb > 0.0)
		*new_offset = top / (track - thumb) * limit;
	kl_scroll_bar_moved(bar, now_us);

	/* Succeeded: the content follows the thumb. */
	return 1;
}

/*
 * The button was let go.  Returns 1 when that ended a drag of the thumb.
 */
int
kl_scroll_bar_release(
	struct kl_scroll_bar *bar,
	uint64_t now_us)
{
	/* A release without a drag is not the bar's. */
	if (bar->dragging == 0)
		return 0;

	/* The drag ends; the bar waits to fade. */
	bar->dragging = 0;
	kl_scroll_bar_moved(bar, now_us);

	/* Succeeded: the drag ended. */
	return 1;
}

/*
 * Tells whether the bar still changes with time (it shows and has not
 * finished fading): the view should draw again soon.
 */
int
kl_scroll_bar_busy(
	const struct kl_scroll_bar *bar,
	uint64_t now_us)
{
	double alpha;

	/* A held or approached bar stays as it is until something happens. */
	if (bar->dragging != 0 || bar->near != 0)
		return 0;

	/* A bar that shows waits to fade, then fades. */
	alpha = scroll_bar_alpha(bar, now_us);
	if (alpha > 0.0)
		return 1;

	/* Nothing shows. */
	return 0;
}

/* Reports how far the content scrolls (0 when it fits). */
static double
scroll_bar_limit(
	const struct kl_rect *viewport,
	double content)
{
	/* Content that fits the view does not scroll. */
	if (content <= (double)viewport->height)
		return 0.0;

	/* Reports what does not show. */
	return content - (double)viewport->height;
}

/* Reports the track's length: the view's height less the gaps at its ends. */
static double
scroll_bar_track(
	const struct kl_rect *viewport)
{
	double track;

	/* The gaps at both ends, never below a pixel. */
	track = (double)viewport->height - 2.0 * (double)KL_SCROLL_BAR_GAP;
	if (track < 1.0)
		track = 1.0;

	/* Reports the length. */
	return track;
}

/* Reports the thumb's length: the share of the content that shows, not shorter than KL_SCROLL_BAR_MIN. */
static double
scroll_bar_thumb(
	const struct kl_rect *viewport,
	double content)
{
	double track;
	double thumb;

	/* The share of the track the view's share of the content gives. */
	track = scroll_bar_track(viewport);
	thumb = track;
	if (content > 0.0)
		thumb = track * (double)viewport->height / content;

	/* Long enough to be held, and never longer than the track. */
	if (thumb < (double)KL_SCROLL_BAR_MIN)
		thumb = (double)KL_SCROLL_BAR_MIN;
	if (thumb > track)
		thumb = track;

	/* Reports the length. */
	return thumb;
}

/* Reports the strength of the bar's ink now: full while out, fading after KL_SCROLL_BAR_SHOW_US, 0 when gone. */
static double
scroll_bar_alpha(
	const struct kl_scroll_bar *bar,
	uint64_t now_us)
{
	uint64_t since;

	/* Held or approached: full. */
	if (bar->dragging != 0 || bar->near != 0)
		return 1.0;

	/* Never out, or a time from before it came out: nothing. */
	if (bar->active_us == 0U || now_us < bar->active_us)
		return 0.0;

	/* Full for a while after it came out. */
	since = now_us - bar->active_us;
	if (since < (uint64_t)KL_SCROLL_BAR_SHOW_US)
		return 1.0;

	/* Gone after the fade. */
	since -= (uint64_t)KL_SCROLL_BAR_SHOW_US;
	if (since >= (uint64_t)KL_SCROLL_BAR_FADE_US)
		return 0.0;

	/* Reports the share of the fade left. */
	return 1.0 - (double)since / (double)KL_SCROLL_BAR_FADE_US;
}
