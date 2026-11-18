/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the 64-bit PowerPC HAL asks of a board: the firmware's answers,
 * the form of the hashed page table, the tick and the external
 * interrupts.  Each board's support provides these.
 */

#ifndef KERN_HAL_PPC64_BOARD_H
#define KERN_HAL_PPC64_BOARD_H

#include <hal/hal.h>

/*
 * What the firmware reported, read once before the HAL takes the machine:
 * the RAM, the time base's rate, the hashed table, the display.
 *
 * A board in a logical partition leaves the hashed table to the
 * hypervisor, which the HAL fills through calls, and waits through the
 * hypervisor when idle.  A board that runs the processor directly gives
 * the table's place in RAM; the HAL fills it itself.  The kernel's
 * windows use the large page the processor offers: 64 KiB on the
 * partitions, 16 MiB on the PowerPC 970.  The segment lookaside buffer's
 * size (32 entries on POWER8 and later, 64 on the 970) bounds how many
 * segments of RAM the kernel maps.  The device windows are the
 * 256 MiB blocks of the address space that hold the devices the kernel
 * drives, which the HAL maps at their own addresses, uncached and
 * guarded; the framebuffer is mapped by itself when it is in none of
 * them.
 */
/* The most device windows a board names. */
#define PPC64_BOARD_WINDOWS_MAX	8U

struct ppc64_board_info {
	uint64_t memory_base;
	uint64_t memory_size;
	uint64_t ramdisk_base;
	uint64_t ramdisk_size;
	uint32_t timebase_frequency;
	uint32_t under_hypervisor;
	uint32_t htab_shift;
	uint32_t large_page_shift;
	uint32_t slb_entries;
	uint64_t htab_base;
	uint64_t framebuffer_address;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint32_t framebuffer_linebytes;
	uint32_t framebuffer_depth;
	uint32_t window_count;
	uint64_t windows[PPC64_BOARD_WINDOWS_MAX];
};

const struct ppc64_board_info *
ppc64_board(
	void);

void
ppc64_clock_init(
	void);

void
ppc64_clock_interrupt(
	void);

bool
ppc64_clock_ticked(
	void);

void
ppc64_take_external(
	void);

#endif
