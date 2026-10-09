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
 * The windows' inputs and the descriptors watched become one queue of
 * events.  A dispatch waits for the compositor or a descriptor, no longer
 * than until a held key's repeat is due, reads and runs the compositor's
 * events, queues the descriptors that became ready, and then presses the
 * keys whose repeat is due -- after the events read with them, so that a
 * release among them stops a repeat first (BUG-111).
 *
 * The library runs on one thread; the list of the applications open is
 * the library's own.
 */

#include "window.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

/* The applications open, newest first. */
static struct kl_app *app_list;

static void app_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void app_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static int app_has(const struct kl_app *app, const char *interface);
static void app_fd_events(struct kl_app *app, const struct pollfd *descriptors, unsigned used);
static int app_wait(const struct kl_app *app, int timeout_ms, uint64_t now_us);
static void app_repeat(struct kl_app *app);
static struct kl_app_event *app_slot(struct kl_app *app);
static void app_appearance(void *data, unsigned appearance);

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

	/* No application, nothing to close. */
	if (app == NULL)
		return;

	/* Each window (its close takes it out of the list). */
	while (app->windows != NULL)
		kl_window_close(app->windows);

	/* The appearance, the system and the menu service. */
	kl_appearance_close(app->appearance);
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
	free(app->application);
	free(app);
}

/*
 * Waits for the compositor or a watched descriptor and queues what
 * happened (app.c's header).  Returns 0, or -1 when the connection is
 * broken.
 */
int
kl_app_dispatch(
	struct kl_app *app,
	int timeout_ms)
{
	struct pollfd descriptors[1U + KL_APP_FDS_MAX];
	struct kl_window *window;
	unsigned index;
	unsigned used;
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

	/* No longer than until a repeat is due, and not at all while events wait. */
	timeout_ms = app_wait(app, timeout_ms, kl_clock_us());

	/* The compositor's descriptor, then each one watched. */
	descriptors[0].fd = wl_display_get_fd(app->display);
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
	used = 1U;
	for (index = 0; index < app->fd_count; index++) {
		descriptors[used].fd = app->fds[index];
		descriptors[used].events = 0;
		if ((app->fd_events[index] & KL_APP_FD_READ) != 0U)
			descriptors[used].events |= POLLIN;
		if ((app->fd_events[index] & KL_APP_FD_WRITE) != 0U)
			descriptors[used].events |= POLLOUT;
		descriptors[used].revents = 0;
		used++;
	}

	/* Waits for any of them. */
	status = poll(descriptors, (nfds_t)used, timeout_ms);

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

	/* The descriptors that became ready, then the repeats now due. */
	app_fd_events(app, descriptors, used);
	app_repeat(app);

	/* Succeeded: the events so far are queued. */
	return 0;
}

/*
 * Watches a descriptor (or stops watching it, events 0).
 */
int
kl_app_watch_fd(
	struct kl_app *app,
	int fd,
	unsigned events)
{
	unsigned index;

	/* A descriptor, waited for reading or writing. */
	if (fd < 0 || (events & ~(KL_APP_FD_READ | KL_APP_FD_WRITE)) != 0U)
		return EINVAL;

	/* One watched already: its new events, or out of the table. */
	for (index = 0; index < app->fd_count; index++) {
		if (app->fds[index] != fd)
			continue;
		if (events != 0U) {
			app->fd_events[index] = events;
			return 0;
		}

		/* Out of the table: the last one takes its place. */
		app->fd_count--;
		app->fds[index] = app->fds[app->fd_count];
		app->fd_events[index] = app->fd_events[app->fd_count];
		return 0;
	}

	/* Not watched, and not to be. */
	if (events == 0U)
		return 0;

	/* A new one, while there is room. */
	if (app->fd_count == KL_APP_FDS_MAX)
		return ENOSPC;
	app->fds[app->fd_count] = fd;
	app->fd_events[app->fd_count] = events;
	app->fd_count++;

	/* Succeeded. */
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

	/* The oldest event, and the queue moves on. */
	*event = app->events[app->event_first];
	app->event_first = (app->event_first + 1U) % KEIUI_APP_EVENTS;
	app->event_count--;

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
	event->fd = -1;
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

	/* The queue again without the window's events. */
	kept = 0;
	for (index = 0; index < app->event_count; index++) {
		from = (app->event_first + index) % KEIUI_APP_EVENTS;
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

/* Queues an event for each watched descriptor that became ready. */
static void
app_fd_events(
	struct kl_app *app,
	const struct pollfd *descriptors,
	unsigned used)
{
	struct kl_app_event *event;
	unsigned ready;
	unsigned index;

	/* Each watched descriptor's answer. */
	for (index = 1U; index < used; index++) {
		/* What it became. */
		ready = 0;
		if ((descriptors[index].revents & POLLIN) != 0)
			ready |= KL_APP_FD_READ;
		if ((descriptors[index].revents & POLLOUT) != 0)
			ready |= KL_APP_FD_WRITE;
		if ((descriptors[index].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
			ready |= KL_APP_FD_HANGUP;
		if (ready == 0U)
			continue;

		/* Its event; a full queue drops it (the next dispatch finds it ready again). */
		event = app_slot(app);
		if (event == NULL)
			return;
		event->kind = KL_APP_FD;
		event->fd = descriptors[index].fd;
		event->ready = ready;
	}
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
