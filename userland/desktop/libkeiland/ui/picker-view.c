/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The media picker's view (ws197-p014): one frame of the window -- one card
 * on the system's glass (or Files' pale ground without it) with the title
 * and how many are selected, the grid of thumbnails, and the bar with
 * Cancel and Add -- and what the clicks and the keys do to the model.
 */

#include "picker.h"

#include <stdio.h>
#include <string.h>

/* The space around the card, its corners, and the space inside it. */
#define VIEW_GAP		8
#define VIEW_RADIUS		16
#define VIEW_PAD		12

/* The title's row and the bottom bar. */
#define VIEW_HEADER		44
#define VIEW_BAR		56

/* The buttons' height and widths. */
#define VIEW_BUTTON		32
#define VIEW_ACCEPT_WIDTH	104
#define VIEW_CANCEL_WIDTH	84

/* A grid cell, and the margin of its picture inside it. */
#define VIEW_CELL		136
#define VIEW_INSET		5

/* The picture's corners, the selection's ring and the number's circle. */
#define VIEW_THUMB_RADIUS	10.0f
#define VIEW_RING		3.0f
#define VIEW_BADGE		11.0f

/* The kind's icon on an item without a picture. */
#define VIEW_ICON		40.0f

/* The text sizes. */
#define VIEW_TEXT_TITLE		15U
#define VIEW_TEXT_ROW		13U
#define VIEW_TEXT_BADGE		12U

/* The warning's colours (a refusal's message), the file chooser's. */
#define VIEW_WARNING_TEXT	KL_RGB(0xc8313a)
#define VIEW_WARNING_GROUND	kl_theme_choose(KL_RGB(0xfff1f1), KL_RGB(0x3a2326))
#define VIEW_WARNING_EDGE	KL_RGB(0xf6c9cb)

/* The ground of an item still without a picture, and the dark chip of a video's mark. */
#define VIEW_EMPTY_GROUND	kl_theme_choose(KL_RGB(0xe9edf2), KL_RGB(0x343a45))
#define VIEW_VIDEO_GROUND	KL_RGBA(0x000000, 150)
#define VIEW_WHITE		KL_RGB(0xffffff)

/*
 * Where the window's parts are for its size.
 */
struct view_layout {
	struct kl_rect card;
	struct kl_rect header;
	struct kl_rect grid;
	struct kl_rect bar;
	struct kl_rect cancel;
	struct kl_rect accept;
};

static void view_layout(int width, int height, struct view_layout *layout);
static void view_header(const struct keiui_picker *picker, const struct kl_style *style, const struct view_layout *layout);
static int view_grid(struct keiui_picker *picker, struct kl_ui *ui, const struct kl_style *style, const struct view_layout *layout, uint64_t now_us);
static void view_item(const struct keiui_picker *picker, const struct kl_style *style, size_t index, const struct kl_rect *cell, unsigned state);
static void view_badge(const struct kl_style *style, const struct kl_rect *picture, unsigned order, size_t limit);
static void view_video_mark(const struct kl_style *style, const struct kl_rect *picture);
static void view_empty(const struct kl_style *style, const struct view_layout *layout);
static void view_lines(const struct kl_style *style, const struct view_layout *layout, const char *title, const char *line);
static int view_bar(struct keiui_picker *picker, struct kl_ui *ui, const struct kl_style *style, const struct view_layout *layout);
static void view_message(const struct keiui_picker *picker, const struct kl_style *style, const struct view_layout *layout);
static int view_key(struct keiui_picker *picker, uint32_t code, uint64_t now_us);
static void view_move(struct keiui_picker *picker, long delta, uint64_t now_us);
static void view_set(struct kl_rect *rect, int x, int y, int width, int height);

/*
 * Draws one frame of the picker at a size and a time and carries out the
 * input since the last one; reports 1 when another frame is wanted (the
 * model changed, the grid moves, or thumbnails in sight are still coming).
 */
