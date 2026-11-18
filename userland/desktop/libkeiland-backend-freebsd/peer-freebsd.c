/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The process at the other end of a connection on FreeBSD: LOCAL_PEERCRED's
 * record, whose cr_pid FreeBSD 13 and later fill (keiland-backend.h, WS191).
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ucred.h>
#include <sys/un.h>

/*
 * Gives the process at the other end of a connected local socket.
 */
int
kl_backend_peer_pid(
	int descriptor,
	pid_t *pid)
{
	struct xucred credentials;
	socklen_t length;
	int status;

	/* Reads what the kernel recorded of the peer when the connection was made. */
	length = sizeof(credentials);
	status = getsockopt(descriptor, SOL_LOCAL, LOCAL_PEERCRED, &credentials, &length);
	if (status != 0)
		return errno;

	/* A record of another version is not one this code can read. */
	if (credentials.cr_version != XUCRED_VERSION)
		return EINVAL;

	/* Gives the caller the peer's process. */
	*pid = credentials.cr_pid;

	/* Succeeded: *pid is the peer's process. */
	return 0;
}
