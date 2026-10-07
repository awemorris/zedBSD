/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text box of Notes (ws175-p008 with ws079-p017): the box the words of
 * a line of the PDF are changed in, or the words put on a page are typed
 * in, over the page.
 *
 * It is libkeiland's widget (kl_field for a line's words, kl_text_area for
 * the words put on a page, several lines), drawn by kl_ui on a canvas over
 * the overlay picture of the renderer, which is the window's size, so that
 * the widget's places are the window's: the pointer's input reaches it
 * where the window has it, and the caret's rectangle it gives the input
 * method is the window's.  The window's inputs come to it through
 * kl_ui_window_input while it is open (window.c queues them), and after
 * each of its frames kl_ui_window_text asks for the text input while the
 * widget has the keyboard, so that an input method composes in it and the
 * on-screen keyboard types in it.
 *
 * The canvas is premultiplied; the overlay picture is drawn with straight
 * alpha, so the box's pixels are turned to straight alpha once drawn.  The
 * box's own font is opened here (the desktop's font, with the CJK font as
 * the fallback for an input method's words), apart from the toolbar's.
 */

#include "app.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The box's fonts: the desktop's, and the one with the CJK characters. */
#define BOX_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"
#define BOX_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

/* The fonts tried in turn when the desktop's is missing (ws177-p012): the monospaced, then the fallbacks. */
#define BOX_SECOND_FONT		KEILAND_DATADIR "/fonts/keiland-mono.ttf"
#define BOX_THIRD_FONT		KEILAND_DATADIR "/fonts/keiland-fallback-mono.ttf"

/* The widget's id among the box's. */
#define BOX_WIDGET		1U

/* The room around the box its shadow takes, the shadow's softness and its corner, in pixels. */
#define BOX_MARGIN		16
#define BOX_SHADOW_SOFT		10.0f
#define BOX_RADIUS		8.0f

/* The shadow's colour: a slate, translucent. */
#define BOX_SHADOW_COLOR	KL_RGBA(0x1f3a66U, 64U)

static int box_font(struct notes_box *box);
static int box_canvas(struct notes_box *box, struct notes_renderer *renderer);
static void box_clear(struct notes_box *box, const struct kl_rect *rect);
static void box_straight(struct notes_box *box, const struct kl_rect *rect);
static void box_outer(const struct notes_box *box, struct kl_rect *outer);

/*
 * Opens the box in a rectangle of the window, of a shape (NOTES_BOX_LINE or
 * NOTES_BOX_AREA), with words in it, the caret at their end and the
 * keyboard its.  Returns 0, ENOMEM, or the failure of opening its font.
 */
int
notes_box_open(
	struct notes_box *box,
	unsigned shape,
	const struct kl_rect *rect,
	const char *text)
{
	int error;

	/* The widgets' state, made once. */
	if (box->ui == NULL) {
		box->ui = kl_ui_create();
		if (box->ui == NULL)
			return ENOMEM;
	}

	/*
	 * The font, opened once: the desktop's, else the next font the system
	 * has (ws177-p012: a missing keiland.ttf does not keep the box shut);
	 * without any the failure is told again each time.
	 */
	if (!box->font_ready) {
		error = box_font(box);
		if (error != 0)
			return error;
		box->font_ready = 1;
	}

	/* The shape, the place and the words, first as they were. */
	box->shape = shape;
	box->rect = *rect;
	kl_field_set(&box->field, text);
	kl_text_area_set(&box->area, text);
	(void)snprintf(box->initial, sizeof(box->initial), "%s", text);
	box->reported = 0;
	box->again = 0;

	/* Succeeded: open, with the keyboard. */
	kl_ui_set_focus(box->ui, BOX_WIDGET, 0U);
	box->open = 1;
	return 0;
}

/*
 * Closes the box (the overlay is no longer drawn; what it shows is
 * cleared when it is drawn next).
 */
void
notes_box_close(
	struct notes_box *box)
{
	/* Closed, without the keyboard, wanting no frame. */
	box->open = 0;
	box->again = 0;
	if (box->ui != NULL)
		kl_ui_clear_focus(box->ui);
}

/* Gives one input of the window to the box's widget. */
void
notes_box_input(
	struct notes_box *box,
	const struct kl_window_event *event)
{
	/* Only while it is open. */
	if (!box->open || box->ui == NULL)
		return;

	/* The widget's, through libkeiland. */
	(void)kl_ui_window_input(box->ui, event);
}

/*
 * Draws the box on the overlay and takes the input given to it since its
 * last frame; then asks the window for its text input while the widget has
 * the keyboard (with the caret's place), or ends it.  What the widget
 * reported waits for notes_box_take.  Returns 0, or the renderer's or the
 * canvas's failure.
 */
