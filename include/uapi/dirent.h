/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_UAPI_DIRENT_H
#define KERN_UAPI_DIRENT_H

#include <stdint.h>

#define KERN_DT_UNKNOWN	0U
#define KERN_DT_REG	1U
#define KERN_DT_DIR	2U
#define KERN_DT_BLK	3U
#define KERN_DT_CHR	4U
#define KERN_DT_FIFO	5U
#define KERN_DT_LNK	6U
#define KERN_DT_SOCK	7U

struct dirent_record {
	uint64_t d_ino;
	uint32_t d_type;
	char d_name[256];
} __attribute__((packed, aligned(4)));

_Static_assert(
	sizeof(struct dirent_record) == 268,
	"zedBSD ELF32 dirent ABI must remain 268 bytes");

#endif
