/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The recent files, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_RECENT_H
#define KEILAND_RECENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The recent files.
 *
 * One list of recently used files for all applications, newest first: a
 * file manager shows it as Recents, an application may offer it as "open
 * recent".  An application adds a file when it opens or saves one.  Every
 * call returns 0 or an errno value.
 */

/* The longest path and application name an entry holds, with the terminating NUL. */
#define KL_RECENT_PATH_MAX	4096U
#define KL_RECENT_NAME_MAX	64U

/* How many entries the list keeps (the oldest go first). */
#define KL_RECENT_KEPT		256U

/*
 * One entry of the recent list: the file's absolute path, the application
 * that used it (its app_id) and when, in seconds since the epoch.
 */
struct kl_recent_item {
	char path[KL_RECENT_PATH_MAX];
	char application[KL_RECENT_NAME_MAX];
	int64_t time;
};

/*
 * Adds a file (an absolute path) to the recent list, or makes it the
 * newest when it is listed.
 */
int kl_recent_add(const char *path, const char *application);

/*
 * Reads the recent list, newest first, into up to capacity items.
 */
int kl_recent_list(struct kl_recent_item *items, size_t capacity, size_t *count);

/*
 * Takes a file off the recent list.
 */
int kl_recent_remove(const char *path);

/*
 * Empties the recent list (KL_VERSION 50: Files' Recents, "Clear Recents").
 */
int kl_recent_clear(void);

/*
 * Chooses whether the recent list is kept (KL_VERSION 50: Settings'
 * Storage, "Keep recent items"): 0 empties it and kl_recent_add adds
 * nothing until 1 starts it again.  The choice is the user's, for every
 * application.
 */
int kl_recent_set_keep(int keep);

/*
 * Tells whether the recent list is kept (*keep is 1) or stopped (0).
 */
int kl_recent_keep(int *keep);

/*
 * Gives a stamp of the recent list (KL_VERSION 66): a value that changes
 * whenever the list changes, is emptied, or is stopped or started again, so a
 * program showing the list ("Open Recent") reads it again when the stamp is
 * not the one it read it at (when its window gets the keyboard, say).
 * Returns 0, or an errno value.
 */
int kl_recent_stamp(uint64_t *stamp);

#ifdef __cplusplus
}
#endif

#endif
