/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * runas: runs a command as another user, for the tests (WS131 p011).
 *
 *	runas USER COMMAND [ARGUMENT...]
 *
 * Root starts it.  It takes the user's groups, group and user, sets HOME,
 * USER and LOGNAME to the user's and keeps the rest of the environment
 * (XDG_RUNTIME_DIR, for one), goes to the user's home and runs the command
 * there, found by PATH.  The tests start an application in a user's desktop
 * session from root's SSH; the compositor serves its system extension only
 * to its own user (plan/ws131/design.md section 4.1, D5), so an application
 * root started would find no network nor sound.
 */

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int become(const struct passwd *account);

/*
 * Runs the command as the user; returns 2 for a wrong command line, 1 when
 * the user cannot be become, 127 when the command cannot be run.
 */
int
main(
	int argc,
	char **argv)
{
	struct passwd *account;
	int error;

	/* The user and the command. */
	if (argc < 3) {
		fprintf(stderr, "usage: runas USER COMMAND [ARGUMENT...]\n");
		return 2;
	}

	/* The user's account. */
	account = getpwnam(argv[1]);
	if (account == NULL) {
		fprintf(stderr, "runas: %s: no such user\n", argv[1]);
		return 1;
	}

	/* The user's groups, group and user. */
	error = become(account);
	if (error != 0) {
		fprintf(stderr, "runas: %s: %s\n", argv[1], strerror(error));
		return 1;
	}

	/* The command in its place; returning means it could not be run. */
	(void)execvp(argv[2], &argv[2]);
	fprintf(stderr, "runas: %s: %s\n", argv[2], strerror(errno));
	return 127;
}

/* Takes an account's groups, group and user, its environment's names and its home; returns 0 or an errno. */
static int
become(
	const struct passwd *account)
{
	int status;

	/* The groups first, while still root. */
	status = initgroups(account->pw_name, account->pw_gid);
	if (status != 0)
		return errno;

	/* The group. */
	status = setgid(account->pw_gid);
	if (status != 0)
		return errno;

	/* The user, last: root's rights end here. */
	status = setuid(account->pw_uid);
	if (status != 0)
		return errno;

	/* The names a program looks for its user and home by. */
	(void)setenv("HOME", account->pw_dir, 1);
	(void)setenv("USER", account->pw_name, 1);
	(void)setenv("LOGNAME", account->pw_name, 1);

	/* The home, or the root when it is missing. */
	status = chdir(account->pw_dir);
	if (status != 0)
		(void)chdir("/");

	/* Succeeded: the process is the user's. */
	return 0;
}
