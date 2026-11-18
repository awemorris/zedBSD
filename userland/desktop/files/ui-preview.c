/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The preview of files: the pane at the right of the content
 * (spec §17) and Quick Look, the large look at one item drawn over the
 * window (spec §18).
 *
 * Both show the item the keyboard's cursor is on when it is selected, or
 * else the first selected item.  The pane also sums up a selection of
 * several items, and shows the place itself when nothing is selected.
 * What they show of a file's contents (its type, its first lines, its
 * picture) is read by peek.c for the one file shown; a picture's small
 * version comes from the thumbnails (thumb.c).
 */

#include "files.h"

#include <keiland/keiland.h>

#include <stdio.h>
#include <string.h>

/* The pane's padding, the height of its picture, and the size of an icon drawn instead. */
#define PREVIEW_PADDING		16
#define PREVIEW_PICTURE		184
#define PREVIEW_ICON		112

/* The height of a row of the pane's information, and the most lines of text it shows. */
#define PREVIEW_ROW		22
#define PREVIEW_TEXT_LINES	12
#define PREVIEW_TEXT_LINE	15

/* The pane's text sizes. */
#define PREVIEW_TEXT_NAME	15U
#define PREVIEW_TEXT_INFO	12U
#define PREVIEW_TEXT_HEADER	11U
#define PREVIEW_TEXT_BODY	11U

/* The panel's corner radius. */
#define PREVIEW_RADIUS		16.0f

/* Quick Look: the margins it keeps from the window's edges, its header's height, and its card's corner. */
#define LOOK_MARGIN_X		60
#define LOOK_MARGIN_Y		40
#define LOOK_HEADER		56
#define LOOK_RADIUS		18.0f

/* Quick Look's picture: the largest side it is read at, how many times its own size it is grown at most, and the size a tiny one may grow to anyway. */
#define LOOK_PICTURE_SIDE	1600
#define LOOK_PICTURE_GROWTH	3
#define LOOK_PICTURE_SMALLEST	160

/* Quick Look's card for text and for other items, and its text's size and line height. */
#define LOOK_TEXT_WIDTH		720
#define LOOK_OTHER_WIDTH	420
#define LOOK_OTHER_HEIGHT	300
#define LOOK_TEXT_BODY		13U
#define LOOK_TEXT_LINE		19

static void preview_panel(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area);
static void preview_place(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area);
static void preview_many(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area, size_t count, uint64_t bytes);
static void preview_one(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area, struct fm_entry *entry);
static int preview_picture(struct fm_app *app, struct kl_canvas *canvas, const struct fm_entry *entry, const struct kl_rect *box);
static int preview_name(struct fm_app *app, struct kl_canvas *canvas, const char *name, int x, int width, int baseline);
static int preview_row(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area, int y, const char *label, const char *value);
static int preview_text(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area, int y);
static void preview_size_text(struct fm_app *app, struct fm_entry *entry, char *text, size_t size);
static void preview_where(struct fm_app *app, const char *path, char *text, size_t size);
static void preview_centered(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *area, int baseline, const char *text, unsigned pixels, int bold, kl_color color);
static void look_card(struct fm_app *app, struct kl_canvas *canvas, int width, int height, struct kl_rect *card);
static void look_header(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *card, struct fm_entry *entry);
static void look_picture(struct fm_app *app, struct kl_canvas *canvas, struct fm_entry *entry, int room_width, int room_height);
static void look_text(struct fm_app *app, struct kl_canvas *canvas, struct fm_entry *entry, int room_width, int room_height);
static void look_other(struct fm_app *app, struct kl_canvas *canvas, struct fm_entry *entry);

/*
 * Draws the preview pane in its area: the selected item, a summary of
 * several, or the place shown when nothing is selected.
 */
