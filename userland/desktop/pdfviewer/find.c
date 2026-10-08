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
 * Find: the titlebar's find field (Ctrl+F gives it the keyboard; without
 * the titlebar, the find field inside the window, ws177-p043) brings the
 * words as they are typed; the first place they are found from the page
 * in view is shown and marked, and Enter in the field, F3 or Edit > Find
 * Next go on to the next (Shift+F3, Find Previous: back), round the
 * document.  Every place the words are found on a page drawn is marked in
 * yellow, the one shown in orange.
 *
 * ws177-p041: the words and each page's characters are compared as keys:
 * the characters folded (the cases of Latin, Greek and Cyrillic letters,
 * full and half width, a voiced mark composed with its kana, ligatures and
 * a sharp s spelled out), each run of spaces one space, and after a line's
 * end a space that may be passed over (a hyphen that ends a line may be
 * too), so that a word broken across lines is found as one.  As the clock
 * ticks, the pages are read in turn and the places counted ("Match 3 of
 * 17"); words found nowhere when some characters could not be read
 * (U+FFFD) say so.
 *
 * The selection: a press of the pointer on a character of a page starts
 * a selection there instead of a drag of the view; the drag selects the
 * characters up to the one nearest the pointer, on whichever page it is
 * over (ws177-p042), in the pages' order; a press without a drag lets it
 * go.  Two clicks select a word, three a line; Ctrl+A (Edit > Select All)
 * the whole document.  A long press of a finger selects a word and shows
 * two handles, which a finger moves (touch.c).  Ctrl+C (Edit > Copy) copies
 * the characters, a newline at each line's and each page's end, for main.c
 * to put on the clipboard.  Esc lets the selection and the marks go.  The
 * marks fill each character's corners as they lie (a turned character
 * turned).
 *
 * The pages' words and keys are read once a page and kept with the page
 * (document.c frees them).
 */

#include "viewer.h"

#include <errno.h>
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

/* How long the ticks may read pages to count the places found, each time (milliseconds, ws177-p041). */
#define FIND_COUNT_BUDGET_MS	8U

/* How long the message of a place found stays (milliseconds). */
#define FIND_MESSAGE_MS		2000U

/* The most keys one character folds into (a ligature of three letters). */
#define FIND_FOLD_MAX		3U

/* How close in time and place a press must follow the last to count as its second or third click. */
#define FIND_CLICK_MS		400U
#define FIND_CLICK_DISTANCE	4

/* The most bytes Copy puts on the clipboard (whole pages up to it). */
#define FIND_COPY_MAX		(4U * 1024U * 1024U)

/* The handles of a selection made with a finger: their radius, and how far from one a finger still takes it (pixels). */
#define FIND_HANDLE_RADIUS	8
#define FIND_HANDLE_REACH	24.0

/* The kinds of characters a word is a run of (two clicks, a long press). */
#define FIND_CLASS_OTHER	0
#define FIND_CLASS_LETTER	1
#define FIND_CLASS_KANJI	2
#define FIND_CLASS_HIRAGANA	3
#define FIND_CLASS_KATAKANA	4

/*
 * A page's characters as the keys Find compares: one folded character
 * each, the first and the last character of the page's text it stands for,
 * and whether it may be passed over (the space after a line's end, a
 * hyphen that ends a line).  The page keeps it (pv_page.find_key) until
 * the document closes.
 */
struct pv_find_key {
	size_t count;
	size_t capacity;
	uint32_t *keys;
	size_t *origins;
	size_t *lasts;
	unsigned char *optional;
};

/*
 * The half width katakana U+FF61 to U+FF9F as their full width forms (the
 * voiced marks as the combining ones, which compose with the kana before).
 */
static const uint16_t find_half_width[63] = {
	0x3002U, 0x300cU, 0x300dU, 0x3001U, 0x30fbU, 0x30f2U, 0x30a1U, 0x30a3U, 0x30a5U, 0x30a7U, 0x30a9U, 0x30e3U, 0x30e5U,
	0x30e7U, 0x30c3U, 0x30fcU, 0x30a2U, 0x30a4U, 0x30a6U, 0x30a8U, 0x30aaU, 0x30abU, 0x30adU, 0x30afU, 0x30b1U, 0x30b3U,
	0x30b5U, 0x30b7U, 0x30b9U, 0x30bbU, 0x30bdU, 0x30bfU, 0x30c1U, 0x30c4U, 0x30c6U, 0x30c8U, 0x30caU, 0x30cbU, 0x30ccU,
	0x30cdU, 0x30ceU, 0x30cfU, 0x30d2U, 0x30d5U, 0x30d8U, 0x30dbU, 0x30deU, 0x30dfU, 0x30e0U, 0x30e1U, 0x30e2U, 0x30e4U,
	0x30e6U, 0x30e8U, 0x30e9U, 0x30eaU, 0x30ebU, 0x30ecU, 0x30edU, 0x30efU, 0x30f3U, 0x3099U, 0x309aU
};

static const struct pdf_page_text *find_page_text(struct pv_app *app, size_t index);
static struct pv_find_key *find_page_key(struct pv_app *app, size_t index);
static int find_key_make(const struct pdf_page_text *text, struct pv_find_key **made);
static int find_key_grow(struct pv_find_key *key);
static int find_key_add(struct pv_find_key *key, uint32_t character, size_t origin, int optional);
static size_t find_query_keys(const char *utf8, uint32_t *keys, size_t capacity);
static size_t find_fold(uint32_t character, uint32_t *out);
static uint32_t find_case(uint32_t character);
static uint32_t find_compose(uint32_t kana, uint32_t mark);
static int find_match(const struct pv_find_key *key, size_t at, const uint32_t *query, size_t length, size_t *last);
static size_t find_page_count(const struct pv_find_key *key, const uint32_t *query, size_t length);
static void find_announce(struct pv_app *app);
static int find_unreadable(struct pv_app *app);
static void find_reveal(struct pv_app *app);
static int find_character_at(const struct pdf_page_text *text, double x, double y, int nearest, size_t *index);
static void find_mark(struct pv_canvas *canvas, const struct pdf_page_text *text, size_t from, size_t count, int x, int y, double scale, uint32_t color);
static size_t find_utf8(uint32_t character, char *out);
static void find_bounds(const struct pv_app *app, size_t *first_page, size_t *first, size_t *last_page, size_t *last);
static int find_page_range(struct pv_app *app, size_t index, size_t *from, size_t *to);
static int find_point(struct pv_app *app, int x, int y, int nearest, size_t *page, size_t *index);
static int find_class(uint32_t character);
static void find_word(const struct pdf_page_text *text, size_t index, size_t *from, size_t *to);
static void find_line(const struct pdf_page_text *text, size_t index, size_t *from, size_t *to);
static void find_select(struct pv_app *app, size_t page, size_t from, size_t to, const char *how);
static int find_handle_place(struct pv_app *app, int which, double *x, double *y);
static void find_order(struct pv_app *app);
static int find_copy_page(struct pv_app *app, size_t index, char **out, size_t *length, size_t *capacity, size_t *unreadable);

