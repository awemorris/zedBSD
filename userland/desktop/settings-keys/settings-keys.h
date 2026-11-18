/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The table of the desktop's settings (WS135, plan/ws135/design.md section
 * 2.1): each key, who resolves it (the compositor, or libkeiland from the
 * application's own file), its type and its range.
 *
 * Neither a client nor a server: libkeiland and the compositor each build
 * settings-keys.c, so that both check a value the same way.  A new setting
 * is a new row of the table.
 */

#ifndef KEILAND_SETTINGS_KEYS_H
#define KEILAND_SETTINGS_KEYS_H

#include <stddef.h>

/* The longest key and value, with their NUL (keiland.h gives the same). */
#define KL_SETTINGS_KEY_MAX	64U
#define KL_SETTINGS_VALUE_MAX	256U

/* Who resolves a key. */
#define KL_SETTINGS_RESOLVER_COMPOSITOR	1U
#define KL_SETTINGS_RESOLVER_APP	2U

/* A key's type. */
#define KL_SETTINGS_TYPE_INT		1U
#define KL_SETTINGS_TYPE_BOOL		2U
#define KL_SETTINGS_TYPE_PATH		3U
#define KL_SETTINGS_TYPE_OPENER		4U

/* A key a client reads but never sets (the compositor's report). */
#define KL_SETTINGS_KEY_READ_ONLY	0x1U

/* A key kept in the desktop's file at the session's end. */
#define KL_SETTINGS_KEY_KEPT		0x2U

/*
 * A row whose name is a prefix: the keys are the prefix and a MIME type
 * (files.open-with.image/png), made as they are chosen (plan/ws135/
 * design.md section 2.4).
 */
#define KL_SETTINGS_KEY_PREFIX		0x4U

/*
 * One setting: its name, its resolver, its type, the range of a number
 * (minimum to maximum; 0 and 1 for a Boolean), the default of a number its
 * resolver gives when nothing else does, and its flags.
 *
 * The rows live in settings-keys.c's constant table for the program's life.
 */
struct kl_settings_key {
	const char *name;
	unsigned resolver;
	unsigned type;
	int minimum;
	int maximum;
	int fallback;
	unsigned flags;
};

size_t kl_settings_key_count(void);
const struct kl_settings_key *kl_settings_key_at(size_t index);
const struct kl_settings_key *kl_settings_key_find(const char *name);
int kl_settings_key_check(const struct kl_settings_key *key, const char *value);
int kl_settings_key_number(const struct kl_settings_key *key, const char *value, int *number);
int kl_settings_name_valid(const char *name);
int kl_settings_value_valid(const char *value);

#endif
