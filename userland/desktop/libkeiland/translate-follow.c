/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A program following the desktop's language (WS158, kl_tr_follow): the
 * setting ui.language read once, and watched, so that the catalogs are
 * read again when it changes and the program draws its text again without
 * starting over (the 2026-10-05 user direction: no log out and no restart
 * for Keiland's programs).
 */

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/* The desktop's setting of the language. */
#define FOLLOW_KEY		"ui.language"

/* A parameter the callback's type has and this one does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/*
 * What a following program asked for: its domain and the call after a
 * change.  kl_tr_follow fills it; the watch reads it from
 * kl_settings_dispatch on the same thread.  A program follows one
 * language, so there is one.
 */
struct follow_state {
	char domain[KL_TR_NAME_MAX + 1U];
	kl_tr_changed_fn changed;
	void *data;
};

/*
 * The following program's request.  Its zero value means nothing follows
 * yet; kl_tr_follow sets it again for a second call.
 */
static struct follow_state follow_state;

static void follow_changed(void *data, const char *key, const char *value, unsigned flags);
static int follow_read(int setting);

/*
 * Follows the desktop's language from the settings.
 */
int
kl_tr_follow(
	struct kl_settings *settings,
	const char *domain,
	kl_tr_changed_fn changed,
	void *data)
{
	int setting;
	int length;
	int error;

	/* The settings and a domain's name are needed. */
	if (settings == NULL || domain == NULL)
		return EINVAL;
	length = snprintf(follow_state.domain, sizeof(follow_state.domain), "%s", domain);
	if (length < 0 || (size_t)length >= sizeof(follow_state.domain))
		return EINVAL;
	follow_state.changed = changed;
	follow_state.data = data;

	/* The language now (English when the desktop does not say). */
	setting = kl_settings_get_int(settings, FOLLOW_KEY, 0);
	error = follow_read(setting);
	if (error != 0)
		return error;

	/* And whenever it changes. */
	error = kl_settings_watch(settings, FOLLOW_KEY, follow_changed, NULL, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the program follows the language. */
	return 0;
}

/*
 * Takes a change of the language: the catalogs are read in it, and the
 * program is told to draw again.
 */
static void
follow_changed(
	void *data,
	const char *key,
	const char *value,
	unsigned flags)
{
	long setting;
	char *end;
	int error;

	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(flags);

	/* The new value, a whole number; none (the compositor went) is English. */
	setting = 0;
	if (value != NULL) {
		setting = strtol(value, &end, 10);
		if (end == value || *end != '\0')
			setting = 0;
	}

	/* The catalogs of that language; a failure leaves English. */
	error = follow_read((int)setting);
	if (error != 0)
		(void)kl_tr_open(follow_state.domain, "en");

	/* The program draws its text again. */
	if (follow_state.changed != NULL)
		follow_state.changed(follow_state.data, kl_tr_language());
}

/*
 * Reads the program's catalogs in the language a value of the setting
 * names (English for a value no language has).  Returns as kl_tr_open.
 */
static int
follow_read(
	int setting)
{
	const char *language;
	int error;

	/* The language's code. */
	language = kl_tr_language_code(setting);
	if (language == NULL)
		language = "en";

	/* Its catalogs. */
	error = kl_tr_open(follow_state.domain, language);
	if (error != 0)
		return error;

	/* Succeeded: the texts are the language's. */
	return 0;
}
