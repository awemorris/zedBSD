/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Initializes the maintained UFS1 format in an existing regular file.
 */

#include "ufs1-format.h"
#include "userland/base/common/format-file.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * Runs the explicit UFS1 regular-file formatter.
 */
int
main(
	int argc,
	char **argv)
{
	static const struct format_file_ops ops = {
		ufs1_format_validate_size, ufs1_format_write, ufs1_format_verify
	};
	uint64_t size;
	int error;

	/* Requires exactly the supported type and one explicit existing file. */
	if (argc != 4 || strcmp(argv[1], "-t") != 0 ||
	    strcmp(argv[2], "ufs1") != 0) {
		fprintf(stderr, "usage: mkfs -t ufs1 FILE\n");
		return 2;
	}

	/* Performs generation and durable verification under one reservation. */
	error = format_file_run(argv[3], &ops, &size);
	if (error != 0) {
		fprintf(stderr, "mkfs: %s: %s\n", argv[3], strerror(error));
		return 1;
	}

	/* Reports success only after flush, reopen, validation and close. */
	printf("mkfs: %s: ufs1 initialized (%" PRIu64 " bytes)\n", argv[3], size);
	return 0;
}
