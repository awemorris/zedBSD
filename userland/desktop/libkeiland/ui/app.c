/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The application (WS131 p015, plan/ws131/design.md section 6): one
 * connection to the compositor, whose globals are learnt once, when it
 * opens, and kept in a table.  Its windows bind what they need from the
 * one registry (window.c), and so do libkeiland's menus, titlebars,
 * glass, keyboard inset and editing operations: they ask
 * keiui_app_global for a display's global before they would search with a
 * registry and a roundtrip of their own.
 *
 * The windows' inputs, the events posted, the processes' and the copies'
 * news become one queue of events.  A dispatch waits for the compositor or
 * the wake of the posts, no longer than until a held key's repeat is due,
 * reads and runs the compositor's events, moves the posts that wait into
 * the queue, and then presses the keys whose repeat is due -- after the
 * events read with them, so that a release among them stops a repeat
 * first.
 *
 * The library runs on the application's thread; only a post comes from
 * another, and it waits in the application's list of posts (under its lock)
 * until the dispatch takes it.  The list of the applications open is the
 * library's own.
 */

#include "window.h"

#include "userland/desktop/libkeiland/process.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The desktop's setting of the language: a whole number, a language's place (kl_tr_language_code). */
#define APP_LANGUAGE_KEY	"ui.language"

/* The applications open, newest first. */
static struct kl_app *app_list;

static void app_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void app_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static int app_has(const struct kl_app *app, const char *interface);
static int app_wait(const struct kl_app *app, int timeout_ms, uint64_t now_us);
static void app_repeat(struct kl_app *app);
static struct kl_app_event *app_slot(struct kl_app *app);
static void app_appearance(void *data, unsigned appearance);
static void app_language_open(struct kl_app *app);
static void app_language_set(struct kl_app *app, int setting);
static void app_language_changed(void *data, const char *key, const char *value, unsigned flags);
static void app_posts_move(struct kl_app *app);
static int app_post_deliver(struct kl_app *app, struct kl_app_event *event);
static void app_post_orphan(struct kl_app_event *event);
static int app_has_window(const struct kl_app *app, const struct kl_window *window);

/* The registry's callbacks, for as long as the application lives. */
static const struct wl_registry_listener app_registry_listener = {
	app_global,
	app_global_remove
};

/*
 * Opens an application: connects, learns the globals with one roundtrip
 * and keeps them.
 */
