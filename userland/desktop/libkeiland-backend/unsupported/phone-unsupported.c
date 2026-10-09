/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The phone where no backend is written (ws197-p004a, the user's decision
 * Q10: Linux's and FreeBSD's Keiland have no phone link): there is no
 * phone, nothing comes, and every request is refused with ENOTSUP.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The following of nothing: a record so that the callers need no special case. */
struct kl_backend_phone {
	unsigned refreshed;
};

/*
 * Starts following nothing.  Returns NULL only without memory.
 */
struct kl_backend_phone *
kl_backend_phone_open(
	void)
{
	struct kl_backend_phone *phone;

	/* The record. */
	phone = calloc(1, sizeof(*phone));
	if (phone == NULL)
		return NULL;

	/* Succeeded: nothing to follow. */
	return phone;
}

/*
 * Stops following nothing.
 */
void
kl_backend_phone_close(
	struct kl_backend_phone *phone)
{
	/* The record. */
	free(phone);
}

/*
 * Reads nothing: nothing changes.
 */
int
kl_backend_phone_update(
	struct kl_backend_phone *phone,
	unsigned *changed)
{
	/* A record and somewhere to say what changed. */
	if (phone == NULL || changed == NULL)
		return EINVAL;

	/* Succeeded: no change. */
	*changed = 0U;
	return 0;
}

/*
 * Copies the state: no phone link on this system.
 */
void
kl_backend_phone_get_state(
	const struct kl_backend_phone *phone,
	struct kl_backend_phone_state *state)
{
	(void)phone;

	/* Nothing, and why. */
	memset(state, 0, sizeof(*state));
	(void)snprintf(state->why, sizeof(state->why), "%s", "unsupported");
}

/*
 * Refuses a page.
 */
int
kl_backend_phone_page(
	struct kl_backend_phone *phone,
	unsigned what,
	int64_t since,
	unsigned limit,
	const char *cursor,
	unsigned count,
	uint32_t *id)
{
	(void)phone;
	(void)what;
	(void)since;
	(void)limit;
	(void)cursor;
	(void)count;
	(void)id;

	/* Not here. */
	return ENOTSUP;
}

/*
 * Refuses a message marked read.
 */
int
kl_backend_phone_read(
	struct kl_backend_phone *phone,
	const char *handle,
	uint32_t *id)
{
	(void)phone;
	(void)handle;
	(void)id;

	/* Not here. */
	return ENOTSUP;
}

/*
 * Refuses a text sent.
 */
int
kl_backend_phone_send(
	struct kl_backend_phone *phone,
	const char *to,
	const uint8_t *text,
	size_t length,
	uint32_t *id)
{
	(void)phone;
	(void)to;
	(void)text;
	(void)length;
	(void)id;

	/* Not here. */
	return ENOTSUP;
}

/*
 * Refuses the phone's switch.
 */
int
kl_backend_phone_link_set(
	struct kl_backend_phone *phone,
	const char *address,
	unsigned on,
	unsigned profiles,
	uint32_t *id)
{
	(void)phone;
	(void)address;
	(void)on;
	(void)profiles;
	(void)id;

	/* Not here. */
	return ENOTSUP;
}

/*
 * Has no item.
 */
int
kl_backend_phone_take_item(
	struct kl_backend_phone *phone,
	struct kl_backend_phone_item *item)
{
	(void)phone;
	(void)item;

	/* None. */
	return 0;
}

/*
 * Has no result.
 */
int
kl_backend_phone_take_result(
	struct kl_backend_phone *phone,
	struct kl_backend_phone_result *result)
{
	(void)phone;
	(void)result;

	/* None. */
	return 0;
}

/*
 * Has no state of a text sent.
 */
int
kl_backend_phone_take_sent(
	struct kl_backend_phone *phone,
	uint32_t *sent_request,
	unsigned *state)
{
	(void)phone;
	(void)sent_request;
	(void)state;

	/* None. */
	return 0;
}

/*
 * Has nothing to read again.
 */
void
kl_backend_phone_refresh(
	struct kl_backend_phone *phone)
{
	/* Counted, for nothing. */
	if (phone != NULL)
		phone->refreshed++;
}
