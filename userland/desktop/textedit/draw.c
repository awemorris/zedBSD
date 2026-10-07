/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frame of Text Editor, drawn on the CPU (plan/ws092/design.md
 * section 4): the card, the line numbers, the rows of text with the
 * selection, the places found and the cursor, the scroll bar, the status
 * chip and the message, and over them a dialog.
 *
 * Only the rows in view are drawn.  The window is glass when the compositor has
 * glass: the frame is then clear around the card, whose white lets the
 * frosted desktop show a little.
 */

#include "textedit.h"

#include <stdio.h>
#include <string.h>

/* The colours (0xAARRGGBB, not premultiplied), the light appearance's and the dark one's (ws089-p017). */
#define DRAW_GROUND		kl_theme_choose(0xffe6ecf2U, 0xff16191fU)
#define DRAW_CARD		kl_theme_choose(0xf4fbfcfdU, 0xf4262b34U)
#define DRAW_CARD_OPAQUE	kl_theme_choose(0xfffbfcfdU, 0xff262b34U)
#define DRAW_TEXT		kl_theme_choose(0xff1e293bU, 0xffe2e8f0U)
#define DRAW_FAINT		kl_theme_choose(0xff94a3b8U, 0xff7d8794U)
#define DRAW_NUMBER_NOW		kl_theme_choose(0xff334155U, 0xffcbd5e1U)
#define DRAW_CONTROL		kl_theme_choose(0xff94a3b8U, 0xff7d8794U)
#define DRAW_ROW_NOW		kl_theme_choose(0xfff1f5f9U, 0xff2c313bU)
#define DRAW_SELECTION		kl_theme_choose(0xffcfe3ffU, 0xff28466eU)
#define DRAW_SELECTION_IDLE	kl_theme_choose(0xffe2e8f0U, 0xff3a414dU)
#define DRAW_MATCH		kl_theme_choose(0xfffde68aU, 0xff6b5a1eU)
#define DRAW_MATCH_NOW		kl_theme_choose(0xfffbbf24U, 0xffa77b0fU)
#define DRAW_CURSOR		kl_theme_choose(0xff2563ebU, 0xff60a5faU)
#define DRAW_PREEDIT_LINE	kl_theme_choose(0xff64748bU, 0xff94a3b8U)
#define DRAW_PREEDIT_FOCUS	kl_theme_choose(0xffcfe3ffU, 0xff28466eU)
#define DRAW_PREEDIT_FOCUS_LINE	kl_theme_choose(0xff2563ebU, 0xff60a5faU)
#define DRAW_SCROLL		kl_theme_choose(0x50334155U, 0x50cbd5e1U)
#define DRAW_CHIP		kl_theme_choose(0xecffffffU, 0xec2c313bU)
#define DRAW_CHIP_EDGE		kl_theme_choose(0x1f334155U, 0x1fcbd5e1U)
#define DRAW_CHIP_TEXT		kl_theme_choose(0xff475569U, 0xffcbd5e1U)

/* How long the cursor shows and hides, in milliseconds (as the editor's). */
#define DRAW_BLINK_MS		530U

/* The status chip's text size. */
#define DRAW_SMALL_PIXELS	12U

/* The scroll bar's width and shortest thumb. */
#define DRAW_SCROLL_WIDTH	4
#define DRAW_SCROLL_MIN		24

