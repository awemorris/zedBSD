/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * XSI process limits.
 */

#ifndef LIBC_ULIMIT_H
#define LIBC_ULIMIT_H

#ifdef __cplusplus
extern "C" {
#endif

#define UL_GETFSIZE 1
#define UL_SETFSIZE 2
#define UL_GETMAXBRK 3
#define UL_GETOPENMAX 4

long ulimit(int, ...);

#ifdef __cplusplus
}
#endif

#endif
