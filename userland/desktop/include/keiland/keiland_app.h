/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The application, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_APP_H
#define KEILAND_APP_H

#include <keiland/keiland_tr.h>
#include <keiland/keiland_window.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KL_VERSION 26: the application.  One kl_app is one connection to the
 * compositor: its globals are learnt with one roundtrip when it opens, and
 * its windows, menus, titlebars and glass bind what they need from that one
 * registry without searching again.  Its windows' input, the actions chosen
 * in their menus and controls, the program's posts and the news of its
 * processes arrive as one queue of kl_app_event values, in the order they happened: the application
 * waits with kl_app_dispatch and takes them with kl_app_take (a window of an
 * application has no queue of its own; kl_window_take finds nothing).  A held
 * key repeats by itself within kl_app_dispatch, after the events read with it
 * (a release read in the same dispatch stops it first).  The desktop's system
 * (network, sound, power, kl_system) is the application's too.  A window made
 * by kl_window_open is still a connection of its own.
 *
 * Every call is made from the one thread that opened the application, but
 * kl_app_post and kl_window_post (KL_VERSION 97), which the program's other
 * threads call to hand the application's thread an event of their own.
 */
struct kl_app;
struct kl_system;
struct kl_glass_panel;
struct kl_process;
struct kl_instance;

/* The kinds of an application's event (2, a watched descriptor's, is gone since KL_VERSION 102). */
#define KL_APP_WINDOW		1U
#define KL_APP_THEME		3U
#define KL_APP_POST		4U
#define KL_APP_PROCESS		5U
#define KL_APP_INSTANCE	6U
#define KL_APP_LANGUAGE	7U

/*
 * What an application is opened with.  Either pointer may be NULL:
 * display (the WAYLAND_DISPLAY one) and application (the app_id its
 * windows get when their options name none).
 */
struct kl_app_options {
	const char *display;
	const char *application;
};

/*
 * One event of an application: its kind; for KL_APP_WINDOW the window and
 * its input (as kl_window_take gave it; KL_WINDOW_ACTION for an action
 * chosen); KL_APP_THEME (KL_VERSION 35) says the desktop's appearance changed and
 * kl_theme_default's colours with it, for the application to draw its
 * windows again (kl_appearance_get(NULL) tells which); KL_APP_POST
 * (KL_VERSION 97) is the program's own, with the code and the data
 * kl_app_post was given (or kl_window_post's, window NULL, when the window
 * closed before it came); KL_APP_PROCESS (KL_VERSION 98) is a process's
 * news (<keiland/keiland_process.h>): the process, and in ready what it
 * tells (KL_PROCESS_READY_*); KL_APP_INSTANCE (KL_VERSION 99) says later
 * starts of the program handed requests to its copy that runs (instance,
 * bound with kl_instance_bind): the program takes them with
 * kl_instance_take until none is left; KL_APP_LANGUAGE (KL_VERSION 101)
 * says the desktop's language changed, and language names the language it
 * changed to ("en", "ja", ...), as kl_tr_open takes it.  Changes that come
 * one after another come as events in the same order: the last one tells
 * the language now.
 */
struct kl_app_event {
	unsigned kind;
	struct kl_window *window;
	struct kl_window_event input;
	unsigned ready;
	uint32_t code;
	void *data;
	struct kl_process *process;
	struct kl_instance *instance;
	char language[KL_TR_NAME_MAX + 1U];
};

/*
 * Opens an application: connects, learns the globals and binds what every
 * window shares.  Returns NULL with errno set: a connection's error,
 * EOPNOTSUPP (no compositor or shell), EPROTO, ENOMEM.
 */
struct kl_app *kl_app_open(const struct kl_app_options *options);

/*
 * Closes the windows still open, the system, and the connection.
 */
void kl_app_close(struct kl_app *app);

/*
 * Waits up to a timeout (milliseconds, -1 for ever; no wait while events
 * are queued or a repeat is due) for the compositor or a post, and queues
 * what happened.  Returns 0, or -1 when the
 * connection is broken.
 */
int kl_app_dispatch(struct kl_app *app, int timeout_ms);

/*
 * KL_VERSION 101: the desktop's language as the application last heard it
 * ("en", "ja", ...; "en" when the compositor does not tell): at
 * kl_app_open, then at each change a dispatch reads (the change's
 * KL_APP_LANGUAGE event comes after).
 */
const char *kl_app_language(const struct kl_app *app);

/*
 * KL_VERSION 97: posts an event of the program's own (KL_APP_POST with code
 * and data) to the application, from any thread.  The application's own
 * thread queues it at once, after the events queued so far; another
 * thread's post wakes kl_app_dispatch and is queued there, after the
 * compositor's events read with it.  The posts of one thread arrive in the
 * order they were made, and none is dropped: what the queue has no room for
 * waits for the next dispatch.  The data goes to the one who takes the
 * event.  The program ends the threads that post before it closes the
 * application (the posts not taken by then are dropped, their data with
 * them).  Returns 0, EINVAL, or ENOMEM.
 */
int kl_app_post(struct kl_app *app, uint32_t code, void *data);

/*
 * Takes the oldest event.  Returns 1 with it in *event, 0 when none is queued.
 */
int kl_app_take(struct kl_app *app, struct kl_app_event *event);

/*
 * The application's system, opened the first time it is asked for; NULL
 * with errno set when the compositor has none (kl_system_open).
 */
struct kl_system *kl_app_system(struct kl_app *app);

/*
 * KL_VERSION 49: posts a notification of the application (its application ID
 * as the name shown), without waiting for its number.  Returns 0 when asked,
 * ENOTSUP when the compositor takes none, or EINVAL.
 */
int kl_app_notify(struct kl_app *app, const char *title, const char *body);

/* KL_VERSION 79: kl_app_notify with what the lock screen shows of it (kl_system_notify_lock). */
int kl_app_notify_lock(struct kl_app *app, const char *title, const char *body, const char *lock_text);

/* The application's connection, for libkeiland's other objects. */
struct wl_display *kl_app_display(const struct kl_app *app);

/*
 * Writes the directory where the application keeps its data (KL_VERSION 105):
 * Wayland systems use $XDG_DATA_HOME/keiland/<application> (with
 * $HOME/.local/share when XDG_DATA_HOME is unset or not absolute); the
 * directory is not created.  application is the kl_app_options application
 * the app was opened with.  Returns 0, EINVAL (NULL arguments, size 0, no
 * application name), ENOENT (neither variable set), ERANGE (buffer too small;
 * it then holds the empty string).
 */
int kl_app_storage_directory(const struct kl_app *app, char *buffer, size_t size);

/*
 * Writes the directory of the bundled read-only data (KL_VERSION 105):
 * KEILAND_DATADIR ("/usr/share") on the Wayland systems.  Returns 0, EINVAL,
 * ERANGE.
 */
int kl_data_directory(char *buffer, size_t size);

/*
 * Makes a window of the application (options->display is not used; a
 * NULL application takes the application's).  It is configured when this
 * returns, as kl_window_open's is.  Returns NULL with errno set as
 * kl_window_open does.
 */
struct kl_window *kl_app_window_create(struct kl_app *app, const struct kl_window_options *options);

#ifdef __cplusplus
}
#endif

#endif
