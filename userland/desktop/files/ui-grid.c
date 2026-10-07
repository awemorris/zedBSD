/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The content panel of files: the place's title and its items as
 * a grid of icons (spec §11).
 *
 * Only the rows in sight are drawn.  A folder's item count, shown under
 * its icon, is counted the first time the folder is drawn and kept with
 * the entry.
 */

#include "files.h"

#include <keiland/keiland.h>

#include <stdio.h>
#include <string.h>

/* The panel's padding, and the height of its title row. */
#define GRID_PADDING		16
#define GRID_TITLE_HEIGHT	58

/* One cell of the grid, and the icon in it. */
#define GRID_CELL_WIDTH		112
#define GRID_CELL_HEIGHT	132
#define GRID_ICON		64

/* The text sizes of the panel. */
#define GRID_TEXT_TITLE		20U
#define GRID_TEXT_COUNT		13U
#define GRID_TEXT_NAME		13U
#define GRID_TEXT_DETAIL	11U

/* The panel's corner radius. */
#define GRID_RADIUS		16.0f

/* The faint Kei mark over an empty place's words, in pixels a side (ws035-p108). */
#define GRID_MARK		72

static void grid_panel(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area);
static void grid_title(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area);
static void grid_items(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *inner);
static void grid_cell(struct fm_app *app, struct kl_canvas *canvas, struct fm_entry *entry, int index, const struct kl_rect *slot);
static void grid_message(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *inner, const char *message, const char *hint);
static void grid_band(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *inner);
static void grid_status(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area);
static void grid_trash_buttons(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area);
static void grid_scope_chips(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area);
static void grid_button(struct fm_app *app, struct kl_canvas *canvas, int right, int y, const char *label, int index, int enabled);
static void grid_thumbnail(struct kl_canvas *canvas, const struct kl_image *thumb, float x, float y, float size);

/*
 * Draws the content panel for the place the tab shows.
 */
void
fm_grid_draw(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area)
{
	struct kl_rect inner;
	struct fm_tab *tab;
	char message[128];

	/* The panel. */
	grid_panel(app, canvas, area);
	tab = fm_ui_tab(app);

	/* Today, the dashboard, fills the panel itself. */
	if (tab->history[tab->history_index].location.kind == FM_LOCATION_TODAY) {
		inner.x = area->x + GRID_PADDING;
		inner.y = area->y + GRID_PADDING;
		inner.width = area->width - 2 * GRID_PADDING;
		inner.height = area->height - 2 * GRID_PADDING;
		app->layout.items = inner;
		fm_ui_hit(app, area, FM_HIT_CONTENT, 0);
		fm_home_draw(app, canvas, &inner);
		grid_status(app, canvas, area);
		return;
	}


	/* The whole panel's ground can be clicked (a rubber band starts there); what is drawn on it comes after. */
	fm_ui_hit(app, area, FM_HIT_CONTENT, 0);
	grid_title(app, canvas, area);

	/* The items' part of the panel, under the title. */
	inner.x = area->x + GRID_PADDING;
	inner.y = area->y + GRID_TITLE_HEIGHT;
	inner.width = area->width - 2 * GRID_PADDING;
	inner.height = area->height - GRID_TITLE_HEIGHT - 4;
	app->layout.content_height = 0;
	app->layout.items = inner;

	/* A folder that could not be read says so. */
	if (tab->listing.error != 0) {
		snprintf(message, sizeof(message), "%s", kl_tr("This folder can't be opened."));
		grid_message(app, canvas, &inner, message, NULL);
		return;
	}

	/* Empty Recents while the list is stopped says why, and where to start it (ws177-p008). */
	if (tab->listing.count == 0 &&
	    tab->history[tab->history_index].location.kind == FM_LOCATION_RECENTS &&
	    app->recents_off) {
		grid_message(app, canvas, &inner, kl_tr("Recent items are not kept"), kl_tr("Turn on Keep recent items in Settings > Storage."));
		return;
	}

	/* An empty place says so too. */
	if (tab->listing.count == 0) {
		grid_message(app, canvas, &inner, kl_tr("Nothing here"), NULL);
		return;
	}

	/* The items, as a list or a grid, and the rubber band over them. */
	kl_canvas_clip_push(canvas, &inner);
	if (app->view == FM_VIEW_LIST) {
		fm_list_draw(app, canvas, &inner);
	} else {
		grid_items(app, canvas, &inner);
	}

	/* The rubber band over the items. */
	grid_band(app, canvas, &inner);
	kl_canvas_clip_pop(canvas);

	/* The status pill, when there is something to say. */
	grid_status(app, canvas, area);
}

