/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * useradd: adds a user, for root (ws196-p003;
 * docs/reference/user-administration.md).
 *
 *   useradd [-c COMMENT] [-g GROUP] [-G GROUP[,GROUP]] [-m | -M] [-s SHELL] [-u UID] NAME
 *
 * The options become account-admin's user-add request (front.c), which
 * makes the account: a private group, a locked password (set it with
 * passwd NAME), and unless -M the home /home/NAME with /etc/skel.
 */

#include "userland/base/user-control/front.h"

#include <stdio.h>
#include <unistd.h>

/* The command's name, in its messages. */
#define USERADD_PROGRAM		"useradd"

static void useradd_usage(void);

/*
 * Reads the options and has account-admin add the user.
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
	const char *uid;
	const char *home;
	const char *name;
	int make_home;
	int no_home;
	int option;
	int status;

	/* Only root adds a user. */
	status = user_front_start(USERADD_PROGRAM);
	if (status != USER_FRONT_OK)
		return status;

	/* The options; a letter given again replaces its earlier value. */
	comment = NULL;
	primary = NULL;
	groups = NULL;
	shell = NULL;
	uid = NULL;
	make_home = 0;
	no_home = 0;
	for (;;) {
		/* The next option. */
		option = getopt(argc, argv, "c:g:G:mMs:u:");
		if (option == -1)
			break;

		/* What the option gives. */
		switch (option) {
		case 'c':
			comment = optarg;
			break;
		case 'g':
			primary = optarg;
			break;
		case 'G':
			groups = optarg;
			break;
		case 'm':
			make_home = 1;
			break;
		case 'M':
			no_home = 1;
			break;
		case 's':
			shell = optarg;
			break;
		case 'u':
			uid = optarg;
			break;
		default:
			useradd_usage();
			return USER_FRONT_USAGE;
		}
	}

	/* A home made and none at once cannot both be meant. */
	if (make_home && no_home) {
		fprintf(stderr, "%s: -m and -M are not taken together\n", USERADD_PROGRAM);
		useradd_usage();
		return USER_FRONT_USAGE;
	}

	/* One name, not empty. */
	if (argc - optind != 1 || argv[optind][0] == '\0') {
		useradd_usage();
		return USER_FRONT_USAGE;
	}

	/* The user the request is about. */
	name = argv[optind];

	/* The home: made with the skeleton unless -M. */
	home = "make";
	if (no_home)
		home = "none";

	/* The request: the name, each value given, and the home. */
	user_front_begin(&request, USERADD_PROGRAM, "user-add");
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
	status = user_front_key(&request, "groups", groups);
	if (status != USER_FRONT_OK)
		return status;

	/* The shell. */
	status = user_front_key(&request, "shell", shell);
	if (status != USER_FRONT_OK)
		return status;

	/* The user ID. */
	status = user_front_key(&request, "uid", uid);
	if (status != USER_FRONT_OK)
		return status;

	/* Whether the home is made. */
	status = user_front_key(&request, "home", home);
	if (status != USER_FRONT_OK)
		return status;

	/* account-admin adds the user, or says why not. */
	status = user_front_send(&request, name);
	if (status != USER_FRONT_OK)
		return status;

	/* Succeeded: the user is added. */
	return USER_FRONT_OK;
}

/*
 * Says how the command is used.
 */
static void
useradd_usage(void)
{
	/* The one form. */
	fprintf(stderr, "usage: useradd [-c comment] [-g group] [-G group[,group]] [-m | -M] [-s shell] [-u uid] name\n");
}
