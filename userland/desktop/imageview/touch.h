/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch screen of Image Viewer (ws091, after PDF Viewer's ws081-p012,
 * plan/ws081/design.md section 5): the window's wl_touch events become
 * libkeiland's gestures and scroller.  One finger moves a zoomed image
 * and, let go fast, it glides on and slows down (past its edges it
 * stretches and springs back); a touch catches it.  Two fingers zoom about
 * the point between them (below the fit, it springs back to the fit).  A
 * sideways drag of a fitted image swipes to the next or the previous
 * image.  A double tap zooms in (100 %, or twice the fit), or back to the
 * fit; a long press opens the context menu.  Over the empty window a
 * finger plays the pointer's left button.
 */

#ifndef IMAGEVIEW_TOUCH_H
#define IMAGEVIEW_TOUCH_H

#include "imageview.h"

#include <keiland/keiland.h>

/*
 * The fingers and what they are doing: the gestures over the image, the
 * scroller that moves the view, and the state between them.
 *
 * pointer says a finger plays the pointer (pointer_id, last at pointer_x,
 * pointer_y) over the empty window; other fingers are then left alone.
 * fingers counts the fingers on the image.  pressed says the scroller
 * holds a touch that has not been let go; moving that the scroller owns
 * the view (from a touch until the content rests), and written the view's
 * place it last set, so that a move made elsewhere (a key, the wheel, a
 * resize) is seen and taken over.  drag is what the fingers' drag does
 * (TOUCH_DRAG_*), and base the drag's offset when the scroller was last
 * pressed.  caught says the touch caught gliding content (it taps
 * nothing).  While two fingers zoom (pinching), place is the place of the
 * image held under them, scale the scale when they started and ratio
 * their distance's ratio then.  bounds_* are the scroller's bounds as last
 * set.
 */
struct iv_touch {
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
	struct iv_place place;
	double scale;
	double ratio;
	double bounds_x;
	double bounds_y;
	double bounds_width;
	double bounds_height;
};

/* The touch screen (touch.c). */
int iv_touch_open(struct iv_touch *touch);
void iv_touch_close(struct iv_touch *touch);
void iv_touch_event(struct iv_touch *touch, struct iv_app *app, const struct kl_window_event *event);
int iv_touch_tick(struct iv_touch *touch, struct iv_app *app, uint64_t now);

#endif
