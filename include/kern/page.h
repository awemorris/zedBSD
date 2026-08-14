/*
 * Kernel hardware-page geometry.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_PAGE_H
#define ZEDBSD_KERN_PAGE_H

#ifndef ZEDBSD_PAGE_SIZE
#define ZEDBSD_PAGE_SIZE 4096U
#endif

#if ZEDBSD_PAGE_SIZE == 0 || \
    (ZEDBSD_PAGE_SIZE & (ZEDBSD_PAGE_SIZE - 1U)) != 0
#error ZEDBSD_PAGE_SIZE must be a non-zero power of two
#endif

#endif

