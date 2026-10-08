/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Bluetooth where no backend is written yet (ws143-p006: Linux until its
 * BlueZ backend, ws143-p007, and FreeBSD): the state is unreachable, there
 * is no device, and every request is refused with ENOTSUP.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The following of nothing: a record so that the callers need no special case. */
struct kl_backend_bluetooth {
	unsigned watching;
};

/*
 * Starts following nothing.  Returns NULL only without memory.
 */
struct kl_backend_bluetooth *
kl_backend_bluetooth_open(
	void)
{
	struct kl_backend_bluetooth *bluetooth;

	/* The record. */
	bluetooth = calloc(1, sizeof(*bluetooth));
	if (bluetooth == NULL)
		return NULL;

	/* Succeeded: unreachable from the start. */
	return bluetooth;
}

/*
 * Stops following nothing.
 */
void
kl_backend_bluetooth_close(
	struct kl_backend_bluetooth *bluetooth)
{
	/* The record. */
	free(bluetooth);
}

/*
 * Reads nothing: nothing changes.
 */
int
kl_backend_bluetooth_update(
	struct kl_backend_bluetooth *bluetooth,
	unsigned *changed)
{
	/* A record and somewhere to say what changed. */
	if (bluetooth == NULL || changed == NULL)
		return EINVAL;

	/* Succeeded: no change. */
	*changed = 0U;
	return 0;
}

/*
 * Copies the state: unreachable.
 */
void
kl_backend_bluetooth_get_state(
	const struct kl_backend_bluetooth *bluetooth,
	struct kl_backend_bluetooth_state *state)
{
	/* No service to ask. */
	(void)bluetooth;
	memset(state, 0, sizeof(*state));
	state->state = KL_BACKEND_BT_ABSENT;
}

/*
 * Copies no device.
 */
size_t
kl_backend_bluetooth_get_devices(
	const struct kl_backend_bluetooth *bluetooth,
	struct kl_backend_bluetooth_device *devices,
	size_t capacity)
{
	/* None. */
	(void)bluetooth;
	(void)devices;
	(void)capacity;
	return 0U;
}

/*
 * Watches nothing.
 */
void
kl_backend_bluetooth_set_watching(
	struct kl_backend_bluetooth *bluetooth,
	unsigned on)
{
	/* Kept, for nothing. */
	if (bluetooth != NULL)
		bluetooth->watching = on;
}

/*
 * Scans nothing.
 */
void
kl_backend_bluetooth_set_scanning(
	struct kl_backend_bluetooth *bluetooth,
	unsigned on)
{
	/* Nothing to do. */
	(void)bluetooth;
	(void)on;
}

/*
 * Refuses every request.
 */
int
kl_backend_bluetooth_request(
	struct kl_backend_bluetooth *bluetooth,
	unsigned request,
	const char *address,
	unsigned type,
	uint32_t *id)
{
	/* Not here. */
	(void)bluetooth;
	(void)request;
	(void)address;
	(void)type;
	(void)id;
	return ENOTSUP;
}

/*
 * Has no answer.
 */
int
kl_backend_bluetooth_take_result(
	struct kl_backend_bluetooth *bluetooth,
	uint32_t *id,
	int *error,
	char *reason,
	size_t size)
{
	/* None. */
	(void)bluetooth;
	(void)id;
	(void)error;
	(void)reason;
	(void)size;
	return 0;
}

/*
 * Has no question.
 */
int
kl_backend_bluetooth_take_question(
	struct kl_backend_bluetooth *bluetooth,
	struct kl_backend_bluetooth_question *question)
{
	/* None. */
	(void)bluetooth;
	(void)question;
	return 0;
}

/*
 * Has no question to answer.
 */
int
kl_backend_bluetooth_answer(
	struct kl_backend_bluetooth *bluetooth,
	uint32_t id,
	unsigned yes)
{
	/* None asked. */
	(void)bluetooth;
	(void)id;
	(void)yes;
	return ENOENT;
}

/*
 * Has no pairing to give up.
 */
int
kl_backend_bluetooth_cancel(
	struct kl_backend_bluetooth *bluetooth)
{
	/* None. */
	(void)bluetooth;
	return ENOENT;
}
