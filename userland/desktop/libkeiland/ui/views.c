/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The views of many items (ws090-p024), in Files' look (files/ui-list.c and
 * files/ui-grid.c until then): a list of several columns under a header
 * whose titles sort it, and a grid of icons with their names; either has
 * many items selected at once, and a rubber band picks them.
 *
 * These are the parts the application draws its view with: the header, an
 * item's ground (the accent under a selected item while the view has the
 * keyboard, a quiet one without it, a faint one under the pointer), a
 * column's text, the grid's measure and an item's place in it, an icon's
 * ground and name, and the band.  The application keeps which items are
 * selected and draws each icon itself (a thumbnail, a file's kind).
 */

#include "internal.h"

#include <string.h>

/* The header's text size, the corner of a title lit under the pointer, and the sort's arrow. */
#define VIEWS_TEXT_HEADER	12U
#define VIEWS_HEADER_RADIUS	6.0f
#define VIEWS_ARROW		12.0f

/* A list row's corner radius, and the opacity of the secondary text on the accent (its ink is the theme's accent_ink). */
#define VIEWS_ROW_RADIUS	7.0f
#define VIEWS_ON_ACCENT_FAINT	210U

/* A grid item: the margin of its lit ground, the corner radius, the icon's top and the name's first baseline under it. */
#define VIEWS_CELL_MARGIN_X	4
#define VIEWS_CELL_MARGIN_Y	2
#define VIEWS_CELL_RADIUS	12.0f
#define VIEWS_ICON_TOP		10
#define VIEWS_ICON_GROUND	6
#define VIEWS_NAME_BELOW	30
#define VIEWS_NAME_SIDE		12
#define VIEWS_PILL_RADIUS	7.0f

/* The band: its corner radius, and its ground's and edge's opacity of the accent. */
#define VIEWS_BAND_RADIUS	3.0f
#define VIEWS_BAND_GROUND	30U
#define VIEWS_BAND_EDGE		150U

static void views_name_lines(const struct kl_style *style, const char *name, unsigned pixels, int limit, size_t *first, char *second, size_t size);

/*
 * Draws a list's header in a rectangle: each column's title (the sorted
 * one in the main ink with its direction, a title under the pointer lit),
 * a line under it, and the edge before each column but the first.
 */
void
kl_list_header(
	const struct kl_style *style,
	const struct kl_rect *rect,
	const struct kl_column *columns,
	size_t count)
{
	const struct kl_theme *theme;
	struct kl_rect line;
	kl_color ink;
	enum kl_icon arrow;
	size_t index;
	int baseline;
	int width;

	/* A thin line under the header. */
	theme = style->theme;
	line.x = rect->x;
	line.y = rect->y + rect->height - 1;
	line.width = rect->width;
	line.height = 1;
	kl_canvas_fill(style->canvas, &line, theme->separator);

	/* Each column's title. */
	baseline = kl_text_center(VIEWS_TEXT_HEADER, rect->y, rect->height);
	for (index = 0; index < count; index++) {
		/* Lit under the pointer. */
		if ((columns[index].flags & KL_COLUMN_HOVER) != 0U)
			kl_canvas_round(style->canvas, (float)columns[index].x - 4.0f, (float)rect->y + 3.0f, (float)columns[index].width, (float)rect->height - 6.0f, VIEWS_HEADER_RADIUS, theme->hover);

		/* The title, in the main ink when the list is sorted by it. */
		ink = theme->text_secondary;
		if ((columns[index].flags & KL_COLUMN_SORTED) != 0U)
			ink = theme->text;
		width = kl_text_draw_fit(style->text, style->canvas, columns[index].x + 4, baseline, columns[index].title, VIEWS_TEXT_HEADER, 1, columns[index].width - 24, ink);

		/* The sort's direction after the sorted one's title. */
		if ((columns[index].flags & KL_COLUMN_SORTED) == 0U)
			continue;
		arrow = KL_ICON_DOWN;
		if ((columns[index].flags & KL_COLUMN_REVERSED) != 0U)
			arrow = KL_ICON_UP;
		kl_icon_draw(style->canvas, arrow, (float)(columns[index].x + width + 8), (float)(baseline - 11), VIEWS_ARROW, ink);
	}

	/* The edge before each column but the first, which a pointer may drag. */
	for (index = 1; index < count; index++) {
		line.x = columns[index].x;
		line.y = rect->y + 8;
		line.width = 1;
		line.height = rect->height - 16;
		kl_canvas_fill(style->canvas, &line, theme->separator);
	}
}

/*
 * Draws a list row's ground for its state (KL_ITEM_* bits) and gives the
 * inks of its text on it: the main one and the secondary one.
 */
void
kl_list_item(
	const struct kl_style *style,
	const struct kl_rect *row,
	unsigned state,
	kl_color *ink,
	kl_color *faint)
{
	const struct kl_theme *theme;
	kl_color ground;

	/* The text on the panel. */
	theme = style->theme;
	*ink = theme->text;
	*faint = theme->text_secondary;

	/* Selected while the view has the keyboard: on the accent. */
	if ((state & KL_ITEM_SELECTED) != 0U && (state & KL_ITEM_FOCUSED) != 0U) {
		kl_canvas_round(style->canvas, (float)row->x, (float)row->y + 1.0f, (float)row->width, (float)row->height - 2.0f, VIEWS_ROW_RADIUS, theme->accent);
		*ink = theme->accent_ink;
		*faint = KL_RGBA(theme->accent_ink, VIEWS_ON_ACCENT_FAINT);
		return;
	}

	/* Selected without it, or under the pointer: a quiet ground. */
	ground = 0U;
	if ((state & KL_ITEM_SELECTED) != 0U)
		ground = theme->selection_inactive;
	else if ((state & KL_ITEM_HOVER) != 0U)
		ground = theme->hover;
	if (ground == 0U)
		return;

	/* Succeeded: the ground under the row. */
	kl_canvas_round(style->canvas, (float)row->x, (float)row->y + 1.0f, (float)row->width, (float)row->height - 2.0f, VIEWS_ROW_RADIUS, ground);
}

/*
 * Draws a cell's text of a list row at a baseline within a column (x and
 * width), cut to fit: at its left, or at its right (KL_CELL_RIGHT, a
 * size); text size pixels.
 */
void
kl_list_cell(
	const struct kl_style *style,
	int x,
	int width,
	int baseline,
	const char *text,
	unsigned pixels,
	unsigned flags,
	kl_color ink)
{
	int measure;

	/* At the column's right, short of the scroll bar's margin. */
	if ((flags & KL_CELL_RIGHT) != 0U) {
		measure = kl_text_width(style->text, text, strlen(text), pixels, 0);
		(void)kl_text_draw(style->text, style->canvas, x + width - 16 - measure, baseline, text, strlen(text), pixels, 0, ink);
		return;
	}

	/* Succeeded: at its left, cut to fit. */
	(void)kl_text_draw_fit(style->text, style->canvas, x + 4, baseline, text, pixels, 0, width - 12, ink);
}

/*
 * Measures a grid of count cells of a size in a rectangle: as many columns
 * as fit (one at least), the grid centred, and its rows' height.
 */
void
kl_grid_layout(
	const struct kl_rect *rect,
	int cell_width,
	int cell_height,
	size_t count,
	struct kl_grid *grid)
{
	/* The columns that fit. */
	memset(grid, 0, sizeof(*grid));
	grid->cell_width = cell_width;
	grid->cell_height = cell_height;
	grid->columns = 1;
	if (cell_width > 0 && rect->width / cell_width > 1)
		grid->columns = rect->width / cell_width;

	/* Centred, from the rectangle's top. */
	grid->left = rect->x + (rect->width - grid->columns * cell_width) / 2;
	grid->top = rect->y;

	/* Succeeded: the rows the cells take and their height. */
	grid->rows = (count + (size_t)grid->columns - 1U) / (size_t)grid->columns;
	grid->content_height = (int)grid->rows * cell_height;
}

/*
 * Gives the place of a grid's cell, scrolled up by scroll pixels.
 */
void
kl_grid_cell(
	const struct kl_grid *grid,
	size_t index,
	int scroll,
	struct kl_rect *cell)
{
	size_t row;
	size_t column;

	/* Its row and column. */
	row = index / (size_t)grid->columns;
	column = index % (size_t)grid->columns;

	/* Succeeded: the cell. */
	cell->x = grid->left + (int)column * grid->cell_width;
	cell->y = grid->top + (int)row * grid->cell_height - scroll;
	cell->width = grid->cell_width;
	cell->height = grid->cell_height;
}

/*
 * Draws a grid item's ground for its state (KL_ITEM_* bits) in its cell --
 * a ground behind its icon of a size when selected, a faint one over the
 * cell under the pointer -- and its name under the icon in up to two
 * centred lines of a text size, on the accent when selected (none when
 * name is NULL: a field takes its place).  The application draws the icon
 * at kl_grid_icon's place.  Returns the baseline of the name's last line.
 */
int
kl_grid_item(
	const struct kl_style *style,
	const struct kl_rect *cell,
	int icon,
	const char *name,
	unsigned pixels,
	unsigned state)
{
	const struct kl_theme *theme;
	struct kl_text_line line;
	char second[KL_GRID_NAME_MAX];
	kl_color selection;
	kl_color ink;
	size_t first;
	int first_width;
	int second_width;
	int baseline;
	int widest;
	int lines;
	int limit;
	int left;

	/* Selected: a ground behind the icon, the accent's while the view has the keyboard. */
	theme = style->theme;
	left = cell->x + (cell->width - icon) / 2;
	if ((state & KL_ITEM_SELECTED) != 0U) {
		selection = theme->selection;
		if ((state & KL_ITEM_FOCUSED) == 0U)
			selection = theme->selection_inactive;
		kl_canvas_round(style->canvas, (float)(left - VIEWS_ICON_GROUND), (float)cell->y + 4.0f, (float)(icon + 2 * VIEWS_ICON_GROUND), (float)(icon + 2 * VIEWS_ICON_GROUND), VIEWS_CELL_RADIUS, selection);
	} else if ((state & KL_ITEM_HOVER) != 0U) {
		kl_canvas_round(style->canvas, (float)(cell->x + VIEWS_CELL_MARGIN_X), (float)(cell->y + VIEWS_CELL_MARGIN_Y), (float)(cell->width - 2 * VIEWS_CELL_MARGIN_X), (float)(cell->height - 2 * VIEWS_CELL_MARGIN_Y), VIEWS_CELL_RADIUS, theme->hover);
	}

	/* No name: the field the application draws in its place. */
	baseline = cell->y + icon + VIEWS_NAME_BELOW;
	if (name == NULL)
		return baseline;

	/* The name's lines: what fits, then the rest cut with an ellipsis. */
	limit = cell->width - VIEWS_NAME_SIDE;
	views_name_lines(style, name, pixels, limit, &first, second, sizeof(second));
	first_width = kl_text_width(style->text, name, first, pixels, 0);
	second_width = 0;
	lines = 1;
	if (second[0] != '\0') {
		second_width = kl_text_width(style->text, second, strlen(second), pixels, 0);
		lines = 2;
	}

	/* A selected name sits on an accent pill as wide as its widest line. */
	kl_text_metrics(style->text, pixels, &line);
	ink = theme->text;
	if ((state & KL_ITEM_SELECTED) != 0U) {
		widest = first_width;
		if (second_width > widest)
			widest = second_width;
		kl_canvas_round(style->canvas, (float)(cell->x + (cell->width - widest) / 2 - 6), (float)(baseline - line.ascent - 3), (float)(widest + 12), (float)(lines * line.height + 5), VIEWS_PILL_RADIUS, theme->accent);
		ink = theme->accent_ink;
	}

	/* The first line, centred. */
	(void)kl_text_draw(style->text, style->canvas, cell->x + (cell->width - first_width) / 2, baseline, name, first, pixels, 0, ink);

	/* A name of one line ends there. */
	if (lines == 1)
		return baseline;

	/* The second line under it. */
	(void)kl_text_draw(style->text, style->canvas, cell->x + (cell->width - second_width) / 2, baseline + line.height, second, strlen(second), pixels, 0, ink);

	/* Reports the second line's baseline. */
	return baseline + line.height;
}

/*
 * Gives where a grid item's icon of a size goes in its cell: centred, near
 * its top.
 */
void
kl_grid_icon(
	const struct kl_rect *cell,
	int icon,
	struct kl_rect *place)
{
	/* Centred across, under the cell's top. */
	place->x = cell->x + (cell->width - icon) / 2;
	place->y = cell->y + VIEWS_ICON_TOP;
	place->width = icon;
	place->height = icon;
}

/*
 * Draws a rubber band over the items it picks: a faint accent box with an
 * edge.
 */
void
kl_band(
	const struct kl_style *style,
	const struct kl_rect *rect)
{
	kl_color accent;

	/* The box and its edge. */
	accent = style->theme->accent;
	kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, VIEWS_BAND_RADIUS, KL_RGBA(accent, VIEWS_BAND_GROUND));
	kl_canvas_round_border(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, VIEWS_BAND_RADIUS, 1.0f, KL_RGBA(accent, VIEWS_BAND_EDGE));
}

/* Breaks a name into the first line that fits a width and the rest cut to fit ("" when one line holds it). */
static void
views_name_lines(
	const struct kl_style *style,
	const char *name,
	unsigned pixels,
	int limit,
	size_t *first,
	char *second,
	size_t size)
{
	/* The first line: what fits. */
	*first = kl_text_break(style->text, name, pixels, 0, limit);
	second[0] = '\0';

	/* A name of one line has no second. */
	if (name[*first] == '\0')
		return;

	/* Succeeded: the rest, cut with an ellipsis. */
	(void)kl_text_fit(style->text, name + *first, pixels, 0, limit, second, size);
}
