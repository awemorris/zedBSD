/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the zedBSD versiontest userland behavior.
 */

__asm__(".symver versioned_value_v1,versioned_value@ZEDBSD_1.0");
__asm__(".symver versioned_value_v2,versioned_value@@ZEDBSD_2.0");

/*
 * Implements the versioned value v1 operation.
 */
int
versioned_value_v1(
	void)
{
	/* Returns the computed result. */
	return 41;
}

/*
 * Implements the versioned value v2 operation.
 */
int
versioned_value_v2(
	void)
{
	/* Returns the computed result. */
	return 42;
}
