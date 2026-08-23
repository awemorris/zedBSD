/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/mouse event interface
 */

#ifndef ZEDBSD_UAPI_MOUSE_H
#define ZEDBSD_UAPI_MOUSE_H

#include <stdint.h>

#define ZEDBSD_MOUSE_BUTTON_LEFT	0x0001U
#define ZEDBSD_MOUSE_BUTTON_MIDDLE	0x0002U
#define ZEDBSD_MOUSE_BUTTON_RIGHT	0x0004U

#define ZEDBSD_MOUSE_EVENT_OVERFLOW	0x0001U

/*
 * Fixed-size relative-pointer event returned by read(2).
 */
struct mouse_event {
	uint64_t timestamp_ns;
	uint32_t sequence;
	uint16_t flags;
	uint16_t reserved;
	uint32_t device_id;
	int32_t dx;
	int32_t dy;
	uint32_t buttons;
};

#endif
