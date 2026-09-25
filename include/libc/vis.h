/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_VIS_H
#define LIBC_VIS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Encoding choices.
 *
 * Text that came from somewhere else is not safe to print: a control
 * sequence in it acts on the terminal of whoever reads the log.  These
 * turn such a string into one that only ever prints as itself.
 */
#define VIS_OCTAL   0x0001	/* Always use \\ddd, never a letter escape. */
#define VIS_CSTYLE  0x0002	/* Prefer \\n, \\t, \\b and their like. */
#define VIS_SP      0x0004	/* Encode the space. */
#define VIS_TAB     0x0008	/* Encode the tab. */
#define VIS_NL      0x0010	/* Encode the newline. */
#define VIS_WHITE   (VIS_SP | VIS_TAB | VIS_NL)
#define VIS_SAFE    0x0020	/* Leave alone whatever cannot act on a terminal. */
#define VIS_DQ      0x8000	/* Encode the double quote. */

/* Output choices. */
#define VIS_NOSLASH 0x0040	/* Never emit the backslash itself. */
#define VIS_GLOB    0x0100	/* Encode the pattern characters * ? [ #. */

/* Results from unvis. */
#define UNVIS_VALID     1	/* A character is ready. */
#define UNVIS_VALIDPUSH 2	/* A character is ready; offer this byte again. */
#define UNVIS_NOCHAR    3	/* Nothing yet; keep feeding. */
#define UNVIS_SYNBAD   (-1)	/* The sequence is not one this can read. */
#define UNVIS_ERROR    (-2)	/* The arguments are wrong. */

/* Tells unvis that the input has ended. */
#define UNVIS_END 1

char *vis(char *, int, int, int);
char *nvis(char *, size_t, int, int, int);
int strvis(char *, const char *, int);
int stravis(char **, const char *, int);
int strnvis(char *, const char *, size_t, int);
int strvisx(char *, const char *, size_t, int);
int strnvisx(char *, size_t, const char *, size_t, int);
int strunvis(char *, const char *);
int strnunvis(char *, size_t, const char *);
int strunvisx(char *, const char *, int);
int strnunvisx(char *, size_t, const char *, int);
int unvis(char *, int, int *, int);

#ifdef __cplusplus
}
#endif

#endif