void
fm_preview_draw(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area)
{
	struct fm_tab *tab;
	uint64_t bytes;
	size_t count;
	int item;

	/* The panel, and nothing drawn outside it. */
	preview_panel(app, canvas, area);
	kl_canvas_clip_push(canvas, area);

	/* How many items are selected, and the one shown. */
	tab = fm_ui_tab(app);
	count = fm_select_count(tab, &bytes);
	item = fm_preview_item(app);

	/* Nothing selected: the place; several: their sum; one: the item itself. */
	if (count == 0U || item < 0) {
		preview_place(app, canvas, area);
	} else if (count >= 2U) {
		preview_many(app, canvas, area, count, bytes);
	} else {
		preview_one(app, canvas, area, &tab->listing.entries[item]);
	}

	/* The panel's clip ends. */
	kl_canvas_clip_pop(canvas);
}

/*
 * Draws Quick Look over the window when it is open: the item shown large
 * on a card in the middle, the rest of the window dimmed.
 */
void
fm_look_draw(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	struct fm_entry *entry;
	struct fm_tab *tab;
	struct kl_rect whole;
	int room_width;
	int room_height;
	int textual;
	int item;

	/* Closed, nothing is drawn. */
	if (app->quicklook == 0)
		return;

	/* The item it shows; with no item left (the folder changed under it), it closes. */
	tab = fm_ui_tab(app);
	item = fm_preview_item(app);
	if (item < 0) {
		fm_look_close(app);
		return;
	}

	/* The item's entry. */
	entry = &tab->listing.entries[item];

	/* The window dimmed; a click on it closes Quick Look. */
	whole.x = 0;
	whole.y = 0;
	whole.width = app->width;
	whole.height = app->height;
	kl_canvas_fill(canvas, &whole, KL_RGBA(0x1b2233, 110));
	fm_ui_hit(app, &whole, FM_HIT_OVERLAY, FM_OVERLAY_LOOK_GROUND);

	/* What is read of the item, and the room the card has inside the margins. */
	fm_peek_read(&app->peek, entry);
	room_width = app->width - 2 * LOOK_MARGIN_X;
	room_height = app->height - 2 * LOOK_MARGIN_Y;

	/* A picture large; text as its first lines; anything else as a large icon. */
	textual = 0;
	if (app->peek.text != NULL)
		textual = 1;
	if (entry->folder == 0 && entry->mime->category == FM_CATEGORY_IMAGE) {
		look_picture(app, canvas, entry, room_width, room_height);
	} else if (textual != 0) {
		look_text(app, canvas, entry, room_width, room_height);
	} else {
		look_other(app, canvas, entry);
	}
}

/*
 * Opens Quick Look on the item shown, or closes it when it is open.
 */
void
fm_look_toggle(
	struct fm_app *app)
{
	struct fm_tab *tab;
	int item;

	/* Open: it closes. */
	if (app->quicklook != 0) {
		fm_look_close(app);
		return;
	}

	/* Nothing selected: nothing to look at. */
	tab = fm_ui_tab(app);
	item = fm_preview_item(app);
	if (item < 0)
		return;

	/* It opens on the item. */
	app->quicklook = 1;
	app->dirty = 1;
	fm_log("LOOK open path=%s", tab->listing.entries[item].path);
}

/*
 * Closes Quick Look.
 */
void
fm_look_close(
	struct fm_app *app)
{
	/* Already closed. */
	if (app->quicklook == 0)
		return;

	/* The window shows again. */
	app->quicklook = 0;
	app->dirty = 1;
	fm_log("LOOK close");
}

/*
 * Moves Quick Look to the item before (step -1) or after (step 1): within
 * the selection when several items are selected, else to the next item of
 * the place, which becomes the selection.
 */
