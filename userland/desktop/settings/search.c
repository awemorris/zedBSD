/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The search of Settings (ws089-p008): the titlebar's field finds pages by
 * their names and words, and the settings on the pages by theirs.
 *
 * While the query has a word in it, the page pane shows the results in
 * place of the page; the page stays the history's step and comes back when
 * the search ends.  One result is chosen (the first of a new list, drawn as
 * the list of pages draws the page shown); Up and Down choose another
 * (ws089-p012), Enter opens the chosen one, Esc ends the search, and a
 * result clicked opens its page.  A search is not a step of the history.
 */

#include "settings.h"

#include "include/kern/os-name.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* How many words of a query count, and the bytes of one with its NUL. */
#define SEARCH_WORDS		8
#define SEARCH_WORD		32

/* A result's row, and the text sizes of its name and of its page. */
#define SEARCH_ROW		52
#define SEARCH_TEXT_NAME	15U
#define SEARCH_TEXT_PAGE	12U

/* The space between the header and the card, and the card's inner margin. */
#define SEARCH_PAD		18

/* How much of the page pane a chosen result keeps from the pane's edges when the keys scroll it into sight. */
#define SEARCH_REVEAL_MARGIN	12

/*
 * One setting a page shows, as the search knows it: the page, the
 * setting's name, and more words it is found by.
 */
struct search_setting {
	unsigned page;
	const char *name;
	const char *words;
};

/*
 * The words of a query, lowered, each a separate string.
 */
struct search_words {
	char word[SEARCH_WORDS][SEARCH_WORD];
	int count;
};

/*
 * The settings the pages show, in the order the results list them after
 * the pages.  A page that is not ready has none yet.  The table lives for
 * the whole run; results point at its names.
 */
static const struct search_setting search_settings[] = {
	{ SE_PAGE_WIFI, "Wi-Fi radio", "switch on off turn radio wireless" },
	{ SE_PAGE_WIFI, "Networks around", "scan list ssid signal strength" },
	{ SE_PAGE_WIFI, "Network key", "password key passphrase wpa join new" },
	{ SE_PAGE_WIFI, "Disconnect", "disconnect leave forget" },
	{ SE_PAGE_ETHERNET, "IP address", "ip ipv4 address" },
	{ SE_PAGE_ETHERNET, "Netmask", "netmask prefix subnet mask" },
	{ SE_PAGE_ETHERNET, "MAC address", "mac hardware ethernet address" },
	{ SE_PAGE_ETHERNET, "MTU", "mtu packet size" },
	{ SE_PAGE_ETHERNET, "Bytes received and sent", "counters traffic bytes packets" },
	{ SE_PAGE_NETWORK, "Connection status", "status internet online offline connected" },
	{ SE_PAGE_NETWORK, "DNS servers", "dns name server resolver" },
	{ SE_PAGE_NETWORK, "Network activity", "activity usage traffic graph bandwidth" },
	{ SE_PAGE_APPEARANCE, "Window opacity", "transparency opacity see-through windows glass" },
	{ SE_PAGE_WALLPAPER, "Desktop picture", "wallpaper background picture image" },
	{ SE_PAGE_DISPLAY, "Resolution", "resolution mode size refresh rate hz" },
	{ SE_PAGE_DISPLAY, "Scale", "scale zoom hidpi size" },
	{ SE_PAGE_STORAGE, "Disk usage", "disk space free used capacity" },
	{ SE_PAGE_MOUSE, "Pointer speed", "speed pointer cursor mouse fast slow" },
	{ SE_PAGE_MOUSE, "Natural scrolling", "natural scroll wheel direction reverse" },
	{ SE_PAGE_MOUSE, "Acceleration", "acceleration accel pointer mouse ballistics" },
	{ SE_PAGE_TOUCHPAD, "Touchpad speed", "speed pointer cursor touchpad trackpad fast slow" },
	{ SE_PAGE_TOUCHPAD, "Touchpad acceleration", "acceleration accel touchpad trackpad" },
	{ SE_PAGE_TOUCHPAD, "Touchpad scrolling", "natural scroll two fingers direction touchpad" },
	{ SE_PAGE_KEYBOARD, "Key repeat rate", "repeat rate keys held" },
	{ SE_PAGE_KEYBOARD, "Delay before repeat", "delay repeat keys held" },
	{ SE_PAGE_SOUND, "Sound service", "audio sound service audiod output" },
	{ SE_PAGE_ABOUT, "System version", "version kernel release " ZEDBSD_OS_ID " zedbsd operating system" },
	{ SE_PAGE_ABOUT, "Processor", "processor cpu cores machine" },
	{ SE_PAGE_ABOUT, "Graphics", "graphics gpu vulkan video" },
	{ SE_PAGE_ABOUT, "Screen", "screen resolution display refresh" },
	{ SE_PAGE_ABOUT, "Computer name", "hostname host computer name" },
	{ SE_PAGE_ABOUT, "Uptime", "uptime running time since boot" }
};

