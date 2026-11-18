/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The seat where the backend has none (ws131-p006): zedBSD, whose
 * compositor takes the display and the input devices through its own
 * kernel interfaces.
 * Opening answers ENOTSUP, nothing is paused, and the poll has nothing.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <stddef.h>

/*
 * Has no seat to take.
 */
int
kl_backend_seat_open(
	struct kl_backend *backend)
{
	/* The compositor uses its own interfaces. */
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/*
 * Has nothing to return.
 */
void
kl_backend_seat_close(
	struct kl_backend *backend)
{
	/* Nothing was taken. */
	(void)backend;
}

/*
 * Has no primary node.
 */
int
kl_backend_seat_primary_fd(
	const struct kl_backend *backend)
{
	/* Nothing was taken. */
	(void)backend;
	return -1;
}

/*
 * Has no primary node's path.
 */
const char *
kl_backend_seat_primary_path(
	const struct kl_backend *backend)
{
	/* Nothing was taken. */
	(void)backend;
	return "";
}

/*
 * Is never paused.
 */
int
kl_backend_seat_paused(
	const struct kl_backend *backend)
{
	/* No seat takes anything away. */
	(void)backend;
	return 0;
}

/*
 * Opens no device.
 */
int
kl_backend_seat_device_open(
	struct kl_backend *backend,
	const char *path)
{
	/* The compositor uses its own interfaces. */
	(void)backend;
	(void)path;
	errno = ENOTSUP;
	return -1;
}

/*
 * Has no device to return.
 */
void
kl_backend_seat_device_close(
	struct kl_backend *backend,
	int descriptor)
{
	/* Nothing was opened here. */
	(void)backend;
	(void)descriptor;
}

/*
 * Keeps no device.
 */
int
kl_backend_seat_device_revoked(
	struct kl_backend *backend,
	int descriptor)
{
	/* The compositor closes it as usual. */
	(void)backend;
	(void)descriptor;
	return 0;
}

/*
 * Waits for no descriptor.
 */
void
kl_backend_seat_device_let_go(
	struct kl_backend *backend,
	int descriptor)
{
	/* No callback asked for an answer. */
	(void)backend;
	(void)descriptor;
}

/*
 * Has no pause to finish.
 */
void
kl_backend_seat_paused_done(
	struct kl_backend *backend)
{
	/* No seat pauses here. */
	(void)backend;
}

/*
 * Polls nothing.
 */
size_t
kl_backend_seat_poll_count(
	const struct kl_backend *backend)
{
	/* No service to hear. */
	(void)backend;
	return 0;
}

/*
 * Fills nothing.
 */
void
kl_backend_seat_poll_fill(
	struct kl_backend *backend,
	struct pollfd *descriptors)
{
	/* No service to hear. */
	(void)backend;
	(void)descriptors;
}

/*
 * Handles nothing.
 */
void
kl_backend_seat_poll_done(
	struct kl_backend *backend,
	const struct pollfd *descriptors)
{
	/* No service to hear. */
	(void)backend;
	(void)descriptors;
}