int
notes_box_draw(
	struct notes_box *box,
	struct notes_renderer *renderer,
	struct kl_window *window,
	uint64_t now_us)
{
	struct kl_style style;
	struct kl_event event;
	struct kl_rect outer;
	unsigned reported;
	int taken;
	int error;

	/* Only while it is open. */
	if (!box->open || box->ui == NULL)
		return 0;

	/* The canvas over the overlay, at the window's size; what the box showed before goes. */
	error = box_canvas(box, renderer);
	if (error != 0)
		return error;
	box_clear(box, &box->drawn);

	/* Its style: its canvas, its font and the desktop's theme. */
	memset(&style, 0, sizeof(style));
	style.canvas = &box->canvas;
	style.text = &box->text;
	style.theme = kl_theme_default();
	style.glass = 0;

	/* Its shadow, then the widget. */
	kl_canvas_shadow(&box->canvas, (float)box->rect.x, (float)box->rect.y, (float)box->rect.width, (float)box->rect.height, BOX_RADIUS,
			 BOX_SHADOW_SOFT, BOX_SHADOW_COLOR);
	kl_ui_begin(box->ui, now_us);
	if (box->shape == NOTES_BOX_AREA)
		reported = kl_text_area(box->ui, &style, BOX_WIDGET, &box->rect, &box->area, "Type here");
	else
		reported = kl_field(box->ui, &style, BOX_WIDGET, &box->rect, &box->field, "Type the line");
	box->again = kl_ui_end(box->ui, now_us);
	box->reported |= reported;

	/* What no part took is nothing (a press outside is the main loop's, which closes the box). */
	for (;;) {
		taken = kl_ui_take(box->ui, &event);
		if (taken == 0)
			break;
	}

	/* The pixels in straight alpha, the place they cover kept for the next frame. */
	box_outer(box, &outer);
	box_straight(box, &outer);
	box->drawn = outer;

	/* The text input while the widget has the keyboard, at its caret. */
	kl_ui_window_text(box->ui, window);
	return 0;
}

/* Takes what the widget reported since last taken (KL_FIELD_*). */
unsigned
notes_box_take(
	struct notes_box *box)
{
	unsigned reported;

	/* The bits, then none. */
	reported = box->reported;
	box->reported = 0;
	return reported;
}

/* Gives the box's words (UTF-8). */
const char *
notes_box_text(
	const struct notes_box *box)
{
	/* The area's, or the field's. */
	if (box->shape == NOTES_BOX_AREA)
		return box->area.text;
	return box->field.text;
}

/* Gives the words the box was opened with. */
const char *
notes_box_initial(
	const struct notes_box *box)
{
	/* As they were. */
	return box->initial;
}

/* Puts the words back as the box was opened with (an undo before the box is done). */
void
notes_box_revert(
	struct notes_box *box)
{
	/* The first words, the caret at their end. */
	kl_field_set(&box->field, box->initial);
	kl_text_area_set(&box->area, box->initial);
}

/* Gives the keyboard back to the box's widget (after a press outside it that did not close it). */
void
notes_box_focus(
	struct notes_box *box)
{
	/* While it is open. */
	if (box->open && box->ui != NULL)
		kl_ui_set_focus(box->ui, BOX_WIDGET, 0U);
}

/* Tells whether a point of the window is on the box. */
int
notes_box_hit(
	const struct notes_box *box,
	float x,
	float y)
{
	/* Only while it is open. */
	if (!box->open)
		return 0;

	/* Inside its rectangle. */
	if (x < (float)box->rect.x || y < (float)box->rect.y)
		return 0;
	if (x >= (float)(box->rect.x + box->rect.width) || y >= (float)(box->rect.y + box->rect.height))
		return 0;
	return 1;
}

/*
 * Releases what the box holds: its widgets' state, its font and its
 * canvas.
 */
void
notes_box_free(
	struct notes_box *box)
{
	/* Each, where made. */
	if (box->ui != NULL)
		kl_ui_destroy(box->ui);
	box->ui = NULL;
	if (box->font_ready)
		kl_text_close(&box->text);
	box->font_ready = 0;
	if (box->canvas_ready)
		kl_canvas_release(&box->canvas);
	box->canvas_ready = 0;
	box->open = 0;
}

/*
 * Opens the box's font: the desktop's with the CJK fallback, else the
 * first of the others the system has (the CJK font itself last).
 * Returns 0, or the desktop font's failure when none opens.
 */
static int
box_font(
	struct notes_box *box)
{
	static const char *const others[] = { BOX_SECOND_FONT, BOX_THIRD_FONT, BOX_FALLBACK_FONT };
	size_t index;
	int first;
	int error;

	/* The desktop's font, with the CJK one for an input method's words. */
	first = kl_text_open(&box->text, BOX_FONT, BOX_FALLBACK_FONT);
	if (first == 0)
		return 0;

