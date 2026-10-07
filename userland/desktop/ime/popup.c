/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The candidate window (ws095-p005, plan/ws095/design.md section 5).
 *
 * The input method draws the candidates of the segment being converted
 * into a wl_shm buffer and attaches it to its input popup surface;
 * the compositor places that surface below the text input's cursor rectangle and
 * composes it over the windows.  A page holds nine candidates, numbered as
 * the digit keys choose them, the one chosen on the accent's tint.  An
 * empty surface (no buffer) hides the window.  The shared memory is made
 * once for two windows of the largest size; each change gets a wl_buffer
 * of the window's own size over one of them, so that compositor takes the
 * window's size (and its shadow) from the buffer.
 */

#include "program.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The fonts: the interface's, and the fallback with the Japanese glyphs. */
#define POPUP_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"
#define POPUP_FALLBACK		KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

/* The candidates a page shows, and the text's size and a row's height. */
#define POPUP_PAGE		9U
#define POPUP_PIXELS		17U
#define POPUP_ROW		30

/* The window's padding, its corners and its width's limits. */
#define POPUP_PADDING		6
#define POPUP_RADIUS		10.0f
#define POPUP_MIN_WIDTH		120
#define POPUP_MAX_WIDTH		480

/* The largest window: its buffers are made once at this size. */
#define POPUP_MAX_HEIGHT	(POPUP_ROW * (int)POPUP_PAGE + 2 * POPUP_PADDING + POPUP_ROW)

/* wl_shm's ARGB8888, premultiplied. */
#define POPUP_FORMAT		0U

/* The colours: the ground, the text, the numbers, the chosen row and the page line. */
#define POPUP_GROUND		KL_RGB(0xffffff)
#define POPUP_TEXT		KL_RGB(0x1e293b)
#define POPUP_NUMBER		KL_RGB(0x94a3b8)
#define POPUP_CHOSEN		KL_RGB(0x2563eb)
#define POPUP_CHOSEN_TEXT	KL_RGB(0xffffff)
#define POPUP_EDGE		KL_RGBA(0x334155, 0x30)

static void popup_buffer_release(void *data, struct wl_buffer *buffer);
static int popup_buffers_make(struct program_popup *popup);
static struct program_popup_buffer *popup_free_buffer(struct program_popup *popup);
static void popup_draw(struct program_popup *popup, struct program_popup_buffer *buffer, const struct ime_output *out, int width, int height);
static int popup_width(struct program_popup *popup, const struct ime_output *out, size_t first, size_t count);

/* The buffer's listener: the compositor gives a buffer back. */
static const struct wl_buffer_listener popup_buffer_listener = {
	popup_buffer_release
};

/*
 * Makes the candidate window's surface and its buffers, and opens its
 * fonts.
 *
 * Returns 0, or an errno value; without a window the input method still
 * converts, only the candidates are not shown.
 */
int
program_popup_start(
	struct program *program)
{
	struct program_popup *popup;
	int error;

	popup = &program->popup;

	/* The compositor's globals the window needs. */
	if (program->compositor == NULL)
		return ENOTSUP;
	if (program->shm == NULL)
		return ENOTSUP;

	/* The fonts. */
	error = kl_text_open(&popup->text, POPUP_FONT, POPUP_FALLBACK);
	if (error != 0)
		return error;

	popup->text_open = 1;

	/* The surface, given the input popup role. */
	popup->surface = wl_compositor_create_surface(program->compositor);
	if (popup->surface == NULL)
		return ENOMEM;

	popup->role = zwp_input_method_v2_get_input_popup_surface(program->method, popup->surface);
	if (popup->role == NULL)
		return ENOMEM;

	/* The buffers, made once at the largest size. */
	popup->shm = program->shm;
	error = popup_buffers_make(popup);
	if (error != 0)
		return error;

	/* Succeeded: the window can show candidates. */
	popup->ready = 1;
	return 0;
}

/*
 * Shows the candidates the engine lists, or hides the window when it lists
 * none.
 */
