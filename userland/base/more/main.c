/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD more userland command.
 */

#include "userland/base/common/pager.h"

/*
 * Runs the more command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;

	/* Obtains the pager main result. */
	function_result = pager_main(PAGER_MORE, argc, argv);

	/* Returns the computed result. */
	return function_result;
}
