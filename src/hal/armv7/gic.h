/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GICv1 and GICv2 interrupt controller of the armv7 boards: the
 * PL390 of the Cortex-A9 MPCore and the GICv2 of the emulator's virt
 * board, whose registers are the same.
 */

#ifndef KERN_HAL_ARMV7_GIC_H
#define KERN_HAL_ARMV7_GIC_H

#include <hal/types.h>

/*
 * The software-generated interrupts the HAL uses between processors: one
 * that stops a processor after a panic, and the kernel's notification.
 */
#define ARMV7_SGI_STOP		0U
#define ARMV7_SGI_NOTIFY	1U

/* The interrupt number the acknowledge register gives when nothing is pending. */
#define ARMV7_GIC_SPURIOUS	1023U

/* The interrupt number in an acknowledge value; the rest names an SGI's sender. */
#define ARMV7_GIC_INTID_MASK	0x3ffU

void armv7_gic_init(void);
void armv7_gic_init_cpu(void);
uint32_t armv7_gic_ack(void);
void armv7_gic_eoi(uint32_t value);
uint32_t armv7_gic_line_count(void);
void armv7_gic_mask(uint32_t intid);
void armv7_gic_unmask(uint32_t intid);
int armv7_gic_set_trigger(uint32_t intid, int trigger);
void armv7_gic_send_sgi(uint32_t sgi, uint32_t target_interfaces);

#endif
