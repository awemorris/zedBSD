/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Initializes a pre-sized regular file for ZEDSWAP2 activation.
 */

#include "swap-format.h"
#include "userland/base/common/format-file.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * Runs the regular-file swap initializer without activating it.
 */
int
main(
	int argc,
	char **argv)
{
	static const struct format_file_ops ops = {
		swap_format_validate_size, swap_format_write, swap_format_verify
	};
	uint64_t size;
	int error;

	/* Requires exactly one explicit pre-sized file. */
	if (argc != 2 || argv[1][0] == '-') {
		fprintf(stderr, "usage: mkswap FILE\n");
		return 2;
	}

	/* Generates, flushes and validates while holding exclusive mutation. */
	error = format_file_run(argv[1], &ops, &size);
	if (error != 0) {
		fprintf(stderr, "mkswap: %s: %s\n", argv[1], strerror(error));
		return 1;
	}

	/* Reports the checked slot count after all completion gates pass. */
	printf("mkswap: %s: ZEDSWAP2 initialized (%" PRIu64 " slots)\n",
		argv[1], size / 4096 - 1);
	return 0;
}