int
keiui_picker_frame(
	struct keiui_picker *picker,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height,
	uint64_t now_us)
{
	struct view_layout layout;
	struct kl_event event;
	struct kl_rect whole;
	int changed;
	int moving;
	int taken;

	/* The frame's time, and the parts' places. */
	kl_ui_begin(ui, now_us);
	view_layout(width, height, &layout);

	/* The ground: clear over glass (the card's veil), Files' pale ground otherwise, and the card. */
	view_set(&whole, 0, 0, width, height);
	if (style->glass)
		kl_canvas_clear(style->canvas);
	else
		kl_canvas_gradient(style->canvas, &whole, style->theme->ground_top, style->theme->ground_bottom);
	kl_panel(style, &layout.card, 0);

	/* The title, the grid (or why it is empty), the bar and a refusal's message. */
	view_header(picker, style, &layout);
	changed = view_grid(picker, ui, style, &layout, now_us);
	changed |= view_bar(picker, ui, style, &layout);
	view_message(picker, style, &layout);

	/* The frame is drawn; the keys no widget took are the picker's commands. */
	moving = kl_ui_end(ui, now_us);
	for (;;) {
		taken = kl_ui_take(ui, &event);
		if (!taken)
			break;
		if (event.kind == KL_EVENT_KEY)
			changed |= view_key(picker, event.code, now_us);
	}

	/* Another frame when the model changed or something moves. */
	if (changed || moving)
		return 1;

	/* Nothing changes until the next input. */
	return 0;
}

/*
 * Writes the glass panel under the card for a size; returns how many (1, or
 * 0 without room for it).
 */
size_t
keiui_picker_panels(
	int width,
	int height,
	struct kl_rect *panels,
	size_t capacity)
{
	/* Room for it. */
	if (capacity < 1U)
		return 0U;

	/* The card, inset by the gap. */
	view_set(&panels[0], VIEW_GAP, VIEW_GAP, width - 2 * VIEW_GAP, height - 2 * VIEW_GAP);

	/* Succeeded: one panel. */
	return 1U;
}

/* Lays the window's parts out for its size. */
static void
view_layout(
	int width,
	int height,
	struct view_layout *layout)
{
	int middle;
	int right;

	/* The card. */
	(void)keiui_picker_panels(width, height, &layout->card, 1U);

	/* The title's row on top, the bar at the bottom, and the grid between them. */
	view_set(&layout->header, layout->card.x + VIEW_PAD, layout->card.y + 4, layout->card.width - 2 * VIEW_PAD, VIEW_HEADER);
	view_set(&layout->bar, layout->header.x, layout->card.y + layout->card.height - VIEW_BAR, layout->header.width, VIEW_BAR);
	view_set(&layout->grid, layout->header.x, layout->header.y + VIEW_HEADER, layout->header.width, layout->bar.y - layout->header.y - VIEW_HEADER - 4);

	/* The buttons from the right of the bar, in the middle of its height. */
	middle = layout->bar.y + (VIEW_BAR - VIEW_BUTTON) / 2 + 2;
	right = layout->bar.x + layout->bar.width;
	view_set(&layout->accept, right - VIEW_ACCEPT_WIDTH, middle, VIEW_ACCEPT_WIDTH, VIEW_BUTTON);
	view_set(&layout->cancel, layout->accept.x - 8 - VIEW_CANCEL_WIDTH, middle, VIEW_CANCEL_WIDTH, VIEW_BUTTON);
}

