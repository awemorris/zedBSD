/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the arm64 HAL asks of the board support.
 *
 * The board's directory (bsp-rpi4/, bsp-rpi5/, bsp-odroidc5/) is on the
 * include path, so board.h is the board's own: its name and its fixed
 * addresses.  What the Raspberry Pi boards share is in bsp-rpi-common/, and
 * what the S905X5M boards share in bsp-s905x5m-common/, whose directory is
 * on the include path after the board's.  Besides the functions below,
 * each board defines hal_reset() and hal_poweroff().
 */

#ifndef KERN_HAL_ARM64_BSP_H
#define KERN_HAL_ARM64_BSP_H

#include <hal/types.h>
#include "board.h"
#include "fdt.h"

void arm64_boot_set_info(const struct arm64_fdt_info *info, uintptr_t fdt_phys);
void arm64_boot_set_framebuffer(uint64_t phys, uint64_t size, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format);
const struct arm64_fdt_info *arm64_boot_info(void);
int arm64_boot_is_no_map(uint64_t base, uint64_t size);
uintptr_t arm64_boot_fdt_phys(void);
const void *arm64_kernel_handoff(void);
void prekern_bsp_cons_init(void);

/*
 * Finds the framebuffer the firmware or the bootloader gives the console
 * and hands it to arm64_framebuffer_adopt(); returns 0 when the framebuffer
 * is ready, and nonzero when the console stays on the UART.
 */
int arm64_bsp_framebuffer_init(void);
void prekern_bsp_cons_irq_init(void);

/*
 * Gives the framebuffer up to the kernel's console once it removes the
 * early console's window of it: nothing in the HAL paints it again.
 */
void arm64_cons_framebuffer_release(void);

/*
 * Maps the peripherals the HAL itself uses into the kernel's tables while
 * arm64_space_init() builds them, before they are switched to.
 */
void arm64_bsp_map_devices(void);

/*
 * One plane the board's display compositor shows that the stall watch can
 * draw its report on: an unscaled 32-bit image, where its pixels are, how
 * they are laid out, and where it sits on the screen.  handle is the
 * board's own name for the plane (arm64_bsp_watch_show_alone()).
 */
struct arm64_watch_plane {
	uint32_t handle;
	uint64_t physical;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
};

/*
 * Maps the display compositor the stall watch writes its report onto, at
 * boot while no CPU can be stuck holding the table lock; a board without
 * one the watch knows does nothing.
 */
void arm64_bsp_watch_init(void);

/*
 * Reports the unscaled 32-bit planes the compositor shows now, at most
 * capacity of them; 0 on a board without a compositor the watch knows,
 * which leaves the report on the early console's framebuffer.  It runs
 * while CPUs are stopped: it may not lock, allocate or wait.
 */
unsigned arm64_bsp_watch_planes(struct arm64_watch_plane *planes, unsigned capacity);

/*
 * Has the compositor show one of the planes arm64_bsp_watch_planes()
 * reported alone, so that no window above it hides the report; a display
 * driver that still runs takes the screen back with its next flip.
 */
void arm64_bsp_watch_show_alone(const struct arm64_watch_plane *plane);

/*
 * Reports a core's number from its MPIDR affinity: the field the board
 * numbers its cores by.  It names the core's GIC CPU interface and its
 * boot block (locore-board.h's arm64_board_core_index computes the same).
 */
uint64_t arm64_bsp_core_of(uint64_t mpidr);

/* Tells whether the board can release a core the way the device tree says it is held. */
int arm64_bsp_cpu_startable(const struct arm64_fdt_cpu *cpu);

/*
 * Releases a secondary core to a physical entry point, with its MMU off;
 * reports HAL_OK when the core was let go.
 */
int arm64_bsp_cpu_release(const struct arm64_fdt_cpu *cpu, uint64_t entry_phys);

#endif
