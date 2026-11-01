/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The entry of the program on a POSIX system: the command line goes to
 * browser_main (main.c).
 */

#include "browser-main.h"

/*
 * Runs the program.
 */
int
main(
	int argc,
	char **argv)
{
	/* The program's own entry does everything. */
	return browser_main(argc, argv);
}
