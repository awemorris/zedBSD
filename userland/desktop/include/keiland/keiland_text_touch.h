/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch of a view of editable text, and its bar of editing buttons, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_TEXT_TOUCH_H
#define KEILAND_TEXT_TOUCH_H

#include <stddef.h>
#include <stdint.h>
#include <keiland/keiland_canvas.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kl_scroll;
struct kl_text;
struct kl_theme;

/*
 * The touch of a view of editable text (text-touch.c): in a text editor's
 * body and a text field, one finger's drag selects and two fingers scroll.
 *
 * A tap puts the caret, a double tap selects a word, one finger's drag
 * selects from where it touched (the content scrolls by itself while the
 * finger is near the view's edge), and a long press asks for the context
 * menu.  A selection made by touch shows a handle at each end; dragging a
 * handle moves that end.  Two fingers are the scroll's (kl_ui gives them
 * to it).
 *
 * The view gives three answers in its content's coordinates (the scroll
 * already undone): the text position nearest a point, the caret's
 * rectangle at a position, and the word around a position.  Positions are
 * whatever the view counts in (byte offsets in Text Editor).
 */
struct kl_text_view {
	size_t (*position_at)(void *data, double x, double y);
	void (*caret_rect)(void *data, size_t position, struct kl_rect *rect);
	void (*word_at)(void *data, size_t position, size_t *start, size_t *end);
};

/* What the fingers changed, for kl_text_touch_take. */
#define KL_TEXT_TOUCH_SELECTION	1U
#define KL_TEXT_TOUCH_MENU		2U
#define KL_TEXT_TOUCH_BAR		4U	/* KL_VERSION 74: the bar came or went */

/* The handles: their drawn diameter and the diameter a finger finds them within. */
#define KL_TEXT_HANDLE		12
#define KL_TEXT_HANDLE_REACH	44

/* How near the view's edge a selecting finger scrolls the content, and how fast at the edge (pixels a second). */
#define KL_TEXT_EDGE		24
#define KL_TEXT_EDGE_SPEED	1200.0

/* Which end of the selection a handle drag moves. */
#define KL_TEXT_HANDLE_NONE	0
#define KL_TEXT_HANDLE_ANCHOR	1
#define KL_TEXT_HANDLE_CARET	2

struct kl_text_touch {
	/* The view's answers and their data. */
	const struct kl_text_view *view;
	void *data;

	/* The selection: from the anchor to the caret (equal: only a caret). */
	size_t anchor;
	size_t caret;

	/*
	 * Whether a finger is selecting, which handle it holds, whether the
	 * handles show, the finger (content coordinates), and how far the finger
	 * holding a handle is from the middle of its end's caret (subtracted, so
	 * that the end follows the caret's line, not the knob's below it).
	 */
	int selecting;
	int handle;
	int handles;
	double finger_x;
	double finger_y;
	double grip_x;
	double grip_y;

	/* What changed since kl_text_touch_take, and where the context menu was asked for (window coordinates). */
	unsigned changes;
	double menu_x;
	double menu_y;

	/*
	 * KL_VERSION 74: whether the bar of editing buttons (kl_text_bar_*) shows
	 * over the selection.  It is the struct's last field: a program built with
	 * an older header has a touch without it and must not give that touch to
	 * kl_ui_text_region of a library of 74 or later.
	 */
	int bar;
};

void kl_text_touch_init(struct kl_text_touch *touch, const struct kl_text_view *view, void *data);
void kl_text_touch_set_selection(struct kl_text_touch *touch, size_t anchor, size_t caret);
void kl_text_touch_tap(struct kl_text_touch *touch, double x, double y, int twice);
void kl_text_touch_long_press(struct kl_text_touch *touch, double window_x, double window_y);
void kl_text_touch_drag_begin(struct kl_text_touch *touch, double x, double y);
void kl_text_touch_drag(struct kl_text_touch *touch, double x, double y);
void kl_text_touch_drag_end(struct kl_text_touch *touch);
int kl_text_touch_edge(const struct kl_text_touch *touch, const struct kl_scroll *scroll, double *vx, double *vy);
unsigned kl_text_touch_take(struct kl_text_touch *touch);
void kl_text_touch_select(struct kl_text_touch *touch, size_t anchor, size_t caret);
void kl_text_touch_hide_bar(struct kl_text_touch *touch);
void kl_text_touch_toggle_bar(struct kl_text_touch *touch);
void kl_text_touch_draw_handles(const struct kl_text_touch *touch, struct kl_canvas *canvas, double origin_x, double origin_y, const struct kl_theme *theme);

/*
 * KL_VERSION 74: the bar of editing buttons over a selection the fingers made
 * -- Cut, Copy, Paste and Select All, those that apply.  kl_text_bar_buttons
 * chooses the buttons from what the text is (KL_TEXT_BAR_SELECTED ...),
 * kl_text_bar_layout places the bar above the selection (below it when there
 * is no room, over it when neither fits) within bounds, kl_text_bar_hit
 * records its buttons for the input of a frame and reports the one clicked
 * since the last frame, and kl_text_bar_draw draws it.  The library's fields
 * and text area show it themselves; a text view of a program's own (Text
 * Editor) uses these calls.  Ids from 0xfffffff0 up are the library's own and
 * are not used by programs.
 */
#define KL_TEXT_BAR_CUT		1U
#define KL_TEXT_BAR_COPY	2U
#define KL_TEXT_BAR_PASTE	4U
#define KL_TEXT_BAR_SELECT_ALL	8U
#define KL_TEXT_BAR_BUTTONS	4U

/* What the bar is chosen from (bits for kl_text_bar_buttons). */
#define KL_TEXT_BAR_SELECTED	1U	/* the selection is not empty */
#define KL_TEXT_BAR_WHOLE	2U	/* the selection is the whole text */
#define KL_TEXT_BAR_EMPTY	4U	/* the text is empty */
#define KL_TEXT_BAR_SECRET	8U	/* a secret field's: nothing is copied out of it */
#define KL_TEXT_BAR_READ_ONLY	16U	/* nothing is cut out of it or pasted into it */
#define KL_TEXT_BAR_CLIPBOARD	32U	/* a window's clipboard is there */
#define KL_TEXT_BAR_CAN_PASTE	64U	/* the clipboard has text to paste */

/* The bar's height, the gap between it and the selection, and a button's margin on each side of its label. */
#define KL_TEXT_BAR_HEIGHT	36
#define KL_TEXT_BAR_GAP		8
#define KL_TEXT_BAR_MARGIN	12

/*
 * A bar laid out: the buttons it shows (KL_TEXT_BAR_* bits), its rectangle
 * in the window, and its cells from the left, each a button's kind and
 * rectangle.  count is 0 for no bar.
 */
struct kl_ui;
struct kl_style;

struct kl_text_bar {
	unsigned buttons;
	struct kl_rect rect;
	size_t count;
	unsigned kinds[KL_TEXT_BAR_BUTTONS];
	struct kl_rect cells[KL_TEXT_BAR_BUTTONS];
};

unsigned kl_text_bar_buttons(unsigned facts);
int kl_text_bar_layout(struct kl_text_bar *bar, struct kl_text *text, unsigned buttons, const struct kl_rect *selection, const struct kl_rect *visible, const struct kl_rect *bounds);
unsigned kl_text_bar_hit(struct kl_ui *ui, uint32_t id, const struct kl_text_bar *bar, unsigned *held);
void kl_text_bar_draw(const struct kl_text_bar *bar, const struct kl_style *style, unsigned held);

#ifdef __cplusplus
}
#endif

#endif
