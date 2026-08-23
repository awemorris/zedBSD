/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Swap on FAT support
 */

#ifndef ZEDBSD_KERN_SWAP_FAT_H
#define ZEDBSD_KERN_SWAP_FAT_H

struct cwdinfo;

int
swap_fat_activate(
	struct cwdinfo *,
	const char *);

unsigned
swap_fat_extent_count(void);

#endif
