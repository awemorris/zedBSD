/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * usermod: changes a user, for root (ws196-p003;
 * docs/reference/user-administration.md).
 *
 *   usermod [-c COMMENT] [-g GROUP] [-G GROUP[,GROUP] [-a]] [-s SHELL] [-L | -U] NAME
 *
 * The options become account-admin's user-modify request (front.c).  -G
 * replaces the user's groups, or with -a adds to them; -L locks the
 * password and -U unlocks it.  The name, the user ID and the home are not
 * changed here.
 */

#include "userland/base/user-control/front.h"

#include <stdio.h>
#include <unistd.h>

/* The command's name, in its messages. */
#define USERMOD_PROGRAM		"usermod"

static void usermod_usage(void);

/*
 * Reads the options and has account-admin change the user.
 */
int
main(
	int argc,
	char **argv)
{
	struct user_front_request request;
	const char *comment;
	const char *primary;
	const char *groups;
	const char *shell;
	const char *password;
	const char *groups_key;
	const char *name;
	int append;
	int lock;
	int unlock;
	int changes;
	int option;
	int status;

	/* Only root changes a user. */
	status = user_front_start(USERMOD_PROGRAM);
	if (status != USER_FRONT_OK)
		return status;

	/* The options; a letter given again replaces its earlier value. */
	comment = NULL;
	primary = NULL;
	groups = NULL;
	shell = NULL;
	append = 0;
	lock = 0;
	unlock = 0;
	changes = 0;
	for (;;) {
		/* The next option. */
		option = getopt(argc, argv, "ac:g:G:s:LU");
		if (option == -1)
			break;

		/* What the option gives; every one but -a is a change. */
		switch (option) {
		case 'a':
			append = 1;
			break;
		case 'c':
			comment = optarg;
			changes++;
			break;
		case 'g':
			primary = optarg;
			changes++;
			break;
		case 'G':
			groups = optarg;
			changes++;
			break;
		case 's':
			shell = optarg;
			changes++;
			break;
		case 'L':
			lock = 1;
			changes++;
			break;
		case 'U':
			unlock = 1;
			changes++;
			break;
		default:
			usermod_usage();
			return USER_FRONT_USAGE;
		}
	}

	/* -a says how -G's groups are taken, so it means nothing alone. */
	if (append && groups == NULL) {
		fprintf(stderr, "%s: -a is taken only with -G\n", USERMOD_PROGRAM);
		usermod_usage();
		return USER_FRONT_USAGE;
	}

	/* A lock and an unlock at once cannot both be meant. */
	if (lock && unlock) {
		fprintf(stderr, "%s: -L and -U are not taken together\n", USERMOD_PROGRAM);
		usermod_usage();
		return USER_FRONT_USAGE;
	}

	/* Something to change. */
	if (changes == 0) {
		fprintf(stderr, "%s: no change is given\n", USERMOD_PROGRAM);
		usermod_usage();
		return USER_FRONT_USAGE;
	}

	/* One name, not empty. */
	if (argc - optind != 1 || argv[optind][0] == '\0') {
		usermod_usage();
		return USER_FRONT_USAGE;
	}

	/* The user the request is about. */
	name = argv[optind];

	/* -G's groups replace the user's, or with -a are added to them. */
	groups_key = "groups";
	if (append)
		groups_key = "groups-add";

	/* The password locked, unlocked, or left. */
	password = NULL;
	if (lock) {
		password = "lock";
	} else if (unlock) {
		password = "unlock";
	}

	/* The request: the name, then each change given. */
	user_front_begin(&request, USERMOD_PROGRAM, "user-modify");
	status = user_front_key(&request, "name", name);
	if (status != USER_FRONT_OK)
		return status;

	/* The display name. */
	status = user_front_key(&request, "comment", comment);
	if (status != USER_FRONT_OK)
		return status;

	/* The primary group. */
	status = user_front_key(&request, "group", primary);
	if (status != USER_FRONT_OK)
		return status;

	/* The other groups. */
	status = user_front_key(&request, groups_key, groups);
	if (status != USER_FRONT_OK)
		return status;

	/* The shell. */
	status = user_front_key(&request, "shell", shell);
	if (status != USER_FRONT_OK)
		return status;

	/* The lock. */
	status = user_front_key(&request, "password", password);
	if (status != USER_FRONT_OK)
		return status;

	/* account-admin changes the user, or says why not. */
	status = user_front_send(&request, name);
	if (status != USER_FRONT_OK)
		return status;

	/* Succeeded: the user is changed. */
	return USER_FRONT_OK;
}

/*
 * Says how the command is used.
 */
static void
usermod_usage(void)
{
	/* The one form. */
	fprintf(stderr, "usage: usermod [-c comment] [-g group] [-G group[,group] [-a]] [-s shell] [-L | -U] name\n");
}
