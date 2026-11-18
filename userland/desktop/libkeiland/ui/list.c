/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The list and the sidebar of the library (ws090-p005), in Files' look
 * (files/ui-list.c and files/ui.c: 28-pixel rows with 7-pixel corners,
 * the accent under the selected row while the list has the keyboard, a
 * faint ground under the pointer; 30-pixel places under an 11-pixel
 * section title, the place shown in the accent).
 *
 * A list scrolls with libkeiland's scroll (the wheel's glide, a finger's
 * drag and flight); a click or a tap selects a row and a double one
 * activates it; while the list has the keyboard, the arrows, Page Up,
 * Page Down, Home and End move the selection and Enter activates it.  The
 * application draws each row's content over the ground the list gives it.
 */

#include "internal.h"

#include <errno.h>
#include <string.h>

/* The index that stands for the list itself (its viewport) among its records. */
#define LIST_SELF		0xffffffffU

/* A row's corner radius, and the margin the scroll bar keeps at the right. */
#define LIST_ROW_RADIUS		7.0f
#define LIST_BAR_MARGIN		8

/* The sidebar: a place's height and corner radius, a section title's height and size, and a label's size. */
#define LIST_PLACE		30
#define LIST_PLACE_RADIUS	9.0f
#define LIST_SECTION		30
#define LIST_TEXT_SECTION	11U
#define LIST_TEXT_PLACE		14U

static int list_wants(uint32_t code, unsigned modifiers);
static int list_focused(const struct kl_ui *ui, uint32_t id);
static void list_select(struct kl_list *list, long index, const struct kl_rect *rect, int row_height, uint64_t now_us);

/*
 * Makes a list's state: no items, none selected, at the top.
 */
int
kl_list_init(
	struct kl_list *list)
{
	int error;

	/* Nothing yet. */
	memset(list, 0, sizeof(list[0]));
	list->selected = -1;

	/* The scroll, down only. */
	error = kl_scroll_init(&list->scroll, KL_SCROLL_Y);
	if (error != 0)
		return error;

	/* Succeeded: the list can be drawn. */
	return 0;
}

/*
 * Frees what a list's state holds.
 */
void
kl_list_release(
	struct kl_list *list)
{
	/* The scroll. */
	kl_scroll_release(&list->scroll);
}

/*
 * Starts drawing a list of count items in a rectangle: its scroll's
 * viewport is recorded, the drawing is clipped to it, the keys move the
 * selection, and *first and *last are the rows that show (the application
 * draws each with kl_list_row).  Reports what the keys did
 * (KL_LIST_* bits).
 */
unsigned
kl_list_begin(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	const struct kl_rect *rect,
	struct kl_list *list,
	size_t count,
	size_t *first,
	size_t *last)
{
	uint32_t code;
	unsigned modifiers;
	unsigned changes;
	long selected;
	long page;
	int height;
	int taken;

	/* The items and the scroll's sizes. */
	height = style->theme->row_height;
	list->count = count;
	if (list->selected >= (long)count)
		list->selected = -1;
	kl_scroll_set_size(&list->scroll, (double)rect->width, (double)count * (double)height, (double)rect->width, (double)rect->height);

	/* The viewport takes the wheel and a finger's drag; the list itself takes the keyboard (a Tab reaches it). */
	kl_ui_scroll_region(ui, id, rect, &list->scroll);
	(void)keiui_ui_widget(ui, id, LIST_SELF, rect, KEIUI_FOCUSABLE);

	/* The keys pressed while the list had the keyboard. */
	changes = 0;
	page = (long)(rect->height / height) - 1L;
	if (page < 1L)
		page = 1L;
	for (;;) {
		taken = keiui_ui_take_key(ui, id, KEIUI_ANY, list_wants, &code, &modifiers);
		if (!taken)
			break;

		/* Enter activates the selected item. */
		if (code == KL_KEY_ENTER || code == KL_KEY_KPENTER) {
			if (list->selected >= 0)
				changes |= KL_LIST_ACTIVATED;
			continue;
		}

		/* The others move the selection. */
		selected = list->selected;
		switch (code) {
		case KL_KEY_UP:
			selected--;
			break;
		case KL_KEY_DOWN:
			selected++;
			break;
		case KL_KEY_PAGEUP:
			selected -= page;
			break;
		case KL_KEY_PAGEDOWN:
			selected += page;
			break;
		case KL_KEY_HOME:
			selected = 0;
			break;
		case KL_KEY_END:
			selected = (long)count - 1L;
			break;
		default:
			break;
		}

		/* Kept inside the list. */
		if (selected < 0)
			selected = 0;
		if (selected >= (long)count)
			selected = (long)count - 1L;
		if (selected != list->selected && count > 0U) {
			list_select(list, selected, rect, height, keiui_ui_now(ui));
			changes |= KL_LIST_SELECTED;
		}
	}

	/* The rows that show. */
	*first = (size_t)(list->scroll.y / (double)height);
	*last = (size_t)((list->scroll.y + (double)rect->height) / (double)height) + 1U;
	if (*last > count)
		*last = count;
	if (*first > *last)
		*first = *last;

	/* The rows stay inside the viewport. */
	kl_canvas_clip_push(style->canvas, rect);

	/* Reports what the keys did. */
	return changes;
}