static void draw_rows(struct te_app *app, struct te_canvas *canvas, const struct te_rect *text);
static void draw_row(struct te_app *app, struct te_canvas *canvas, const struct te_rect *text, size_t row, int top, size_t start, size_t end);
static void draw_span(struct te_app *app, struct te_canvas *canvas, const struct te_rect *text, int top, size_t row_start, size_t row_end, size_t from, size_t to, int newline, uint32_t color);
static void draw_matches(struct te_app *app, struct te_canvas *canvas, const struct te_rect *text, int top, size_t start, size_t end);
static void draw_glyphs(struct te_app *app, struct te_canvas *canvas, const struct te_rect *text, int top, size_t start, size_t end, int composing);
static unsigned draw_preedit(struct te_app *app, struct te_canvas *canvas, const struct te_rect *text, int top, unsigned column);
static void draw_glyph(struct te_app *app, struct te_canvas *canvas, int x, int baseline, uint32_t codepoint, unsigned cells, uint32_t color);
static void draw_numbers(struct te_app *app, struct te_canvas *canvas, const struct te_rect *text);
static void draw_cursor(struct te_app *app, struct te_canvas *canvas, const struct te_rect *text);
static void draw_scroll(struct te_app *app, struct te_canvas *canvas, const struct te_rect *text);
static void draw_status(struct te_app *app, struct te_canvas *canvas);
static void draw_chip(struct te_app *app, struct te_canvas *canvas, int x, int y, const char *text, uint32_t fill, uint32_t color);
static size_t draw_count(struct te_app *app);

/*
 * Draws the whole frame.
 */
void
te_draw(
	struct te_app *app,
	struct te_canvas *canvas)
{
	struct te_rect card;
	struct te_rect text;
	uint32_t fill;

	/* The ground: clear for glass (the compositor draws the frosted desktop), a pale slate otherwise. */
	te_canvas_unclip(canvas);
	fill = DRAW_GROUND;
	if (app->glass)
		fill = 0x00000000U;
	te_canvas_fill(canvas, 0, 0, canvas->width, canvas->height, fill);

	/* The card. */
	te_app_card(app, &card);
	fill = DRAW_CARD_OPAQUE;
	if (app->glass)
		fill = DRAW_CARD;
	te_canvas_round(canvas, card.x, card.y, card.width, card.height, TE_CARD_RADIUS, fill);

	/* The text, its line numbers and its cursor, within the card's text area. */
	te_app_text_rect(app, &text);
	draw_rows(app, canvas, &text);
	draw_numbers(app, canvas, &text);
	draw_cursor(app, canvas, &text);
	te_canvas_unclip(canvas);

	/* The scroll bar and the status (a message's chip and a dialog are libkeiland's, drawn over the frame by main.c). */
	draw_scroll(app, canvas, &text);
	draw_status(app, canvas);

	/* The frame is drawn. */
	app->dirty = 0;
}

/* Draws the rows in view: their backgrounds, the selection, the places found and the characters. */
static void
draw_rows(
	struct te_app *app,
	struct te_canvas *canvas,
	const struct te_rect *text)
{
	size_t first;
	size_t row;
	size_t start;
	size_t end;
	int top;

	/* Only the text area is drawn in (the rows scroll under its edges). */
	te_canvas_clip(canvas, text->x, text->y, text->width, text->height);

	/* From the first row in view until below the area. */
	first = (size_t)(app->scroll_y / (double)app->row_height);
	for (row = first; row < app->layout.total; row++) {
		top = text->y + (int)((double)row * (double)app->row_height - app->scroll_y);
		if (top >= text->y + text->height)
			break;

		/* The row's bytes, and the row. */
		te_layout_row_range(&app->layout, &app->buffer, row, &start, &end);
		draw_row(app, canvas, text, row, top, start, end);
	}
}

/* Draws one row: the cursor's row tint, the selection, the places found, then its characters. */
static void
draw_row(
	struct te_app *app,
	struct te_canvas *canvas,
	const struct te_rect *text,
	size_t row,
	int top,
	size_t start,
	size_t end)
{
	size_t selection_start;
	size_t selection_end;
	size_t cursor_row;
	size_t column;
	size_t line;
	size_t line_end;
	uint32_t color;
	int newline;
	int composing;

	/* The cursor's row is tinted when nothing is selected. */
	te_edit_selection(app, &selection_start, &selection_end);
	te_layout_place(&app->layout, &app->buffer, app->cursor, &cursor_row, &column);
	if (selection_start == selection_end && cursor_row == row)
		te_canvas_fill(canvas, text->x, top, text->width, app->row_height, DRAW_ROW_NOW);

	/* The selection's part in the row (with a cell for the newline it takes in). */
	if (selection_start < selection_end && selection_start <= end && selection_end > start) {
		line = te_buffer_line_of(&app->buffer, start);
		line_end = te_buffer_line_end(&app->buffer, line);
		newline = 0;
		if (end == line_end && selection_end > end)
			newline = 1;
		color = DRAW_SELECTION;
		if (!app->focused)
			color = DRAW_SELECTION_IDLE;
		draw_span(app, canvas, text, top, start, end, selection_start, selection_end, newline, color);
	}

	/* The places the find text occurs. */
	draw_matches(app, canvas, text, top, start, end);

	/* The characters, with the text being composed in the cursor's row. */
	composing = 0;
	if (cursor_row == row && app->preedit[0] != '\0')
		composing = 1;
	draw_glyphs(app, canvas, text, top, start, end, composing);
}

