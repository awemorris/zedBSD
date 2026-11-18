/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * alloca() takes memory from the caller's stack frame, released when the
 * caller returns.  It is not in POSIX; the compiler provides it.
 */

#ifndef LIBC_ALLOCA_H
#define LIBC_ALLOCA_H

#define alloca(size) __builtin_alloca(size)

#endif
