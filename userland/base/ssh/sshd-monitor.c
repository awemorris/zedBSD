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
	const struct sshd_context *context,
	struct ssh_key *host_key,
	int client,
	int started)
{
	/* Marks the arguments used until the function is implemented. */
	(void)context;
	(void)host_key;
	(void)client;
	(void)started;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

