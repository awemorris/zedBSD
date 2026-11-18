/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Kernel hardware-page geometry.
 */

#ifndef KERN_KERN_PAGE_H
#define KERN_KERN_PAGE_H

#ifndef KERN_PAGE_SIZE
#define KERN_PAGE_SIZE	4096U
#endif

#if KERN_PAGE_SIZE == 0 || (KERN_PAGE_SIZE & (KERN_PAGE_SIZE - 1U)) != 0
#error KERN_PAGE_SIZE must be a non-zero power of two
#endif

#endif
