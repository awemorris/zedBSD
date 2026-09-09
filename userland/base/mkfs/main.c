/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Initializes the maintained UFS format in an existing regular file.
 */

#include "ufs-format.h"
#include "block-command.h"
#include "userland/base/common/format-file.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * Runs the explicit UFS regular-file formatter.
 */
int
main(
	int argc,
	char **argv)
{
	static const struct format_file_ops ops = {
		ufs_format_validate_size, ufs_format_write, ufs_format_verify
	};
	static const struct format_file_ops feature_ops = {
		ufs_format_validate_size, ufs_format_feature_write, ufs_format_feature_verify
	};
	const struct format_file_ops *selected;
	const char *path;
	uint64_t size;
	int error;
	int index;
	int pristine;
	int profile;
	int (*verify)(int, uint64_t);

	/* Capacity inspection never falls through to a device or file mutation. */
	for (index = 1; index < argc; index++) {
		if (strcmp(argv[index], "--check-size") == 0)
			return mkfs_capacity_command(argc, argv);
	}

	/* Require an explicit format and an optional supported profile. */
	if (argc >= 3 && strcmp(argv[1], "-t") == 0 && strcmp(argv[2], "fat32") == 0)
		return mkfs_block_command(argc, argv);
	if (argc >= 4 && strcmp(argv[1], "-t") == 0 && strcmp(argv[2], "ufs") == 0 &&
	    strcmp(argv[3], "--profile=native") == 0)
		return mkfs_block_command(argc, argv);
	if (argc < 4 || argc > 6 || strcmp(argv[1], "-t") != 0 ||
	    strcmp(argv[2], "ufs") != 0) {
		fprintf(stderr, "usage: mkfs -t ufs [--profile=journal-snapshot] [--verify-pristine] FILE\n");
		return 2;
	}

	/* Parses each supported option once, before the single final operand. */
	pristine = 0;
	profile = 0;
	for (index = 3; index < argc - 1; index++) {
		if (strcmp(argv[index], "--verify-pristine") == 0 && !pristine)
			pristine = 1;
		else if (strcmp(argv[index], "--profile=journal-snapshot") == 0 && !profile)
			profile = 1;
		else {
			fprintf(stderr, "mkfs: unsupported or repeated option\n");
			return 2;
		}
	}

	/* Refuses an option where the explicit filename is required. */
	path = argv[argc - 1];
	if (path[0] == '-') {
		fprintf(stderr, "mkfs: expected FILE\n");
		return 2;
	}

	/* Binds the same profile for generation and exact pristine comparison. */
	selected = &ops;
	verify = ufs_format_pristine;
	if (profile) {
		selected = &feature_ops;
		verify = ufs_format_feature_pristine;
	}

	/* Selects read-only observation or the existing reserved mutation path. */
	if (pristine)
		error = format_file_verify(path, selected->validate_size, verify, &size);
	else
		error = format_file_run(path, selected, &size);

	/* Reports failures without claiming a usable initial image. */
	if (error != 0) {
		fprintf(stderr, "mkfs: %s: %s\n", path, strerror(error));
		return 1;
	}

	/* Distinguishes successful observation from successful initialization. */
	if (pristine)
		printf("mkfs: %s: ufs pristine (%" PRIu64 " bytes)\n", path, size);
	else
		printf("mkfs: %s: ufs initialized (%" PRIu64 " bytes)\n", path, size);
	return 0;
}
