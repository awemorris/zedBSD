/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The content's overlay scroll bar (ws127-p002): the macOS-style bar the
 * user chose on 2026-10-02 for the icons and the list, the shared part of
 * libkeiland (kl_scroll_bar, scroll-bar.c) drawn with files' own canvas.
 *
 * It comes out thin when the content scrolls, thick near the window's
 * right edge, can be dragged and paged, and fades after a while; a frame
 * is asked for while it fades (fm_scrollbar_busy).  The bar works on the
 * content's card (app->layout.content), whose height and scroll are the
 * layout's and the tab's.
 */

#include "files.h"

#include <keiland/keiland.h>
#include <time.h>

/*
 * The content's bar.  One window has one content view, so the bar lives
 * as long as the program; a new tab or folder only moves it (its scroll
 * is that tab's), and a zero state is a bar that has not shown.
 */
static struct kl_scroll_bar scrollbar_content;

static void scrollbar_viewport(const struct fm_app *app, struct kl_rect *viewport);
static uint64_t scrollbar_now(void);
static void scrollbar_set(struct fm_app *app, double offset);

/*
 * Tells the bar the content scrolled (the wheel, a key, the fingers): it
 * comes out.
 */
void
fm_scrollbar_moved(
	struct fm_app *app)
{
	/* The bar shows from now on, and a frame shows it. */
	kl_scroll_bar_moved(&scrollbar_content, scrollbar_now());
	app->dirty = 1;
}

/*
 * Follows the pointer to (x, y): a drag of the thumb moves the content,
 * and the pointer near the right edge makes the bar thick.  Returns 1 when
 * the motion was the bar's drag (nothing else is to act on it).
 */
int
fm_scrollbar_motion(
	struct fm_app *app,
	int x,
	int y)
{
	struct kl_rect viewport;
	double offset;
	int dragged;
	int changed;

	/* The content's card, and its height to scroll through. */
	scrollbar_viewport(app, &viewport);

	/* A drag of the thumb: the content follows it. */
	dragged = kl_scroll_bar_drag(&scrollbar_content, &viewport, (double)app->layout.content_height, (double)y, scrollbar_now(), &offset);
	if (dragged != 0) {
		scrollbar_set(app, offset);
		return 1;
	}

	/* Near the edge or away from it: a frame when the bar grows or shrinks. */
	changed = kl_scroll_bar_hover(&scrollbar_content, &viewport, (double)app->layout.content_height, (double)x, (double)y, scrollbar_now());
	if (changed != 0)
		app->dirty = 1;

	/* Not the bar's: the motion goes on to the content. */
	return 0;
}

/*
 * A press of the left button at (x, y): on the bar's thumb it starts a
 * drag, on its track it pages the content.  Returns 1 when the bar took
 * the press.
 */
int
fm_scrollbar_press(
	struct fm_app *app,
	int x,
	int y)
{
	struct kl_rect viewport;
	struct fm_tab *tab;
	double offset;
	int taken;

	/* The content's card and where it is scrolled to. */
	scrollbar_viewport(app, &viewport);
	tab = fm_ui_tab(app);

	/* The bar's press, which may page the content. */
	taken = kl_scroll_bar_press(&scrollbar_content, &viewport, (double)app->layout.content_height, (double)tab->scroll, (double)x, (double)y, scrollbar_now(), &offset);
	if (taken == 0)
		return 0;

	/* The content where the press sent it, and a frame for the bar. */
	scrollbar_set(app, offset);
	fm_log("SCROLLBAR press y=%d scroll=%d dragging=%d", y, tab->scroll, scrollbar_content.dragging);

	/* Succeeded: the press was the bar's. */
	return 1;
}

/*
 * The left button let go.  Returns 1 when that ended a drag of the
 * thumb.
 */
int
fm_scrollbar_release(
	struct fm_app *app)
{
	int ended;

	/* A drag of the thumb ends. */
	ended = kl_scroll_bar_release(&scrollbar_content, scrollbar_now());
	if (ended == 0)
		return 0;

	/* A frame shows the bar shrink when the pointer went away. */
	fm_log("SCROLLBAR release scroll=%d", fm_ui_tab(app)->scroll);
	app->dirty = 1;

	/* Succeeded: the drag is over. */
	return 1;
}

/*
 * The pointer left the window: the bar shrinks and waits to fade.
 */
void
fm_scrollbar_leave(
	struct fm_app *app)
{
	int changed;

	/* A frame when it shrank. */
	changed = kl_scroll_bar_leave(&scrollbar_content, scrollbar_now());
	if (changed != 0)
		app->dirty = 1;
}

/*
 * Draws the bar over the content's card, when it shows.
 */
void
fm_scrollbar_draw(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	struct kl_scroll_bar_shape shape;
	struct kl_rect viewport;
	struct fm_tab *tab;
	kl_color track;
	kl_color thumb;
	int shown;

	/* What shows of the bar now. */
	scrollbar_viewport(app, &viewport);
	tab = fm_ui_tab(app);
	shown = kl_scroll_bar_shape(&scrollbar_content, &viewport, (double)app->layout.content_height, (double)tab->scroll, scrollbar_now(), &shape);
	if (shown == 0)
		return;

	/* The faint track while the bar is thick. */
	if (shape.thick != 0) {
		track = 0x00f4f4f4U | ((uint32_t)(150.0 * shape.alpha) << 24);
		kl_canvas_round(canvas, (float)shape.track_x, (float)shape.track_y, (float)shape.track_width, (float)shape.track_height, (float)shape.track_width / 2.0f, track);
	}

	/* The thumb, dark grey and rounded. */
	thumb = 0x00303030U | ((uint32_t)(110.0 * shape.alpha) << 24);
	kl_canvas_round(canvas, (float)shape.thumb_x, (float)shape.thumb_y, (float)shape.thumb_width, (float)shape.thumb_height, (float)shape.thumb_width / 2.0f, thumb);
}

/*
 * Tells whether the bar still changes with time (it waits to fade or
 * fades): the loop should draw again soon.
 */
int
fm_scrollbar_busy(
	struct fm_app *app)
{
	int busy;

	/* The bar's own clock decides. */
	(void)app;
	busy = kl_scroll_bar_busy(&scrollbar_content, scrollbar_now());

	/* Reports whether it changes on. */
	return busy;
}

/* Gives the content's card as the bar's viewport. */
static void
scrollbar_viewport(
	const struct fm_app *app,
	struct kl_rect *viewport)
{
	/* The card the items are drawn in. */
	viewport->x = app->layout.content.x;
	viewport->y = app->layout.content.y;
	viewport->width = app->layout.content.width;
	viewport->height = app->layout.content.height;
}

/* Reports the monotonic clock in microseconds (the bar's times are its own). */
static uint64_t
scrollbar_now(
	void)
{
	struct timespec now;
	int error;

	/* The monotonic clock; a clock that cannot be read is the start of time. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0U;

	/* Succeeded: reports the sample in microseconds. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/* Scrolls the content to an offset the bar gave, within what there is. */
static void
scrollbar_set(
	struct fm_app *app,
	double offset)
{
	struct fm_tab *tab;
	int limit;

	/* The furthest the content scrolls. */
	tab = fm_ui_tab(app);
	limit = app->layout.content_height - app->layout.content.height;
	if (limit < 0)
		limit = 0;

	/* The new scroll, inside the range, and a frame for it. */
	tab->scroll = (int)(offset + 0.5);
	if (tab->scroll > limit)
		tab->scroll = limit;
	if (tab->scroll < 0)
		tab->scroll = 0;
	app->dirty = 1;
}
