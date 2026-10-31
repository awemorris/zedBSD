/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The monitor of one connection: the privileged half of sshd.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "sshd.h"

/*
 * Runs the monitor of one connection on descriptor client until the
 * connection ends.
 */
int
sshd_monitor_run(
	struct sshd_context *context,
	int client)
{
	/* Marks the arguments used until the function is implemented. */
	(void)context;
	(void)client;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