void
fm_look_step(
	struct fm_app *app,
	int step)
{
	struct fm_tab *tab;
	uint64_t bytes;
	size_t count;
	int target;
	int item;
	int turns;

	/* The item shown now. */
	tab = fm_ui_tab(app);
	item = fm_preview_item(app);
	if (item < 0 || tab->listing.count == 0U)
		return;

	/* One item selected: the neighbour becomes the selection, stopping at the ends. */
	count = fm_select_count(tab, &bytes);
	if (count < 2U) {
		target = item + step;
		if (target < 0 || (size_t)target >= tab->listing.count)
			return;
		fm_select_only(tab, target);
		app->dirty = 1;
		fm_log("LOOK path=%s", tab->listing.entries[target].path);
		return;
	}

	/* Several: the next selected item, going round, gets the cursor and the selection stays. */
	target = item;
	for (turns = 0; turns < (int)tab->listing.count; turns++) {
		target += step;
		if (target < 0)
			target = (int)tab->listing.count - 1;
		if ((size_t)target >= tab->listing.count)
			target = 0;
		if (tab->listing.entries[target].selected != 0)
			break;
	}

	/* The cursor on it. */
	tab->cursor = target;
	app->dirty = 1;
	fm_log("LOOK path=%s", tab->listing.entries[target].path);
}

/*
 * Reports which item the preview and Quick Look show: the one with the
 * keyboard's cursor when it is selected, else the first selected one; -1
 * when nothing is selected.
 */
int
fm_preview_item(
	struct fm_app *app)
{
	struct fm_tab *tab;
	size_t index;

	/* The cursor's item, when it is selected. */
	tab = fm_ui_tab(app);
	if (tab->cursor >= 0 && (size_t)tab->cursor < tab->listing.count) {
		if (tab->listing.entries[tab->cursor].selected != 0)
			return tab->cursor;
	}

	/* Otherwise the first selected item. */
	for (index = 0; index < tab->listing.count; index++) {
		if (tab->listing.entries[index].selected != 0)
			return (int)index;
	}

	/* Nothing is selected. */
	return -1;
}

/* Draws the pane's white panel with its shadow and edge, or its tint on the compositor's glass. */
static void
preview_panel(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area)
{
	/* On glass, the compositor draws the card: only the content's white tint. */
	if (app->glass != 0) {
		kl_canvas_round(canvas, (float)area->x, (float)area->y, (float)area->width, (float)area->height, PREVIEW_RADIUS, FM_COLOR_GLASS_CONTENT);
		return;
	}

	/* The shadow, the panel and its thin edge, as the content's. */
	kl_canvas_shadow(canvas, (float)area->x, (float)area->y + 4.0f, (float)area->width, (float)area->height, PREVIEW_RADIUS, 14.0f, FM_COLOR_SHADOW);
	kl_canvas_round(canvas, (float)area->x, (float)area->y, (float)area->width, (float)area->height, PREVIEW_RADIUS, FM_COLOR_PANEL);
	kl_canvas_round_border(canvas, (float)area->x, (float)area->y, (float)area->width, (float)area->height, PREVIEW_RADIUS, 1.0f, FM_COLOR_PANEL_EDGE);
}

/* Draws the place shown when nothing is selected: a large folder, its name, how many items it holds. */
static void
preview_place(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area)
{
	const struct fm_location *location;
	struct fm_tab *tab;
	char count[64];
	int baseline;
	int top;

	/* A large folder at the top. */
	tab = fm_ui_tab(app);
	location = &tab->history[tab->history_index].location;
	top = area->y + PREVIEW_PADDING + (PREVIEW_PICTURE - PREVIEW_ICON) / 2;
	kl_icon_folder(canvas, (float)(area->x + (area->width - PREVIEW_ICON) / 2), (float)top, (float)PREVIEW_ICON, FM_COLOR_FOLDER);

	/* The place's name under it. */
	baseline = area->y + PREVIEW_PADDING + PREVIEW_PICTURE + 24;
	preview_centered(app, canvas, area, baseline, kl_tr(fm_location_name(location, app->home)), PREVIEW_TEXT_NAME, 1, FM_COLOR_TEXT);

	/* How many items it holds (the dashboard lists none of its own). */
	if (location->kind != FM_LOCATION_TODAY && tab->listing.error == 0) {
		fm_dir_items_text((long)tab->listing.count, count, sizeof(count));
		preview_centered(app, canvas, area, baseline + 22, count, PREVIEW_TEXT_INFO, 0, FM_COLOR_TEXT_SECONDARY);
	}

	/* What the pane is for. */
	preview_centered(app, canvas, area, baseline + 56, "Select an item to preview it.", PREVIEW_TEXT_INFO, 0, FM_COLOR_TEXT_FAINT);
}

