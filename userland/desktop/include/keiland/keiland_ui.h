/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input of a window: which part of a frame an input meant, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_UI_H
#define KEILAND_UI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kl_rect;
struct kl_scroll;
struct kl_text_touch;

/*
 * The input of a window (ui.c): which part of a frame a pointer, the wheel or
 * a finger meant.
 *
 * While a frame is drawn, each part that takes input is recorded: a
 * widget (kl_ui_hit, by an id the application chooses and an index), a
 * scroll's viewport (kl_ui_scroll_region) and a view of editable text
 * (kl_ui_text_region).  Input that arrives before the next frame is
 * resolved against the parts of the frame last drawn, the latest recorded
 * first (on top): a press and a release on the same widget click it (twice
 * within 400 ms: a double click), the wheel goes to the scroll under the
 * pointer, and a finger goes by libkeiland's gestures: a tap to the widget
 * it touched (else to the text view there), a drag to the scroll or text
 * view it touched (a widget such as a list's row inside a scroll does not
 * take a drag; the scroll does).  In a text view one finger's drag selects
 * and two fingers' drag scrolls (kl_text_touch).  Input that meets no
 * part is kept for the application (kl_ui_take; a drag's distance from
 * kl_ui_drag_offset).  Each input call returns 1 when the window must
 * draw again.
 *
 * A scroll or a text touch given to kl_ui_scroll_region or
 * kl_ui_text_region must live until the next frame is drawn (the input
 * in between reaches it).
 */
struct kl_ui;

/* What a widget's record reports of the input (bits). */
#define KL_HIT_HOT		1U	/* the pointer is over it */
#define KL_HIT_ACTIVE		2U	/* a press on it is held */
#define KL_HIT_CLICKED	4U	/* pressed and released on it since the last frame */
#define KL_HIT_DOUBLE		8U	/* the click was the second of a double click or tap */
#define KL_HIT_FOCUSED	16U	/* it has the keyboard's focus (a widget that takes the keyboard) */
#define KL_HIT_TOUCHED	32U	/* the click was a finger's tap */

/* The input no part took. */
#define KL_EVENT_PRESS		1U
#define KL_EVENT_RELEASE	2U
#define KL_EVENT_WHEEL		3U
#define KL_EVENT_TAP		4U
#define KL_EVENT_DOUBLE_TAP	5U
#define KL_EVENT_LONG_PRESS	6U
#define KL_EVENT_DRAG_BEGIN	7U
#define KL_EVENT_DRAG_END	8U

/*
 * One input no part took: its kind, where (window coordinates), the
 * wheel's distance or a drag's velocity, the fingers down, and the id of
 * the region it happened over (a long press over a text view: that view's
 * id, 0 over none).
 */
struct kl_event {
	unsigned kind;
	double x;
	double y;
	double dx;
	double dy;
	unsigned fingers;
	uint32_t region;
	uint32_t code;
	unsigned modifiers;
};

struct kl_window_event;

struct kl_ui *kl_ui_create(void);
void kl_ui_destroy(struct kl_ui *ui);
int kl_ui_pointer_motion(struct kl_ui *ui, double x, double y);
int kl_ui_pointer_leave(struct kl_ui *ui);
int kl_ui_pointer_cancel(struct kl_ui *ui);
int kl_ui_take_damage(struct kl_ui *ui, struct kl_rect *rect);

/*
 * KL_VERSION 90: what the next frame draws.  The lit widget's change gives
 * the widget lit when the frame shown was drawn and the one lit now, not
 * every widget the pointer crossed in between; a change the widgets do not
 * know of is added with kl_ui_damage (a part) or kl_ui_damage_whole.
 * kl_ui_take_frame takes them as at most KL_FRAME_PARTS_MAX parts, near ones
 * merged, and forgets them: KL_FRAME_NONE (nothing to draw), KL_FRAME_PARTS
 * (count parts, each drawn clipped to it, the rest of the frame keeping its
 * pixels) or KL_FRAME_WHOLE.  kl_window_present_parts shows a frame by such
 * parts.  From this version kl_ui_pointer_motion and kl_ui_pointer_leave
 * return 1 only when the lighting changed, without adding parts themselves,
 * and kl_ui_take_damage gives the box round the same parts (1 with one pixel
 * when the lighting changed and came back, 0 for nothing or the whole window
 * as before).  The lighting drawn is the one kl_ui_end saw the frame record.
 */