struct kl_app *
kl_app_open(
	const struct kl_app_options *options)
{
	const char *display;
	struct kl_app *app;
	int compositor;
	int shell;
	int status;
	int error;

	/* The record. */
	app = calloc(1, sizeof(*app));
	if (app == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* The posts' list, its lock and its wake, for the application's thread. */
	app->thread = keiui_thread_self();
	error = keiui_lock_open(&app->post_lock);
	if (error != 0) {
		free(app);
		errno = error;
		return NULL;
	}

	/* Opens the wake the posting threads ring. */
	error = keiui_wake_open(&app->post_wake);
	if (error != 0) {
		keiui_lock_close(&app->post_lock);
		free(app);
		errno = error;
		return NULL;
	}

	/* Marks the posts' list, lock and wake as open, to be closed with the application. */
	app->post_open = 1;

	/* The app_id its windows get. */
	if (options != NULL && options->application != NULL) {
		app->application = strdup(options->application);
		if (app->application == NULL) {
			kl_app_close(app);
			errno = ENOMEM;
			return NULL;
		}
	}

	/* The connection. */
	display = NULL;
	if (options != NULL)
		display = options->display;
	app->display = wl_display_connect(display);
	if (app->display == NULL) {
		error = errno;
		kl_app_close(app);
		errno = error;
		return NULL;
	}

	/* The globals, announced once to the one registry. */
	app->registry = wl_display_get_registry(app->display);
	if (app->registry == NULL) {
		kl_app_close(app);
		errno = ENOMEM;
		return NULL;
	}

	/* Its listener, and one roundtrip for every global. */
	status = wl_registry_add_listener(app->registry, &app_registry_listener, app);
	if (status == 0)
		status = wl_display_roundtrip(app->display);
	if (status < 0) {
		kl_app_close(app);
		errno = EPROTO;
		return NULL;
	}

	/* Windows need a compositor and a shell. */
	compositor = app_has(app, "wl_compositor");
	shell = app_has(app, "xdg_wm_base");
	if (!compositor || !shell) {
		kl_app_close(app);
		errno = EOPNOTSUPP;
		return NULL;
	}

	/* One of the applications open. */
	app->next = app_list;
	app_list = app;

	/* The desktop's appearance, watched (a compositor without it leaves the application light). */
	(void)kl_appearance_open(app->display, app_appearance, app, &app->appearance);

	/* The desktop's language, watched (a compositor without its settings leaves English). */
	app_language_open(app);

	/* Succeeded. */
	return app;
}

/*
 * Closes the windows still open, the system, the menu service and the
 * connection.
 */
void
kl_app_close(
	struct kl_app *app)
{
	struct kl_app **link;
	struct keiui_post *post;

	/* No application, nothing to close. */
	if (app == NULL)
		return;

	/* Each window (its close takes it out of the list), and each process (its threads no longer reach the application). */
	while (app->windows != NULL)
		kl_window_close(app->windows);
	if (app->post_open) {
		keiui_processes_close(app);
		keiui_instances_unbind(app);
	}

	/* The appearance, the settings, the system and the menu service. */
	kl_appearance_close(app->appearance);
	if (app->settings != NULL)
		kl_settings_close(app->settings);
	if (app->system != NULL)
		kl_system_close(app->system);
	if (app->menu_service != NULL)
		kl_menu_service_close(app->menu_service);

	/* Out of the list of the applications open. */
	for (link = &app_list; *link != NULL; link = &(*link)->next) {
		if (*link == app) {
			*link = app->next;
			break;
		}
	}

	/* The registry, the connection, then the record. */
	if (app->registry != NULL)
		wl_registry_destroy(app->registry);
	if (app->display != NULL)
		wl_display_disconnect(app->display);

	/* The posts never taken (the threads that posted have ended), their wake and their lock. */
	if (app->post_open) {
		while (app->post_first != NULL) {
			post = app->post_first;
			app->post_first = post->next;
			free(post);
		}

		/* Closes the wake and the lock. */
		keiui_wake_close(&app->post_wake);
		keiui_lock_close(&app->post_lock);
	}

	/* The record. */
	free(app->application);
	free(app);
}

/*
 * Waits for the compositor or a post and queues what happened (app.c's
 * header).  Returns 0, or -1 when the connection is
 * broken.
 */
int
kl_app_dispatch(
	struct kl_app *app,
	int timeout_ms)
{
	struct pollfd descriptors[2];
	struct kl_window *window;
	int status;

	/* Each window's editing state as it is now goes out with what is flushed (edit.c). */
	for (window = app->windows; window != NULL; window = window->app_next)
		keiui_edit_update(window);

	/* Runs what is queued until a read of new events can be reserved. */
	for (;;) {
		status = wl_display_dispatch_pending(app->display);
		if (status < 0)
			return -1;

		/* A reserved read means nothing is queued any more. */
		status = wl_display_prepare_read(app->display);
		if (status == 0)
			break;

		/* EAGAIN asks for another dispatch; anything else is a broken connection. */
		if (errno != EAGAIN)
			return -1;
	}

	/* Sends what the windows asked for. */
	status = wl_display_flush(app->display);
	if (status < 0 && errno != EAGAIN) {
		wl_display_cancel_read(app->display);
		return -1;
	}

	/* The language's changes read so far (by another reader of the connection, too). */
	if (app->settings != NULL)
		(void)kl_settings_dispatch(app->settings);

	/* The processes' and the copies' news so far, then no longer than until a repeat is due, and not at all while events wait. */
	keiui_processes_queue(app);
	keiui_instances_queue(app);
	timeout_ms = app_wait(app, timeout_ms, kl_clock_us());

	/* The compositor's descriptor, then the posts' wake. */
	descriptors[0].fd = wl_display_get_fd(app->display);
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
	descriptors[1].fd = keiui_wake_descriptor(&app->post_wake);
	descriptors[1].events = POLLIN;
	descriptors[1].revents = 0;

	/* Waits for either. */
	status = poll(descriptors, 2, timeout_ms);

	/* Reads the compositor's events, or gives the reservation back. */
	if (status > 0 && (descriptors[0].revents & POLLIN) != 0) {
		status = wl_display_read_events(app->display);
		if (status < 0)
			return -1;
	} else {
		wl_display_cancel_read(app->display);
		if (status < 0 && errno != EINTR)
			return -1;

		/* A hung-up connection has no more events. */
		if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			return -1;
	}

	/* Runs the events read; they queue the windows' inputs. */
	status = wl_display_dispatch_pending(app->display);
	if (status < 0)
		return -1;

	/* The settings' events read: a change of the language queues its event. */
	if (app->settings != NULL)
		(void)kl_settings_dispatch(app->settings);

	/* The posts that wait, the processes' and the copies' news, then the repeats now due. */
	app_posts_move(app);
	keiui_processes_queue(app);
	keiui_instances_queue(app);
	app_repeat(app);

	/* Succeeded: the events so far are queued. */
	return 0;
}

/*
 * Reports the desktop's language as the application last heard it.
 */
const char *
kl_app_language(
	const struct kl_app *app)
{
	/* No application: English. */
	if (app == NULL)
		return "en";

	/* The language heard last. */
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
	struct kl_app_event event;
	int error;

	/* No application. */
	if (app == NULL)
		return EINVAL;

	/* The event, posted. */
	memset(&event, 0, sizeof(event));
	event.kind = KL_APP_POST;
	event.code = code;
	event.data = data;
	error = keiui_app_post_event(app, &event);
	if (error != 0)
		return error;

	/* Succeeded: posted. */
	return 0;
}

/*
 * Posts an event to the application from any thread (kl_app_post's way):
 * the application's thread queues it at once while no post waits before it,
 * any other post waits in the list for the dispatch, and the first one in
 * the list rings the wake.  Returns 0, or ENOMEM.
 */
int
keiui_app_post_event(
	struct kl_app *app,
	const struct kl_app_event *event)
{
	struct kl_app_event now;
	struct keiui_post *post;
	int own;
	int queued;
	int first;

	/* The application's thread, with no post before it: straight into the queue, while it has room. */
	own = keiui_thread_is(app->thread);
	if (own) {
		now = *event;

		/* Queues the event now when no earlier post waits. */
		keiui_lock_take(&app->post_lock);

		queued = 0;
		if (app->post_first == NULL)
			queued = app_post_deliver(app, &now);

		keiui_lock_give(&app->post_lock);
		if (queued)
			return 0;
	}

	/* A post to wait in the list. */
	post = malloc(sizeof(*post));
	if (post == NULL)
		return ENOMEM;
	post->event = *event;
	post->next = NULL;

	/* At the list's end; the first one rings the wake. */
	keiui_lock_take(&app->post_lock);

	first = 0;
	if (app->post_last != NULL)
		app->post_last->next = post;
	else {
		app->post_first = post;
		first = 1;
	}

	/* The new post is the list's end. */
	app->post_last = post;

	keiui_lock_give(&app->post_lock);
	if (first)
		keiui_wake_ring(&app->post_wake);

	/* Succeeded: it waits for the dispatch. */
	return 0;
}

/*
 * Takes the oldest event: 1 with it in *event, 0 when none is queued.
 */
int
kl_app_take(
	struct kl_app *app,
	struct kl_app_event *event)
{
	/* An empty queue. */
	if (app->event_count == 0U)
		return 0;

	/* The oldest event, and the queue moves on (a process may queue its next one). */
	*event = app->events[app->event_first];
	app->event_first = (app->event_first + 1U) % KEIUI_APP_EVENTS;
	app->event_count--;
	keiui_process_taken(event);
	keiui_instance_taken(event);

	/* Succeeded: one event taken. */
	return 1;
}

/*
 * The application's system, opened the first time it is asked for.
 */
struct kl_system *
kl_app_system(
	struct kl_app *app)
{
	/* Once. */
	if (app->system == NULL)
		app->system = kl_system_open(app->display);
	return app->system;
}

/*
 * Posts a notification of the application (ws156-p002): its application
 * ID is the name shown; the answer is not waited for.
 */
int
kl_app_notify(
	struct kl_app *app,
	const char *title,
	const char *body)
{
	struct kl_notification notification;
	struct kl_system *system;
	int error;

	/* The application's system. */
	if (app == NULL || title == NULL)
		return EINVAL;
	system = kl_app_system(app);
	if (system == NULL)
		return ENOTSUP;

	/* The words, under the application's ID. */
	memset(&notification, 0, sizeof(notification));
	notification.app = app->application;
	notification.title = title;
	notification.body = body;
	error = kl_system_notify(system, &notification, NULL);
	if (error != 0)
		return error;

	/* Sent at once. */
	(void)wl_display_flush(app->display);
	return 0;
}

/*
 * Posts a notification of the application with what the lock screen
 * shows of it (ws197-p004c), without waiting for its number.
 */
int
kl_app_notify_lock(
	struct kl_app *app,
	const char *title,
	const char *body,
	const char *lock_text)
{
	struct kl_notification notification;
	struct kl_system *system;
	int error;

	/* The application's system. */
	if (app == NULL || title == NULL)
		return EINVAL;
	system = kl_app_system(app);
	if (system == NULL)
		return ENOTSUP;

	/* The words, under the application's ID. */
	memset(&notification, 0, sizeof(notification));
	notification.app = app->application;
	notification.title = title;
	notification.body = body;
	error = kl_system_notify_lock(system, &notification, lock_text, NULL);
	if (error != 0)
		return error;

	/* Sent at once. */
	(void)wl_display_flush(app->display);
	return 0;
}

/*
 * Reports the application's connection.
 */
struct wl_display *
kl_app_display(
	const struct kl_app *app)
{
	/* The connection. */
	return app->display;
}

/*
 * Makes a window of the application (window.c).
 */
struct kl_window *
kl_app_window_create(
	struct kl_app *app,
	const struct kl_window_options *options)
{
	struct kl_window *window;

	/* An application to make it on. */
	if (app == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* The window, bound from the application's registry. */
	window = keiui_window_open_app(app, options);
	return window;
}

/*
 * Queues an event of a window and reports its input for the details (the
 * window's own record of what every input carries is filled by the
 * window); NULL when the queue is full and the input is dropped.
 */
struct kl_window_event *
keiui_app_push(
	struct kl_app *app,
	struct kl_window *window)
{
	struct kl_app_event *event;

	/* A slot, unless the queue is full. */
	event = app_slot(app);
	if (event == NULL)
		return NULL;

	/* A window's input. */
	event->kind = KL_APP_WINDOW;
	event->window = window;
	return &event->input;
}

/*
 * Takes a window out of its application: out of the list, and its events
 * out of the queue (the others keep their order).
 */
void
keiui_app_forget(
	struct kl_app *app,
	struct kl_window *window)
{
	struct kl_window **link;
	struct keiui_post *post;
	unsigned kept;
	unsigned index;
	unsigned from;
	unsigned to;

	/* Out of the list. */
	for (link = &app->windows; *link != NULL; link = &(*link)->app_next) {
		if (*link == window) {
			*link = window->app_next;
			break;
		}
	}

	/* The posts waiting for it are the application's now (their data is not lost). */
	keiui_lock_take(&app->post_lock);

	for (post = app->post_first; post != NULL; post = post->next) {
		if (post->event.window == window)
			app_post_orphan(&post->event);
	}

	keiui_lock_give(&app->post_lock);

	/* The queue again without the window's events, but its posts, which become the application's. */
	kept = 0;
	for (index = 0; index < app->event_count; index++) {
		from = (app->event_first + index) % KEIUI_APP_EVENTS;
		if (app->events[from].window == window && app->events[from].input.kind == KL_WINDOW_POST)
			app_post_orphan(&app->events[from]);
		if (app->events[from].window == window)
			continue;
		to = (app->event_first + kept) % KEIUI_APP_EVENTS;
		if (to != from)
			app->events[to] = app->events[from];
		kept++;
	}

	/* The queue is as long as what was kept. */
	app->event_count = kept;

	/* The window is no application's any more. */
	window->app = NULL;
	window->app_next = NULL;
}

/*
 * The application's menu service, opened the first time a window asks for
 * it; NULL when the compositor has none.
 */
struct kl_menu_service *
keiui_app_menu_service(
	struct kl_app *app)
{
	/* Once, found or not. */
	if (!app->menu_tried) {
		app->menu_tried = 1;
		app->menu_service = kl_menu_service_open(app->display);
	}

	/* The service, or NULL. */
	return app->menu_service;
}

/*
 * Finds a global of a display's application: its registry, with the
 * global's name and version, or NULL when the display is not an
 * application's (the caller searches itself) or the application has no
 * such global (*name is 0 then, and the caller need not search).
 */
struct wl_registry *
keiui_app_global(
	struct wl_display *display,
	const char *interface,
	uint32_t *name,
	uint32_t *version)
{
	struct kl_app *app;
	unsigned index;
	int same;

	/* Nothing found yet. */
	*name = 0;
	*version = 0;

	/* The display's application. */
	for (app = app_list; app != NULL; app = app->next) {
		if (app->display == display)
			break;
	}

	/* No application's display. */
	if (app == NULL)
		return NULL;

	/* Its global of the interface, the first announced. */
	for (index = 0; index < app->global_count; index++) {
		same = strcmp(app->globals[index].interface, interface);
		if (same == 0) {
			*name = app->globals[index].name;
			*version = app->globals[index].version;
			break;
		}
	}

	/* The registry it is bound from (with *name 0 when the compositor has none). */
	return app->registry;
}

/* Keeps a global the compositor announced. */
static void
app_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct kl_app *app;
	size_t length;

	/* A table with room, and a name that fits. */
	(void)registry;
	app = data;
	length = strlen(interface);
	if (app->global_count == KEIUI_APP_GLOBALS || length >= KEIUI_APP_INTERFACE)
		return;

	/* The global. */
	app->globals[app->global_count].name = name;
	app->globals[app->global_count].version = version;
	memcpy(app->globals[app->global_count].interface, interface, length + 1U);
	app->global_count++;
}

/* Forgets a global that went away. */
static void
app_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	struct kl_app *app;
	unsigned index;

	/* The global of the name, replaced by the last one. */
	(void)registry;
	app = data;
	for (index = 0; index < app->global_count; index++) {
		if (app->globals[index].name == name) {
			app->global_count--;
			app->globals[index] = app->globals[app->global_count];
			return;
		}
	}
}