void
program_popup_update(
	struct program *program,
	const struct ime_output *out)
{
	struct program_popup *popup;
	struct program_popup_buffer *buffer;
	size_t first;
	size_t count;
	int width;
	int height;

	popup = &program->popup;

	/* Nothing without a window. */
	if (!popup->ready)
		return;

	/* No candidates, or no text input served: the window goes. */
	if (!out->candidates_shown ||
	    out->candidate_count == 0U ||
	    !program->active) {
		if (popup->shown) {
			wl_surface_attach(popup->surface, NULL, 0, 0);
			wl_surface_commit(popup->surface);
			popup->shown = 0;
			printf("KEI-IME POPUP hidden\n");
		}

		return;
	}

	/* The page with the candidate chosen. */
	first = (out->candidate_selected / POPUP_PAGE) * POPUP_PAGE;
	count = out->candidate_count - first;
	if (count > POPUP_PAGE)
		count = POPUP_PAGE;

	/* The window's size: the widest candidate, the rows and a line for the page. */
	width = popup_width(popup, out, first, count);
	height = POPUP_ROW * (int)count + 2 * POPUP_PADDING;
	if (out->candidate_count > POPUP_PAGE)
		height += POPUP_ROW;

	/* A buffer the compositor is not reading (none: this change is skipped, the next one shows). */
	buffer = popup_free_buffer(popup);
	if (buffer == NULL)
		return;

	/* A wl_buffer of the window's size over its pixels (the last one, given back, goes). */
	if (buffer->buffer != NULL)
		wl_buffer_destroy(buffer->buffer);
	buffer->buffer = wl_shm_pool_create_buffer(popup->pool, buffer->offset, width, height, POPUP_MAX_WIDTH * 4, POPUP_FORMAT);
	if (buffer->buffer == NULL)
		return;

	(void)wl_buffer_add_listener(buffer->buffer, &popup_buffer_listener, buffer);

	/* Drawn, attached and shown. */
	popup_draw(popup, buffer, out, width, height);
	buffer->busy = 1;
	wl_surface_attach(popup->surface, buffer->buffer, 0, 0);
	wl_surface_damage(popup->surface, 0, 0, width, height);
	wl_surface_commit(popup->surface);
	popup->shown = 1;
	printf("KEI-IME POPUP shown count=%lu selected=%lu width=%d height=%d\n", (unsigned long)out->candidate_count,
	       (unsigned long)out->candidate_selected, width, height);
}

/* Marks a buffer the compositor gave back as free to draw into. */
static void
popup_buffer_release(
	void *data,
	struct wl_buffer *buffer)
{
	struct program_popup_buffer *owned;

	UNUSED_PARAMETER(buffer);

	/* The compositor no longer reads it. */
	owned = data;
	owned->busy = 0;
}

/* Makes the shared memory for two windows of the largest size and the pool over it; 0 or an errno value. */
static int
popup_buffers_make(
	struct program_popup *popup)
{
	struct wl_shm_pool *pool;
	char name[64];
	void *mapped;
	size_t one;
	size_t size;
	unsigned i;
	int descriptor;
	int status;

	/* A shared memory object with a name nobody else uses, unlinked at once. */
	one = (size_t)POPUP_MAX_WIDTH * (size_t)POPUP_MAX_HEIGHT * 4U;
	size = one * PROGRAM_POPUP_BUFFERS;
	snprintf(name, sizeof(name), "/keiland-ime-popup-%ld", (long)getpid());
	descriptor = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0)
		return errno;

	(void)shm_unlink(name);

	/* Its size. */
	status = ftruncate(descriptor, (off_t)size);
	if (status != 0) {
		status = errno;
		close(descriptor);
		return status;
	}

	/* Mapped for drawing. */
	mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
	if (mapped == MAP_FAILED) {
		status = errno;
		close(descriptor);
		return status;
	}

	/* The compositor's pool over it. */
	pool = wl_shm_create_pool(popup->shm, descriptor, (int32_t)size);
	close(descriptor);
	if (pool == NULL) {
		munmap(mapped, size);
		return ENOMEM;
	}

	/* Each window's place in it; its wl_buffer is made at each change, at the window's size. */
	for (i = 0; i < PROGRAM_POPUP_BUFFERS; i++) {
		popup->buffers[i].pixels = (uint32_t *)((char *)mapped + one * i);
		popup->buffers[i].offset = (int32_t)(one * i);
		popup->buffers[i].buffer = NULL;
		popup->buffers[i].busy = 0;
	}

	/* Succeeded: the pool is kept for the buffers. */
	popup->pool = pool;
	return 0;
}

/* Gives a buffer the compositor is not reading, or NULL when it reads both. */
static struct program_popup_buffer *
popup_free_buffer(
	struct program_popup *popup)
{
	unsigned i;

	/* The first one free. */
	for (i = 0; i < PROGRAM_POPUP_BUFFERS; i++) {
		if (!popup->buffers[i].busy)
			return &popup->buffers[i];
	}

	/* Both are the compositor's still. */
	return NULL;
}

/*
 * Draws a page of candidates into a buffer: a white rounded card with a
 * faint edge, each candidate with its digit, the chosen one on the
 * accent, and "page / pages" under them when there are more.
 */
