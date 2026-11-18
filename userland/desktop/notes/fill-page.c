/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fill tool on a page (ws216-p005, plan/ws216/phase005/phase.md
 * sections 3 and 4): the page drawn off the screen a band at a time at the
 * fill's scale (render.c), the area found from the bands (fill.c), and the
 * fill's stroke put on the page just above what is seen where it was
 * poured, as a change the user can undo.
 *
 * A band is drawn as the page's picture is: the white page, the page of
 * the PDF the notebook writes on when the caller has it drawn, and the
 * strokes that reach the band's rows, through the same pipelines (the
 * pencil's grain and the watercolour's Multiply too).
 */

#include "app.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How far past the band's rows a stroke's box may reach and still be drawn into the band, in points (its soft edge). */
#define FILL_PAGE_MARGIN	1.0f

/*
 * What one fill draws its bands with: the renderer, the page, the
 * function that draws its background's rows (NULL: white paper) and its
 * context, the fill's scale and the page's width in pixels, the frame each
 * band is built in (freed after the fill), and the device's last answer.
 */
struct fill_page {
	struct notes_renderer *renderer;
	const struct notes_page *page;
	notes_fill_background *background;
	void *context;
	double scale;
	uint32_t width;
	struct notes_frame frame;
	VkResult refused;
	uint64_t background_us;
};

static int fill_page_band(void *context, uint32_t top, uint32_t height, unsigned char *pixels);
static int fill_page_stroke(const struct notes_fill_region *region, struct notes_document *document, uint32_t color, struct notes_stroke **made);
static uint64_t fill_page_now(void);
static uint64_t fill_page_now_us(void);

/*
 * Fills the area around a point of a page (points) with a colour
 * (0xRRGGBBAA, laid opaque): finds the area from the page as it is seen
 * and puts the fill's stroke just above the stroke seen at the point (at
 * the bottom when the paper is seen), as a change the user can undo; the
 * journal logs it.
 *
 * background, when not NULL, draws the rows of the page's background
 * (the PDF's page under the strokes) for each band, at the band's scale
 * (ws216-p006); without it the page is white paper.  *place
 * tells where the fill went among the page's strokes.  Returns 0, EINVAL
 * for a point off the page, EEXIST when a fill of the colour is seen at
 * the point already, ENOENT for an area too small, E2BIG for an
 * area too complex to fill (the page is not changed then), ENOMEM, or EIO
 * when the device failed.
 */
