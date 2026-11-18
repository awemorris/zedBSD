/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The backlight devices: /dev/backlight/backlightN.
 *
 * The names and the structures follow FreeBSD's sys/backlight.h, so a
 * program written for backlight(9) builds unchanged; the request numbers
 * are zedBSD's own (the group 'G' is the GPU's here) and no binary
 * compatibility is intended.  The brightness is a percentage: 0 is the
 * device's lowest light, not off.
 */

#ifndef KERN_UAPI_BACKLIGHT_H
#define KERN_UAPI_BACKLIGHT_H

#include <stdint.h>
#include <uapi/ioctl.h>

#define KERN_BACKLIGHT_IOC_GROUP	'L'

/* The most levels a device names, and the longest provider name. */
#define BACKLIGHTMAXLEVELS		100
#define BACKLIGHTMAXNAMELENGTH		64

/*
 * The brightness (0..100) and the levels the device has: nlevels of
 * levels[], each a percentage, or none (nlevels 0) when any percentage is
 * taken.  BACKLIGHTUPDATESTATUS reads only brightness.
 */
struct backlight_props {
	uint32_t brightness;
	uint32_t nlevels;
	uint32_t levels[BACKLIGHTMAXLEVELS];
};

/* What a backlight lights. */
enum backlight_info_type {
	BACKLIGHT_TYPE_PANEL = 0,
	BACKLIGHT_TYPE_KEYBOARD
};

/* The provider's name ("i915") and what it lights. */
struct backlight_info {
	char name[BACKLIGHTMAXNAMELENGTH];
	enum backlight_info_type type;
};

/*
 * GETSTATUS reports the brightness; UPDATESTATUS sets it (the file must be
 * open for writing: EBADF otherwise; EINVAL above 100); both answer EBUSY
 * while the provider cannot reach the light (the panel is not lit by the
 * driver), ENXIO once the provider is gone and EIO for a driver error.
 */
#define BACKLIGHTGETSTATUS	_IOWR(KERN_BACKLIGHT_IOC_GROUP, 0, struct backlight_props)
#define BACKLIGHTUPDATESTATUS	_IOWR(KERN_BACKLIGHT_IOC_GROUP, 1, struct backlight_props)
#define BACKLIGHTGETINFO	_IOWR(KERN_BACKLIGHT_IOC_GROUP, 2, struct backlight_info)

#endif
