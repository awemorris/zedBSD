/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The account where the backend does not offer it yet (ws160-p002): Linux
 * and FreeBSD.  A password change and the administration of the accounts
 * (ws089-p026) answer ENOTSUP.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stddef.h>

/* Refuses a password change. */
int
kl_backend_account_set_password(
	const char *current,
	const char *fresh)
{
	/* Nothing is offered here. */
	(void)current;
	(void)fresh;
	return ENOTSUP;
}

/* Tells that the accounts are not administered here. */
int
kl_backend_account_can_administer(void)
{
	/* Not offered. */
	return 0;
}

/* Refuses an administration's change. */
int
kl_backend_account_administer(
	const char *password,
	const char *operation,
	char *reason,
	size_t size)
{
	/* Nothing is offered here; no word of a refusal. */
	(void)password;
	(void)operation;
	if (reason != NULL && size != 0U)
		reason[0] = '\0';
	return ENOTSUP;
}