/* Tells whether the compositor announced an interface. */
static int
app_has(
	const struct kl_app *app,
	const char *interface)
{
	unsigned index;
	int same;

	/* Any global of the interface. */
	for (index = 0; index < app->global_count; index++) {
		same = strcmp(app->globals[index].interface, interface);
		if (same == 0)
			return 1;
	}

	/* None. */
	return 0;
}

/* Reports how long a dispatch may wait: not past a due repeat, not at all while events wait. */
static int
app_wait(
	const struct kl_app *app,
	int timeout_ms,
	uint64_t now_us)
{
	const struct kl_window *window;
	int wait;

	/* Events wait already. */
	if (app->event_count != 0U)
		return 0;

	/* The nearest repeat. */
	for (window = app->windows; window != NULL; window = window->app_next) {
		wait = kl_window_repeat_wait(window, now_us);
		if (wait >= 0 && (timeout_ms < 0 || wait < timeout_ms))
			timeout_ms = wait;
	}

	/* The wait. */
	return timeout_ms;
}

/* Presses each held key whose repeat is due. */
static void
app_repeat(
	struct kl_app *app)
{
	struct kl_window *window;
	uint64_t now;

	/* Each window's held key, at the time now. */
	now = kl_clock_us();
	for (window = app->windows; window != NULL; window = window->app_next)
		(void)kl_window_repeat(window, now);
}