/* Draws the sum of several selected items: their icons fanned out, how many and how large. */
static void
preview_many(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area,
	size_t count,
	uint64_t bytes)
{
	struct fm_tab *tab;
	char text[64];
	char size[32];
	size_t index;
	float x;
	float y;
	int drawn;
	int baseline;

	/* Up to three of the selected items' icons, each a little lower right of the one before. */
	tab = fm_ui_tab(app);
	drawn = 0;
	x = (float)(area->x + (area->width - PREVIEW_ICON) / 2) - 16.0f;
	y = (float)(area->y + PREVIEW_PADDING + (PREVIEW_PICTURE - PREVIEW_ICON) / 2) - 12.0f;
	for (index = 0; index < tab->listing.count && drawn < 3; index++) {
		if (tab->listing.entries[index].selected == 0)
			continue;
		fm_grid_entry_icon(app, canvas, &tab->listing.entries[index], x + (float)drawn * 16.0f, y + (float)drawn * 12.0f, (float)PREVIEW_ICON);
		drawn++;
	}

	/* How many. */
	baseline = area->y + PREVIEW_PADDING + PREVIEW_PICTURE + 24;
	fm_dir_items_text((long)count, text, sizeof(text));
	preview_centered(app, canvas, area, baseline, text, PREVIEW_TEXT_NAME, 1, FM_COLOR_TEXT);

	/* How large, when files (not only folders) are among them. */
	if (bytes != 0U) {
		fm_dir_size_text(bytes, size, sizeof(size));
		snprintf(text, sizeof(text), "%s in all", size);
		preview_centered(app, canvas, area, baseline + 22, text, PREVIEW_TEXT_INFO, 0, FM_COLOR_TEXT_SECONDARY);
	}
}

/* Draws one item: its picture or icon, its name, its information and, for text, its first lines. */
static void
preview_one(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area,
	struct fm_entry *entry)
{
	struct kl_rect box;
	char modified[64];
	char where[FM_PATH_MAX];
	char size[64];
	int baseline;
	int y;

	/* What is read of the file: the type its contents tell, and its first lines. */
	fm_peek_read(&app->peek, entry);

	/* The picture, or a large icon, at the top. */
	box.x = area->x + PREVIEW_PADDING;
	box.y = area->y + PREVIEW_PADDING;
	box.width = area->width - 2 * PREVIEW_PADDING;
	box.height = PREVIEW_PICTURE;
	(void)preview_picture(app, canvas, entry, &box);

	/* The name, in up to two lines, and the kind under it. */
	baseline = preview_name(app, canvas, entry->name, box.x, box.width, box.y + box.height + 24);
	preview_centered(app, canvas, area, baseline + 20, app->peek.mime->kind, PREVIEW_TEXT_INFO, 0, FM_COLOR_TEXT_SECONDARY);

	/* A rule, and the information's header. */
	y = baseline + 36;
	kl_canvas_round(canvas, (float)box.x, (float)y, (float)box.width, 1.0f, 0.0f, FM_COLOR_SEPARATOR);
	y += 22;
	(void)kl_text_draw(app->text, canvas, box.x, y, "Information", 11, PREVIEW_TEXT_HEADER, 1, FM_COLOR_TEXT_FAINT);
	y += 8;

	/* The kind, the size (a folder's item count), when it changed, where it is. */
	preview_size_text(app, entry, size, sizeof(size));
	fm_time_text(entry->modified, app->wall, modified, sizeof(modified));
	preview_where(app, entry->path, where, sizeof(where));
	y = preview_row(app, canvas, area, y, "Kind", app->peek.mime->kind);
	y = preview_row(app, canvas, area, y, "Size", size);
	y = preview_row(app, canvas, area, y, "Modified", modified);
	y = preview_row(app, canvas, area, y, "Where", where);

