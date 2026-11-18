/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The password check that login and sessiond share.
 *
 * An account logs in when passwd and shadow both know it, its shadow entry
 * is not locked ("!" or "*" at the front), and the password matches: an
 * empty shadow password takes only an empty password, any other takes the
 * password whose crypt() is the stored hash.  The password is overwritten
 * before the check returns, whatever it answers.
 */

#include "verify.h"

#include <crypt.h>
#include <shadow.h>
#include <string.h>

static int verify_hash(const char *stored, char *password);

/*
 * Checks a user's password against the shadow file.
 *
 * Returns 0 and fills account (its strings in buffer) when the account may
 * log in with the password, -1 when it may not.  The password is erased in
 * either case.
 */
int
login_verify(
	const char *name,
	char *password,
	struct passwd *account,
	char *buffer,
	size_t size)
{
	struct passwd *found;
	struct spwd shadow;
	struct spwd *shadow_found;
	char shadow_buffer[LOGIN_VERIFY_BUFFER];
	int error;
	int matched;

	/* Looks the account up in passwd. */
	found = NULL;
	error = getpwnam_r(name, account, buffer, size, &found);
	if (error != 0 || found == NULL) {
		memset(password, 0, strlen(password));
		return -1;
	}

	/* Looks its password up in shadow. */
	shadow_found = NULL;
	error = getspnam_r(name, &shadow, shadow_buffer, sizeof(shadow_buffer), &shadow_found);
	if (error != 0 || shadow_found == NULL) {
		memset(password, 0, strlen(password));
		return -1;
	}

	/* Compares the password with the stored one, which erases it. */
	matched = verify_hash(shadow.sp_pwdp, password);
	memset(shadow_buffer, 0, sizeof(shadow_buffer));
	if (!matched)
		return -1;

	/* Succeeded: the account may log in. */
	return 0;
}

/* Compares a password with a stored shadow hash and erases the password. */
static int
verify_hash(
	const char *stored,
	char *password)
{
	char *hash;
	int empty;
	int equal;

	/* A locked account ("!" or "*" in front of its hash) takes no password. */
	if (stored[0] == '!' || stored[0] == '*') {
		memset(password, 0, strlen(password));
		return 0;
	}

	/* An empty stored password takes only an empty password. */
	if (stored[0] == '\0') {
		empty = 0;
		if (password[0] == '\0')
			empty = 1;
		return empty;
	}

	/* Hashes the password with the stored salt, then erases it. */
	hash = crypt(password, stored);
	memset(password, 0, strlen(password));
	if (hash == NULL)
		return 0;

	/* The password matches when its hash is the stored one. */
	equal = strcmp(hash, stored);
	if (equal != 0)
		return 0;

	/* Succeeded: the password is the account's. */
	return 1;
}