/* Takes the slot after the last event queued, zeroed; NULL when the queue is full. */
static struct kl_app_event *
app_slot(
	struct kl_app *app)
{
	struct kl_app_event *event;
	unsigned slot;

	/* A full queue (the user is far ahead of the program). */
	if (app->event_count == KEIUI_APP_EVENTS)
		return NULL;

	/* The slot, zeroed. */
	slot = (app->event_first + app->event_count) % KEIUI_APP_EVENTS;
	app->event_count++;
	event = &app->events[slot];
	memset(event, 0, sizeof(*event));

	/* Succeeded. */
	return event;
}

/* Queues a KL_APP_THEME event when the desktop's appearance changed (the theme handed out changed already). */
static void
app_appearance(
	void *data,
	unsigned appearance)
{
	struct kl_app *app;
	struct kl_app_event *event;

	/* The event, when the queue has room. */
	(void)appearance;
	app = data;
	event = app_slot(app);
	if (event == NULL)
		return;
	event->kind = KL_APP_THEME;
}

/*
 * Opens the desktop's settings for the language: the language now, and a
 * watch of its changes.  Without them the application stays in English.
 */
static void
app_language_open(
	struct kl_app *app)
{
	int setting;

	/* English until the desktop says otherwise. */
	(void)snprintf(app->language, sizeof(app->language), "%s", "en");

