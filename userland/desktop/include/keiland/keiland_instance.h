/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One copy of a program, and the activation, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_INSTANCE_H
#define KEILAND_INSTANCE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_surface;
struct kl_app;

/*
 * One copy of a program, and the activation.
 *
 * A program that keeps one window (Settings) runs once per user: a second
 * start hands its request -- a line of text the program gives meaning to,
 * such as a page to open -- to the copy that runs, with an activation
 * token, and ends.  The copy that runs takes the request, carries it out
 * and brings its window to the front with the token (kl_activate).  The
 * copies find each other through a socket in the user's runtime directory
 * (XDG_RUNTIME_DIR), which must be the user's own and closed to others.
 * The copy that runs binds itself to its application (kl_instance_bind):
 * the requests then arrive as the application's KL_APP_INSTANCE events.
 *
 * An activation token is a compositor's permission to bring a window to
 * the front (xdg_activation_v1).  A program started by the desktop has one
 * in XDG_ACTIVATION_TOKEN; a program may ask the compositor for one to
 * hand to a program it starts (kl_activation_token).
 */

/* The longest request (its NUL counted), and the longest activation token. */
#define KL_INSTANCE_REQUEST_MAX		256U
#define KL_ACTIVATION_TOKEN_MAX		256U

/* The copy of a program that runs: its socket. */
struct kl_instance;

/*
 * Makes this program the one copy of the name (lowercase letters, digits
 * and '-'), or hands request to the copy that runs.  Returns 0 with
 * *instance set when this is the one copy (it listens for later starts),
 * 0 with *instance NULL when the request was handed over (the program
 * ends), or an error (the program runs on its own, as without the call):
 * EINVAL for a bad name or request, ENOTSUP without a private runtime
 * directory.  The token handed over is one asked of the compositor (which
 * grants it to a program that has just started), or XDG_ACTIVATION_TOKEN's
 * when none can be asked for; the variable is removed either way.
 */
int kl_instance_open(const char *name, const char *request, struct kl_instance **instance);

/*
 * KL_VERSION 99: has the requests of later starts arrive as the
 * application's KL_APP_INSTANCE events (event.instance), from now until the
 * copy is closed or the application closes (whichever comes first).  A
 * thread of the copy's own takes them; it keeps up to eight not yet taken.
 * Returns 0, EINVAL (no copy, or bound already), or another error of the
 * system.
 */
int kl_instance_bind(struct kl_instance *instance, struct kl_app *app);

/*
 * Takes one request handed over: 1 with the request and the token (an
 * empty string when there is none) copied, 0 when none waits.
 */
int kl_instance_take(struct kl_instance *instance, char *request, size_t request_size, char *token, size_t token_size);

/*
 * Stops being the one copy: the socket goes.
 */
void kl_instance_close(struct kl_instance *instance);

/*
 * Asks the compositor for an activation token for a program to be started
 * (app_id, NULL for none), from a surface the user works in (NULL for
 * none).  Returns 0 with the token copied, ENOTSUP when the compositor has
 * no activation, or another error.
 */
int kl_activation_token(struct wl_display *display, struct wl_surface *surface, const char *app_id, char *token, size_t size);

/*
 * Brings a surface's window to the front with an activation token (the
 * compositor decides whether the token allows it).  Returns 0 when it was
 * asked, ENOTSUP when the compositor has no activation.
 */
int kl_activate(struct wl_display *display, struct wl_surface *surface, const char *token);

#ifdef __cplusplus
}
#endif

#endif