	/* Text shows its first lines under the information. */
	if (app->peek.text != NULL)
		(void)preview_text(app, canvas, area, y + 12);
}

/*
 * Draws an item's thumbnail fitted in a box, or a large icon when it has
 * none (yet); returns nonzero when the thumbnail was drawn.
 */
static int
preview_picture(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct fm_entry *entry,
	const struct kl_rect *box)
{
	const struct kl_image *thumb;
	float x;
	float y;
	int width;
	int height;
	int kind;

	/* A picture's thumbnail, once it is made. */
	thumb = NULL;
	kind = fm_thumb_kind(entry);
	if (kind != 0)
		thumb = fm_thumb_get(app, entry->path, entry->modified);

	/* No thumbnail: the item's icon, large, in the middle of the box. */
	if (thumb == NULL) {
		x = (float)(box->x + (box->width - PREVIEW_ICON) / 2);
		y = (float)(box->y + (box->height - PREVIEW_ICON) / 2);
		fm_grid_entry_icon(app, canvas, entry, x, y, (float)PREVIEW_ICON);
		return 0;
	}

	/* The thumbnail fitted in the box (a small one grown to fill it), on a soft shadow. */
	fm_image_fit(thumb->width, thumb->height, box->width, box->height, &width, &height);
	x = (float)(box->x + (box->width - width) / 2);
	y = (float)(box->y + (box->height - height) / 2);
	kl_canvas_shadow(canvas, x, y + 3.0f, (float)width, (float)height, 8.0f, 10.0f, FM_COLOR_SHADOW);
	kl_canvas_image(canvas, thumb, x, y, (float)width, (float)height, 8.0f, 1.0f);

	/* Succeeded: the thumbnail is shown. */
	return 1;
}

/* Draws a name centred in up to two lines of a width, and returns the last line's baseline. */
static int
preview_name(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const char *name,
	int x,
	int width,
	int baseline)
{
	struct kl_text_line line;
	char second[FM_NAME_MAX];
	size_t first_length;
	int first_width;
	int second_width;

	/* The first line: what fits the width. */
	first_length = kl_text_break(app->text, name, PREVIEW_TEXT_NAME, 1, width);
	first_width = kl_text_width(app->text, name, first_length, PREVIEW_TEXT_NAME, 1);
	(void)kl_text_draw(app->text, canvas, x + (width - first_width) / 2, baseline, name, first_length, PREVIEW_TEXT_NAME, 1, FM_COLOR_TEXT);

	/* A name of one line ends there. */
	if (name[first_length] == '\0')
		return baseline;

	/* The rest on a second line, cut with an ellipsis. */
	kl_text_metrics(app->text, PREVIEW_TEXT_NAME, &line);
	(void)kl_text_fit(app->text, name + first_length, PREVIEW_TEXT_NAME, 1, width, second, sizeof(second));
	second_width = kl_text_width(app->text, second, strlen(second), PREVIEW_TEXT_NAME, 1);
	(void)kl_text_draw(app->text, canvas, x + (width - second_width) / 2, baseline + line.height, second, strlen(second), PREVIEW_TEXT_NAME, 1, FM_COLOR_TEXT);

	/* Reports the second line's baseline. */
	return baseline + line.height;
}

