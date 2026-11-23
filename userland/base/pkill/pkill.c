/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pkill program: signals the processes its options and patterns
 * select (match.c).
 */

#include "match.h"

/*
 * Runs the pkill command.
 */
int
main(
	int argc,
	char **argv)
{
	int status;

	/* Selects the processes and acts on them as pkill. */
	status = pkill_run(argc, argv, PKILL_PROGRAM_PKILL);

	/* Reports that no process was selected, or why the command failed. */
	if (status != PKILL_STATUS_MATCH)
		return status;

	/* Succeeded: the selected processes are signalled. */
	return PKILL_STATUS_MATCH;
}
