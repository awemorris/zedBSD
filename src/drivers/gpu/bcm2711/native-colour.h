/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Integer-only UNORM clear conversion preserves the kernel's no-floating-point execution contract. */
#ifndef KERN_DRIVERS_GPU_BCM2711_NATIVE_COLOUR_H
#define KERN_DRIVERS_GPU_BCM2711_NATIVE_COLOUR_H

#include <stdint.h>

/* Copies four raw IEEE component words into one clamped nearest-even packed colour; swap selects BGRA storage. */
int bcm2711_native_colour_pack(const uint32_t words[4], uint32_t swap, uint32_t *colour);

#endif