/* Draws a row of information (its label at the left, its value at the right) and returns the next row's top. */
static int
preview_row(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area,
	int y,
	const char *label,
	const char *value)
{
	char fitted[FM_PATH_MAX];
	int label_width;
	int value_width;
	int room;
	int baseline;
	int left;
	int right;

	/* The label, quiet, at the left. */
	left = area->x + PREVIEW_PADDING;
	right = area->x + area->width - PREVIEW_PADDING;
	baseline = kl_text_center(PREVIEW_TEXT_INFO, y, PREVIEW_ROW);
	label_width = kl_text_draw(app->text, canvas, left, baseline, label, strlen(label), PREVIEW_TEXT_INFO, 0, FM_COLOR_TEXT_SECONDARY);

	/* The value, cut to the room left, against the right edge. */
	room = right - left - label_width - 12;
	(void)kl_text_fit(app->text, value, PREVIEW_TEXT_INFO, 0, room, fitted, sizeof(fitted));
	value_width = kl_text_width(app->text, fitted, strlen(fitted), PREVIEW_TEXT_INFO, 0);
	(void)kl_text_draw(app->text, canvas, right - value_width, baseline, fitted, strlen(fitted), PREVIEW_TEXT_INFO, 0, FM_COLOR_TEXT);

	/* Reports where the next row starts. */
	return y + PREVIEW_ROW;
}

/* Draws the first lines of the text read on a light box from a height down; returns how many lines were drawn. */
static int
preview_text(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area,
	int y)
{
	const char *line;
	const char *end;
	int bottom;
	int height;
	int lines;
	int room;
	int x;

	/* As many lines as the pane has room for, up to its limit. */
	bottom = area->y + area->height - PREVIEW_PADDING;
	room = (bottom - y - 12) / PREVIEW_TEXT_LINE;
	if (room > PREVIEW_TEXT_LINES)
		room = PREVIEW_TEXT_LINES;
	if (room > app->peek.line_count)
		room = app->peek.line_count;
	if (room <= 0)
		return 0;

	/* The box behind them. */
	x = area->x + PREVIEW_PADDING;
	height = room * PREVIEW_TEXT_LINE + 12;
	kl_canvas_round(canvas, (float)x, (float)y, (float)(area->width - 2 * PREVIEW_PADDING), (float)height, 8.0f, FM_COLOR_INNER);

	/* Each line, cut at the box's width. */
	line = app->peek.text;
	for (lines = 0; lines < room && *line != '\0'; lines++) {
		end = strchr(line, '\n');
		if (end == NULL)
			end = line + strlen(line);
		(void)kl_text_draw(app->text, canvas, x + 8, y + 6 + (lines + 1) * PREVIEW_TEXT_LINE - 4, line, (size_t)(end - line), PREVIEW_TEXT_BODY, 0, FM_COLOR_TEXT_SECONDARY);

		/* The next line starts after the newline. */
		line = end;
		if (*line == '\n')
			line++;
	}

	/* Reports how many lines were drawn. */
	return lines;
}

/* Writes an item's size: a file's bytes, or how many items a folder holds (counted once). */
static void
preview_size_text(
	struct fm_app *app,
	struct fm_entry *entry,
	char *text,
	size_t size)
{
	/* A file's size. */
	if (entry->folder == 0) {
		fm_dir_size_text(entry->size, text, size);
		return;
	}

	/* A folder's items, counted the first time it is shown. */
	if (entry->child_count < 0)
		entry->child_count = fm_dir_count(entry->path, app->show_hidden);
	fm_dir_items_text(entry->child_count, text, size);
}

/* Writes the folder an item is in, the home folder written as "~". */
static void
preview_where(
	struct fm_app *app,
	const char *path,
	char *text,
	size_t size)
{
	const char *slash;
	size_t home_length;
	size_t length;
	int prefix;

	/* The folder: the path up to its last slash (the root keeps its slash). */
	slash = strrchr(path, '/');
	length = 0;
	if (slash != NULL)
		length = (size_t)(slash - path);
	if (length == 0U)
		length = 1;
	if (length >= size)
		length = size - 1U;

	/* Under the home folder, "~" stands for it. */
	home_length = strlen(app->home);
	prefix = strncmp(path, app->home, home_length);
	if (prefix == 0 &&
	    home_length > 1U &&
	    length >= home_length &&
	    (path[home_length] == '/' ||
	     path[home_length] == '\0')) {
		snprintf(text, size, "~%.*s", (int)(length - home_length), path + home_length);
		return;
	}

	/* Elsewhere the whole path. */
	snprintf(text, size, "%.*s", (int)length, path);
}

