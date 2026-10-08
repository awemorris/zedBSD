/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Find and the selection of PDF Viewer (ws128-p004): the words of the
 * pages, as libpdf reads them (pdf_page_text_open: each character with its
 * corners on the page), searched and selected.
 *
 * Find: the titlebar's find field (Ctrl+F gives it the keyboard) brings
 * the words as they are typed; the first place they are found from the
 * page in view is shown and marked, and Enter in the field, F3 or Edit >
 * Find Next go on to the next (Shift+F3, Find Previous: back), round the
 * document.  Letters match without their case (ASCII).  Every place the
 * words are found on a page drawn is marked in yellow, the one shown in
 * orange.
 *
 * The selection: a press of the pointer on a character of a page starts
 * a selection there instead of a drag of the view; the drag selects the
 * characters of the page up to the one nearest the pointer, in the page's
 * order; a press without a drag lets it go.  Ctrl+C (Edit > Copy) copies
 * the characters, a newline at each line's end, for main.c to put on the
 * clipboard.  Esc lets the selection and the marks go.
 *
 * The pages' words are read once a page and kept with the page
 * (document.c frees them).
 */

#include "viewer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The marks' colours (0xAARRGGBB, not premultiplied): the places found (yellow), the one shown (orange), the selection (Kei's blue). */
#define FIND_MATCH		0x60ffd400U
#define FIND_CURRENT		0x80ff8a00U
#define FIND_SELECTION		((pv_draw_accent() & 0x00ffffffU) | 0x50000000U)

/* Where the place shown stands in the view: across its middle, a third down. */
#define FIND_SHOW_DOWN		3.0

/* How far a press in the selection moves before its words are dragged out of the window (pixels, ws189-p003). */
#define FIND_DRAG_DISTANCE	6

static const struct pdf_page_text *find_page_text(struct pv_app *app, size_t index);
static size_t find_query(const char *utf8, uint32_t *characters, size_t capacity);
static int find_match(const struct pdf_page_text *text, size_t at, const uint32_t *query, size_t length);
static uint32_t find_fold(uint32_t character);
static void find_reveal(struct pv_app *app);
static int find_character_at(const struct pdf_page_text *text, double x, double y, int nearest, size_t *index);
static void find_mark(struct pv_canvas *canvas, const struct pdf_page_text *text, size_t from, size_t count, int x, int y, double scale, uint32_t color);
static size_t find_utf8(uint32_t character, char *out);

/*
 * Takes the find field's words as they are typed: the first place they are
 * found from the start of the page in view is shown (none: the marks go).
 */
void
pv_find_text(
	struct pv_app *app,
	const char *query)
{
	/* The words, and no place found yet. */
	(void)snprintf(app->find_query, sizeof(app->find_query), "%s", query);
	app->find_found = 0;
	app->dirty = 1;
	if (app->find_query[0] == '\0' || !app->has_document)
		return;

	/* From the start of the page in view. */
	app->find_page = pv_app_current_page(app);
	app->find_from = 0;
	app->find_found = 0;
	pv_find_next(app, 0);
}

/*
 * Shows the next place the words are found (direction 1), the one before
 * (-1), or the first from the place kept (0), round the document; logs it.
 */
