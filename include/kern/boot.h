/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_ABI_H
#define ZEDBSD_ABI_H

/* Shared data contract between the real-mode Stage 1 and 32-bit Stage 2. */
#include <stdint.h>

#define ZEDBSD_STAGE2_MAGIC 0x53383942U  /* "B98S" */
#define ZEDBSD_HANDOFF_MAGIC 0x48323842U /* "B82H" */
#define ZEDBSD_BOOTSTRAP_MAGIC 0x4839384cU /* "L98H" */

enum zedbsd_bootstrap_filesystem {
	ZEDBSD_BOOTSTRAP_FS_FAT16 = 1,
};

enum zedbsd_bootstrap_flags {
	ZEDBSD_BOOTSTRAP_HAS_GEOMETRY = 1U << 0,
};

/* Versioned PBR -> IO.SYS contract at physical address 0000:0700. */
struct zedbsd_bootstrap_handoff {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint32_t partition_lba;
	uint8_t boot_bios_id;
	uint8_t heads;
	uint8_t sectors;
	uint8_t filesystem_hint;
	uint16_t sector_size;
	uint16_t flags;
	uint16_t saved_si;
	uint16_t saved_di;
} __attribute__((packed));

struct zedbsd_stage2_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t image_size;
	uint32_t entry_offset;
	uint32_t payload_checksum;
} __attribute__((packed));

struct zedbsd_handoff {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint8_t device_count;
	uint8_t boot_bios_id;
	uint16_t reserved;
	uint32_t device_table;
	uint32_t bios_gateway;
	uint32_t boot_partition_lba;
} __attribute__((packed));

enum zedbsd_bios_service {
	ZEDBSD_BIOS_DISK_READ = 1,
	ZEDBSD_BIOS_KEY_READ = 2,
	ZEDBSD_BIOS_KEY_POLL = 3,
	ZEDBSD_BIOS_DISPLAY_RESET = 4,
	ZEDBSD_BIOS_RETURN_MENU = 5,
	/* Service 6 was the retired IPLware bridge; the number stays unused. */
	ZEDBSD_BIOS_REPROBE = 7,
	ZEDBSD_BIOS_CHAIN_BOOT = 8,
	ZEDBSD_BIOS_CLOCK_SECOND = 9,
	/* Probe exactly request.bios_id; request.status is a device-class hint. */
	ZEDBSD_BIOS_PROBE_FIXED = 10,
	/* One 512-byte fixed-disk write through the low-memory BIOS bounce area. */
	ZEDBSD_BIOS_DISK_WRITE = 11,
	/* Stop displaying G-VRAM (INT 18h, AH=41h). */
	ZEDBSD_BIOS_DISPLAY_STOP = 12,
	/* One byte of the BIOS real-time key state table; request.status
	 * selects the scan-code group (0..15). */
	ZEDBSD_BIOS_KEY_STATE = 13,
};

struct zedbsd_bios_request {
	uint16_t service;
	uint16_t status;
	uint8_t bios_id;
	uint8_t heads;
	uint8_t sectors;
	uint8_t reserved;
	uint32_t lba;
	uint32_t buffer;
} __attribute__((packed));

#define ZEDBSD_APPLET_MAGIC 0x41383942U /* "B98A" */
struct zedbsd_applet_header {
	uint32_t magic;
	uint16_t abi_version;
	uint16_t header_size;
	uint32_t image_size;
	uint16_t entry_offset;
	uint16_t flags;
	uint32_t crc32;
	char name[16];
} __attribute__((packed));

struct zedbsd_applet_services {
	uint16_t abi_version;
	uint16_t size;
	void (*putc)(char c);
	void (*puts)(const char *s);
	uint32_t (*key_read)(void);
};

typedef uint32_t (*zedbsd_applet_entry_t)(
        const struct zedbsd_applet_services *services, uint32_t argc,
        const char *const *argv);

typedef uint32_t (*zedbsd_bios_gateway_t)(struct zedbsd_bios_request *request);

_Static_assert(sizeof(struct zedbsd_stage2_header) == 20,
               "zedBSD Stage 2 header must remain 20 bytes");
_Static_assert(sizeof(struct zedbsd_bootstrap_handoff) == 24,
               "zedBSD bootstrap handoff must remain 24 bytes");
_Static_assert(sizeof(struct zedbsd_handoff) == 24,
	       "zedBSD handoff version 2 must remain 24 bytes");
_Static_assert(sizeof(struct zedbsd_bios_request) == 16,
               "zedBSD BIOS request must remain 16 bytes");
_Static_assert(sizeof(struct zedbsd_applet_header) == 36,
               "zedBSD applet header must remain 36 bytes");

enum zedbsd_device_class {
	ZEDBSD_DEV_FDD = 1,
	ZEDBSD_DEV_IDE = 2,
	ZEDBSD_DEV_SCSI = 3,
};

enum zedbsd_device_flags {
	ZEDBSD_DEV_PRESENT = 1U << 0,
	ZEDBSD_DEV_HAS_GEOMETRY = 1U << 1,
	ZEDBSD_DEV_BOOT_ORIGIN = 1U << 2,
};

/* flags in the Linux SETUP_PC98_DISK payload */
enum zedbsd_linux_disk_flags {
	ZEDBSD_LINUX_DISK_F_FALLBACK = 1U << 0,
	ZEDBSD_LINUX_DISK_F_BOOT = 1U << 1,
};

struct zedbsd_device {
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

_Static_assert(sizeof(struct zedbsd_device) == 16,
               "zedBSD device descriptor ABI must remain 16 bytes");

#endif