/*
 * Takes the find field's words as they are typed: the first place they are
 * found from the start of the page in view is shown (none: the marks go),
 * and the places are counted again as the ticks read the pages.
 */
void
pv_find_text(
	struct pv_app *app,
	const char *query)
{
	/* The words, their keys, and no place found yet. */
	(void)snprintf(app->find_query, sizeof(app->find_query), "%s", query);
	app->find_key_length = find_query_keys(app->find_query, app->find_keys, sizeof(app->find_keys) / sizeof(app->find_keys[0]));
	app->find_found = 0;
	app->dirty = 1;

	/*
	 * A new count: the generation says which words the pages' counts are
	 * of, so that each page counts again once for these.
	 */
	app->find_generation++;
	app->find_counted = 0;
	app->find_count_next = 0;
	app->find_total = 0;

	/* Nothing to look for. */
	if (app->find_key_length == 0 || !app->has_document)
		return;

	/* From the start of the page in view. */
	app->find_page = pv_app_current_page(app);
	app->find_from = 0;
	app->find_key_at = 0;
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
	const struct pv_find_key *key;
	size_t length;
	size_t count;
	size_t page;
	size_t at;
	size_t last;
	size_t tried;
	int found;
	int first;
	int unreadable;

	/* Words to look for, in a document. */
	length = app->find_key_length;
	if (length == 0 || !app->has_document)
		return;

	/* Where to look from: the place found (past it), or the page in view. */
	count = app->document.count;
	page = app->find_page;
	at = app->find_key_at;
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
	last = 0;
	for (tried = 0; tried <= count; tried++) {
		key = find_page_key(app, page);
		if (key != NULL && key->count > 0U) {
			if (direction >= 0) {
				/* Forward from the place (the first page) or the start. */
				if (!first)
					at = 0;
				for (; at < key->count; at++) {
					found = find_match(key, at, app->find_keys, length, &last);
					if (found)
						break;
				}
			} else {
				/* Back from before the place (the first page) or the end. */
				if (!first || !app->find_found)
					at = key->count;
				while (at > 0) {
					at--;
					found = find_match(key, at, app->find_keys, length, &last);
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

	/* Not found: the marks go (logged), saying so when some characters could not be read. */
	if (!found) {
		app->find_found = 0;
		pv_log("FIND none query=\"%s\"", app->find_query);
		unreadable = find_unreadable(app);
		if (unreadable) {
			pv_app_message(app, "Not found (some text could not be read)", FIND_MESSAGE_MS);
		} else {
			pv_app_message(app, "Not found", FIND_MESSAGE_MS);
		}

		/* The marks go from the frame. */
		app->dirty = 1;
		return;
	}

	/* Found: the place kept (its keys and its characters), shown, logged and told. */
	key = find_page_key(app, page);
	app->find_found = 1;
	app->find_page = page;
	app->find_key_at = at;
	app->find_from = key->origins[at];
	app->find_length = key->lasts[last] - key->origins[at] + 1U;
	pv_log("FIND found query=\"%s\" page=%lu from=%lu length=%lu", app->find_query, (unsigned long)page, (unsigned long)app->find_from,
	       (unsigned long)app->find_length);
	find_reveal(app);
	find_announce(app);
}

/*
 * Moves the count of the places found on (ws177-p041): reads and counts
 * the pages not counted yet for the words, in the pages' order, for at
 * most FIND_COUNT_BUDGET_MS of the clock (at least one page), and logs the
 * total once every page is counted.  Returns how many milliseconds until
 * it wants the next tick (-1: nothing left to count).
 */
int
pv_find_tick(
	struct pv_app *app)
{
	struct pv_find_key *key;
	struct pv_page *page;
	uint64_t start;
	uint64_t spent;
	size_t index;

	/* Words to count, a document, and pages left. */
	if (app->find_key_length == 0 || !app->has_document || app->find_counted)
		return -1;

	/* The pages in turn while the time lasts. */
	start = pv_clock();
	while (app->find_count_next < app->document.count) {
		index = app->find_count_next;
		page = &app->document.pages[index];

		/* Counted once for these words. */
		if (page->find_generation != app->find_generation) {
			key = find_page_key(app, index);
			page->find_count = 0;
			if (key != NULL)
				page->find_count = find_page_count(key, app->find_keys, app->find_key_length);
			page->find_generation = app->find_generation;
		}

		/* Added to the total. */
		app->find_total += page->find_count;
		app->find_count_next++;

		/* Out of time for this tick. */
		spent = pv_clock() - start;
		if (spent >= FIND_COUNT_BUDGET_MS)
			break;
	}

	/* Some pages left: the next tick soon. */
	if (app->find_count_next < app->document.count)
		return 1;

	/* Every page counted (logged); a place shown is told with its number. */
	app->find_counted = 1;
	pv_log("FIND count query=\"%s\" total=%lu", app->find_query, (unsigned long)app->find_total);
	if (app->find_found)
		find_announce(app);
	return -1;
}

/*
 * Writes what tells the place shown: its number among all the places once
 * every page is counted ("Match 3 of 17"), its page before then, nothing
 * when none is shown.
 */
void
pv_find_status(
	struct pv_app *app,
	char *out,
	size_t size)
{
	const struct pv_find_key *key;
	size_t before;
	size_t index;
	size_t at;
	size_t last;
	int found;

	/* Nothing found. */
	out[0] = '\0';
	if (!app->find_found || !app->has_document)
		return;

	/* Its page until every page is counted. */
	if (!app->find_counted) {
		(void)snprintf(out, size, "Match on page %lu", (unsigned long)(app->find_page + 1U));
		return;
	}

	/* The places on the pages before it, then those before it on its page. */
	before = 0;
	for (index = 0; index < app->find_page; index++)
		before += app->document.pages[index].find_count;
	key = app->document.pages[app->find_page].find_key;
	for (at = 0; key != NULL && at < app->find_key_at; at++) {
		found = find_match(key, at, app->find_keys, app->find_key_length, &last);
		if (found)
			before++;
	}

	/* Its number among all. */
	(void)snprintf(out, size, "Match %lu of %lu", (unsigned long)(before + 1U), (unsigned long)app->find_total);
}

/*
 * Frees a page's keys.
 */
void
pv_find_key_free(
	struct pv_find_key *key)
{
	/* Nothing to free. */
	if (key == NULL)
		return;

	/* The arrays, then the record. */
	free(key->keys);
	free(key->origins);
	free(key->lasts);
	free(key->optional);
	free(key);
}

/*
 * Takes the pointer's button for the selection (event in the pages' view):
 * a press on a character starts a selection there, or with the second
 * click selects its word and with the third its line; the release ends it
 * (a press without a drag lets it go).  Returns 1 when the selection took
 * it.
 */
int
pv_select_button(
	struct pv_app *app,
	const struct pv_event *event)
{
	const struct pdf_page_text *text;
	size_t first_page;
	size_t last_page;
	size_t page;
	size_t index;
	size_t low;
	size_t high;
	size_t from;
	size_t to;
	int distance_x;
	int distance_y;
	int inside;
	int hit;

	/* The left button, over a document. */
	if (event->button != PV_BUTTON_LEFT || !app->has_document)
		return 0;

	/* A release of a press in the selection that did not drag: a click, which lets the selection go (ws189-p003). */
	if (!event->pressed && app->text_drag_armed) {
		app->text_drag_armed = 0;
		app->has_selection = 0;
		app->select_handles = 0;
		app->dirty = 1;
		return 1;
	}

	/* A release ends a selection under way. */
	if (!event->pressed) {
		if (!app->selecting)
			return 0;
		app->selecting = 0;

		/* A press that did not move selects nothing. */
		if (app->select_page == app->select_caret_page && app->select_anchor == app->select_caret && !app->select_moved) {
			app->has_selection = 0;
			app->dirty = 1;
			return 1;
		}

		/* Selected (logged). */
		pv_log("SELECT page=%lu from=%lu to=%lu to-page=%lu", (unsigned long)app->select_page, (unsigned long)app->select_anchor,
		       (unsigned long)app->select_caret, (unsigned long)app->select_caret_page);
		return 1;
	}

	/* The clicks in a row: a press soon after the last, near it, is its next click. */
	distance_x = abs(event->x - app->click_x);
	distance_y = abs(event->y - app->click_y);
	if (app->click_count > 0 &&
	    event->time >= app->click_time &&
	    event->time - app->click_time <= FIND_CLICK_MS &&
	    distance_x <= FIND_CLICK_DISTANCE &&
	    distance_y <= FIND_CLICK_DISTANCE) {
		app->click_count++;
	} else {
		app->click_count = 1;
	}

	/* This press is the one the next is compared with. */
	app->click_time = event->time;
	app->click_x = event->x;
	app->click_y = event->y;

	/* A press on a character of the page under it. */
	hit = find_point(app, event->x, event->y, 0, &page, &index);
	if (!hit) {
		/* Off the words: the selection goes, the press drags the view. */
		if (app->has_selection) {
			app->has_selection = 0;
			app->select_handles = 0;
			app->dirty = 1;
		}

		/* Not taken. */
		return 0;
	}

	/* The second click selects the word, the third the line. */
	text = find_page_text(app, page);
	if (app->click_count == 2) {
		find_word(text, index, &from, &to);
		find_select(app, page, from, to, "word");
		return 1;
	}

	/* The third (or more) selects the line. */
	if (app->click_count >= 3) {
		find_line(text, index, &from, &to);
		find_select(app, page, from, to, "line");
		return 1;
	}

	/* A press in the selection may drag its words out of the window, once it moves (ws189-p003). */
	if (app->has_selection) {
		find_bounds(app, &first_page, &low, &last_page, &high);
		inside = 0;
		if (page > first_page && page < last_page)
			inside = 1;
		if (page == first_page &&
		    page == last_page &&
		    index >= low &&
		    index <= high &&
		    low != high)
			inside = 1;
		if (page == first_page &&
		    page != last_page &&
		    index >= low)
			inside = 1;
		if (page == last_page &&
		    page != first_page &&
		    index <= high)
			inside = 1;

		/* The press is within it (a selection of one place is none). */
		if (inside) {
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
	app->select_handles = 0;
	app->select_page = page;
	app->select_anchor = index;
	app->select_caret_page = page;
	app->select_caret = index;
	app->dirty = 1;
	return 1;
}

/*
 * Follows the pointer while a selection is under way: up to the character
 * nearest it on the page it is over (or the nearest page).  Returns 1 when
 * the selection took the motion.
 */
int
pv_select_motion(
	struct pv_app *app,
	const struct pv_event *event)
{
	size_t page;
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

	/* The nearest character on the page under the pointer. */
	hit = find_point(app, event->x, event->y, 1, &page, &index);
	if (!hit)
		return 1;

	/* The selection's end moves there. */
	if (page != app->select_caret_page || index != app->select_caret) {
		app->select_caret_page = page;
		app->select_caret = index;
		app->select_moved = 1;
		app->dirty = 1;
	}

	/* Taken. */
	return 1;
}

/*
 * Selects the whole document (Ctrl+A, Edit > Select All): from the first
 * page's first character to the last page's last, read as they are needed.
 */
void
pv_select_all(
	struct pv_app *app)
{
	/* A document. */
	if (!app->has_document || app->document.count == 0)
		return;

	/*
	 * From the first character to the last of the last page; (size_t)-1
	 * stands for "its last", whatever the page holds.
	 */
	app->selecting = 0;
	app->has_selection = 1;
	app->select_handles = 0;
	app->select_page = 0;
	app->select_anchor = 0;
	app->select_caret_page = app->document.count - 1U;
	app->select_caret = (size_t)-1;
	app->dirty = 1;
	pv_log("SELECT all pages=%lu", (unsigned long)app->document.count);
}

/*
 * Selects the word under a point of the pages' view (a finger's long
 * press, ws177-p042) and shows the handles.  Returns 1 when a word was
 * selected.
 */
int
pv_select_word_at(
	struct pv_app *app,
	int x,
	int y)
{
	const struct pdf_page_text *text;
	size_t page;
	size_t index;
	size_t from;
	size_t to;
	int hit;

	/* A character under the finger. */
	hit = find_point(app, x, y, 0, &page, &index);
	if (!hit)
		return 0;

	/* Its word, with the handles. */
	text = find_page_text(app, page);
	find_word(text, index, &from, &to);
	find_select(app, page, from, to, "word");
	app->select_handles = 1;

	/* Selected. */
	return 1;
}

/*
 * Tells which handle of the selection a point of the pages' view is on: 1
 * the start's, 2 the end's, 0 neither (or no handles shown).
 */
int
pv_select_handle_at(
	struct pv_app *app,
	int x,
	int y)
{
	double handle_x;
	double handle_y;
	double distance;
	int known;
	int which;

	/* Handles are shown. */
	if (!app->has_selection || !app->select_handles)
		return 0;

	/* Each handle in turn: the point is within reach of it. */
	for (which = 1; which <= 2; which++) {
		known = find_handle_place(app, which, &handle_x, &handle_y);
		if (!known)
			continue;
		distance = hypot((double)x - handle_x, (double)y - handle_y);
		if (distance <= FIND_HANDLE_REACH)
			return which;
	}

	/* Neither. */
	return 0;
}

/*
 * Moves a handle of the selection (1 the start's, 2 the end's) to the
 * character nearest a point of the pages' view, on the page it is over;
 * the ends keep their order (the handle that passes the other becomes it).
 */
void
pv_select_handle_move(
	struct pv_app *app,
	int which,
	int x,
	int y)
{
	size_t page;
	size_t index;
	int hit;

	/* The nearest character, for a selection. */
	if (!app->has_selection)
		return;
	hit = find_point(app, x, y, 1, &page, &index);
	if (!hit)
		return;

	/* The end the handle holds. */
	if (which == 1) {
		app->select_page = page;
		app->select_anchor = index;
	} else {
		app->select_caret_page = page;
		app->select_caret = index;
	}

	/* The start before the end again, drawn anew (logged). */
	find_order(app);
	app->dirty = 1;
	pv_log("SELECT handle=%d page=%lu from=%lu to-page=%lu to=%lu", which, (unsigned long)app->select_page,
	       (unsigned long)app->select_anchor, (unsigned long)app->select_caret_page, (unsigned long)app->select_caret);
}

/*
 * Lets the selection go (a tap of a finger off it).
 */
void
pv_select_clear(
	struct pv_app *app)
{
	/* Nothing selected, no handles. */
	if (!app->has_selection)
		return;
	app->has_selection = 0;
	app->selecting = 0;
	app->select_handles = 0;
	app->dirty = 1;
}

/*
 * Copies the selection (Ctrl+C, Edit > Copy) as UTF-8, a newline at each
 * line's and each page's end within it, for main.c to put on the
 * clipboard, at most FIND_COPY_MAX bytes of whole pages (logged, and told
 * when it was cut short or some characters could not be read).
 */
void
pv_select_copy(
	struct pv_app *app)
{
	char message[128];
	char *out;
	size_t first_page;
	size_t last_page;
	size_t first;
	size_t last;
	size_t index;
	size_t length;
	size_t capacity;
	size_t unreadable;
	size_t copied_pages;
	int error;

	/* A selection. */
	if (!app->has_selection || !app->has_document)
		return;
	find_bounds(app, &first_page, &first, &last_page, &last);

	/* Each page of it in turn, until the most a copy takes. */
	out = NULL;
	length = 0;
	capacity = 0;
	unreadable = 0;
	copied_pages = 0;
	for (index = first_page; index <= last_page; index++) {
		error = find_copy_page(app, index, &out, &length, &capacity, &unreadable);
		if (error != 0)
			break;
		copied_pages++;
	}

	/* Nothing to copy. */
	if (out == NULL)
		return;

	/* For main.c (an earlier copy not yet taken goes; logged). */
	free(app->copy_text);
	app->copy_text = out;
	app->copy_length = length;
	pv_log("COPY bytes=%lu text=\"%.40s\" unreadable=%lu", (unsigned long)length, out, (unsigned long)unreadable);

	/* Told when it was cut short. */
	if (copied_pages < last_page - first_page + 1U) {
		(void)snprintf(message, sizeof(message), "Copied the first %lu pages", (unsigned long)copied_pages);
		pv_app_message(app, message, FIND_MESSAGE_MS);
		return;
	}

	/* Told when some characters could not be read (they are copied as U+FFFD). */
	if (unreadable > 0U) {
		(void)snprintf(message, sizeof(message), "%lu characters could not be read", (unsigned long)unreadable);
		pv_app_message(app, message, FIND_MESSAGE_MS);
	}
}

/*
 * Opens the find field inside the window (ws177-p043: Ctrl+F or Edit >
 * Find without the titlebar's field) and asks main.c to give it the
 * keyboard.
 */
void
pv_find_bar_open(
	struct pv_app *app)
{
	/* A document to look in. */
	if (!app->has_document)
		return;

	/* Open, with the keyboard asked for (logged). */
	app->bar_open = 1;
	app->want_bar_focus = 1;
	app->dirty = 1;
	pv_log("FIND bar open");
}

/*
 * Closes the find field inside the window (Esc in it): the marks of Find
 * go with it.
 */
void
pv_find_bar_close(
	struct pv_app *app)
{
	/* Nothing open. */
	if (!app->bar_open)
		return;

	/* Closed, the places found no longer marked (logged). */
	app->bar_open = 0;
	app->want_bar_focus = 0;
	app->find_found = 0;
	app->dirty = 1;
	pv_log("FIND bar close");
}

/*
 * Gives the place of the find field inside the window: its panel at the
 * top right of the pages' part (window coordinates), and within it the
 * field and, at its right, the room for the place's number.
 */
void
pv_find_bar_place(
	const struct pv_app *app,
	struct pv_bar_place *place)
{
	int width;

	/* The panel: at most PV_BAR_WIDTH, within the window less a margin either side. */
	width = PV_BAR_WIDTH;
	if (width > app->window_width - 2 * PV_MARGIN)
		width = app->window_width - 2 * PV_MARGIN;
	if (width < 0)
		width = 0;
	place->x = app->window_width - PV_MARGIN - width;
	place->y = PV_MARGIN;
	place->width = width;
	place->height = PV_BAR_HEIGHT;

	/* The field, the panel less its padding and the number's room. */
	place->field_x = place->x + PV_BAR_PADDING;
	place->field_y = place->y + PV_BAR_PADDING;
	place->field_width = width - 3 * PV_BAR_PADDING - PV_BAR_STATUS;
	if (place->field_width < 0)
		place->field_width = 0;
	place->field_height = PV_BAR_HEIGHT - 2 * PV_BAR_PADDING;

	/* The number, right of the field. */
	place->status_x = place->field_x + place->field_width + PV_BAR_PADDING;
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
	app->select_handles = 0;
	app->find_found = 0;
	app->find_page = (size_t)-1;
	app->dirty = 1;
}

/*
 * Marks a page drawn with its top left at a place and at a scale: every
 * place the words are found on it, the one shown, the selection, and the
 * handles of a finger's selection.
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
	const struct pv_find_key *key;
	double handle_x;
	double handle_y;
	size_t at;
	size_t last;
	size_t from;
	size_t to;
	size_t origin;
	int selected;
	int found;
	int known;
	int which;

	/* The places found on the page, the one shown brighter. */
	if (app->find_found && app->find_key_length > 0) {
		key = find_page_key(app, index);
		text = find_page_text(app, index);
		for (at = 0; key != NULL && text != NULL && at < key->count; at++) {
			found = find_match(key, at, app->find_keys, app->find_key_length, &last);
			if (!found)
				continue;
			origin = key->origins[at];
			if (index == app->find_page && at == app->find_key_at) {
				find_mark(canvas, text, origin, key->lasts[last] - origin + 1U, x, y, scale, FIND_CURRENT);
			} else {
				find_mark(canvas, text, origin, key->lasts[last] - origin + 1U, x, y, scale, FIND_MATCH);
			}
		}
	}

	/* The selection's part of the page. */
	selected = 0;
	if (app->has_selection)
		selected = find_page_range(app, index, &from, &to);
	if (selected) {
		text = find_page_text(app, index);
		find_mark(canvas, text, from, to - from + 1U, x, y, scale, FIND_SELECTION);
	}

	/* The handles of a finger's selection on this page (in the canvas, past the sidebar). */
	if (!app->has_selection || !app->select_handles)
		return;
	for (which = 1; which <= 2; which++) {
		/* The handle of an end on this page. */
		if (which == 1 && app->select_page != index)
			continue;
		if (which == 2 && app->select_caret_page != index)
			continue;
		known = find_handle_place(app, which, &handle_x, &handle_y);
		if (!known)
			continue;

		/* A round dot in the accent. */
		handle_x += (double)pv_app_sidebar_width(app);
		pv_canvas_round(canvas, (int)floor(handle_x) - FIND_HANDLE_RADIUS, (int)floor(handle_y) - FIND_HANDLE_RADIUS, 2 * FIND_HANDLE_RADIUS,
				2 * FIND_HANDLE_RADIUS, FIND_HANDLE_RADIUS, pv_draw_accent() | 0xff000000U);
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

/* Gives a page's keys, made once from its words (NULL: no words, or no memory). */
static struct pv_find_key *
find_page_key(
	struct pv_app *app,
	size_t index)
{
	const struct pdf_page_text *text;
	struct pv_page *page;
	int error;

	/* The page's words. */
	text = find_page_text(app, index);
	if (text == NULL)
		return NULL;
	page = &app->document.pages[index];

	/* Made the first time, kept after. */
	if (page->find_key == NULL) {
		error = find_key_make(text, &page->find_key);
		if (error != 0) {
			page->find_key = NULL;
			pv_log("FIND key-failed page=%lu error=%d", (unsigned long)index, error);
		}
	}

	/* Kept. */
	return page->find_key;
}

/*
 * Makes the keys of a page's words: each character folded, a space after
 * each line's end that may be passed over, and a hyphen ending a line
 * that may be too.  Returns 0 or ENOMEM.
 */
static int
find_key_make(
	const struct pdf_page_text *text,
	struct pv_find_key **made)
{
	struct pv_find_key *key;
	uint32_t folded[FIND_FOLD_MAX];
	uint32_t character;
	size_t count;
	size_t at;
	size_t in;
	size_t before;
	int error;

	/* The record. */
	key = calloc(1, sizeof(*key));
	if (key == NULL)
		return ENOMEM;

	/* Each character of the page. */
	for (at = 0; at < text->count; at++) {
		/* Its keys. */
		character = text->characters[at].character;
		count = find_fold(character, folded);
		before = key->count;
		for (in = 0; in < count; in++) {
			error = find_key_add(key, folded[in], at, 0);
			if (error != 0) {
				pv_find_key_free(key);
				return error;
			}
		}

		/* Not a line's end. */
		if ((text->characters[at].flags & PDF_TEXT_LINE_END) == 0U)
			continue;

		/* A hyphen that ends the line may be passed over (a word broken across the lines). */
		if (key->count == before + 1U && key->keys[before] == '-')
			key->optional[before] = 1;

		/* A space after the line's end, which may be passed over (none after a space). */
		if (key->count > 0 && key->keys[key->count - 1U] == ' ')
			continue;
		error = find_key_add(key, ' ', at, 1);
		if (error != 0) {
			pv_find_key_free(key);
			return error;
		}
	}

	/* Succeeded: the keys. */
	*made = key;
	return 0;
}

/* Makes room for one more key; returns 0 or ENOMEM. */
static int
find_key_grow(
	struct pv_find_key *key)
{
	uint32_t *keys;
	size_t *origins;
	size_t *lasts;
	unsigned char *optional;
	size_t larger;

	/* Room enough already. */
	if (key->count < key->capacity)
		return 0;

	/* Twice the room, each array in turn (one grown and the next failing leaves both usable at the old count). */
	larger = key->capacity * 2U + 64U;
	keys = realloc(key->keys, larger * sizeof(*keys));
	if (keys == NULL)
		return ENOMEM;
	key->keys = keys;

	/* The first characters each key stands for. */
	origins = realloc(key->origins, larger * sizeof(*origins));
	if (origins == NULL)
		return ENOMEM;
	key->origins = origins;

	/* The last characters. */
	lasts = realloc(key->lasts, larger * sizeof(*lasts));
	if (lasts == NULL)
		return ENOMEM;
	key->lasts = lasts;

	/* Whether each may be passed over. */
	optional = realloc(key->optional, larger * sizeof(*optional));
	if (optional == NULL)
		return ENOMEM;
	key->optional = optional;

	/* Succeeded: the larger room. */
	key->capacity = larger;
	return 0;
}

/*
 * Adds one folded character of the page's character origin to the keys:
 * a space after a space joins it (one that may not be passed over makes
 * the joined one so too), and a voiced mark after a kana it composes with
 * becomes part of that kana.  Returns 0 or ENOMEM.
 */
static int
find_key_add(
	struct pv_find_key *key,
	uint32_t character,
	size_t origin,
	int optional)
{
	uint32_t composed;
	size_t last;
	int error;

	/* A space after a space is one with it. */
	if (character == ' ' && key->count > 0 && key->keys[key->count - 1U] == ' ') {
		last = key->count - 1U;
		key->lasts[last] = origin;
		if (!optional)
			key->optional[last] = 0;
		return 0;
	}

	/* A voiced mark composes with the kana before it. */
	if (key->count > 0 && (character == 0x3099U || character == 0x309aU)) {
		last = key->count - 1U;
		composed = find_compose(key->keys[last], character);
		if (composed != 0U) {
			key->keys[last] = composed;
			key->lasts[last] = origin;
			return 0;
		}
	}

	/* Room for one more. */
	error = find_key_grow(key);
	if (error != 0)
		return error;

	/* The key. */
	key->keys[key->count] = character;
	key->origins[key->count] = origin;
	key->lasts[key->count] = origin;
	key->optional[key->count] = (unsigned char)optional;
	key->count++;

	/* Succeeded: added. */
	return 0;
}

/* Reads the find field's UTF-8 words as keys (folded, spaces joined, voiced marks composed); returns how many. */
static size_t
find_query_keys(
	const char *utf8,
	uint32_t *keys,
	size_t capacity)
{
	const unsigned char *bytes;
	uint32_t folded[FIND_FOLD_MAX];
	uint32_t character;
	uint32_t composed;
	size_t count;
	size_t folds;
	size_t at;
	size_t in;
	unsigned more;

	/* Each character, its lead byte then its continuations. */
	bytes = (const unsigned char *)utf8;
	count = 0;
	at = 0;
	while (bytes[at] != '\0') {
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

		/* Past the lead byte, then its continuations. */
		at++;
		while (more > 0 && (bytes[at] & 0xc0U) == 0x80U) {
			character = (character << 6) | (bytes[at] & 0x3fU);
			at++;
			more--;
		}

		/* Its keys, joined and composed as the page's are. */
		folds = find_fold(character, folded);
		for (in = 0; in < folds; in++) {
			/* A space after a space. */
			if (folded[in] == ' ' && count > 0 && keys[count - 1U] == ' ')
				continue;

			/* A voiced mark after its kana. */
			composed = 0;
			if (count > 0 && (folded[in] == 0x3099U || folded[in] == 0x309aU))
				composed = find_compose(keys[count - 1U], folded[in]);
			if (composed != 0U) {
				keys[count - 1U] = composed;
				continue;
			}

			/* The key, within the room. */
			if (count >= capacity)
				return count;
			keys[count] = folded[in];
			count++;
		}
	}

	/* Reports them. */
	return count;
}

/*
 * Folds a character into the keys Find compares (0 to FIND_FOLD_MAX):
 * spaces become one kind of space, a soft hyphen nothing, a full width
 * letter its ASCII, a half width kana its full width, a spacing voiced
 * mark the combining one, a ligature its letters, a sharp s "ss", hyphens
 * '-', and a letter its small form.
 */
static size_t
find_fold(
	uint32_t character,
	uint32_t *out)
{
	/* Every space. */
	if (character == 0x20U || character == 0x09U || character == 0xa0U || character == 0x202fU || character == 0x205fU ||
	    character == 0x3000U || (character >= 0x2000U && character <= 0x200aU)) {
		out[0] = ' ';
		return 1;
	}

	/* A soft hyphen, which shows only where a line breaks. */
	if (character == 0xadU)
		return 0;

	/* The hyphens. */
	if (character == 0x2010U || character == 0x2011U) {
		out[0] = '-';
		return 1;
	}

	/* A full width ASCII character, in ASCII. */
	if (character >= 0xff01U && character <= 0xff5eU)
		character -= 0xfee0U;

	/* A half width kana, in full width. */
	if (character >= 0xff61U && character <= 0xff9fU)
		character = find_half_width[character - 0xff61U];

	/* The spacing voiced marks, as the combining ones. */
	if (character == 0x309bU)
		character = 0x3099U;
	if (character == 0x309cU)
		character = 0x309aU;

	/* The Latin ligatures and the sharp s, spelled out. */
	switch (character) {
	case 0xdfU:
		out[0] = 's';
		out[1] = 's';
		return 2;
	case 0xfb00U:
		out[0] = 'f';
		out[1] = 'f';
		return 2;
	case 0xfb01U:
		out[0] = 'f';
		out[1] = 'i';
		return 2;
	case 0xfb02U:
		out[0] = 'f';
		out[1] = 'l';
		return 2;
	case 0xfb03U:
		out[0] = 'f';
		out[1] = 'f';
		out[2] = 'i';
		return 3;
	case 0xfb04U:
		out[0] = 'f';
		out[1] = 'f';
		out[2] = 'l';
		return 3;
	case 0xfb05U:
	case 0xfb06U:
		out[0] = 's';
		out[1] = 't';
		return 2;
	default:
		break;
	}

	/* Any other, in its small form. */
	out[0] = find_case(character);
	return 1;
}

/* Gives a letter's small form: ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic (simple case folding); any other as it is. */
static uint32_t
find_case(
	uint32_t character)
{
	/* ASCII. */
	if (character >= 'A' && character <= 'Z')
		return character + 0x20U;

	/* Latin-1's capitals (not the multiplication sign), and the micro sign. */
	if (character >= 0xc0U && character <= 0xdeU && character != 0xd7U)
		return character + 0x20U;
	if (character == 0xb5U)
		return 0x3bcU;

	/* Latin Extended-A: capital I with a dot, the long s, Y with diaeresis. */
	if (character == 0x130U)
		return 'i';
	if (character == 0x17fU)
		return 's';
	if (character == 0x178U)
		return 0xffU;

	/* Latin Extended-A's pairs whose capitals are even. */
	if ((character >= 0x100U && character <= 0x12fU) ||
	    (character >= 0x132U && character <= 0x137U) ||
	    (character >= 0x14aU && character <= 0x177U)) {
		if ((character & 1U) == 0U)
			return character + 1U;
		return character;
	}

	/* Latin Extended-A's pairs whose capitals are odd. */
	if ((character >= 0x139U && character <= 0x148U) ||
	    (character >= 0x179U && character <= 0x17eU)) {
		if ((character & 1U) != 0U)
			return character + 1U;
		return character;
	}

	/* Greek: the capitals with a tonos, the capitals, and the final sigma. */
	if (character == 0x386U)
		return 0x3acU;
	if (character >= 0x388U && character <= 0x38aU)
		return character + 0x25U;
	if (character == 0x38cU)
		return 0x3ccU;
	if (character == 0x38eU || character == 0x38fU)
		return character + 0x3fU;
	if (character >= 0x391U && character <= 0x3abU && character != 0x3a2U)
		return character + 0x20U;
	if (character == 0x3c2U)
		return 0x3c3U;

	/* Cyrillic's capitals. */
	if (character >= 0x400U && character <= 0x40fU)
		return character + 0x50U;
	if (character >= 0x410U && character <= 0x42fU)
		return character + 0x20U;

	/* Any other as it is. */
	return character;
}

/* Gives a kana composed with a voiced (U+3099) or semi-voiced (U+309A) mark, or 0 when they do not compose. */
static uint32_t
find_compose(
	uint32_t kana,
	uint32_t mark)
{
	uint32_t base;

	/* The katakana compose as the hiragana do, 0x60 apart (and four of their own). */
	base = kana;
	if (kana >= 0x30a1U && kana <= 0x30f6U)
		base = kana - 0x60U;

	/* The semi-voiced mark: the h row only. */
	if (mark == 0x309aU) {
		if (base == 0x306fU || base == 0x3072U || base == 0x3075U || base == 0x3078U || base == 0x307bU)
			return kana + 2U;
		return 0;
	}

	/* The voiced mark: the k and s rows and chi (odd), the t row's tsu, te and to, the h row. */
	if (base >= 0x304bU && base <= 0x3061U && (base & 1U) == 1U)
		return kana + 1U;
	if (base == 0x3064U || base == 0x3066U || base == 0x3068U)
		return kana + 1U;
	if (base == 0x306fU || base == 0x3072U || base == 0x3075U || base == 0x3078U || base == 0x307bU)
		return kana + 1U;

	/* u, the katakana wa to wo, and the iteration marks. */
	if (kana == 0x3046U)
		return 0x3094U;
	if (kana == 0x30a6U)
		return 0x30f4U;
	if (kana >= 0x30efU && kana <= 0x30f2U)
		return kana + 8U;
	if (kana == 0x309dU || kana == 0x30fdU)
		return kana + 1U;

	/* No composition. */
	return 0;
}

/*
 * Tells whether the words' keys are found at a key of a page: each of
 * them the same as the page's next, a page's key that may be passed over
 * and is not the word's next passed over (the first must match).  last
 * gets the page's last key used.
 */
static int
find_match(
	const struct pv_find_key *key,
	size_t at,
	const uint32_t *query,
	size_t length,
	size_t *last)
{
	size_t in;
	size_t on;

	/* The first key must be the words' first. */
	if (length == 0 || at >= key->count)
		return 0;
	if (key->keys[at] != query[0])
		return 0;

	/* Each key of the words in turn, passing over what may be. */
	in = 1;
	on = at + 1U;
	while (in < length) {
		/* The page ends first. */
		if (on >= key->count)
			return 0;

		/* The same: both go on. */
		if (key->keys[on] == query[in]) {
			in++;
			on++;
			continue;
		}

		/* Another, which may be passed over or not. */
		if (!key->optional[on])
			return 0;
		on++;
	}

	/* Found: the page's last key used. */
	*last = on - 1U;
	return 1;
}

/* Counts the places a page's keys have the words. */
static size_t
find_page_count(
	const struct pv_find_key *key,
	const uint32_t *query,
	size_t length)
{
	size_t count;
	size_t at;
	size_t last;
	int found;

	/* Each key a place may start at. */
	count = 0;
	for (at = 0; at < key->count; at++) {
		found = find_match(key, at, query, length, &last);
		if (found)
			count++;
	}

	/* The places. */
	return count;
}

/* Tells the place shown in the message (its number once the pages are counted). */
static void
find_announce(
	struct pv_app *app)
{
	char status[96];

	/* Its words, kept a moment. */
	pv_find_status(app, status, sizeof(status));
	if (status[0] != '\0')
		pv_app_message(app, status, FIND_MESSAGE_MS);
}

/* Tells whether a page read so far has characters that could not be read (U+FFFD). */
static int
find_unreadable(
	struct pv_app *app)
{
	const struct pdf_page_text *text;
	size_t index;
	size_t at;

	/* Each page read, each character. */
	for (index = 0; index < app->document.count; index++) {
		text = app->document.pages[index].text;
		if (text == NULL)
			continue;
		for (at = 0; at < text->count; at++) {
			if (text->characters[at].character == 0xfffdU)
				return 1;
		}
	}

	/* None. */
	return 0;
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

/*
 * Fills the corners of characters of a page drawn at a place and a scale
 * with a translucent colour, each as its four corners lie (a turned
 * character turned, ws177-p042).
 */
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
	double xs[4];
	double ys[4];
	size_t at;
	unsigned corner;

	/* Each character's corners in the canvas. */
	for (at = from; at < from + count && at < text->count; at++) {
		quad = text->characters[at].quad;
		for (corner = 0; corner < 4U; corner++) {
			xs[corner] = (double)x + quad[corner * 2U] * scale;
			ys[corner] = (double)y + quad[corner * 2U + 1U] * scale;
		}

		/* Blended over the page. */
		pv_canvas_blend_quad(canvas, xs, ys, color);
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

/* Gives the selection's two ends in the pages' order (its first and its last page, and the characters there). */
static void
find_bounds(
	const struct pv_app *app,
	size_t *first_page,
	size_t *first,
	size_t *last_page,
	size_t *last)
{
	int swap;

	/* The end on the later page, or later on the same page, is the last. */
	swap = 0;
	if (app->select_caret_page < app->select_page)
		swap = 1;
	if (app->select_caret_page == app->select_page && app->select_caret < app->select_anchor)
		swap = 1;
	if (swap) {
		*first_page = app->select_caret_page;
		*first = app->select_caret;
		*last_page = app->select_page;
		*last = app->select_anchor;
		return;
	}

	/* As they are. */
	*first_page = app->select_page;
	*first = app->select_anchor;
	*last_page = app->select_caret_page;
	*last = app->select_caret;
}

/*
 * Gives the characters of a page the selection holds (from, to within its
 * words).  Returns 1 when it holds some.
 */
static int
find_page_range(
	struct pv_app *app,
	size_t index,
	size_t *from,
	size_t *to)
{
	const struct pdf_page_text *text;
	size_t first_page;
	size_t last_page;
	size_t first;
	size_t last;

	/* The page lies within the selection. */
	find_bounds(app, &first_page, &first, &last_page, &last);
	if (index < first_page || index > last_page)
		return 0;

	/* Its words. */
	text = find_page_text(app, index);
	if (text == NULL || text->count == 0)
		return 0;

	/* From the first end on its page, else from its start. */
	*from = 0;
	if (index == first_page)
		*from = first;

	/* To the last end on its page, else to its end ((size_t)-1 is the end too). */
	*to = text->count - 1U;
	if (index == last_page && last < text->count)
		*to = last;

	/* A range within the words. */
	if (*from > *to || *from >= text->count)
		return 0;

	/* Some. */
	return 1;
}

/*
 * Finds the page under a point of the pages' view and its character there
 * (or the nearest, when asked).  Returns 1 when one is found.
 */
static int
find_point(
	struct pv_app *app,
	int x,
	int y,
	int nearest,
	size_t *page,
	size_t *index)
{
	const struct pdf_page_text *text;
	struct pv_place place;
	int hit;

	/* The page under the point, and the point on it. */
	pv_app_place_at(app, (double)x, (double)y, &place);
	text = find_page_text(app, place.page);
	if (text == NULL)
		return 0;

	/* The character there. */
	hit = find_character_at(text, place.x, place.y, nearest, index);
	if (!hit)
		return 0;

	/* Found. */
	*page = place.page;
	return 1;
}

/* Gives the kind of character a word is a run of (FIND_CLASS_*). */
static int
find_class(
	uint32_t character)
{
	/* ASCII's letters and digits. */
	if ((character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z'))
		return FIND_CLASS_LETTER;

	/* The letters of Latin past ASCII, Greek and Cyrillic, and the full width letters and digits. */
	if (character >= 0xc0U && character <= 0x24fU && character != 0xd7U && character != 0xf7U)
		return FIND_CLASS_LETTER;
	if (character >= 0x370U && character <= 0x4ffU)
		return FIND_CLASS_LETTER;
	if (character >= 0xff10U && character <= 0xff5aU)
		return FIND_CLASS_LETTER;

	/* The ideographs and their iteration mark. */
	if ((character >= 0x4e00U && character <= 0x9fffU) || (character >= 0x3400U && character <= 0x4dbfU) || character == 0x3005U)
		return FIND_CLASS_KANJI;

	/* The kana (the long vowel mark with the katakana). */
	if (character >= 0x3041U && character <= 0x309fU)
		return FIND_CLASS_HIRAGANA;
	if ((character >= 0x30a0U && character <= 0x30ffU) || (character >= 0xff66U && character <= 0xff9fU))
		return FIND_CLASS_KATAKANA;

	/* Anything else stands alone. */
	return FIND_CLASS_OTHER;
}

/* Gives the word around a character: the run of characters of its kind within its line (the character alone for another kind). */
static void
find_word(
	const struct pdf_page_text *text,
	size_t index,
	size_t *from,
	size_t *to)
{
	int kind;
	int other;

	/* The character alone, for a kind that makes no word. */
	*from = index;
	*to = index;
	kind = find_class(text->characters[index].character);
	if (kind == FIND_CLASS_OTHER)
		return;

	/* Back while the one before is of its kind on its line. */
	while (*from > 0) {
		if ((text->characters[*from - 1U].flags & PDF_TEXT_LINE_END) != 0U)
			break;
		other = find_class(text->characters[*from - 1U].character);
		if (other != kind)
			break;
		(*from)--;
	}

	/* On while the one after is of its kind on its line. */
	while (*to + 1U < text->count) {
		if ((text->characters[*to].flags & PDF_TEXT_LINE_END) != 0U)
			break;
		other = find_class(text->characters[*to + 1U].character);
		if (other != kind)
			break;
		(*to)++;
	}
}

/* Gives the line around a character: from the one after the last line's end to its own line's end. */
static void
find_line(
	const struct pdf_page_text *text,
	size_t index,
	size_t *from,
	size_t *to)
{
	/* Back to the start of the line. */
	*from = index;
	while (*from > 0 && (text->characters[*from - 1U].flags & PDF_TEXT_LINE_END) == 0U)
		(*from)--;

	/* On to its end. */
	*to = index;
	while (*to + 1U < text->count && (text->characters[*to].flags & PDF_TEXT_LINE_END) == 0U)
		(*to)++;
}

/* Selects characters of one page as a whole (a word, a line), not dragged on (logged). */
static void
find_select(
	struct pv_app *app,
	size_t page,
	size_t from,
	size_t to,
	const char *how)
{
	/* The range, done. */
	app->selecting = 0;
	app->has_selection = 1;
	app->select_handles = 0;
	app->select_page = page;
	app->select_anchor = from;
	app->select_caret_page = page;
	app->select_caret = to;
	app->dirty = 1;
	pv_log("SELECT %s page=%lu from=%lu to=%lu", how, (unsigned long)page, (unsigned long)from, (unsigned long)to);
}

/*
 * Gives where a handle of the selection is in the pages' view: the start's
 * at its first character's bottom left, the end's at its last's bottom
 * right.  Returns 1 when it is known.
 */
static int
find_handle_place(
	struct pv_app *app,
	int which,
	double *x,
	double *y)
{
	const struct pdf_page_text *text;
	const double *quad;
	size_t first_page;
	size_t last_page;
	size_t first;
	size_t last;
	size_t page;
	size_t index;
	double scale;
	double left;
	double top;

	/* The end the handle holds. */
	find_bounds(app, &first_page, &first, &last_page, &last);
	page = first_page;
	index = first;
	if (which == 2) {
		page = last_page;
		index = last;
	}

	/* Its character ((size_t)-1: the page's last). */
	text = find_page_text(app, page);
	if (text == NULL || text->count == 0)
		return 0;
	if (index >= text->count)
		index = text->count - 1U;
	quad = text->characters[index].quad;

	/* The corner, through the page's place and scale in the view. */
	scale = pv_app_scale(app, page);
	left = pv_app_page_left(app, page);
	top = pv_app_page_top(app, page) - app->scroll_y;
	if (which == 2) {
		*x = left + quad[4] * scale;
		*y = top + quad[5] * scale;
	} else {
		*x = left + quad[6] * scale;
		*y = top + quad[7] * scale;
	}

	/* Known. */
	return 1;
}

/* Puts the selection's start before its end again (after a handle moved past the other). */
static void
find_order(
	struct pv_app *app)
{
	size_t first_page;
	size_t last_page;
	size_t first;
	size_t last;

	/* The two ends in the pages' order. */
	find_bounds(app, &first_page, &first, &last_page, &last);
	app->select_page = first_page;
	app->select_anchor = first;
	app->select_caret_page = last_page;
	app->select_caret = last;
}

/*
 * Adds a page's part of the selection to the copy (a newline between
 * pages), counting the characters that could not be read.  Returns 0, or
 * ENOSPC when it would pass FIND_COPY_MAX (nothing of it added), or ENOMEM.
 */
static int
find_copy_page(
	struct pv_app *app,
	size_t index,
	char **out,
	size_t *length,
	size_t *capacity,
	size_t *unreadable)
{
	const struct pdf_page_text *text;
	char *grown;
	size_t needed;
	size_t from;
	size_t to;
	size_t at;
	int some;

	/* The page's part (none: nothing added). */
	some = find_page_range(app, index, &from, &to);
	if (!some)
		return 0;
	text = find_page_text(app, index);

	/* Room for four bytes a character, a newline after each, the page's newline and the NUL, within the most. */
	needed = *length + (to - from + 1U) * 5U + 2U;
	if (needed > FIND_COPY_MAX)
		return ENOSPC;
	if (needed > *capacity) {
		grown = realloc(*out, needed);
		if (grown == NULL)
			return ENOMEM;
		*out = grown;
		*capacity = needed;
	}

	/* A newline between this page's words and the page before. */
	if (*length > 0) {
		(*out)[*length] = '\n';
		(*length)++;
	}

	/* Each character (counting the unreadable), a newline after a line's end (not after the last). */
	for (at = from; at <= to; at++) {
		if (text->characters[at].character == 0xfffdU)
			(*unreadable)++;
		*length += find_utf8(text->characters[at].character, *out + *length);
		if ((text->characters[at].flags & PDF_TEXT_LINE_END) != 0U && at < to) {
			(*out)[*length] = '\n';
			(*length)++;
		}
	}

	/* Succeeded: ended. */
	(*out)[*length] = '\0';
	return 0;
}