/*
 * Reports where an item is drawn in the last frame's layout (scroll
 * included), for the keyboard and the rubber band.
 */
void
fm_view_item_rect(
	struct fm_app *app,
	int index,
	struct kl_rect *rect)
{
	struct fm_tab *tab;
	int columns;

	/* The list: one row an item under the header. */
	tab = fm_ui_tab(app);
	if (app->view == FM_VIEW_LIST) {
		rect->x = app->layout.items.x;
		rect->y = app->layout.items.y + FM_LIST_HEADER + index * FM_LIST_ROW - tab->scroll;
		rect->width = app->layout.items.width;
		rect->height = FM_LIST_ROW;
		return;
	}

	/* The grid: the cell of its row and column. */
	columns = app->layout.columns;
	if (columns < 1)
		columns = 1;
	rect->x = app->layout.grid_left + (index % columns) * GRID_CELL_WIDTH + 4;
	rect->y = app->layout.items.y + (index / columns) * GRID_CELL_HEIGHT - tab->scroll + 2;
	rect->width = GRID_CELL_WIDTH - 8;
	rect->height = GRID_CELL_HEIGHT - 4;
}

/*
 * Draws an entry's icon in a square box: a folder, a picture's thumbnail
 * once it is made, or a page of its kind's color.
 */
void
fm_grid_entry_icon(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct fm_entry *entry,
	float x,
	float y,
	float size)
{
	const struct kl_image *thumb;
	char label[8];
	int kind;

	/* A folder is blue. */
	if (entry->folder != 0) {
		kl_icon_folder(canvas, x, y, size, FM_COLOR_FOLDER);
		return;
	}

	/* A picture with a path shows its thumbnail, once it is made (it is asked for until then). */
	thumb = NULL;
	kind = fm_thumb_kind(entry);
	if (kind != 0)
		thumb = fm_thumb_get(app, entry->path, entry->modified);
	if (thumb != NULL) {
		grid_thumbnail(canvas, thumb, x, y, size);
		return;
	}

	/* A file is a page with its kind's band and its extension. */
	fm_mime_label(entry->name, label, sizeof(label));
	kl_icon_file(canvas, app->text, x, y, size, fm_mime_color(entry->mime->category), label);
}

/* Draws the content's card: the white panel with its shadow and edge, or its tint on the compositor's glass. */
static void
grid_panel(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area)
{
	/* The whole card, the row of tabs included (the content is under the row). */
	area = &app->layout.card;

	/* On glass, the compositor draws the card, its rim and its shadow: only a light white tint for reading. */
	if (app->glass != 0) {
		kl_canvas_round(canvas, (float)area->x, (float)area->y, (float)area->width, (float)area->height, GRID_RADIUS, FM_COLOR_GLASS_CONTENT);
		return;
	}

	/* The shadow, the white panel and its thin edge. */
	kl_canvas_shadow(canvas, (float)area->x, (float)area->y + 4.0f, (float)area->width, (float)area->height, GRID_RADIUS, 14.0f, FM_COLOR_SHADOW);
	kl_canvas_round(canvas, (float)area->x, (float)area->y, (float)area->width, (float)area->height, GRID_RADIUS, FM_COLOR_PANEL);
	kl_canvas_round_border(canvas, (float)area->x, (float)area->y, (float)area->width, (float)area->height, GRID_RADIUS, 1.0f, FM_COLOR_PANEL_EDGE);
}

/* Draws the place's name and how many items it holds. */
static void
grid_title(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area)
{
	struct fm_tab *tab;
	const struct fm_location *location;
	const char *name;
	char title[FM_PATH_MAX + 8];
	char count[64];
	int baseline;
	int width;
	int left;

	/* The place's name, large (a search says what it looks for). */
	tab = fm_ui_tab(app);
	location = &tab->history[tab->history_index].location;
	name = kl_tr(fm_location_name(location, app->home));
	if (location->kind == FM_LOCATION_SEARCH) {
		snprintf(title, sizeof(title), "\u201c%s\u201d", location->path);
		name = title;
	}

	/* Its baseline and left end. */
	baseline = area->y + 38;
	left = area->x + 24;

	/* The name itself. */
	width = kl_text_draw_fit(app->text, canvas, left, baseline, name, GRID_TEXT_TITLE, 1, area->width / 2, FM_COLOR_TEXT);
	width += left - (area->x + 24);

	/* In the trash: Put Back (for a selection) and Empty Trash at the right. */
	if (location->kind == FM_LOCATION_TRASH)
		grid_trash_buttons(app, canvas, area);

	/* In the recent files: Clear Recents at the right (q824). */
	if (location->kind == FM_LOCATION_RECENTS)
		grid_button(app, canvas, area->x + area->width - 20, area->y + 18, "Clear Recents", FM_BUTTON_CLEAR_RECENTS, tab->listing.count != 0U);

	/* A search: where it looks, as three chips at the right. */
	if (location->kind == FM_LOCATION_SEARCH)
		grid_scope_chips(app, canvas, area);

	/* How many items, after it, quietly (a search still walking says so). */
	if (tab->listing.error != 0)
		return;
	fm_dir_items_text((long)tab->listing.count, count, sizeof(count));
	if (location->kind == FM_LOCATION_SEARCH && app->search.active != 0)
		snprintf(count + strlen(count), sizeof(count) - strlen(count), ", searching\u2026");
	(void)kl_text_draw(app->text, canvas, area->x + 24 + width + 12, baseline, count, strlen(count), GRID_TEXT_COUNT, 0, FM_COLOR_TEXT_SECONDARY);
}

