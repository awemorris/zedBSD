/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The application on Android (kl_app): the one place where what the system
 * and Java tell the program becomes libkeiland's events.
 *
 * kl_app_dispatch runs on M, the program's native thread.  It waits in M's
 * looper (ALooper_pollOnce) for the wake-up descriptor and the input queue.
 * A wake-up means one of three things were queued: the Activity's events
 * (windows, the input queue, life cycle: activity.c), the events Java told
 * (the bridge: text, insets, configuration, focus, the clipboard), and the
 * posts other threads made (kl_app_post and kl_window_post).  All of them are
 * turned here, in this order, into events of one ring that kl_app_take
 * hands out; the windows have no queue of their own.
 *
 * Only M touches the ring and the windows.  The posts are the one thing other
 * threads add to: they wait in a list under the application's lock.
 */

#include "apps/libkeiland/android/android.h"

#include <android/configuration.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The ring's first size, in events; it doubles when it is full. */
#define APP_RING_FIRST	64U

/* A density the platform does not state (ACONFIGURATION_DENSITY_ANY and _NONE) is taken as medium. */
#define APP_DENSITY_MEDIUM	160U
#define APP_DENSITY_UNSTATED	0xFFFEU

/* The application: one for the process, made by kl_app_open. */
static struct kl_app *app_open;

static void app_activity_event(struct kl_app *app, const struct keiland_android_event *event);
static void app_activity_drain(struct kl_app *app);
static void app_bridge_drain(struct kl_app *app);
static void app_bridge_event(struct kl_app *app, const struct keiland_bridge_event *event);
static void app_posts_drain(struct kl_app *app);
static void app_focus(struct kl_app *app, int focused);
static unsigned app_scale(void);

/*
 * Opens the application: the one of the process, with the scale of the
 * screen's density.
 */
