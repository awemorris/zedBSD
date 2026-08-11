/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_KERN_CLOCK_H
#define BOOTS_KERN_CLOCK_H

#include <stdint.h>

uint64_t boots_kernel_ticks(void);
uint64_t boots_kernel_milliseconds(void *context);

#endif
