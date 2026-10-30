/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GIC-400 interrupt controller of the Raspberry Pi 4 and 5.
 */

#ifndef KERN_HAL_ARM64_BSP_RPI_COMMON_GIC_H
#define KERN_HAL_ARM64_BSP_RPI_COMMON_GIC_H

#include <hal/types.h>

/*
 * The software-generated interrupts the HAL uses between CPUs: one that
 * stops a CPU after a panic, and the kernel's notification.
 */
#define RPI_SGI_STOP 0U
#define RPI_SGI_NOTIFY 1U

void rpi_gic_init(void);
uint32_t rpi_gic_ack(void);
void rpi_gic_eoi(uint32_t value);
void rpi_gic_mask(uint32_t intid);
void rpi_gic_unmask(uint32_t intid);
int rpi_gic_set_trigger(uint32_t intid, int trigger);
void rpi_gic_init_cpu(void);
void rpi_gic_send_sgi(uint32_t sgi, uint32_t target_interfaces);

#endif