static void search_split(const char *query, struct search_words *words);
static int search_matches(const struct search_words *words, const char *name, const char *more);
static int search_begins(const char *haystack, const char *needle);
static void search_add(struct se_app *app, unsigned page, const char *setting);
static void search_row(struct se_app *app, struct kl_canvas *canvas, unsigned index, int x, int y, int width, int last);
static void search_reveal(struct se_app *app, const struct kl_rect *row);

/*
 * Searches the pages and their settings for a query as typed; a query
 * without a word ends the search.
 */
void
se_search_set(
	struct se_app *app,
	const char *query)
{
	struct se_search *search;
	struct search_words words;
	const struct se_page *page;
	unsigned index;
	int matched;
	int started;

	/* The query as typed, and its words. */
	search = &app->search;
	(void)snprintf(search->query, sizeof(search->query), "%s", query);
	search_split(search->query, &words);

	/* A query without a word shows the page again. */
	if (words.count == 0) {
		se_search_end(app);
		return;
	}

	/* A search that starts shows its results from their top. */
	started = 0;
	if (search->active == 0)
		started = 1;
	search->active = 1;
	search->count = 0;
	if (started != 0)
		app->page_scroll = 0;

	/* A new list of results has its first one chosen, wherever the keys had moved in the last. */
	search->chosen = 0;
	search->reveal = 0;

	/* The pages whose name or words hold every word of the query (Home is not a result). */
	for (index = SE_PAGE_HOME + 1; index < SE_PAGES; index++) {
		page = &se_pages[index];
		matched = search_matches(&words, page->name, page->keywords);
		if (matched != 0)
			search_add(app, index, NULL);
	}

	/* Then the settings on the pages. */
	for (index = 0; index < sizeof(search_settings) / sizeof(search_settings[0]); index++) {
		matched = search_matches(&words, search_settings[index].name, search_settings[index].words);
		if (matched != 0)
			search_add(app, search_settings[index].page, search_settings[index].name);
	}

	/* A new list of results needs a frame. */
	app->dirty = 1;
	se_log("SEARCH query=%s results=%u", search->query, search->count);
}

/*
 * Ends the search: its results go and the page shows again.  The query is
 * emptied, which the titlebar's field shows.
 */
void
se_search_end(
	struct se_app *app)
{
	struct se_search *search;

	/* Nothing to end when no query is typed. */
	search = &app->search;
	if (search->active == 0 && search->query[0] == '\0')
		return;

	/* The query, the results and the choice among them are dropped; the page is shown from its top. */
	search->query[0] = '\0';
	search->count = 0;
	search->chosen = 0;
	search->reveal = 0;
	if (search->active != 0)
		app->page_scroll = 0;
	search->active = 0;
	app->dirty = 1;
	se_log("SEARCH end page=%s", se_pages[app->page].word);
}