/* Draws a line of text centred across an area. */
static void
preview_centered(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *area,
	int baseline,
	const char *text,
	unsigned pixels,
	int bold,
	kl_color color)
{
	char fitted[FM_PATH_MAX];
	int width;

	/* The text, cut to the area's width inside its padding, in the middle. */
	(void)kl_text_fit(app->text, text, pixels, bold, area->width - 2 * PREVIEW_PADDING, fitted, sizeof(fitted));
	width = kl_text_width(app->text, fitted, strlen(fitted), pixels, bold);
	(void)kl_text_draw(app->text, canvas, area->x + (area->width - width) / 2, baseline, fitted, strlen(fitted), pixels, bold, color);
}

/* Draws Quick Look's card of a size in the middle of the window, and reports where it is. */
static void
look_card(
	struct fm_app *app,
	struct kl_canvas *canvas,
	int width,
	int height,
	struct kl_rect *card)
{
	/* The card in the middle. */
	card->width = width;
	card->height = height;
	card->x = (app->width - width) / 2;
	card->y = (app->height - height) / 2;

	/* Its shadow, its white face; a click on it stays there. */
	kl_canvas_shadow(canvas, (float)card->x, (float)card->y + 10.0f, (float)width, (float)height, LOOK_RADIUS, 30.0f, KL_RGBA(0x0f1a33, 90));
	kl_canvas_round(canvas, (float)card->x, (float)card->y, (float)width, (float)height, LOOK_RADIUS, FM_COLOR_PANEL);
	fm_ui_hit(app, card, FM_HIT_OVERLAY, FM_OVERLAY_CARD);
}

/* Draws Quick Look's header: the item's name and kind, and the close button. */
static void
look_header(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *card,
	struct fm_entry *entry)
{
	struct kl_rect close;
	char size[64];
	char detail[160];

	/* The name. */
	(void)kl_text_draw_fit(app->text, canvas, card->x + 22, card->y + 27, entry->name, 15U, 1, card->width - 90, FM_COLOR_TEXT);

	/* The kind and the size under it. */
	preview_size_text(app, entry, size, sizeof(size));
	snprintf(detail, sizeof(detail), "%s \xc2\xb7 %s", app->peek.mime->kind, size);
	(void)kl_text_draw_fit(app->text, canvas, card->x + 22, card->y + 45, detail, 12U, 0, card->width - 90, FM_COLOR_TEXT_SECONDARY);

	/* The close button at the right, lit under the pointer. */
	close.x = card->x + card->width - 44;
	close.y = card->y + 14;
	close.width = 28;
	close.height = 28;
	fm_icon_button(app, canvas, FM_WIDGET_ICON, FM_BUTTON_LOOK_CLOSE, &close, KL_ICON_CLOSE, 16, KL_BUTTON_ROUND);
	fm_ui_hit(app, &close, FM_HIT_BUTTON, FM_BUTTON_LOOK_CLOSE);
}

