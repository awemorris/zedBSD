/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The processor's part of the PowerPC Mac's shared support: each
 * processor's board directory says how the firmware is called, how the
 * time base and the decrementer are reached, how interrupts are masked
 * and how the HAL locks.
 */

#ifndef KERN_HAL_POWERMAC_MAC_ARCH_H
#define KERN_HAL_POWERMAC_MAC_ARCH_H

#if defined(HAL_ARCH_PPC32)
#include "../ppc32/bsp-mac99/mac-arch.h"
#elif defined(HAL_ARCH_PPC64)
#include "../ppc64/bsp-macppc64/mac-arch.h"
#else
#error "the PowerPC Mac's support serves the 32-bit and 64-bit PowerPC"
#endif

#endif