#define KL_FRAME_NONE		0
#define KL_FRAME_PARTS		1
#define KL_FRAME_WHOLE		2
#define KL_FRAME_PARTS_MAX	4U
int kl_ui_take_frame(struct kl_ui *ui, struct kl_rect *parts, unsigned capacity, unsigned *count);
void kl_ui_damage(struct kl_ui *ui, const struct kl_rect *rect);
void kl_ui_damage_whole(struct kl_ui *ui);
int kl_ui_pointer_button(struct kl_ui *ui, int pressed, uint64_t now_us);
int kl_ui_wheel(struct kl_ui *ui, double dx, double dy, uint64_t now_us);
/*
 * Takes a window's scrolling (KL_WINDOW_AXIS, KL_WINDOW_AXIS_STOP): 0 when
 * nothing took it, 1 when a scroll did, KL_UI_AXIS_FLUNG (KL_VERSION 43)
 * when the fingers' lift threw the content (it flies on).
 */
#define KL_UI_AXIS_FLUNG	2
int kl_ui_axis(struct kl_ui *ui, const struct kl_window_event *event);
int kl_ui_touch_down(struct kl_ui *ui, int32_t id, uint64_t time_us, uint64_t now_us, double x, double y);
int kl_ui_touch_motion(struct kl_ui *ui, int32_t id, uint64_t time_us, uint64_t now_us, double x, double y);
int kl_ui_touch_up(struct kl_ui *ui, int32_t id, uint64_t time_us, uint64_t now_us);
int kl_ui_touch_cancel(struct kl_ui *ui, uint64_t now_us);
void kl_ui_begin(struct kl_ui *ui, uint64_t now_us);
unsigned kl_ui_hit(struct kl_ui *ui, uint32_t id, uint32_t index, const struct kl_rect *rect);

/*
 * KL_VERSION 89: a widget that draws the same lit, held or neither (a
 * picture a press drags, a row's ground that only takes clicks).  It is
 * recorded and reports KL_HIT_* as kl_ui_hit, but the pointer coming onto
 * it, leaving it or pressing it asks for no part of the window to be drawn
 * again: kl_ui_pointer_motion and kl_ui_pointer_leave return 0 and
 * kl_ui_take_damage has nothing for it.  An application that draws such a
 * widget lit (KL_HIT_HOT) uses kl_ui_hit instead.
 */
unsigned kl_ui_hit_still(struct kl_ui *ui, uint32_t id, uint32_t index, const struct kl_rect *rect);
void kl_ui_scroll_region(struct kl_ui *ui, uint32_t id, const struct kl_rect *rect, struct kl_scroll *scroll);
void kl_ui_text_region(struct kl_ui *ui, uint32_t id, const struct kl_rect *rect, struct kl_scroll *scroll, struct kl_text_touch *touch);

/*
 * KL_VERSION 74: the fingers' selection of the fields and the text areas.  A
 * finger's double tap on a focused field's text selects the word there with a
 * handle at each end and the bar of editing buttons (kl_text_bar_*); a finger
 * on a handle drags that end, and the bar's buttons cut, copy, paste and
 * select all through the window's clipboard (kl_ui_window_text ties it).
 * kl_ui_end draws the handles and the bar on the canvas the field was drawn
 * on, within the clip in force then, over everything drawn before it, so a
 * program calls it while that canvas is still the frame's.
 * kl_ui_set_text_bar turns the selection off (enabled 0: a double tap selects
 * the whole text as a click's does) or gives where the bar may stand (bounds
 * in window coordinates; NULL: the whole canvas).
 */
void kl_ui_set_text_bar(struct kl_ui *ui, const struct kl_rect *bounds, int enabled);
int kl_ui_end(struct kl_ui *ui, uint64_t now_us);
int kl_ui_take(struct kl_ui *ui, struct kl_event *event);
int kl_ui_drag_offset(struct kl_ui *ui, uint64_t now_us, double *dx, double *dy);

#ifdef __cplusplus
}
#endif

#endif
