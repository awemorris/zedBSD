/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The windows on Android.
 *
 * The system gives the program one window, the Activity's (an
 * ANativeWindow), as large as the screen the program may use.  The program
 * may make several kl_windows, though: they are logical.  The first of the
 * application's list is the one shown; it has the size of the native window
 * less the insets (the system's bars, the cutout and the keyboard), and
 * every other window has the same size and waits behind it.  A window is
 * always full screen, and drawn at the scale of the screen's density from
 * the start, so a program has nothing to follow.
 *
 * Every function runs on M, the program's native thread.
 */

#include "apps/libkeiland/android/android.h"
#include "apps/libkeiland/android/bridge-jni.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* The size a window has before the system gives its own, and the least it takes. */
#define WINDOW_FIRST_WIDTH	800U
#define WINDOW_FIRST_HEIGHT	600U

/*
 * Atomic snapshot of the current logical window size and scale (96 = 100 %)
 * published on layout so worker threads can read it without locking.
 */
static atomic_uint screen_width_atomic;
static atomic_uint screen_height_atomic;
static atomic_uint screen_scale_atomic;

static void window_input(struct kl_window *window, unsigned kind, int pressed);
static void window_purge(struct kl_app *app, struct kl_window *window);
static void window_size_take(struct kl_app *app, uint32_t strip, uint32_t *width, uint32_t *height);

/*
 * Makes a window of the application and puts it on top: the one before it
 * loses the focus and the new one is told its size and the focus.
 */
struct kl_window *
kl_app_window_create(
	struct kl_app *app,
	const struct kl_window_options *options)
{
	struct kl_window *window;
	struct kl_window *before;
	uint32_t width;
	uint32_t height;
	char *title;

	/* An application to make it on. */
	if (app == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* The window's state. */
	window = calloc(1U, sizeof(*window));
	if (window == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	window->app = app;
	window->present = KL_PRESENT_NONE;
	android_titlebar_init(window);

	/* The size asked for is the size before the system's window is known. */
	width = WINDOW_FIRST_WIDTH;
	height = WINDOW_FIRST_HEIGHT;
	if (options != NULL) {
		window->present = options->present;
		if (options->width != 0U)
			width = options->width;
		if (options->height != 0U)
			height = options->height;
	}
	window->width = width;
	window->height = height;

	/* Its title. */
	if (options != NULL && options->title != NULL) {
		title = strdup(options->title);
		if (title == NULL) {
			free(window);
			errno = ENOMEM;
			return NULL;
		}
		window->title = title;
	}

	/* Takes the size of the native window when there is one. */
	if (app->native != NULL)
		window_size_take(app, 0U, &window->width, &window->height);

	/* On top: the one before it loses the focus. */
	before = android_window_top(app);
	window->next = app->windows;
	app->windows = window;
	if (before != NULL && app->focused)
		window_input(before, KL_WINDOW_FOCUS, 0);

	/* The first size and the focus, which the program waits for. */
	window_input(window, KL_WINDOW_RESIZE, 0);
	if (app->focused)
		window_input(window, KL_WINDOW_FOCUS, 1);
	if (window->title != NULL)
		(void)keiland_java_set_title(window->title);

	/* Succeeded: the window is shown. */
	return window;
}

/*
 * Closes a window: out of the application's list, its inputs dropped, and the
 * window under it shown.
 */
void
kl_window_close(
	struct kl_window *window)
{
	struct kl_window **link;
	struct kl_window *top;
	struct kl_app *app;
	int was_top;

	/* No window. */
	if (window == NULL)
		return;
	app = window->app;

	/* Out of the list. */
	was_top = (app->windows == window);
	for (link = &app->windows; *link != NULL; link = &(*link)->next) {
		if (*link == window) {
			*link = window->next;
			break;
		}
	}

	/* What was queued for it. */
	window_purge(app, window);

	/* The window under it is shown, drawn again, and has the focus. */
	top = android_window_top(app);
	if (was_top && top != NULL) {
		if (app->focused)
			window_input(top, KL_WINDOW_FOCUS, 1);
		window_input(top, KL_WINDOW_RESIZE, 0);
		if (top->title != NULL)
			(void)keiland_java_set_title(top->title);
	}

	/* The title, the frame kept and the state; a menu popup that was open for it is left to close itself. */
	android_present_release(window);
	android_titlebar_release(window);
	free(window->title);
	free(window);
}

/*
 * Reports the content's size as last told.
 */
void
kl_window_size(
	const struct kl_window *window,
	uint32_t *width,
	uint32_t *height)
{
	/* No window: nothing. */
	if (window == NULL) {
		if (width != NULL)
			*width = 0U;
		if (height != NULL)
			*height = 0U;
		return;
	}

	/* The size. */
	if (width != NULL)
		*width = window->width;
	if (height != NULL)
		*height = window->height;
}

/*
 * Reports the scale the window is drawn at: the screen's density.
 */
unsigned
kl_window_scale(
	const struct kl_window *window)
{
	/* No window: the base scale. */
	if (window == NULL || window->app->scale == 0U)
		return KL_SCALE_BASE;

	/* The application's. */
	return window->app->scale;
}

/*
 * Has the window follow the system's scale: it does from the start.
 */
int
kl_window_follow_scale(
	struct kl_window *window)
{
	/* A window. */
	if (window == NULL)
		return EINVAL;

	/* Succeeded: it follows the density already. */
	return 0;
}

/*
 * Keeps whether the window defers its configures: none ever comes.
 */
int
kl_window_set_configure_deferred(
	struct kl_window *window,
	int deferred)
{
	(void)deferred;

	/* A window. */
	if (window == NULL)
		return EINVAL;

	/* Succeeded: nothing changes. */
	return 0;
}

/*
 * Takes the configure waiting for an acknowledgement: none on Android.
 */
int
kl_window_configure_take(
	struct kl_window *window,
	struct kl_window_configure *configure)
{
	/* A window and the place for the configure. */
	if (window == NULL || configure == NULL)
		return EINVAL;

	/* No configure waits. */
	return EAGAIN;
}

/*
 * Acknowledges a configure: none was taken, so every serial is another.
 */
int
kl_window_configure_ack(
	struct kl_window *window,
	uint32_t serial)
{
	(void)window;
	(void)serial;

	/* No take gave a serial. */
	return EINVAL;
}

/*
 * Keeps the program's callback for the sizes during a resize: Android has
 * no modal loop, so it is never called (as on Wayland).
 */
void
kl_window_on_resize(
	struct kl_window *window,
	kl_window_resize_fn callback,
	void *data)
{
	/* A window. */
	if (window == NULL)
		return;

	/* The callback. */
	window->resize_callback = callback;
	window->resize_data = data;
}

/*
 * Changes the window's title; the one shown names the task.
 */
void
kl_window_set_title(
	struct kl_window *window,
	const char *title)
{
	char *copy;

	/* A window and a title. */
	if (window == NULL || title == NULL)
		return;

	/* The title, kept. */
	copy = strdup(title);
	if (copy == NULL)
		return;
	free(window->title);
	window->title = copy;

	/* Shown when the window is the one shown, in the task's name and in the strip. */
	if (android_window_top(window->app) == window)
		(void)keiland_java_set_title(title);
	android_titlebar_changed(window);
}

/*
 * Makes the window full screen or not: it always is.
 */
void
kl_window_set_fullscreen(
	struct kl_window *window,
	int fullscreen)
{
	(void)window;
	(void)fullscreen;

	/* Nothing to change. */
}

/*
 * Reports whether the window is full screen: it always is.
 */
int
kl_window_fullscreen(
	const struct kl_window *window)
{
	(void)window;

	/* Always. */
	return 1;
}

/*
 * Takes the system's window: held until it is detached, and the windows fit
 * it.
 */
void
android_window_attach(
	struct kl_app *app,
	ANativeWindow *native)
{
	/* A window of an earlier Activity that was not let go. */
	if (app->native != NULL)
		ANativeWindow_release(app->native);

	/* Held. */
	ANativeWindow_acquire(native);
	app->native = native;
	android_window_layout(app);
}

/*
 * Lets the system's window go: nothing is drawn until the next one.
 */
void
android_window_detach(
	struct kl_app *app)
{
	/* No window held. */
	if (app->native == NULL)
		return;

	/* Let go. */
	ANativeWindow_release(app->native);
	app->native = NULL;
}

/*
 * Takes the insets and fits the windows to them.
 */
void
android_window_insets(
	struct kl_app *app,
	const int32_t insets[5])
{
	/* Kept. */
	memcpy(app->insets, insets, sizeof(app->insets));
	android_window_layout(app);
}

/*
 * Fits the windows to the native window less the insets.  A window whose
 * size changed is told; the window on top is told even when it did not (a
 * call to draw it again).
 */
void
android_window_layout(
	struct kl_app *app)
{
	struct kl_window *window;
	uint32_t width;
	uint32_t height;
	uint32_t strip;
	int changed;

	/* No window to draw in: the sizes stay. */
	if (app->native == NULL)
		return;

	/* Each window: the size the native window leaves, less its strip (the strip comes or goes with the menu and the tabs). */
	for (window = app->windows; window != NULL; window = window->next) {
		strip = android_titlebar_wanted(window);
		window_size_take(app, strip, &width, &height);
		changed = (window->width != width || window->height != height);
		window->width = width;
		window->height = height;
		android_titlebar_resize(window);
		if (changed || window == app->windows)
			window_input(window, KL_WINDOW_RESIZE, 0);
		if (window == app->windows) {
			atomic_store(&screen_width_atomic, width);
			atomic_store(&screen_height_atomic, height);
			atomic_store(&screen_scale_atomic, app->scale != 0U ? app->scale : KL_SCALE_BASE);
		}
	}

	/* The window shown, of the size it was, is painted again from its last frame (the native window may be new and empty). */
	android_present_repaint(app);
}

/*
 * Returns the window on top.
 */
struct kl_window *
android_window_top(
	struct kl_app *app)
{
	/* The first of the list, which may be none. */
	return app->windows;
}

/*
 * Reports whether the window is one of the application's.
 */
int
android_window_exists(
	const struct kl_app *app,
	const struct kl_window *window)
{
	const struct kl_window *each;

	/* The list. */
	for (each = app->windows; each != NULL; each = each->next) {
		if (each == window)
			return 1;
	}

	/* Not one of them. */
	return 0;
}

/*
 * Reads the latest logical window size and scale (96 = 100 %) from any thread.
 */
void
keiland_android_screen(
	unsigned *width,
	unsigned *height,
	unsigned *scale)
{
	/* Reads the latest published dimensions without locking. */
	if (width != NULL)
		*width = atomic_load(&screen_width_atomic);
	if (height != NULL)
		*height = atomic_load(&screen_height_atomic);
	if (scale != NULL)
		*scale = atomic_load(&screen_scale_atomic);
}

/*
 * Android has no hardware mouse cursor to hide over remote desktop tabs.
 */
void
kl_window_set_cursor_hidden(
	struct kl_window *window,
	int hidden)
{
	(void)window;
	(void)hidden;
}

/* Queues an input of a kind with no more than the kind and whether it is pressed (a focus, 1 when it came). */
static void
window_input(
	struct kl_window *window,
	unsigned kind,
	int pressed)
{
	struct kl_window_event input;

	/* The input. */
	memset(&input, 0, sizeof(input));
	input.kind = kind;
	input.pressed = pressed;

	/* Queued; without memory the program is not told. */
	(void)android_window_queue(window, &input);
}

/* Drops what was queued for a closed window; its posts reach the program as the application's own. */
static void
window_purge(
	struct kl_app *app,
	struct kl_window *window)
{
	struct kl_app_event *event;
	size_t kept;
	size_t index;
	size_t from;

	/* Each event in order, the ones kept moved to the front. */
	kept = 0U;
	for (index = 0U; index < app->event_count; index++) {
		from = (app->event_head + index) % app->event_capacity;
		event = &app->events[from];
		if (event->kind == KL_APP_WINDOW && event->window == window) {
			if (event->input.kind != KL_WINDOW_POST)
				continue;

			/* A post arrives as the application's own. */
			event->kind = KL_APP_POST;
			event->code = event->input.code;
			event->data = event->input.data;
			event->window = NULL;
		}
		if (kept != index)
			app->events[(app->event_head + kept) % app->event_capacity] = *event;
		kept++;
	}
	app->event_count = kept;
}

/* Gives the size the native window has less the insets (the keyboard's height if it is the larger bottom) and less the strip's height. */
static void
window_size_take(
	struct kl_app *app,
	uint32_t strip,
	uint32_t *width,
	uint32_t *height)
{
	int32_t native_width;
	int32_t native_height;
	int32_t bottom;
	int32_t taken_width;
	int32_t taken_height;

	/* The native window's size and the insets. */
	native_width = ANativeWindow_getWidth(app->native);
	native_height = ANativeWindow_getHeight(app->native);
	bottom = app->insets[ANDROID_INSET_BOTTOM];
	if (app->insets[ANDROID_INSET_IME] > bottom)
		bottom = app->insets[ANDROID_INSET_IME];
	taken_width = native_width - app->insets[ANDROID_INSET_LEFT] - app->insets[ANDROID_INSET_RIGHT];
	taken_height = native_height - app->insets[ANDROID_INSET_TOP] - bottom - (int32_t)strip;

	/* At least one pixel: a window of nothing cannot be drawn. */
	if (taken_width < 1)
		taken_width = 1;
	if (taken_height < 1)
		taken_height = 1;
	*width = (uint32_t)taken_width;
	*height = (uint32_t)taken_height;
}
