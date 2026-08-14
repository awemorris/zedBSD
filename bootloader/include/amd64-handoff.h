/* PC/AT Stage 2 -> amd64 HAL bootstrap handoff. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BOOTLOADER_AMD64_HANDOFF_H
#define ZEDBSD_BOOTLOADER_AMD64_HANDOFF_H

#define ZBL6_HANDOFF_MAGIC              0x364c425a
#define ZBL6_HANDOFF_VERSION            1
#define ZBL6_HANDOFF_SIZE               24

#define ZBL6_HANDOFF_MAGIC_OFFSET       0
#define ZBL6_HANDOFF_VERSION_OFFSET     4
#define ZBL6_HANDOFF_SIZE_OFFSET        6
#define ZBL6_HANDOFF_BOOT_DRIVE_OFFSET  8
#define ZBL6_HANDOFF_PARTITION_OFFSET   9
#define ZBL6_HANDOFF_FLAGS_OFFSET       10
#define ZBL6_HANDOFF_MEM_LOWER_OFFSET   12
#define ZBL6_HANDOFF_MEM_UPPER_OFFSET   16
#define ZBL6_HANDOFF_RESERVED_OFFSET    20

#ifndef __ASSEMBLER__
#include <stdint.h>

struct zbl6_handoff {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint8_t boot_drive;
	uint8_t partition_index;
	uint16_t flags;
	uint32_t mem_lower_kib;
	uint32_t mem_upper_kib;
	uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct zbl6_handoff) == ZBL6_HANDOFF_SIZE,
	"ZBL6 handoff size");
#endif

#endif
