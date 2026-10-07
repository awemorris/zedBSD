/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of how a USB storage device finds its medium and its
 * partitions (BUG-258, include/drivers/usb/usb-storage-media.h compiled
 * unchanged): a probe pass over a card reader's LUNs keeps the first LUN
 * with a medium, waits as an empty reader when only empty slots answered,
 * and fails with the first error otherwise; the partition table of a
 * published disk is read only after the root is mounted and only when the
 * boot scan did not already publish it.
 *
 *   sh plan/ws132/tests/run-host-storage-media.sh
 */

#include "drivers/usb/usb-storage-media.h"

#include <stdio.h>

/* The number of checks that failed, and of those that ran. */
static int failures;
static int checks;

static void check(int condition, const char *what);
static int pass(const int *errors, const int *absent, int count, int *found_lun, int *medium_absent);

/* Counts one check, and reports it when it failed. */
static void
check(
	int condition,
	const char *what)
{
	/* One more check ran. */
	checks++;

	/* A failed check is printed and counted. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/*
 * One probe pass as usb-storage.c runs it, over LUNs whose probes report the
 * given errors and absent flags.  Reports the pass's error, the LUN it stopped
 * at, and whether the reader waits for a medium.
 */
static int
pass(
	const int *errors,
	const int *absent,
	int count,
	int *found_lun,
	int *medium_absent)
{
	struct storage_lun_scan scan;
	int lun;
	int stop;
	int error;

	/* A pass that has seen no LUN yet. */
	storage_lun_scan_init(&scan);
	*found_lun = -1;

	/* Each LUN in turn, stopping at the first with a medium. */
	for (lun = 0; lun < count; lun++) {
		/* Records this LUN's probe. */
		stop = storage_lun_scan_record(&scan, errors[lun], absent[lun]);
		if (stop) {
			*found_lun = lun;
			break;
		}
	}

	/* The outcome of the pass. */
	error = storage_lun_scan_finish(&scan, medium_absent);
	return error;
}

/* Runs every case. */
int
main(void)
{
	static const int single_ok_errors[1] = { 0 };
	static const int single_ok_absent[1] = { 0 };
	static const int single_empty_errors[1] = { EIO };
	static const int single_empty_absent[1] = { 1 };
	static const int single_fail_errors[1] = { ENODEV };
	static const int single_fail_absent[1] = { 0 };
	static const int reader_errors[4] = { EIO, 0, 0, EIO };
	static const int reader_absent[4] = { 1, 0, 0, 1 };
	static const int empty_reader_errors[4] = { EIO, EIO, ENODEV, EIO };
	static const int empty_reader_absent[4] = { 1, 1, 0, 1 };
	static const int failing_errors[2] = { ENODEV, EIO };
	static const int failing_absent[2] = { 0, 0 };
	int found;
	int absent;
	int error;

	/* 1. A single-LUN stick with a medium: LUN 0 is used. */
	error = pass(single_ok_errors, single_ok_absent, 1, &found, &absent);
	check(error == 0 && found == 0 && !absent, "single LUN with a medium: LUN 0");

	/* 2. A single-LUN reader without a card: attached as an empty reader (as before BUG-258). */
	error = pass(single_empty_errors, single_empty_absent, 1, &found, &absent);
	check(error == EIO && found == -1 && absent, "single empty LUN: an empty reader");

	/* 3. A single LUN that fails otherwise: the attach fails with its error (as before). */
	error = pass(single_fail_errors, single_fail_absent, 1, &found, &absent);
	check(error == ENODEV && found == -1 && !absent, "single failing LUN: its error");

	/* 4. A four-slot reader with the card in LUN 1 (the 5330's 05e3:0748 case): LUN 1, not 2. */
	error = pass(reader_errors, reader_absent, 4, &found, &absent);
	check(error == 0 && found == 1 && !absent, "reader: the first LUN with a medium");

	/* 5. A reader whose slots are all empty, one LUN not a disk: an empty reader. */
	error = pass(empty_reader_errors, empty_reader_absent, 4, &found, &absent);
	check(error == EIO && found == -1 && absent, "empty reader: waits for a card");

	/* 6. No LUN empty and none usable: the first failure is reported. */
	error = pass(failing_errors, failing_absent, 2, &found, &absent);
	check(error == ENODEV && found == -1 && !absent, "every LUN failing: the first error");

	/* 7. Before the root is mounted the boot scan may still read the disk: wait. */
	check(storage_partition_decide(0, 0) == STORAGE_PARTITIONS_WAIT, "no root, nothing published: wait");
	check(storage_partition_decide(0, 1) == STORAGE_PARTITIONS_WAIT, "no root, published: wait");

	/* 8. The boot disk, whose partitions the boot scan published (the root is on one): kept. */
	check(storage_partition_decide(1, 1) == STORAGE_PARTITIONS_KEEP, "root mounted, published: keep");

	/* 9. A hot-plugged stick nobody read: its table is read. */
	check(storage_partition_decide(1, 0) == STORAGE_PARTITIONS_RELOAD, "root mounted, nothing published: reload");

	/* The result. */
	if (failures != 0) {
		printf("host-storage-media: %d of %d checks FAILED\n", failures, checks);
		return 1;
	}

	/* Succeeded. */
	printf("host-storage-media: ok (%d checks)\n", checks);
	return 0;
}