	/* The settings (no application's own file); none without memory. */
	app->settings = kl_settings_open(app->display, NULL);
	if (app->settings == NULL)
		return;

	/* The language now. */
	setting = kl_settings_get_int(app->settings, APP_LANGUAGE_KEY, 0);
	app_language_set(app, setting);

	/* And each change, as an event (a watch that cannot be made leaves the language as it is now). */
	(void)kl_settings_watch(app->settings, APP_LANGUAGE_KEY, app_language_changed, app, NULL);
}

/* Keeps the language a value of the setting names (English for a value no language has). */
static void
app_language_set(
	struct kl_app *app,
	int setting)
{
	const char *code;

	/* The language's name. */
	code = kl_tr_language_code(setting);
	if (code == NULL)
		code = "en";
	(void)snprintf(app->language, sizeof(app->language), "%s", code);
}

/*
 * Takes a change of the desktop's language (from kl_settings_dispatch, on
 * the application's thread): the value the change carries, not the
 * settings read again, so that each of several quick changes tells its
 * own language, in order.  The event waits in the posts' list when the
 * queue is full; without memory for that it is lost, and kl_app_language
 * still tells the language.
 */
static void
app_language_changed(
	void *data,
	const char *key,
	const char *value,
	unsigned flags)
{
	struct kl_app_event event;
	struct kl_app *app;
	char *end;
	long setting;

	/* The new value, a whole number; none (the compositor went) is English. */
	(void)key;
	(void)flags;
	app = data;
	setting = 0;
	if (value != NULL) {
		setting = strtol(value, &end, 10);
		if (end == value || *end != '\0')
			setting = 0;
	}

	/* Takes the language in. */
	app_language_set(app, (int)setting);

	/* The event, with the language's name. */
	memset(&event, 0, sizeof(event));
	event.kind = KL_APP_LANGUAGE;
	(void)snprintf(event.language, sizeof(event.language), "%s", app->language);
	(void)keiui_app_post_event(app, &event);
}

