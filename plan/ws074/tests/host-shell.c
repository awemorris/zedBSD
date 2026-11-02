/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host build's stand-in for the zdesktop window: the host tests use the
 * headless modes only, and the engine gets the POSIX threads (the window's
 * build gives it libkeiland's).
 */

#include "shell/shell.h"
#include "userland/tests/browser-threads/threads-posix.h"

#include <stdio.h>

/*
 * Reports the threads the engine is given: the POSIX threads.
 */
const struct browser_threads *
shell_threads(void)
{
	/* The table over the POSIX threads. */
	return browser_threads_posix();
}

/*
 * Refuses the window mode on the host.
 */
int
shell_run(
	const struct shell_options *options)
{
	(void)options;

	/* Says why nothing opened. */
	fprintf(stderr, "browser: the host build has no window mode\n");

	/* Reports the refusal. */
	return 1;
}
