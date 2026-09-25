/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Prints the name of the effective user (as BSD and GNU whoami do; POSIX
 * has id -un for this).
 */

#include <pwd.h>
#include <stdio.h>
#include <unistd.h>

/*
 * Runs whoami.
 */
int
main(
	void)
{
	struct passwd *entry;
	uid_t user;

	/* The effective user and its entry in the user database. */
	user = geteuid();
	entry = getpwuid(user);
	if (entry == NULL) {
		fprintf(stderr, "whoami: cannot find name for user ID %lu\n", (unsigned long)user);
		return 1;
	}

	/* Succeeded: the name. */
	printf("%s\n", entry->pw_name);
	return 0;
}