/*
 * Moves the posts that wait into the queue, as far as it has room: the
 * list is taken whole under the lock and what does not fit goes back to
 * its head, with the wake rung again so that the next dispatch does not
 * sleep on it.
 */
static void
app_posts_move(
	struct kl_app *app)
{
	struct keiui_post *list;
	struct keiui_post *post;
	struct keiui_post *last;
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
		queued = app_post_deliver(app, &list->event);
		if (!queued)
			break;
		post = list;
		list = list->next;
		free(post);
	}

	/* All moved. */
	if (list == NULL)
		return;

	/* The rest before what was posted meanwhile, and the wake again. */
	for (last = list; last->next != NULL; last = last->next)
		continue;

	/* Puts the rest back in front of what was posted meanwhile. */
	keiui_lock_take(&app->post_lock);

	last->next = app->post_first;
	if (app->post_first == NULL)
		app->post_last = last;
	app->post_first = list;

	keiui_lock_give(&app->post_lock);
	keiui_wake_ring(&app->post_wake);
}

/*
 * Queues one posted event on the application's thread: a window's post
 * among the window's inputs when the window is still the application's,
 * otherwise as the application's own.  Returns 1 when it was queued, 0 when
 * the queue has no room.
 */
static int
app_post_deliver(
	struct kl_app *app,
	struct kl_app_event *event)
{
	struct kl_app_event *slot;
	int present;
	int error;