int
notes_fill_page(
	struct notes_renderer *renderer,
	struct notes_document *document,
	size_t page,
	float x,
	float y,
	uint32_t color,
	notes_fill_background *background,
	void *context,
	size_t *place)
{
	struct fill_page fill;
	struct notes_fill_region region;
	struct notes_stroke *stroke;
	const struct notes_page *target;
	uint64_t started;
	uint64_t finished;
	size_t seen;
	int found;
	int error;

	/* A page that is not there has nothing to fill. */
	*place = 0;
	if (page >= document->page_count)
		return EINVAL;
	target = document->pages[page];

	/* The bands' context. */
	memset(&fill, 0, sizeof(fill));
	fill.renderer = renderer;
	fill.page = target;
	fill.background = background;
	fill.context = context;
	fill.scale = notes_fill_scale(target->width, target->height);
	fill.width = notes_fill_width(target->width, target->height);
	fill.refused = VK_SUCCESS;

	/*
	 * A fill of the same colour seen at the point already fills it (a second
	 * tap, or a finger's double tap that does not zoom while it writes): it
	 * is told, not filled again (p006 review-1 C-5).
	 */
	found = notes_document_seen_at(document, page, x, y, &seen);
	if (found) {
		stroke = target->strokes[seen];
		if (stroke->tool == NOTES_TOOL_FILL && stroke->color == (color | 0xffU)) {
			printf("NOTES FILL refused reason=same place=%lu\n", (unsigned long)seen);
			fflush(stdout);
			return EEXIST;
		}
	}

	/*
	 * Where it goes: just above the stroke seen at the point, or under
	 * every stroke when the paper is seen.  Only below the strokes that
	 * bound it does it reach further under them (review-1 M-2: above a
	 * stroke it would cover the edges of the lines round it).
	 */
	if (found)
		*place = seen + 1U;

	/* The area, from bands drawn as the flood needs them; the band's objects and frame go after it. */
	started = fill_page_now();
	error = notes_fill_find(target->width, target->height, x, y, !found, fill_page_band, &fill, &region);
	notes_renderer_band_free(renderer);
	notes_frame_free(&fill.frame);
	finished = fill_page_now();
	printf("NOTES FILL bands=%u background_us=%lu\n", region.bands, (unsigned long)fill.background_us);
	fflush(stdout);

	/* An area too complex is refused, with what it would have been. */
	if (error == E2BIG) {
		printf("NOTES FILL refused reason=complex points=%lu bands=%u ms=%lu\n", (unsigned long)region.point_count, region.bands,
		       (unsigned long)(finished - started));
		fflush(stdout);
		return E2BIG;
	}

	/* A point off the page. */
	if (error == EINVAL) {
		printf("NOTES FILL refused reason=outside\n");
		fflush(stdout);
		return EINVAL;
	}

	/* An area too small to hold a fill. */
	if (error == ENOENT) {
		printf("NOTES FILL refused reason=empty\n");
		fflush(stdout);
		return ENOENT;
	}

	/* A device that failed, or memory that ran out. */
	if (error != 0) {
		printf("NOTES FILL refused reason=failed error=%d device=%d\n", error, (int)fill.refused);
		fflush(stdout);
		return error;
	}

	/* The fill's stroke. */
	error = fill_page_stroke(&region, document, color, &stroke);
	if (error != 0) {
		notes_fill_region_free(&region);
		return error;
	}

	/* Puts it on the page as a change to undo. */
	error = notes_document_add_stroke_at(document, page, *place, stroke);
	if (error != 0) {
		notes_stroke_free(stroke);
		notes_fill_region_free(&region);
		return error;
	}

	/* The fill is logged for the tests and the measurements. */
	printf("NOTES FILL made place=%lu contours=%lu points=%lu bands=%u band=%u scale=%.3f pixels=%lu tolerance=%u ms=%lu\n",
	       (unsigned long)*place,
	       (unsigned long)region.contour_count,
	       (unsigned long)region.point_count,
	       region.bands,
	       region.band_height,
	       region.scale,
	       (unsigned long)region.pixels,
	       region.tolerance_step,
	       (unsigned long)(finished - started));
	fflush(stdout);
	notes_fill_region_free(&region);

	/* Succeeded: the page holds the fill. */
	return 0;
}

/*
 * Gives the notice a refused fill shows (the UI's words), or NULL for a
 * fill that was made.
 */
const char *
notes_fill_notice(
	int error)
{
	/* Each reason a fill is refused. */
	switch (error) {
	case 0:
		return NULL;
	case E2BIG:
		return "The area is too complex to fill";
	case EINVAL:
		return "Fill inside the page";
	case ENOENT:
		return "The area is too small to fill";
	case EEXIST:
		return "Already filled";
	default:
		break;
	}

	/* Every other failure. */
	return "Could not fill the area";
}

/*
 * Draws the page's rows from top into the band and reads them back (the
 * band function of notes_fill_find()): the white page, the background,
 * and the strokes whose box reaches the rows, placed with the band's top
 * at the band's first row.
 */
