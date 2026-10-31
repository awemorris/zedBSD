/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The session process of sshd: the connection protocol as the user.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "sshd.h"

/*
 * Runs the session process for the authenticated user: imports the
 * transport from size bytes at state, then the connection protocol on
 * client.
 */
void
sshd_session_run(
	const struct sshd_context *context,
	int client,
	int monitor,
	const char *user,
	const uint8_t *state,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)context;
	(void)client;
	(void)monitor;
	(void)user;
	(void)state;
	(void)size;
}