	/* Each other font in turn, with the CJK one too. */
	for (index = 0; index < sizeof(others) / sizeof(others[0]); index++) {
		error = kl_text_open(&box->text, others[index], BOX_FALLBACK_FONT);
		if (error != 0)
			continue;

		/* The tests' line: which font stands in. */
		printf("NOTES TEXT box font=%s error=%d\n", others[index], first);
		fflush(stdout);

		/* Succeeded: the box draws in that font. */
		return 0;
	}

	/* No font: the desktop font's failure. */
	return first;
}

/*
 * Makes the canvas over the overlay picture at the window's size, again
 * when the picture was made again or its pixels moved.  Returns 0, or the
 * renderer's or the canvas's failure.
 */
static int
box_canvas(
	struct notes_box *box,
	struct notes_renderer *renderer)
{
	unsigned char *pixels;
	size_t pitch;
	int made;
	int error;

	/* The overlay at the window's size. */
	error = (int)notes_renderer_overlay(renderer, renderer->extent.width, renderer->extent.height, &pixels, &pitch, &made);
	if (error != (int)VK_SUCCESS || pixels == NULL)
		return EIO;

	/* A canvas over the same pixels stands. */
	if (box->canvas_ready && !made && box->canvas.pixels == (uint32_t *)(void *)pixels && box->canvas.stride == pitch / 4U)
		return 0;

	/* A new one; a new picture shows nothing yet. */
	if (box->canvas_ready)
		kl_canvas_release(&box->canvas);
	box->canvas_ready = 0;
	error = kl_canvas_init(&box->canvas, (uint32_t *)(void *)pixels, pitch / 4U, (int)renderer->extent.width, (int)renderer->extent.height);
	if (error != 0)
		return error;

	/* Succeeded: nothing drawn on it yet. */
	box->canvas_ready = 1;
	memset(&box->drawn, 0, sizeof(box->drawn));
	return 0;
}

/* Makes a rectangle of the canvas transparent (inside the canvas only). */
static void
box_clear(
	struct notes_box *box,
	const struct kl_rect *rect)
{
	uint32_t *row;
	int left;
	int top;
	int right;
	int bottom;
	int y;

	/* The rectangle inside the canvas. */
	left = rect->x;
	top = rect->y;
	right = rect->x + rect->width;
	bottom = rect->y + rect->height;
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > box->canvas.width)
		right = box->canvas.width;
	if (bottom > box->canvas.height)
		bottom = box->canvas.height;
	if (right <= left || bottom <= top)
		return;

	/* Each row's part, every byte zero. */
	for (y = top; y < bottom; y++) {
		row = box->canvas.pixels + (size_t)y * box->canvas.stride;
		memset(row + left, 0, (size_t)(right - left) * sizeof(*row));
	}
}

/* Turns a rectangle's pixels from premultiplied to straight alpha (inside the canvas only). */
static void
box_straight(
	struct notes_box *box,
	const struct kl_rect *rect)
{
	uint32_t *row;
	uint32_t pixel;
	uint32_t alpha;
	uint32_t red;
	uint32_t green;
	uint32_t blue;
	int left;
	int top;
	int right;
	int bottom;
	int x;
	int y;

	/* The rectangle inside the canvas. */
	left = rect->x;
	top = rect->y;
	right = rect->x + rect->width;
	bottom = rect->y + rect->height;
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > box->canvas.width)
		right = box->canvas.width;
	if (bottom > box->canvas.height)
		bottom = box->canvas.height;

	/* Each pixel that is neither clear nor opaque. */
	for (y = top; y < bottom; y++) {
		row = box->canvas.pixels + (size_t)y * box->canvas.stride;
		for (x = left; x < right; x++) {
			pixel = row[x];
			alpha = pixel >> 24;
			if (alpha == 0U || alpha == 255U)
				continue;

			/* Each colour divided by the alpha, at most full. */
			red = ((pixel >> 16) & 0xffU) * 255U / alpha;
			green = ((pixel >> 8) & 0xffU) * 255U / alpha;
			blue = (pixel & 0xffU) * 255U / alpha;
			if (red > 255U)
				red = 255U;
			if (green > 255U)
				green = 255U;
			if (blue > 255U)
				blue = 255U;
			row[x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
		}
	}
}

/* Gives the rectangle the box and its shadow cover. */
static void
box_outer(
	const struct notes_box *box,
	struct kl_rect *outer)
{
	/* The box with its margin on every side. */
	outer->x = box->rect.x - BOX_MARGIN;
	outer->y = box->rect.y - BOX_MARGIN;
	outer->width = box->rect.width + 2 * BOX_MARGIN;
	outer->height = box->rect.height + 2 * BOX_MARGIN;
}
