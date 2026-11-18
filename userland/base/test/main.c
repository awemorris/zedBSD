/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Evaluates an expression (POSIX XCU test): the shell's test builtin as a command of its own.
 * The builtin and the command are the same source
 * (userland/base/sh/test.c), so they behave alike.
 */

#include "userland/base/sh/shell.h"

#include <stdio.h>

/*
 * Runs the command.
 */
int
main(
	int argc,
	char **argv)
{
	int status;

	/* The builtin, then its output flushed. */
	status = sh_builtin_test(argc, argv);
	fflush(stdout);

	/* Succeeded: the builtin's status. */
	return status;
}
