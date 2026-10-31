/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The account checks of sshd: who may log in, with which key or password, and the login records.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "sshd.h"

/*
 * Checks that user may log in at all with method (SSH_AUTH_PUBLICKEY or
 * SSH_AUTH_PASSWORD of ssh-auth.h): the account exists (getpwnam_r into
 * account, its strings in buffer of size bytes), is not locked, has a
 * shell other than /sbin/nologin, and is root only as PermitRootLogin
 * allows for the method.
 */
int
sshd_account_allowed(
	const struct sshd_config *config,
	const char *user,
	int method,
	struct passwd *account,
	char *buffer,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)config;
	(void)user;
	(void)method;
	(void)account;
	(void)buffer;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}


/*
 * Reports whether public_key is in the account's authorized_keys file
 * (AuthorizedKeysFile, expanded): a child with the account's ids reads
 * the file as described above and passes at most SSH_HOSTFILE_MAX bytes
 * back over a pipe, and the monitor looks for the key with
 * ssh_authorized_keys_find.
 */
int
sshd_key_authorized(
	const struct sshd_config *config,
	const struct passwd *account,
	const uint8_t *public_key)
{
	/* Marks the arguments used until the function is implemented. */
	(void)config;
	(void)account;
	(void)public_key;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}


/*
 * Checks a password with the console login's rule (login_verify),
 * refusing an empty one unless PermitEmptyPasswords; for an account that
 * does not exist or is locked, crypt still runs on a dummy hash.
 */
int
sshd_password_ok(
	const struct sshd_config *config,
	const char *user,
	char *password)
{
	/* Marks the arguments used until the function is implemented. */
	(void)config;
	(void)user;
	(void)password;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}


/*
 * Writes the login record (login 1: USER_PROCESS) or the logout record
 * (login 0: DEAD_PROCESS) of a session on tty (a /dev/pts path) for pid,
 * with pututxline as login does.
 */
int
sshd_login_record(
	const char *user,
	const char *tty,
	pid_t pid,
	int login)
{
	/* Marks the arguments used until the function is implemented. */
	(void)user;
	(void)tty;
	(void)pid;
	(void)login;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
