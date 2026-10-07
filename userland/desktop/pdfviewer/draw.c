/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frame of PDF Viewer: the sidebar of thumbnails on the left while it
 * is shown, and beside it the pages where the view lays them out, the page
 * indicator, the notice that a page drawn has content libpdf could not
 * show and the message over them; the password card over everything (the
 * file chooser is libkeiland's window of its own, ws090-p008).
 *
 * The pages come from the document's cache of rasters (document.c), made
 * when a page is first drawn at the scale in force.  The pages' part of the
 * frame is a canvas over the frame's pixels right of the sidebar, so that
 * everything laid out for the pages draws there as in a window of its own.
 * A thumbnail not drawn yet is a white page with its number; the view
 * draws thumbnails while it waits (pv_app_prefetch()).
 */

#include "viewer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * Whether the frame is drawn in the dark appearance's colours
 * (ws089-p017): set by pv_draw_set_dark when the desktop tells it (main.c),
 * light until then.  Only the viewer's thread reads or changes it.
 */
static int draw_dark;

/*
 * The accent the user chose and the ink on it (ws179-p002), 0xAARRGGBB:
 * set by pv_draw_set_accent when the desktop tells it (main.c), blue and
 * white until then; the main thread alone uses them.
 */
static uint32_t draw_accent = 0xff2f7cf6U;
static uint32_t draw_accent_ink = 0xffffffffU;

/* The colours of the frame (0xAARRGGBB, not premultiplied), the light appearance's and the dark one's (ws089-p017; the pages stay white). */
#define DRAW_BACKGROUND		draw_choose(0xffdfe2e7U, 0xff16191fU)
#define DRAW_SHADOW		0x22000000U
#define DRAW_EDGE		0x33000000U
#define DRAW_PILL		0xcc1f2430U
#define DRAW_PILL_TEXT		0xffffffffU
#define DRAW_HINT		draw_choose(0xff5a6070U, 0xffa9b2bfU)
#define DRAW_TITLE		draw_choose(0xff2a2f3aU, 0xffe2e8f0U)
#define DRAW_DIM		0x66000000U
#define DRAW_CARD		draw_choose(0xfff7f8faU, 0xff262b34U)
#define DRAW_MESSAGE		draw_choose(0xf0ffffffU, 0xf0262b34U)
#define DRAW_MESSAGE_TEXT	draw_choose(0xff8a1f1fU, 0xfff08a8aU)
#define DRAW_NOTICE		draw_choose(0xf2f7f8faU, 0xf22c313bU)
#define DRAW_NOTICE_TEXT	draw_choose(0xff3a4150U, 0xffcbd5e1U)
#define DRAW_NOTICE_MARK	0xffe0a526U
#define DRAW_SIDEBAR		draw_choose(0xffeceef2U, 0xff1f232aU)
#define DRAW_SIDEBAR_EDGE	draw_choose(0xffcfd3daU, 0xff343a45U)
#define DRAW_CURRENT		((draw_accent & 0x00ffffffU) | 0x40000000U)
#define DRAW_CURRENT_EDGE	draw_accent
#define DRAW_FIELD		draw_choose(0xffffffffU, 0xff1f232aU)
#define DRAW_FIELD_EDGE		draw_choose(0xffb9bfc9U, 0xff4a515dU)
#define DRAW_WRONG		0xffc0392bU
#define DRAW_BUTTON		draw_choose(0xffe3e6ebU, 0xff2f3540U)
#define DRAW_BUTTON_DEFAULT	draw_accent
#define DRAW_LOCK		draw_choose(0xff5a6070U, 0xffa9b2bfU)

/* What the notice says, and the display-list flags that call for it. */
#define DRAW_NOTICE_WORDS	"Some content could not be shown"
#define DRAW_NOTICE_FLAGS	(PDF_DISPLAY_SKIPPED | PDF_DISPLAY_DAMAGED | PDF_DISPLAY_LIMITED)

/* The text sizes, in pixels. */
#define DRAW_TEXT		15U
#define DRAW_TEXT_LARGE		20U

static uint32_t draw_choose(uint32_t light, uint32_t dark);
static void draw_page(struct pv_app *app, struct pv_canvas *canvas, size_t index, int x, int y);
static void draw_around(struct pv_canvas *canvas, int outer_x, int outer_y, int outer_width, int outer_height, int x, int y, int width, int height, uint32_t color);
static void draw_scroll(struct pv_app *app, struct pv_canvas *canvas);
static void draw_single(struct pv_app *app, struct pv_canvas *canvas);
static void draw_empty(struct pv_app *app, struct pv_canvas *canvas);
static void draw_indicator(struct pv_app *app, struct pv_canvas *canvas);
static void draw_notice(struct pv_app *app, struct pv_canvas *canvas);
static void draw_message(struct pv_app *app, struct pv_canvas *canvas);
static void draw_centred(struct pv_app *app, struct pv_canvas *canvas, int baseline, const char *text, unsigned pixels, uint32_t color);
static void draw_sidebar(struct pv_app *app, struct pv_canvas *canvas, int width);
static void draw_thumbnail(struct pv_app *app, struct pv_canvas *canvas, size_t index, size_t current);
static void draw_password(struct pv_app *app, struct pv_canvas *canvas);
static void draw_button(struct pv_app *app, struct pv_canvas *canvas, int x, int y, const char *label, int is_default);

/*
 * Draws the frame.
 */
void
pv_draw(
	struct pv_app *app,
	struct pv_canvas *canvas)
{
	struct pv_canvas pages;
	int sidebar;

	/* The ground; the pages drawn gather their flags anew. */
	pv_canvas_fill(canvas, 0, 0, canvas->width, canvas->height, DRAW_BACKGROUND);
	app->shown_flags = 0;

	/* The sidebar, when it is shown. */
	sidebar = pv_app_sidebar_width(app);
	if (sidebar > canvas->width)
		sidebar = canvas->width;
	if (sidebar > 0)
		draw_sidebar(app, canvas, sidebar);

	/* The pages' canvas: the frame right of the sidebar. */
	pages.pixels = canvas->pixels + sidebar;
	pages.stride = canvas->stride;
	pages.width = canvas->width - sidebar;
	pages.height = canvas->height;

	/* The pages, by the mode, or the empty window's hint. */
	if (!app->has_document) {
		draw_empty(app, &pages);
	} else if (app->mode == PV_MODE_SCROLL) {
		draw_scroll(app, &pages);
	} else {
		draw_single(app, &pages);
	}

	/* The page indicator, the notice and the message over the pages. */
	if (app->has_document && app->indicator_until != 0)
		draw_indicator(app, &pages);
	draw_notice(app, &pages);
	if (app->message[0] != '\0')
		draw_message(app, &pages);

	/* The password card over everything. */
	if (app->asking_password)
		draw_password(app, canvas);
	app->dirty = 0;
}

/* Draws a page with its top left at a place: a shadow, an edge, and its raster. */
static void
draw_page(
	struct pv_app *app,
	struct pv_canvas *canvas,
	size_t index,
	int x,
	int y)
{
	const struct pv_page *page;
	double difference;
	double scale;
	int width;
	int height;
	int error;

	/* The page's size at its scale. */
	scale = pv_app_scale(app, index);
	width = (int)ceil(app->document.pages[index].width * scale - 1e-6);
	height = (int)ceil(app->document.pages[index].height * scale - 1e-6);

	/* Nothing to draw for a page outside the frame. */
	if (x >= canvas->width ||
	    y >= canvas->height ||
	    x + width <= 0 ||
	    y + height <= 0)
		return;

	/*
	 * A soft shadow and a thin edge around it, blended only where the page
	 * leaves them showing (BUG-259: blending them under the whole page,
	 * which then covers them, cost most of a frame).
	 */
	draw_around(canvas, x - 1, y + 2, width + 2, height + 2, x, y, width, height, DRAW_SHADOW);
	draw_around(canvas, x - 1, y - 1, width + 2, height + 2, x, y, width, height, DRAW_EDGE);

	/* While two fingers zoom, a raster at another scale is stretched rather than drawn again (ws081-p012). */
	page = &app->document.pages[index];
	if (app->zooming && page->raster != NULL) {
		if (page->list != NULL)
			app->shown_flags |= page->list->flags;
		pv_canvas_stretch(canvas, x, y, width, height, page->raster, page->raster_width, page->raster_height);
		return;
	}

	/*
	 * While the window is resized, a raster at another scale is stretched
	 * too (BUG-259): the page is drawn again once the size settles.
	 */
	difference = 0.0;
	if (page->raster != NULL)
		difference = page->raster_scale - scale;
	if (app->resizing &&
	    page->raster != NULL &&
	    (difference > 1e-6 || difference < -1e-6)) {
		if (page->list != NULL)
			app->shown_flags |= page->list->flags;
		pv_canvas_stretch(canvas, x, y, width, height, page->raster, page->raster_width, page->raster_height);
		return;
	}

	/* The raster at the scale; a page without one is white. */
	error = pv_document_raster(&app->document, index, scale, &page);
	if (error == 0 && page->list != NULL)
		app->shown_flags |= page->list->flags;
	if (error != 0 || page->raster == NULL) {
		pv_canvas_fill(canvas, x, y, width, height, 0xffffffffU);
		return;
	}

	/*
	 * A raster smaller than the page (one kept within the largest side)
	 * leaves part of it showing what is under: the shadow and the edge,
	 * as they were blended under the whole page.
	 */
	if (page->raster_width < width || page->raster_height < height) {
		pv_canvas_blend(canvas, x, y, width, height, DRAW_SHADOW);
		pv_canvas_blend(canvas, x, y, width, height, DRAW_EDGE);
	}

	/* The page's raster, and the places found and the selection over it (ws128-p004). */
	pv_canvas_copy(canvas, x, y, page->raster, page->raster_width, page->raster_height);
	pv_find_draw(app, canvas, index, x, y, page->raster_scale);
}

/*
 * Blends a colour over a rectangle (outer_*) less the part a page's
 * rectangle (x, y, width, height) covers: the bands above and below the
 * page, and the columns left and right of it between them.
 */
static void
draw_around(
	struct pv_canvas *canvas,
	int outer_x,
	int outer_y,
	int outer_width,
	int outer_height,
	int x,
	int y,
	int width,
	int height,
	uint32_t color)
{
	int outer_bottom;
	int outer_right;
	int middle_top;
	int middle_bottom;

	/* The outer rectangle's far edges. */
	outer_right = outer_x + outer_width;
	outer_bottom = outer_y + outer_height;

	/* The band above the page. */
	if (y > outer_y)
		pv_canvas_blend(canvas, outer_x, outer_y, outer_width, y - outer_y, color);

	/* The band below it. */
	if (outer_bottom > y + height)
		pv_canvas_blend(canvas, outer_x, y + height, outer_width, outer_bottom - (y + height), color);

	/* The rows beside the page, within the outer rectangle. */
	middle_top = y;
	if (middle_top < outer_y)
		middle_top = outer_y;
	middle_bottom = y + height;
	if (middle_bottom > outer_bottom)
		middle_bottom = outer_bottom;
	if (middle_bottom <= middle_top)
		return;

	/* The column left of the page. */
	if (x > outer_x)
		pv_canvas_blend(canvas, outer_x, middle_top, x - outer_x, middle_bottom - middle_top, color);

	/* The column right of it. */
	if (outer_right > x + width)
		pv_canvas_blend(canvas, x + width, middle_top, outer_right - (x + width), middle_bottom - middle_top, color);
}

/* Draws the scroll mode's column: every page that meets the frame. */
static void
draw_scroll(
	struct pv_app *app,
	struct pv_canvas *canvas)
{
	double scale;
	double top;
	double content_width;
	size_t index;
	size_t first;
	size_t last;
	int x;
	int y;
	int width;
	int height;

	/* Every page from the top, drawn when it meets the frame. */
	scale = pv_app_scale(app, 0);
	content_width = pv_app_content_width(app);
	top = PV_MARGIN;
	first = app->document.count;
	last = 0;
	for (index = 0; index < app->document.count; index++) {
		width = (int)ceil(app->document.pages[index].width * scale - 1e-6);
		height = (int)ceil(app->document.pages[index].height * scale - 1e-6);
		y = (int)floor(top - app->scroll_y);
		top += (double)height + PV_GAP;
		if (y + height <= 0)
			continue;
		if (y >= canvas->height)
			break;

		/* Centred across, or from the left of the view when the pages are wider. */
		x = (canvas->width - width) / 2;
		if (content_width > (double)canvas->width)
			x = (int)floor(PV_MARGIN + (content_width - 2.0 * PV_MARGIN - (double)width) / 2.0 - app->scroll_x);
		draw_page(app, canvas, index, x, y);
		if (index < first)
			first = index;
		last = index;
	}

	/* The rasters of the pages far from the view may go. */
	if (first < app->document.count) {
		if (first > 2)
			first -= 2;
		else
			first = 0;
		pv_document_trim(&app->document, first, last + 2);
	}
}

/* Draws the page mode's page, and its neighbour while it is swiped or turned. */
static void
draw_single(
	struct pv_app *app,
	struct pv_canvas *canvas)
{
	double scale;
	double top;
	double offset;
	size_t neighbour;
	int width;
	int height;
	int x;
	int y;

	/* The page, centred or panned, moved by the swipe. */
	scale = pv_app_scale(app, app->page);
	width = (int)ceil(app->document.pages[app->page].width * scale - 1e-6);
	height = (int)ceil(app->document.pages[app->page].height * scale - 1e-6);
	top = pv_app_page_top(app, app->page);
	x = (canvas->width - width) / 2;
	if (width + 2 * PV_MARGIN > canvas->width)
		x = (int)floor(PV_MARGIN - app->scroll_x);
	y = (int)floor(top - app->scroll_y);
	offset = app->swipe;
	draw_page(app, canvas, app->page, x + (int)floor(offset), y);

	/* The previous page comes from the left while the page moves right. */
	if (offset > 0.5 && app->page > 0) {
		neighbour = app->page - 1;
		scale = pv_app_scale(app, neighbour);
		width = (int)ceil(app->document.pages[neighbour].width * scale - 1e-6);
		height = (int)ceil(app->document.pages[neighbour].height * scale - 1e-6);
		x = (canvas->width - width) / 2 - (int)floor(pv_app_neighbour_distance(app, neighbour)) + (int)floor(offset);
		y = (canvas->height - height) / 2;
		if (y < PV_MARGIN)
			y = PV_MARGIN;
		draw_page(app, canvas, neighbour, x, y);
	}

	/* The next page comes from the right while the page moves left. */
	if (offset < -0.5 && app->page + 1 < app->document.count) {
		neighbour = app->page + 1;
		scale = pv_app_scale(app, neighbour);
		width = (int)ceil(app->document.pages[neighbour].width * scale - 1e-6);
		height = (int)ceil(app->document.pages[neighbour].height * scale - 1e-6);
		x = (canvas->width - width) / 2 + (int)floor(pv_app_neighbour_distance(app, neighbour)) + (int)floor(offset);
		y = (canvas->height - height) / 2;
		if (y < PV_MARGIN)
			y = PV_MARGIN;
		draw_page(app, canvas, neighbour, x, y);
	}

	/* The rasters of pages beyond the neighbours may go. */
	neighbour = 0;
	if (app->page > 1)
		neighbour = app->page - 1;
	pv_document_trim(&app->document, neighbour, app->page + 1);
}

/* Draws the window without a document: how to open one. */
static void
draw_empty(
	struct pv_app *app,
	struct pv_canvas *canvas)
{
	int middle;

	/* Two lines in the middle. */
	middle = canvas->height / 2;
	draw_centred(app, canvas, middle - 8, "No document is open", DRAW_TEXT_LARGE, DRAW_TITLE);
	draw_centred(app, canvas, middle + 22, "Open a PDF with Ctrl+O, or from Files", DRAW_TEXT, DRAW_HINT);
}

/* Draws the page indicator at the bottom: "Page 3 of 10". */
static void
draw_indicator(
	struct pv_app *app,
	struct pv_canvas *canvas)
{
	char text[64];
	size_t page;
	double middle;
	double bottom;
	double scale;
	int width;
	int height;
	int x;
	int y;

	/* The page the view is on: the page mode's, or the scroll mode's across the middle. */
	page = app->page;
	if (app->mode == PV_MODE_SCROLL) {
		middle = app->scroll_y + (double)app->height / 2.0;
		scale = pv_app_scale(app, 0);
		bottom = PV_MARGIN;
		for (page = 0; page + 1 < app->document.count; page++) {
			bottom += app->document.pages[page].height * scale + PV_GAP;
			if (bottom > middle)
				break;
		}
	}

	/* A dark pill with the text. */
	snprintf(text, sizeof(text), "Page %lu of %lu", (unsigned long)(page + 1), (unsigned long)app->document.count);
	width = pv_text_width(app->text, text, DRAW_TEXT) + 28;
	height = 30;
	x = (canvas->width - width) / 2;
	y = canvas->height - height - 20;
	pv_canvas_round(canvas, x, y, width, height, height / 2, DRAW_PILL);
	pv_text_draw(app->text, canvas, x + 14, y + 20, text, DRAW_TEXT, DRAW_PILL_TEXT);
}

/*
 * Draws, in the bottom left corner, a small notice that a page in view has
 * content libpdf left out (a font, filter or shading it does not read) or
 * could not finish, and logs when the notice comes and goes.
 */
static void
draw_notice(
	struct pv_app *app,
	struct pv_canvas *canvas)
{
	int wanted;
	int width;
	int height;
	int x;
	int y;

	/* Only a document whose pages in view left something out has the notice. */
	wanted = 0;
	if (app->has_document) {
		if ((app->shown_flags & DRAW_NOTICE_FLAGS) != 0)
			wanted = 1;
	}

	/* Logs a change, for the tests. */
	if (wanted != app->notice_shown) {
		app->notice_shown = wanted;
		if (wanted) {
			pv_log("NOTICE shown flags=%u", app->shown_flags);
		} else {
			pv_log("NOTICE hidden");
		}
	}

	/* Nothing to draw without the notice. */
	if (!wanted)
		return;

	/* A light pill with an amber mark and the words, clear of the page indicator in the middle. */
	width = pv_text_width(app->text, DRAW_NOTICE_WORDS, 13U) + 40;
	height = 28;
	x = 16;
	y = canvas->height - height - 21;
	pv_canvas_round(canvas, x + 1, y + 2, width, height, height / 2, DRAW_SHADOW);
	pv_canvas_round(canvas, x - 1, y - 1, width + 2, height + 2, height / 2 + 1, DRAW_EDGE);
	pv_canvas_round(canvas, x, y, width, height, height / 2, DRAW_NOTICE);
	pv_canvas_round(canvas, x + 12, y + 10, 8, 8, 4, DRAW_NOTICE_MARK);
	pv_text_draw(app->text, canvas, x + 28, y + 19, DRAW_NOTICE_WORDS, 13U, DRAW_NOTICE_TEXT);
}

/* Draws the message in a card at the top (in the middle for an empty window). */
static void
draw_message(
	struct pv_app *app,
	struct pv_canvas *canvas)
{
	int width;
	int height;
	int x;
	int y;

	/* A card as wide as the text, within the window. */
	width = pv_text_width(app->text, app->message, DRAW_TEXT) + 32;
	if (width > canvas->width - 32)
		width = canvas->width - 32;
	height = 40;
	x = (canvas->width - width) / 2;
	y = 20;
	if (!app->has_document)
		y = canvas->height / 2 + 48;
	pv_canvas_round(canvas, x + 1, y + 2, width, height, 10, DRAW_SHADOW);
	pv_canvas_round(canvas, x, y, width, height, 10, DRAW_MESSAGE);
	pv_text_draw(app->text, canvas, x + 16, y + 26, app->message, DRAW_TEXT, DRAW_MESSAGE_TEXT);
}

/*
 * Draws the sidebar: its panel and edge, and the slot of each page that
 * meets its view, the page in view marked.  The thumbnails far from the
 * view may go afterwards.
 */
static void
draw_sidebar(
	struct pv_app *app,
	struct pv_canvas *canvas,
	int width)
{
	struct pv_canvas panel;
	size_t first;
	size_t last;
	size_t index;
	size_t current;

	/* The sidebar's own canvas, so that nothing drawn in it spills onto the pages. */
	panel.pixels = canvas->pixels;
	panel.stride = canvas->stride;
	panel.width = width;
	panel.height = canvas->height;

	/* The panel, and its edge towards the pages. */
	pv_canvas_fill(&panel, 0, 0, width, panel.height, DRAW_SIDEBAR);
	pv_canvas_fill(&panel, width - 1, 0, 1, panel.height, DRAW_SIDEBAR_EDGE);

	/* The slots in view; nothing more for a document without pages. */
	pv_thumbnail_range(app, &first, &last);
	if (last < first)
		return;
	current = pv_app_current_page(app);
	for (index = first; index <= last; index++)
		draw_thumbnail(app, &panel, index, current);

	/* The thumbnails far from the view may go when they take too much memory. */
	if (first > 16) {
		first -= 16;
	} else {
		first = 0;
	}

	/* Frees those outside sixteen slots around the view. */
	pv_document_trim_thumbnails(&app->document, first, last + 16);
}

/*
 * Draws one page's slot in the sidebar: the mark of the page in view, the
 * thumbnail (a white page until it is drawn) with a shadow and an edge, and
 * the page's number under it.
 */
static void
draw_thumbnail(
	struct pv_app *app,
	struct pv_canvas *canvas,
	size_t index,
	size_t current)
{
	const struct pv_page *page;
	char number[32];
	int x;
	int y;
	int width;
	int height;
	int label_width;
	int baseline;

	/* Where the thumbnail stands, and the baseline of its number. */
	pv_thumbnail_place(app, index, &x, &y, &width, &height);
	baseline = y + height + 20;

	/* The page in view: a tinted band behind its slot and a blue edge around its thumbnail. */
	if (index == current) {
		pv_canvas_round(canvas, 8, y - 6, canvas->width - 17, height + 34, 8, DRAW_CURRENT);
		pv_canvas_round(canvas, x - 3, y - 3, width + 6, height + 6, 3, DRAW_CURRENT_EDGE);
	}

	/* A soft shadow and a thin edge. */
	pv_canvas_blend(canvas, x - 1, y + 1, width + 2, height + 2, DRAW_SHADOW);
	pv_canvas_blend(canvas, x - 1, y - 1, width + 2, height + 2, DRAW_EDGE);

	/* The thumbnail once drawn, or a white page. */
	page = &app->document.pages[index];
	if (page->thumbnail != NULL) {
		pv_canvas_copy(canvas, x, y, page->thumbnail, page->thumbnail_width, page->thumbnail_height);
	} else {
		pv_canvas_fill(canvas, x, y, width, height, 0xffffffffU);
	}

	/* The page's number, centred under it. */
	snprintf(number, sizeof(number), "%lu", (unsigned long)(index + 1));
	label_width = pv_text_width(app->text, number, 13U);
	pv_text_draw(app->text, canvas, (canvas->width - label_width) / 2, baseline, number, 13U, DRAW_TITLE);
}

/*
 * Draws the password card over the dimmed frame: what it asks, the field
 * with a dot for each character typed and the caret, a word when the last
 * password was refused, and the Cancel and Open buttons.
 */
static void
draw_password(
	struct pv_app *app,
	struct pv_canvas *canvas)
{
	char dots[3 * PV_PASSWORD_MAX + 1];
	char line[PV_PATH_MAX + 64];
	const char *name;
	size_t index;
	int x;
	int y;
	int width;
	int height;
	int field_y;
	int dots_width;
	int open_x;
	int buttons_y;

	/* The frame dimmed, and the card with a soft shadow. */
	pv_canvas_blend(canvas, 0, 0, canvas->width, canvas->height, DRAW_DIM);
	pv_password_layout(app, &x, &y, &width, &height);
	pv_canvas_round(canvas, x + 1, y + 3, width, height, 14, DRAW_SHADOW);
	pv_canvas_round(canvas, x, y, width, height, 14, DRAW_CARD);

	/* A small padlock: its shackle and its body. */
	pv_canvas_round(canvas, x + 24, y + 20, 16, 16, 8, DRAW_LOCK);
	pv_canvas_round(canvas, x + 27, y + 23, 10, 12, 5, DRAW_CARD);
	pv_canvas_round(canvas, x + 21, y + 30, 22, 17, 4, DRAW_LOCK);

	/* What the card asks, and for which file. */
	name = strrchr(app->password_path, '/');
	if (name == NULL) {
		name = app->password_path;
	} else {
		name++;
	}

	/* The title beside the padlock, and the file's name under it. */
	pv_text_draw(app->text, canvas, x + 56, y + 38, "This document is protected", DRAW_TEXT_LARGE, DRAW_TITLE);
	snprintf(line, sizeof(line), "Enter the password to open %s.", name);
	pv_text_draw(app->text, canvas, x + 24, y + 78, line, 14U, DRAW_HINT);

	/* The field: a dot for each character typed, and the caret after them. */
	field_y = y + 94;
	pv_canvas_round(canvas, x + 23, field_y - 1, width - 46, 38, 8, DRAW_FIELD_EDGE);
	pv_canvas_round(canvas, x + 24, field_y, width - 48, 36, 7, DRAW_FIELD);
	dots[0] = '\0';
	for (index = 0; index < app->password_length; index++)
		strcat(dots, "\xe2\x80\xa2");
	dots_width = pv_text_width(app->text, dots, 16U);
	if (dots_width > width - 72)
		dots_width = width - 72;
	pv_text_draw(app->text, canvas, x + 36, field_y + 24, dots, 16U, DRAW_TITLE);
	pv_canvas_fill(canvas, x + 38 + dots_width, field_y + 9, 2, 19, DRAW_CURRENT_EDGE);

	/* The refusal of the last password, in red under the field. */
	if (app->password_wrong)
		pv_text_draw(app->text, canvas, x + 24, field_y + 58, "The password is not correct.", 14U, DRAW_WRONG);

	/* Cancel, and Open, the default, in the bottom right corner. */
	open_x = x + width - PV_PASSWORD_BUTTON_INSET - PV_PASSWORD_BUTTON_WIDTH;
	buttons_y = y + height - PV_PASSWORD_BUTTON_INSET - PV_PASSWORD_BUTTON_HEIGHT;
	draw_button(app, canvas, open_x - 12 - PV_PASSWORD_BUTTON_WIDTH, buttons_y, "Cancel", 0);
	draw_button(app, canvas, open_x, buttons_y, "Open", 1);
}

/* Draws a button of the password card with its label centred; the default button is blue with white words. */
static void
draw_button(
	struct pv_app *app,
	struct pv_canvas *canvas,
	int x,
	int y,
	const char *label,
	int is_default)
{
	uint32_t fill;
	uint32_t ink;
	int label_width;

	/* The colours by the kind of button. */
	fill = DRAW_BUTTON;
	ink = DRAW_TITLE;
	if (is_default) {
		fill = DRAW_BUTTON_DEFAULT;
		ink = draw_accent_ink;
	}

	/* The button and its label. */
	pv_canvas_round(canvas, x, y, PV_PASSWORD_BUTTON_WIDTH, PV_PASSWORD_BUTTON_HEIGHT, 9, fill);
	label_width = pv_text_width(app->text, label, DRAW_TEXT);
	pv_text_draw(app->text, canvas, x + (PV_PASSWORD_BUTTON_WIDTH - label_width) / 2, y + 23, label, DRAW_TEXT, ink);
}

/* Draws a line of text centred across the frame. */
static void
draw_centred(
	struct pv_app *app,
	struct pv_canvas *canvas,
	int baseline,
	const char *text,
	unsigned pixels,
	uint32_t color)
{
	int width;

	/* Its width decides where it starts. */
	width = pv_text_width(app->text, text, pixels);
	pv_text_draw(app->text, canvas, (canvas->width - width) / 2, baseline, text, pixels, color);
}

/*
 * Draws the frame in the dark appearance's colours (dark not 0) or the
 * light one's (ws089-p017).
 */
void
pv_draw_set_dark(
	int dark)
{
	/* The appearance. */
	draw_dark = dark;
}

/*
 * Draws the frame's accent (the current page, the default button, the
 * found text) in the accent the user chose, with its ink, 0xAARRGGBB.
 */
void
pv_draw_set_accent(
	uint32_t accent,
	uint32_t ink)
{
	/* The colours. */
	draw_accent = accent;
	draw_accent_ink = ink;
}

/*
 * Reports the frame's accent, 0xAARRGGBB (find.c tints the found text with
 * it).
 */
uint32_t
pv_draw_accent(void)
{
	/* The accent set last. */
	return draw_accent;
}

/* Chooses a colour of the frame by the appearance. */
static uint32_t
draw_choose(
	uint32_t light,
	uint32_t dark)
{
	/* The dark colour in the dark appearance. */
	if (draw_dark)
		return dark;

	/* The light one otherwise. */
	return light;
}
