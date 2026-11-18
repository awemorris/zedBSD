/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The process at the other end of a connection on zedBSD: SO_PEERCRED's
 * record (keiland-backend.h, WS191).
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>

/*
 * Gives the process at the other end of a connected local socket.
 */
int
kl_backend_peer_pid(
	int descriptor,
	pid_t *pid)
{
	struct kern_peercred credentials;
	socklen_t length;
	int status;

	/* Reads what the kernel recorded of the peer when the connection was made. */
	length = sizeof(credentials);
	status = getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &length);
	if (status != 0)
		return errno;

	/* An answer of another size is not the record asked for. */
	if (length != sizeof(credentials))
		return EINVAL;

	/* Gives the caller the peer's process. */
	*pid = (pid_t)credentials.pid;

	/* Succeeded: *pid is the peer's process. */
	return 0;
}
