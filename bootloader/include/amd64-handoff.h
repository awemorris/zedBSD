/* PC/AT Stage 2 -> amd64 HAL bootstrap handoff. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BOOTLOADER_AMD64_HANDOFF_H
#define ZEDBSD_BOOTLOADER_AMD64_HANDOFF_H

#include "boot-parameter-handoff.h"

/* Values carried by the root-partition fields in the ZBL6 ABI. */
#define ZBL6_PARTITION_SCHEME_MBR 1U
#define ZBL6_PARTITION_SCHEME_GPT 4U
#define ZBL6_PARTITION_INDEX_UNKNOWN 0U

#define ZBL6_HANDOFF_MAGIC 0x364c425a
#define ZBL6_HANDOFF_VERSION 1
#define ZBL6_HANDOFF_SIZE 24

#define ZBL6_HANDOFF_MAGIC_OFFSET 0
#define ZBL6_HANDOFF_VERSION_OFFSET 4
#define ZBL6_HANDOFF_SIZE_OFFSET 6
#define ZBL6_HANDOFF_BOOT_DRIVE_OFFSET 8
#define ZBL6_HANDOFF_PARTITION_OFFSET 9
#define ZBL6_HANDOFF_FLAGS_OFFSET 10
#define ZBL6_HANDOFF_MEM_LOWER_OFFSET 12
#define ZBL6_HANDOFF_MEM_UPPER_OFFSET 16
#define ZBL6_HANDOFF_RESERVED_OFFSET 20
#define ZBL6_HANDOFF_BOOT_SERIAL_OFFSET 20

/* A legacy-BIOS loader retains the compact v1 memory description and may
 * append a VBE linear-framebuffer descriptor when size and flags say so. */
#define ZBL6_HANDOFF_FB_SIZE 56
#define ZBL6_HANDOFF_FRAMEBUFFER_BASE_OFFSET 24
#define ZBL6_HANDOFF_FRAMEBUFFER_SIZE_OFFSET 32
#define ZBL6_HANDOFF_FRAMEBUFFER_WIDTH_OFFSET 40
#define ZBL6_HANDOFF_FRAMEBUFFER_HEIGHT_OFFSET 44
#define ZBL6_HANDOFF_FRAMEBUFFER_STRIDE_OFFSET 48
#define ZBL6_HANDOFF_FRAMEBUFFER_FORMAT_OFFSET 52

#define ZBL6_HANDOFF_V2_VERSION 2
#define ZBL6_HANDOFF_V2_SIZE 88
#define ZBL6_HANDOFF_V3_VERSION 3
#define ZBL6_HANDOFF_V3_SIZE 96
#define ZBL6_HANDOFF_V4_VERSION 4
#define ZBL6_HANDOFF_V4_SIZE 100
#define ZBL6_HANDOFF_V5_VERSION 5
#define ZBL6_HANDOFF_V5_BIOS_SIZE \
	(ZBL6_HANDOFF_FB_SIZE + ZEDBSD_BOOT_PARAMETER_RECORD_SIZE)
#define ZBL6_HANDOFF_V5_UEFI_SIZE \
	(ZBL6_HANDOFF_V4_SIZE + ZEDBSD_BOOT_PARAMETER_RECORD_SIZE)
#define ZBL6_HANDOFF_V5_BIOS_PARAMETERS_OFFSET ZBL6_HANDOFF_FB_SIZE
#define ZBL6_HANDOFF_V5_UEFI_PARAMETERS_OFFSET ZBL6_HANDOFF_V4_SIZE

#define ZBL6_HANDOFF_V3_FRAMEBUFFER_BASE_OFFSET 64
#define ZBL6_HANDOFF_V3_FRAMEBUFFER_WIDTH_OFFSET 80
#define ZBL6_HANDOFF_V3_FRAMEBUFFER_HEIGHT_OFFSET 84
#define ZBL6_HANDOFF_V3_FRAMEBUFFER_STRIDE_OFFSET 88

#define ZBL6_HANDOFF_FLAG_UEFI (1U << 0)
#define ZBL6_HANDOFF_FLAG_MEMORY_MAP (1U << 1)
#define ZBL6_HANDOFF_FLAG_ACPI_RSDP (1U << 2)
#define ZBL6_HANDOFF_FLAG_FRAMEBUFFER (1U << 3)
#define ZBL6_HANDOFF_FLAG_BOOT_UUID (1U << 4)
#define ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS (1U << 5)

#define ZBL6_FRAMEBUFFER_RGBX8888 1U
#define ZBL6_FRAMEBUFFER_BGRX8888 2U
#define ZBL6_FRAMEBUFFER_VIRTUAL_BASE 0xffffffffc2000000ULL

#define ZBL6_MEMORY_USABLE 1U
#define ZBL6_MEMORY_RESERVED 2U
#define ZBL6_MEMORY_ACPI_RECLAIM 3U
#define ZBL6_MEMORY_ACPI_NVS 4U
#define ZBL6_MEMORY_MMIO 5U

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

struct zbl6_handoff_framebuffer {
	struct zbl6_handoff common;
	uint64_t framebuffer_base;
	uint64_t framebuffer_size;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint32_t framebuffer_stride;
	uint32_t framebuffer_format;
} __attribute__((packed));

