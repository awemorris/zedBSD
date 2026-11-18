/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The backlight devices' class (src/drivers/generic/backlight.c).
 *
 * A driver that controls a light (a panel's backlight) registers it as a
 * provider and the class publishes /dev/backlight/backlightN, which answers
 * the requests of uapi/backlight.h.  The provider's operations may sleep;
 * the class calls them one at a time per device, never after the provider
 * was unregistered.
 */

#ifndef KERN_KERN_BACKLIGHT_H
#define KERN_KERN_BACKLIGHT_H

#include <stdint.h>

struct kern_backlight;

/* The most backlight devices published at once. */
#define KERN_BACKLIGHT_MAX	4U

/*
 * What a provider does: reads the brightness into *percent, or sets it to
 * percent (0..100).  Each returns 0, EBUSY while the light cannot be reached,
 * or another errno value.
 */
struct kern_backlight_ops {
	int (*get)(void *context, uint32_t *percent);
	int (*set)(void *context, uint32_t percent);
};

/*
 * Registers a provider (name: what GETINFO reports, type: a
 * BACKLIGHT_TYPE_*) and publishes its device.
 */
int
kern_backlight_register(
	const char *name,
	uint32_t type,
	const struct kern_backlight_ops *ops,
	void *context,
	struct kern_backlight **result);

/*
 * Withdraws a provider: no operation runs after this returns, and its
 * device answers ENXIO until the last open of it is closed.
 */
void
kern_backlight_unregister(
	struct kern_backlight *backlight);

#endif