void
pv_find_next(
	struct pv_app *app,
	int direction)
{
	const struct pdf_page_text *text;
	uint32_t query[sizeof(app->find_query)];
	size_t length;
	size_t page;
	size_t at;
	size_t tried;
	size_t count;
	int found;
	int first;

	/* The words as characters. */
	length = find_query(app->find_query, query, sizeof(query) / sizeof(query[0]));
	if (length == 0 || !app->has_document)
		return;

	/* Where to look from: the place found (past it), or the page in view. */
	count = app->document.count;
	page = app->find_page;
	at = app->find_from;
	if (page >= count) {
		page = pv_app_current_page(app);
		at = 0;
	}

	/* Past the place shown, going on. */
	if (app->find_found && direction > 0)
		at++;

	/* Each page round the document once, the first from the place, then whole. */
	found = 0;
	first = 1;
	for (tried = 0; tried <= count && !found; tried++) {
		text = find_page_text(app, page);
		if (text != NULL && text->count >= length) {
			if (direction >= 0) {
				/* Forward from the place (the first page) or the start. */
				if (!first)
					at = 0;
				for (; at + length <= text->count; at++) {
					found = find_match(text, at, query, length);
					if (found)
						break;
				}
			} else {
				/* Back from before the place (the first page) or the end. */
				if (!first || !app->find_found)
					at = text->count - length + 1U;
				while (at > 0) {
					at--;
					found = find_match(text, at, query, length);
					if (found)
						break;
				}
			}
		}

		/* Found, or the next page that way. */
		if (found)
			break;
		first = 0;
		if (direction >= 0) {
			page = (page + 1U) % count;
		} else {
			page = (page + count - 1U) % count;
		}
	}

	/* Not found: the marks go (logged). */
	if (!found) {
		app->find_found = 0;
		pv_log("FIND none query=\"%s\"", app->find_query);
		pv_app_message(app, "Not found", 2000U);
		app->dirty = 1;
		return;
	}

	/* Found: the place kept, shown (logged). */
	app->find_found = 1;
	app->find_page = page;
	app->find_from = at;
	app->find_length = length;
	pv_log("FIND found query=\"%s\" page=%lu from=%lu length=%lu", app->find_query, (unsigned long)page, (unsigned long)at,
	       (unsigned long)length);
	find_reveal(app);
}

/*
 * Takes the pointer's button for the selection (event in the pages' view):
 * a press on a character starts a selection there; the release ends it (a
 * press without a drag lets it go).  Returns 1 when the selection took it.
 */
int
pv_select_button(
	struct pv_app *app,
	const struct pv_event *event)
{
	const struct pdf_page_text *text;
	struct pv_place place;
	size_t index;
	size_t low;
	size_t high;
	int hit;

	/* The left button, over a document. */
	if (event->button != PV_BUTTON_LEFT || !app->has_document)
		return 0;

	/* A release of a press in the selection that did not drag: a click, which lets the selection go (ws189-p003). */
	if (!event->pressed && app->text_drag_armed) {
		app->text_drag_armed = 0;
		app->has_selection = 0;
		app->dirty = 1;
		return 1;
	}

	/* A release ends a selection under way. */
	if (!event->pressed) {
		if (!app->selecting)
			return 0;
		app->selecting = 0;
		if (app->select_anchor == app->select_caret && !app->select_moved) {
			app->has_selection = 0;
			app->dirty = 1;
			return 1;
		}

		/* Selected (logged). */
		pv_log("SELECT page=%lu from=%lu to=%lu", (unsigned long)app->select_page, (unsigned long)app->select_anchor,
		       (unsigned long)app->select_caret);
		return 1;
	}

	/* A press on a character of the page under it. */
	pv_app_place_at(app, (double)event->x, (double)event->y, &place);
	text = find_page_text(app, place.page);
	if (text == NULL)
		return 0;
	hit = find_character_at(text, place.x, place.y, 0, &index);
	if (!hit) {
		/* Off the words: the selection goes, the press drags the view. */
		if (app->has_selection) {
			app->has_selection = 0;
			app->dirty = 1;
		}

		/* Not taken. */
		return 0;
	}

	/* A press in the selection may drag its words out of the window, once it moves (ws189-p003). */
	if (app->has_selection && place.page == app->select_page) {
		low = app->select_anchor;
		high = app->select_caret;
		if (high < low) {
			low = app->select_caret;
			high = app->select_anchor;
		}

		/* The press is within it (a selection of one place is none). */
		if (index >= low && index <= high && low != high) {
			app->text_drag_armed = 1;
			app->text_press_x = event->x;
			app->text_press_y = event->y;
			return 1;
		}
	}

	/* The selection starts at the character. */
	app->selecting = 1;
	app->select_moved = 0;
	app->has_selection = 1;
	app->select_page = place.page;
	app->select_anchor = index;
	app->select_caret = index;
	app->dirty = 1;
	return 1;
}

/*
 * Follows the pointer while a selection is under way: up to the character
 * of the page nearest it.  Returns 1 when the selection took the motion.
 */