/* UEFI v2-v4 preserve their historical MBR/index contract.  V5 identifies
 * the selected FAT by UUID and carries its actual MBR/GPT style with both
 * partition-index bytes set to ZBL6_PARTITION_INDEX_UNKNOWN. */
struct zbl6_handoff_v2 {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint32_t flags;
	uint8_t boot_drive;
	uint8_t root_partition_scheme;
	uint8_t root_partition_index;
	uint8_t loader_partition_index;
	uint32_t memory_range_count;
	uint32_t memory_range_entry_size;
	uint64_t memory_ranges;
	uint64_t kernel_phys_start;
	uint64_t kernel_phys_end;
	uint64_t bootstrap_cr3;
	uint64_t rsdp;
	uint64_t reserved[3];
} __attribute__((packed));

struct zbl6_handoff_v3 {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint32_t flags;
	uint8_t boot_drive;
	uint8_t root_partition_scheme;
	uint8_t root_partition_index;
	uint8_t loader_partition_index;
	uint32_t memory_range_count;
	uint32_t memory_range_entry_size;
	uint64_t memory_ranges;
	uint64_t kernel_phys_start;
	uint64_t kernel_phys_end;
	uint64_t bootstrap_cr3;
	uint64_t rsdp;
	uint64_t framebuffer_base;
	uint64_t framebuffer_size;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint32_t framebuffer_stride;
	uint32_t framebuffer_format;
} __attribute__((packed));

struct zbl6_handoff_v4 {
	struct zbl6_handoff_v3 common;
	uint32_t boot_volume_serial;
} __attribute__((packed));

struct zbl6_handoff_v5_bios {
	struct zbl6_handoff_framebuffer common;
	struct zedbsd_boot_parameter_record parameters;
} __attribute__((packed));

struct zbl6_handoff_v5_uefi {
	struct zbl6_handoff_v4 common;
	struct zedbsd_boot_parameter_record parameters;
} __attribute__((packed));

struct zbl6_framebuffer {
	uint64_t physical_base;
	uint64_t size;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
};

struct zbl6_memory_range {
	uint64_t base;
	uint64_t size;
	uint32_t type;
	uint32_t flags;
} __attribute__((packed));

_Static_assert(sizeof(struct zbl6_handoff) == ZBL6_HANDOFF_SIZE,
	       "ZBL6 handoff size");
_Static_assert(sizeof(struct zbl6_handoff_framebuffer) == ZBL6_HANDOFF_FB_SIZE,
	       "ZBL6 BIOS framebuffer handoff size");
_Static_assert(sizeof(struct zbl6_handoff_v2) == ZBL6_HANDOFF_V2_SIZE,
	       "ZBL6 handoff v2 size");
_Static_assert(sizeof(struct zbl6_handoff_v3) == ZBL6_HANDOFF_V3_SIZE,
	       "ZBL6 handoff v3 size");
_Static_assert(sizeof(struct zbl6_handoff_v4) == ZBL6_HANDOFF_V4_SIZE,
	       "ZBL6 handoff v4 size");
_Static_assert(sizeof(struct zbl6_handoff_v5_bios) ==
	       ZBL6_HANDOFF_V5_BIOS_SIZE, "ZBL6 BIOS v5 handoff size");
_Static_assert(sizeof(struct zbl6_handoff_v5_uefi) ==
	       ZBL6_HANDOFF_V5_UEFI_SIZE, "ZBL6 UEFI v5 handoff size");
_Static_assert(__builtin_offsetof(struct zbl6_handoff_v5_bios, parameters) ==
	       ZBL6_HANDOFF_V5_BIOS_PARAMETERS_OFFSET,
	       "ZBL6 BIOS v5 parameter offset");
_Static_assert(__builtin_offsetof(struct zbl6_handoff_v5_uefi, parameters) ==
	       ZBL6_HANDOFF_V5_UEFI_PARAMETERS_OFFSET,
	       "ZBL6 UEFI v5 parameter offset");
_Static_assert(__builtin_offsetof(struct zbl6_handoff_v3, framebuffer_base) ==
		   ZBL6_HANDOFF_V3_FRAMEBUFFER_BASE_OFFSET,
	       "ZBL6 framebuffer base offset");
_Static_assert(__builtin_offsetof(struct zbl6_handoff_v3, framebuffer_width) ==
		   ZBL6_HANDOFF_V3_FRAMEBUFFER_WIDTH_OFFSET,
	       "ZBL6 framebuffer width offset");
_Static_assert(__builtin_offsetof(struct zbl6_handoff_v3,
				 framebuffer_height) ==
		   ZBL6_HANDOFF_V3_FRAMEBUFFER_HEIGHT_OFFSET,
	       "ZBL6 framebuffer height offset");
_Static_assert(__builtin_offsetof(struct zbl6_handoff_v3,
				 framebuffer_stride) ==
		   ZBL6_HANDOFF_V3_FRAMEBUFFER_STRIDE_OFFSET,
	       "ZBL6 framebuffer stride offset");
_Static_assert(sizeof(struct zbl6_memory_range) == 24,
	       "ZBL6 memory range size");
#endif

#endif
