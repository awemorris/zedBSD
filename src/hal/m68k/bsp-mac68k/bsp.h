/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Macintosh (68040) board's parts that its files share.
 */

#ifndef KERN_HAL_M68K_MAC68K_BSP_H
#define KERN_HAL_M68K_MAC68K_BSP_H

#include <hal/hal.h>
#include "../m68040/board.h"

/*
 * The Gestalt machine types the HAL tells apart.
 *
 * Values referenced: QEMU v10.0.0 include/standard-headers/asm-m68k/
 * bootinfo-mac.h (MAC_MODEL_Q800 and MAC_MODEL_Q630).
 */
#define MAC68K_MODEL_QUADRA_800		35U
#define MAC68K_MODEL_QUADRA_630		98U

/*
 * The first VIA, at the I/O window's usual place; the second follows it.
 * The I/O repeats every 256 KiB from 0x50000000, so these are the same
 * registers as the ones at 0x50000000.
 *
 * Values referenced: Linux master arch/m68k/include/asm/mac_via.h (the
 * VIA bases); QEMU v10.0.0 hw/m68k/q800.c (VIA_BASE, the I/O slice and
 * its repetition).
 */
#define MAC68K_VIA1_BASE		0x50f00000U
#define MAC68K_VIA2_BASE		0x50f02000U

/*
 * What the boot information says about the machine, filled once at boot.
 * A field the loader did not give is zero.
 */
struct mac68k_board_info {
	uint32_t model;
	uint32_t memory_bank_count;
	struct m68040_memory_bank memory_banks[M68040_MEMORY_BANKS_MAX];
	uint32_t ramdisk_base;
	uint32_t ramdisk_size;
	uint32_t video_base;
	uint32_t video_depth;
	uint32_t video_row_bytes;
	uint32_t video_width;
	uint32_t video_height;
	uint32_t scc_base;
	uint32_t rom_base;
};

const struct mac68k_board_info *mac68k_board(void);
void mac68k_cmain(const void *records);
uint32_t mac68k_bootinfo_parse(const void *records);
const struct kern_boot_handoff *mac68k_kernel_handoff(void);

void mac68k_via_init(void);
void mac68k_via_take_interrupt(unsigned level);
void mac68k_via_enable_clock(void);
bool mac68k_via_clock_pending(void);
void mac68k_via_power_off(void);

void mac68k_clock_init(void);
void mac68k_clock_interrupt(void);
bool mac68k_clock_ticked(void);

int mac68k_framebuffer_init(void);
unsigned mac68k_framebuffer_columns(void);
unsigned mac68k_framebuffer_rows(void);
void mac68k_framebuffer_cell(unsigned row, unsigned column, int character, bool cursor);
void mac68k_framebuffer_scroll(void);
void mac68k_framebuffer_describe(void);

void mac68k_cons_putc(int character);
void mac68k_cons_attach(void);

#endif
