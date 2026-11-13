/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the armv7 HAL asks of its board: the console, the parts of the
 * machine the device tree does not describe or that the board knows
 * better, the timer, and the ways the machine stops.
 */

#ifndef KERN_HAL_ARMV7_BSP_H
#define KERN_HAL_ARMV7_BSP_H

#include <hal/types.h>
#include "fdt.h"

/* The scheduling tick's rate on every board, in hertz, which never changes. */
#define ARMV7_TICK_HZ	1000U

/*
 * The board's scheduling tick and counter.
 *
 * init_boot reads what the timer needs once, on the boot processor;
 * init_cpu starts the calling processor's periodic tick, one interval
 * ahead; ack_tick ends the timer's own interrupt and arms the next one,
 * before the interrupt controller ends the tick; read_counter reads the
 * free-running counter and its frequency; tick_irq names the tick's GIC
 * interrupt.  The board keeps one table of these for the kernel's life.
 */
struct armv7_clock_ops {
	int (*init_boot)(const struct armv7_fdt_info *info);
	void (*init_cpu)(void);
	void (*ack_tick)(void);
	bool (*read_counter)(uint64_t *counter, uint64_t *frequency);
	uint32_t (*tick_irq)(void);
};

/*
 * The board's entry points.
 *
 * bsp_cpu_early_setup runs in locore.S on the boot processor before the
 * MMU and the caches are on, from the physical copy of the image: it may
 * use r0 to r3 and r12 only and reach nothing by a linked address (the
 * ZYBO turns on the SCU and the processor's coherency bits there).
 * bsp_early_init makes the console reachable; bsp_find_fdt names the
 * device tree's physical address from the loader's r2, or 0.
 */
void bsp_cpu_early_setup(void);
void bsp_early_init(void);
uintptr_t bsp_find_fdt(uintptr_t loader_argument);
void bsp_init(const struct armv7_fdt_info *info);
void bsp_describe(void);
void bsp_cons_putc(int character);
int bsp_cons_getc(void);
int bsp_gic_bases(const struct armv7_fdt_info *info, uint64_t *distributor, uint64_t *cpu_interface, int *secure);
const struct armv7_clock_ops *bsp_clock(void);
unsigned bsp_reserved_ranges(struct armv7_fdt_range *ranges, unsigned capacity);
int bsp_is_emulated(void);
void bsp_reset(void);
void bsp_poweroff(void);

#endif
