/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The native PC-98 GDC display-switching contract.
 */

#ifndef ZEDBSD_PLATFORM_PC98_DISPLAY_H
#define ZEDBSD_PLATFORM_PC98_DISPLAY_H

int pc98_display_graphics_start(void);
int pc98_display_graphics_stop(void);
int pc98_display_text_restore(void);

#endif
