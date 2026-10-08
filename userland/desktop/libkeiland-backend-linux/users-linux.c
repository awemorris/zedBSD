/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The people's accounts as Settings lists them (ws188-p002): the shared
 * machine/users.c reads them with this system's group of administrators.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

/*
 * Reads the people's accounts: Linux's administrators are sudo's members
 * (Debian, Ubuntu) or wheel's (Fedora, Arch).
 */
size_t
kl_backend_users_read(
	struct kl_backend_user *list,
	size_t capacity,
	unsigned *skipped)
{
	static const char *const admin_groups[] = { "sudo", "wheel", NULL };
	size_t count;

	/* The shared reading with this system's groups. */
	count = kl_backend_users_posix(list, capacity, skipped, admin_groups);

	/* Succeeded: the accounts read. */
	return count;
}
