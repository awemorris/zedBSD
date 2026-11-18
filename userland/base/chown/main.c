/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Changes the owner and group of files (POSIX XCU chown).
 *
 *	chown [-h] owner[:group] file...
 *	chown -R [-H|-L|-P] owner[:group] file...
 *
 * owner and group are names or numeric IDs.  As on other systems, the
 * owner may be left out (:group changes the group only), and owner: with
 * nothing after the colon also gives the owner's login group.  The
 * traversal and the symbolic link rules are in change.c.
 */

#include "userland/base/chown/change.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_owner(const char *text, struct owner_change *change);
static void usage(void);

/*
 * Runs chown.
 */
int
main(
	int argc,
	char **argv)
{
	struct owner_change change;
	int first;
	int index;
	int failed;
	int status;

	/* Reads the options; the owner and the files follow them. */
	change.program = "chown";
	first = owner_read_options(argc, argv, &change);
	if (first < 0)
		usage();
	if (argc - first < 2)
		usage();

	/* Resolves the owner and group before any file changes. */
	status = parse_owner(argv[first], &change);
	if (status != 0)
		return 1;

	/* Changes each file; a failure is remembered and the rest go on. */
	failed = 0;
	for (index = first + 1; index < argc; index++) {
		status = owner_change_operand(&change, argv[index]);
		if (status != 0)
			failed = 1;
	}

	/* Reports whether any file could not be changed. */
	if (failed)
		return 1;

	/* Succeeded: every file has its new owner. */
	return 0;
}

/*
 * Parses owner[:group] into the IDs to set.  Writes a diagnostic and
 * returns -1 for an unknown user or group.
 */
static int
parse_owner(
	const char *text,
	struct owner_change *change)
{
	char user[256];
	const char *colon;
	const char *group;
	gid_t login_group;
	size_t length;
	int status;

	/* Nothing changes until the operand says so. */
	change->uid = (uid_t)-1;
	change->gid = (gid_t)-1;
	login_group = (gid_t)-1;

	/* Splits the operand at its colon. */
	colon = strchr(text, ':');
	length = strlen(text);
	group = NULL;
	if (colon != NULL) {
		length = (size_t)(colon - text);
		group = colon + 1;
	}

	/* Copies the user part, which must fit. */
	if (length >= sizeof(user)) {
		fprintf(stderr, "chown: invalid user: '%s'\n", text);
		return -1;
	}

	/* Terminates the copy of the user part. */
	memcpy(user, text, length);
	user[length] = '\0';

	/* Resolves the user, when one is given. */
	if (length > 0) {
		status = owner_parse_user(user, &change->uid, &login_group);
		if (status != 0) {
			fprintf(stderr, "chown: invalid user: '%s'\n", text);
			return -1;
		}
	}

	/* Resolves the group after the colon. */
	if (group != NULL && group[0] != '\0') {
		status = owner_parse_group(group, &change->gid);
		if (status != 0) {
			fprintf(stderr, "chown: invalid group: '%s'\n", text);
			return -1;
		}
	}

	/* owner: with nothing after the colon gives the login group too. */
	if (group != NULL && group[0] == '\0' && length > 0) {
		if (login_group == (gid_t)-1) {
			fprintf(stderr, "chown: invalid spec: '%s'\n", text);
			return -1;
		}

		/* The login group of the user. */
		change->gid = login_group;
	}

	/* Succeeded: the IDs to set. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX forms. */
	fprintf(stderr,
		"usage: chown [-h] owner[:group] file...\n"
		"       chown -R [-H|-L|-P] owner[:group] file...\n");
	exit(1);
}
