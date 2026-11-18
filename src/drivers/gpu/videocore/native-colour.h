/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Integer-only clear conversion preserves the kernel's no-floating-point execution contract. */
#ifndef KERN_DRIVERS_GPU_BCM2711_NATIVE_COLOUR_H
#define KERN_DRIVERS_GPU_BCM2711_NATIVE_COLOUR_H

#include <stdint.h>

/* Copies four raw IEEE component words into one clamped nearest-even packed colour; swap selects BGRA storage. */
int bcm2711_native_colour_pack(const uint32_t words[4], uint32_t swap, uint32_t *colour);

/* Converts four raw clear words to a tile buffer target's clear colour words of its internal type and bpp. */
int bcm2711_native_colour_clear(const uint32_t words[4], uint32_t type, uint32_t bpp, uint32_t swap, uint32_t clear[4]);

/* Packs four raw clear words into one raster pixel of an Output Image Format; ENOTSUP for a format it does not pack. */
int bcm2711_native_colour_texel(const uint32_t words[4], uint32_t format, uint32_t swap, uint8_t *texel, uint32_t *bytes);

#endif
