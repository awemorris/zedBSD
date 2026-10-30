/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the arm64 HAL asks of the board support.
 *
 * The board's directory (bsp-rpi4/, bsp-rpi5/) is on the include path, so
 * board.h is the board's own: its name and its fixed addresses.  What both
 * Raspberry Pi boards share is in bsp-rpi-common/.  Besides the functions
 * below, each board defines hal_reset() and hal_poweroff().
 */

#ifndef KERN_HAL_ARM64_BSP_H
#define KERN_HAL_ARM64_BSP_H

#include <hal/types.h>
#include "board.h"
#include "bsp-rpi-common/fdt.h"

void rpi_boot_set_info(const struct rpi_fdt_info *info, uintptr_t fdt_phys);
void rpi_boot_set_framebuffer(uint64_t phys, uint64_t size, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format);
const struct rpi_fdt_info *rpi_boot_info(void);
int rpi_boot_is_no_map(uint64_t base, uint64_t size);
uintptr_t rpi_boot_fdt_phys(void);
const void *rpi_kernel_handoff(void);
void prekern_bsp_cons_init(void);
void prekern_bsp_cons_irq_init(void);

/*
 * Gives the framebuffer up to the kernel's console once it removes the
 * early console's window of it: nothing in the HAL paints it again.
 */
void rpi_cons_framebuffer_release(void);

/*
 * Maps the peripherals the HAL itself uses into the kernel's tables while
 * arm64_space_init() builds them, before they are switched to.
 */
void arm64_bsp_map_devices(void);

/*
 * Maps the display compositor the stall watch writes its report into, and
 * reports its registers; NULL when the board has none the watch knows.
 */
volatile uint32_t *arm64_bsp_watch_compositor(void);

/*
 * Reports a core's number from its MPIDR affinity: the field the board
 * numbers its cores by.  It names the core's GIC CPU interface and its
 * boot block (locore-board.h's arm64_board_core_index computes the same).
 */
uint64_t arm64_bsp_core_of(uint64_t mpidr);

/* Tells whether the board can release a core the way the device tree says it is held. */
int arm64_bsp_cpu_startable(const struct rpi_fdt_cpu *cpu);

/*
 * Releases a secondary core to a physical entry point, with its MMU off;
 * reports HAL_OK when the core was let go.
 */
int arm64_bsp_cpu_release(const struct rpi_fdt_cpu *cpu, uint64_t entry_phys);

#endif