static int
fill_page_band(
	void *context,
	uint32_t top,
	uint32_t height,
	unsigned char *pixels)
{
	struct fill_page *fill;
	struct notes_stroke *stroke;
	struct notes_view view;
	float page_top;
	float page_bottom;
	float page_width;
	float page_height;
	size_t index;
	uint64_t started;
	VkResult result;
	int error;

	/* The page at the fill's scale, its row top at the band's first. */
	fill = context;
	view.x = 0.0f;
	view.y = -(float)top;
	view.scale = (float)fill->scale;
	page_width = fill->page->width * view.scale;
	page_height = fill->page->height * view.scale;

	/* The band's rows in page points, widened by the strokes' soft edge. */
	page_top = (float)top / view.scale - FILL_PAGE_MARGIN;
	page_bottom = (float)(top + height) / view.scale + FILL_PAGE_MARGIN;

	/*
	 * The background's rows, drawn for the band at its scale (ws216-p006);
	 * one that cannot be drawn fails the fill, which would flood past the
	 * PDF's lines on white paper.
	 */
	if (fill->background != NULL) {
		started = fill_page_now_us();
		error = fill->background(fill->context, top, fill->width, height, fill->scale);
		fill->background_us += fill_page_now_us() - started;
		if (error == ENOMEM) {
			/* Memory refused for the band's background: the fill draws a smaller band (review-1 L-5). */
			return ENOMEM;
		} else if (error != 0) {
			printf("NOTES FILL background failed error=%d\n", error);
			fflush(stdout);
			return EIO;
		}
	}

	/* The white page, and the background's rows over it, one to one. */
	notes_frame_begin(&fill->frame);
	notes_frame_rect(&fill->frame, 0.0f, view.y, page_width, page_height, 0xffffffffU);
	if (fill->background != NULL)
		notes_frame_texture(&fill->frame, NOTES_TEXTURE_BACKGROUND, 0.0f, 0.0f, (float)fill->width, (float)height);

	/* The strokes that reach the band, bottom first, clipped to the page. */
	notes_frame_clip(&fill->frame, 1, 0.0f, view.y, page_width, page_height);
	for (index = 0; index < fill->page->stroke_count; index++) {
		stroke = fill->page->strokes[index];
		error = notes_stroke_outline(stroke);
		if (error != 0)
			continue;
		if (stroke->bounds[3] < page_top || stroke->bounds[1] > page_bottom)
			continue;
		notes_frame_stroke(&fill->frame, stroke, &view);
	}

	/* A frame that could not grow fails the fill. */
	if (fill->frame.error != 0)
		return ENOMEM;

	/* The band drawn and read back. */
	result = notes_renderer_band(fill->renderer, &fill->frame, fill->width, height, pixels);
	fill->refused = result;
	if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY) {
		/* A refusal for memory: the fill draws a smaller band. */
		printf("NOTES FILL band refused rows=%u error=%d\n", height, (int)result);
		fflush(stdout);
		return ENOMEM;
	} else if (result != VK_SUCCESS) {
		/* Any other failure of the device ends the fill. */
		return EIO;
	}

	/* Succeeded: the band's pixels. */
	return 0;
}

/*
 * Makes the fill's stroke from an area: its corners the samples, its
 * contours the area's, its colour opaque, the next number of the document
 * (taken when the stroke goes on the page: notes_document_insert_stroke()
 * moves the next number on, p005 code review 5).
 */
static int
fill_page_stroke(
	const struct notes_fill_region *region,
	struct notes_document *document,
	uint32_t color,
	struct notes_stroke **made)
{
	struct notes_stroke *stroke;
	struct notes_point point;
	size_t index;
	int error;

	/* The stroke, with the document's next number. */
	*made = NULL;
	stroke = notes_stroke_create(document->next_id, NOTES_TOOL_FILL, color | 0xffU, NOTES_FILL_WIDTH, 0U);
	if (stroke == NULL)
		return ENOMEM;

	/* Each corner is a sample. */
	memset(&point, 0, sizeof(point));
	for (index = 0; index < region->point_count; index++) {
		point.x = (float)region->points[index].x;
		point.y = (float)region->points[index].y;
		error = notes_stroke_append(stroke, &point);
		if (error != 0) {
			notes_stroke_free(stroke);
			return error;
		}
	}

	/* The contours' corner counts. */
	stroke->contours = malloc(region->contour_count * sizeof(stroke->contours[0]));
	if (stroke->contours == NULL) {
		notes_stroke_free(stroke);
		return ENOMEM;
	}
	memcpy(stroke->contours, region->contours, region->contour_count * sizeof(stroke->contours[0]));
	stroke->contour_count = region->contour_count;

	/* Its shape, which must be made from the contours. */
	error = notes_stroke_outline(stroke);
	if (error != 0) {
		notes_stroke_free(stroke);
		return error;
	}

	/* Succeeded: the stroke for the page. */
	*made = stroke;
	return 0;
}

/* Reads the monotonic clock in microseconds. */
static uint64_t
fill_page_now_us(void)
{
	struct timespec now;
	int error;

	/* The clock, or nothing when it cannot be read. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Reports the microseconds. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/* Reads the monotonic clock in milliseconds. */
static uint64_t
fill_page_now(void)
{
	struct timespec now;
	int error;

	/* The clock, or nothing when it cannot be read. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Reports the milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
