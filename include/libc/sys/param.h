/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Historical BSD compatibility header.  It is not part of POSIX, but portable
 * software written against Unix systems includes it for the size limits and
 * arithmetic macros below, so the names are kept to their traditional
 * meanings rather than given new ones.
 */

#ifndef LIBC_SYS_PARAM_H
#define LIBC_SYS_PARAM_H

#include <limits.h>
#include <sys/types.h>

/* Bits in a byte, and the traditional spelling of a path length limit. */
#define NBBY CHAR_BIT
#define MAXPATHLEN PATH_MAX
#define MAXNAMLEN NAME_MAX
/* The traditional spelling counts the terminator. */
#define MAXHOSTNAMELEN (HOST_NAME_MAX + 1)

/* Number of y-sized units needed to hold x, and x rounded to those units. */
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define roundup(x, y) (howmany(x, y) * (y))
#define rounddown(x, y) (((x) / (y)) * (y))

/* True when y is a power of two, in which case the mask forms are valid. */
#define powerof2(x) ((((x) - 1) & (x)) == 0)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#endif