/*
 * Fills the cells of a row from one position to another (clipped to the
 * row), and a cell more for a newline taken in.
 */
static void
draw_span(
	struct te_app *app,
	struct te_canvas *canvas,
	const struct te_rect *text,
	int top,
	size_t row_start,
	size_t row_end,
	size_t from,
	size_t to,
	int newline,
	uint32_t color)
{
	size_t left;
	size_t right;
	int x;

	/* The part within the row. */
	if (from < row_start)
		from = row_start;
	if (to > row_end)
		to = row_end;

	/* Its cells. */
	left = te_layout_columns_between(&app->layout, &app->buffer, row_start, from);
	right = te_layout_columns_between(&app->layout, &app->buffer, row_start, to);
	if (newline)
		right++;
	if (right <= left)
		return;

	/* The fill. */
	x = text->x + (int)((double)left * (double)app->cell - app->scroll_x);
	te_canvas_fill(canvas, x, top, (int)(right - left) * app->cell, app->row_height, color);
}

/* Marks the places in a row where the find text occurs (the one selected more strongly). */
static void
draw_matches(
	struct te_app *app,
	struct te_canvas *canvas,
	const struct te_rect *text,
	int top,
	size_t start,
	size_t end)
{
	size_t position;
	size_t selection_start;
	size_t selection_end;
	uint32_t color;
	int match;

	/* Nothing to mark without a find text. */
	if (app->find_length == 0U)
		return;

	/* Each place in the row where it starts. */
	te_edit_selection(app, &selection_start, &selection_end);
	position = start;
	while (position < end) {
		match = te_find_at(&app->buffer, app->find, app->find_length, position);
		if (!match) {
			position++;
			continue;
		}

		/* The place, stronger when it is the one selected. */
		color = DRAW_MATCH;
		if (position == selection_start && position + app->find_length == selection_end)
			color = DRAW_MATCH_NOW;
		draw_span(app, canvas, text, top, start, end, position, position + app->find_length, 0, color);
		position += app->find_length;
	}
}

/*
 * Draws the characters of a row in their cells (wide ones centred in two,
 * controls as ^X).  In the cursor's row while text is composed, the
 * composed text takes cells at the cursor and the characters after it move
 * on by as many.
 */
static void
draw_glyphs(
	struct te_app *app,
	struct te_canvas *canvas,
	const struct te_rect *text,
	int top,
	size_t start,
	size_t end,
	int composing)
{
	uint32_t codepoint;
	size_t position;
	size_t next;
	size_t column;
	unsigned cells;
	int baseline;
	int x;

	/* The baseline in the row. */
	baseline = top + app->ascent;

	/* Each character, the pen at its cell. */
	column = 0;
	position = start;
	while (position < end) {
		/* The composed text goes in before the character at the cursor. */
		if (composing && position == app->cursor) {
			column += draw_preedit(app, canvas, text, top, (unsigned)column);
			composing = 0;
		}

		/* The character, the cells it takes from its column, and where it is drawn. */
		codepoint = te_buffer_char(&app->buffer, position, &next);
		cells = te_layout_cells(codepoint, (unsigned)column, app->layout.tab);
		x = text->x + (int)((double)column * (double)app->cell - app->scroll_x);

		/* Past the right edge nothing more shows. */
		if (x > text->x + text->width)
			break;

		/* The character in its cells. */
		draw_glyph(app, canvas, x, baseline, codepoint, cells, DRAW_TEXT);

		/* The next cell. */
		column += cells;
		position = next;
	}

	/* A cursor at the row's end has the composed text after the last character. */
	if (composing)
		(void)draw_preedit(app, canvas, text, top, (unsigned)column);
}