/*
 * Asks for the titlebar's search field to have the keyboard (Ctrl+F); the
 * titlebar gives it at its next state.
 */
void
se_search_focus(
	struct se_app *app)
{
	/* A new serial is a new request, which the titlebar notices. */
	app->search.focus_serial++;
	app->dirty = 1;
	se_log("SEARCH focus");
}

/*
 * Opens the chosen result's page and ends the search (Enter, in the
 * titlebar's field or in the window).  Returns 1, or 0 when the search
 * found nothing and nothing changes.
 */
int
se_search_open_chosen(
	struct se_app *app)
{
	/* Nothing found, nothing opened. */
	if (app->search.active == 0 || app->search.count == 0U)
		return 0;

	/* The chosen result (the first unless the keys moved). */
	se_search_press(app, (int)app->search.chosen);

	/* Succeeded: its page is shown. */
	return 1;
}

/*
 * Chooses the result after (direction 1, Down) or before (-1, Up) the
 * chosen one; the ends stay.  The next frame scrolls it into sight.
 */
void
se_search_step(
	struct se_app *app,
	int direction)
{
	struct se_search *search;
	unsigned chosen;

	/* Without results there is nothing to choose. */
	search = &app->search;
	if (search->active == 0 || search->count == 0U)
		return;

	/* The neighbour in the list, short of either end. */
	chosen = search->chosen;
	if (direction < 0 && chosen > 0U)
		chosen--;
	if (direction > 0 && chosen + 1U < search->count)
		chosen++;

	/* The choice, drawn and brought into sight at the next frame. */
	search->chosen = chosen;
	search->reveal = 1;
	app->dirty = 1;
	se_log("SEARCH chosen index=%u page=%s", chosen, se_pages[search->results[chosen].page].word);
}

/*
 * Draws the results of the search from a top edge within a column: the
 * header, and a card of the pages and settings found (or a line saying
 * nothing matched).  Returns the edge below them.
 */
int
se_search_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_page header;
	char summary[SE_SEARCH_QUERY + 64];
	unsigned index;
	int height;
	int y;
	int last;

	/* The header: what was searched, and how much was found. */
	memset(&header, 0, sizeof(header));
	header.glyph = SE_GLYPH_GRID;
	header.name = "Search Results";
	if (app->search.count == 0U) {
		(void)snprintf(summary, sizeof(summary), "Nothing matches \"%s\".", app->search.query);
	} else if (app->search.count == 1U) {
		(void)snprintf(summary, sizeof(summary), "1 result for \"%s\".", app->search.query);
	} else {
		(void)snprintf(summary, sizeof(summary), "%u results for \"%s\".", app->search.count, app->search.query);
	}

	/* The header as a page's, large. */
	header.summary = summary;
	y = se_page_header(app, canvas, &header, x, top, width);

	/* Nothing found: the header says so. */
	if (app->search.count == 0U)
		return y;

	/* One card holds every result, a row each. */
	y += 20;
	height = 2 * SEARCH_PAD + (int)app->search.count * SEARCH_ROW;
	(void)se_card_begin(app, canvas, x, y, width, height, NULL, NULL);

	/* Each result, a line between two. */
	for (index = 0; index < app->search.count; index++) {
		last = 0;
		if (index + 1U == app->search.count)
			last = 1;
		search_row(app, canvas, index, x, y + SEARCH_PAD + (int)index * SEARCH_ROW, width, last);
	}

	/* The edge below the card. */
	return y + height;
}

/*
 * Opens a result's page (a result clicked, or the first for Enter) and
 * ends the search.
 */
void
se_search_press(
	struct se_app *app,
	int index)
{
	unsigned page;

	/* An index past the results does nothing. */
	if (index < 0 || (unsigned)index >= app->search.count)
		return;

	/* The page, then the search ends (it shows that page from its top). */
	page = app->search.results[index].page;
	se_log("SEARCH open page=%s", se_pages[page].word);
	se_ui_go(app, page);
	se_search_end(app);
}

