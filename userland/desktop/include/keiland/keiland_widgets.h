/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The widgets, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_WIDGETS_H
#define KEILAND_WIDGETS_H

#include <stddef.h>
#include <stdint.h>
#include <keiland/keiland_canvas.h>
#include <keiland/keiland_icon.h>
#include <keiland/keiland_scroll.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kl_text;
struct kl_theme;
struct kl_ui;
struct kl_window;
struct kl_window_event;

/*
 * The widgets (widgets.c, field.c, list.c, cards.c): each is drawn by one
 * call during a frame, which also records where it is for the input and
 * reports what the input did to it since the last frame (the immediate way).
 * What a widget remembers between frames -- a field's text, a list's
 * selection and scroll -- is the application's, in a small struct it keeps.
 * A widget draws with a style: the canvas of the frame, the text, the theme,
 * and whether the window stands on glass.
 *
 * The keyboard's focus is on one widget at a time (by id and index).  A
 * click or a tap on a widget that takes the keyboard gives it the focus;
 * Tab and Shift+Tab move it through those widgets in the order they were
 * drawn.  The keys a focused widget does not take, and every key while no
 * widget has the focus, are the application's (KL_EVENT_KEY).
 */
struct kl_style {
	struct kl_canvas *canvas;
	struct kl_text *text;
	const struct kl_theme *theme;
	int glass;
};

/* A key no widget took (an event's kind; kl_event's code and modifiers name it). */
#define KL_EVENT_KEY		9U

/* A button's look and state (bits). */
#define KL_BUTTON_PRIMARY	1U
#define KL_BUTTON_DANGER	2U
#define KL_BUTTON_DISABLED	4U

/* KL_VERSION 60: a picture's button that is a circle, and one whose ground shows only under the pointer. */
#define KL_BUTTON_ROUND	8U
#define KL_BUTTON_QUIET	16U

/* What a text field reports (bits). */
#define KL_FIELD_CHANGED	1U
#define KL_FIELD_SUBMITTED	2U
#define KL_FIELD_CANCELLED	4U

/* What a list reports (bits). */
#define KL_LIST_SELECTED	1U
#define KL_LIST_ACTIVATED	2U
#define KL_LIST_TOUCHED	4U	/* the row was chosen by a finger's tap */

/* The longest text a field holds, with its NUL. */
#define KL_FIELD_MAX		512U

/*
 * A one-line text field's state: its UTF-8 text, the caret and the other end
 * of the selection (byte offsets on character boundaries), how far the text
 * is scrolled across, whether its characters are shown as dots, and
 * (KL_VERSION 47) whether it takes no input method though its characters show
 * (an address, a key shown as typed; a secret field takes none either), and
 * (KL_VERSION 64) the most bytes its text may hold: what the receiver of the
 * text takes, so a longer text cannot be typed (0, as a zeroed field starts,
 * is the field's room, KL_FIELD_MAX less one).
 */
struct kl_field {
	char text[KL_FIELD_MAX];
	size_t length;
	size_t caret;
	size_t anchor;
	int scroll;
	int secret;
	int plain;
	size_t limit;
};

/*
 * KL_VERSION 47: a text area's state, text of several lines (kl_text_area):
 * its UTF-8 text, the caret and the other end of the selection (byte offsets
 * on character boundaries), how far it is scrolled down, and the place across
 * Up and Down keep (-1 for none).
 */
#define KL_TEXT_AREA_MAX	8192U
struct kl_text_area {
	char text[KL_TEXT_AREA_MAX];
	size_t length;
	size_t caret;
	size_t anchor;
	int scroll;
	int goal_x;
};

/*
 * A list's state: how many items it has, the one selected (-1 for none),
 * and its scroll.
 */
struct kl_list {
	size_t count;
	long selected;
	struct kl_scroll scroll;
};

