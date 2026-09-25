/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Character set conversion (POSIX iconv).
 *
 * The C library converts between UTF-8 and ASCII, the two character sets the
 * system uses; it has no tables for others, and iconv_open() refuses them
 * with EINVAL.  See src/libc/iconv.c for the names accepted.
 */

#ifndef LIBC_ICONV_H
#define LIBC_ICONV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *iconv_t;

iconv_t iconv_open(const char *, const char *);
size_t iconv(iconv_t, char **__restrict, size_t *__restrict,
	     char **__restrict, size_t *__restrict);
int iconv_close(iconv_t);

#ifdef __cplusplus
}
#endif

#endif