/* Splits a query into its words, lowered; words past the most kept are dropped, a word too long is cut. */
static void
search_split(
	const char *query,
	struct search_words *words)
{
	const char *next;
	size_t length;
	int character;

	/* No word yet. */
	memset(words, 0, sizeof(*words));

	/* Each run of characters between spaces is a word. */
	length = 0;
	for (next = query; *next != '\0'; next++) {
		character = (unsigned char)*next;

		/* A space ends the word being gathered. */
		if (character == ' ' || character == '\t') {
			if (length != 0U) {
				words->count++;
				length = 0;
			}

			/* The space itself is no part of a word. */
			continue;
		}

		/* A word past the most kept is not gathered. */
		if (words->count == SEARCH_WORDS)
			break;

		/* The character joins the word, lowered, unless the word is full. */
		if (length + 1U < SEARCH_WORD) {
			words->word[words->count][length] = (char)tolower(character);
			length++;
		}
	}

	/* The last word, when the query did not end with a space. */
	if (length != 0U && words->count < SEARCH_WORDS)
		words->count++;
}

/* Tells whether every word of a query begins a word of a name or of its further words. */
static int
search_matches(
	const struct search_words *words,
	const char *name,
	const char *more)
{
	int index;
	int found;

	/* Each word must be somewhere. */
	for (index = 0; index < words->count; index++) {
		/* In the name. */
		found = search_begins(name, words->word[index]);
		if (found != 0)
			continue;

		/* Or in the further words. */
		found = search_begins(more, words->word[index]);
		if (found == 0)
			return 0;
	}

	/* Every word was found. */
	return 1;
}

/*
 * Tells whether a lowered word begins one of a text's words, ignoring the
 * text's case ("wi" finds "Wi-Fi" and "wired", not "bandwidth").
 */
static int
search_begins(
	const char *haystack,
	const char *needle)
{
	size_t start;
	size_t offset;
	int character;
	int letter;

	/* Each place of the text the word could start at. */
	for (start = 0; haystack[start] != '\0'; start++) {
		/* Only the start of the text or a place after a character that is not a letter or a digit starts a word. */
		if (start > 0) {
			letter = isalnum((unsigned char)haystack[start - 1]);
			if (letter != 0)
				continue;
		}

		/* The word's characters against the text's from there. */
		for (offset = 0; needle[offset] != '\0'; offset++) {
			character = tolower((unsigned char)haystack[start + offset]);
			if (character != needle[offset])
				break;
		}

		/* Every character matched. */
		if (needle[offset] == '\0')
			return 1;
	}

	/* The word is not in the text. */
	return 0;
}

/* Adds a result, unless the list is full. */
static void
search_add(
	struct se_app *app,
	unsigned page,
	const char *setting)
{
	struct se_search_result *result;

	/* A full list keeps the first results. */
	if (app->search.count == SE_SEARCH_RESULTS)
		return;

	/* The result at the end. */
	result = &app->search.results[app->search.count];
	result->page = page;
	result->setting = setting;
	app->search.count++;
}