/*
 * Draws the text being composed from a column of a row in the body's cells
 * and size, underlined, its segment being converted on the selection's
 * tint with a thicker line; gives the cells it takes.
 */
static unsigned
draw_preedit(
	struct te_app *app,
	struct te_canvas *canvas,
	const struct te_rect *text,
	int top,
	unsigned column)
{
	uint32_t codepoint;
	size_t length;
	size_t index;
	size_t before;
	unsigned cells;
	unsigned used;
	int baseline;
	int focused;
	int x;

	/* The baseline in the row. */
	baseline = top + app->ascent;

	/* Each character of the composed text, in the cells it takes after the ones before it. */
	length = strlen(app->preedit);
	used = 0;
	index = 0;
	while (index < length) {
		before = index;
		codepoint = te_utf8_next(app->preedit, length, &index);
		cells = te_layout_cells(codepoint, column + used, app->layout.tab);
		x = text->x + (int)((double)(column + used) * (double)app->cell - app->scroll_x);

		/* The segment being converted is the bytes from the preedit's cursor's begin to its end. */
		focused = 0;
		if (app->preedit_begin >= 0 && app->preedit_begin < app->preedit_end) {
			if ((int32_t)before >= app->preedit_begin && (int32_t)before < app->preedit_end)
				focused = 1;
		}

		/* The ground and the line under the composed character. */
		if (focused) {
			/* The segment: the selection's tint and a line two pixels thick. */
			te_canvas_fill(canvas, x, top, (int)cells * app->cell, app->row_height, DRAW_PREEDIT_FOCUS);
			te_canvas_fill(canvas, x, top + app->row_height - 3, (int)cells * app->cell, 2, DRAW_PREEDIT_FOCUS_LINE);
		} else {
			/* The rest: the row's ground and a thin line. */
			te_canvas_fill(canvas, x, top, (int)cells * app->cell, app->row_height, DRAW_ROW_NOW);
			te_canvas_fill(canvas, x, top + app->row_height - 2, (int)cells * app->cell, 1, DRAW_PREEDIT_LINE);
		}

		/* The character over them, and the cells used. */
		draw_glyph(app, canvas, x, baseline, codepoint, cells, DRAW_TEXT);
		used += cells;
	}

	/* The cells the composed text takes. */
	return used;
}

/* Draws one character in its cells: a control as ^ and its letter, faint; a wide one centred in two. */
static void
draw_glyph(
	struct te_app *app,
	struct te_canvas *canvas,
	int x,
	int baseline,
	uint32_t codepoint,
	unsigned cells,
	uint32_t color)
{
	int advance;

	/* The kind of character decides how it shows. */
	if (codepoint < 0x20U && codepoint != '\t') {
		/* A control character as ^ and its letter, faint. */
		te_text_draw_char(app->body, canvas, x, baseline, '^', app->pixels, DRAW_CONTROL);
		te_text_draw_char(app->body, canvas, x + app->cell, baseline, codepoint + 0x40U, app->pixels, DRAW_CONTROL);
	} else if (codepoint == 0x7fU) {
		/* Delete as ^?, faint. */
		te_text_draw_char(app->body, canvas, x, baseline, '^', app->pixels, DRAW_CONTROL);
		te_text_draw_char(app->body, canvas, x + app->cell, baseline, '?', app->pixels, DRAW_CONTROL);
	} else if (codepoint != '\t' && codepoint != ' ') {
		/* A character centred in its cells. */
		advance = te_text_advance(app->body, codepoint, app->pixels);
		if (cells == 2U && advance > 0)
			x += (2 * app->cell - advance) / 2;
		te_text_draw_char(app->body, canvas, x, baseline, codepoint, app->pixels, color);
	}
}