/* Draws a picture as large as the room allows (grown at most a few times its size), on a card that fits it. */
static void
look_picture(
	struct fm_app *app,
	struct kl_canvas *canvas,
	struct fm_entry *entry,
	int room_width,
	int room_height)
{
	struct kl_rect card;
	const struct kl_image *picture;
	int width;
	int height;
	int box_width;
	int box_height;
	int card_width;
	int limit;

	/* The picture at the size Quick Look reads it. */
	fm_peek_picture(&app->peek, LOOK_PICTURE_SIDE);
	picture = &app->peek.picture;

	/* A picture that cannot be read is shown as any other item. */
	if (picture->pixels == NULL) {
		look_other(app, canvas, entry);
		return;
	}

	/* How far it may grow: a few times its own size, and a tiny one to be seen. */
	limit = picture->width;
	if (picture->height > limit)
		limit = picture->height;
	limit *= LOOK_PICTURE_GROWTH;
	if (limit < LOOK_PICTURE_SMALLEST)
		limit = LOOK_PICTURE_SMALLEST;

	/* The box it fills: the room under the header, no larger than that. */
	box_width = room_width - 32;
	box_height = room_height - LOOK_HEADER - 16;
	if (box_width > limit)
		box_width = limit;
	if (box_height > limit)
		box_height = limit;

	/* Its size fitted in the box. */
	fm_image_fit(picture->width, picture->height, box_width, box_height, &width, &height);

	/* A card around it, wide enough for the header. */
	card_width = width + 32;
	if (card_width < 380)
		card_width = 380;
	look_card(app, canvas, card_width, height + LOOK_HEADER + 16, &card);
	look_header(app, canvas, &card, entry);

	/* The picture under the header. */
	kl_canvas_image(canvas, picture, (float)(card.x + (card.width - width) / 2), (float)(card.y + LOOK_HEADER), (float)width, (float)height, 6.0f, 1.0f);
}

/* Draws text as its first lines on a tall card. */
static void
look_text(
	struct fm_app *app,
	struct kl_canvas *canvas,
	struct fm_entry *entry,
	int room_width,
	int room_height)
{
	struct kl_rect card;
	const char *line;
	const char *end;
	int width;
	int lines;
	int baseline;
	int bottom;

	/* A card as wide as a page, as tall as the room. */
	width = LOOK_TEXT_WIDTH;
	if (width > room_width)
		width = room_width;
	look_card(app, canvas, width, room_height, &card);
	look_header(app, canvas, &card, entry);

	/* The page's ground under the header. */
	kl_canvas_round(canvas, (float)card.x + 12.0f, (float)(card.y + LOOK_HEADER), (float)card.width - 24.0f, (float)(card.height - LOOK_HEADER - 12), 10.0f, FM_COLOR_INNER);

	/* Each line that fits, cut at the page's width. */
	bottom = card.y + card.height - 20;
	line = app->peek.text;
	baseline = card.y + LOOK_HEADER + 8 + LOOK_TEXT_LINE;
	for (lines = 0; lines < app->peek.line_count && *line != '\0'; lines++) {
		if (baseline > bottom)
			break;
		end = strchr(line, '\n');
		if (end == NULL)
			end = line + strlen(line);
		(void)kl_text_draw(app->text, canvas, card.x + 28, baseline, line, (size_t)(end - line), LOOK_TEXT_BODY, 0, FM_COLOR_TEXT);

		/* The next line starts after the newline. */
		baseline += LOOK_TEXT_LINE;
		line = end;
		if (*line == '\n')
			line++;
	}
}

/* Draws any other item: a large icon and when it was modified. */
static void
look_other(
	struct fm_app *app,
	struct kl_canvas *canvas,
	struct fm_entry *entry)
{
	struct kl_rect card;
	char modified[64];
	char text[96];
	int icon;
	int width;

	/* The card and its header. */
	look_card(app, canvas, LOOK_OTHER_WIDTH, LOOK_OTHER_HEIGHT, &card);
	look_header(app, canvas, &card, entry);

	/* The item's icon, large, in the middle. */
	icon = 128;
	fm_grid_entry_icon(app, canvas, entry, (float)(card.x + (card.width - icon) / 2), (float)(card.y + LOOK_HEADER + 20), (float)icon);

	/* When it was modified, under the icon. */
	fm_time_text(entry->modified, app->wall, modified, sizeof(modified));
	snprintf(text, sizeof(text), "Modified %s", modified);
	width = kl_text_width(app->text, text, strlen(text), 12U, 0);
	(void)kl_text_draw(app->text, canvas, card.x + (card.width - width) / 2, card.y + card.height - 24, text, strlen(text), 12U, 0, FM_COLOR_TEXT_SECONDARY);
}
