/*
 * Boots
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_ABI_H
#define BOOTS_ABI_H

/* Shared data contract between the real-mode Stage 1 and 32-bit Stage 2. */
#include <stdint.h>

#define BOOTS_STAGE2_MAGIC 0x53383942U  /* "B98S" */
#define BOOTS_HANDOFF_MAGIC 0x48323842U /* "B82H" */
#define BOOTS_BOOTSTRAP_MAGIC 0x4839384cU /* "L98H" */

enum boots_bootstrap_filesystem {
	BOOTS_BOOTSTRAP_FS_FAT16 = 1,
};

enum boots_bootstrap_flags {
	BOOTS_BOOTSTRAP_HAS_GEOMETRY = 1U << 0,
};

/* Versioned PBR -> IO.SYS contract at physical address 0000:0700. */
struct boots_bootstrap_handoff {
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

struct boots_stage2_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t image_size;
	uint32_t entry_offset;
	uint32_t payload_checksum;
} __attribute__((packed));

struct boots_handoff {
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

enum boots_bios_service {
	BOOTS_BIOS_DISK_READ = 1,
	BOOTS_BIOS_KEY_READ = 2,
	BOOTS_BIOS_KEY_POLL = 3,
	BOOTS_BIOS_DISPLAY_RESET = 4,
	BOOTS_BIOS_RETURN_MENU = 5,
	/* Service 6 was the retired IPLware bridge; the number stays unused. */
	BOOTS_BIOS_REPROBE = 7,
	BOOTS_BIOS_CHAIN_BOOT = 8,
	BOOTS_BIOS_CLOCK_SECOND = 9,
	/* Probe exactly request.bios_id; request.status is a device-class hint. */
	BOOTS_BIOS_PROBE_FIXED = 10,
	/* One 512-byte fixed-disk write through the low-memory BIOS bounce area. */
	BOOTS_BIOS_DISK_WRITE = 11,
	/* Stop displaying G-VRAM (INT 18h, AH=41h). */
	BOOTS_BIOS_DISPLAY_STOP = 12,
	/* One byte of the BIOS real-time key state table; request.status
	 * selects the scan-code group (0..15). */
	BOOTS_BIOS_KEY_STATE = 13,
};

struct boots_bios_request {
	uint16_t service;
	uint16_t status;
	uint8_t bios_id;
	uint8_t heads;
	uint8_t sectors;
	uint8_t reserved;
	uint32_t lba;
	uint32_t buffer;
} __attribute__((packed));

#define BOOTS_APPLET_MAGIC 0x41383942U /* "B98A" */
struct boots_applet_header {
	uint32_t magic;
	uint16_t abi_version;
	uint16_t header_size;
	uint32_t image_size;
	uint16_t entry_offset;
	uint16_t flags;
	uint32_t crc32;
	char name[16];
} __attribute__((packed));

struct boots_applet_services {
	uint16_t abi_version;
	uint16_t size;
	void (*putc)(char c);
	void (*puts)(const char *s);
	uint32_t (*key_read)(void);
};

typedef uint32_t (*boots_applet_entry_t)(
        const struct boots_applet_services *services, uint32_t argc,
        const char *const *argv);

typedef uint32_t (*boots_bios_gateway_t)(struct boots_bios_request *request);

_Static_assert(sizeof(struct boots_stage2_header) == 20,
               "Boots Stage 2 header must remain 20 bytes");
_Static_assert(sizeof(struct boots_bootstrap_handoff) == 24,
               "Boots bootstrap handoff must remain 24 bytes");
_Static_assert(sizeof(struct boots_handoff) == 24,
	       "Boots handoff version 2 must remain 24 bytes");
_Static_assert(sizeof(struct boots_bios_request) == 16,
               "Boots BIOS request must remain 16 bytes");
_Static_assert(sizeof(struct boots_applet_header) == 36,
               "Boots applet header must remain 36 bytes");

enum boots_device_class {
	BOOTS_DEV_FDD = 1,
	BOOTS_DEV_IDE = 2,
	BOOTS_DEV_SCSI = 3,
};

enum boots_device_flags {
	BOOTS_DEV_PRESENT = 1U << 0,
	BOOTS_DEV_HAS_GEOMETRY = 1U << 1,
	BOOTS_DEV_BOOT_ORIGIN = 1U << 2,
};

/* flags in the Linux SETUP_PC98_DISK payload */
enum boots_linux_disk_flags {
	BOOTS_LINUX_DISK_F_FALLBACK = 1U << 0,
	BOOTS_LINUX_DISK_F_BOOT = 1U << 1,
};

struct boots_device {
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

_Static_assert(sizeof(struct boots_device) == 16,
               "Boots device descriptor ABI must remain 16 bytes");

#endif
