/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scheduling tick of each CPU, from the generic timer.
 */

#ifndef KERN_HAL_ARM64_CLOCK_H
#define KERN_HAL_ARM64_CLOCK_H

#include <hal/hal.h>

void arm64_timer_init(void);
void arm64_timer_interrupt(hal_irq_ack_t acknowledge);

#endif
