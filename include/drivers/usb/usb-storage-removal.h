/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a USB storage device's detach does with its disk (BUG-192,
 * usb-storage.c): a pure decision, so that the host tests check it alone.
 *
 * An idle disk goes at once.  A disk in use (mounted) whose device was
 * pulled out cannot go yet: its medium is revoked (every I/O fails with
 * ENXIO, opening it fails, so a scan sees it gone), the system's events are
 * told it and its partitions are gone, and the detach is refused (EBUSY);
 * the USB core tries the detach again on its periodic scans, and once the
 * file system is unmounted the revoked medium is retired and the disk freed.
 * A detach that is not the device's going away (no FORCE) only waits.
 */

#ifndef DRIVERS_USB_USB_STORAGE_REMOVAL_H
#define DRIVERS_USB_USB_STORAGE_REMOVAL_H

#include <uapi/errno.h>

/* What the detach does next. */
enum storage_removal_step {
	/* The disk went (retired or removed); it is destroyed now. */
	STORAGE_REMOVAL_DESTROY,
	/* The disk is in use and its device is gone: revoke the medium, post its going, and wait. */
	STORAGE_REMOVAL_REVOKE,
	/* Wait (and report the error): in use, or a refusal to try again later. */
	STORAGE_REMOVAL_WAIT
};

/*
 * Decides the next step from whether the medium was already revoked, what
 * removing (or retiring) the disk answered, and whether the device is going
 * away (DRV_USB_DETACH_FORCE).
 */
static inline enum storage_removal_step
storage_removal_decide(
	int revoked,
	int error,
	int force)
{
	/* Removed or retired. */
	if (error == 0)
		return STORAGE_REMOVAL_DESTROY;

	/* In use, the device gone, the medium not revoked yet: revoke it. */
	if (error == EBUSY && force && !revoked)
		return STORAGE_REMOVAL_REVOKE;

	/* Succeeded: anything else waits for the next try. */
	return STORAGE_REMOVAL_WAIT;
}

#endif
