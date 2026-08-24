/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_ATOMIC_H
#define ZEDBSD_UAPI_ATOMIC_H

/*
 * Compiler-runtime fallback operations for atomic objects which the target
 * cannot manipulate lock-free.  These values are private to libc and the
 * kernel; they are not part of the POSIX public namespace.
 */
#define ZEDBSD_ATOMIC_LOAD	0U
#define ZEDBSD_ATOMIC_STORE	1U
#define ZEDBSD_ATOMIC_EXCHANGE	2U
#define ZEDBSD_ATOMIC_COMPARE_EXCHANGE	3U

#endif
