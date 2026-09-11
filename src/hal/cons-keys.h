/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_HAL_CONS_KEYS_H
#define ZEDBSD_HAL_CONS_KEYS_H

/*
 * Keyboard event encoding for the HAL console input path. These values
 * must match KERN_KEY_* in include/kern/input-device.h because they
 * cross the hal_cons_read_event() boundary. Both copies disappear when
 * console input moves to a kernel-side evdev driver.
 */
#define HAL_KEY_SYMBOL_SIZE	16U
#define HAL_KEY_EVENT_PRESS	0x00000001U
#define HAL_KEY_EVENT_RELEASE	0x00000002U
#define HAL_KEY_EVENT_REPEAT	0x00000004U
#define HAL_KEY_EVENT_RESYNC	0x00000008U
#define HAL_KEY_EVENT_SNAPSHOT	0x00000010U
#define HAL_KEY_EVENT_RESYNC_END	0x00000020U
#define HAL_KEY_EVENT_LOCK_CAPS	0x00000040U
#define HAL_KEY_EVENT_LOCK_KANA	0x00000080U

#endif
