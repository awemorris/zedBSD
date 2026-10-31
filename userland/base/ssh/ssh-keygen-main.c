/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ssh-keygen program: makes ssh-ed25519 keys, prints fingerprints and
 * public keys, and makes the host key of a machine.
 *
 * Skeleton: reports that it is not implemented yet.
 */

#include <stdio.h>

#include "ssh-key.h"

/*
 * Runs the ssh-keygen program.
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
	fputs("ssh-keygen: not implemented\n", stderr);

	/* Reports failure. */
	return 1;
}
