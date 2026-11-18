/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A page's web fonts (ws074-p070): the faces the @font-face rules of its
 * style sheets name, each fetched once from its first source this pass
 * reads (WOFF, TrueType), turned into an sfnt file, and added to the
 * page's text system under its family when the page is next laid out.
 *
 * With the embedder's loader an http or https font is fetched without
 * blocking: the page is laid out with the fonts it has until the loader's
 * callback counts the arrival in the page's fonts_generation, which lays
 * the page out again.  A font that cannot be fetched or read is
 * remembered as failed, and its family falls to the next one of the style.
 */

#include "page/page.h"
#include "net/net.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * One web font of the page: its face's family, weights and style, the
 * location it is fetched from, its sfnt file once it arrived (until the
 * text system takes it), and where it is: fetched, arrived, added to the
 * text system, or failed.  While it is fetched it has its request and the
 * page it is for.
 */
struct page_font {
	struct vm_string *family;
	int weight_min;
	int weight_max;
	int italic;
	char *location;
	struct wb_buffer data;
	int state;
	struct page *page;
	struct net_request *request;
};

/* Where a web font is. */
enum fonts_state {
	FONTS_FETCHING,
	FONTS_ARRIVED,
	FONTS_ADDED,
	FONTS_FAILED
};

static int fonts_add_face(struct page *page, const struct css_font_face *face);
static const struct vm_string *fonts_pick_source(const struct css_font_face *face);
static int fonts_url_ends(const struct vm_string *url, const char *ending);
static struct page_font *fonts_find(const struct page *page, const struct css_font_face *face, const char *location);
static void fonts_read(struct page_font *font, const unsigned char *bytes, size_t length);
static void fonts_arrived(void *context, struct net_request *request);
static void fonts_free(struct page_font *font);

/*
 * Starts a page's table of web fonts empty.
 */
void
page_fonts_init(
	struct page *page)
{
	/* No font yet. */
	wb_vector_init(&page->fonts, sizeof(struct page_font *));
	page->fonts_generation = 0;
	page->laid_out_fonts = 0;
}

/*
 * Starts loading the faces a style sheet's @font-face rules name that the
 * page does not have yet.
 */
int
page_fonts_add_sheet(
	struct page *page,
	const struct css_sheet *sheet)
{
	size_t count;
	size_t index;
	int error;

	/* Each rule's face. */
	count = css_sheet_font_face_count(sheet);
	for (index = 0; index < count; index++) {
		error = fonts_add_face(page, css_sheet_font_face(sheet, index));
		if (error != 0)
			return error;
	}

	/* Succeeded: the sheet's faces are loading or loaded. */
	return 0;
}

/*
 * Adds the fonts that arrived to the page's text system (its fonts must be
 * open), so that the next layout finds their families.
 */
void
page_fonts_install(
	struct page *page)
{
	struct page_font *font;
	size_t index;
	int error;

	/* Nothing is added before the text system is open. */
	if (!page->text_open)
		return;

	/* Each font that arrived, added under its family (a face the text system cannot open fails). */
	for (index = 0; index < page->fonts.count; index++) {
		font = *(struct page_font **)wb_vector_at(&page->fonts, index);
		if (font->state != FONTS_ARRIVED)
			continue;
		error = text_add_face(&page->text, font->family, font->weight_min, font->weight_max, font->italic, &font->data);
		font->state = FONTS_ADDED;
		if (error != 0)
			font->state = FONTS_FAILED;
		wb_buffer_release(&font->data);
	}

	/* The layout that follows has every font that arrived. */
	page->laid_out_fonts = page->fonts_generation;
}

/* A ready promise waits until every requested font has finished fetching. */
int
page_fonts_pending(
	const struct page *page)
{
	const struct page_font *font;
	size_t index;

	for (index = 0; index < page->fonts.count; index++) {
		font = *(struct page_font *const *)wb_vector_at(&page->fonts, index);
		if (font->state == FONTS_FETCHING)
			return 1;
	}
	return 0;
}

/*
 * Frees the page's web fonts, cancelling the fetches under way.
 */
void
page_fonts_release(
	struct page *page)
{
	struct page_font *font;
	size_t index;

	/* Each entry. */
	for (index = 0; index < page->fonts.count; index++) {
		font = *(struct page_font **)wb_vector_at(&page->fonts, index);
		fonts_free(font);
	}

	/* The table itself. */
	wb_vector_release(&page->fonts);
}

/*
 * Adds one @font-face rule's face: nothing when it has no source this pass
 * reads or the page has it already; otherwise an entry fetched through the
 * loader (http, https) or read at once (other locations).
 */