/* Draws one result's row: the page's picture, the name found, and where it is; the chosen one selected, another lit under the pointer, and clickable. */
static void
search_row(
	struct se_app *app,
	struct kl_canvas *canvas,
	unsigned index,
	int x,
	int y,
	int width,
	int last)
{
	const struct se_search_result *result;
	const struct se_page *page;
	struct kl_rect row;
	char where[96];
	const char *name;
	kl_color glyph;
	int left;
	int right;
	int lit;
	int divided;

	/* The result's page. */
	result = &app->search.results[index];
	page = &se_pages[result->page];

	/* The row's rectangle inside the card's margins. */
	row.x = x + 8;
	row.y = y;
	row.width = width - 16;
	row.height = SEARCH_ROW;

	/* The chosen row has the selection's ground (as the list's page shown), another a shade under the pointer. */
	lit = se_ui_lit(app, SE_HIT_RESULT, (int)index);
	if (index == app->search.chosen) {
		kl_canvas_round(canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 10.0f, SE_COLOR_SELECTION);
	} else if (lit != 0) {
		kl_canvas_round(canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 10.0f, SE_COLOR_HOVER);
	}

	/* The chosen row the keys just moved to is scrolled into sight. */
	if (index == app->search.chosen && app->search.reveal != 0)
		search_reveal(app, &row);

	/* The page's picture: the accent for a page that works, quiet for one that is coming. */
	glyph = SE_COLOR_ACCENT;
	if (page->ready == 0)
		glyph = SE_COLOR_ICON;
	left = x + SEARCH_PAD + 2;
	se_glyph_draw(canvas, page->glyph, (float)left, (float)y + 15.0f, 22.0f, glyph);

	/* The name found: the setting's, or the page's own; under it, where it is. */
	name = kl_tr(page->name);
	if (result->setting != NULL) {
		name = result->setting;
		(void)snprintf(where, sizeof(where), "%s", kl_tr(page->name));
	} else if (page->ready != 0) {
		(void)se_named(where, sizeof(where), kl_tr(page->summary));
	} else {
		(void)snprintf(where, sizeof(where), "%s", kl_tr("Coming in a later version"));
	}

	/* The two lines, beside the picture and short of the chevron. */
	right = x + width - SEARCH_PAD;
	(void)kl_text_draw_fit(app->text, canvas, left + 38, y + 22, name, SEARCH_TEXT_NAME, 1, right - left - 60, SE_COLOR_TEXT);
	(void)kl_text_draw_fit(app->text, canvas, left + 38, y + 40, where, SEARCH_TEXT_PAGE, 0, right - left - 60, SE_COLOR_TEXT_SECONDARY);

	/* The chevron at the right says the row opens a page. */
	se_glyph_draw(canvas, SE_GLYPH_CHEVRON, (float)right - 18.0f, (float)y + 17.0f, 16.0f, SE_COLOR_TEXT_FAINT);

	/* The line under the row, unless it is the last or it would cross the chosen row's ground. */
	divided = 1;
	if (last != 0)
		divided = 0;
	if (index == app->search.chosen || index + 1U == app->search.chosen)
		divided = 0;
	if (divided != 0)
		kl_canvas_line(canvas, (float)left + 38.0f, (float)(y + SEARCH_ROW) - 0.5f, (float)right, (float)(y + SEARCH_ROW) - 0.5f, 1.0f, SE_COLOR_SEPARATOR);

	/* A click opens the result's page. */
	se_ui_hit(app, &row, SE_HIT_RESULT, (int)index);
}

/* Scrolls the page pane (at the next frame) so that the chosen result's row is in sight, once after the keys moved it. */
static void
search_reveal(
	struct se_app *app,
	const struct kl_rect *row)
{
	const struct kl_rect *pane;
	int scroll;

	/* Only once for a move of the keys. */
	app->search.reveal = 0;

	/* A row above the pane's top comes down to it, one below its bottom comes up to it. */
	pane = &app->layout.page;
	scroll = app->page_scroll;
	if (row->y < pane->y + SEARCH_REVEAL_MARGIN)
		scroll -= pane->y + SEARCH_REVEAL_MARGIN - row->y;
	if (row->y + row->height > pane->y + pane->height - SEARCH_REVEAL_MARGIN)
		scroll += row->y + row->height - (pane->y + pane->height - SEARCH_REVEAL_MARGIN);
	if (scroll < 0)
		scroll = 0;

	/* A change needs a frame (the page's extent keeps it within the page at the next one). */
	if (scroll != app->page_scroll) {
		app->page_scroll = scroll;
		app->dirty = 1;
	}
}
