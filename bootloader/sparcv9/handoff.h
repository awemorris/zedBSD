/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u boot loader's handoff to the HAL, and its OpenFirmware builder.
 *
 * The second stage hands the HAL the common boot record followed by what
 * OpenFirmware told it about the machine.  The HAL checks it, keeps a copy,
 * and gives the kernel only the common record; the layout below is known to
 * the loader and the HAL alone.
 */

#ifndef BOOTLOADER_SPARCV9_HANDOFF_H
#define BOOTLOADER_SPARCV9_HANDOFF_H

#include <kern/boot.h>

/* The version the loader writes into the common record. */
#define SUN4U_HANDOFF_COMMON_VERSION	4U

/*
 * The magic and the version of the sun4u part that follows the common
 * record.  Version 2 adds the PCI memory space and the display; version 3
 * adds the firmware framebuffer and the 8042 keyboard controller.
 */
#define SUN4U_HANDOFF_EXTENSION_MAGIC	0x53345548U /* "S4UH" */
#define SUN4U_HANDOFF_EXTENSION_VERSION	3U

/* How many installed and how many available memory ranges a handoff holds. */
#define SUN4U_MAX_MEMORY_RANGES		16U

/* The size of the OpenFirmware boot path, its terminator included. */
#define SUN4U_BOOTPATH_SIZE		256U

/*
 * One physical memory range as OpenFirmware reports it in a memory node's
 * "reg" or "available" property.
 */
struct sun4u_memory_range {
	uint64_t base;
	uint64_t size;
} __attribute__((packed));

/*
 * The handoff the second stage builds for the HAL.
 *
 * The loader fills one in its own memory and passes its address; the HAL
 * copies it once during early boot.  The display's framebuffer and its
 * registers are PCI memory addresses, which lie at pci_memory_base in the
 * processor's physical space; a zero display_vendor means no display was
 * found.
 *
 * The firmware framebuffer is the display OpenFirmware drew on, as its
 * node describes it: a physical address, the size in pixels, the bytes of
 * a row and a KERN_BOOT_FRAMEBUFFER_* format, with the display's PCI IDs,
 * zero for a display that is not on PCI.  A zero framebuffer_phys means the
 * loader found none it can hand over; the standard VGA above remains for
 * the HAL to set up itself.  The 8042's data and command ports are offsets
 * in the PCI I/O space, zero when there is no 8042.
 */
struct sun4u_boot_handoff {
	struct kern_boot_handoff common;
	uint32_t extension_magic;
	uint16_t extension_version;
	uint16_t extension_size;
	uint64_t tick_frequency;
	uint8_t installed_count;
	uint8_t available_count;
	uint8_t boot_channel;
	uint8_t boot_drive;
	struct sun4u_memory_range installed[SUN4U_MAX_MEMORY_RANGES];
	struct sun4u_memory_range available[SUN4U_MAX_MEMORY_RANGES];
	uint64_t pci_io_base;
	uint32_t serial_io_offset;
	uint16_t ide_vendor;
	uint16_t ide_device;
	uint16_t ide_primary_command;
	uint16_t ide_primary_control;
	uint16_t ide_secondary_command;
	uint16_t ide_secondary_control;
	uint64_t pci_memory_base;
	uint16_t display_vendor;
	uint16_t display_device;
	uint32_t display_framebuffer;
	uint32_t display_framebuffer_size;
	uint32_t display_registers;
	uint64_t framebuffer_phys;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint32_t framebuffer_pitch;
	uint32_t framebuffer_format;
	uint16_t framebuffer_vendor;
	uint16_t framebuffer_device;
	uint16_t i8042_data;
	uint16_t i8042_command;
	char bootpath[SUN4U_BOOTPATH_SIZE];
} __attribute__((packed));

/*
 * Fills a handoff from OpenFirmware's device tree and the boot path.
 *
 * Returns 0, or -1 when the tree lacks a device the HAL needs.
 */
int
sparcv9_handoff_build(
	struct sun4u_boot_handoff *handoff,
	const char *bootpath);

#endif
