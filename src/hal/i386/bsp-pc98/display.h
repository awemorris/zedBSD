/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The native PC-98 GDC display-switching contract.
 */

#ifndef KERN_PLATFORM_PC98_DISPLAY_H
#define KERN_PLATFORM_PC98_DISPLAY_H

int pc98_display_graphics_start(void);
int pc98_display_graphics_stop(void);
int pc98_display_text_restore(void);

#endif
