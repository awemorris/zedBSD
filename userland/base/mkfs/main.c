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

	/* Require an explicit format and an optional supported profile. */
	if ((argc != 4 && argc != 5) || strcmp(argv[1], "-t") != 0 ||
	    strcmp(argv[2], "ufs") != 0) {
		fprintf(stderr, "usage: mkfs -t ufs [--profile=journal-snapshot] FILE\n");
		return 2;
	}
	selected = &ops;
	path = argv[3];

	/* Bind profile callbacks before entering the descriptor-owned transaction. */
	if (argc == 5) {
		if (strcmp(argv[3], "--profile=journal-snapshot") != 0) {
			fprintf(stderr, "mkfs: unsupported profile\n");
			return 2;
		}
		selected = &feature_ops;
		path = argv[4];
	}

	/* Performs generation and durable verification under one reservation. */
	error = format_file_run(path, selected, &size);
	if (error != 0) {
		fprintf(stderr, "mkfs: %s: %s\n", path, strerror(error));
		return 1;
	}

	/* Reports success only after flush, reopen, validation and close. */
	printf("mkfs: %s: ufs initialized (%" PRIu64 " bytes)\n", path, size);
	return 0;
}