/* Draws the rows of cells that are in sight with libkeiland's grid (ws090-p024), and records each cell. */
static void
grid_items(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *inner)
{
	struct kl_grid grid;
	struct kl_rect cell;
	struct fm_tab *tab;
	size_t index;

	/* As many columns as fit, the grid centred. */
	tab = fm_ui_tab(app);
	kl_grid_layout(inner, GRID_CELL_WIDTH, GRID_CELL_HEIGHT, tab->listing.count, &grid);

	/* The grid's measure, which scrolling and the keyboard use. */
	app->layout.columns = grid.columns;
	app->layout.grid_left = grid.left;
	app->layout.cell_width = GRID_CELL_WIDTH;
	app->layout.cell_height = GRID_CELL_HEIGHT;
	app->layout.content_height = grid.content_height + GRID_TITLE_HEIGHT + 8;

	/* Each cell whose row is in sight. */
	for (index = 0; index < tab->listing.count; index++) {
		kl_grid_cell(&grid, index, tab->scroll, &cell);
		if (cell.y + GRID_CELL_HEIGHT < inner->y || cell.y > inner->y + inner->height)
			continue;
		grid_cell(app, canvas, &tab->listing.entries[index], (int)index, &cell);
	}
}

/* Draws one cell: libkeiland's ground and name (ws090-p024), the icon and the detail line. */
static void
grid_cell(
	struct fm_app *app,
	struct kl_canvas *canvas,
	struct fm_entry *entry,
	int index,
	const struct kl_rect *slot)
{
	struct kl_style style;
	struct kl_rect cell;
	struct kl_rect field;
	struct kl_rect icon;
	const char *name;
	char detail[64];
	unsigned state;
	int renaming;
	int match;
	int baseline;
	int width;

	/* The cell's rectangle, recorded as the item. */
	cell.x = slot->x + 4;
	cell.y = slot->y + 2;
	cell.width = GRID_CELL_WIDTH - 8;
	cell.height = GRID_CELL_HEIGHT - 4;

	/* The name being changed is a field, drawn after the ground. */
	renaming = 0;
	if (app->focus == FM_FOCUS_RENAME) {
		match = strcmp(entry->path, app->rename_path);
		if (match == 0)
			renaming = 1;
	}

	/* The ground for the item's state and its name (none while it is being changed). */
	state = 0U;
	if (entry->selected != 0)
		state |= KL_ITEM_SELECTED;
	if (app->focused != 0)
		state |= KL_ITEM_FOCUSED;
	if (app->hover_kind == FM_HIT_ITEM && app->hover_index == index)
		state |= KL_ITEM_HOVER;
	name = entry->name;
	if (renaming != 0)
		name = NULL;
	fm_style(app, canvas, &style);
	baseline = kl_grid_item(&style, slot, GRID_ICON, name, GRID_TEXT_NAME, state);

	/* The icon, faded when the item is cut. */
	kl_grid_icon(slot, GRID_ICON, &icon);
	fm_grid_entry_icon(app, canvas, entry, (float)icon.x, (float)icon.y, (float)GRID_ICON);
	if (entry->cut != 0)
		kl_canvas_round(canvas, (float)icon.x, (float)icon.y, (float)GRID_ICON, (float)GRID_ICON, 8.0f, FM_COLOR_TILE);

	/* The field, white with the accent edge, where the name was. */
	if (renaming != 0) {
		field.x = slot->x + 2;
		field.y = slot->y + GRID_ICON + 16;
		field.width = GRID_CELL_WIDTH - 4;
		field.height = 24;
		fm_rename_draw(app, canvas, &field);
		baseline = slot->y + GRID_ICON + 32;
	}

	/* The detail: a folder's item count, a file's size. */
	if (entry->folder != 0) {
		if (entry->child_count < 0)
			entry->child_count = fm_dir_count(entry->path, app->show_hidden);
		fm_dir_items_text(entry->child_count, detail, sizeof(detail));
	} else {
		fm_dir_size_text(entry->size, detail, sizeof(detail));
	}

	/* The detail, centred under the name. */
	width = kl_text_width(app->text, detail, strlen(detail), GRID_TEXT_DETAIL, 0);
	(void)kl_text_draw(app->text, canvas, slot->x + (GRID_CELL_WIDTH - width) / 2, baseline + 16, detail, strlen(detail), GRID_TEXT_DETAIL, 0, FM_COLOR_TEXT_SECONDARY);

	/* The cell can be clicked. */
	fm_ui_hit(app, &cell, FM_HIT_ITEM, index);
}

