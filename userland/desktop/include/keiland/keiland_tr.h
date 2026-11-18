/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The translations of the user interface's text, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_TR_H
#define KEILAND_TR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kl_settings;

/*
 * The translations of the user interface's text.  The English text in the
 * source is the key: kl_tr("Open") gives the text of the language chosen, or
 * "Open" itself when the catalog has none.  The catalogs are UTF-8 text
 * files, KEILAND_DATADIR/keiland/locale/LANGUAGE/ DOMAIN.tr, one for each
 * program (its domain) and one the desktop's programs share ("keiland").  The
 * language is the desktop's setting ui.language (0 English, 1 Japanese),
 * which kl_tr_follow watches, so that a program changes its language while it
 * runs.  One thread uses them; a text they give lives until the language or
 * the domain changes.
 */

/* The domain the desktop's programs share, looked in after a program's own. */
#define KL_TR_SHARED_DOMAIN	"keiland"

/* The longest name of a language or a domain. */
#define KL_TR_NAME_MAX		31U

/*
 * Called by kl_tr_follow after the language changed and its catalogs were
 * read: the program draws its text again.  language is "en", "ja", ...
 */
typedef void (*kl_tr_changed_fn)(void *data, const char *language);

/*
 * Reads a program's catalogs (domain and KL_TR_SHARED_DOMAIN) in a
 * language from the installed directory.  English needs none.  A catalog
 * that is not there leaves its texts in English.  Returns 0, EINVAL for a
 * name that is not lower-case letters, digits, '-' and '_', or ENOMEM; the
 * texts are English after a failure.
 */
int kl_tr_open(const char *domain, const char *language);

/*
 * Reads them from another directory, which holds LANGUAGE/DOMAIN.tr (a
 * program installed elsewhere, or a test).  Returns as kl_tr_open.
 */
int kl_tr_open_directory(const char *directory, const char *domain, const char *language);

/*
 * Forgets the catalogs: every text is English again.
 */
void kl_tr_close(void);

/* A language's catalogs read by kl_tr_read and not used yet. */
struct kl_tr_catalogs;

/*
 * Reads a program's catalogs in a language as kl_tr_open does, into a
 * record of their own, on any thread: the texts in use do not change (a
 * program reads them away from its windows' thread, whose files may be
 * slow).  Returns 0 with *out set, or as kl_tr_open with *out NULL.
 */
int kl_tr_read(const char *domain, const char *language, struct kl_tr_catalogs **out);

/*
 * Makes the catalogs read the program's, on the thread that uses the
 * texts, as kl_tr_open would have, and frees the record (NULL does
 * nothing).
 */
void kl_tr_use(struct kl_tr_catalogs *catalogs);

/*
 * Frees catalogs read that are not to be used (NULL does nothing).
 */
void kl_tr_discard(struct kl_tr_catalogs *catalogs);

/*
 * Reports the language read ("en" before any).
 */
const char *kl_tr_language(void);

/*
 * Reports the language a value of ui.language names ("en" for 0, "ja"
 * for 1), or NULL for a value no language has.
 */
const char *kl_tr_language_code(int setting);

/*
 * Gives the text of the language for an English text.
 */
const char *kl_tr(const char *english);

/*
 * Gives the text for an English text in a context, for an English word
 * that is translated in more than one way ("Open" a verb or an adjective).
 */
const char *kl_trc(const char *context, const char *english);

/*
 * Gives the text for a number of things: the singular or the plural in
 * English, and the form the language takes for count.  The number itself
 * goes in with kl_tr_format.
 */
const char *kl_trn(const char *singular, const char *plural, unsigned long count);

/*
 * Writes a text of the language with its places filled: {1} to {9} are
 * the strings after pattern, in order, which a NULL ends; {{ is a brace.
 * The places can come in any order, so that a language puts the words
 * where its grammar wants them.  out always ends with a NUL.  Returns 0,
 * ERANGE when the text was cut to fit, or EINVAL for a place without its
 * string.
 */
int kl_tr_format(char *out, size_t size, const char *pattern, ...);

/*
 * Follows the desktop's language: reads the program's catalogs in the
 * language of ui.language now, and again whenever it changes, calling
 * changed (may be NULL) after each change.  The watch runs from
 * kl_settings_dispatch, and ends with the settings.  Returns 0, EINVAL or
 * ENOMEM.
 */
int kl_tr_follow(struct kl_settings *settings, const char *domain, kl_tr_changed_fn changed, void *data);

#ifdef __cplusplus
}
#endif

#endif
