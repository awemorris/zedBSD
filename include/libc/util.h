/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_UTIL_H
#define LIBC_UTIL_H

#include <sys/types.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Conveniences that are not part of the C library proper.
 *
 * These live in libutil rather than libc because nothing in the standard
 * asks for them and a program that does not want them should not carry
 * them.  They are built alongside libc from the same tree.
 */

/* The longest string fmt_scaled writes, terminator included. */
#define FMT_SCALED_STRSIZE 7

/*
 * A number written for a person to read: 1024 becomes "1.0K", and "1.0K" is
 * read back as 1024.  The point is that the two agree, so a value shown to
 * somebody can be typed back in.
 */
int fmt_scaled(long long, char *);
int scan_scaled(char *, long long *);

#ifdef __cplusplus
}
#endif

#endif