/* Draws a quiet message in the middle of the panel. */
static void
grid_message(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *inner,
	const char *message,
	const char *hint)
{
	int width;

	/* The Kei mark, faint, above the words (ws035-p108). */
	fm_mark_draw(canvas, inner->x + (inner->width - GRID_MARK) / 2, inner->y + inner->height / 2 - 44 - GRID_MARK, (unsigned)GRID_MARK, 0.45f);

	/* Centred, faint. */
	width = kl_text_width(app->text, message, strlen(message), 15U, 0);
	(void)kl_text_draw(app->text, canvas, inner->x + (inner->width - width) / 2, inner->y + inner->height / 2 - 20, message, strlen(message), 15U, 0, FM_COLOR_TEXT_FAINT);

	/* What to do, smaller under it, when there is something (ws177-p008). */
	if (hint == NULL)
		return;
	width = kl_text_width(app->text, hint, strlen(hint), 13U, 0);
	(void)kl_text_draw(app->text, canvas, inner->x + (inner->width - width) / 2, inner->y + inner->height / 2 + 4, hint, strlen(hint), 13U, 0, FM_COLOR_TEXT_SECONDARY);
}

/* Draws the rubber band being dragged with libkeiland's (ws090-p024). */
static void
grid_band(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *inner)
{
	struct kl_style style;
	struct kl_rect band;
	struct fm_tab *tab;
	int left;
	int top;
	int right;
	int bottom;

	/* No band is being dragged. */
	if (app->band == 0)
		return;

	/* Its corners in order, on the screen. */
	tab = fm_ui_tab(app);
	left = app->band_x0;
	right = app->band_x1;
	if (left > right) {
		left = app->band_x1;
		right = app->band_x0;
	}

	/* And its top and bottom. */
	top = app->band_y0;
	bottom = app->band_y1;
	if (top > bottom) {
		top = app->band_y1;
		bottom = app->band_y0;
	}

	/* On the screen, the scroll taken off. */
	(void)inner;
	band.x = left;
	band.y = top - tab->scroll;
	band.width = right - left;
	band.height = bottom - top;

	/* The box and its edge. */
	fm_style(app, canvas, &style);
	kl_band(&style, &band);
}

/* Draws the status pill at the bottom of the panel: a message, or the selection's count and size. */
static void
grid_status(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area)
{
	struct kl_style style;
	struct fm_tab *tab;
	uint64_t bytes;
	size_t count;
	char text[256];
	char size[32];

	/* A message while it lasts. */
	tab = fm_ui_tab(app);
	text[0] = '\0';
	if (app->message[0] != '\0' && app->now < app->message_until)
		snprintf(text, sizeof(text), "%s", app->message);

	/* Otherwise the running operation's progress (spec §32). */
	if (text[0] == '\0' && app->task_count > 0)
		fm_task_text(app->tasks[0], text, sizeof(text));

	/* Otherwise two or more selected items: how many and how large. */
	if (text[0] == '\0') {
		count = fm_select_count(tab, &bytes);
		if (count < 2U)
			return;
		snprintf(text, sizeof(text), "%lu items selected", (unsigned long)count);

		/* The size, when files (not only folders) are among them. */
		if (bytes != 0U) {
			fm_dir_size_text(bytes, size, sizeof(size));
			snprintf(text, sizeof(text), "%lu items selected \xe2\x80\x94 %s", (unsigned long)count, size);
		}
	}

	/* libkeiland's chip, centred at the bottom (ws090-p023). */
	fm_style(app, canvas, &style);
	kl_chip(&style, area->x + area->width / 2, area->y + area->height - 16, text);
}

