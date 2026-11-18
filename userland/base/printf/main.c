/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Formats and writes its arguments (POSIX XCU printf): the shell's printf builtin as a command of its own.
 * The builtin and the command are the same source
 * (userland/base/sh/printf.c), so they behave alike.
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
	status = sh_builtin_printf(argc, argv);
	fflush(stdout);

	/* Succeeded: the builtin's status. */
	return status;
}
