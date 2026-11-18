/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The media picker (ws197-p014): the window every application shares to
 * choose photos and videos of the user's media library.
 *
 * It is made as the file chooser is (chooser.c): a toplevel window of its
 * own on the application's connection (keiui_window_open_shared), drawn
 * with the widgets on the CPU into wl_shm buffers, on the compositor's
 * glass where it has glass (one frosted panel under the window and the
 * card on it) and on Files' pale ground elsewhere, a sheet under the
 * parent's title bar where the compositor has sheets.  Its objects live on
 * the application's default queue, so its input, configures and frames
 * run while the application dispatches that queue.  The window wakes the
 * picker after the events that queued its input; the picker draws at most
 * once a frame and keeps asking for frames while the grid glides or
 * thumbnails in sight are still being made by its thread.
 *
 * The list is the library's metadata, asked for once when the picker
 * opens without waiting for it (kl_system_media_list_begin, ws157-p014):
 * the window shows "Loading…" and polls the answer at each frame until it
 * comes; the picker never reads the library's database.
 * The answer is told through the listener from a wl_display.sync callback
 * after the window is gone, so that the application may destroy the picker
 * from its callback.
 */

#include "picker.h"
#include "window.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The corner radius of the window's panel (the compositor's windows') and of the card on it. */
#define PICKER_GLASS_RADIUS	14
#define PICKER_CARD_RADIUS	16

/* The window's panel and the card. */
#define PICKER_PANELS		2U

/* The fonts used unless the application names others. */
#define PICKER_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"
#define PICKER_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

/*
 * One media picker: the model, the text, the window and its input, the
 * frame being drawn, and who is told the answer.  It lives from
 * kl_media_picker_open to kl_media_picker_destroy; the window goes when the
 * answer is known.
 */
struct kl_media_picker {
	struct keiui_picker model;
	int model_made;
	struct kl_text text;
	int text_open;

	/* The application's connection, the window (NULL once gone) and the input of its frames. */
	struct wl_display *display;
	struct kl_window *window;
	struct kl_ui *ui;
	/* Its titlebar in the sheet mode, hung under the parent's title bar (NULL for a window of its own). */
	struct kl_titlebar *sheet;
	/* The compositor's glass under the window (NULL when opaque), and the panels it was last given. */
	struct kl_glass *glass;
	struct kl_glass_panel panels[PICKER_PANELS];
	size_t panel_count;

	/* The frame: its pixels and size, the canvas over them, the style, the frame asked for, and whether another is due. */
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	struct kl_canvas canvas;
	int canvas_made;
	struct kl_style style;
	struct wl_callback *frame;
	int dirty;

	/* The answer's delivery, and who hears it. */
	struct wl_callback *telling;
	const struct kl_media_picker_listener *listener;
	void *data;

	/* The system object the list is asked of until it comes (NULL after), and the request. */
	struct kl_system *system;
	uint32_t list_request;
};

static void picker_list_begin(struct kl_media_picker *picker);
static void picker_list_poll(struct kl_media_picker *picker);
static void picker_list_end(struct kl_media_picker *picker);
static void picker_woken(void *data);
static void picker_sheet(struct kl_media_picker *picker);
static void picker_glass(struct kl_media_picker *picker);
static void picker_glass_panels(struct kl_media_picker *picker);
static void picker_draw(struct kl_media_picker *picker);
static int picker_canvas(struct kl_media_picker *picker);
static void picker_answered(struct kl_media_picker *picker);
static void picker_window_gone(struct kl_media_picker *picker);
static void picker_frame_done(void *data, struct wl_callback *callback, uint32_t time);
static void picker_tell_done(void *data, struct wl_callback *callback, uint32_t time);

/* The frame asked for is due. */
static const struct wl_callback_listener picker_frame_listener = {
	picker_frame_done
};

/* The answer can be told. */
static const struct wl_callback_listener picker_tell_listener = {
	picker_tell_done
};

/*
 * Opens a media picker over an application's window.
 */
