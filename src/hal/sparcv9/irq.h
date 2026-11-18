/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's interrupt acknowledgement.
 */

#ifndef KERN_HAL_SPARCV9_IRQ_H
#define KERN_HAL_SPARCV9_IRQ_H

#include <hal/hal.h>

/*
 * Starts the handling of one interrupt and returns its acknowledgement.
 */
hal_irq_ack_t
sparcv9_irq_begin(
	int irq);

#endif
