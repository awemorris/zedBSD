/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The application on Windows (kl_app): the windows' classes, the message
 * loop and the posts.
 *
 * kl_app_dispatch waits with MsgWaitForMultipleObjectsEx for the posts'
 * wake (an event object other threads set) and for a message, then moves
 * the posts into the queue and runs every message waiting
 * (PeekMessageW, TranslateMessage, DispatchMessageW); the window
 * procedure (window.c) queues the windows' inputs.  The posts never go
 * through PostMessageW, whose queue can be full: like the Wayland
 * backend's, a post of the application's own thread goes straight into
 * the queue while no other post waits, and the others wait in a list
 * until the dispatch.  Every event queued takes the next number of the
 * application's counter, so that kl_app_take gives the application's
 * posts and its windows' inputs in the order they came.
 */

#include "userland/desktop/libkeiland/win32/window-win32.h"

#include <ole2.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void app_posts_move(struct kl_app *app);
static int app_post_event(struct kl_app *app, const struct kl_app_event *event);
static int app_deliver(struct kl_app *app, struct kl_app_event *event);
static int app_has_window(const struct kl_app *app, const struct kl_window *window);
static int app_queued(const struct kl_app *app);
static int app_name(struct kl_app *app, const char *application);

/*
 * The counter's rate of kl_clock_us (QueryPerformanceFrequency), read once;
 * the C runtime's once is not needed: any thread that reads 0 asks again
 * and finds the same value.
 */
static LONGLONG app_clock_rate;

/*
 * Opens the application: its name, its windows' classes, the posts' lock
 * and wake, and the system's appearance.
 */
