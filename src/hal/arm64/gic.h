/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board's GIC-400 (GICv2) interrupt controller.
 */

#ifndef KERN_HAL_ARM64_GIC_H
#define KERN_HAL_ARM64_GIC_H

#include <hal/types.h>

/*
 * The software-generated interrupts the HAL uses between CPUs: one that
 * stops a CPU after a panic, and the kernel's notification.
 */
#define ARM64_SGI_STOP 0U
#define ARM64_SGI_NOTIFY 1U

void arm64_gic_init(void);
uint32_t arm64_gic_ack(void);
void arm64_gic_eoi(uint32_t value);
void arm64_gic_mask(uint32_t intid);
void arm64_gic_unmask(uint32_t intid);
int arm64_gic_set_trigger(uint32_t intid, int trigger);
void arm64_gic_init_cpu(void);
void arm64_gic_send_sgi(uint32_t sgi, uint32_t target_interfaces);

#endif
