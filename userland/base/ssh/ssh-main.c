/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ssh program: connects to a server, checks its host key against
 * known_hosts, logs in with a key or a password, and runs a shell or a
 * command on a terminal or on the standard streams.
 *
 * Skeleton: reports that it is not implemented yet.
 */

#include <stdio.h>

#include "ssh-transport.h"

/*
 * Runs the ssh program.
 */
int
main(
	int argc,
	char **argv)
{
	/* Marks the arguments used until the program is implemented. */
	(void)argc;
	(void)argv;

	/* Reports that the program is not implemented yet. */
	fputs("ssh: not implemented\n", stderr);

	/* Reports failure. */
	return 1;
}
