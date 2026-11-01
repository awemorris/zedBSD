/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The entry of the program on a POSIX system: the command line goes to
 * browser_main (main.c).  The storage backend is not wrapped here
 * (shell_storage_wrap).
 */

#include "browser-main.h"
#include "shell/shell.h"

/*
 * Leaves the storage backend as it is (shell.h).
 */
int
shell_storage_wrap(
	const struct browser_storage_calls **calls,
	void **context)
{
	(void)calls;
	(void)context;

	/* Succeeded: nothing to wrap. */
	return 0;
}

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
