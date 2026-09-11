/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Graphics device
 */

#ifndef KERN_KERN_GRAPHICS_DEVICE_H
#define KERN_KERN_GRAPHICS_DEVICE_H

int
drv_graphics_device_register(void);

void
drv_graphics_device_restore_text(void);

#endif