/*
 * Draws the ground of one row of a list and takes its clicks: *row is
 * where the application draws its content, *ink the colour to draw it in.
 * Reports what happened (KL_LIST_* bits).
 */
unsigned
kl_list_row(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	const struct kl_rect *rect,
	struct kl_list *list,
	size_t index,
	struct kl_rect *row,
	kl_color *ink)
{
	const struct kl_theme *theme;
	unsigned changes;
	unsigned state;
	int focused;

	/* The row's place, scrolled, short of the scroll bar. */
	theme = style->theme;
	row->x = rect->x;
	row->y = rect->y + (int)index * theme->row_height - (int)list->scroll.y;
	row->width = rect->width - LIST_BAR_MARGIN;
	row->height = theme->row_height;

	/* The record: a press on a row gives the keyboard to its list (the list's own record under it). */
	state = keiui_ui_widget(ui, id, (uint32_t)index, row, 0U);

	/* A click selects it; a double click (or tap) activates it. */
	changes = 0;
	if ((state & KL_HIT_CLICKED) != 0U) {
		if ((long)index != list->selected)
			changes |= KL_LIST_SELECTED;
		list->selected = (long)index;
		if ((state & KL_HIT_DOUBLE) != 0U)
			changes |= KL_LIST_ACTIVATED;
		if ((state & KL_HIT_TOUCHED) != 0U)
			changes |= KL_LIST_TOUCHED;
	}

	/* The ground: the accent when selected (pale without the keyboard), faint under the pointer. */
	focused = list_focused(ui, id);
	*ink = theme->text;
	if ((long)index == list->selected) {
		if (focused) {
			kl_canvas_round(style->canvas, (float)row->x, (float)row->y + 1.0f, (float)row->width, (float)row->height - 2.0f, LIST_ROW_RADIUS, theme->accent);
			*ink = theme->accent_ink;
		} else {
			kl_canvas_round(style->canvas, (float)row->x, (float)row->y + 1.0f, (float)row->width, (float)row->height - 2.0f, LIST_ROW_RADIUS, theme->selection_inactive);
		}
	} else if ((state & KL_HIT_HOT) != 0U) {
		kl_canvas_round(style->canvas, (float)row->x, (float)row->y + 1.0f, (float)row->width, (float)row->height - 2.0f, LIST_ROW_RADIUS, theme->hover);
	}

	/* Reports what happened. */
	return changes;
}

/*
 * Ends a list: the clip goes, and the scroll bar is drawn while the list
 * moves.
 */
void
kl_list_end(
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *rect,
	struct kl_list *list)
{
	/* The clip of the rows goes. */
	kl_canvas_clip_pop(style->canvas);

	/* The bar, at the frame's time. */
	(void)kl_scroll_draw_bars(&list->scroll, style->canvas, rect, style->theme, keiui_ui_now(ui));
}

/*
 * Draws a sidebar's section title at (x, y) and reports the top of its
 * first place.
 */
int
kl_sidebar_section(
	const struct kl_style *style,
	int x,
	int y,
	int width,
	const char *title)
{
	kl_color ink;

	/* Secondary, on glass and on an opaque ground alike (the faint ink is under 3:1 on the sidebar, ws090-p014). */
	ink = style->theme->text_secondary;

	/* The title, small and bold. */
	(void)kl_text_draw_fit(style->text, style->canvas, x + 8, y + LIST_SECTION - 10, title, LIST_TEXT_SECTION, 1, width - 16, ink);

	/* Reports where the places start. */
	return y + LIST_SECTION;
}

