/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Sharing where the backend does not offer it (ws089-p025: Linux and
 * FreeBSD for the beta, whose own tools turn sshd on and off): no Remote
 * Login to change, and a state that says it is not available.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <string.h>

/*
 * Refuses every request.
 */
int
kl_backend_sharing_request(
	struct kl_backend *backend,
	unsigned action)
{
	(void)backend;
	(void)action;

	/* Not supported here. */
	return ENOTSUP;
}

/*
 * Gives a state that says Remote Login is not available.
 */
void
kl_backend_sharing_get(
	const struct kl_backend *backend,
	struct kl_backend_sharing *state)
{
	(void)backend;

	/* Nothing known, nothing available. */
	memset(state, 0, sizeof(*state));
}
