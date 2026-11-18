/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Changes the group of files (POSIX XCU chgrp).
 *
 *	chgrp [-h] group file...
 *	chgrp -R [-H|-L|-P] group file...
 *
 * group is a group name or a numeric group ID.  The traversal and the
 * symbolic link rules are those of chown, in userland/base/chown/change.c.
 */

#include "userland/base/chown/change.h"
#include <stdio.h>
#include <stdlib.h>

static void usage(void);

/*
 * Runs chgrp.
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

	/* Reads the options; the group and the files follow them. */
	change.program = "chgrp";
	first = owner_read_options(argc, argv, &change);
	if (first < 0)
		usage();
	if (argc - first < 2)
		usage();

	/* Resolves the group before any file changes; the owner stays. */
	change.uid = (uid_t)-1;
	status = owner_parse_group(argv[first], &change.gid);
	if (status != 0) {
		fprintf(stderr, "chgrp: invalid group: '%s'\n", argv[first]);
		return 1;
	}

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

	/* Succeeded: every file has its new group. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX forms. */
	fprintf(stderr,
		"usage: chgrp [-h] group file...\n"
		"       chgrp -R [-H|-L|-P] group file...\n");
	exit(1);
}