/*
 * Draws a sidebar's place (its icon and label, lit when it is the place
 * shown or under the pointer) in a rectangle and reports 1 when it was
 * clicked or tapped.
 */
int
kl_sidebar_item(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	uint32_t index,
	const struct kl_rect *rect,
	enum kl_icon icon,
	const char *label,
	int current)
{
	int clicked;

	/* A place in the usual ink. */
	clicked = kl_sidebar_place(ui, style, id, index, rect, icon, label, current, 0U);
	return clicked;
}

/*
 * Draws a sidebar's place as kl_sidebar_item does, with flags (KL_VERSION
 * 48, ws090-p023): KL_PLACE_FAINT for a place that is not there (the
 * faint ink), KL_PLACE_QUIET for one that is there but not ready (the
 * secondary ink, a device not mounted).  The place shown keeps the
 * accent.  Without the input (a NULL ui, KL_VERSION 60) it is drawn
 * unlit.  Reports whether it was clicked.
 */
int
kl_sidebar_place(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	uint32_t index,
	const struct kl_rect *rect,
	enum kl_icon icon,
	const char *label,
	int current,
	unsigned flags)
{
	const struct kl_theme *theme;
	kl_color ink;
	unsigned state;

	/* The record (none without the input, KL_VERSION 60). */
	theme = style->theme;
	state = 0;
	if (ui != NULL)
		state = kl_ui_hit(ui, id, index, rect);

	/* The ink: faint for a place not there, quiet for one not ready. */
	ink = theme->text;
	if ((flags & KL_PLACE_QUIET) != 0U)
		ink = theme->text_secondary;
	if ((flags & KL_PLACE_FAINT) != 0U)
		ink = theme->text_faint;

	/* The ground: the accent for the place shown, faint under the pointer. */
	if (current) {
		kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, LIST_PLACE_RADIUS, theme->selection);
		ink = theme->accent_text;
	} else if ((state & KL_HIT_HOT) != 0U) {
		kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, LIST_PLACE_RADIUS, theme->hover);
	}

	/* The icon and the label (bold for the place shown). */
	kl_icon_draw(style->canvas, icon, (float)rect->x + 8.0f, (float)rect->y + ((float)rect->height - 18.0f) * 0.5f, 18.0f, ink);
	(void)kl_text_draw_fit(style->text, style->canvas, rect->x + 36, kl_text_center(LIST_TEXT_PLACE, rect->y, rect->height), label, LIST_TEXT_PLACE, current, rect->width - 44, ink);

	/* Reports whether it was clicked. */
	if ((state & KL_HIT_CLICKED) != 0U)
		return 1;
	return 0;
}

/* Tells whether a list takes a key: the arrows up and down, Page Up and Page Down, Home, End and Enter (without Control or Alt). */
static int
list_wants(
	uint32_t code,
	unsigned modifiers)
{
	/* A command is the application's. */
	if ((modifiers & (KL_MOD_CTRL | KL_MOD_ALT)) != 0U)
		return 0;

	/* The keys that move and activate. */
	switch (code) {
	case KL_KEY_UP:
	case KL_KEY_DOWN:
	case KL_KEY_PAGEUP:
	case KL_KEY_PAGEDOWN:
	case KL_KEY_HOME:
	case KL_KEY_END:
	case KL_KEY_ENTER:
	case KL_KEY_KPENTER:
		return 1;
	default:
		break;
	}

	/* Any other. */
	return 0;
}

/* Tells whether the keyboard's focus is on a list (on itself or one of its rows). */
static int
list_focused(
	const struct kl_ui *ui,
	uint32_t id)
{
	uint32_t focus_id;
	uint32_t focus_index;
	int focused;

	/* The widget with the focus, if any. */
	focused = keiui_ui_focused(ui, &focus_id, &focus_index);
	if (!focused)
		return 0;

	/* This list's. */
	if (focus_id != id)
		return 0;
	return 1;
}

/* Selects an item and glides the list just enough to show it. */
static void
list_select(
	struct kl_list *list,
	long index,
	const struct kl_rect *rect,
	int row_height,
	uint64_t now_us)
{
	struct kl_rect row;

	/* The selection. */
	list->selected = index;

	/* Its row, in the content, shown. */
	row.x = 0;
	row.y = (int)index * row_height;
	row.width = rect->width;
	row.height = row_height;
	kl_scroll_reveal(&list->scroll, &row, now_us);
}
