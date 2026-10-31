/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pre-authentication process of sshd: transport and user authentication without privileges.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "sshd.h"

/*
 * Runs the pre-authentication process: the transport and the user
 * authentication on client, asking the monitor on monitor.
 */
void
sshd_preauth_run(
	const struct sshd_context *context,
	int client,
	int monitor)
{
	/* Marks the arguments used until the function is implemented. */
	(void)context;
	(void)client;
	(void)monitor;
}

