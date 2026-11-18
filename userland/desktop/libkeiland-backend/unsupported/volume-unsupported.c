/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The removable media where the backend does not offer them yet
 * (ws132-p004): Linux and FreeBSD.  The list is always empty, and a mount
 * or an eject answers ENOTSUP.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stdlib.h>

/* The record of an empty following; it holds nothing. */
struct kl_backend_volumes {
	unsigned unused;
};

/*
 * Starts following nothing.
 */
struct kl_backend_volumes *
kl_backend_volumes_open(
	void)
{
	/* A record, so that the caller can tell an empty list from no memory. */
	return calloc(1U, sizeof(struct kl_backend_volumes));
}

/*
 * Stops following nothing.
 */
void
kl_backend_volumes_close(
	struct kl_backend_volumes *volumes)
{
	/* The record. */
	free(volumes);
}

/*
 * Nothing ever changes.
 */
int
kl_backend_volumes_update(
	struct kl_backend_volumes *volumes,
	unsigned *changed)
{
	/* A record and somewhere to say what changed. */
	if (volumes == NULL || changed == NULL)
		return EINVAL;

	/* Nothing changed. */
	*changed = 0U;
	return 0;
}

/*
 * No volume.
 */
size_t
kl_backend_volumes_get(
	const struct kl_backend_volumes *volumes,
	struct kl_backend_volume *list,
	size_t capacity)
{
	/* The list is empty. */
	(void)volumes;
	(void)list;
	(void)capacity;
	return 0U;
}

/*
 * No volume to mount.
 */
int
kl_backend_volumes_mount(
	struct kl_backend_volumes *volumes,
	const char *id,
	uint32_t *request)
{
	/* Not offered here. */
	(void)volumes;
	(void)id;
	(void)request;
	return ENOTSUP;
}

/*
 * No volume to eject.
 */
int
kl_backend_volumes_eject(
	struct kl_backend_volumes *volumes,
	const char *id,
	uint32_t *request)
{
	/* Not offered here. */
	(void)volumes;
	(void)id;
	(void)request;
	return ENOTSUP;
}

/*
 * No answer ever waits.
 */
int
kl_backend_volumes_take_result(
	struct kl_backend_volumes *volumes,
	uint32_t *request,
	int *error,
	char *user,
	size_t size)
{
	/* None. */
	(void)volumes;
	(void)request;
	(void)error;
	(void)user;
	(void)size;
	return 0;
}
