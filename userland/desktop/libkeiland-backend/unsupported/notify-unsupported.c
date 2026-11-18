/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bus's notifications where the backend does not serve them
 * (ws156-p006): zedBSD has no D-Bus, and FreeBSD's backend does not serve
 * its session bus yet (plan/ws156/phase001/phase.md section 11.1).
 * Keiland's programs post through the compositor's own protocol there.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <stddef.h>

/* Serves nothing: the system has no such bus. */
int
kl_backend_notify_open(
	struct kl_backend *backend,
	const struct kl_backend_notify_callbacks *callbacks,
	void *data)
{
	/* Nothing to serve. */
	(void)backend;
	(void)callbacks;
	(void)data;
	return ENOTSUP;
}

/* Has no sender to tell. */
void
kl_backend_notify_closed(
	struct kl_backend *backend,
	uint32_t id,
	unsigned reason)
{
	/* No notification came from a bus. */
	(void)backend;
	(void)id;
	(void)reason;
}

/* Has no sender to tell. */
void
kl_backend_notify_activated(
	struct kl_backend *backend,
	uint32_t id)
{
	/* No notification came from a bus. */
	(void)backend;
	(void)id;
}

/* Has nothing to stop. */
void
kl_backend_notify_close(
	struct kl_backend *backend)
{
	/* Nothing was served. */
	(void)backend;
}

/* Polls nothing. */
size_t
kl_backend_notify_poll_count(
	const struct kl_backend *backend)
{
	/* No descriptor. */
	(void)backend;
	return 0;
}

/* Fills nothing. */
void
kl_backend_notify_poll_fill(
	struct kl_backend *backend,
	struct pollfd *descriptors)
{
	/* No descriptor. */
	(void)backend;
	(void)descriptors;
}

/* Is told nothing. */
void
kl_backend_notify_poll_done(
	struct kl_backend *backend,
	const struct pollfd *descriptors)
{
	/* No descriptor. */
	(void)backend;
	(void)descriptors;
}
