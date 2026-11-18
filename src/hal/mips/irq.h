/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS interrupt handling inside the HAL.
 */

#ifndef KERN_HAL_MIPS_IRQ_H
#define KERN_HAL_MIPS_IRQ_H

#include <hal/hal.h>

/*
 * The acknowledgement of an interrupt that needs no end at a controller:
 * the tick.  It is not HAL_IRQ_ACK_NONE, and hal_irq_send_eoi() only
 * clears the record of the interrupt being handled for it.
 */
#define MIPS_ACK_LOCAL	((hal_irq_ack_t)~(uintptr_t)0)

void
mips_irq_begin_local(
	void);

#endif