/* Draws the title and, on the right, how many are selected (or how many may be). */
static void
view_header(
	const struct keiui_picker *picker,
	const struct kl_style *style,
	const struct view_layout *layout)
{
	char line[64];
	int baseline;
	int width;

	/* The title, bold. */
	baseline = kl_text_center(VIEW_TEXT_TITLE, layout->header.y, layout->header.height);
	(void)kl_text_draw_fit(style->text, style->canvas, layout->header.x + 4, baseline, picker->title, VIEW_TEXT_TITLE, 1, layout->header.width / 2, style->theme->text);

	/* What the selection is, in words. */
	if (picker->selected != 0U && picker->limit > 1U)
		(void)snprintf(line, sizeof(line), "%zu selected, up to %zu", picker->selected, picker->limit);
	else if (picker->limit > 1U)
		(void)snprintf(line, sizeof(line), "Choose up to %zu", picker->limit);
	else
		(void)snprintf(line, sizeof(line), "Choose one");

	/* Right-aligned, quiet. */
	width = kl_text_width(style->text, line, strlen(line), VIEW_TEXT_ROW, 0);
	baseline = kl_text_center(VIEW_TEXT_ROW, layout->header.y, layout->header.height);
	(void)kl_text_draw(style->text, style->canvas, layout->header.x + layout->header.width - width - 4, baseline, line, strlen(line), VIEW_TEXT_ROW, 0, style->theme->text_secondary);
}

/* Draws the grid of the items in sight and carries out their clicks; 1 when the model changed or thumbnails are coming. */
static int
view_grid(
	struct keiui_picker *picker,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct view_layout *layout,
	uint64_t now_us)
{
	struct kl_grid grid;
	struct kl_rect cell;
	struct kl_rect picture;
	unsigned state;
	size_t first_row;
	size_t last_row;
	size_t index;
	int changed;
	int pending;

	/* The list still being read: frames keep coming until it is (ws157-p014). */
	if (picker->loading) {
		view_lines(style, layout, "Loading\xe2\x80\xa6", "");
		return 1;
	}

	/* A library that could not be read says so. */
	if (picker->unavailable) {
		view_lines(style, layout, "The media library is unavailable", "Try again later.");
		return 0;
	}

	/* An empty library says so. */
	if (picker->count == 0U) {
		view_empty(style, layout);
		return 0;
	}

	/* The grid's measure, the scroll's sizes, and the viewport that takes the wheel and a finger's drag. */
	kl_grid_layout(&layout->grid, VIEW_CELL, VIEW_CELL, picker->count, &grid);
	picker->columns = grid.columns;
	kl_scroll_set_size(&picker->scroll, (double)layout->grid.width, (double)grid.content_height, (double)layout->grid.width, (double)layout->grid.height);
	kl_ui_scroll_region(ui, KEIUI_PICKER_ID_GRID, &layout->grid, &picker->scroll);

	/* The rows in sight, and the items in them. */
	first_row = (size_t)(picker->scroll.y / (double)VIEW_CELL);
	last_row = (size_t)((picker->scroll.y + (double)layout->grid.height) / (double)VIEW_CELL) + 1U;
	picker->first = first_row * (size_t)grid.columns;
	picker->last = last_row * (size_t)grid.columns;
	if (picker->last > picker->count)
		picker->last = picker->count;
	if (picker->first > picker->last)
		picker->first = picker->last;

	/* The thread makes the thumbnails in sight; the finished ones are taken now. */
	keiui_picker_want(picker, picker->first, picker->last);
	pending = keiui_picker_collect(picker, picker->first, picker->last);

	/* Each item in sight, within the grid; a click selects it, a double click chooses it. */
	changed = 0;
	kl_canvas_clip_push(style->canvas, &layout->grid);
	for (index = picker->first; index < picker->last; index++) {
		kl_grid_cell(&grid, index, (int)picker->scroll.y, &cell);
		view_set(&picture, cell.x + VIEW_INSET, cell.y + VIEW_INSET, VIEW_CELL - 2 * VIEW_INSET, VIEW_CELL - 2 * VIEW_INSET);
		state = kl_ui_hit(ui, KEIUI_PICKER_ID_ITEMS, (uint32_t)index, &picture);
		if ((state & KL_HIT_DOUBLE) != 0U) {
			keiui_picker_choose(picker, index);
			changed = 1;
		} else if ((state & KL_HIT_CLICKED) != 0U) {
			keiui_picker_toggle(picker, index);
			changed = 1;
		}

		/* The item as it is after its click. */
		view_item(picker, style, index, &picture, state);
	}

	/* Drawing reaches the whole window again, and the bar shows while the grid moves. */
	kl_canvas_clip_pop(style->canvas);
	(void)kl_scroll_draw_bars(&picker->scroll, style->canvas, &layout->grid, style->theme, now_us);

	/* Reports whether the model changed or thumbnails in sight are still coming. */
	if (changed || pending)
		return 1;
	return 0;
}