int
pv_select_motion(
	struct pv_app *app,
	const struct pv_event *event)
{
	const struct pdf_page_text *text;
	struct pv_place place;
	double scale;
	double top;
	double left;
	size_t index;
	int distance_x;
	int distance_y;
	int hit;

	/* A press in the selection moved far enough: its words are dragged out (main.c, ws189-p003). */
	if (app->text_drag_armed) {
		distance_x = abs(event->x - app->text_press_x);
		distance_y = abs(event->y - app->text_press_y);
		if (distance_x > FIND_DRAG_DISTANCE || distance_y > FIND_DRAG_DISTANCE) {
			app->text_drag_armed = 0;
			app->drag_request = PV_DRAG_TEXT;
		}

		/* The motion is the press's. */
		return 1;
	}

	/* A selection under way. */
	if (!app->selecting)
		return 0;

	/* The pointer's point on the selection's page (whichever page is under it). */
	scale = pv_app_scale(app, app->select_page);
	top = pv_app_page_top(app, app->select_page);
	left = pv_app_page_left(app, app->select_page);
	place.page = app->select_page;
	place.x = ((double)event->x - left) / scale;
	place.y = (app->scroll_y + (double)event->y - top) / scale;

	/* The nearest character. */
	text = find_page_text(app, app->select_page);
	if (text == NULL)
		return 1;
	hit = find_character_at(text, place.x, place.y, 1, &index);
	if (hit && index != app->select_caret) {
		app->select_caret = index;
		app->select_moved = 1;
		app->dirty = 1;
	}

	/* Taken. */
	return 1;
}

/*
 * Copies the selection (Ctrl+C, Edit > Copy) as UTF-8, a newline at each
 * line's end within it, for main.c to put on the clipboard (logged).
 */
void
pv_select_copy(
	struct pv_app *app)
{
	const struct pdf_page_text *text;
	char *out;
	size_t from;
	size_t to;
	size_t at;
	size_t length;

	/* A selection of a page with words. */
	if (!app->has_selection)
		return;
	text = find_page_text(app, app->select_page);
	if (text == NULL)
		return;
	from = app->select_anchor;
	to = app->select_caret;
	if (from > to) {
		from = app->select_caret;
		to = app->select_anchor;
	}

	/* A selection within the page's words. */
	if (to >= text->count)
		return;

	/* Room for four bytes a character and a newline, and the NUL. */
	out = malloc((to - from + 1U) * 5U + 1U);
	if (out == NULL)
		return;

	/* Each character, a newline after a line's end (not after the last). */
	length = 0;
	for (at = from; at <= to; at++) {
		length += find_utf8(text->characters[at].character, out + length);
		if ((text->characters[at].flags & PDF_TEXT_LINE_END) != 0U && at < to) {
			out[length] = '\n';
			length++;
		}
	}

	/* Ended. */
	out[length] = '\0';

	/* For main.c (an earlier copy not yet taken goes). */
	free(app->copy_text);
	app->copy_text = out;
	app->copy_length = length;
	pv_log("COPY bytes=%lu text=\"%.40s\"", (unsigned long)length, out);
}

/*
 * Lets the selection and the marks of Find go (Esc, another document).
 */
void
pv_find_clear(
	struct pv_app *app)
{
	/* Nothing selected or found. */
	app->has_selection = 0;
	app->selecting = 0;
	app->find_found = 0;
	app->find_page = (size_t)-1;
	app->dirty = 1;
}

/*
 * Marks a page drawn with its top left at a place and at a scale: every
 * place the words are found on it, the one shown, and the selection.
 */
