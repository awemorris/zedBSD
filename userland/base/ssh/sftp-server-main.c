/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sftp-server program: serves the SSH file transfer protocol version 3
 * on its standard input and output, as the logged-in user.
 *
 * Skeleton: reports that it is not implemented yet.
 */

#include <stdio.h>

#include "ssh-sftp.h"

/*
 * Runs the sftp-server program.
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
	fputs("sftp-server: not implemented\n", stderr);

	/* Reports failure. */
	return 1;
}