/* Draws one item: its thumbnail (or its kind's icon), a video's mark, and its place in the selection. */
static void
view_item(
	const struct keiui_picker *picker,
	const struct kl_style *style,
	size_t index,
	const struct kl_rect *picture,
	unsigned state)
{
	const struct keiui_picker_item *item;
	enum kl_icon icon;
	float x;
	float y;
	float width;
	float height;

	/* The picture's place. */
	item = &picker->items[index];
	x = (float)picture->x;
	y = (float)picture->y;
	width = (float)picture->width;
	height = (float)picture->height;

	/* The thumbnail the view took from the thread, else a quiet ground with the kind's icon. */
	if (item->shown == KEIUI_PICKER_THUMB_READY) {
		kl_canvas_image(style->canvas, &item->image, x, y, width, height, VIEW_THUMB_RADIUS, 1.0f);
	} else {
		kl_canvas_round(style->canvas, x, y, width, height, VIEW_THUMB_RADIUS, VIEW_EMPTY_GROUND);
		icon = KL_ICON_PICTURES;
		if (item->shown != 0 && item->video)
			icon = KL_ICON_MOVIES;
		kl_icon_draw(style->canvas, icon, x + (width - VIEW_ICON) * 0.5f, y + (height - VIEW_ICON) * 0.5f, VIEW_ICON, style->theme->text_faint);
	}

	/* A video's mark, once its kind is known. */
	if (item->shown != 0 && item->video)
		view_video_mark(style, picture);

	/* The item the keys are on: a thin ring. */
	if ((long)index == picker->cursor && item->order == 0U)
		kl_canvas_round_border(style->canvas, x - 2.0f, y - 2.0f, width + 4.0f, height + 4.0f, VIEW_THUMB_RADIUS + 2.0f, 1.5f, style->theme->text_secondary);

	/* A selected item: the accent's ring and its number (a check for a single choice). */
	if (item->order != 0U) {
		kl_canvas_round_border(style->canvas, x - 2.0f, y - 2.0f, width + 4.0f, height + 4.0f, VIEW_THUMB_RADIUS + 2.0f, VIEW_RING, style->theme->accent);
		view_badge(style, picture, item->order, picker->limit);
		return;
	}

	/* Under the pointer: a veil that says it can be clicked. */
	if ((state & KL_HIT_HOT) != 0U)
		kl_canvas_round(style->canvas, x, y, width, height, VIEW_THUMB_RADIUS, KL_RGBA(0xffffff, 40));
}

/* Draws the circle of a selected item at its top right: its number, or a check for a single choice. */
static void
view_badge(
	const struct kl_style *style,
	const struct kl_rect *picture,
	unsigned order,
	size_t limit)
{
	char number[16];
	float cx;
	float cy;
	int width;

	/* The circle in the accent, with a white edge so it reads on any picture. */
	cx = (float)(picture->x + picture->width) - VIEW_BADGE - 4.0f;
	cy = (float)picture->y + VIEW_BADGE + 4.0f;
	kl_canvas_circle(style->canvas, cx, cy, VIEW_BADGE + 1.5f, VIEW_WHITE);
	kl_canvas_circle(style->canvas, cx, cy, VIEW_BADGE, style->theme->accent);

	/* A single choice: a check. */
	if (limit == 1U) {
		kl_canvas_line(style->canvas, cx - 5.0f, cy, cx - 1.5f, cy + 4.0f, 2.0f, style->theme->accent_ink);
		kl_canvas_line(style->canvas, cx - 1.5f, cy + 4.0f, cx + 5.0f, cy - 4.0f, 2.0f, style->theme->accent_ink);
		return;
	}

	/* Several: the item's place in the order. */
	(void)snprintf(number, sizeof(number), "%u", order);
	width = kl_text_width(style->text, number, strlen(number), VIEW_TEXT_BADGE, 1);
	(void)kl_text_draw(style->text, style->canvas, (int)cx - width / 2, kl_text_center(VIEW_TEXT_BADGE, (int)(cy - VIEW_BADGE), (int)(2.0f * VIEW_BADGE)), number, strlen(number), VIEW_TEXT_BADGE, 1, style->theme->accent_ink);
}

