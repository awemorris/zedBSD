/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frame of a page that is not ready yet: its picture and name on a
 * card, and a line saying it comes in a later version of the system.
 */

#include "settings.h"

#include "include/kern/os-name.h"

#include <string.h>

/* The card's height, the picture's size, and the texts' sizes. */
#define SOON_HEIGHT		220
#define SOON_GLYPH		56.0f
#define SOON_TEXT_NAME		20U
#define SOON_TEXT_LINE		14U

/* The line every such page shows. */
#define SOON_MESSAGE		"This page is coming in a later version of " ZEDBSD_OS_NAME "."

/*
 * Draws the card of a page that is not ready from a top edge; returns the
 * edge below it.
 */
int
se_soon_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_page *page;
	struct kl_text_line name;
	struct kl_text_line line;
	int centre;
	int text_width;
	int baseline;
	int glyph_top;

	/* The page shown, and the card. */
	page = &se_pages[app->page];
	(void)se_card_begin(app, canvas, x, top, width, SOON_HEIGHT, NULL, NULL);

	/* The picture, faint, in the middle of the card's upper part. */
	centre = x + width / 2;
	glyph_top = top + 34;
	se_glyph_draw(canvas, page->glyph, (float)centre - SOON_GLYPH * 0.5f, (float)glyph_top, SOON_GLYPH, SE_COLOR_TEXT_FAINT);

	/* The name under it, centred. */
	kl_text_metrics(app->text, SOON_TEXT_NAME, &name);
	baseline = glyph_top + (int)SOON_GLYPH + 18 + name.ascent;
	text_width = kl_text_width(app->text, page->name, strlen(page->name), SOON_TEXT_NAME, 1);
	(void)kl_text_draw(app->text, canvas, centre - text_width / 2, baseline, page->name, strlen(page->name), SOON_TEXT_NAME, 1, SE_COLOR_TEXT);

	/* The line under that, centred. */
	kl_text_metrics(app->text, SOON_TEXT_LINE, &line);
	baseline += name.descent + 8 + line.ascent;
	text_width = kl_text_width(app->text, SOON_MESSAGE, strlen(SOON_MESSAGE), SOON_TEXT_LINE, 0);
	(void)kl_text_draw(app->text, canvas, centre - text_width / 2, baseline, SOON_MESSAGE, strlen(SOON_MESSAGE), SOON_TEXT_LINE, 0, SE_COLOR_TEXT_SECONDARY);

	/* The edge below the card. */
	return top + SOON_HEIGHT;
}
