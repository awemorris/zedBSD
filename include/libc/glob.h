/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_GLOB_H
#define LIBC_GLOB_H

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The names a pattern selects.
 *
 * Expansion reads directories rather than guessing: a name is reported only
 * because it is there.  That is what separates this from matching a string,
 * and why the result is in the order the filesystem's names sort in.
 */
typedef struct {
	size_t gl_pathc;	/* Names found. */
	size_t gl_matchc;	/* Names found by this call alone. */
	size_t gl_offs;		/* Null slots reserved before the first. */
	int gl_flags;		/* The flags the call was made with. */
	char **gl_pathv;	/* The names, followed by a null. */

	/* Used in place of the filesystem under GLOB_ALTDIRFUNC. */
	void *(*gl_opendir)(const char *);
	struct dirent *(*gl_readdir)(void *);
	void (*gl_closedir)(void *);
	int (*gl_lstat)(const char *, struct stat *);
	int (*gl_stat)(const char *, struct stat *);
} glob_t;

#define GLOB_APPEND     0x0001	/* Add to what a previous call found. */
#define GLOB_DOOFFS     0x0002	/* Reserve gl_offs slots at the front. */
#define GLOB_ERR        0x0004	/* Stop at a directory that cannot be read. */
#define GLOB_MARK       0x0008	/* Mark a directory with a trailing slash. */
#define GLOB_NOCHECK    0x0010	/* Report the pattern when nothing matched. */
#define GLOB_NOSORT     0x0020	/* Leave the names in the order found. */
#define GLOB_NOESCAPE   0x2000	/* A backslash is an ordinary character. */

/* Extensions this interface has carried since 4.4BSD. */
#define GLOB_ALTDIRFUNC 0x0040	/* Read directories through the callbacks. */
#define GLOB_BRACE      0x0080	/* Expand {a,b} before matching. */
#define GLOB_MAGCHAR    0x0100	/* Set on return when the pattern had magic. */
#define GLOB_NOMAGIC    0x0200	/* GLOB_NOCHECK, but only without magic. */
#define GLOB_QUOTE      0x0400	/* Historical; a backslash always quotes. */
#define GLOB_TILDE      0x0800	/* Expand a leading ~ or ~user. */
#define GLOB_LIMIT      0x1000	/* Refuse a result that grows without bound. */

#define GLOB_NOSPACE  (-1)	/* Out of memory, or past a limit. */
#define GLOB_ABORTED  (-2)	/* A directory could not be read. */
#define GLOB_NOMATCH  (-3)	/* Nothing matched. */
#define GLOB_NOSYS    (-4)	/* Not implemented. */

int glob(const char *, int, int (*)(const char *, int), glob_t *);
void globfree(glob_t *);

#ifdef __cplusplus
}
#endif

#endif
