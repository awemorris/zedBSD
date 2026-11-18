/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arm64 HAL's constants that assembly and C share.
 *
 * A board's fixed peripheral addresses are in its own board.h.
 */

#ifndef KERN_HAL_ARM64_DEFS_H
#define KERN_HAL_ARM64_DEFS_H

/* The page size of the kernel's tables. */
#define ARM64_PAGE_SIZE 4096

/* Where the kernel's direct map of physical memory starts. */
#define ARM64_DIRECT_BASE 0xffff000000000000

/* Marks a parameter a function takes but does not need. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

#endif