static int
fonts_add_face(
	struct page *page,
	const struct css_font_face *face)
{
	const struct vm_string *source;
	struct page_font *font;
	struct wb_buffer location;
	struct wb_buffer bytes;
	int remote;
	int error;

	/* The first source in a format this pass reads. */
	source = fonts_pick_source(face);
	if (source == NULL)
		return 0;

	/* Its location (resolved with the sheet's URLs already). */
	wb_buffer_init(&location);
	error = vm_string_to_utf8(source, &location);
	if (error != 0) {
		wb_buffer_release(&location);
		return error;
	}

	/* A face the page has already is not loaded again. */
	font = fonts_find(page, face, wb_buffer_string(&location));
	if (font != NULL) {
		wb_buffer_release(&location);
		return 0;
	}

	/* The new entry. */
	font = calloc(1, sizeof(*font));
	if (font == NULL) {
		wb_buffer_release(&location);
		return ENOMEM;
	}

	/* It is fetched for the face's family, weights and style. */
	font->family = face->family;
	font->weight_min = face->weight_min;
	font->weight_max = face->weight_max;
	font->italic = face->italic;
	font->page = page;
	font->state = FONTS_FETCHING;
	wb_buffer_init(&font->data);
	font->location = strdup(wb_buffer_string(&location));
	wb_buffer_release(&location);
	if (font->location == NULL) {
		fonts_free(font);
		return ENOMEM;
	}

	/* The entry goes into the table. */
	error = wb_vector_push(&page->fonts, &font);
	if (error != 0) {
		fonts_free(font);
		return error;
	}

	/* An http or https font with a loader is fetched without blocking. */
	remote = net_loader_takes(font->location);
	if (page->loader != NULL && remote) {
		error = net_loader_client_send(page_client(page), font->location, NET_FETCH_DOCUMENT, NULL, fonts_arrived, font, &font->request);
		if (error != 0)
			font->state = FONTS_FAILED;
		return 0;
	}

	/* Any other location is read now. */
	wb_buffer_init(&bytes);
	error = page_fetch(page_cookies(page), page->base, font->location, &bytes, NULL);
	if (error == 0) {
		fonts_read(font, (const unsigned char *)bytes.data, bytes.length);
	} else {
		font->state = FONTS_FAILED;
	}

	/* The file's bytes were turned into the font's own. */
	wb_buffer_release(&bytes);

	/* Succeeded: the face is loading or loaded. */
	return 0;
}

/*
 * Picks a face's first source this pass reads: one declared WOFF or
 * TrueType (or OpenType), or one without a format whose name ends in
 * .woff, .ttf or .otf.  NULL when there is none.
 */
static const struct vm_string *
fonts_pick_source(
	const struct css_font_face *face)
{
	const struct vm_string *url;
	size_t index;
	int ends;

	/* The sources in the order the rule gives them. */
	for (index = 0; index < face->source_count; index++) {
		url = face->sources[index];

		/* A declared format this pass reads. */
		if (face->formats[index] == CSS_FONT_FORMAT_WOFF || face->formats[index] == CSS_FONT_FORMAT_TRUETYPE)
			return url;
		if (face->formats[index] != CSS_FONT_FORMAT_UNKNOWN)
			continue;

		/* No format: the name's ending decides. */
		ends = fonts_url_ends(url, ".woff");
		if (ends)
			return url;
		ends = fonts_url_ends(url, ".ttf");
		if (ends)
			return url;
		ends = fonts_url_ends(url, ".otf");
		if (ends)
			return url;
	}

	/* No source this pass reads. */
	return NULL;
}

/* Tells whether a URL ends in an ASCII ending, ignoring ASCII case (a query or fragment after it is not looked at). */
static int
fonts_url_ends(
	const struct vm_string *url,
	const char *ending)
{
	size_t length;
	size_t index;
	uint16_t unit;
	uint16_t wanted;

	/* The URL must be at least as long as the ending. */
	length = strlen(ending);
	if (url->length < length)
		return 0;

	/* Compares its last characters, folding ASCII upper case. */
	for (index = 0; index < length; index++) {
		unit = vm_string_at(url, url->length - length + index);
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit + 0x20U);
		wanted = (unsigned char)ending[index];
		if (unit != wanted)
			return 0;
	}

	/* The URL ends so. */
	return 1;
}

/* Finds a font of the table by its face's family, weights, style and location. */
static struct page_font *
fonts_find(
	const struct page *page,
	const struct css_font_face *face,
	const char *location)
{
	struct page_font *font;
	size_t index;
	int differs;

	/* Each entry of the table. */
	for (index = 0; index < page->fonts.count; index++) {
		font = *(struct page_font **)wb_vector_at(&page->fonts, index);
		if (font->family != face->family || font->italic != face->italic)
			continue;
		if (font->weight_min != face->weight_min || font->weight_max != face->weight_max)
			continue;

		/* The same face: the location decides. */
		differs = strcmp(font->location, location);
		if (differs == 0)
			return font;
	}

	/* The face is not in the table. */
	return NULL;
}

/* Turns a font's bytes into its sfnt file, which the next layout adds; a file this pass does not read fails. */
static void
fonts_read(
	struct page_font *font,
	const unsigned char *bytes,
	size_t length)
{
	int error;

	/* The sfnt file. */
	error = text_font_file(bytes, length, &font->data);
	if (error != 0) {
		wb_buffer_release(&font->data);
		font->state = FONTS_FAILED;
		return;
	}

	/* It waits for the next layout. */
	font->state = FONTS_ARRIVED;
}

/*
 * The loader's callback for a font: its body read when it came (a failure,
 * or a status other than 2xx, marks the font failed), and the page told to
 * lay itself out again.
 */
static void
fonts_arrived(
	void *context,
	struct net_request *request)
{
	const struct net_response *response;
	struct page_font *font;
	int error;

	/* The font, which waits no longer. */
	font = context;
	font->request = NULL;

	/* A response with the font's bytes. */
	error = net_request_error(request);
	response = net_request_response(request);
	if (error == 0 && (response->status < 200 || response->status > 299))
		error = EINVAL;
	if (error == 0) {
		fonts_read(font, (const unsigned char *)response->body.data, response->body.length);
	} else {
		font->state = FONTS_FAILED;
	}

	/* The page is laid out again with it. */
	font->page->fonts_generation++;
}

/* Frees one entry: its fetch, bytes and location. */
static void
fonts_free(
	struct page_font *font)
{
	/* The fetch under way is cancelled (its callback is not called). */
	net_request_cancel(font->request);
	wb_buffer_release(&font->data);
	free(font->location);
	free(font);
}