struct kl_media_picker *
kl_media_picker_open(
	struct wl_display *display,
	struct xdg_toplevel *parent,
	const struct kl_media_picker_options *options,
	const struct kl_media_picker_listener *listener,
	void *data)
{
	struct kl_window_options window_options;
	struct kl_media_picker *picker;
	const char *application;
	const char *font;
	const char *fallback;
	int error;

	/* A display and someone to tell the answer. */
	if (display == NULL || listener == NULL || listener->done == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* The record. */
	picker = calloc(1, sizeof(*picker));
	if (picker == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* The connection and who hears the answer. */
	picker->display = display;
	picker->listener = listener;
	picker->data = data;

	/* The model, which checks the options. */
	error = keiui_picker_init(&picker->model, options);
	picker->model_made = 1;
	if (error != 0) {
		kl_media_picker_destroy(picker);
		errno = error;
		return NULL;
	}

	/* The library's items asked for through the compositor; the window shows "Loading…" until they come. */
	picker_list_begin(picker);

	/* The fonts, the application's or the system's, and the application's name. */
	font = PICKER_FONT;
	fallback = PICKER_FALLBACK_FONT;
	application = NULL;
	if (options != NULL) {
		if (options->font != NULL)
			font = options->font;
		if (options->fallback_font != NULL)
			fallback = options->fallback_font;
		application = options->application;
	}

	/* The text. */
	error = kl_text_open(&picker->text, font, fallback);
	if (error != 0) {
		kl_media_picker_destroy(picker);
		errno = error;
		return NULL;
	}

	/* Opened: destroy closes it. */
	picker->text_open = 1;

	/* The input of the frames. */
	picker->ui = kl_ui_create();
	if (picker->ui == NULL) {
		kl_media_picker_destroy(picker);
		errno = ENOMEM;
		return NULL;
	}

	/* The window on the application's connection; its first configure arrives with the application's next dispatch. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = picker->model.title;
	window_options.application = application;
	window_options.width = KEIUI_PICKER_WIDTH;
	window_options.height = KEIUI_PICKER_HEIGHT;
	window_options.present = KL_PRESENT_SHM;
	picker->window = keiui_window_open_shared(display, parent, &window_options, KEIUI_PICKER_MIN_WIDTH, KEIUI_PICKER_MIN_HEIGHT);
	if (picker->window == NULL) {
		error = errno;
		kl_media_picker_destroy(picker);
		errno = error;
		return NULL;
	}

	/* It wakes the picker for its input. */
	keiui_window_set_notify(picker->window, picker_woken, picker);

	/* Over a parent it is a sheet under the parent's title bar where the compositor has sheets. */
	if (parent != NULL)
		picker_sheet(picker);

	/* The style: on glass where the compositor has it, else opaque. */
	picker->style.text = &picker->text;
	picker->style.theme = kl_theme_default();
	picker_glass(picker);

	/* Succeeded: the picker shows itself once configured. */
	(void)wl_display_flush(display);
	return picker;
}

/*
 * Closes a picker; one still open closes without telling.
 */
void
kl_media_picker_destroy(
	struct kl_media_picker *picker)
{
	/* No picker, nothing to close. */
	if (picker == NULL)
		return;

	/* The list not waited for any more, the window, and the answer not told any more. */
	picker_list_end(picker);
	picker_window_gone(picker);
	if (picker->telling != NULL)
		wl_callback_destroy(picker->telling);

	/* The input, the frame and the fonts. */
	kl_ui_destroy(picker->ui);
	if (picker->canvas_made)
		kl_canvas_release(&picker->canvas);
	free(picker->pixels);
	if (picker->text_open)
		kl_text_close(&picker->text);

	/* The model last: its thread ends after the thumbnail it makes. */
	if (picker->model_made)
		keiui_picker_fini(&picker->model);
	free(picker);
}

/*
 * Asks for the library's metadata through a system object of its own on
 * the application's connection, without waiting; a library that cannot be
 * asked shows as unavailable.
 */
static void
picker_list_begin(
	struct kl_media_picker *picker)
{
	int error;

	/* Loading until the answer comes. */
	picker->model.loading = 1;

	/* The compositor's system extension; without it there is no library. */
	picker->system = kl_system_open(picker->display);
	if (picker->system == NULL) {
		picker->model.loading = 0;
		picker->model.unavailable = 1;
		return;
	}

	/* The snapshot of the metadata asked for, never the photos' bytes. */
	error = kl_system_media_list_begin(picker->system, &picker->list_request);
	if (error != 0) {
		picker_list_end(picker);
		picker->model.loading = 0;
		picker->model.unavailable = 1;
	}
}

/*
 * Takes the list's answer when it came: the items loaded and their
 * thumbnails' thread started, or the library shown unavailable.
 */
static void
picker_list_poll(
	struct kl_media_picker *picker)
{
	uint32_t request;
	unsigned changed;
	int descriptor;
	int status;
	int error;
	int taken;

	/* Nothing asked for any more. */
	if (picker->system == NULL)
		return;

	/* The compositor's answers; a compositor gone sends none. */
	changed = 0U;
	status = kl_system_dispatch(picker->system, &changed);
	if (status != 0) {
		picker_list_end(picker);
		picker->model.loading = 0;
		picker->model.unavailable = 1;
		return;
	}

	/* The list's answer, any other dropped. */
	for (;;) {
		taken = kl_system_media_take(picker->system, &request, &error, &descriptor);
		if (!taken)
			return;
		if (request != picker->list_request) {
			if (descriptor >= 0)
				(void)close(descriptor);
			continue;
		}

		/* The items, then the thread that makes their thumbnails (waiting until items show). */
		if (error == 0) {
			error = keiui_picker_load(&picker->model, descriptor);
			(void)close(descriptor);
		}

		/* The thread, once the items are there. */
		if (error == 0)
			error = keiui_picker_start(&picker->model);

		/* Loaded, or unavailable; the system object is no longer needed. */
		picker->list_request = 0U;
		picker_list_end(picker);
		picker->model.loading = 0;
		if (error != 0)
			picker->model.unavailable = 1;
		return;
	}
}

/* Gives up the list's request (its answer is dropped) and closes the system object. */
static void
picker_list_end(
	struct kl_media_picker *picker)
{
	/* Nothing open. */
	if (picker->system == NULL)
		return;

	/* The request, then the object. */
	if (picker->list_request != 0U)
		kl_system_media_cancel(picker->system, picker->list_request);
	kl_system_close(picker->system);
	picker->system = NULL;
	picker->list_request = 0U;
}

/* The window's input is queued, its configure came or a buffer came back: carries out the input and draws. */
static void
picker_woken(
	void *data)
{
	struct kl_media_picker *picker;
	struct kl_window_event event;
	int taken;

	/* Each input queued, given to the widgets; the close button cancels. */
	picker = data;
	for (;;) {
		taken = kl_window_take(picker->window, &event);
		if (!taken)
			break;
		picker->dirty = 1;
		taken = kl_ui_window_input(picker->ui, &event);
		if (!taken && event.kind == KL_WINDOW_CLOSE)
			keiui_picker_cancel(&picker->model);
	}

	/* An answer closes the window. */
	if (picker->model.answered) {
		picker_answered(picker);
		return;
	}

	/* A frame now, unless one is asked for already (it draws then). */
	if (picker->frame == NULL)
		picker_draw(picker);
}

/* Draws a frame when one is due and shows it, asking for the next while something moves. */
static void
picker_draw(
	struct kl_media_picker *picker)
{
	uint32_t width;
	uint32_t height;
	int again;
	int status;

	/* Nothing before the first configure, and nothing once the window is gone. */
	if (picker->window == NULL || !picker->window->configured)
		return;

	/* The frame's size: the window's (a new canvas when it changed). */
	(void)kl_window_present_resize(picker->window, &width, &height);
	if (width != picker->width || height != picker->height || !picker->canvas_made) {
		picker->width = width;
		picker->height = height;
		status = picker_canvas(picker);
		if (status != 0)
			return;
	}

	/* The library's list, when it came. */
	picker_list_poll(picker);

	/* The frame and its input. */
	again = keiui_picker_frame(&picker->model, picker->ui, &picker->style, (int)picker->width, (int)picker->height, kl_clock_us());
	picker->dirty = again;

	/* An answer closes the window. */
	if (picker->model.answered) {
		picker_answered(picker);
		return;
	}

	/* The next frame asked for while something moves or thumbnails are coming. */
	if (again && picker->frame == NULL) {
		picker->frame = wl_surface_frame(kl_window_surface(picker->window));
		if (picker->frame != NULL)
			(void)wl_callback_add_listener(picker->frame, &picker_frame_listener, picker);
	}

	/* The glass panels for the frame's size, then the frame shown (drawn again when a buffer the compositor still reads comes back). */
	picker_glass_panels(picker);
	status = kl_window_present(picker->window, picker->pixels, (size_t)picker->width);
	if (status != 0)
		picker->dirty = 1;
	(void)wl_display_flush(picker->display);
}

/* Makes the frame's memory and canvas at its size; nonzero when memory runs out. */
static int
picker_canvas(
	struct kl_media_picker *picker)
{
	uint32_t *pixels;
	size_t count;
	int status;

	/* The frame's memory. */
	count = (size_t)picker->width * (size_t)picker->height;
	pixels = malloc(count * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* The canvas over it, in place of the old one. */
	if (picker->canvas_made)
		kl_canvas_release(&picker->canvas);
	picker->canvas_made = 0;
	free(picker->pixels);
	picker->pixels = pixels;
	status = kl_canvas_init(&picker->canvas, picker->pixels, (size_t)picker->width, (int)picker->width, (int)picker->height);
	if (status != 0)
		return -1;
	picker->canvas_made = 1;
	picker->style.canvas = &picker->canvas;

	/* Succeeded: the frame can be drawn. */
	return 0;
}

/* The answer is known: the window goes, and the answer is told after a roundtrip. */
static void
picker_answered(
	struct kl_media_picker *picker)
{
	/* Once. */
	if (picker->telling != NULL || picker->window == NULL)
		return;

	/* The window, then a sync whose answer tells. */
	picker_window_gone(picker);
	picker->telling = wl_display_sync(picker->display);
	if (picker->telling != NULL)
		(void)wl_callback_add_listener(picker->telling, &picker_tell_listener, picker);
	(void)wl_display_flush(picker->display);
}

/* Takes the window away (the answer is known, or the picker is destroyed). */
static void
picker_window_gone(
	struct kl_media_picker *picker)
{
	/* The frame asked for. */
	if (picker->frame != NULL) {
		wl_callback_destroy(picker->frame);
		picker->frame = NULL;
	}

	/* The sheet's titlebar before its window. */
	if (picker->sheet != NULL) {
		kl_titlebar_destroy(picker->sheet);
		picker->sheet = NULL;
	}

	/* The glass before its window, and the panels it had. */
	kl_glass_destroy(picker->glass);
	picker->glass = NULL;
	picker->panel_count = 0;

	/* The window, with its own shell. */
	kl_window_close(picker->window);
	picker->window = NULL;
}

/* The frame asked for is due: draws when something changed or moves. */
static void
picker_frame_done(
	void *data,
	struct wl_callback *callback,
	uint32_t time)
{
	struct kl_media_picker *picker;

	/* The callback is used up. */
	(void)time;
	picker = data;
	wl_callback_destroy(callback);
	picker->frame = NULL;

	/* The next frame when one is due. */
	if (picker->dirty)
		picker_draw(picker);
}

/* Tells the answer; the application may destroy the picker from its callback. */
static void
picker_tell_done(
	void *data,
	struct wl_callback *callback,
	uint32_t time)
{
	struct kl_media_picker *picker;

	/* The callback is used up. */
	(void)time;
	picker = data;
	wl_callback_destroy(callback);
	picker->telling = NULL;

	/* The answer, last: nothing of the picker is touched after it. */
	picker->listener->done(picker->data, picker, picker->model.result, picker->model.answer, picker->model.answer_count);
}

/*
 * Asks the compositor to hang the picker under its parent's title bar: its
 * titlebar in the sheet mode.  A compositor without sheets leaves it a
 * window of its own.
 */
static void
picker_sheet(
	struct kl_media_picker *picker)
{
	int error;

	/* The window's titlebar presentation. */
	picker->sheet = kl_titlebar_create(picker->display, picker->window->toplevel, NULL, NULL);
	if (picker->sheet == NULL)
		return;

	/* The sheet mode, in one transaction. */
	error = kl_titlebar_begin(picker->sheet);
	if (error == 0)
		error = kl_titlebar_set_mode(picker->sheet, KL_TITLEBAR_SHEET);
	if (error == 0)
		error = kl_titlebar_commit(picker->sheet);

	/* Refused: a window of its own. */
	if (error != 0) {
		kl_titlebar_destroy(picker->sheet);
		picker->sheet = NULL;
	}
}

/*
 * Gives the window the compositor's glass when it can have it (a see-through
 * frame and a compositor with glass); otherwise the window stays opaque.
 */
static void
picker_glass(
	struct kl_media_picker *picker)
{
	int see_through;

	/* Opaque unless the glass is made. */
	picker->style.glass = 0;

	/* A frame the compositor does not blend cannot show the glass. */
	see_through = kl_window_see_through(picker->window);
	if (!see_through)
		return;

	/* The glass, with no panels until the first frame. */
	picker->glass = kl_glass_create(picker->display, kl_window_surface(picker->window));
	if (picker->glass == NULL)
		return;

	/* Succeeded: the frames leave the ground clear for it. */
	picker->style.glass = 1;
}

/*
 * Sets the glass panels for the frame's size before it is shown, when they
 * changed: the window's panel and the card on it.
 */
static void
picker_glass_panels(
	struct kl_media_picker *picker)
{
	struct kl_glass_panel panels[PICKER_PANELS];
	struct kl_rect card;
	size_t count;
	int same;
	int error;

	/* A window without glass has no panels. */
	if (picker->glass == NULL)
		return;

	/* The window's panel; a sheet's reaches a radius above its top, so that its upper corners are square under the title bar. */
	memset(panels, 0, sizeof(panels));
	panels[0].x = 0;
	panels[0].y = 0;
	panels[0].width = (int32_t)picker->width;
	panels[0].height = (int32_t)picker->height;
	panels[0].radius = PICKER_GLASS_RADIUS;
	panels[0].kind = KL_GLASS_CARD;
	if (picker->sheet != NULL) {
		panels[0].y = -PICKER_GLASS_RADIUS;
		panels[0].height += PICKER_GLASS_RADIUS;
	}

	/* The card on it, where the view draws it. */
	count = 1U + keiui_picker_panels((int)picker->width, (int)picker->height, &card, 1U);
	if (count == PICKER_PANELS) {
		panels[1].x = card.x;
		panels[1].y = card.y;
		panels[1].width = card.width;
		panels[1].height = card.height;
		panels[1].radius = PICKER_CARD_RADIUS;
		panels[1].kind = KL_GLASS_CARD;
	}

	/* Nothing to send when the compositor has these. */
	if (count == picker->panel_count) {
		same = memcmp(panels, picker->panels, sizeof(panels[0]) * count);
		if (same == 0)
			return;
	}

	/* Sent with the frame; a refused list leaves the old panels. */
	error = kl_glass_set_panels(picker->glass, panels, count);
	if (error != 0)
		return;

	/* Remembered, to send again only what changes. */
	memcpy(picker->panels, panels, sizeof(panels[0]) * count);
	picker->panel_count = count;
}
