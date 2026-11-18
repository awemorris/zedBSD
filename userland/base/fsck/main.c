/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * fsck: checks a UFS volume, and with -y repairs it (BUG-312).
 *
 *   fsck [-n | -y] device
 *
 * Without -y (or with -n) the volume is only read and every problem is
 * reported.  With -y each problem is repaired, and the volume is then
 * checked again without writing: the run answers 0 when the volume was
 * clean, 4 when it was repaired and is now clean, and 8 when a problem
 * is left (or the volume could not be read).  init runs fsck -y on a root
 * the kernel could only mount read-only, and reboots after a repair.
 *
 * The volume's batched journal must have been replayed: the kernel does
 * that at every mount, also a read-only one.
 */

#include "fsck.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fsck_run(const char *name, int repair, unsigned *problems, unsigned *repairs);
static void fsck_usage(void);

/*
 * Runs the fsck command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *name;
	unsigned problems;
	unsigned repairs;
	unsigned left;
	unsigned ignored;
	int repair;
	int index;
	int is_yes;
	int is_no;
	int error;

	/* The options, then the one device. */
	repair = 0;
	name = NULL;
	for (index = 1; index < argc; index++) {
		is_yes = strcmp(argv[index], "-y") == 0;
		is_no = strcmp(argv[index], "-n") == 0;
		if (is_yes) {
			repair = 1;
		} else if (is_no) {
			repair = 0;
		} else if (argv[index][0] == '-' || name != NULL) {
			fsck_usage();
			return FSCK_EXIT_PROBLEMS;
		} else {
			name = argv[index];
		}
	}

	/* A device is needed. */
	if (name == NULL) {
		fsck_usage();
		return FSCK_EXIT_PROBLEMS;
	}

	/* The check, repairing with -y. */
	error = fsck_run(name, repair, &problems, &repairs);
	if (error != 0) {
		fprintf(stderr, "fsck: %s: %s; the volume is left as it is\n", name, strerror(error));
		return FSCK_EXIT_PROBLEMS;
	}

	/* A clean volume. */
	if (problems == 0) {
		printf("fsck: %s: the volume is clean\n", name);
		return FSCK_EXIT_CLEAN;
	}

	/* Problems, only reported without -y. */
	if (!repair) {
		printf("fsck: %s: %u problem(s) found; run fsck -y to repair them\n", name, problems);
		return FSCK_EXIT_PROBLEMS;
	}

	/* After a repair, a second check that writes nothing. */
	error = fsck_run(name, 0, &left, &ignored);
	if (error != 0 || left != 0) {
		printf("fsck: %s: %u problem(s) repaired, but %u are left; the volume is NOT clean\n", name, repairs, left);
		return FSCK_EXIT_PROBLEMS;
	}

	/* Succeeded: repaired, and clean now. */
	printf("fsck: %s: %u problem(s) repaired; ***** FILE SYSTEM WAS MODIFIED *****\n", name, repairs);
	return FSCK_EXIT_REPAIRED;
}

/*
 * Opens, checks and closes a volume once.  Returns 0 with the problems
 * found and repaired, or the error that stopped the check.
 */
static int
fsck_run(
	const char *name,
	int repair,
	unsigned *problems,
	unsigned *repairs)
{
	struct fsck_volume volume;
	int error;
	int close_error;

	/* The volume and its passes. */
	error = fsck_volume_open(&volume, name, repair);
	if (error == 0)
		error = fsck_check(&volume);

	/* What a repair leaves, and the counts. */
	if (error == 0 && repair) {
		printf("fsck: %s: %llu files, %llu blocks used\n", name,
		    (unsigned long long)volume.files, (unsigned long long)volume.used_blocks);
	}

	/* The counts, whatever the outcome. */
	*problems = volume.problems;
	*repairs = volume.repairs;

	/* A check that stopped half way writes nothing back. */
	if (error != 0)
		volume.repair = 0;
	close_error = fsck_volume_close(&volume);
	if (error == 0)
		error = close_error;

	/* Reports what stopped the check. */
	if (error != 0)
		return error;

	/* Succeeded: the counts are reported. */
	return 0;
}

/* Says how fsck is run. */
static void
fsck_usage(void)
{
	/* Succeeded: the usage is on the error stream. */
	fprintf(stderr, "usage: fsck [-n | -y] device\n");
}
