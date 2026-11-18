/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 keyboard driver.
 *
 * The keyboard is an 8251 on its own interrupt; the bus mouse is a
 * separate chip with a separate driver.
 */

#ifndef KERN_DRIVERS_PC98_KEYBOARD_H
#define KERN_DRIVERS_PC98_KEYBOARD_H

int drv_pc98_keyboard_init(void);

#endif