static void
popup_draw(
	struct program_popup *popup,
	struct program_popup_buffer *buffer,
	const struct ime_output *out,
	int width,
	int height)
{
	struct kl_canvas canvas;
	struct kl_rect clip;
	char number[8];
	char page[32];
	size_t first;
	size_t index;
	size_t row;
	kl_color color;
	int baseline;
	int top;
	int status;
	int page_width;

	/* A canvas over the buffer's top left, the size of the window, cleared to transparent. */
	status = kl_canvas_init(&canvas, buffer->pixels, POPUP_MAX_WIDTH, POPUP_MAX_WIDTH, POPUP_MAX_HEIGHT);
	if (status != 0)
		return;

	clip.x = 0;
	clip.y = 0;
	clip.width = width;
	clip.height = height;
	kl_canvas_clip_push(&canvas, &clip);
	kl_canvas_clear(&canvas);

	/* The card and its edge. */
	kl_canvas_round(&canvas, 0.0f, 0.0f, (float)width, (float)height, POPUP_RADIUS, POPUP_GROUND);
	kl_canvas_round_border(&canvas, 0.5f, 0.5f, (float)width - 1.0f, (float)height - 1.0f, POPUP_RADIUS, 1.0f, POPUP_EDGE);

	/* Each candidate of the page with its digit. */
	first = (out->candidate_selected / POPUP_PAGE) * POPUP_PAGE;
	for (row = 0; row < POPUP_PAGE; row++) {
		index = first + row;
		if (index >= out->candidate_count)
			break;

		/* The chosen one on the accent. */
		top = POPUP_PADDING + POPUP_ROW * (int)row;
		color = POPUP_TEXT;
		if (index == out->candidate_selected) {
			kl_canvas_round(&canvas, 4.0f, (float)top + 1.0f, (float)width - 8.0f, (float)POPUP_ROW - 2.0f, 6.0f, POPUP_CHOSEN);
			color = POPUP_CHOSEN_TEXT;
		}

		/* The digit, then the candidate. */
		baseline = kl_text_center(POPUP_PIXELS, top, POPUP_ROW);
		snprintf(number, sizeof(number), "%lu", (unsigned long)(row + 1U));
		if (index == out->candidate_selected) {
			(void)kl_text_draw(&popup->text, &canvas, 14, baseline, number, strlen(number), POPUP_PIXELS - 3U, 0, color);
		} else {
			(void)kl_text_draw(&popup->text, &canvas, 14, baseline, number, strlen(number), POPUP_PIXELS - 3U, 0, POPUP_NUMBER);
		}

		(void)kl_text_draw(&popup->text, &canvas, 36, baseline, out->candidates[index], strlen(out->candidates[index]), POPUP_PIXELS, 0,
				    color);
	}

	/* The page line, when the candidates take more than one page. */
	if (out->candidate_count > POPUP_PAGE) {
		snprintf(page, sizeof(page), "%lu / %lu", (unsigned long)(first / POPUP_PAGE + 1U),
			 (unsigned long)((out->candidate_count + POPUP_PAGE - 1U) / POPUP_PAGE));
		page_width = kl_text_width(&popup->text, page, strlen(page), POPUP_PIXELS - 4U, 0);
		baseline = kl_text_center(POPUP_PIXELS - 4U, height - POPUP_PADDING - POPUP_ROW, POPUP_ROW);
		(void)kl_text_draw(&popup->text, &canvas, width - 14 - page_width, baseline, page, strlen(page), POPUP_PIXELS - 4U, 0, POPUP_NUMBER);
	}

	kl_canvas_clip_pop(&canvas);
	kl_canvas_release(&canvas);
}

/* Gives the window's width: the widest candidate of the page, its digit and the padding, within the limits. */
static int
popup_width(
	struct program_popup *popup,
	const struct ime_output *out,
	size_t first,
	size_t count)
{
	size_t row;
	int widest;
	int width;

	/* The widest candidate of the page. */
	widest = 0;
	for (row = 0; row < count; row++) {
		width = kl_text_width(&popup->text, out->candidates[first + row], strlen(out->candidates[first + row]), POPUP_PIXELS, 0);
		if (width > widest)
			widest = width;
	}

	/* The digit's column, the candidate and the padding, within the limits. */
	width = 36 + widest + 20;
	if (width < POPUP_MIN_WIDTH)
		width = POPUP_MIN_WIDTH;
	if (width > POPUP_MAX_WIDTH)
		width = POPUP_MAX_WIDTH;

	/* The width. */
	return width;
}
