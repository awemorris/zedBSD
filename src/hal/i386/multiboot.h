/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GRUB multiboot specification support
 */

#ifndef ZEDBSD_HAL_I386_MULTIBOOT_H
#define ZEDBSD_HAL_I386_MULTIBOOT_H

#include <hal/types.h>

#define MULTIBOOT_HEADER_MAGIC 0x1badb002U
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2badb002U
#define MULTIBOOT_HEADER_FLAGS 0x00000003U

#define MBINFO_FLAG_MEMORY       (1U << 0)
#define MBINFO_FLAG_BOOT_DEVICE  (1U << 1)
#define MBINFO_FLAG_CMDLINE      (1U << 2)
#define MBINFO_FLAG_MODULES      (1U << 3)
#define MBINFO_FLAG_ELF          (1U << 5)
#define MBINFO_FLAG_MMAP         (1U << 6)
#define MBINFO_FLAG_LOADER_NAME  (1U << 9)

struct multiboot_info {
	uint32_t flags;
	uint32_t mem_lower;
	uint32_t mem_upper;
	uint32_t boot_device;
	uint32_t cmdline;
	uint32_t mods_count;
	uint32_t mods_addr;
	union {
		struct {
			uint32_t tabsize, strsize, addr, reserved;
		} aout;
		struct {
			uint32_t num, size, addr, shndx;
		} elf;
	} symbols;
	uint32_t mmap_length;
	uint32_t mmap_addr;
	uint32_t drives_length;
	uint32_t drives_addr;
	uint32_t config_table;
	uint32_t boot_loader_name;
	uint32_t apm_table;
	uint32_t vbe_control_info;
	uint32_t vbe_mode_info;
	uint16_t vbe_mode;
	uint16_t vbe_interface_seg;
	uint16_t vbe_interface_off;
	uint16_t vbe_interface_len;
};

struct multiboot_mmap_entry {
	uint32_t size;
	uint32_t base_low;
	uint32_t base_high;
	uint32_t length_low;
	uint32_t length_high;
	uint32_t type;
} __attribute__((packed));

#endif
