/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system's events where the backend does not offer them (ws132-p003):
 * Linux and FreeBSD until the beta (2026-10-05 user: "libkeiland-backendの
 * Linux、FreeBSDの実装は、ベータ1までにはやらなくていいです").  Nothing is
 * heard, and the compositor finds input devices by its own scans.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <stddef.h>

/* Hears nothing: the descriptor stays -1. */
void
kl_backend_events_open(
	struct kl_backend *backend)
{
	/* Nothing to open. */
	(void)backend;
}

/* Has nothing to close. */
void
kl_backend_events_close(
	struct kl_backend *backend)
{
	/* Nothing was opened. */
	(void)backend;
}

/* Polls nothing. */
size_t
kl_backend_events_poll_count(
	const struct kl_backend *backend)
{
	/* No descriptor. */
	(void)backend;
	return 0;
}

/* Fills nothing. */
void
kl_backend_events_poll_fill(
	struct kl_backend *backend,
	struct pollfd *descriptors)
{
	/* No descriptor. */
	(void)backend;
	(void)descriptors;
}

/* Is told nothing. */
void
kl_backend_events_poll_done(
	struct kl_backend *backend,
	const struct pollfd *descriptors)
{
	/* No descriptor. */
	(void)backend;
	(void)descriptors;
}
