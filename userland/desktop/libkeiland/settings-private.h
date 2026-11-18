/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of libkeiland's settings (settings.c; WS135, plan/ws135/
 * design.md section 3): the cache of the values, the watches and the
 * finished requests (settings-cache.c), and the application's own file
 * (settings-app.c).  Neither knows Wayland, so that the host tests build
 * them alone (plan/tools/settings/host-settings.sh).
 */

#ifndef KEILAND_SETTINGS_PRIVATE_H
#define KEILAND_SETTINGS_PRIVATE_H

#include <keiland/keiland.h>

#include "userland/desktop/settings-keys/settings-keys.h"

#include <stdint.h>

/* The most keys the cache holds (the table's rows and the prefix rows' keys), watches, and finished requests kept. */
#define SETTINGS_CACHE_KEYS	64U
#define SETTINGS_CACHE_WATCHES	32U
#define SETTINGS_CACHE_RESULTS	32U

/* The longest application name, and the longest path of its file, with their NULs. */
#define SETTINGS_APP_NAME_MAX	32U
#define SETTINGS_APP_PATH_MAX	1024U

/*
 * One key as the cache holds it: its name and its row of the table (a
 * prefix row's key has a name of its own).
 *
 * value, flags and present are the state in effect (present zero: no value,
 * the value is empty).  The pending ones are what the compositor sent
 * since its last done; touched says the key is among them.  The told ones
 * are what the watches last heard, so that a key that changed and changed
 * back within one dispatch is not told.  changed marks a key to compare at
 * the next notify.
 */
struct settings_cache_entry {
	char name[KL_SETTINGS_KEY_MAX];
	const struct kl_settings_key *key;
	char value[KL_SETTINGS_VALUE_MAX];
	unsigned flags;
	unsigned present;
	char pending_value[KL_SETTINGS_VALUE_MAX];
	unsigned pending_flags;
	unsigned pending_present;
	unsigned touched;
	char told_value[KL_SETTINGS_VALUE_MAX];
	unsigned told_flags;
	unsigned told_present;
	unsigned changed;
};

/*
 * One watch: the keys it covers (those starting with prefix), its callback
 * and data, its number, and whether it is in use or was stopped while the
 * watches were being run (removed: taken out after the run).
 */
struct settings_cache_watch {
	char prefix[KL_SETTINGS_KEY_MAX];
	kl_settings_watch_fn fn;
	void *data;
	unsigned id;
	unsigned used;
	unsigned removed;
	unsigned added;
};

/* One finished request and its error. */
struct settings_cache_result {
	uint32_t request;
	int error;
};

/*
 * The values, the watches and the finished requests of one kl_settings.
 * notifying is nonzero while the watches run (a watch added then hears
 * the next change, one stopped then hears nothing more).  results is a
 * ring: result_head is the oldest, result_count how many wait.
 */
struct settings_cache {
	struct settings_cache_entry entries[SETTINGS_CACHE_KEYS];
	unsigned count;
	struct settings_cache_watch watches[SETTINGS_CACHE_WATCHES];
	unsigned next_watch;
	unsigned notifying;
	struct settings_cache_result results[SETTINGS_CACHE_RESULTS];
	unsigned result_head;
	unsigned result_count;
};

/*
 * An application's own settings file: the application's name (the keys'
 * prefix), the file (~/.config/keiland/<name>.conf) and its folder, and
 * whether it is in use.
 */
struct settings_app {
	char name[SETTINGS_APP_NAME_MAX];
	char folder[SETTINGS_APP_PATH_MAX];
	char path[SETTINGS_APP_PATH_MAX];
	unsigned used;
};

/* settings-cache.c */
void settings_cache_init(struct settings_cache *cache);
struct settings_cache_entry *settings_cache_find(struct settings_cache *cache, const char *key);
void settings_cache_pending(struct settings_cache *cache, const char *key, const char *value, unsigned flags, unsigned present);
void settings_cache_done(struct settings_cache *cache);
void settings_cache_set(struct settings_cache *cache, const char *key, const char *value, unsigned flags, unsigned present);
void settings_cache_lost(struct settings_cache *cache, unsigned resolver);
void settings_cache_settle(struct settings_cache *cache);
int settings_cache_get(const struct settings_cache *cache, const char *key, char *value, size_t size, unsigned *flags);
int settings_cache_watch(struct settings_cache *cache, const char *prefix, kl_settings_watch_fn fn, void *data, unsigned *watch);
void settings_cache_unwatch(struct settings_cache *cache, unsigned watch);
void settings_cache_notify(struct settings_cache *cache);
void settings_cache_result(struct settings_cache *cache, uint32_t request, int error);
int settings_cache_take_result(struct settings_cache *cache, uint32_t *request, int *error);

/* settings-app.c */
int settings_app_open(struct settings_app *app, const char *name, const char *home);
void settings_app_load(const struct settings_app *app, struct settings_cache *cache);
int settings_app_write(const struct settings_app *app, const char *key, const char *value);

#endif
