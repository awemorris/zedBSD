/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The peer of a connection where the system has getpeereid (zedBSD and
 * FreeBSD; keiland-backend.h, WS135).
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Gives the user at the other end of a connected local socket.
 */
int
kl_backend_peer_uid(
	int descriptor,
	uid_t *uid)
{
	gid_t group;
	int status;

	/* The peer's user and group, as the system recorded them at the connection. */
	status = getpeereid(descriptor, uid, &group);
	if (status != 0)
		return errno;

	/* Succeeded: the peer's user. */
	return 0;
}
