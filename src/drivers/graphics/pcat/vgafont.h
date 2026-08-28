/* Built-in IBM PC-compatible VGA font.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DRIVERS_GRAPHICS_PCAT_VGAFONT_H
#define ZEDBSD_DRIVERS_GRAPHICS_PCAT_VGAFONT_H

#include <stdint.h>

#define PCAT_VGAFONT_GLYPHS 256U
#define PCAT_VGAFONT_HEIGHT 16U

extern const uint8_t
pcat_vgafont16[PCAT_VGAFONT_GLYPHS * PCAT_VGAFONT_HEIGHT];

#endif
