/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stream inspection functions of Solaris and glibc: what a FILE holds
 * in its buffers and which way it is being used.  Portable software
 * (gnulib) uses these instead of reading the FILE structure itself.
 */

#ifndef LIBC_STDIO_EXT_H
#define LIBC_STDIO_EXT_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FSETLOCKING_QUERY 0
#define FSETLOCKING_INTERNAL 1
#define FSETLOCKING_BYCALLER 2

/* How much the stream holds and how it may be used. */
size_t __fbufsize(FILE *stream);
size_t __fpending(FILE *stream);
size_t __freadahead(FILE *stream);
int __flbf(FILE *stream);
int __freadable(FILE *stream);
int __fwritable(FILE *stream);
int __freading(FILE *stream);
int __fwriting(FILE *stream);

/* Changes to the stream's state. */
void __fpurge(FILE *stream);
void __fseterr(FILE *stream);
int __fsetlocking(FILE *stream, int type);

#ifdef __cplusplus
}
#endif

#endif
