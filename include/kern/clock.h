/*
 * Clock
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_CLOCK_H
#define ZEDBSD_KERN_CLOCK_H

#include <stdint.h>

uint64_t zedbsd_kernel_ticks(void);
uint64_t zedbsd_kernel_milliseconds(void *context);
void zedbsd_clock_realtime(int32_t *seconds, int32_t *nanoseconds);

#endif
