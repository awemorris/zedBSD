/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch screen of PDF Viewer (ws081-p012, plan/ws081/design.md
 * section 5): the window's wl_touch events become libkeiland's gestures
 * and scroller.  One finger scrolls the pages and, let go fast, they glide
 * on and slow down (past either end they stretch and spring back); a
 * touch catches them.  Two fingers zoom about the point between them.  In
 * the page mode a sideways drag of a page that fits across swipes to the
 * next or the previous page.  A double tap zooms in to twice the scale, or
 * back to the mode's fit.  A long press selects the word under it, whose
 * handles a finger then moves, and a tap lets the selection go
 * (ws177-p042).  Over the sidebar and the password card a finger plays
 * the pointer's left button.
 */

#ifndef PDFVIEWER_TOUCH_H
#define PDFVIEWER_TOUCH_H

#include "viewer.h"

#include <keiland/keiland.h>

/*
 * The fingers and what they are doing: the gestures of the pages, the
 * scroller that moves the view, and the state between them.
 *
 * pointer says a finger plays the pointer (pointer_id, last at pointer_x,
 * pointer_y) over the sidebar or the password card; other
 * fingers are then left alone.
 * fingers counts the fingers on the pages.  pressed says the scroller
 * holds a touch that has not been let go; moving that the scroller owns
 * the view (from a touch until the content rests), and written the view's
 * place it last set, so that a move made elsewhere (a key, the wheel, a
 * resize) is seen and taken over.  drag is what the fingers' drag does
 * (TOUCH_DRAG_*), and base the drag's offset when the scroller was last
 * pressed.  caught says the touch caught gliding content (it taps
 * nothing).  While two fingers zoom (pinching), place is the place of the
 * document held under them, scale the scale when they started and ratio
 * their distance's ratio then.  bounds_* are the scroller's bounds as last
 * set.  ws177-p042: handle says a finger (handle_id) holds a handle of the
 * selection (1 its start's, 2 its end's) and moves it; no gesture sees it.
 */
struct pv_touch {
	struct kl_gesture *gesture;
	struct kl_scroller *scroller;
	int pointer;
	int32_t pointer_id;
	int pointer_x;
	int pointer_y;
	unsigned fingers;
	int pressed;
	int moving;
	double written_x;
	double written_y;
	int drag;
	double base_x;
	double base_y;
	int caught;
	int pinching;
	struct pv_place place;
	double scale;
	double ratio;
	double bounds_x;
	double bounds_y;
	double bounds_width;
	double bounds_height;
	int handle;
	int32_t handle_id;
};

/* The touch screen (touch.c). */
int pv_touch_open(struct pv_touch *touch);
void pv_touch_close(struct pv_touch *touch);
void pv_touch_event(struct pv_touch *touch, struct pv_app *app, const struct kl_window_event *event);
int pv_touch_tick(struct pv_touch *touch, struct pv_app *app, uint64_t now);
void pv_touch_pad(struct pv_touch *touch, struct pv_app *app, const struct kl_window_event *event);
void pv_touch_pad_stop(struct pv_touch *touch, const struct kl_window_event *event);

#endif
