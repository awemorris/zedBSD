/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Console device
 */

#ifndef ZEDBSD_KERN_CONSOLE_DEVICE_H
#define ZEDBSD_KERN_CONSOLE_DEVICE_H

int
drv_console_device_register(void);

int
drv_console_input_poll_event(void);

int
drv_console_input_read_event(void);

#endif
