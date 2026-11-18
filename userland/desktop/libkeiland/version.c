/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The version of the desktop's system library.
 */

#include <keiland/keiland.h>

/*
 * Reports the interface version of this library.
 */
unsigned
kl_version(
	void)
{
	/* Returns the version this library was built as. */
	return KL_VERSION;
}
