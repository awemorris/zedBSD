/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The table of the desktop's settings and the checks of their values
 * (settings-keys.h).
 */

#include "userland/desktop/settings-keys/settings-keys.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Every setting the desktop knows, in the order the compositor reports
 * them.  The compositor's keys come first; the ranges are the ones
 * the compositor and Settings used before (ws089-p007).  appearance.accent
 * (ws179-p001) is the accent the user chose, 0 blue to 7 graphite
 * (artwork/accent.h).  The pointer is set for
 * a mouse and for the touch pads apart (ws089-p024): the speed in percent,
 * the acceleration's level (0 none, 1 mild, 2 medium, 3 strong) and the
 * natural scrolling; a mouse 150% and strong without natural scrolling, a
 * touch pad 100% and medium (the curve of ws159-p004) with it.  The
 * language of the interface (WS158) is 0 English or 1 Japanese
 * (kl_tr_language_code).  welcome.done (ws164-p002) is 1 once an account
 * has taken or skipped the Welcome.  mail.codes.browser (ws169-p002) lets
 * the browser hear the arrivals of mail and their sign-in codes, off until
 * the user allows it in Mail.  notify.allow.<application> (ws177-p026)
 * lets an application's notifications show, on unless the user turns it
 * off on Settings' Notifications page (Mail, Calendar, Phone, Browser).
 * phone.backend (ws170-p004) chooses the
 * phone's backend: 0 none, 1 loopback (the tests' and a demo's), 2 the paired
 * phone by Bluetooth (ws197-p004a, set by Settings' "Use as phone").  The one
 * pointer setting of before (pointer.*) is only read, to be moved to the
 * mouse's (settings.c).  Files keeps the width of each list column the
 * user dragged (BUG-220), in pixels, 0 for the column's own width.
 * notes.clean-copy-told (ws177-p011) is 1 once Notes told, after a clean
 * copy, that the notebook itself keeps what was removed in its history.
 * power.sleep.ac and power.sleep.battery (ws052-p012) are the minutes
 * without input before the machine sleeps, on the power adapter and on
 * battery (0 never); the screen goes out at half that time.
 */
static const struct kl_settings_key settings_keys[] = {
	{ "wallpaper", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_PATH, 0, 0, 0, KL_SETTINGS_KEY_KEPT },
	{ "window.opacity", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 85, 100, 100, KL_SETTINGS_KEY_KEPT },
	{ "window.frosted", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 1, KL_SETTINGS_KEY_KEPT },
	{ "appearance.dark", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 0, KL_SETTINGS_KEY_KEPT },
	{ "appearance.accent", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 0, 7, 0, KL_SETTINGS_KEY_KEPT },
	{ "mouse.speed", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 25, 300, 150, KL_SETTINGS_KEY_KEPT },
	{ "mouse.acceleration", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 0, 3, 3, KL_SETTINGS_KEY_KEPT },
	{ "mouse.natural", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 0, KL_SETTINGS_KEY_KEPT },
	{ "touchpad.speed", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 25, 300, 100, KL_SETTINGS_KEY_KEPT },
	{ "touchpad.acceleration", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 0, 3, 2, KL_SETTINGS_KEY_KEPT },
	{ "touchpad.natural", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 1, KL_SETTINGS_KEY_KEPT },
	{ "pointer.speed", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 25, 300, 100, KL_SETTINGS_KEY_KEPT | KL_SETTINGS_KEY_READ_ONLY },
	{ "pointer.natural", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 0, KL_SETTINGS_KEY_KEPT | KL_SETTINGS_KEY_READ_ONLY },
	{ "keyboard.repeat.rate", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 5, 60, 25, KL_SETTINGS_KEY_KEPT },
	{ "keyboard.repeat.delay", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 150, 1000, 400, KL_SETTINGS_KEY_KEPT },
	{ "ime.method", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 0, 2, 1, KL_SETTINGS_KEY_KEPT },
	{ "ui.language", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 0, 1, 0, KL_SETTINGS_KEY_KEPT },
	{ "welcome.done", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 0, KL_SETTINGS_KEY_KEPT },
	{ "sound.volume", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 0, 100, 100, KL_SETTINGS_KEY_KEPT },
	{ "sound.muted", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 0, KL_SETTINGS_KEY_KEPT },
	{ "sound.available", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 0, KL_SETTINGS_KEY_READ_ONLY },
	{ "mail.codes.browser", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 0, KL_SETTINGS_KEY_KEPT },
	{ "notify.allow.mailer", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 1, KL_SETTINGS_KEY_KEPT },
	{ "notify.allow.calendar", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 1, KL_SETTINGS_KEY_KEPT },
	{ "notify.allow.phone", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 1, KL_SETTINGS_KEY_KEPT },
	{ "notify.allow.browser", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_BOOL, 0, 1, 1, KL_SETTINGS_KEY_KEPT },
	{ "phone.backend", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 0, 2, 0, KL_SETTINGS_KEY_KEPT },
	{ "power.sleep.ac", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 0, 240, 30, KL_SETTINGS_KEY_KEPT },
	{ "power.sleep.battery", KL_SETTINGS_RESOLVER_COMPOSITOR, KL_SETTINGS_TYPE_INT, 0, 240, 15, KL_SETTINGS_KEY_KEPT },
	{ "terminal.ambiguous-wide", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_BOOL, 0, 1, 0, 0U },
	{ "terminal.font-size", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_INT, 8, 32, 16, 0U },
	{ "terminal.theme", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_INT, 0, 2, 0, 0U },
	{ "files.column-width.kind", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_INT, 0, 2000, 0, 0U },
	{ "files.column-width.size", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_INT, 0, 2000, 0, 0U },
	{ "files.column-width.modified", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_INT, 0, 2000, 0, 0U },
	{ "files.column-width.changed", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_INT, 0, 2000, 0, 0U },
	{ "files.column-width.owner", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_INT, 0, 2000, 0, 0U },
	{ "files.column-width.location", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_INT, 0, 2000, 0, 0U },
	{ "files.column-width.deleted", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_INT, 0, 2000, 0, 0U },
	{ "files.open-with.", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_OPENER, 0, 0, 0, KL_SETTINGS_KEY_PREFIX },
	{ "notes.clean-copy-told", KL_SETTINGS_RESOLVER_APP, KL_SETTINGS_TYPE_BOOL, 0, 1, 0, 0U }
};

static int keys_parse(const char *value, long *parsed);
static int keys_type_valid(const char *type);
static int keys_opener_valid(const char *value);

/*
 * Reports how many settings the table has.
 */
size_t
kl_settings_key_count(
	void)
{
	/* Reports the table's size counted in rows. */
	return sizeof(settings_keys) / sizeof(settings_keys[0]);
}

/*
 * Gives the setting at a place in the table; NULL past its end.
 */
const struct kl_settings_key *
kl_settings_key_at(
	size_t index)
{
	size_t count;

	/* A place past the last row names nothing. */
	count = kl_settings_key_count();
	if (index >= count)
		return NULL;

	/* Reports the setting at that place. */
	return &settings_keys[index];
}

/*
 * Finds a setting by its name: a row of that name, or a prefix row whose
 * prefix the name starts with and whose rest is a MIME type.  NULL for a
 * name the table does not have.
 */
const struct kl_settings_key *
kl_settings_key_find(
	const char *name)
{
	size_t prefix_length;
	size_t length;
	size_t count;
	size_t index;
	int differs;
	int valid;

	/* Compares each row's name with the one asked for. */
	count = kl_settings_key_count();
	for (index = 0; index < count; index++) {
		/* A prefix row is matched by its prefix below, not by its whole name. */
		if ((settings_keys[index].flags & KL_SETTINGS_KEY_PREFIX) != 0U)
			continue;

		/* A row of that very name is the setting. */
		differs = strcmp(settings_keys[index].name, name);
		if (differs == 0)
			return &settings_keys[index];
	}

	/* A name too long is no prefix row's either. */
	length = strlen(name);
	if (length >= KL_SETTINGS_KEY_MAX)
		return NULL;

	/* Looks for a prefix row the name starts with, followed by a MIME type. */
	for (index = 0; index < count; index++) {
		/* Only a prefix row is looked at. */
		if ((settings_keys[index].flags & KL_SETTINGS_KEY_PREFIX) == 0U)
			continue;

		/* The name must start with the row's prefix. */
		prefix_length = strlen(settings_keys[index].name);
		differs = strncmp(name, settings_keys[index].name, prefix_length);
		if (differs != 0)
			continue;

		/* The rest must be a MIME type for the row to be the setting. */
		valid = keys_type_valid(name + prefix_length);
		if (valid)
			return &settings_keys[index];
	}

	/* The table has no such setting. */
	return NULL;
}

/*
 * Checks that a value may be given to a setting: well formed, and of its
 * type and within its range.  Returns 0 or EINVAL.
 */
int
kl_settings_key_check(
	const struct kl_settings_key *key,
	const char *value)
{
	long parsed;
	int valid;
	int error;

	/* Any value but an opener (which holds one tab) must fit and hold no control character. */
	if (key->type != KL_SETTINGS_TYPE_OPENER) {
		valid = kl_settings_value_valid(value);
		if (!valid)
			return EINVAL;
	}

	/* An opener is a name, a tab and a command. */
	if (key->type == KL_SETTINGS_TYPE_OPENER) {
		valid = keys_opener_valid(value);
		if (!valid)
			return EINVAL;

		/* Succeeded: the opener may be given. */
		return 0;
	}

	/* A path is absolute. */
	if (key->type == KL_SETTINGS_TYPE_PATH) {
		/* A relative path is refused. */
		if (value[0] != '/')
			return EINVAL;

		/* Succeeded: the path may be given. */
		return 0;
	}

	/* A number or a Boolean is a whole number. */
	error = keys_parse(value, &parsed);
	if (error != 0)
		return EINVAL;

	/* It must not lie below the range (it is not moved into it). */
	if (parsed < (long)key->minimum)
		return EINVAL;

	/* Nor above it. */
	if (parsed > (long)key->maximum)
		return EINVAL;

	/* Succeeded: the value may be given. */
	return 0;
}

/*
 * Reads a setting's value as a whole number, moved into the setting's
 * range when it lies outside (a value written by hand).  Returns 0, or
 * EINVAL for a value that is not a whole number.
 */
int
kl_settings_key_number(
	const struct kl_settings_key *key,
	const char *value,
	int *number)
{
	long parsed;
	int error;

	/* A whole number, and nothing after it. */
	error = keys_parse(value, &parsed);
	if (error != 0)
		return EINVAL;

	/* Below the range, above it, or within it. */
	if (parsed < (long)key->minimum) {
		*number = key->minimum;
	} else if (parsed > (long)key->maximum) {
		*number = key->maximum;
	} else {
		*number = (int)parsed;
	}

	/* Succeeded: the number within the range. */
	return 0;
}

/*
 * Tells whether a name is well formed: lower-case letters, digits, '.',
 * '_' and '-', not empty and short enough.
 */
int
kl_settings_name_valid(
	const char *name)
{
	size_t length;
	size_t index;
	char character;

	/* An empty name is not. */
	length = strlen(name);
	if (length == 0U)
		return 0;

	/* Nor one too long. */
	if (length >= KL_SETTINGS_KEY_MAX)
		return 0;

	/* Each character must be one of those allowed. */
	for (index = 0; index < length; index++) {
		/* A lower-case letter is allowed. */
		character = name[index];
		if (character >= 'a' && character <= 'z')
			continue;

		/* So is a digit. */
		if (character >= '0' && character <= '9')
			continue;

		/* And the three separators. */
		if (character == '.' ||
		    character == '_' ||
		    character == '-')
			continue;

		/* Any other character refuses the name. */
		return 0;
	}

	/* Every character is allowed. */
	return 1;
}

/*
 * Tells whether a value is well formed: short enough, without a newline
 * or another control character (UTF-8's bytes above 0x7f are allowed).
 */
int
kl_settings_value_valid(
	const char *value)
{
	size_t length;
	size_t index;
	unsigned char character;

	/* A value too long is not. */
	length = strlen(value);
	if (length >= KL_SETTINGS_VALUE_MAX)
		return 0;

	/* No control character. */
	for (index = 0; index < length; index++) {
		/* A control character refuses the value. */
		character = (unsigned char)value[index];
		if (character < 0x20U || character == 0x7fU)
			return 0;
	}

	/* Every byte is allowed. */
	return 1;
}

/* Reads a whole decimal number with nothing after it; returns 0 or EINVAL. */
static int
keys_parse(
	const char *value,
	long *parsed)
{
	char *end;

	/* Reads the number, with errno cleared so that a range error shows. */
	errno = 0;
	*parsed = strtol(value, &end, 10);

	/* Nothing read is no number. */
	if (end == value)
		return EINVAL;

	/* Nor is one with something after it. */
	if (*end != '\0')
		return EINVAL;

	/* Nor one out of a long's range. */
	if (errno != 0)
		return EINVAL;

	/* Succeeded: a whole number. */
	return 0;
}

/* Tells whether a text is a MIME type: lower-case letters, digits, '.', '+', '-' and '_', with one '/' inside. */
static int
keys_type_valid(
	const char *type)
{
	size_t length;
	size_t index;
	unsigned slashes;
	char character;

	/* Too short to hold a type, a slash and a subtype is not one. */
	length = strlen(type);
	if (length < 3U)
		return 0;

	/* Nor is one starting or ending with the slash. */
	if (type[0] == '/' || type[length - 1U] == '/')
		return 0;

	/* Each character one of those allowed, the slashes counted. */
	slashes = 0;
	for (index = 0; index < length; index++) {
		/* A slash is counted. */
		character = type[index];
		if (character == '/') {
			slashes++;
			continue;
		}

		/* A lower-case letter is allowed. */
		if (character >= 'a' && character <= 'z')
			continue;

		/* So is a digit. */
		if (character >= '0' && character <= '9')
			continue;

		/* And the four marks. */
		if (character == '.' ||
		    character == '+' ||
		    character == '-' ||
		    character == '_')
			continue;

		/* Any other character refuses the type. */
		return 0;
	}

	/* Exactly one slash. */
	if (slashes != 1U)
		return 0;

	/* Succeeded: a MIME type. */
	return 1;
}

/* Tells whether a value is an opener: a name, one tab and a command, neither empty, no other control character, short enough. */
static int
keys_opener_valid(
	const char *value)
{
	size_t length;
	size_t index;
	size_t tab;
	unsigned tabs;
	unsigned char character;

	/* A value too long is not. */
	length = strlen(value);
	if (length >= KL_SETTINGS_VALUE_MAX)
		return 0;

	/* Each byte printable but the tabs, which are counted. */
	tabs = 0;
	tab = 0;
	for (index = 0; index < length; index++) {
		/* A tab is counted, and where it is kept. */
		character = (unsigned char)value[index];
		if (character == '\t') {
			tabs++;
			tab = index;
			continue;
		}

		/* Any other control character refuses the value. */
		if (character < 0x20U || character == 0x7fU)
			return 0;
	}

	/* Exactly one tab. */
	if (tabs != 1U)
		return 0;

	/* With a name before it and a command after it. */
	if (tab == 0U || tab + 1U == length)
		return 0;

	/* Succeeded: an opener. */
	return 1;
}
