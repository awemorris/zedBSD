/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Gives an account's master.passwd entry in the shape of a shadow entry (shadow-freebsd.h, ws172-p011). */
#include "shadow-freebsd.h"

#include <errno.h>
#include <pwd.h>
#include <string.h>

/* The seconds of a day, for the expiry FreeBSD keeps in seconds and shadow in days. */
#define SHADOW_DAY_SECONDS	86400L

/*
 * Looks an account up and gives its hash and expiry as a shadow entry,
 * its strings in buffer.  Returns 0 with *result the entry, or 0 with
 * *result NULL when there is no such account, or an errno value (ERANGE
 * when buffer is too small).  The caller is root, so the hash is the real
 * one; another caller gets FreeBSD's "*" in its place.
 */
int
getspnam_r(
	const char *name,
	struct spwd *entry,
	char *buffer,
	size_t size,
	struct spwd **result)
{
	struct passwd account;
	struct passwd *found;
	int error;

	/* Nothing yet. */
	*result = NULL;

	/* The account, its strings in the caller's buffer. */
	found = NULL;
	error = getpwnam_r(name, &account, buffer, size, &found);
	if (error != 0)
		return error;

	/* No such account. */
	if (found == NULL)
		return 0;

	/* The fields the callers read; the others are not known. */
	memset(entry, 0, sizeof(*entry));
	entry->sp_namp = account.pw_name;
	entry->sp_pwdp = account.pw_passwd;
	entry->sp_lstchg = -1;
	entry->sp_min = -1;
	entry->sp_max = -1;
	entry->sp_warn = -1;
	entry->sp_inact = -1;
	entry->sp_expire = -1;
	if (account.pw_expire > 0)
		entry->sp_expire = (long)(account.pw_expire / SHADOW_DAY_SECONDS);

	/* Succeeded: the entry. */
	*result = entry;
	return 0;
}
