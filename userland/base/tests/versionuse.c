/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the zedBSD versionuse userland behavior.
 */

extern int versioned_value(void);
extern int versioned_value_v1_reference(void);

__asm__(".symver versioned_value_v1_reference,versioned_value@ZEDBSD_1.0");

/*
 * Implements the versionuse value operation.
 */
int
versionuse_value(
	void)
{
	int function_result;

	/* Computes the function result. */
	function_result = versioned_value() + versioned_value_v1_reference();

	/* Returns the computed result. */
	return function_result;
}