/* Draws a video's mark at the bottom left of its picture: a play triangle on a dark chip. */
static void
view_video_mark(
	const struct kl_style *style,
	const struct kl_rect *picture)
{
	float points[6];
	float x;
	float y;

	/* The chip. */
	x = (float)picture->x + 6.0f;
	y = (float)(picture->y + picture->height) - 26.0f;
	kl_canvas_round(style->canvas, x, y, 30.0f, 20.0f, 6.0f, VIEW_VIDEO_GROUND);

	/* The triangle, pointing right. */
	points[0] = x + 11.0f;
	points[1] = y + 5.0f;
	points[2] = x + 11.0f;
	points[3] = y + 15.0f;
	points[4] = x + 20.0f;
	points[5] = y + 10.0f;
	kl_canvas_polygon(style->canvas, points, 3, VIEW_WHITE);
}

/* Draws what an empty library shows: two lines in the middle of the grid. */
static void
view_empty(
	const struct kl_style *style,
	const struct view_layout *layout)
{
	/* Its two lines. */
	view_lines(style, layout, "No photos or videos", "Import them into the library with Photos.");
}

/* Draws two lines in the middle of the grid: a title and the line under it (empty: none). */
static void
view_lines(
	const struct kl_style *style,
	const struct view_layout *layout,
	const char *title,
	const char *line)
{
	int width;
	int middle;

	/* The title, then the line under it. */
	middle = layout->grid.y + layout->grid.height / 2;
	width = kl_text_width(style->text, title, strlen(title), VIEW_TEXT_TITLE, 1);
	(void)kl_text_draw(style->text, style->canvas, layout->grid.x + (layout->grid.width - width) / 2, middle - 6, title, strlen(title), VIEW_TEXT_TITLE, 1, style->theme->text_secondary);
	if (line[0] == '\0')
		return;
	width = kl_text_width(style->text, line, strlen(line), VIEW_TEXT_ROW, 0);
	(void)kl_text_draw(style->text, style->canvas, layout->grid.x + (layout->grid.width - width) / 2, middle + 18, line, strlen(line), VIEW_TEXT_ROW, 0, style->theme->text_faint);
}

/* Draws the bottom bar with Cancel and Add; 1 when the model changed. */
static int
view_bar(
	struct keiui_picker *picker,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct view_layout *layout)
{
	struct kl_rect line;
	char accept[32];
	unsigned flags;
	int pressed;

	/* The line above the bar. */
	view_set(&line, layout->bar.x, layout->bar.y, layout->bar.width, 1);
	kl_canvas_fill(style->canvas, &line, style->theme->separator);

	/* Cancel ends it without items. */
	pressed = kl_button(ui, style, KEIUI_PICKER_ID_CANCEL, &layout->cancel, "Cancel", 0U);
	if (pressed) {
		keiui_picker_cancel(picker);
		return 1;
	}

	/* Add, with the count when several may be chosen, faded while nothing is selected. */
	(void)snprintf(accept, sizeof(accept), "Add");
	if (picker->limit > 1U && picker->selected != 0U)
		(void)snprintf(accept, sizeof(accept), "Add (%zu)", picker->selected);
	flags = KL_BUTTON_PRIMARY;
	if (picker->selected == 0U)
		flags |= KL_BUTTON_DISABLED;
	pressed = kl_button(ui, style, KEIUI_PICKER_ID_ACCEPT, &layout->accept, accept, flags);
	if (pressed) {
		keiui_picker_accept(picker);
		return 1;
	}

