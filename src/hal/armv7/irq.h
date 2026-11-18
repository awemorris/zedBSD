/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 interrupt dispatch, inside the HAL.
 */

#ifndef KERN_HAL_ARMV7_IRQ_H
#define KERN_HAL_ARMV7_IRQ_H

#include <hal/hal.h>

void
armv7_irq_init(
	void);

void
armv7_irq_take(
	void);

#endif