struct kl_app *
kl_app_open(
	const struct kl_app_options *options)
{
	struct kl_app *app;
	const char *application;

	/* One application for the process. */
	if (app_open != NULL) {
		errno = EBUSY;
		return NULL;
	}

	/* The state. */
	app = calloc(1U, sizeof(*app));
	if (app == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* The posts' lock. */
	if (pthread_mutex_init(&app->lock, NULL) != 0) {
		free(app);
		errno = ENOMEM;
		return NULL;
	}

	/* The name (the window's and the storage directory's). */
	application = "keiland";
	if (options != NULL && options->application != NULL && options->application[0] != '\0')
		application = options->application;
	(void)snprintf(app->application, sizeof(app->application), "%s", application);

	/* The scale of the screen, English until the system tells the language. */
	app->scale = app_scale();
	(void)snprintf(app->language, sizeof(app->language), "%s", "en");

	/* Succeeded: what the Activity queued before is seen by the first dispatch (the wake-up is still set). */
	app_open = app;
	return app;
}

/*
 * Closes the windows still open, the posts not taken (their data stays with
 * their makers) and the application.
 */
void
kl_app_close(
	struct kl_app *app)
{
	struct android_post *post;
	struct kl_window *window;

	/* No application. */
	if (app == NULL)
		return;

	/* Each window still open. */
	for (;;) {
		window = android_window_top(app);
		if (window == NULL)
			break;
		kl_window_close(window);
	}

	/* The posts that still wait. */
	while (app->posts != NULL) {
		post = app->posts;
		app->posts = post->next;
		free(post);
	}

	/* The ring and the state. */
	if (app_open == app)
		app_open = NULL;
	free(app->events);
	(void)pthread_mutex_destroy(&app->lock);
	free(app);
}

/*
 * Waits up to a timeout (milliseconds, -1 for ever; no wait while events
 * are queued) for the Activity, Java, a post or the input, and queues what
 * happened.
 */
int
kl_app_dispatch(
	struct kl_app *app,
	int timeout_ms)
{
	int ident;
	int wait;

	/* An application. */
	if (app == NULL)
		return -1;

	/* No wait while something is already queued. */
	wait = timeout_ms;
	if (app->event_count != 0U)
		wait = 0;

	/*
	 * What was queued before the wake-up descriptor was there (or while no
	 * application was open) is looked at first, without a wake-up to tell.
	 */
	keiland_android_wake_clear();
	app_activity_drain(app);
	app_bridge_drain(app);
	app_posts_drain(app);
	if (app->event_count != 0U)
		wait = 0;

	/* The first wait has the time asked, the rest none: they only empty what is ready. */
	for (;;) {
		ident = ALooper_pollOnce(wait, NULL, NULL, NULL);
		wait = 0;

		/* A looper that does not work. */
		if (ident == ALOOPER_POLL_ERROR)
			return -1;

		/* Nothing more ready. */
		if (ident == ALOOPER_POLL_TIMEOUT)
			break;

		/* What the Activity, Java and the other threads queued. */
		if (ident == KEILAND_IDENT_WAKE) {
			keiland_android_wake_clear();
			app_activity_drain(app);
			app_bridge_drain(app);
			app_posts_drain(app);
		}

		/* What the screen was touched or a key was pressed for. */
		if (ident == KEILAND_IDENT_INPUT && app->input != NULL)
			android_input_drain(app, app->input);
	}

	/* Succeeded: what happened is queued. */
	return 0;
}

/*
 * Reports the application's language, as the system last told it.
 */
const char *
kl_app_language(
	const struct kl_app *app)
{
	/* No application: English. */
	if (app == NULL)
		return "en";

	/* The language. */
	return app->language;
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
	struct android_post *post;

	/* An application. */
	if (app == NULL)
		return EINVAL;

	/* The post, which waits for the dispatch. */
	post = calloc(1U, sizeof(*post));
	if (post == NULL)
		return ENOMEM;
	post->code = code;
	post->data = data;

	/* At the list's end. */
	(void)pthread_mutex_lock(&app->lock);

	if (app->posts_tail != NULL)
		app->posts_tail->next = post;
	else
		app->posts = post;
	app->posts_tail = post;

	(void)pthread_mutex_unlock(&app->lock);

	/* M looks at the list. */
	keiland_android_wake();

	/* Succeeded: it waits for the dispatch. */
	return 0;
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
	struct android_post *post;

	/* A window of the application. */
	if (window == NULL)
		return EINVAL;
	app = android_app();
	if (app == NULL)
		return EINVAL;

	/* The post, which waits for the dispatch. */
	post = calloc(1U, sizeof(*post));
	if (post == NULL)
		return ENOMEM;
	post->window = window;
	post->code = code;
	post->data = data;

	/* At the list's end. */
	(void)pthread_mutex_lock(&app->lock);

	if (app->posts_tail != NULL)
		app->posts_tail->next = post;
	else
		app->posts = post;
	app->posts_tail = post;

	(void)pthread_mutex_unlock(&app->lock);

	/* M looks at the list. */
	keiland_android_wake();

	/* Succeeded: it waits for the dispatch. */
	return 0;
}

/*
 * Hands the application an event of its own before (or without) an
 * application: the bridge keeps it until a dispatch.
 */
int
keiland_post_app(
	uint32_t code,
	void *data)
{
	struct keiland_bridge_event event;

	/* The bridge's event. */
	memset(&event, 0, sizeof(event));
	event.kind = KEILAND_BRIDGE_APP_POST;
	event.code = code;
	event.data = data;

	/* Queued for M. */
	return keiland_bridge_push(&event, NULL, 0U);
}

/*
 * Takes the oldest event of the application: its own posts' and its windows'
 * inputs, in the order they were queued.
 */
int
kl_app_take(
	struct kl_app *app,
	struct kl_app_event *event)
{
	/* An application with an event. */
	if (app == NULL || event == NULL || app->event_count == 0U)
		return 0;

	/* The ring's first. */
	*event = app->events[app->event_head];
	app->event_head = (app->event_head + 1U) % app->event_capacity;
	app->event_count--;

	/* Succeeded: one event taken. */
	return 1;
}

/*
 * Reports the application's system: none on Android (the programs go on
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
 * Reports a monotonic time in microseconds.
 */
uint64_t
kl_clock_us(void)
{
	struct timespec now;

	/* The clock does not go back. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the time. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/*
 * Returns the open application, or NULL.
 */
struct kl_app *
android_app(void)
{
	/* The one of the process. */
	return app_open;
}

/*
 * Appends an event to the ring, which doubles when it is full.
 */
int
android_app_queue(
	struct kl_app *app,
	const struct kl_app_event *event)
{
	struct kl_app_event *grown;
	size_t capacity;
	size_t index;

	/* Makes room. */
	if (app->event_count == app->event_capacity) {
		capacity = app->event_capacity == 0U ? APP_RING_FIRST : app->event_capacity * 2U;
		grown = malloc(capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;

		/* The old events, oldest first. */
		for (index = 0U; index < app->event_count; index++)
			grown[index] = app->events[(app->event_head + index) % app->event_capacity];
		free(app->events);
		app->events = grown;
		app->event_head = 0U;
		app->event_capacity = capacity;
	}

	/* The ring's end. */
	app->events[(app->event_head + app->event_count) % app->event_capacity] = *event;
	app->event_count++;

	/* Succeeded: queued. */
	return 0;
}

/*
 * Appends a window's input, timed now, as the application's event.
 */
int
android_window_queue(
	struct kl_window *window,
	const struct kl_window_event *input)
{
	struct kl_app *app;
	struct kl_app_event event;
	uint64_t now;

	/* The application the window belongs to. */
	app = android_app();
	if (app == NULL)
		return EINVAL;

	/* The event, its input read now. */
	memset(&event, 0, sizeof(event));
	event.kind = KL_APP_WINDOW;
	event.window = window;
	event.input = *input;
	now = kl_clock_us();
	if (event.input.time_us == 0U)
		event.input.time_us = now;
	event.input.arrival_us = now;

	/* Queued. */
	return android_app_queue(app, &event);
}

/* Handles what the Activity queued: the windows, the input queue and the life cycle. */
static void
app_activity_drain(
	struct kl_app *app)
{
	struct keiland_android_event event;

	/* Each event, oldest first. */
	while (keiland_android_take(&event))
		app_activity_event(app, &event);
}

/* Handles one thing that happened to the Activity. */
static void
app_activity_event(
	struct kl_app *app,
	const struct keiland_android_event *event)
{
	/* By kind. */
	switch (event->kind) {
	case KEILAND_ANDROID_WINDOW_CREATED:
		/* A window to draw in. */
		android_window_attach(app, event->window);
		break;
	case KEILAND_ANDROID_WINDOW_RESIZED:
	case KEILAND_ANDROID_WINDOW_REDRAW:
		/* Its size, or a call to draw it again. */
		android_window_layout(app);
		break;
	case KEILAND_ANDROID_WINDOW_DESTROYED:
		/* Nothing is drawn in it from now on, and the system may take it. */
		android_window_detach(app);
		keiland_android_done(event);
		break;
	case KEILAND_ANDROID_INPUT_CREATED:
		/* The queue is joined to the looper already (keiland_android_take). */
		app->input = event->queue;
		break;
	case KEILAND_ANDROID_INPUT_DESTROYED:
		/* The queue is let go of already. */
		app->input = NULL;
		keiland_android_done(event);
		break;
	case KEILAND_ANDROID_START:
		/* Shown again: the frame is drawn again. */
		app->started = 1;
		android_window_layout(app);
		break;
	case KEILAND_ANDROID_STOP:
		/* Not shown: nothing is drawn. */
		app->started = 0;
		break;
	case KEILAND_ANDROID_FOCUS_GAINED:
		app_focus(app, 1);
		break;
	case KEILAND_ANDROID_FOCUS_LOST:
		app_focus(app, 0);
		break;
	case KEILAND_ANDROID_ACTIVITY_DESTROYED:
		/* The Activity is gone; the program stays. */
		keiland_android_done(event);
		break;
	case KEILAND_ANDROID_RESUME:
	case KEILAND_ANDROID_PAUSE:
	case KEILAND_ANDROID_CONFIG:
	case KEILAND_ANDROID_LOW_MEMORY:
	default:
		/* The size and the colours come in other ways. */
		break;
	}
}

/* Handles what Java told, in the order it told. */
static void
app_bridge_drain(
	struct kl_app *app)
{
	struct keiland_bridge_event *list;
	struct keiland_bridge_event *event;

	/* The whole list, then each event. */
	list = keiland_bridge_take_all();
	for (event = list; event != NULL; event = event->next)
		app_bridge_event(app, event);
	keiland_bridge_free(list);
}

/* Handles one thing Java told. */
static void
app_bridge_event(
	struct kl_app *app,
	const struct keiland_bridge_event *event)
{
	struct kl_app_event posted;
	int32_t insets[5];

	/* By kind. */
	switch (event->kind) {
	case KEILAND_BRIDGE_COMMIT:
	case KEILAND_BRIDGE_COMPOSE:
	case KEILAND_BRIDGE_DELETE:
		android_ime_bridge(app, event);
		break;
	case KEILAND_BRIDGE_INSETS:
		insets[ANDROID_INSET_LEFT] = event->a;
		insets[ANDROID_INSET_TOP] = event->b;
		insets[ANDROID_INSET_RIGHT] = event->c;
		insets[ANDROID_INSET_BOTTOM] = event->d;
		insets[ANDROID_INSET_IME] = event->e;
		android_window_insets(app, insets);
		break;
	case KEILAND_BRIDGE_CONFIG:
		android_theme_bridge(app, event);
		break;
	case KEILAND_BRIDGE_FOCUS:
		app_focus(app, event->a != 0);
		break;
	case KEILAND_BRIDGE_CLIPBOARD:
		android_clipboard_bridge(app, event);
		break;
	case KEILAND_BRIDGE_APP_POST:
		/* The program's own event (the data is the program's). */
		memset(&posted, 0, sizeof(posted));
		posted.kind = KL_APP_POST;
		posted.code = event->code;
		posted.data = event->data;
		(void)android_app_queue(app, &posted);
		break;
	case KEILAND_BRIDGE_KEY:
		(void)android_input_bridge_key(app, event);
		break;
	case KEILAND_BRIDGE_MENU:
	default:
		/* The menu's answer has no taker yet. */
		break;
	}
}

/* Turns the posts other threads made into events, in their order. */
static void
app_posts_drain(
	struct kl_app *app)
{
	struct android_post *list;
	struct android_post *post;
	struct kl_app_event event;
	struct kl_window_event input;

	/* The list, taken whole. */
	(void)pthread_mutex_lock(&app->lock);

	list = app->posts;
	app->posts = NULL;
	app->posts_tail = NULL;

	(void)pthread_mutex_unlock(&app->lock);

	/* Each post in order. */
	while (list != NULL) {
		post = list;
		list = post->next;

		/* A window's input while the window is open. */
		if (post->window != NULL && android_window_exists(app, post->window)) {
			memset(&input, 0, sizeof(input));
			input.kind = KL_WINDOW_POST;
			input.code = post->code;
			input.data = post->data;
			(void)android_window_queue(post->window, &input);
		} else {
			/* The application's own: also a window's that closed before its post came. */
			memset(&event, 0, sizeof(event));
			event.kind = KL_APP_POST;
			event.code = post->code;
			event.data = post->data;
			(void)android_app_queue(app, &event);
		}
		free(post);
	}
}

/* Takes the focus the system or Java told, and tells the window on top. */
static void
app_focus(
	struct kl_app *app,
	int focused)
{
	struct kl_window *window;
	struct kl_window_event input;

	/* Nothing changed. */
	if (app->focused == focused)
		return;
	app->focused = focused;

	/* The window shown has the focus or loses it. */
	window = android_window_top(app);
	if (window == NULL)
		return;
	memset(&input, 0, sizeof(input));
	input.kind = KL_WINDOW_FOCUS;
	input.pressed = focused;
	(void)android_window_queue(window, &input);
}

/*
 * Returns the scale of the screen in 96ths: its density in dots per inch
 * over 160 (the system's medium), as 96 is a hundredth percent.
 */
static unsigned
app_scale(void)
{
	ANativeActivity *activity;
	AConfiguration *configuration;
	unsigned density;

	/* No Activity yet: 100 %. */
	activity = keiland_android_activity();
	if (activity == NULL || activity->assetManager == NULL)
		return KL_SCALE_BASE;

	/* The density the Activity runs at. */
	configuration = AConfiguration_new();
	if (configuration == NULL)
		return KL_SCALE_BASE;
	AConfiguration_fromAssetManager(configuration, activity->assetManager);
	density = (unsigned)AConfiguration_getDensity(configuration);
	AConfiguration_delete(configuration);

	/* A density not stated is medium; none at all is 100 %. */
	if (density == 0U)
		return KL_SCALE_BASE;
	if (density >= APP_DENSITY_UNSTATED)
		density = APP_DENSITY_MEDIUM;

	/* Succeeded: the scale. */
	return density * KL_SCALE_BASE / APP_DENSITY_MEDIUM;
}
