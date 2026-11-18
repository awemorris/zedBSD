/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop's grid and its layout file (files --desktop, ws094-p004,
 * plan/ws094/design.md §4).
 *
 * The desktop is a grid of cells, counted from the top-right corner: a
 * column is a cell's distance from the right edge, a row from the top.
 * Items are placed where the user put them (the layout file), and the
 * others fill the free cells from the top-right corner down a column,
 * then the next column to the left.  The layout file is
 * $XDG_CONFIG_HOME/keiland/desktop-layout (or ~/.config/...), one line an
 * item the user placed: NAME<TAB>COLUMN<TAB>ROW.  It is written to a new
 * file beside it and renamed over it.
 *
 * The places the items are shown at are also kept by name in memory
 * (ws094-p005), and a new layout keeps them after the saved ones, so that
 * an item made or pasted takes a free cell and the others stay where they
 * were; a renamed item keeps its cell under its new name.
 *
 * An item's name is shown in one or two lines under its icon (ws094-p010):
 * a name that does not fit one line is broken into two, and one too long
 * for two keeps its start on the first line and its end -- the
 * extension with it -- on the second, after an ellipsis ("A long name
 * of" / "...the end.txt").
 */

#include "files.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A cell of the grid, and the margin from the desktop's edges. */
#define LAYOUT_CELL_WIDTH	96
#define LAYOUT_CELL_HEIGHT	104
#define LAYOUT_MARGIN		16

/* The layout file under the configuration folder, and the suffix of the new one written before the rename. */
#define LAYOUT_FILE		"keiland/desktop-layout"
#define LAYOUT_NEW_SUFFIX	".new"

/* The longest line of the layout file. */
#define LAYOUT_LINE		(FM_NAME_MAX + 32)

/* The ellipsis put where the middle of a long name is left out (UTF-8). */
#define LABEL_ELLIPSIS "\xe2\x80\xa6"

static int layout_saved_index(const struct fm_desktop_saved *saved, size_t count, const char *name);
static size_t label_prefix(struct kl_text *text, const char *name, size_t length, int width, unsigned pixels);
static size_t label_suffix(struct kl_text *text, const char *name, size_t length, size_t from, int width, unsigned pixels);
static size_t label_next(const char *name, size_t length, size_t at);
static size_t label_previous(const char *name, size_t at);

/*
 * Works out how many columns and rows of cells a desktop of a size has (at
 * least one of each).
 */
void
fm_desktop_grid(
	int width,
	int height,
	int *columns,
	int *rows)
{
	/* The columns across. */
	*columns = (width - 2 * LAYOUT_MARGIN) / LAYOUT_CELL_WIDTH;
	if (*columns < 1)
		*columns = 1;

	/* The rows down. */
	*rows = (height - 2 * LAYOUT_MARGIN) / LAYOUT_CELL_HEIGHT;
	if (*rows < 1)
		*rows = 1;

	/* Succeeded: the caller has a nonempty grid. */
	return;
}

/*
 * Finds the rectangle of a cell on a desktop of a size.  Returns 1, or 0
 * when the desktop has no such cell.
 */
int
fm_desktop_cell_rect(
	int column,
	int row,
	int width,
	int height,
	struct kl_rect *rect)
{
	int columns;
	int rows;

	/* The cell must be in the grid. */
	fm_desktop_grid(width, height, &columns, &rows);
	if (column < 0 || row < 0)
		return 0;
	if (column >= columns || row >= rows)
		return 0;

	/* From the right edge leftwards, from the top down. */
	rect->x = width - LAYOUT_MARGIN - (column + 1) * LAYOUT_CELL_WIDTH;
	rect->y = LAYOUT_MARGIN + row * LAYOUT_CELL_HEIGHT;
	rect->width = LAYOUT_CELL_WIDTH;
	rect->height = LAYOUT_CELL_HEIGHT;

	/* Succeeded: the cell. */
	return 1;
}

/*
 * Finds the cell of the grid at a point of a desktop of a size.  Returns 1
 * with its column and row, or 0 when the point is in no cell (the margin).
 */
int
fm_desktop_cell_at(
	int x,
	int y,
	int width,
	int height,
	int *column,
	int *row)
{
	int columns;
	int rows;
	int from_right;
	int from_top;

	/* The point's distance from the grid's top-right corner; the margin has no cell. */
	from_right = width - LAYOUT_MARGIN - x;
	from_top = y - LAYOUT_MARGIN;
	if (from_right <= 0 || from_top < 0)
		return 0;

	/* The cell it falls in, which must be in the grid. */
	fm_desktop_grid(width, height, &columns, &rows);
	*column = (from_right - 1) / LAYOUT_CELL_WIDTH;
	*row = from_top / LAYOUT_CELL_HEIGHT;
	if (*column >= columns || *row >= rows)
		return 0;

	/* Succeeded: the cell. */
	return 1;
}

/*
 * Places the items of a list of names on a desktop of a size: each saved
 * place that is in the grid and not taken first, then the others in the
 * free cells from the top-right corner down.  An item without a cell gets
 * column -1.
 */
void
fm_desktop_arrange(
	const char *const *names,
	size_t count,
	const struct fm_desktop_saved *saved,
	size_t saved_count,
	int width,
	int height,
	struct fm_desktop_place *places)
{
	unsigned char *taken;
	size_t index;
	int columns;
	int rows;
	int found;
	int cell;
	int next;

	/* The grid, and which of its cells are taken (none without memory for the marks: every item then fills in order). */
	fm_desktop_grid(width, height, &columns, &rows);
	taken = calloc((size_t)columns * (size_t)rows, 1U);

	/* The items the user placed, where they were put, when the cell is in the grid and free. */
	for (index = 0; index < count; index++) {
		/* No place yet. */
		places[index].column = -1;
		places[index].row = -1;
		found = layout_saved_index(saved, saved_count, names[index]);
		if (found < 0 || taken == NULL)
			continue;

		/* A place outside the grid, or one another item has, is not kept. */
		if (saved[found].column < 0 || saved[found].column >= columns)
			continue;
		if (saved[found].row < 0 || saved[found].row >= rows)
			continue;
		cell = saved[found].column * rows + saved[found].row;
		if (taken[cell] != 0U)
			continue;

		/* The item takes its cell. */
		places[index].column = saved[found].column;
		places[index].row = saved[found].row;
		taken[cell] = 1U;
	}

	/* The other items in the free cells, a column at a time from the right. */
	next = 0;
	for (index = 0; index < count; index++) {
		/* An item placed already. */
		if (places[index].column >= 0)
			continue;

		/* The next free cell. */
		while (next < columns * rows &&
		       taken != NULL &&
		       taken[next] != 0U)
			next++;
		if (next >= columns * rows)
			continue;

		/* The item takes it. */
		places[index].column = next / rows;
		places[index].row = next % rows;
		if (taken != NULL)
			taken[next] = 1U;
		next++;
	}

	/* The marks are done with. */
	free(taken);

	/* Succeeded: every item has a cell or the overflow marker. */
	return;
}

/*
 * Finds the layout file's path: under $XDG_CONFIG_HOME, or ~/.config.
 * Returns 0, ENOENT without either, or ENAMETOOLONG.
 */
int
fm_desktop_layout_path(
	char *path,
	size_t size)
{
	const char *config;
	const char *home;
	int written;

	/* $XDG_CONFIG_HOME, when it is set. */
	config = getenv("XDG_CONFIG_HOME");
	if (config != NULL && config[0] != '\0') {
		written = snprintf(path, size, "%s/%s", config, LAYOUT_FILE);
		if (written < 0 || (size_t)written >= size)
			return ENAMETOOLONG;

		/* Succeeded: the file under the configuration folder. */
		return 0;
	}

	/* Otherwise ~/.config. */
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return ENOENT;
	written = snprintf(path, size, "%s/.config/%s", home, LAYOUT_FILE);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the file under ~/.config. */
	return 0;
}

/*
 * Reads the layout file into a new array (the caller frees it); a missing
 * file is an empty layout.  Malformed lines are left out.  Returns 0 or
 * ENOMEM.
 */
int
fm_desktop_layout_read(
	const char *path,
	struct fm_desktop_saved **saved,
	size_t *count)
{
	struct fm_desktop_saved *grown;
	char line[LAYOUT_LINE];
	char *first_tab;
	char *second_tab;
	char *end;
	char *read;
	FILE *file;
	size_t capacity;
	long column;
	long row;

	/* Nothing yet. */
	*saved = NULL;
	*count = 0;
	capacity = 0;

	/* A missing file is an empty layout. */
	file = fopen(path, "r");
	if (file == NULL)
		return 0;

	/* Each line: NAME<TAB>COLUMN<TAB>ROW. */
	for (;;) {
		/* The next line, until the file ends. */
		read = fgets(line, sizeof(line), file);
		if (read == NULL)
			break;

		/* The name ends at the first tab, the column at the second. */
		first_tab = strchr(line, '\t');
		if (first_tab == NULL || first_tab == line)
			continue;
		second_tab = strchr(first_tab + 1, '\t');
		if (second_tab == NULL)
			continue;
		*first_tab = '\0';
		*second_tab = '\0';

		/* The column and the row, whole numbers. */
		column = strtol(first_tab + 1, &end, 10);
		if (end == first_tab + 1 || *end != '\0')
			continue;
		row = strtol(second_tab + 1, &end, 10);
		if (end == second_tab + 1)
			continue;

		/* Room for one more. */
		if (*count == capacity) {
			capacity += 16U;
			grown = realloc(*saved, capacity * sizeof(**saved));
			if (grown == NULL) {
				fclose(file);
				free(*saved);
				*saved = NULL;
				*count = 0;
				return ENOMEM;
			}

			/* The grown array. */
			*saved = grown;
		}

		/* A name longer than an item's is not one. */
		if ((size_t)(first_tab - line) >= sizeof((*saved)[*count].name))
			continue;

		/* The saved place. */
		memcpy((*saved)[*count].name, line, (size_t)(first_tab - line) + 1U);
		(*saved)[*count].column = (int)column;
		(*saved)[*count].row = (int)row;
		(*count)++;
	}

	/* The file is done with. */
	fclose(file);

	/* Succeeded: the saved places. */
	return 0;
}

/*
 * Writes the layout file: a new file beside it, renamed over it, the
 * folder made when it is not there (no places: the file is removed).
 * Returns 0 or an errno value with the old file as it was.
 */
int
fm_desktop_layout_write(
	const char *path,
	const struct fm_desktop_saved *saved,
	size_t count)
{
	char fresh[FM_PATH_MAX + sizeof(LAYOUT_NEW_SUFFIX)];
	char folder[FM_PATH_MAX];
	char *slash;
	FILE *file;
	size_t index;
	int written;
	int error;

	/* No places: no file. */
	if (count == 0U) {
		error = unlink(path);
		if (error != 0 && errno != ENOENT)
			return errno;
		return 0;
	}

	/* The folder, and the configuration folder above it, made when they are not there. */
	snprintf(folder, sizeof(folder), "%s", path);
	slash = strrchr(folder, '/');
	if (slash != NULL) {
		*slash = '\0';
		slash = strrchr(folder, '/');
		if (slash != NULL && slash != folder) {
			*slash = '\0';
			(void)mkdir(folder, 0755);
			*slash = '/';
		}

		/* The keiland folder in it. */
		(void)mkdir(folder, 0755);
	}

	/* The new file. */
	snprintf(fresh, sizeof(fresh), "%s%s", path, LAYOUT_NEW_SUFFIX);
	file = fopen(fresh, "w");
	if (file == NULL)
		return errno;

	/* Each place, a line. */
	for (index = 0; index < count; index++) {
		written = fprintf(file, "%s\t%d\t%d\n", saved[index].name, saved[index].column, saved[index].row);
		if (written < 0) {
			fclose(file);
			(void)unlink(fresh);
			return EIO;
		}
	}

	/* The new file, complete. */
	error = fclose(file);
	if (error != 0) {
		(void)unlink(fresh);
		return EIO;
	}

	/* It takes the old one's place at once. */
	error = rename(fresh, path);
	if (error != 0) {
		error = errno;
		(void)unlink(fresh);
		return error;
	}

	/* Succeeded: the layout is written. */
	return 0;
}

/*
 * Keeps the place the user gave an item (a move), in the desktop's saved
 * places and its layout file; the next layout uses it.  Returns 0 or an
 * errno value.
 */
int
fm_desktop_layout_set(
	struct fm_desktop *desk,
	const char *name,
	int column,
	int row)
{
	struct fm_desktop_saved *grown;
	char path[FM_PATH_MAX];
	int found;
	int error;

	/* The item's saved place, or a new one. */
	found = layout_saved_index(desk->saved, desk->saved_count, name);
	if (found < 0) {
		grown = realloc(desk->saved, (desk->saved_count + 1U) * sizeof(desk->saved[0]));
		if (grown == NULL)
			return ENOMEM;
		desk->saved = grown;
		found = (int)desk->saved_count;
		desk->saved_count++;
		snprintf(desk->saved[found].name, sizeof(desk->saved[found].name), "%s", name);
	}

	/* The place. */
	desk->saved[found].column = column;
	desk->saved[found].row = row;
	desk->laid_count = (size_t)-1;

	/* The file. */
	error = fm_desktop_layout_path(path, sizeof(path));
	if (error != 0)
		return error;
	error = fm_desktop_layout_write(path, desk->saved, desk->saved_count);
	fm_log("DESKTOP saved name=%s column=%d row=%d error=%d", name, column, row, error);
	if (error != 0)
		return error;

	/* Succeeded: the place is kept. */
	return 0;
}

/*
 * Forgets saved places whose names are absent from a successful listing.
 *
 * Names without a visible cell still keep their saved place. The layout
 * file changes only when a later placement or rename writes these places.
 */
void
fm_desktop_layout_prune(
	struct fm_desktop *desk,
	const char *const *names,
	size_t count)
{
	size_t saved_index;
	size_t name_index;
	size_t kept;
	size_t removed;
	int differs;
	int present;

	/* Keep each saved place whose name belongs to the complete listing. */
	kept = 0U;
	for (saved_index = 0U; saved_index < desk->saved_count; saved_index++) {
		/* Find this name among all entries, including overflow entries. */
		present = 0;
		for (name_index = 0U; name_index < count; name_index++) {
			differs = strcmp(desk->saved[saved_index].name, names[name_index]);
			if (differs == 0) {
				present = 1;
				break;
			}
		}

		/* Missing entries leave no place in the compacted saved array. */
		if (!present)
			continue;

		/* Preserve the retained places in their original order. */
		desk->saved[kept] = desk->saved[saved_index];
		kept++;
	}

	/* Publish the retained count to the next layout-file writer. */
	removed = desk->saved_count - kept;
	desk->saved_count = kept;

	/* Record only listings that actually removed stale saved names. */
	if (removed != 0U) {
		fm_log("DESKTOP prune removed=%lu kept=%lu",
		       (unsigned long)removed,
		       (unsigned long)kept);
	}

	/* Succeeded: remaining saved places belong to the listing. */
	return;
}

/*
 * Forgets every place the user gave (Clean Up): the items are laid out in
 * order again.  Returns 0 or an errno value.
 */
int
fm_desktop_clean_up(
	struct fm_desktop *desk)
{
	char path[FM_PATH_MAX];
	int error;

	/* No saved places, nor the places shown, and a new layout. */
	free(desk->saved);
	desk->saved = NULL;
	desk->saved_count = 0;
	free(desk->shown);
	desk->shown = NULL;
	desk->shown_count = 0;
	desk->laid_count = (size_t)-1;

	/* No file. */
	error = fm_desktop_layout_path(path, sizeof(path));
	if (error != 0)
		return error;
	error = fm_desktop_layout_write(path, NULL, 0U);
	fm_log("DESKTOP clean-up error=%d", error);
	if (error != 0)
		return error;

	/* Succeeded: the desktop is in order. */
	return 0;
}

/*
 * Gives an item's place a new name (the item was renamed), in the places
 * shown and in the saved places, whose file is written again when the
 * item had one.  Returns 0 or an errno value.
 */
int
fm_desktop_layout_rename(
	struct fm_desktop *desk,
	const char *old_name,
	const char *new_name)
{
	char path[FM_PATH_MAX];
	int found;
	int error;

	/* The place it is shown at keeps it under its new name. */
	found = layout_saved_index(desk->shown, desk->shown_count, old_name);
	if (found >= 0)
		snprintf(desk->shown[found].name, sizeof(desk->shown[found].name), "%s", new_name);

	/* An item the user did not place has nothing saved. */
	found = layout_saved_index(desk->saved, desk->saved_count, old_name);
	if (found < 0)
		return 0;

	/* The saved place under the new name, and the file. */
	snprintf(desk->saved[found].name, sizeof(desk->saved[found].name), "%s", new_name);
	error = fm_desktop_layout_path(path, sizeof(path));
	if (error != 0)
		return error;
	error = fm_desktop_layout_write(path, desk->saved, desk->saved_count);
	fm_log("DESKTOP saved-rename from=%s to=%s error=%d", old_name, new_name, error);
	if (error != 0)
		return error;

	/* Succeeded: the saved place follows the item. */
	return 0;
}

/*
 * Remembers where the items of a layout are shown (names and the
 * desktop's places, in the same order), for the next layout.  Returns 0 or
 * ENOMEM (the places shown before are then forgotten).
 */
int
fm_desktop_remember(
	struct fm_desktop *desk,
	const char *const *names,
	size_t count)
{
	struct fm_desktop_saved *shown;
	size_t index;

	/* The places shown before go. */
	free(desk->shown);
	desk->shown = NULL;
	desk->shown_count = 0;

	/* Room for every placed item. */
	shown = calloc(count + 1U, sizeof(shown[0]));
	if (shown == NULL)
		return ENOMEM;
	desk->shown = shown;

	/* Each item that has a cell, by its name. */
	for (index = 0; index < count && index < desk->place_count; index++) {
		/* An item without a cell is placed afresh next time. */
		if (desk->places[index].column < 0)
			continue;

		/* Its name and cell. */
		snprintf(shown[desk->shown_count].name, sizeof(shown[desk->shown_count].name), "%s", names[index]);
		shown[desk->shown_count].column = desk->places[index].column;
		shown[desk->shown_count].row = desk->places[index].row;
		desk->shown_count++;
	}

	/* Succeeded: the next layout keeps these places. */
	return 0;
}

/*
 * Frees the desktop's places, saved places and places shown.
 */
void
fm_desktop_release(
	struct fm_desktop *desk)
{
	/* The arrays, and nothing is left. */
	free(desk->places);
	free(desk->saved);
	free(desk->shown);
	free(desk->painted_cells);
	memset(desk, 0, sizeof(*desk));

	/* Succeeded: the desktop owns no layout or paint records. */
	return;
}

/*
 * Works out how an item's name is shown under its icon, in lines at most
 * width pixels wide at a size: the whole name on the first line when it
 * fits (second empty); otherwise broken into two lines, at a space just
 * after what fits the first line or in its second half when the rest then
 * fits, else where the first line is full; and when even two lines are too
 * few, the start that fits on the first line and, on the second, an
 * ellipsis and as much of the end as fits there (so the extension, at the
 * very end, shows).  first and second hold FM_DESKTOP_LABEL_MAX bytes each.
 */
void
fm_desktop_label(
	struct kl_text *text,
	const char *name,
	int width,
	unsigned pixels,
	char *first,
	char *second)
{
	size_t length;
	size_t head;
	size_t space;
	size_t tail;
	int wide;

	/* Nothing yet; a name longer than the lines hold is cut there. */
	first[0] = '\0';
	second[0] = '\0';
	length = strlen(name);
	if (length >= FM_DESKTOP_LABEL_MAX - sizeof(LABEL_ELLIPSIS))
		length = label_previous(name, FM_DESKTOP_LABEL_MAX - sizeof(LABEL_ELLIPSIS));

	/* The whole name on one line. */
	wide = kl_text_width(text, name, length, pixels, 0);
	if (wide <= width) {
		memcpy(first, name, length);
		first[length] = '\0';
		return;
	}

	/*
	 * The start that fits the first line, broken at a space when one is
	 * just after it or in its second half (the space is shown on neither
	 * line), with the rest on the second line when it fits there.
	 */
	head = label_prefix(text, name, length, width, pixels);
	space = head;
	if (head < length && name[head] != ' ') {
		while (space > head / 2U && name[space - 1U] != ' ')
			space--;
		if (space > head / 2U)
			space--;
		else
			space = head;
	}

	/* Prefer a word boundary when the remaining text fits on the second line. */
	if (space < length &&
	    name[space] == ' ' &&
	    space > 0U) {
		wide = kl_text_width(text, name + space + 1U, length - space - 1U, pixels, 0);
		if (wide <= width) {
			memcpy(first, name, space);
			first[space] = '\0';
			memcpy(second, name + space + 1U, length - space - 1U);
			second[length - space - 1U] = '\0';
			return;
		}
	}

	/* Otherwise the first line is the whole start that fits (not broken at a space), and the rest on the second when it fits. */
	if (head == 0U)
		head = label_next(name, length, 0U);
	memcpy(first, name, head);
	first[head] = '\0';
	wide = kl_text_width(text, name + head, length - head, pixels, 0);
	if (wide <= width) {
		memcpy(second, name + head, length - head);
		second[length - head] = '\0';
		return;
	}

	/* Too long for two lines: the second line is the ellipsis and the end. */
	tail = label_suffix(text, name, length, head, width, pixels);
	memcpy(second, LABEL_ELLIPSIS, sizeof(LABEL_ELLIPSIS) - 1U);
	memcpy(second + sizeof(LABEL_ELLIPSIS) - 1U, name + tail, length - tail);
	second[sizeof(LABEL_ELLIPSIS) - 1U + length - tail] = '\0';

	/* Succeeded: both output lines contain the fitted name. */
	return;
}

/* Finds a name among the saved places; -1 when it is not there. */
static int
layout_saved_index(
    const struct fm_desktop_saved *saved,
    size_t count,
    const char *name)
{
	size_t index;
	int differs;

	/* Each saved place. */
	for (index = 0; index < count; index++) {
		/* The same name. */
		differs = strcmp(saved[index].name, name);
		if (differs == 0)
			return (int)index;
	}

	/* Not saved. */
	return -1;
}

/* Finds how many bytes of a name's start fit a width (whole characters; 0 when not even one does). */
static size_t
label_prefix(
	struct kl_text *text,
	const char *name,
	size_t length,
	int width,
	unsigned pixels)
{
	size_t fits;
	size_t next;
	int wide;

	/* One character more each time, while the start fits. */
	fits = 0U;
	for (;;) {
		next = label_next(name, length, fits);
		if (next == fits)
			break;
		wide = kl_text_width(text, name, next, pixels, 0);
		if (wide > width)
			break;
		fits = next;
	}

	/* Succeeded: the bytes that fit. */
	return fits;
}

/*
 * Finds where the end of a name shown after an ellipsis starts: as much of
 * the end as fits a width with the ellipsis, not before from.  Returns the
 * byte it starts at.
 */
static size_t
label_suffix(
	struct kl_text *text,
	const char *name,
	size_t length,
	size_t from,
	int width,
	unsigned pixels)
{
	char line[FM_DESKTOP_LABEL_MAX];
	size_t start;
	size_t earlier;
	size_t ellipsis;
	int wide;

	/* The ellipsis, then the end put after it. */
	ellipsis = sizeof(LABEL_ELLIPSIS) - 1U;
	memcpy(line, LABEL_ELLIPSIS, ellipsis);

	/* One character more of the end each time, while it fits. */
	start = length;
	for (;;) {
		earlier = label_previous(name, start);
		if (earlier == start || earlier < from)
			break;
		memcpy(line + ellipsis, name + earlier, length - earlier);
		wide = kl_text_width(text, line, ellipsis + length - earlier, pixels, 0);
		if (wide > width)
			break;
		start = earlier;
	}

	/* Succeeded: where the end starts. */
	return start;
}

/* Gives the byte after the UTF-8 character at a place (the length at the end). */
static size_t
label_next(
	const char *name,
	size_t length,
	size_t at)
{
	/* The end. */
	if (at >= length)
		return length;

	/* Past the character's lead byte and its continuation bytes. */
	at++;
	while (at < length && ((unsigned char)name[at] & 0xc0U) == 0x80U)
		at++;

	/* Succeeded. */
	return at;
}

/* Gives the start of the UTF-8 character before a place (0 at the start). */
static size_t
label_previous(
	const char *name,
	size_t at)
{
	/* The start. */
	if (at == 0U)
		return 0U;

	/* Back over continuation bytes to the lead byte. */
	at--;
	while (at > 0U && ((unsigned char)name[at] & 0xc0U) == 0x80U)
		at--;

	/* Succeeded. */
	return at;
}