	/* Nothing pressed. */
	return 0;
}

/* Draws a refusal's message over the bottom of the grid: a pale red chip as wide as its words. */
static void
view_message(
	const struct keiui_picker *picker,
	const struct kl_style *style,
	const struct view_layout *layout)
{
	struct kl_rect chip;
	int width;

	/* No message, nothing. */
	if (picker->message[0] == '\0')
		return;

	/* The chip at the grid's bottom. */
	width = kl_text_width(style->text, picker->message, strlen(picker->message), VIEW_TEXT_ROW, 0) + 28;
	if (width > layout->grid.width)
		width = layout->grid.width;
	view_set(&chip, layout->grid.x + (layout->grid.width - width) / 2, layout->grid.y + layout->grid.height - 38, width, 32);
	kl_canvas_round(style->canvas, (float)chip.x, (float)chip.y, (float)chip.width, (float)chip.height, 10.0f, VIEW_WARNING_GROUND);
	kl_canvas_round_border(style->canvas, (float)chip.x, (float)chip.y, (float)chip.width, (float)chip.height, 10.0f, 1.0f, VIEW_WARNING_EDGE);
	(void)kl_text_draw_fit(style->text, style->canvas, chip.x + 14, kl_text_center(VIEW_TEXT_ROW, chip.y, chip.height), picker->message, VIEW_TEXT_ROW, 0, chip.width - 28, VIEW_WARNING_TEXT);
}

/* Carries out a key no widget took: the picker's commands; 1 when the model changed. */
static int
view_key(
	struct keiui_picker *picker,
	uint32_t code,
	uint64_t now_us)
{
	long columns;

	/* An answered picker takes nothing more. */
	if (picker->answered)
		return 0;
	columns = (long)picker->columns;
	if (columns < 1L)
		columns = 1L;

	/* Each command. */
	switch (code) {
	case KL_KEY_ESC:
		keiui_picker_cancel(picker);
		return 1;
	case KL_KEY_ENTER:
	case KL_KEY_KPENTER:
		keiui_picker_accept(picker);
		return 1;
	case KL_KEY_SPACE:
		/* Space selects or deselects the item the keys are on. */
		if (picker->cursor >= 0L)
			keiui_picker_toggle(picker, (size_t)picker->cursor);
		return 1;
	case KL_KEY_LEFT:
		view_move(picker, -1L, now_us);
		return 1;
	case KL_KEY_RIGHT:
		view_move(picker, 1L, now_us);
		return 1;
	case KL_KEY_UP:
		view_move(picker, -columns, now_us);
		return 1;
	case KL_KEY_DOWN:
		view_move(picker, columns, now_us);
		return 1;
	default:
		break;
	}

	/* Anything else changes nothing. */
	return 0;
}

/* Moves the item the keys are on by some items, within the grid, and scrolls it into sight. */
static void
view_move(
	struct keiui_picker *picker,
	long delta,
	uint64_t now_us)
{
	struct kl_rect cell;
	long columns;
	long index;

	/* An empty grid has nothing to move to. */
	if (picker->count == 0U)
		return;

	/* From the item the keys are on, or from the first. */
	index = picker->cursor;
	if (index < 0L)
		index = 0L;
	else
		index += delta;

	/* Within the grid. */
	if (index < 0L)
		index = 0L;
	if (index >= (long)picker->count)
		index = (long)picker->count - 1L;
	picker->cursor = index;

	/* Its row in sight (content coordinates). */
	columns = (long)picker->columns;
	if (columns < 1L)
		columns = 1L;
	view_set(&cell, 0, (int)(index / columns) * VIEW_CELL, VIEW_CELL, VIEW_CELL);
	kl_scroll_reveal(&picker->scroll, &cell, now_us);
}

/* Sets a rectangle. */
static void
view_set(
	struct kl_rect *rect,
	int x,
	int y,
	int width,
	int height)
{
	/* Its corner and size. */
	rect->x = x;
	rect->y = y;
	rect->width = width;
	rect->height = height;
}
