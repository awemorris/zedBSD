/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's parts the MC68040 code calls.
 */

#ifndef KERN_HAL_M68K_VIRT_BSP_H
#define KERN_HAL_M68K_VIRT_BSP_H

#include <hal/hal.h>

/*
 * The board's devices, from the boot information: the six interrupt
 * controllers, the clock, the terminal, the control device and the first
 * virtio slot.
 */
struct virt_board_info {
	uint32_t pic_base;
	uint32_t rtc_base;
	uint32_t tty_base;
	uint32_t ctrl_base;
	uint32_t virtio_base;
	uint32_t virtio_irq;
	uint32_t memory_base;
	uint32_t memory_size;
	uint32_t ramdisk_base;
	uint32_t ramdisk_size;
	uint32_t qemu_version;
};

const struct virt_board_info *virt_board(void);
void virt_cmain(const void *records);
uint32_t virt_bootinfo_parse(const void *records);
const struct kern_boot_handoff *virt_kernel_handoff(void);

void virt_pic_init(void);
void virt_pic_enable_clock(unsigned irq);
bool virt_pic_pending(unsigned irq);
void virt_take_interrupt(unsigned level);

void virt_clock_init(void);
void virt_clock_interrupt(void);
bool virt_clock_ticked(void);

void virt_tty_putc(char character);


#endif