struct kl_app *
kl_app_open(
	const struct kl_app_options *options)
{
	struct kl_app *app;
	const char *application;
	int error;

	/* The application's state. */
	app = calloc(1U, sizeof(*app));
	if (app == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* Its name (the windows' property and the storage's directory). */
	application = "keiland";
	if (options != NULL && options->application != NULL && options->application[0] != '\0')
		application = options->application;
	error = app_name(app, application);
	if (error != 0) {
		free(app);
		errno = error;
		return NULL;
	}

	/* The posts' lock and wake. */
	error = keiui_lock_open(&app->post_lock);
	if (error != 0) {
		free(app->application_wide);
		free(app->application);
		free(app);
		errno = error;
		return NULL;
	}

	/* Makes the wake that tells the loop a post is waiting. */
	error = keiui_wake_open(&app->post_wake);
	if (error != 0) {
		keiui_lock_close(&app->post_lock);
		free(app->application_wide);
		free(app->application);
		free(app);
		errno = error;
		return NULL;
	}

	/* The windows' classes. */
	app->module = GetModuleHandleW(NULL);
	error = keiui_win32_classes(app);
	if (error != 0) {
		keiui_wake_close(&app->post_wake);
		keiui_lock_close(&app->post_lock);
		free(app->application_wide);
		free(app->application);
		free(app);
		errno = error;
		return NULL;
	}

	/* The thread that dispatches, and the system's appearance (read once, at the start). */
	app->thread = keiui_thread_self();
	app->dark = keiui_win32_dark();

	/* Succeeded: the application is open. */
	return app;
}

/*
 * Closes the windows still open, the posts not taken (their data with
 * them) and the application.
 */
void
kl_app_close(
	struct kl_app *app)
{
	struct keiui_win32_post *post;

	/* No application. */
	if (app == NULL)
		return;

	/* Each window still open. */
	while (app->windows != NULL)
		kl_window_close(app->windows);

	/* The posts that still wait. */
	while (app->post_first != NULL) {
		post = app->post_first;
		app->post_first = post->next;
		free(post);
	}

	/* The classes, the wake, the lock and the state. */
	keiui_win32_classes_drop(app);
	if (app->ole)
		OleUninitialize();
	keiui_wake_close(&app->post_wake);
	keiui_lock_close(&app->post_lock);
	free(app->application_wide);
	free(app->application);
	free(app);
}

/*
 * Waits up to a timeout (milliseconds, -1 for ever; no wait while events
 * are queued) for a message or a post, and queues what happened.
 */
int
kl_app_dispatch(
	struct kl_app *app,
	int timeout_ms)
{
	HANDLE wake;
	DWORD wait;
	DWORD waited;
	MSG message;
	BOOL found;
	int queued;

	/* An application. */
	if (app == NULL)
		return -1;

	/* No wait while something is already queued. */
	wait = INFINITE;
	if (timeout_ms >= 0)
		wait = (DWORD)timeout_ms;
	queued = app_queued(app);
	if (queued)
		wait = 0U;

	/* The posts' wake and the thread's messages, together. */
	wake = (HANDLE)keiui_wake_event(&app->post_wake);
	waited = MsgWaitForMultipleObjectsEx(1U, &wake, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
	if (waited == WAIT_FAILED)
		return -1;

	/* The posts other threads made, in their order. */
	app_posts_move(app);

	/* Every message waiting, each to its window's procedure (which queues the inputs). */
	for (;;) {
		found = PeekMessageW(&message, NULL, 0U, 0U, PM_REMOVE);
		if (!found)
			break;
		(void)TranslateMessage(&message);
		(void)DispatchMessageW(&message);
	}

	/* Succeeded: what happened is queued. */
	return 0;
}

/*
 * Reports the application's language: English (the Windows backend does
 * not follow a language setting yet).
 */
const char *
kl_app_language(
	const struct kl_app *app)
{
	(void)app;

	/* English. */
	return "en";
}

/*
 * Posts an event of the program's own (KL_APP_POST) to the application,
 * from any thread.
 */
int
kl_app_post(
	struct kl_app *app,
	uint32_t code,
	void *data)
{
	struct kl_app_event event;
	int error;

	/* An application. */
	if (app == NULL)
		return EINVAL;

	/* The event. */
	memset(&event, 0, sizeof(event));
	event.kind = KL_APP_POST;
	event.code = code;
	event.data = data;

	/* Queued now or waiting for the dispatch. */
	error = app_post_event(app, &event);
	if (error != 0)
		return error;

	/* Succeeded: the event is posted. */
	return 0;
}

/*
 * Reads the system's colours again (the window procedure heard they
 * changed); when the appearance or the accent changed, every window's
 * frame follows and the program is told with a KL_APP_THEME event.
 */
void
keiui_win32_app_theme(
	struct kl_app *app)
{
	struct kl_app_event event;
	struct kl_window *window;
	int changed;

	/* The colours now; nothing to do when they are as they were. */
	changed = keiui_win32_theme_reread(app);
	if (!changed)
		return;

	/* Each window's frame in the new appearance. */
	for (window = app->windows; window != NULL; window = window->next)
		keiui_win32_window_theme(window);

	/* The program's event; without memory for it the program is not told. */
	memset(&event, 0, sizeof(event));
	event.kind = KL_APP_THEME;
	(void)app_post_event(app, &event);
}

/*
 * Posts an input of the program's own (KL_WINDOW_POST) to a window of the
 * application, from any thread.
 */
int
kl_window_post(
	struct kl_window *window,
	uint32_t code,
	void *data)
{
	struct kl_app *app;
	struct kl_window_event *input;
	struct keiui_win32_post *post;
	int own;
	int first;

	/* A window. */
	if (window == NULL)
		return EINVAL;
	app = window->app;

	/* The application's thread, with no post waiting: among the window's inputs now. */
	own = keiui_thread_is(app->thread);
	if (own && app->post_first == NULL) {
		input = keiui_win32_push(window, KL_WINDOW_POST);
		if (input != NULL) {
			input->code = code;
			input->data = data;
			return 0;
		}
	}

	/* A post that waits for the dispatch, as the window's input. */
	post = calloc(1U, sizeof(*post));
	if (post == NULL)
		return ENOMEM;
	post->event.kind = KL_APP_WINDOW;
	post->event.window = window;
	post->event.input.kind = KL_WINDOW_POST;
	post->event.input.code = code;
	post->event.input.data = data;
	post->next = NULL;

	/* At the list's end; the first one rings the wake. */
	keiui_lock_take(&app->post_lock);

	first = 0;
	if (app->post_last != NULL) {
		app->post_last->next = post;
	} else {
		app->post_first = post;
		first = 1;
	}

	/* Makes the post the last of the queue. */
	app->post_last = post;

	keiui_lock_give(&app->post_lock);
	if (first)
		keiui_wake_ring(&app->post_wake);

	/* Succeeded: it waits for the dispatch. */
	return 0;
}

/*
 * Takes the oldest event of the application: its own posts' and its
 * windows' inputs, by the order they were queued in.
 */
int
kl_app_take(
	struct kl_app *app,
	struct kl_app_event *event)
{
	struct kl_window *window;
	struct kl_window *oldest;
	uint64_t order;
	uint64_t best;
	int found;
	int have;

	/* An application. */
	if (app == NULL || event == NULL)
		return 0;

	/* The oldest of the application's posts. */
	have = 0;
	best = 0U;
	oldest = NULL;
	if (app->post_count != 0U) {
		best = app->posts[app->post_head].order;
		have = 1;
	}

	/* And of each window's inputs. */
	for (window = app->windows; window != NULL; window = window->next) {
		found = keiui_win32_peek(window, &order);
		if (!found)
			continue;
		if (!have || order < best) {
			best = order;
			oldest = window;
			have = 1;
		}
	}

	/* Nothing queued. */
	if (!have)
		return 0;

	/* A window's input. */
	if (oldest != NULL) {
		memset(event, 0, sizeof(*event));
		event->kind = KL_APP_WINDOW;
		event->window = oldest;
		(void)keiui_win32_take(oldest, &order, &event->input);
		return 1;
	}

	/* The application's post. */
	*event = app->posts[app->post_head].event;
	app->post_head = (app->post_head + 1U) % KEIUI_WIN32_EVENTS;
	app->post_count--;

	/* Succeeded: one event taken. */
	return 1;
}

/*
 * Reports the application's system: none on Windows (the programs go on
 * without it).
 */
struct kl_system *
kl_app_system(
	struct kl_app *app)
{
	(void)app;

	/* No system. */
	return NULL;
}

/*
 * Reports a monotonic time in microseconds (the performance counter).
 */
uint64_t
kl_clock_us(void)
{
	LARGE_INTEGER now;
	LARGE_INTEGER rate;
	LONGLONG frequency;
	LONGLONG seconds;
	LONGLONG rest;

	/* The counter's rate, read once. */
	frequency = app_clock_rate;
	if (frequency == 0) {
		(void)QueryPerformanceFrequency(&rate);
		frequency = rate.QuadPart;
		app_clock_rate = frequency;
	}

	/* The counter, in microseconds without overflowing on the way. */
	(void)QueryPerformanceCounter(&now);
	seconds = now.QuadPart / frequency;
	rest = now.QuadPart % frequency;

	/* Succeeded: the time. */
	return (uint64_t)seconds * 1000000U + (uint64_t)(rest * 1000000 / frequency);
}

/*
 * Gives the next number of the application's counter, which orders every
 * event queued.
 */
uint64_t
keiui_win32_next_order(
	struct kl_app *app)
{
	/* The next number. */
	app->order++;
	return app->order;
}

/* Moves the posts other threads made into the queue, in their order, while it has room. */
static void
app_posts_move(
	struct kl_app *app)
{
	struct keiui_win32_post *list;
	struct keiui_win32_post *post;
	struct keiui_win32_post *last;
	int queued;

	/* The wake heard; a post made from now on rings it again. */
	keiui_wake_clear(&app->post_wake);

	/* The list, taken whole. */
	keiui_lock_take(&app->post_lock);

	list = app->post_first;
	app->post_first = NULL;
	app->post_last = NULL;

	keiui_lock_give(&app->post_lock);

	/* Each post in order, while the queue has room. */
	while (list != NULL) {
		queued = app_deliver(app, &list->event);
		if (!queued)
			break;
		post = list;
		list = list->next;
		free(post);
	}

	/* All moved. */
	if (list == NULL)
		return;

	/* The rest goes back before what was posted meanwhile, and the wake rings again. */
	for (last = list; last->next != NULL; last = last->next)
		continue;

	/* Puts the rest back in front. */
	keiui_lock_take(&app->post_lock);

	last->next = app->post_first;
	if (app->post_first == NULL)
		app->post_last = last;
	app->post_first = list;

	keiui_lock_give(&app->post_lock);
	keiui_wake_ring(&app->post_wake);
}

/*
 * Posts an application's event: on the application's thread with no post
 * waiting before it, straight into the queue while it has room; otherwise
 * at the end of the list the dispatch moves (the first one rings the
 * wake).  Returns 0 or ENOMEM.
 */
static int
app_post_event(
	struct kl_app *app,
	const struct kl_app_event *event)
{
	struct kl_app_event delivered;
	struct keiui_win32_post *post;
	int own;
	int queued;
	int first;

	/* The application's thread, with no post waiting before it. */
	own = keiui_thread_is(app->thread);
	if (own) {
		delivered = *event;
		keiui_lock_take(&app->post_lock);

		queued = 0;
		if (app->post_first == NULL)
			queued = app_deliver(app, &delivered);

		keiui_lock_give(&app->post_lock);
		if (queued)
			return 0;
	}

	/* A post that waits for the dispatch. */
	post = malloc(sizeof(*post));
	if (post == NULL)
		return ENOMEM;
	post->event = *event;
	post->next = NULL;

	/* At the list's end; the first one rings the wake. */
	keiui_lock_take(&app->post_lock);

	first = 0;
	if (app->post_last != NULL) {
		app->post_last->next = post;
	} else {
		app->post_first = post;
		first = 1;
	}

	/* Makes the post the last of the queue. */
	app->post_last = post;

	keiui_lock_give(&app->post_lock);
	if (first)
		keiui_wake_ring(&app->post_wake);

	/* Succeeded: it waits for the dispatch. */
	return 0;
}

/* Queues one post: a window's among its inputs while the window is open, else the application's.  1 when queued, 0 when there is no room. */
static int
app_deliver(
	struct kl_app *app,
	struct kl_app_event *event)
{
	struct kl_window_event *input;
	struct keiui_win32_posted *slot;
	int present;
	uint32_t code;
	void *data;

	/* A window's post. */
	if (event->kind == KL_APP_WINDOW && event->window != NULL) {
		present = app_has_window(app, event->window);
		if (present) {
			input = keiui_win32_push(event->window, KL_WINDOW_POST);
			if (input == NULL)
				return 0;
			input->code = event->input.code;
			input->data = event->input.data;
			return 1;
		}

		/* Its window is gone: the application's post with the same code and data. */
		code = event->input.code;
		data = event->input.data;
		memset(event, 0, sizeof(*event));
		event->kind = KL_APP_POST;
		event->code = code;
		event->data = data;
	}

	/* No room for the application's own. */
	if (app->post_count == KEIUI_WIN32_EVENTS)
		return 0;

	/* The application's own post, at the queue's end. */
	slot = &app->posts[(app->post_head + app->post_count) % KEIUI_WIN32_EVENTS];
	slot->order = keiui_win32_next_order(app);
	slot->event = *event;
	app->post_count++;

	/* Succeeded: queued. */
	return 1;
}

/* Reports whether a window is still one of the application's. */
static int
app_has_window(
	const struct kl_app *app,
	const struct kl_window *window)
{
	const struct kl_window *each;

	/* Each window of the application. */
	for (each = app->windows; each != NULL; each = each->next) {
		if (each == window)
			return 1;
	}

	/* Not one of them. */
	return 0;
}

/* Reports whether an event is queued already (a post of the application's, or a window's input). */
static int
app_queued(
	const struct kl_app *app)
{
	const struct kl_window *window;

	/* The application's posts. */
	if (app->post_count != 0U)
		return 1;

	/* Each window's inputs. */
	for (window = app->windows; window != NULL; window = window->next) {
		if (window->event_count != 0U)
			return 1;
	}

	/* Nothing. */
	return 0;
}

/* Keeps the application's name, and its UTF-16 form; 0, ENOMEM or EILSEQ. */
static int
app_name(
	struct kl_app *app,
	const char *application)
{
	size_t length;
	size_t count;
	int error;

	/* The UTF-8 name. */
	length = strlen(application);
	app->application = malloc(length + 1U);
	if (app->application == NULL)
		return ENOMEM;
	memcpy(app->application, application, length + 1U);

	/* The UTF-16 name, with its NUL. */
	app->application_wide = calloc(length + 1U, sizeof(WCHAR));
	if (app->application_wide == NULL) {
		free(app->application);
		return ENOMEM;
	}

	/* Converts the application's name to UTF-16 for the system. */
	error = keiui_win32_utf16_from_utf8(application, length, (uint16_t *)app->application_wide, length + 1U, &count);
	if (error != 0) {
		free(app->application_wide);
		free(app->application);
		return error;
	}

	/* Succeeded. */
	return 0;
}