/* Draws the line numbers of the rows in view (each line's first row), the cursor's line darker. */
static void
draw_numbers(
	struct te_app *app,
	struct te_canvas *canvas,
	const struct te_rect *text)
{
	char number[24];
	size_t first;
	size_t row;
	size_t line;
	size_t cursor_line;
	uint32_t color;
	int top;
	int width;
	int right;
	int length;

	/* Nothing without line numbers. */
	if (!app->line_numbers)
		return;

	/* The column left of the text, clipped to the text's height. */
	right = text->x - TE_GUTTER_PAD;
	te_canvas_clip(canvas, 0, text->y, text->x, text->height);
	cursor_line = te_buffer_line_of(&app->buffer, app->cursor);

	/* Each row in view that starts a line. */
	first = (size_t)(app->scroll_y / (double)app->row_height);
	for (row = first; row < app->layout.total; row++) {
		top = text->y + (int)((double)row * (double)app->row_height - app->scroll_y);
		if (top >= text->y + text->height)
			break;
		line = te_layout_line_of_row(&app->layout, row);
		if (app->layout.first_row[line] != row)
			continue;

		/* The number, right-aligned, darker on the cursor's line. */
		length = snprintf(number, sizeof(number), "%lu", (unsigned long)(line + 1U));
		width = te_text_width(app->body, number, (size_t)length, app->pixels, 0);
		color = DRAW_FAINT;
		if (line == cursor_line)
			color = DRAW_NUMBER_NOW;
		(void)te_text_draw(app->body, canvas, right - width, top + app->ascent, number, (size_t)length, app->pixels, 0, color);
	}
}

/* Draws the cursor (a bar, blinking) when the window has the keyboard; and text being composed. */
static void
draw_cursor(
	struct te_app *app,
	struct te_canvas *canvas,
	const struct te_rect *text)
{
	size_t row;
	size_t column;
	size_t length;
	size_t offset;
	uint64_t since;
	unsigned cells;
	int top;
	int x;

	/* No cursor without the keyboard, or under a dialog or the chooser. */
	if (!app->focused || app->dialog != TE_DIALOG_NONE || app->choosing)
		return;

	/* Where the cursor is. */
	te_canvas_clip(canvas, text->x - 2, text->y, text->width + 4, text->height);
	te_layout_place(&app->layout, &app->buffer, app->cursor, &row, &column);
	top = text->y + (int)((double)row * (double)app->row_height - app->scroll_y);
	x = text->x + (int)((double)column * (double)app->cell - app->scroll_x);

	/*
	 * While text is composed (drawn with the row, draw_glyphs), the bar is
	 * the preedit's caret: none while a segment is being converted, at its
	 * cursor when it has one, after it when it has none.
	 */
	length = strlen(app->preedit);
	if (length > 0U) {
		if (app->preedit_begin >= 0 && app->preedit_begin < app->preedit_end)
			return;
		offset = length;
		if (app->preedit_begin >= 0 && (size_t)app->preedit_begin < length)
			offset = (size_t)app->preedit_begin;
		cells = te_app_preedit_cells(app, (unsigned)column, offset);
		x += (int)cells * app->cell;
	}

	/* The bar, in the shown half of the blink. */
	since = app->now - app->blink_start;
	if ((since / DRAW_BLINK_MS) % 2U != 0U)
		return;
	te_canvas_fill(canvas, x - 1, top + 1, 2, app->row_height - 2, DRAW_CURSOR);
}

/* Draws the scroll bar at the card's right edge when the text is taller than the view. */
static void
draw_scroll(
	struct te_app *app,
	struct te_canvas *canvas,
	const struct te_rect *text)
{
	struct te_rect card;
	double largest;
	double content;
	int thumb;
	int top;
	int x;

	/* A text that fits has no bar. */
	largest = te_app_max_scroll_y(app);
	if (largest <= 0.0)
		return;

	/* The thumb's length for the share in view, and its place for the offset. */
	content = (double)app->layout.total * (double)app->row_height;
	thumb = (int)((double)text->height * (double)text->height / content);
	if (thumb < DRAW_SCROLL_MIN)
		thumb = DRAW_SCROLL_MIN;
	top = text->y + (int)((double)(text->height - thumb) * app->scroll_y / largest);

	/* The thumb near the card's edge. */
	te_app_card(app, &card);
	x = card.x + card.width - 6 - DRAW_SCROLL_WIDTH;
	te_canvas_round(canvas, x, top, DRAW_SCROLL_WIDTH, thumb, DRAW_SCROLL_WIDTH / 2, DRAW_SCROLL);
}

