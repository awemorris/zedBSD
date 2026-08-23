/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Graphics device
 */

#ifndef ZEDBSD_KERN_GRAPHICS_DEVICE_H
#define ZEDBSD_KERN_GRAPHICS_DEVICE_H

int
graphics_device_register(void);

void
graphics_device_restore_text(void);

#endif