/* Draws the trash's buttons at the right of the title: Put Back (with a selection) and Empty Trash. */
static void
grid_trash_buttons(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area)
{
	struct fm_tab *tab;
	uint64_t bytes;
	size_t selected;
	int right;

	/* Empty Trash at the right end, Put Back before it. */
	tab = fm_ui_tab(app);
	selected = fm_select_count(tab, &bytes);
	right = area->x + area->width - 20;
	grid_button(app, canvas, right, area->y + 18, "Empty Trash", FM_BUTTON_EMPTY_TRASH, tab->listing.count != 0U);
	grid_button(app, canvas, right - 120, area->y + 18, "Put Back", FM_BUTTON_PUT_BACK, selected != 0U);
}

/* Draws a small pill button ending at a right edge; a disabled one is pale and not clickable. */
static void
grid_button(
	struct fm_app *app,
	struct kl_canvas *canvas,
	int right,
	int y,
	const char *label,
	int index,
	int enabled)
{
	struct kl_rect rect;
	unsigned flags;

	/* libkeiland's button ending at the right edge (ws090-p023); a disabled one is faded and not clickable. */
	rect.width = fm_button_width(app, label);
	rect.x = right - rect.width;
	rect.y = y;
	rect.height = 28;
	flags = 0U;
	if (enabled == 0)
		flags = KL_BUTTON_DISABLED;
	fm_button(app, canvas, &rect, label, index, flags);
}

/* Draws the search's scope as three chips at the right of the title: This Folder, Home, Computer. */
static void
grid_scope_chips(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area)
{
	static const char *const labels[] = { "This Folder", "Home", "Computer" };
	struct kl_rect chip;
	kl_color ground;
	kl_color ink;
	int widths[3];
	int right;
	int index;

	/* The chips' widths, laid out from the right edge. */
	right = area->x + area->width - 20;
	for (index = 2; index >= 0; index--) {
		widths[index] = kl_text_width(app->text, labels[index], strlen(labels[index]), 12U, 1) + 24;
		right -= widths[index] + 6;
	}

	/* Each chip, the scope in force in the accent. */
	chip.x = right + 6;
	chip.y = area->y + 20;
	chip.height = 26;
	for (index = 0; index < 3; index++) {
		chip.width = widths[index];
		ground = FM_COLOR_BUTTON;
		ink = FM_COLOR_TEXT_SECONDARY;
		if ((unsigned)index == app->search_scope) {
			ground = FM_COLOR_SELECTION;
			ink = FM_COLOR_ACCENT_TEXT;
		} else if (app->hover_kind == FM_HIT_SCOPE && app->hover_index == index) {
			ground = FM_COLOR_BUTTON_LIT;
		}

		/* The chip and its label. */
		kl_canvas_round(canvas, (float)chip.x, (float)chip.y, (float)chip.width, (float)chip.height, 13.0f, ground);
		(void)kl_text_draw(app->text, canvas, chip.x + 12, kl_text_center(12U, chip.y, chip.height), labels[index], strlen(labels[index]), 12U, 1, ink);
		fm_ui_hit(app, &chip, FM_HIT_SCOPE, index);
		chip.x += chip.width + 6;
	}
}

/* Draws a thumbnail fitted in an icon's square: a large one in a white frame with a shadow, a small one bare. */
static void
grid_thumbnail(
	struct kl_canvas *canvas,
	const struct kl_image *thumb,
	float x,
	float y,
	float size)
{
	float left;
	float top;
	float frame;
	int width;
	int height;

	/* The frame's width: none for a small icon (a list's row). */
	frame = 0.0f;
	if (size >= 48.0f)
		frame = 3.0f;

	/* The picture fitted in the square inside the frame, in its middle. */
	fm_image_fit(thumb->width, thumb->height, (int)(size - 2.0f * frame), (int)(size - 2.0f * frame), &width, &height);
	left = x + (size - (float)width) * 0.5f;
	top = y + (size - (float)height) * 0.5f;

	/* A large icon's white frame and soft shadow. */
	if (frame > 0.0f) {
		kl_canvas_shadow(canvas, left - frame, top - frame + 2.0f, (float)width + 2.0f * frame, (float)height + 2.0f * frame, 5.0f, 6.0f, FM_COLOR_SHADOW);
		kl_canvas_round(canvas, left - frame, top - frame, (float)width + 2.0f * frame, (float)height + 2.0f * frame, 5.0f, FM_COLOR_PANEL);
	}

	/* The picture. */
	kl_canvas_image(canvas, thumb, left, top, (float)width, (float)height, 3.0f, 1.0f);
}
