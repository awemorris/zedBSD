/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * How a USB storage device finds its medium and its partitions (BUG-258,
 * usb-storage.c): pure decisions, so that the host tests check them alone.
 *
 * A card reader reports several logical units (LUNs), one per slot, and the
 * card can sit in any of them.  The driver probes LUN 0 upwards and keeps the
 * first one that holds a readable medium.  When no LUN has one, the reader is
 * still attached without a disk if at least one slot reported its medium as
 * absent, and its control worker probes every LUN again later.
 *
 * A disk published by a hot plug has its partition table read by the control
 * worker.  A disk that is already there when the kernel boots has its table
 * read by the boot scan of the disks instead, and the worker must not race
 * that scan or replace the partitions the root is mounted from.  The kernel
 * mounts its root only after the boot scan, so the worker waits for the root,
 * and then leaves alone a disk whose partitions are already published.
 */

#ifndef DRIVERS_USB_USB_STORAGE_MEDIA_H
#define DRIVERS_USB_USB_STORAGE_MEDIA_H

#include <uapi/errno.h>

/* What the control worker does next about a disk's partition table. */
enum storage_partition_step {
	/* The root is not mounted yet: the boot scan may still read this disk, so try again later. */
	STORAGE_PARTITIONS_WAIT,
	/* The partitions are already published (by the boot scan): nothing to read. */
	STORAGE_PARTITIONS_KEEP,
	/* Read the partition table now. */
	STORAGE_PARTITIONS_RELOAD
};

/*
 * What the probes of a device's LUNs have found so far.
 *
 * One instance lives on the stack of one probe pass, from LUN 0 to the
 * device's last LUN.
 */
struct storage_lun_scan {
	/* Nonzero once a LUN holds a readable medium; the pass stops there. */
	int found;
	/* Nonzero once any LUN reported its medium as absent. */
	int absent_seen;
	/* The error of the first LUN that reported its medium as absent. */
	int absent_error;
	/* The error of the first LUN that failed for another reason. */
	int first_error;
};

/*
 * Decides what to do about a disk's partition table from whether the root is
 * mounted and whether partitions of the disk are already published.
 */
static inline enum storage_partition_step
storage_partition_decide(
	int root_mounted,
	int published)
{
	/* The boot scan has not finished before the root is mounted. */
	if (!root_mounted)
		return STORAGE_PARTITIONS_WAIT;

	/* The boot scan read this disk; reloading would race the mounted root. */
	if (published)
		return STORAGE_PARTITIONS_KEEP;

	/* Succeeded: a disk nobody has read yet gets its table read. */
	return STORAGE_PARTITIONS_RELOAD;
}

/* Starts a probe pass that has seen no LUN yet. */
static inline void
storage_lun_scan_init(
	struct storage_lun_scan *scan)
{
	/* Nothing found, nothing absent, no error yet. */
	scan->found = 0;
	scan->absent_seen = 0;
	scan->absent_error = 0;
	scan->first_error = 0;
}

/*
 * Records one LUN's probe, from the error it reported and whether it reported
 * its medium as absent, and tells whether the pass stops at this LUN.
 */
static inline int
storage_lun_scan_record(
	struct storage_lun_scan *scan,
	int error,
	int absent)
{
	/* This LUN holds a readable medium: the pass keeps it and stops. */
	if (error == 0) {
		scan->found = 1;
		return 1;
	}

	/* An empty slot does not stop the pass. */
	if (absent) {
		/* Keeps the error of the first empty slot. */
		if (!scan->absent_seen)
			scan->absent_error = error;

		/* Remembers an empty slot so that the reader can wait for a card. */
		scan->absent_seen = 1;
		return 0;
	}

	/* Another failure: the first one is what an attach reports. */
	if (scan->first_error == 0)
		scan->first_error = error;

	/* The pass goes on to the next LUN. */
	return 0;
}

/*
 * Reports the outcome of a finished pass: zero when a LUN was found, else the
 * error to report, with *medium_absent set when the reader is to wait for a
 * medium rather than fail.
 */
static inline int
storage_lun_scan_finish(
	const struct storage_lun_scan *scan,
	int *medium_absent)
{
	/* Starts from a pass that did not end in an empty reader. */
	*medium_absent = 0;

	/* An empty slot makes the device a reader that waits for a card. */
	if (!scan->found && scan->absent_seen) {
		*medium_absent = 1;
		return scan->absent_error;
	}

	/* Every LUN failed otherwise: the first failure is reported. */
	if (!scan->found && scan->first_error != 0)
		return scan->first_error;

	/* A pass that probed nothing found no device. */
	if (!scan->found)
		return ENODEV;

	/* Succeeded: the LUN with a medium is used. */
	return 0;
}

#endif
