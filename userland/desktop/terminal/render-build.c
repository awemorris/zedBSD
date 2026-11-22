/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The quads of terminal's frames: every cell of the grid, the cursor, the
 * search bar and the input method's composed text as two triangles that
 * sample the glyph atlas, the same whichever drawing shows them
 * (display-vulkan.c through the GPU, display-cpu.c on the processor).
 *
 * A row the same as in the frame before, at the same place, is not built
 * again; the drawing takes only the parts built (the build's ranges).
 */

#include "terminal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int build_rows_usable(struct terminal_build *build, const struct terminal_screen *screen, const struct terminal_font *font, const struct terminal_theme *theme, unsigned blank);
static void build_row_range(const struct terminal_screen *screen, unsigned long line, struct terminal_render_row *state);
static int build_row_same(const struct terminal_build *build, const struct terminal_render_row *kept, const struct terminal_render_row *state, const struct terminal_cell *cells, unsigned columns, unsigned index);
static void build_range_add(struct terminal_build *build, size_t first, size_t count);
static float *build_preedit(struct terminal_build *build, const struct terminal_screen *screen, struct terminal_font *font, const struct terminal_window *window, float *vertex, int first_row, int last_row);
static float *build_search_bar(struct terminal_build *build, const struct terminal_screen *screen, struct terminal_font *font, const struct terminal_window *window, const struct terminal_theme *theme, float *vertex);
static uint32_t build_color(const struct terminal_theme *theme, uint32_t color);
static uint32_t build_utf8_next(const char *text, size_t *at);
static float *build_quad(float *vertex, const struct terminal_font *font, float x, float y, float width, float height, unsigned slot, uint32_t foreground, uint32_t background);

/*
 * The colour themes of View > Theme, by TERMINAL_THEME_*:
 * the dark one the terminal always had, a light one and white on black.
 */
static const struct terminal_theme build_themes[TERMINAL_THEMES] = {
	{ TERMINAL_FOREGROUND, TERMINAL_BACKGROUND, TERMINAL_SELECTION, 0x6b5a1eU, 0xe8b830U, 0x101010U, 0x2c3446U },
	{ 0x1f2328U, 0xfafafaU, 0xb6d2f5U, 0xf3e08cU, 0xf0a020U, 0x101010U, 0xe2e6ecU },
	{ 0xffffffU, 0x000000U, 0x1f5fffU, 0x7a5c00U, 0xffd000U, 0x000000U, 0x303030U }
};

/*
 * Makes the build's memory: the vertices of the largest grid, the rows as
 * they were built and their cells.
 *
 * Returns 0, or ENOMEM.
 */
int
terminal_build_open(
	struct terminal_build *build)
{
	/* Every cell's two triangles (four rows more, for a view between lines and the padding) and the cursor's. */
	memset(build, 0, sizeof(*build));
	build->capacity = ((size_t)TERMINAL_MAX_COLUMNS * (TERMINAL_MAX_ROWS + 4U) + 1U) * TERMINAL_QUAD_VERTICES;

	/* Where frames are built. */
	build->vertices = malloc(build->capacity * TERMINAL_VERTEX_FLOATS * sizeof(float));
	if (build->vertices == NULL)
		return ENOMEM;

	/* The rows as they are built, none yet. */
	build->rows = calloc(TERMINAL_RENDER_ROWS, sizeof(*build->rows));
	if (build->rows == NULL)
		return ENOMEM;

	/* Their cells. */
	build->row_cells = malloc((size_t)TERMINAL_RENDER_ROWS * TERMINAL_MAX_COLUMNS * sizeof(*build->row_cells));
	if (build->row_cells == NULL)
		return ENOMEM;

	/* Succeeded: frames can be built. */
	return 0;
}

/*
 * Writes the vertices of every cell, of the cursor, of the search bar and
 * of the composed text, and returns how many there are.
 *
 * Rows the same as in the frame before, at the same place, keep the
 * vertices built then; the parts built are the build's ranges.
 */
uint32_t
terminal_build_frame(
	struct terminal_build *build,
	struct terminal_screen *screen,
	struct terminal_font *font,
	const struct terminal_window *window)
{
	static const struct terminal_cell empty = { ' ', TERMINAL_FOREGROUND, TERMINAL_BACKGROUND, 0 };
	static unsigned char marks[TERMINAL_MAX_COLUMNS];
	const struct terminal_theme *theme;
	const struct terminal_cell *cell;
	const struct terminal_cell *next;
	uint32_t query[TERMINAL_SEARCH_LENGTH];
	size_t query_count;
	int marked;
	int current;
	float *vertex;
	float *start;
	uint32_t foreground;
	uint32_t background;
	unsigned long line;
	unsigned long cursor_line;
	long long signed_line;
	long long oldest;
	long long newest;
	long long top;
	size_t used;
	unsigned column;
	int row;
	int first_row;
	int last_row;
	int inside;
	int selected;
	int wide;
	int cursor_here;
	unsigned cells;
	unsigned slot;
	unsigned blank;
	float x;
	float y;
	const struct terminal_cell *base;
	struct terminal_render_row *kept;
	struct terminal_render_row state;
	float *row_start;
	size_t first;
	int keep;
	int store;
	int same;
	int index;

	/* The blank glyph (a space), which empty cells and the right halves of wide characters use. */
	blank = terminal_font_slot(font, ' ');

	/* The theme's colours, and the text the search bar looks for. */
	theme = terminal_build_theme(window);
	query_count = 0;
	if (window != NULL && window->search_open)
		query_count = terminal_search_decode(window->search_query, window->search_length, query, TERMINAL_SEARCH_LENGTH);

	/*
	 * Rows drawn in the frame before may be kept as the mapping holds
	 * them, unless what every row depends on changed; while the search
	 * marks its matches (its marks depend on the query and the lines
	 * around), every row is built and none is kept.
	 */
	build->row_frame++;
	build->range_count = 0U;
	keep = build_rows_usable(build, screen, font, theme, blank);
	store = 1;
	if (query_count != 0U || (window != NULL && window->search_found))
		store = 0;
	if (!store)
		keep = 0;

	/* The cursor's line, which the window shows only when the view reaches it. */
	cursor_line = screen->scrolled + screen->cursor_row;

	/*
	 * The rows the view shows, moved down by its offset within a line:
	 * the rows that meet the window, of the lines the
	 * terminal keeps (past either end nothing is drawn).
	 */
	first_row = 0;
	last_row = (int)screen->rows - 1;
	if (screen->view_offset > 0)
		first_row = -((screen->view_offset + (int)font->cell_height - 1) / (int)font->cell_height);
	if (screen->view_offset < 0)
		last_row = (int)screen->rows - 1 + (-screen->view_offset + (int)font->cell_height - 1) / (int)font->cell_height;
	oldest = (long long)screen->scrolled - (long long)screen->history_count;
	newest = (long long)screen->scrolled + (long long)screen->rows - 1;
	top = (long long)screen->scrolled - (long long)screen->view;

	/* Each cell of those lines, row after row, from the padded top left. */
	start = build->vertices;
	vertex = start;
	for (row = first_row; row <= last_row; row++) {
		/* A line the terminal keeps, whose row meets the window. */
		signed_line = top + (long long)row;
		if (signed_line < oldest || signed_line > newest)
			continue;
		y = (float)((int)TERMINAL_PADDING + row * (int)font->cell_height + screen->view_offset);
		if (y + (float)font->cell_height <= 0.0f || y >= (float)build->height)
			continue;

		/* A row that would not fit the vertex buffer is not drawn (the buffer holds the most rows a window shows). */
		used = (size_t)(vertex - start) / TERMINAL_VERTEX_FLOATS;
		if (used + (size_t)screen->columns * TERMINAL_QUAD_VERTICES > build->capacity)
			break;
		line = (unsigned long)signed_line;

		/*
		 * The row's own state: its place, the cursor on it, the
		 * selections on it.  A row the same as in the frame before, at
		 * the same place, has its vertices in the mapping already.
		 */
		memset(&state, 0, sizeof(state));
		state.valid = 1;
		state.frame = build->row_frame;
		state.first = used;
		state.y = y;
		state.cursor = -1;
		if (screen->cursor_visible && line == cursor_line)
			state.cursor = (long)screen->cursor_column;
		state.selected = 0;
		if (screen->selected && line >= screen->scrolled)
			state.selected = 1;
		build_row_range(screen, line, &state);
		base = terminal_screen_line_cell(screen, 0U, line);
		kept = NULL;
		index = row + 2;
		if (base != NULL && index >= 0 && index < (int)TERMINAL_RENDER_ROWS)
			kept = &build->rows[index];
		if (keep && kept != NULL) {
			same = build_row_same(build, kept, &state, base, screen->columns, (unsigned)index);
			if (same) {
				kept->frame = build->row_frame;
				vertex += kept->count * TERMINAL_VERTEX_FLOATS;
				continue;
			}
		}
		row_start = vertex;

		/* The cells the search's matches cover on this line. */
		marked = 0;
		if (query_count != 0)
			marked = terminal_search_line(screen, query, query_count, line, marks);
		for (column = 0U; column < screen->columns; column++) {
			/* The line's cell; a line the terminal no longer keeps shows blanks. */
			cell = terminal_screen_line_cell(screen, column, line);
			if (cell == NULL)
				cell = &empty;

			/*
			 * A character whose right half is the next cell is drawn once,
			 * two cells wide, and the right half is not drawn on its own.
			 * A left half whose right half was written over
			 * is drawn in its one cell.
			 */
			wide = 0;
			if (cell->continuation == 0 &&
			    cell->codepoint != 0U &&
			    column + 1U < screen->columns) {
				/* The cell to the right, which is this one's right half when it is a continuation. */
				next = terminal_screen_line_cell(screen, column + 1U, line);
				if (next != NULL && next->continuation != 0)
					wide = 1;
			}

			/* The glyph's slot: the blank for spaces and continuations, two slots for a wide character. */
			cells = 1U;
			slot = blank;
			if (wide) {
				cells = 2U;
				slot = terminal_font_wide_slot(font, cell->codepoint);
			} else if (cell->continuation == 0 &&
				   cell->codepoint != ' ' &&
				   cell->codepoint != 0U) {
				slot = terminal_font_slot(font, cell->codepoint);
			}

			/*
			 * A selected screen (Edit > Select All, the live screen's lines)
			 * or cell (the pointer's range, either half of a wide
			 * character) has the selection's background.
			 */
			foreground = build_color(theme, cell->foreground);
			background = build_color(theme, cell->background);
			inside = terminal_screen_in_range(screen, column, line);
			if (!inside && wide)
				inside = terminal_screen_in_range(screen, column + 1U, line);
			selected = 0;
			if (screen->selected && line >= screen->scrolled)
				selected = 1;
			if (selected || inside)
				background = theme->selection;

			/* A match of the search is marked, the one found most strongly. */
			current = 0;
			if (window != NULL &&
			    window->search_found &&
			    line == window->search_line &&
			    column >= window->search_column &&
			    column < window->search_column + window->search_cells)
				current = 1;
			if (current) {
				background = theme->current;
				foreground = theme->match_text;
			} else if (marked != 0 && marks[column] != 0) {
				background = theme->match;
			}

			/* Whether the cursor is on this cell, or on the right half of this wide character. */
			cursor_here = 0;
			if (screen->cursor_visible && line == cursor_line) {
				/* The cursor's column is this cell, or the right half drawn with it. */
				if (column == screen->cursor_column)
					cursor_here = 1;
				else if (wide && column + 1U == screen->cursor_column)
					cursor_here = 1;
			}

			/* The cursor's cell is drawn with its colours swapped: a block cursor. */
			if (cursor_here) {
				foreground = build_color(theme, cell->background);
				background = build_color(theme, cell->foreground);
			}

			/* The cell's two triangles, over both cells of a wide character. */
			x = (float)(TERMINAL_PADDING + column * font->cell_width);
			vertex = build_quad(vertex, font, x, y, (float)(cells * font->cell_width), (float)font->cell_height, slot, foreground, background);

			/* The right half of a wide character is drawn with its left. */
			if (wide)
				column++;
		}

		/* The row's vertices are stored into the mapping; the row is kept for the next frame, or not. */
		state.count = (size_t)(vertex - row_start) / TERMINAL_VERTEX_FLOATS;
		build_range_add(build, state.first, state.count);
		if (kept != NULL && store) {
			*kept = state;
			memcpy(build->row_cells + (size_t)index * TERMINAL_MAX_COLUMNS, base, (size_t)screen->columns * sizeof(*base));
		} else if (kept != NULL) {
			kept->valid = 0;
		}
	}

	/* What follows the rows is built every frame. */
	first = (size_t)(vertex - start) / TERMINAL_VERTEX_FLOATS;

	/* The search bar over the last row, while it is open. */
	vertex = build_search_bar(build, screen, font, window, theme, vertex);

	/* The input method's text being composed, over the cells from the cursor, or from the search bar's caret while it is open. */
	vertex = build_preedit(build, screen, font, window, vertex, first_row, last_row);
	build_range_add(build, first, (size_t)(vertex - start) / TERMINAL_VERTEX_FLOATS - first);

	/* Reports how many vertices the frame draws. */
	return (uint32_t)((size_t)(vertex - start) / TERMINAL_VERTEX_FLOATS);
}

/*
 * Reports the colours of the window's theme (the dark one without a window).
 */
const struct terminal_theme *
terminal_build_theme(
	const struct terminal_window *window)
{
	/* No window, or a theme the table does not have, is the dark one. */
	if (window == NULL || window->theme >= TERMINAL_THEMES)
		return &build_themes[TERMINAL_THEME_DARK];

	/* The window's theme. */
	return &build_themes[window->theme];
}

/*
 * Releases the build's memory.
 */
void
terminal_build_close(
	struct terminal_build *build)
{
	/* The vertices, the rows and their cells. */
	free(build->vertices);
	free(build->rows);
	free(build->row_cells);
	memset(build, 0, sizeof(*build));
}

/*
 * Tells whether the rows kept from the frame before may be used: what
 * every row depends on beside its own state is as it was (render_key).
 * When it is not, it is kept as it is now and every row is dropped.
 */
static int
build_rows_usable(
	struct terminal_build *build,
	const struct terminal_screen *screen,
	const struct terminal_font *font,
	const struct terminal_theme *theme,
	unsigned blank)
{
	struct terminal_render_key key;
	unsigned index;
	int same;

	/* What the rows depend on now (an atlas holding fewer glyphs started again). */
	memset(&key, 0, sizeof(key));
	key.theme = theme;
	key.font = font;
	key.face = font->face;
	key.pixels_size = font->pixels_size;
	key.cell_width = font->cell_width;
	key.cell_height = font->cell_height;
	key.atlas_width = font->atlas_width;
	key.atlas_height = font->atlas_height;
	key.used = font->used;
	key.blank = blank;
	key.columns = screen->columns;
	key.rows = screen->rows;
	key.width = build->width;
	key.height = build->height;
	same = 1;
	if (key.theme != build->row_key.theme ||
	    key.font != build->row_key.font ||
	    key.face != build->row_key.face ||
	    key.pixels_size != build->row_key.pixels_size ||
	    key.cell_width != build->row_key.cell_width ||
	    key.cell_height != build->row_key.cell_height ||
	    key.atlas_width != build->row_key.atlas_width ||
	    key.atlas_height != build->row_key.atlas_height ||
	    key.used < build->row_key.used ||
	    key.blank != build->row_key.blank ||
	    key.columns != build->row_key.columns ||
	    key.rows != build->row_key.rows ||
	    key.width != build->row_key.width ||
	    key.height != build->row_key.height)
		same = 0;
	build->row_key = key;

	/* Whether the drawing has to take the whole frame again (a key changed shows other glyphs in the same places). */
	build->key_changed = 0;
	if (!same)
		build->key_changed = 1;

	/* Changed: no row is kept. */
	if (!same) {
		for (index = 0U; index < TERMINAL_RENDER_ROWS; index++)
			build->rows[index].valid = 0;
	}
	return same;
}

/*
 * Fills a row's columns of the pointer's range on a line: from its first
 * column to its last, or none (from past to).
 */
static void
build_row_range(
	const struct terminal_screen *screen,
	unsigned long line,
	struct terminal_render_row *state)
{
	/* None. */
	state->range_from = 1U;
	state->range_to = 0U;
	if (!screen->range || line < screen->range_from[1] || line > screen->range_to[1])
		return;

	/* The whole line, but from the range's first cell on its first line and up to its last on its last. */
	state->range_from = 0U;
	state->range_to = ULONG_MAX;
	if (line == screen->range_from[1])
		state->range_from = screen->range_from[0];
	if (line == screen->range_to[1])
		state->range_to = screen->range_to[0];
}

/*
 * Tells whether a row is drawn as it was in the frame before, at the same
 * place: kept then, with the same state and the same cells.
 */
static int
build_row_same(
	const struct terminal_build *build,
	const struct terminal_render_row *kept,
	const struct terminal_render_row *state,
	const struct terminal_cell *cells,
	unsigned columns,
	unsigned index)
{
	const struct terminal_cell *was;
	unsigned column;

	/* Kept in the frame before, at the same place, in the same state. */
	if (!kept->valid || kept->frame + 1U != build->row_frame)
		return 0;
	if (kept->first != state->first ||
	    kept->y != state->y ||
	    kept->cursor != state->cursor ||
	    kept->selected != state->selected ||
	    kept->range_from != state->range_from ||
	    kept->range_to != state->range_to)
		return 0;

	/* The same cells. */
	was = build->row_cells + (size_t)index * TERMINAL_MAX_COLUMNS;
	for (column = 0U; column < columns; column++) {
		if (was[column].codepoint != cells[column].codepoint ||
		    was[column].foreground != cells[column].foreground ||
		    was[column].background != cells[column].background ||
		    was[column].continuation != cells[column].continuation)
			return 0;
	}

	/* Succeeded: drawn as it was. */
	return 1;
}

/* Adds a part of the frame's vertices that was built, joined to the part before it when they meet. */
static void
build_range_add(
	struct terminal_build *build,
	size_t first,
	size_t count)
{
	struct terminal_render_range *last;

	/* Nothing built. */
	if (count == 0U)
		return;

	/* After the part before it. */
	if (build->range_count != 0U) {
		last = &build->ranges[build->range_count - 1U];
		if (last->first + last->count == first) {
			last->count += count;
			return;
		}
	}

	/* A new part; with the list full it is stored at once (no frame reads the vertices while they are built). */
	if (build->range_count == sizeof(build->ranges) / sizeof(build->ranges[0])) {
		build->store(build->store_data, first, count);
		return;
	}
	build->ranges[build->range_count].first = first;
	build->ranges[build->range_count].count = count;
	build->range_count++;
}

/*
 * Writes the vertices of the input method's text being composed: from the
 * cursor's cell rightward, a wide character over two cells, on the
 * selection's background with the segment being converted in reverse, cut
 * at the right edge of the grid.  The cursor's block is drawn over.
 * Returns where the next quad goes.
 */
static float *
build_preedit(
	struct terminal_build *build,
	const struct terminal_screen *screen,
	struct terminal_font *font,
	const struct terminal_window *window,
	float *vertex,
	int first_row,
	int last_row)
{
	const float *start;
	uint32_t codepoint;
	uint32_t foreground;
	uint32_t background;
	size_t at;
	size_t used;
	long long row;
	unsigned column;
	unsigned cells;
	unsigned slot;
	int in_segment;
	int wide;
	float x;
	float y;

	/* Nothing is being composed. */
	if (window == NULL || window->preedit[0] == '\0')
		return vertex;

	/* While the search bar is open: from its caret, on the grid's last row (as the bar draws it). */
	if (window->search_open && screen->rows != 0U) {
		y = (float)(TERMINAL_PADDING + (screen->rows - 1U) * font->cell_height);
		column = terminal_search_bar_column(window->search_query, window->search_length, screen->ambiguous_wide);
	} else {
		/* The cursor's row in the view; a cursor the view does not show shows no composed text. */
		row = (long long)screen->cursor_row + (long long)screen->view;
		if (row < (long long)first_row || row > (long long)last_row)
			return vertex;
		y = (float)((int)TERMINAL_PADDING + (int)row * (int)font->cell_height + screen->view_offset);
		column = screen->cursor_column;
	}

	/* Each character, from there, while it fits the grid and the vertex buffer. */
	start = build->vertices;
	at = 0U;
	while (window->preedit[at] != '\0') {
		/* The character, and where the next one starts. */
		in_segment = 0;
		if (window->preedit_begin >= 0 &&
		    window->preedit_end > window->preedit_begin &&
		    at >= (size_t)window->preedit_begin &&
		    at < (size_t)window->preedit_end)
			in_segment = 1;
		codepoint = build_utf8_next(window->preedit, &at);

		/* Its width in cells, as the grid would place it. */
		cells = 1U;
		wide = terminal_width_wide(codepoint, screen->ambiguous_wide);
		if (wide)
			cells = 2U;

		/* Past the grid's right edge the rest is not drawn. */
		if (column + cells > screen->columns)
			break;

		/* A full vertex buffer draws no more. */
		used = (size_t)(vertex - start) / TERMINAL_VERTEX_FLOATS;
		if (used + TERMINAL_QUAD_VERTICES > build->capacity)
			break;

		/* The glyph: two slots for a wide character. */
		if (cells == 2U) {
			slot = terminal_font_wide_slot(font, codepoint);
		} else {
			slot = terminal_font_slot(font, codepoint);
		}

		/* The segment being converted in reverse, the rest on the selection's background. */
		if (in_segment) {
			foreground = terminal_build_theme(window)->background;
			background = terminal_build_theme(window)->foreground;
		} else {
			foreground = terminal_build_theme(window)->foreground;
			background = terminal_build_theme(window)->selection;
		}

		/* The character's quad, and the next cell. */
		x = (float)(TERMINAL_PADDING + column * font->cell_width);
		vertex = build_quad(vertex, font, x, y, (float)(cells * font->cell_width), (float)font->cell_height, slot, foreground, background);
		column += cells;
	}

	/* Succeeded: the composed text is drawn. */
	return vertex;
}

/*
 * Writes the vertices of the search bar over the grid's last
 * row while it is open: "Find:", the text looked for, a block where the
 * next character goes, and "not found" when the text is nowhere.  Returns
 * where the next quad goes.
 */
static float *
build_search_bar(
	struct terminal_build *build,
	const struct terminal_screen *screen,
	struct terminal_font *font,
	const struct terminal_window *window,
	const struct terminal_theme *theme,
	float *vertex)
{
	char text[TERMINAL_SEARCH_BYTES + 48];
	const float *start;
	uint32_t codepoint;
	uint32_t foreground;
	uint32_t background;
	size_t at;
	size_t used;
	size_t caret;
	unsigned column;
	unsigned cells;
	unsigned slot;
	int wide;
	float x;
	float y;

	/* Only while the bar is open, on a grid with a row for it. */
	if (window == NULL ||
	    !window->search_open ||
	    screen->rows == 0U)
		return vertex;

	/* The bar's words; the block goes after the text looked for. */
	snprintf(text, sizeof(text), "Find: %s", window->search_query);
	caret = strlen(text);
	if (window->search_length != 0U && !window->search_found)
		snprintf(text + caret, sizeof(text) - caret, "   not found");

	/* The last row of the grid, from its first column. */
	y = (float)(TERMINAL_PADDING + (screen->rows - 1U) * font->cell_height);
	start = build->vertices;
	column = 0U;
	at = 0;

	/* Each cell of the row: a character of the words, the block, or the bar's background. */
	while (column < screen->columns) {
		/* A full vertex buffer draws no more. */
		used = (size_t)(vertex - start) / TERMINAL_VERTEX_FLOATS;
		if (used + TERMINAL_QUAD_VERTICES > build->capacity)
			break;

		/* The next character of the words, the block at the caret, or a blank past them. */
		foreground = theme->foreground;
		background = theme->bar;
		codepoint = ' ';
		if (at == caret) {
			background = theme->foreground;
			at++;
			caret = (size_t)-1;
		} else if (at < sizeof(text) && text[at] != '\0') {
			codepoint = build_utf8_next(text, &at);
		}

		/* Its width in cells; a wide character that does not fit ends the row. */
		cells = 1U;
		wide = terminal_width_wide(codepoint, screen->ambiguous_wide);
		if (wide)
			cells = 2U;
		if (column + cells > screen->columns)
			break;

		/* Its glyph. */
		if (cells == 2U) {
			slot = terminal_font_wide_slot(font, codepoint);
		} else {
			slot = terminal_font_slot(font, codepoint);
		}

		/* The cell's quad, and the next cell. */
		x = (float)(TERMINAL_PADDING + column * font->cell_width);
		vertex = build_quad(vertex, font, x, y, (float)(cells * font->cell_width), (float)font->cell_height, slot, foreground, background);
		column += cells;
	}

	/* Succeeded: the bar is drawn. */
	return vertex;
}

/*
 * Reports the colour a cell's colour is drawn in: the text's default
 * colours (the dark theme's, which the cells keep) become the theme's;
 * any other colour is itself.
 */
static uint32_t
build_color(
	const struct terminal_theme *theme,
	uint32_t color)
{
	/* The default foreground. */
	if (color == TERMINAL_FOREGROUND)
		return theme->foreground;

	/* The default background. */
	if (color == TERMINAL_BACKGROUND)
		return theme->background;

	/* Any other colour. */
	return color;
}

/*
 * Reads one UTF-8 character of a text at *at and moves *at past it; a
 * byte that does not start a whole character is read as U+FFFD alone.
 */
static uint32_t
build_utf8_next(
	const char *text,
	size_t *at)
{
	const unsigned char *bytes;
	uint32_t codepoint;
	unsigned remaining;
	unsigned index;

	/* The lead byte says how many continuation bytes follow. */
	bytes = (const unsigned char *)text + *at;
	if (bytes[0] < 0x80U) {
		*at += 1U;
		return bytes[0];
	}

	/* Two, three or four bytes by the lead byte's high bits; any other lead byte is a replacement alone. */
	if ((bytes[0] & 0xe0U) == 0xc0U) {
		codepoint = bytes[0] & 0x1fU;
		remaining = 1U;
	} else if ((bytes[0] & 0xf0U) == 0xe0U) {
		codepoint = bytes[0] & 0x0fU;
		remaining = 2U;
	} else if ((bytes[0] & 0xf8U) == 0xf0U) {
		codepoint = bytes[0] & 0x07U;
		remaining = 3U;
	} else {
		*at += 1U;
		return 0xfffdU;
	}

	/* Each continuation byte; a missing one makes the lead byte a replacement alone. */
	for (index = 1U; index <= remaining; index++) {
		if ((bytes[index] & 0xc0U) != 0x80U) {
			*at += 1U;
			return 0xfffdU;
		}

		/* Its six bits join the character. */
		codepoint = (codepoint << 6) | (bytes[index] & 0x3fU);
	}

	/* Succeeded: the character, and the bytes it took. */
	*at += 1U + remaining;
	return codepoint;
}

/*
 * Writes the six vertices of one quad showing a slot of the atlas (as wide
 * as the quad: two slots side by side for a wide character), and returns
 * where the next quad goes.  The foreground's alpha tells the drawing how
 * to read the slot: 1 for a coverage, 0 for a colour glyph.
 */
static float *
build_quad(
	float *vertex,
	const struct terminal_font *font,
	float x,
	float y,
	float width,
	float height,
	unsigned slot,
	uint32_t foreground,
	uint32_t background)
{
	static const float corners[TERMINAL_QUAD_VERTICES][2] = {
		{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f },
		{ 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f }
	};
	unsigned per_row;
	unsigned index;
	float u;
	float v;
	float slot_width;
	float slot_height;
	float mode;
	int colour;

	/* Where the slot is in the atlas, in texture coordinates. */
	per_row = font->atlas_width / font->cell_width;
	u = (float)((slot % per_row) * font->cell_width) / (float)font->atlas_width;
	v = (float)((slot / per_row) * font->cell_height) / (float)font->atlas_height;
	slot_width = width / (float)font->atlas_width;
	slot_height = (float)font->cell_height / (float)font->atlas_height;

	/* How the slot is read: its colours for a colour glyph, else a coverage between the cell's colours. */
	mode = 1.0f;
	colour = terminal_font_colour(font, slot);
	if (colour)
		mode = 0.0f;

	/* Each corner: position and atlas place, then the two colours. */
	for (index = 0U; index < TERMINAL_QUAD_VERTICES; index++) {
		vertex[0] = x + corners[index][0] * width;
		vertex[1] = y + corners[index][1] * height;
		vertex[2] = u + corners[index][0] * slot_width;
		vertex[3] = v + corners[index][1] * slot_height;
		vertex[4] = (float)((foreground >> 16) & 0xffU) / 255.0f;
		vertex[5] = (float)((foreground >> 8) & 0xffU) / 255.0f;
		vertex[6] = (float)(foreground & 0xffU) / 255.0f;
		vertex[7] = mode;
		vertex[8] = (float)((background >> 16) & 0xffU) / 255.0f;
		vertex[9] = (float)((background >> 8) & 0xffU) / 255.0f;
		vertex[10] = (float)(background & 0xffU) / 255.0f;
		vertex[11] = 1.0f;
		vertex += TERMINAL_VERTEX_FLOATS;
	}

	/* Reports where the next quad starts. */
	return vertex;
}
