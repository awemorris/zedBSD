/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Public Domain IBM PC-compatible 8x16 VGA font.
 */

#ifndef KERN_DRIVERS_GRAPHICS_PCAT_VGAFONT_H
#define KERN_DRIVERS_GRAPHICS_PCAT_VGAFONT_H

#include <stdint.h>

#define PCAT_VGAFONT_GLYPHS 256U
#define PCAT_VGAFONT_HEIGHT 16U

extern const uint8_t drv_pcat_vgafont16[PCAT_VGAFONT_GLYPHS * PCAT_VGAFONT_HEIGHT];

#endif
