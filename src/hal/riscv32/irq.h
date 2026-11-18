/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 interrupt dispatch, inside the HAL.
 */

#ifndef KERN_HAL_RISCV32_IRQ_H
#define KERN_HAL_RISCV32_IRQ_H

#include <hal/hal.h>

/*
 * The acknowledgement of an interrupt that needs no end at a controller:
 * the tick and the notifications between harts.  It is not
 * HAL_IRQ_ACK_NONE, and hal_irq_send_eoi() does nothing for it.
 */
#define RISCV32_ACK_LOCAL	((hal_irq_ack_t)~(uintptr_t)0)

void
riscv32_irq_take_external(
	void);

#endif
