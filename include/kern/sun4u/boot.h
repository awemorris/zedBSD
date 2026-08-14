/* zedBSD sun4u platform boot handoff extension. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_KERN_SUN4U_BOOT_H
#define ZEDBSD_KERN_SUN4U_BOOT_H

#include <kern/boot.h>

#define ZEDBSD_SUN4U_HANDOFF_MAGIC 0x53345548U /* "S4UH" */
#define ZEDBSD_SUN4U_HANDOFF_VERSION 1U
#define ZEDBSD_SUN4U_MAX_MEMORY_RANGES 16U
#define ZEDBSD_SUN4U_BOOTPATH_SIZE 256U

struct zedbsd_sun4u_memory_range {
	uint64_t base;
	uint64_t size;
} __attribute__((packed));

struct zedbsd_sun4u_handoff {
	struct zedbsd_handoff common;
	uint32_t extension_magic;
	uint16_t extension_version;
	uint16_t extension_size;
	uint64_t tick_frequency;
	uint8_t installed_count;
	uint8_t available_count;
	uint8_t boot_channel;
	uint8_t boot_drive;
	struct zedbsd_sun4u_memory_range
	    installed[ZEDBSD_SUN4U_MAX_MEMORY_RANGES];
	struct zedbsd_sun4u_memory_range
	    available[ZEDBSD_SUN4U_MAX_MEMORY_RANGES];
	uint64_t pci_io_base;
	uint32_t serial_io_offset;
	uint16_t ide_vendor;
	uint16_t ide_device;
	uint16_t ide_primary_command;
	uint16_t ide_primary_control;
	uint16_t ide_secondary_command;
	uint16_t ide_secondary_control;
	char bootpath[ZEDBSD_SUN4U_BOOTPATH_SIZE];
} __attribute__((packed));

_Static_assert(sizeof(struct zedbsd_sun4u_memory_range) == 16,
    "sun4u memory ranges must remain 16 bytes");
_Static_assert(sizeof(struct zedbsd_sun4u_handoff) < 8192,
    "sun4u handoff must fit in one 8 KiB page");

#endif