void
pv_find_draw(
	struct pv_app *app,
	struct pv_canvas *canvas,
	size_t index,
	int x,
	int y,
	double scale)
{
	const struct pdf_page_text *text;
	uint32_t query[sizeof(app->find_query)];
	size_t length;
	size_t at;
	size_t from;
	size_t to;
	int found;

	/* Nothing to mark, or no words on the page. */
	length = find_query(app->find_query, query, sizeof(query) / sizeof(query[0]));
	if (!app->has_selection && (length == 0 || !app->find_found))
		return;
	if ((!app->has_selection || app->select_page != index) && (!app->find_found || length == 0))
		return;
	text = find_page_text(app, index);
	if (text == NULL)
		return;

	/* Every place found on the page, the one shown brighter. */
	if (app->find_found && length > 0) {
		for (at = 0; at + length <= text->count; at++) {
			found = find_match(text, at, query, length);
			if (!found)
				continue;
			if (index == app->find_page && at == app->find_from)
				find_mark(canvas, text, at, length, x, y, scale, FIND_CURRENT);
			else
				find_mark(canvas, text, at, length, x, y, scale, FIND_MATCH);
		}
	}

	/* The selection. */
	if (app->has_selection && app->select_page == index) {
		from = app->select_anchor;
		to = app->select_caret;
		if (from > to) {
			from = app->select_caret;
			to = app->select_anchor;
		}

		/* Marked, within the page's words. */
		if (to < text->count)
			find_mark(canvas, text, from, to - from + 1U, x, y, scale, FIND_SELECTION);
	}
}

/* Gives a page's words, read once (NULL: none, or not read). */
static const struct pdf_page_text *
find_page_text(
	struct pv_app *app,
	size_t index)
{
	struct pv_page *page;
	int error;

	/* A page of the document. */
	if (!app->has_document || index >= app->document.count)
		return NULL;
	page = &app->document.pages[index];

	/* Read the first time (logged), kept after. */
	if (!page->text_read) {
		page->text_read = 1;
		error = pdf_page_text_open(app->document.document, index, &page->text);
		if (error != 0) {
			page->text = NULL;
			pv_log("TEXT failed page=%lu error=%d", (unsigned long)index, error);
		} else {
			pv_log("TEXT page=%lu characters=%lu", (unsigned long)index, (unsigned long)page->text->count);
		}
	}

	/* Kept. */
	return page->text;
}

/* Reads UTF-8 words into characters (at most capacity); returns how many. */
static size_t
find_query(
	const char *utf8,
	uint32_t *characters,
	size_t capacity)
{
	const unsigned char *bytes;
	uint32_t character;
	size_t count;
	size_t at;
	unsigned more;

	/* Each character, its lead byte then its continuations. */
	bytes = (const unsigned char *)utf8;
	count = 0;
	at = 0;
	while (bytes[at] != '\0' && count < capacity) {
		/* The lead byte. */
		character = bytes[at];
		more = 0;
		if (character >= 0xf0U) {
			character &= 0x07U;
			more = 3;
		} else if (character >= 0xe0U) {
			character &= 0x0fU;
			more = 2;
		} else if (character >= 0xc0U) {
			character &= 0x1fU;
			more = 1;
		}

		/* Past the lead byte. */
		at++;

		/* Its continuations. */
		while (more > 0 && (bytes[at] & 0xc0U) == 0x80U) {
			character = (character << 6) | (bytes[at] & 0x3fU);
			at++;
			more--;
		}

		/* The character. */
		characters[count] = character;
		count++;
	}

	/* Reports them. */
	return count;
}

/* Tells whether the words are found at a character of a page (letters without their case). */
static int
find_match(
	const struct pdf_page_text *text,
	size_t at,
	const uint32_t *query,
	size_t length)
{
	uint32_t left;
	uint32_t right;
	size_t in;

	/* Each character of the words. */
	for (in = 0; in < length; in++) {
		left = find_fold(text->characters[at + in].character);
		right = find_fold(query[in]);
		if (left != right)
			return 0;
	}

	/* All the same. */
	return 1;
}

/* Gives an ASCII letter in lower case, any other character as it is. */
static uint32_t
find_fold(
	uint32_t character)
{
	/* ASCII capitals only. */
	if (character >= 'A' && character <= 'Z')
		return character - 'A' + 'a';
	return character;
}

/* Shows the place found: its page, with the place across the view's middle and a third down. */
static void
find_reveal(
	struct pv_app *app)
{
	const struct pdf_page_text *text;
	const double *quad;
	struct pv_place place;

	/* The place's first character. */
	text = find_page_text(app, app->find_page);
	if (text == NULL || app->find_from >= text->count)
		return;
	quad = text->characters[app->find_from].quad;

