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
	int pristine;
	const char *path;

	/* Accepts one filename, optionally preceded by read-only verification. */
	pristine = 0;
	if (argc == 3 && strcmp(argv[1], "--verify-pristine") == 0)
		pristine = 1;

	/* Rejects missing operands and unknown options before any file access. */
	if ((argc != 2 && !pristine) || argv[argc - 1][0] == '-') {
		fprintf(stderr, "usage: mkswap [--verify-pristine] FILE\n");
		return 2;
	}

	/* Keeps read-only verification separate from reserved initialization. */
	path = argv[argc - 1];
	if (pristine)
		error = format_file_verify(path, swap_format_validate_size, swap_format_pristine, &size);
	else
		error = format_file_run(path, &ops, &size);

	/* Reports the earliest operation failure. */
	if (error != 0) {
		fprintf(stderr, "mkswap: %s: %s\n", path, strerror(error));
		return 1;
	}

	/* Reports whether bytes were checked or initialized, without activation. */
	if (pristine)
		printf("mkswap: %s: ZEDSWAP2 pristine (%" PRIu64 " slots)\n",
			path, size / 4096 - 1);
	else
		printf("mkswap: %s: ZEDSWAP2 initialized (%" PRIu64 " slots)\n",
			path, size / 4096 - 1);
	return 0;
}
