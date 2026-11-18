/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop's settings, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_SETTINGS_H
#define KEILAND_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;

/*
 * The desktop's settings: every application reads, changes and watches them
 * here, and opens no settings file itself.  Each key is resolved where it
 * lives: the compositor's (the wallpaper, the windows' opacity, the pointer,
 * the keyboards' repeat, the sound) through Keiland's system extension, an
 * application's own (terminal.*) in its file under ~/.config/keiland.  The
 * application cannot tell the two apart.
 *
 * Changes are watched, not polled: the compositor tells every client each
 * change, and kl_settings_dispatch, called after the display's events are
 * read, runs the watches.  One thread uses one kl_settings.
 */

/* The longest key and value, with their NUL. */
#define KL_SETTINGS_KEY_MAX	64U
#define KL_SETTINGS_VALUE_MAX	256U

/* A value the user did not choose: its resolver's default. */
#define KL_SETTINGS_DEFAULT	0x1U

/*
 * One application's view of the desktop's settings: the compositor's
 * values it was told, its own file's, its watches and its requests not
 * answered yet.  It lives from kl_settings_open to kl_settings_close.
 */
struct kl_settings;

/*
 * Called from kl_settings_dispatch for a key whose value changed; value is
 * NULL when the key has none now (not reported yet, or the compositor
 * went).
 */
typedef void (*kl_settings_watch_fn)(void *data, const char *key, const char *value, unsigned flags);

/*
 * Opens the settings on a display (app names the application's own
 * settings, "terminal" for terminal.*; NULL for none).  It waits once for
 * the compositor's settings.  Returns NULL with errno ENOMEM; without
 * Keiland's extension it opens all the same and the compositor's keys
 * answer ENOTSUP.
 */
struct kl_settings *kl_settings_open(struct wl_display *display, const char *app);

/*
 * Closes the settings.
 */
void kl_settings_close(struct kl_settings *settings);

/*
 * Copies a key's value and its flags (flags may be NULL).  Returns 0,
 * ENOENT for a key the desktop does not have, ENOTSUP for a compositor's
 * key without the compositor's extension, EAGAIN while it is not reported
 * yet (the sound before audiod), or ERANGE when it does not fit.
 */
int kl_settings_get(const struct kl_settings *settings, const char *key, char *value, size_t size, unsigned *flags);

/*
 * Reports a key's value as a whole number; fallback when it is not known
 * or not a number.
 */
int kl_settings_get_int(const struct kl_settings *settings, const char *key, int fallback);

/*
 * Asks for a key to take a value; request (may be NULL) names the answer
 * kl_settings_take_result gives.  The value comes back as a change.
 * Returns 0 when asked, ENOENT, ENOTSUP, EPERM (a key only reported),
 * EINVAL (a value outside the key's type or range), or an errno value of
 * the application's file.
 */
int kl_settings_set(struct kl_settings *settings, const char *key, const char *value, uint32_t *request);

/*
 * Asks for a key to take a whole number, as kl_settings_set.
 */
int kl_settings_set_int(struct kl_settings *settings, const char *key, int value, uint32_t *request);

/*
 * Asks for a key to go back to its default, as kl_settings_set.
 */
int kl_settings_reset(struct kl_settings *settings, const char *key, uint32_t *request);

/*
 * Watches the keys that start with prefix ("" for every key); *watch (may
 * be NULL) names the watch for kl_settings_unwatch.  Returns 0, EINVAL or
 * ENOMEM.
 */
int kl_settings_watch(struct kl_settings *settings, const char *prefix, kl_settings_watch_fn fn, void *data, unsigned *watch);

/*
 * Stops a watch (also from within a watch's callback).
 */
void kl_settings_unwatch(struct kl_settings *settings, unsigned watch);

/*
 * Takes the compositor's events the display has read, then runs the
 * watches of the keys that changed.  It never waits.  Returns 0, or EPIPE
 * once the compositor went (its keys answer ENOTSUP from then on).
 */
int kl_settings_dispatch(struct kl_settings *settings);

/*
 * Takes one finished request: 1 with its number and its error (0, EPERM,
 * ENOTSUP, EBUSY, EINVAL, ENODEV, EIO), 0 when none is finished.
 */
int kl_settings_take_result(struct kl_settings *settings, uint32_t *request, int *error);

#ifdef __cplusplus
}
#endif

#endif