	/* Its page, then the place under the point. */
	pv_app_go_to(app, app->find_page);
	place.page = app->find_page;
	place.x = (quad[0] + quad[2]) / 2.0;
	place.y = (quad[1] + quad[7]) / 2.0;
	pv_app_show_place(app, &place, (double)app->width / 2.0, (double)app->height / FIND_SHOW_DOWN);
	app->dirty = 1;
}

/*
 * Finds the character of a page at a point (points): the one whose corners
 * hold it, or (nearest) the one nearest it.  Returns 1 when one is found.
 */
static int
find_character_at(
	const struct pdf_page_text *text,
	double x,
	double y,
	int nearest,
	size_t *index)
{
	const double *quad;
	double left;
	double right;
	double top;
	double bottom;
	double away_x;
	double away_y;
	double distance;
	double best;
	size_t at;
	unsigned corner;
	int found;

	/* Each character's box. */
	found = 0;
	best = 0.0;
	for (at = 0; at < text->count; at++) {
		quad = text->characters[at].quad;
		left = quad[0];
		right = quad[0];
		top = quad[1];
		bottom = quad[1];
		for (corner = 1; corner < 4U; corner++) {
			left = fmin(left, quad[corner * 2U]);
			right = fmax(right, quad[corner * 2U]);
			top = fmin(top, quad[corner * 2U + 1U]);
			bottom = fmax(bottom, quad[corner * 2U + 1U]);
		}

		/* How far the point is from the box (0 inside). */
		away_x = fmax(fmax(left - x, x - right), 0.0);
		away_y = fmax(fmax(top - y, y - bottom), 0.0);
		distance = away_x * away_x + away_y * away_y * 4.0;
		if (distance == 0.0) {
			*index = at;
			return 1;
		}

		/* The nearest so far. */
		if (nearest && (!found || distance < best)) {
			best = distance;
			*index = at;
			found = 1;
		}
	}

	/* The nearest, when asked for. */
	return found;
}

/* Fills the boxes of characters of a page drawn at a place and a scale with a translucent colour. */
static void
find_mark(
	struct pv_canvas *canvas,
	const struct pdf_page_text *text,
	size_t from,
	size_t count,
	int x,
	int y,
	double scale,
	uint32_t color)
{
	const double *quad;
	double left;
	double right;
	double top;
	double bottom;
	size_t at;
	unsigned corner;

	/* Each character's box in the canvas. */
	for (at = from; at < from + count && at < text->count; at++) {
		quad = text->characters[at].quad;
		left = quad[0];
		right = quad[0];
		top = quad[1];
		bottom = quad[1];
		for (corner = 1; corner < 4U; corner++) {
			left = fmin(left, quad[corner * 2U]);
			right = fmax(right, quad[corner * 2U]);
			top = fmin(top, quad[corner * 2U + 1U]);
			bottom = fmax(bottom, quad[corner * 2U + 1U]);
		}

		/* Blended over the page. */
		pv_canvas_blend(canvas, x + (int)floor(left * scale), y + (int)floor(top * scale), (int)ceil((right - left) * scale),
				(int)ceil((bottom - top) * scale), color);
	}
}

/* Writes a character as UTF-8; returns its bytes. */
static size_t
find_utf8(
	uint32_t character,
	char *out)
{
	/* By its size. */
	if (character < 0x80U) {
		out[0] = (char)character;
		return 1;
	}

	/* Two bytes. */
	if (character < 0x800U) {
		out[0] = (char)(0xc0U | (character >> 6));
		out[1] = (char)(0x80U | (character & 0x3fU));
		return 2;
	}

	/* Three bytes. */
	if (character < 0x10000U) {
		out[0] = (char)(0xe0U | (character >> 12));
		out[1] = (char)(0x80U | ((character >> 6) & 0x3fU));
		out[2] = (char)(0x80U | (character & 0x3fU));
		return 3;
	}

	/* Four bytes. */
	out[0] = (char)(0xf0U | (character >> 18));
	out[1] = (char)(0x80U | ((character >> 12) & 0x3fU));
	out[2] = (char)(0x80U | ((character >> 6) & 0x3fU));
	out[3] = (char)(0x80U | (character & 0x3fU));
	return 4;
}