/* Draws the status chip at the card's bottom right: the cursor's line and column, the selection, CR LF. */
static void
draw_status(
	struct te_app *app,
	struct te_canvas *canvas)
{
	struct te_rect card;
	char status[96];
	size_t line;
	size_t line_start;
	size_t column;
	size_t position;
	size_t selected;
	int width;
	int length;

	/* The cursor's line, and its column in characters. */
	line = te_buffer_line_of(&app->buffer, app->cursor);
	line_start = te_buffer_line_start(&app->buffer, line);
	column = 1;
	for (position = line_start; position < app->cursor; column++)
		position = te_buffer_next_char(&app->buffer, position);

	/* The words, with the selection's length and CR LF when there are. */
	length = snprintf(status, sizeof(status), "Ln %lu, Col %lu", (unsigned long)(line + 1U), (unsigned long)column);
	selected = draw_count(app);
	if (selected > 0U && length > 0 && (size_t)length < sizeof(status))
		length += snprintf(status + length, sizeof(status) - (size_t)length, "  (%lu selected)", (unsigned long)selected);
	if (app->file.crlf && length > 0 && (size_t)length < sizeof(status))
		(void)snprintf(status + length, sizeof(status) - (size_t)length, "  CRLF");

	/* The chip at the bottom right of the card. */
	te_app_card(app, &card);
	width = te_text_width(app->ui, status, strlen(status), DRAW_SMALL_PIXELS, 0) + 20;
	draw_chip(app, canvas, card.x + card.width - 14 - width, card.y + card.height - 12 - 24, status, DRAW_CHIP, DRAW_CHIP_TEXT);
}

/* Draws a chip: a rounded pill with a hairline edge and its words. */
static void
draw_chip(
	struct te_app *app,
	struct te_canvas *canvas,
	int x,
	int y,
	const char *text,
	uint32_t fill,
	uint32_t color)
{
	int width;
	int height;
	unsigned pixels;

	/* The chip's size for its words. */
	pixels = DRAW_SMALL_PIXELS;
	width = te_text_width(app->ui, text, strlen(text), pixels, 0) + 20;
	height = 24;

	/* The edge, the pill and the words. */
	te_canvas_round(canvas, x - 1, y - 1, width + 2, height + 2, height / 2 + 1, DRAW_CHIP_EDGE);
	te_canvas_round(canvas, x, y, width, height, height / 2, fill);
	(void)te_text_draw(app->ui, canvas, x + 10, te_text_center(pixels, y, height), text, strlen(text), pixels, 0, color);
}

/* Reports how many characters are selected, counted again only when the selection or the text changed. */
static size_t
draw_count(
	struct te_app *app)
{
	size_t start;
	size_t end;
	size_t position;
	size_t count;
	size_t length;

	/* Nothing selected. */
	te_edit_selection(app, &start, &end);
	if (end <= start)
		return 0;

	/* The same selection of the same text was counted already. */
	length = te_buffer_length(&app->buffer);
	if (app->counted_valid &&
	    app->counted_cursor == app->cursor &&
	    app->counted_anchor == app->anchor &&
	    app->counted_length == length)
		return app->counted;

	/* Each character. */
	count = 0;
	for (position = start; position < end; count++)
		position = te_buffer_next_char(&app->buffer, position);

	/* Kept for the next frames. */
	app->counted = count;
	app->counted_cursor = app->cursor;
	app->counted_anchor = app->anchor;
	app->counted_length = length;
	app->counted_valid = 1;

	/* Succeeded: the count. */
	return count;
}