/* The keyboard's focus, and where the pointer is for a widget that follows it. */
int kl_ui_key(struct kl_ui *ui, uint32_t key, int pressed, unsigned modifiers);
void kl_ui_set_focus(struct kl_ui *ui, uint32_t id, uint32_t index);
void kl_ui_clear_focus(struct kl_ui *ui);
int kl_ui_has_focus(const struct kl_ui *ui, uint32_t id, uint32_t index);
void kl_ui_pointer(const struct kl_ui *ui, double *x, double *y);

/*
 * KL_VERSION 38: the text an input method or the on-screen keyboard sends,
 * for the widget with the focus.  The application gives each KL_WINDOW_TEXT_*
 * input of its window to kl_ui_text: a text field puts a commit in place of
 * its selection, deletes the bytes around its caret, and shows the text being
 * composed at its caret.  After each frame kl_ui_text_wanted tells whether
 * the focused widget takes text, with its caret's rectangle in the window,
 * for kl_window_text_input and kl_window_text_cursor.
 */
int kl_ui_text(struct kl_ui *ui, const struct kl_window_event *event);
int kl_ui_text_wanted(const struct kl_ui *ui, struct kl_rect *caret);

/*
 * KL_VERSION 47: the wiring every program with widgets on a window needs, so
 * that each field takes an input method's text.  kl_ui_window_input gives one
 * input of the window to the widgets (the pointer, the main button, the
 * wheel, the keys, the fingers and the text input's KL_WINDOW_TEXT_*) and
 * returns 1 when it was theirs, 0 for another kind (the program's).
 * kl_ui_window_text, after each frame, asks for the window's text input while
 * the focused widget takes text and tells where its caret is
 * (kl_ui_text_wanted, kl_window_text_input and kl_window_text_cursor); a
 * secret field takes none.
 */
int kl_ui_window_input(struct kl_ui *ui, const struct kl_window_event *event);
void kl_ui_window_text(struct kl_ui *ui, struct kl_window *window);

/* The widgets, each drawn and asked by one call during a frame. */
int kl_button(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, const char *label, unsigned flags);
int kl_button_width(const struct kl_style *style, const char *label);

/*
 * KL_VERSION 60: a button that is a picture alone, one of several under an id
 * (index), filling its rectangle: the control's rounded square within its
 * edge, or a circle (KL_BUTTON_ROUND); a quiet one (KL_BUTTON_QUIET) shows
 * its ground only under the pointer or while held.  The icon, pixels square,
 * is in the middle.  KL_BUTTON_DISABLED fades it.  Reports 1 when it was
 * pressed since the last frame.
 *
 * kl_button, kl_icon_button and kl_sidebar_place also draw with a NULL
 * ui (an application whose kl_ui could not be made): unlit, and never
 * pressed.
 */
int kl_icon_button(struct kl_ui *ui, const struct kl_style *style, uint32_t id, uint32_t index, const struct kl_rect *rect, enum kl_icon icon, int pixels, unsigned flags);
int kl_switch(struct kl_ui *ui, const struct kl_style *style, uint32_t id, int x, int y, int *on, unsigned flags);
int kl_slider(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, double minimum, double maximum, double step, double *value);
/* KL_VERSION 48: a slider with flags (KL_BUTTON_DISABLED: faded, no input). */
int kl_slider_flags(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, double minimum, double maximum, double step, double *value, unsigned flags);
void kl_field_set(struct kl_field *field, const char *text);
void kl_field_set_limit(struct kl_field *field, size_t limit);
unsigned kl_field(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, struct kl_field *field, const char *placeholder);
void kl_text_area_set(struct kl_text_area *area, const char *text);
unsigned kl_text_area(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, struct kl_text_area *area, const char *placeholder);
int kl_list_init(struct kl_list *list);
void kl_list_release(struct kl_list *list);
unsigned kl_list_begin(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, struct kl_list *list, size_t count, size_t *first, size_t *last);
unsigned kl_list_row(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *rect, struct kl_list *list, size_t index, struct kl_rect *row, kl_color *ink);
void kl_list_end(struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *rect, struct kl_list *list);

