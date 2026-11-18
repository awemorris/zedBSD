/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's language (language.c, WS158): the catalogs of its text
 * (libkeiland's kl_tr_*, domain "wayland") in the session's language, the
 * setting ui.language, or before a login in the system's language, the
 * one line of KEILAND_SYSCONFDIR/keiland/language ("en", "ja"); and the
 * dates of the system bar and the login screen in it.
 */

#ifndef KWL_LANGUAGE_H
#define KWL_LANGUAGE_H

#include <stddef.h>
#include <time.h>

struct kwl_server;

/* The domain of the compositor's own text. */
#define KWL_LANGUAGE_DOMAIN	"wayland"

/* The system's language before a login (the login screen's), set by an administrator. */
#define KWL_LANGUAGE_SYSTEM_PATH	KEILAND_SYSCONFDIR "/keiland/language"

/* The two forms of a date: the system bar's short one, and the login screen's long one. */
#define KWL_LANGUAGE_DATE_SHORT	0
#define KWL_LANGUAGE_DATE_LONG	1

/*
 * Reads the system's language for the login screen (English when the
 * file is not there).
 */
void kwl_language_system(struct kwl_server *server);

/*
 * Reads the first line of the system's language file as it is (0, or
 * ENOENT without one); the machine's thread uses it (ws188-p002).
 */
int kwl_language_system_word(char *word, size_t size);

/*
 * Takes the session's language from the setting ui.language (0 English,
 * 1 Japanese): the catalogs are read and the screen drawn again.
 */
void kwl_language_set(struct kwl_server *server, int setting);

/*
 * Writes a date in the language: the short form "Mon Oct 5  14:05" with
 * the time, or the long "Monday, October 5" without it.
 */
void kwl_language_date(const struct tm *local, int form, char *out, size_t size);

#endif