	/* No room. */
	if (app->event_count == KEIUI_APP_EVENTS)
		return 0;

	/* A window's post to a window that is gone is the application's. */
	if (event->window != NULL) {
		present = app_has_window(app, event->window);
		if (!present)
			app_post_orphan(event);
	}

	/* A window's post, among its inputs. */
	if (event->kind == KL_APP_WINDOW && event->window != NULL) {
		error = keiui_window_post_input(event->window, event->input.code, event->input.data);
		if (error == 0)
			return 1;
		app_post_orphan(event);
	}

	/* The application's own. */
	slot = app_slot(app);
	if (slot == NULL)
		return 0;
	*slot = *event;

	/* Succeeded: queued. */
	return 1;
}

/*
 * Makes a window's post the application's (its window closed): KL_APP_POST
 * with the code and the data, window NULL.
 */
static void
app_post_orphan(
	struct kl_app_event *event)
{
	uint32_t code;
	void *data;

	/* A window's post keeps its code and data. */
	if (event->kind == KL_APP_WINDOW && event->input.kind == KL_WINDOW_POST) {
		code = event->input.code;
		data = event->input.data;
		memset(event, 0, sizeof(*event));
		event->kind = KL_APP_POST;
		event->code = code;
		event->data = data;
		}

	/* No window either way. */
	event->window = NULL;
}

/* Reports whether a window is one of the application's still. */
static int
app_has_window(
	const struct kl_app *app,
	const struct kl_window *window)
{
	const struct kl_window *each;

	/* Each window of the application. */
	for (each = app->windows; each != NULL; each = each->app_next) {
		if (each == window)
			return 1;
	}

	/* Not one of them. */
	return 0;
}

/*
 * Gives the next slot of the application's queue, zeroed (for the
 * library's other files), or NULL when the queue is full.
 */
struct kl_app_event *
keiui_app_slot(
	struct kl_app *app)
{
	struct kl_app_event *slot;

	/* Takes the slot the queue offers. */
	slot = app_slot(app);

	/* The slot, or NULL when the queue is full. */
	return slot;
}

/*
 * Takes a process's events out of the application's queue (the others
 * keep their order).
 */
void
keiui_app_forget_process(
	struct kl_app *app,
	const struct kl_process *process)
{
	unsigned kept;
	unsigned index;
	unsigned from;
	unsigned to;

	/* The queue again without the process's events. */
	kept = 0;
	for (index = 0; index < app->event_count; index++) {
		from = (app->event_first + index) % KEIUI_APP_EVENTS;
		if (app->events[from].process == process)
			continue;
		to = (app->event_first + kept) % KEIUI_APP_EVENTS;
		if (to != from)
			app->events[to] = app->events[from];
		kept++;
	}

	/* The queue is as long as what was kept. */
	app->event_count = kept;
}

/*
 * Takes a copy's events out of the application's queue (the others keep
 * their order).
 */
void
keiui_app_forget_instance(
	struct kl_app *app,
	const struct kl_instance *instance)
{
	unsigned kept;
	unsigned index;
	unsigned from;
	unsigned to;

	/* The queue again without the copy's events. */
	kept = 0;
	for (index = 0; index < app->event_count; index++) {
		from = (app->event_first + index) % KEIUI_APP_EVENTS;
		if (app->events[from].instance == instance)
			continue;
		to = (app->event_first + kept) % KEIUI_APP_EVENTS;
		if (to != from)
			app->events[to] = app->events[from];
		kept++;
	}

	/* The queue is as long as what was kept. */
	app->event_count = kept;
}
