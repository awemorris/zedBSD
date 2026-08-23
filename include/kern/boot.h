/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Boot handoff and BIOS call
 */

#ifndef ZEDBSD_ABI_H
#define ZEDBSD_ABI_H

#include <stdint.h>

/*
 * Shared data contract between the real-mode Stage 1 and 32-bit Stage 2.
 */

#define ZEDBSD_STAGE2_MAGIC	0x53383942U  /* "B98S" */
#define ZEDBSD_HANDOFF_MAGIC	0x48323842U /* "B82H" */

struct boot_stage2_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t image_size;
	uint32_t entry_offset;
	uint32_t payload_checksum;
} __attribute__((packed));

struct boot_handoff {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint8_t device_count;
	uint8_t boot_bios_id;
	/*
	 * Version 2 treats these bytes as a reserved zero word and selects the
	 * boot partition by LBA.  Version 3 uses an MBR partition index.
	 */
	uint8_t boot_partition_scheme;
	uint8_t boot_partition_index;
	uint32_t device_table;
	uint32_t bios_gateway;
	uint32_t boot_partition_lba;
} __attribute__((packed));

#define ZEDBSD_HANDOFF_VERSION_PC98		2U
#define ZEDBSD_HANDOFF_VERSION_MULTIBOOT	3U
#define ZEDBSD_HANDOFF_VERSION_SUN4U		4U
#define ZEDBSD_HANDOFF_VERSION_X68K		5U
#define ZEDBSD_PARTITION_SCHEME_LBA		0U
#define ZEDBSD_PARTITION_SCHEME_MBR		1U
#define ZEDBSD_PARTITION_SCHEME_SUN		2U
#define ZEDBSD_PARTITION_SCHEME_X68K		3U
#define ZEDBSD_BOOT_PARTITION_LBA_UNKNOWN	0xffffffffU

#define ZEDBSD_X68K_HANDOFF_MAGIC		0x58363848U /* "X68H" */
#define ZEDBSD_X68K_HANDOFF_VERSION		1U
#define ZEDBSD_X68K_MAX_MEMORY_REGIONS		4U

#define ZEDBSD_MEMORY_AVAILABLE			1U
#define ZEDBSD_MEMORY_RESERVED			2U

struct boot_memory_region32 {
	uint32_t base;
	uint32_t size;
	uint32_t type;
} __attribute__((packed));

struct x68k_boot_handoff {
	struct boot_handoff common;
	uint32_t extension_magic;
	uint16_t extension_version;
	uint16_t extension_size;
	uint32_t ram_bytes;
	uint32_t kernel_phys_start;
	uint32_t kernel_phys_end;
	uint32_t loader_phys_start;
	uint32_t loader_phys_end;
	uint32_t memory_region_count;
	struct boot_memory_region32
		memory_regions[ZEDBSD_X68K_MAX_MEMORY_REGIONS];
} __attribute__((packed));

_Static_assert(
	sizeof(struct x68k_boot_handoff) == 104,
	"zedBSD X68k handoff ABI must remain 104 bytes");

enum bios_service {
	ZEDBSD_BIOS_DISK_READ = 1,
	ZEDBSD_BIOS_KEY_READ = 2,
	ZEDBSD_BIOS_KEY_POLL = 3,
	ZEDBSD_BIOS_DISPLAY_RESET = 4,
	ZEDBSD_BIOS_RETURN_MENU = 5,

	/*
	 * Service 6 was the retired IPLware bridge; the number stays unused.
	 */
	ZEDBSD_BIOS_REPROBE = 7,
	ZEDBSD_BIOS_CHAIN_BOOT = 8,
	ZEDBSD_BIOS_CLOCK_SECOND = 9,

	/*
	 * Probe exactly request.bios_id; request.status is a device-class hint.
	 */
	ZEDBSD_BIOS_PROBE_FIXED = 10,

	/*
	 * One 512-byte fixed-disk write through the low-memory BIOS bounce
	 * area.
	 */
	ZEDBSD_BIOS_DISK_WRITE = 11,

	/*
	 * Stop displaying G-VRAM (INT 18h, AH=41h).
	 */
	ZEDBSD_BIOS_DISPLAY_STOP = 12,

	/*
	 * One byte of the BIOS real-time key state table; request.status
	 * selects the scan-code group (0..15).
	 */
	ZEDBSD_BIOS_KEY_STATE = 13,
};

struct bios_request {
	uint16_t service;
	uint16_t status;
	uint8_t bios_id;
	uint8_t heads;
	uint8_t sectors;
	uint8_t reserved;
	uint32_t lba;
	uint32_t buffer;
} __attribute__((packed));

typedef uint32_t (
	*bios_gateway_fn)(
	struct bios_request *request);

_Static_assert(
	sizeof(struct boot_stage2_header) == 20,
	"zedBSD Stage 2 header must remain 20 bytes");

_Static_assert(
	sizeof(struct boot_handoff) == 24,
	"zedBSD handoff version 2 must remain 24 bytes");

_Static_assert(
	sizeof(struct bios_request) == 16,
	"zedBSD BIOS request must remain 16 bytes");

enum boot_device_class {
	ZEDBSD_DEV_FDD = 1,
	ZEDBSD_DEV_IDE = 2,
	ZEDBSD_DEV_SCSI = 3,
	ZEDBSD_DEV_SD = 4,
};

enum boot_device_flags {
	ZEDBSD_DEV_PRESENT = 1U << 0,
	ZEDBSD_DEV_HAS_GEOMETRY = 1U << 1,
	ZEDBSD_DEV_BOOT_ORIGIN = 1U << 2,
};

/*
 * Firmware-discovered boot device descriptor shared with the kernel.
 */
struct boot_device {
	uint8_t device_class;
	uint8_t display_index;
	uint8_t bios_id;
	uint8_t flags;
	uint16_t sector_size;
	uint16_t cylinders;
	uint8_t heads;
	uint8_t sectors;
	uint8_t controller_location;
	uint8_t reserved[5];
} __attribute__((packed));

_Static_assert(
	sizeof(struct boot_device) == 16,
	"zedBSD device descriptor ABI must remain 16 bytes");

#endif
