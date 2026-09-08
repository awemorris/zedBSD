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

#define UL_GETFSIZE 1
#define UL_SETFSIZE 2
#define UL_GETMAXBRK 3
#define UL_GETOPENMAX 4

long ulimit(int, ...);

#endif
