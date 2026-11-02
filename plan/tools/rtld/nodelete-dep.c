/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The dependency of libnodelete-keep.so (nodelete-lib.c) in the dynamic
 * loader's NODELETE test (rtld-nodelete.c).  It must stay mapped as long
 * as the library that needs it, and be finalized after it, at exit.
 */

#include <unistd.h>

int nodelete_dep_value(void);

static void dep_fini(void) __attribute__((destructor));

/*
 * Returns the dependency's part of libnodelete-keep.so's value.
 */
int
nodelete_dep_value(
	void)
{
	/* The value the test expects of the dependency. */
	return 40;
}

/* The dependency's destructor, which the loader runs when it finalizes it. */
static void
dep_fini(
	void)
{
	static const char line[] = "RTLD-NODELETE fini dep\n";

	/* Says when the loader finalized the dependency. */
	(void)write(1, line, sizeof(line) - 1U);
}
