/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD swapon userland command.
 */

#include "userland/base/swap-control/swap-command.h"

#include <zedbsd/system.h>

/*
 * Runs the swapon command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;

	/* Obtains the swap command main result. */
	function_result = swap_command_main("swapon", ZEDBSD_SYSTEM_SWAP_ADD, argc, argv);

	/* Returns the computed result. */
	return function_result;
}