/*
 * KL_VERSION 53: the parts of the views of many items, in Files' look -- a
 * list of several columns under a header whose titles sort it, and a grid of
 * icons with their names -- where many items are selected at once and a
 * rubber band picks them.  The application keeps the selection and the hits,
 * lays out its columns, and draws each icon; these draw the rest.
 * kl_list_header draws the header in its rectangle (each column's title at
 * its x and width, KL_COLUMN_* flags), kl_list_item a row's ground for its
 * state (KL_ITEM_* bits) with the inks of its text, kl_list_cell a cell's
 * text; kl_grid_layout measures a grid (as many columns as fit, centred),
 * kl_grid_cell places a cell, kl_grid_icon its icon, and kl_grid_item draws a
 * cell's ground and its name (returning the name's last baseline); kl_band
 * draws a rubber band.
 */
#define KL_ITEM_SELECTED	1U	/* the item is selected */
#define KL_ITEM_FOCUSED	2U	/* the view has the keyboard: a selection in the accent */
#define KL_ITEM_HOVER		4U	/* the pointer is over the item */

#define KL_COLUMN_SORTED	1U	/* the list is sorted by the column */
#define KL_COLUMN_REVERSED	2U	/* ... the other way */
#define KL_COLUMN_HOVER	4U	/* the pointer is over its title */

#define KL_CELL_RIGHT		1U	/* a cell's text at the column's right (a size) */

/* The longest second line of a grid item's name, with its NUL. */
#define KL_GRID_NAME_MAX	256U

struct kl_column {
	const char *title;
	int x;
	int width;
	unsigned flags;
};

struct kl_grid {
	int columns;
	int left;
	int top;
	int cell_width;
	int cell_height;
	size_t rows;
	int content_height;
};

void kl_list_header(const struct kl_style *style, const struct kl_rect *rect, const struct kl_column *columns, size_t count);
void kl_list_item(const struct kl_style *style, const struct kl_rect *row, unsigned state, kl_color *ink, kl_color *faint);
void kl_list_cell(const struct kl_style *style, int x, int width, int baseline, const char *text, unsigned pixels, unsigned flags, kl_color ink);
void kl_grid_layout(const struct kl_rect *rect, int cell_width, int cell_height, size_t count, struct kl_grid *grid);
void kl_grid_cell(const struct kl_grid *grid, size_t index, int scroll, struct kl_rect *cell);
void kl_grid_icon(const struct kl_rect *cell, int icon, struct kl_rect *place);
int kl_grid_item(const struct kl_style *style, const struct kl_rect *cell, int icon, const char *name, unsigned pixels, unsigned state);
void kl_band(const struct kl_style *style, const struct kl_rect *rect);
int kl_sidebar_section(const struct kl_style *style, int x, int y, int width, const char *title);
int kl_sidebar_item(struct kl_ui *ui, const struct kl_style *style, uint32_t id, uint32_t index, const struct kl_rect *rect, enum kl_icon icon, const char *label, int current);
/* KL_VERSION 48: a sidebar's place in the faint ink (not there) or the quiet one (not ready). */
#define KL_PLACE_FAINT		1U
#define KL_PLACE_QUIET		2U
int kl_sidebar_place(struct kl_ui *ui, const struct kl_style *style, uint32_t id, uint32_t index, const struct kl_rect *rect, enum kl_icon icon, const char *label, int current, unsigned flags);
void kl_panel(const struct kl_style *style, const struct kl_rect *rect, int sidebar);
int kl_card(const struct kl_style *style, const struct kl_rect *rect, const char *title, const char *subtitle);
int kl_row(const struct kl_style *style, int x, int y, int width, const char *label, const char *value, int last);
int kl_header(const struct kl_style *style, int x, int y, int width, const char *title, const char *summary);
int kl_dialog(struct kl_ui *ui, const struct kl_style *style, uint32_t id, const struct kl_rect *area, const char *title, const char *body, const char *const *labels, int count);
void kl_chip(const struct kl_style *style, int centre_x, int bottom, const char *message);
void kl_progress(const struct kl_style *style, const struct kl_rect *rect, double fraction, uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif
