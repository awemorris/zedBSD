/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The list view of files (spec §12): a header of columns and one
 * row an item.
 *
 * The name column takes what the others leave.  Which columns show is the
 * app's choice (View > List Columns); the places whose items come from
 * many folders (search, recent files, the trash) add where each item is.
 * Clicking a column's title sorts by it, clicking again reverses.
 * Dragging the edge before a column changes the widths of the columns on
 * either side of it (BUG-220); the widths are kept for the next run in
 * Files' own settings (files.column-width.<column>, libkeiland's
 * kl_settings_* in ~/.config/keiland/files.conf; 0 is the column's own
 * width).
 */

#include "files.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The header's and a row's heights. */
#define LIST_HEADER		FM_LIST_HEADER
#define LIST_ROW		FM_LIST_ROW

/* The text sizes of the view. */
#define LIST_TEXT		13U

/* The widest a list may show columns: the name, and the six others at most. */
#define LIST_COLUMNS		8

/* The columns' own widths (the name takes what is left), the least a dragged column keeps, the least the name keeps. */
#define LIST_LEAST		60
#define LIST_NAME_LEAST		200

/* How wide the grip on an edge between two columns is. */
#define LIST_EDGE_GRIP		10

/* The application's name, which names its settings. */
#define LIST_SETTINGS_APP	"files"

/*
 * One column as it is laid out for a frame: which one, where and how wide.
 */
struct list_column {
	unsigned column;
	int x;
	int width;
};

static int list_layout(struct fm_app *app, const struct kl_rect *inner, struct list_column *columns);
static void list_header(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *inner, const struct list_column *columns, int count);
static void list_row(struct fm_app *app, struct kl_canvas *canvas, struct fm_entry *entry, int index, const struct kl_rect *row, const struct list_column *columns, int count);
static void list_cell_text(struct fm_app *app, struct fm_entry *entry, unsigned column, char *text, size_t size);
static const char *list_title(unsigned column);
static unsigned list_sort_of(unsigned column);
static int list_width(const struct fm_app *app, unsigned column);
static int list_left_of(struct fm_app *app, unsigned column);
static unsigned list_shown(struct fm_app *app);
static void list_widths_save(struct fm_app *app, int column, int left);

/* The settings' key of each column's width (the name has none: it takes what is left). */
static const char *const list_width_keys[FM_COLUMN_COUNT] = {
	NULL,
	"files.column-width.kind",
	"files.column-width.size",
	"files.column-width.modified",
	"files.column-width.changed",
	"files.column-width.owner",
	"files.column-width.location",
	"files.column-width.deleted"
};

/*
 * Draws the items as a list in the panel's inner rectangle, and records
 * the header's titles and each row.
 */
void
fm_list_draw(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *inner)
{
	struct list_column columns[LIST_COLUMNS];
	struct kl_rect rows;
	struct kl_rect row;
	struct fm_tab *tab;
	size_t index;
	int count;

	/* The columns and the header. */
	tab = fm_ui_tab(app);
	count = list_layout(app, inner, columns);
	list_header(app, canvas, inner, columns, count);

	/* The rows' part, under the header. */
	rows.x = inner->x;
	rows.y = inner->y + LIST_HEADER;
	rows.width = inner->width;
	rows.height = inner->height - LIST_HEADER;
	app->layout.columns = 1;
	app->layout.cell_width = inner->width;
	app->layout.cell_height = LIST_ROW;
	app->layout.content_height = (int)tab->listing.count * LIST_ROW + LIST_HEADER + 64;

	/* Each row in sight. */
	kl_canvas_clip_push(canvas, &rows);
	for (index = 0; index < tab->listing.count; index++) {
		row.x = rows.x;
		row.y = rows.y + (int)index * LIST_ROW - tab->scroll;
		row.width = rows.width;
		row.height = LIST_ROW;
		if (row.y + LIST_ROW < rows.y || row.y > rows.y + rows.height)
			continue;
		list_row(app, canvas, &tab->listing.entries[index], (int)index, &row, columns, count);
	}

	/* The rows' clip ends. */
	kl_canvas_clip_pop(canvas);
}

/*
 * Writes a time the way the lists show it: "Today 16:20", "Yesterday
 * 20:18", "Sep 21 14:03" in this year, "Sep 21, 2025" before.
 */
void
fm_time_text(
	time_t when,
	time_t now,
	char *text,
	size_t size)
{
	static const char *const months[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	struct tm moment;
	struct tm today;
	struct tm *converted;
	long days;

	/* The time and today, in local time. */
	converted = localtime(&when);
	if (converted == NULL) {
		snprintf(text, size, "--");
		return;
	}

	/* The time taken, before the next conversion reuses the buffer. */
	moment = *converted;
	converted = localtime(&now);
	if (converted == NULL) {
		snprintf(text, size, "--");
		return;
	}

	/* Today's date. */
	today = *converted;

	/* How many calendar days ago (in this year). */
	days = -1;
	if (moment.tm_year == today.tm_year)
		days = (long)today.tm_yday - (long)moment.tm_yday;

	/* Today and yesterday are named. */
	if (days == 0) {
		snprintf(text, size, "Today %02d:%02d", moment.tm_hour, moment.tm_min);
		return;
	}

	/* Yesterday is named too. */
	if (days == 1) {
		snprintf(text, size, "Yesterday %02d:%02d", moment.tm_hour, moment.tm_min);
		return;
	}

	/* Earlier this year: the day and the time. */
	if (moment.tm_year == today.tm_year) {
		snprintf(text, size, "%s %d %02d:%02d", months[moment.tm_mon], moment.tm_mday, moment.tm_hour, moment.tm_min);
		return;
	}

	/* Another year: the date. */
	snprintf(text, size, "%s %d, %d", months[moment.tm_mon], moment.tm_mday, moment.tm_year + 1900);
}

/*
 * Reports the column whose title is at a point of the header, as a sort
 * (FM_SORT_*); -1 when the point is on no sortable title.  The layout is
 * the last frame's.
 */
int
fm_list_sort_at(
	struct fm_app *app,
	int index)
{
	/* The header's hit carries the column; its sort. */
	(void)app;
	if (index < 0)
		return -1;

	/* Reports the sort of that column. */
	return (int)list_sort_of((unsigned)index);
}

/*
 * Starts a drag of the edge before a column (a press on it, BUG-220): the
 * widths of the columns on either side of it then.
 */
void
fm_list_edge_press(
	struct fm_app *app,
	int column,
	int x)
{
	int left;

	/* The column and the one left of it (none when the name is: the name takes what is left). */
	if (column <= (int)FM_COLUMN_NAME || column >= (int)FM_COLUMN_COUNT)
		return;
	left = list_left_of(app, (unsigned)column);
	app->column_drag = column;
	app->column_drag_x = x;
	app->column_drag_right = list_width(app, (unsigned)column);
	app->column_drag_left = 0;
	if (left > (int)FM_COLUMN_NAME)
		app->column_drag_left = list_width(app, (unsigned)left);
	app->dirty = 1;
}

/*
 * Follows a drag of a column's edge: moved right, the column narrows and
 * the one left of it widens (the name, when it is that one, takes it);
 * neither below LIST_LEAST.  Returns 1 while a drag is under way.
 */
int
fm_list_edge_motion(
	struct fm_app *app,
	int x)
{
	int left;
	int moved;
	int right_width;
	int left_width;

	/* No drag. */
	if (app->column_drag < 0)
		return 0;

	/* The edge's move, kept where neither column gets narrower than its least. */
	moved = x - app->column_drag_x;
	if (app->column_drag_right - moved < LIST_LEAST)
		moved = app->column_drag_right - LIST_LEAST;
	left = list_left_of(app, (unsigned)app->column_drag);
	if (left > (int)FM_COLUMN_NAME && app->column_drag_left + moved < LIST_LEAST)
		moved = LIST_LEAST - app->column_drag_left;

	/* The new widths (the name's is what the layout leaves; the layout keeps its least). */
	right_width = app->column_drag_right - moved;
	app->column_widths[app->column_drag] = right_width;
	if (left > (int)FM_COLUMN_NAME) {
		left_width = app->column_drag_left + moved;
		app->column_widths[left] = left_width;
	}

	/* Drawn at the new widths. */
	app->dirty = 1;

	/* Succeeded: the drag goes on. */
	return 1;
}

/*
 * Ends a drag of a column's edge: the widths stay, and are kept for the
 * next run.  Returns 1 when one was under way (the release is its).
 */
int
fm_list_edge_release(
	struct fm_app *app)
{
	int left;

	/* No drag. */
	if (app->column_drag < 0)
		return 0;

	/* The widths stay; the log says them (the tests read it). */
	printf("ZFILES COLUMN width column=%d width=%d\n", app->column_drag, app->column_widths[app->column_drag]);
	fflush(stdout);

	/* Kept for the next run: the column, and the one left of it when the drag changed it. */
	left = list_left_of(app, (unsigned)app->column_drag);
	list_widths_save(app, app->column_drag, left);
	app->column_drag = -1;
	app->dirty = 1;

	/* Succeeded: the release was the drag's. */
	return 1;
}

/*
 * Reads the widths kept by an earlier run (BUG-220); a width under the
 * least a dragged column keeps, or none, leaves the column at its own.
 */
void
fm_list_widths_load(
	struct fm_app *app)
{
	struct kl_settings *settings;
	unsigned column;
	int width;

	/* Files' own settings; its own keys need no display.  Without them the columns keep their own widths. */
	settings = kl_settings_open(NULL, LIST_SETTINGS_APP);
	if (settings == NULL)
		return;

	/* Each column's kept width. */
	for (column = FM_COLUMN_NAME + 1U; column < FM_COLUMN_COUNT; column++) {
		width = kl_settings_get_int(settings, list_width_keys[column], 0);
		if (width >= LIST_LEAST) {
			app->column_widths[column] = width;
			printf("ZFILES COLUMN kept column=%u width=%d\n", column, width);
		}
	}

	/* Closes the settings, which are not needed any more. */
	kl_settings_close(settings);
}

/*
 * Keeps the widths of a dragged column and of the one left of it (when it
 * is not the name) in Files' settings.  A failure leaves the old widths for
 * the next run; this run keeps the new ones.
 */
static void
list_widths_save(
	struct fm_app *app,
	int column,
	int left)
{
	struct kl_settings *settings;
	int error;

	/* Opens Files' own settings. */
	settings = kl_settings_open(NULL, LIST_SETTINGS_APP);
	if (settings == NULL) {
		printf("ZFILES COLUMN saved column=%d error=%d\n", column, ENOMEM);
		return;
	}

	/* The dragged column's width. */
	error = kl_settings_set_int(settings, list_width_keys[column], app->column_widths[column], NULL);

	/* The one left of it, when the drag changed it too. */
	if (error == 0 && left > (int)FM_COLUMN_NAME && app->column_widths[left] > 0)
		error = kl_settings_set_int(settings, list_width_keys[left], app->column_widths[left], NULL);

	/* The log says how it went (the tests read it). */
	printf("ZFILES COLUMN saved column=%d left=%d error=%d\n", column, left, error);
	fflush(stdout);
	kl_settings_close(settings);
}

/* Lays out the columns shown, and returns how many there are. */
static int
list_layout(
	struct fm_app *app,
	const struct kl_rect *inner,
	struct list_column *columns)
{
	unsigned shown;
	unsigned column;
	struct list_column swap;
	int count;
	int right;
	int width;
	int first;
	int last;

	/* The columns the app shows, and those the place adds. */
	shown = list_shown(app);

	/* The other columns from the right (each its dragged width, else its own), while the name keeps its least. */
	count = 1;
	right = inner->x + inner->width - 8;
	for (column = FM_COLUMN_COUNT - 1U; column > FM_COLUMN_NAME; column--) {
		if ((shown & (1U << column)) == 0U)
			continue;
		width = list_width(app, column);
		if (right - width < inner->x + LIST_NAME_LEAST)
			continue;
		right -= width;
		columns[count].column = column;
		columns[count].x = right;
		columns[count].width = width;
		count++;
	}

	/* They were laid out right to left: put them in order after the name. */
	first = 1;
	last = count - 1;
	while (first < last) {
		swap = columns[first];
		columns[first] = columns[last];
		columns[last] = swap;
		first++;
		last--;
	}

	/* The name takes the rest. */
	columns[0].column = FM_COLUMN_NAME;
	columns[0].x = inner->x + 8;
	columns[0].width = right - columns[0].x;

	/* Reports how many columns there are. */
	return count;
}

/* Draws the header with libkeiland's (ws090-p024): each column's title, the sorted one with its direction; records the titles and the edges. */
static void
list_header(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *inner,
	const struct list_column *columns,
	int count)
{
	struct kl_column titles[LIST_COLUMNS];
	struct kl_style style;
	struct kl_rect header;
	struct kl_rect title;
	unsigned sort;
	int index;

	/* Each column's title, the sorted one and the one under the pointer. */
	for (index = 0; index < count; index++) {
		titles[index].title = list_title(columns[index].column);
		titles[index].x = columns[index].x;
		titles[index].width = columns[index].width;
		titles[index].flags = 0U;
		sort = list_sort_of(columns[index].column);
		if (sort == app->sort)
			titles[index].flags |= KL_COLUMN_SORTED;
		if (sort == app->sort && app->sort_reverse != 0)
			titles[index].flags |= KL_COLUMN_REVERSED;
		if (app->hover_kind == FM_HIT_HEADER && app->hover_index == (int)columns[index].column)
			titles[index].flags |= KL_COLUMN_HOVER;
	}

	/* The header. */
	header.x = inner->x;
	header.y = inner->y;
	header.width = inner->width;
	header.height = LIST_HEADER;
	fm_style(app, canvas, &style);
	kl_list_header(&style, &header, titles, (size_t)count);

	/* A sortable title can be clicked. */
	for (index = 0; index < count; index++) {
		sort = list_sort_of(columns[index].column);
		if (sort == FM_SORT_COUNT)
			continue;
		title.x = columns[index].x;
		title.y = inner->y;
		title.width = columns[index].width;
		title.height = LIST_HEADER;
		fm_ui_hit(app, &title, FM_HIT_HEADER, (int)columns[index].column);
	}

	/* The edge before each column but the name's can be dragged (over the titles, which it was recorded after). */
	for (index = 1; index < count; index++) {
		title.x = columns[index].x - LIST_EDGE_GRIP / 2;
		title.y = inner->y;
		title.width = LIST_EDGE_GRIP;
		title.height = LIST_HEADER;
		fm_ui_hit(app, &title, FM_HIT_COLUMN_EDGE, (int)columns[index].column);
	}
}

/* Draws one row: its ground, the icon and the name, and the other cells. */
static void
list_row(
	struct fm_app *app,
	struct kl_canvas *canvas,
	struct fm_entry *entry,
	int index,
	const struct kl_rect *row,
	const struct list_column *columns,
	int count)
{
	struct kl_style style;
	struct kl_rect field;
	char text[FM_PATH_MAX];
	kl_color ink;
	kl_color faint;
	unsigned state;
	unsigned flags;
	int renaming;
	int match;
	int baseline;
	int column;
	int width;

	/* The ground for the row's state (libkeiland's, ws090-p024), with the inks of its text. */
	state = 0U;
	if (entry->selected != 0)
		state |= KL_ITEM_SELECTED;
	if (app->focused != 0)
		state |= KL_ITEM_FOCUSED;
	if (app->hover_kind == FM_HIT_ITEM && app->hover_index == index)
		state |= KL_ITEM_HOVER;
	fm_style(app, canvas, &style);
	kl_list_item(&style, row, state, &ink, &faint);

	/* The small icon (faded when cut) and the name, or the field while it is being changed. */
	baseline = kl_text_center(LIST_TEXT, row->y, row->height);
	fm_grid_entry_icon(app, canvas, entry, (float)columns[0].x, (float)row->y + 3.0f, 22.0f);
	if (entry->cut != 0)
		kl_canvas_round(canvas, (float)columns[0].x, (float)row->y + 3.0f, 22.0f, 22.0f, 4.0f, FM_COLOR_TILE);
	width = columns[0].width - 34;
	renaming = 0;
	if (app->focus == FM_FOCUS_RENAME) {
		match = strcmp(entry->path, app->rename_path);
		if (match == 0)
			renaming = 1;
	}

	/* The field where the name was, or the name. */
	if (renaming != 0) {
		field.x = columns[0].x + 26;
		field.y = row->y + 3;
		field.width = width + 4;
		field.height = row->height - 6;
		fm_rename_draw(app, canvas, &field);
	} else {
		(void)kl_text_draw_fit(app->text, canvas, columns[0].x + 30, baseline, entry->name, LIST_TEXT, 0, width, ink);
	}

	/* The other columns' cells. */
	for (column = 1; column < count; column++) {
		list_cell_text(app, entry, columns[column].column, text, sizeof(text));

		/* Sizes are aligned to the right of their column, the others to the left. */
		flags = 0U;
		if (columns[column].column == FM_COLUMN_SIZE)
			flags = KL_CELL_RIGHT;
		kl_list_cell(&style, columns[column].x, columns[column].width, baseline, text, LIST_TEXT, flags, faint);
	}

	/* The row can be clicked. */
	fm_ui_hit(app, row, FM_HIT_ITEM, index);
}

/* Writes what an entry shows in a column. */
static void
list_cell_text(
	struct fm_app *app,
	struct fm_entry *entry,
	unsigned column,
	char *text,
	size_t size)
{
	/* Each column's text. */
	text[0] = '\0';
	switch (column) {
	case FM_COLUMN_KIND:
		snprintf(text, size, "%s", entry->mime->kind);
		break;
	case FM_COLUMN_SIZE:
		if (entry->folder != 0)
			snprintf(text, size, "--");
		else
			fm_dir_size_text(entry->size, text, size);
		break;
	case FM_COLUMN_MODIFIED:
		fm_time_text(entry->modified, (time_t)app->wall, text, size);
		break;
	case FM_COLUMN_CHANGED:
		fm_time_text(entry->changed, (time_t)app->wall, text, size);
		break;
	case FM_COLUMN_DELETED:
		fm_time_text(entry->extra_time, (time_t)app->wall, text, size);
		break;
	case FM_COLUMN_OWNER:
		fm_owner_text(entry->uid, entry->gid, text, size);
		break;
	case FM_COLUMN_LOCATION:
		if (entry->detail != NULL)
			snprintf(text, size, "%s", entry->detail);
		break;
	default:
		break;
	}
}

/* Names a column in the header. */
static const char *
list_title(
	unsigned column)
{
	/* Each column's title. */
	switch (column) {
	case FM_COLUMN_NAME:
		return kl_tr("Name");
	case FM_COLUMN_KIND:
		return kl_tr("Kind");
	case FM_COLUMN_SIZE:
		return kl_tr("Size");
	case FM_COLUMN_MODIFIED:
		return kl_tr("Date Modified");
	case FM_COLUMN_CHANGED:
		return kl_tr("Date Changed");
	case FM_COLUMN_OWNER:
		return kl_tr("Owner");
	case FM_COLUMN_LOCATION:
		return kl_tr("Location");
	case FM_COLUMN_DELETED:
		return kl_tr("Date Deleted");
	default:
		break;
	}

	/* A column this program does not know. */
	return "";
}

/* Reports the sort a column's title chooses (FM_SORT_COUNT for none). */
static unsigned
list_sort_of(
	unsigned column)
{
	/* The columns that sort. */
	switch (column) {
	case FM_COLUMN_NAME:
		return FM_SORT_NAME;
	case FM_COLUMN_KIND:
		return FM_SORT_KIND;
	case FM_COLUMN_SIZE:
		return FM_SORT_SIZE;
	case FM_COLUMN_MODIFIED:
		return FM_SORT_MODIFIED;
	default:
		break;
	}

	/* The others do not. */
	return FM_SORT_COUNT;
}

/* Gives a column's width: the one the user dragged it to, else its own. */
static int
list_width(
	const struct fm_app *app,
	unsigned column)
{
	static const int widths[FM_COLUMN_COUNT] = { 0, 150, 90, 150, 150, 110, 200, 150 };

	/* Dragged. */
	if (column < FM_COLUMN_COUNT && app->column_widths[column] > 0)
		return app->column_widths[column];

	/* Its own. */
	if (column < FM_COLUMN_COUNT)
		return widths[column];
	return 0;
}

/*
 * Gives the column shown left of another in the list (the name when none
 * other is): the next lower column the app shows.
 */
static int
list_left_of(
	struct fm_app *app,
	unsigned column)
{
	unsigned shown;
	unsigned other;

	/* The columns shown (the place's own too, as list_layout takes them). */
	shown = list_shown(app);
	for (other = column - 1U; other > FM_COLUMN_NAME; other--) {
		if ((shown & (1U << other)) != 0U)
			return (int)other;
	}

	/* None: the name. */
	return (int)FM_COLUMN_NAME;
}

/* Gives the columns shown (FM_COLUMN_* bits): the name, the app's, and those the place adds. */
static unsigned
list_shown(
	struct fm_app *app)
{
	const struct fm_location *location;
	struct fm_tab *tab;
	unsigned shown;

	/* The name and the app's choice. */
	tab = fm_ui_tab(app);
	location = &tab->history[tab->history_index].location;
	shown = app->columns | (1U << FM_COLUMN_NAME);

	/* Where each item is, for the places whose items come from many folders; when it was deleted, in the trash. */
	if (location->kind == FM_LOCATION_SEARCH || location->kind == FM_LOCATION_RECENTS)
		shown |= 1U << FM_COLUMN_LOCATION;
	if (location->kind == FM_LOCATION_TRASH) {
		shown |= 1U << FM_COLUMN_LOCATION;
		shown |= 1U << FM_COLUMN_DELETED;
	}

	/* Succeeded: the columns. */
	return shown;
}
