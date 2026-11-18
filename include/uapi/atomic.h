/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_UAPI_ATOMIC_H
#define KERN_UAPI_ATOMIC_H

/*
 * Compiler-runtime fallback operations for atomic objects which the target
 * cannot manipulate lock-free.  These values are private to libc and the
 * kernel. They are not part of the POSIX public namespace.
 */
#define KERN_ATOMIC_LOAD	0U
#define KERN_ATOMIC_STORE	1U
#define KERN_ATOMIC_EXCHANGE	2U
#define KERN_ATOMIC_COMPARE_EXCHANGE	3U

#endif
