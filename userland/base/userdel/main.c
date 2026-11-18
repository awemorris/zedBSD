/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * userdel: removes a user, for root (ws196-p003;
 * docs/reference/user-administration.md).
 *
 *   userdel [-r | -f] NAME
 *
 * The options become account-admin's user-remove request (front.c).  -r
 * also removes the home; -f removes a user who still has a running
 * process.  The two are not taken together: a running program could keep
 * writing in the home while it is removed.
 */

#include "userland/base/user-control/front.h"

#include <stdio.h>
#include <unistd.h>

/* The command's name, in its messages. */
#define USERDEL_PROGRAM		"userdel"

static void userdel_usage(void);

/*
 * Reads the options and has account-admin remove the user.
 */
int
main(
	int argc,
	char **argv)
{
	struct user_front_request request;
	const char *home;
	const char *force;
	const char *name;
	int option;
	int status;

	/* Only root removes a user. */
	status = user_front_start(USERDEL_PROGRAM);
	if (status != USER_FRONT_OK)
		return status;

	/* The options. */
	home = NULL;
	force = NULL;
	for (;;) {
		/* The next option. */
		option = getopt(argc, argv, "rf");
		if (option == -1)
			break;

		/* What the option asks for. */
		switch (option) {
		case 'r':
			home = "remove";
			break;
		case 'f':
			force = "yes";
			break;
		default:
			userdel_usage();
			return USER_FRONT_USAGE;
		}
	}

	/* The home is not removed under a busy user's running programs. */
	if (home != NULL && force != NULL) {
		fprintf(stderr, "%s: -r and -f are not taken together\n", USERDEL_PROGRAM);
		userdel_usage();
		return USER_FRONT_USAGE;
	}

	/* One name, not empty. */
	if (argc - optind != 1 || argv[optind][0] == '\0') {
		userdel_usage();
		return USER_FRONT_USAGE;
	}

	/* The user the request is about. */
	name = argv[optind];

	/* The request: the name, then what the options ask for. */
	user_front_begin(&request, USERDEL_PROGRAM, "user-remove");
	status = user_front_key(&request, "name", name);
	if (status != USER_FRONT_OK)
		return status;

	/* The home removed. */
	status = user_front_key(&request, "home", home);
	if (status != USER_FRONT_OK)
		return status;

	/* A busy user removed all the same. */
	status = user_front_key(&request, "force", force);
	if (status != USER_FRONT_OK)
		return status;

	/* account-admin removes the user, or says why not. */
	status = user_front_send(&request, name);
	if (status != USER_FRONT_OK)
		return status;

	/* Succeeded: the user is removed. */
	return USER_FRONT_OK;
}

/*
 * Says how the command is used.
 */
static void
userdel_usage(void)
{
	/* The one form. */
	fprintf(stderr, "usage: userdel [-r | -f] name\n");
}
