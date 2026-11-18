/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The backlight where the backend does not offer it yet (ws113-p013):
 * Linux (sysfs /sys/class/backlight) and FreeBSD (backlight(9)) come with
 * ws113-p010.  Every call answers ENOTSUP, and nothing is opened.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stddef.h>

/*
 * Opens nothing.
 */
int
kl_backend_backlight_open(
	struct kl_backend_backlight **backlight)
{
	/* Somewhere to put it. */
	if (backlight == NULL)
		return EINVAL;

	/* No backlight is offered here. */
	*backlight = NULL;
	return ENOTSUP;
}

/*
 * Reads nothing.
 */
int
kl_backend_backlight_get(
	struct kl_backend_backlight *backlight,
	unsigned *percent)
{
	/* No backlight is offered here. */
	(void)backlight;
	(void)percent;
	return ENOTSUP;
}

/*
 * Sets nothing.
 */
int
kl_backend_backlight_set(
	struct kl_backend_backlight *backlight,
	unsigned percent)
{
	/* No backlight is offered here. */
	(void)backlight;
	(void)percent;
	return ENOTSUP;
}

/*
 * Closes nothing.
 */
void
kl_backend_backlight_close(
	struct kl_backend_backlight *backlight)
{
	/* Nothing was opened. */
	(void)backlight;
}
