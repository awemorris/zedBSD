/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000 boot loader's handoff to the HAL.
 *
 * The second stage hands the HAL the common boot record followed by the
 * machine's RAM, where the kernel and the loader lie, and the memory map.
 * The HAL checks it, keeps a copy, and gives the kernel only the common
 * record; the layout below is known to the loader and the HAL alone.
 */

#ifndef BOOTLOADER_X68K_HANDOFF_H
#define BOOTLOADER_X68K_HANDOFF_H

#include <kern/boot.h>

/* The magic and the version of the X68000 part that follows the common record. */
#define X68K_HANDOFF_EXTENSION_MAGIC	0x58363848U /* "X68H" */
#define X68K_HANDOFF_EXTENSION_VERSION	1U

/* How many memory regions a handoff holds. */
#define X68K_MAX_MEMORY_REGIONS		4U

/* The offset of ram_bytes, which the kernel's first instructions read. */
#define X68K_HANDOFF_RAM_BYTES_OFFSET	32U

#ifndef _ASM_SRC_

#include <stddef.h>
#include <stdint.h>

/*
 * One physical memory region: available RAM, or a part of it taken.
 */
struct x68k_memory_region {
	uint32_t base;
	uint32_t size;
	uint32_t type;
};

/*
 * The handoff the second stage builds for the HAL.
 *
 * The loader fills one at a fixed low address and passes its address; the
 * HAL copies it once during early boot.
 */
struct x68k_boot_handoff {
	struct kern_boot_handoff common;
	uint32_t extension_magic;
	uint16_t extension_version;
	uint16_t extension_size;
	uint32_t ram_bytes;
	uint32_t kernel_phys_start;
	uint32_t kernel_phys_end;
	uint32_t loader_phys_start;
	uint32_t loader_phys_end;
	uint32_t memory_region_count;
	struct x68k_memory_region memory_regions[X68K_MAX_MEMORY_REGIONS];
};

_Static_assert(offsetof(struct x68k_boot_handoff, ram_bytes) == X68K_HANDOFF_RAM_BYTES_OFFSET,
	       "the kernel's entry reads ram_bytes at a fixed offset");

#endif

#endif
