/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the zedBSD rpathtest userland behavior.
 */

extern int rpath_dependency_value(void);

/*
 * Implements the rpathtest value operation.
 */
int
rpathtest_value(
	void)
{
	int function_result;

	/* Obtains the rpath dependency value result. */
	function_result = rpath_dependency_value();

	/* Returns the computed result. */
	return function_result;
}
