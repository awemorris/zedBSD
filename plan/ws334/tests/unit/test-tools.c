/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The self-test of WS334's unit-test tools: the harness builds and runs a
 * test on each system, with libbrowser's base helpers linked, in a
 * directory of its own.
 */

#include "check.h"

#include "base/base.h"

#include <stdio.h>
#include <string.h>

/*
 * Checks the arguments, a buffer of the base helpers and a file in the
 * test's own directory.
 */
int
main(
	int argc,
	char **argv)
{
	struct wb_buffer buffer;
	FILE *file;
	int differs;
	int error;

	/* The harness passes the system's name. */
	CHECK(argc == 2);
	differs = 1;
	if (argc == 2)
		differs = strcmp(argv[1], "posix") != 0 && strcmp(argv[1], "win32") != 0;
	CHECK(differs == 0);

	/* The base helpers are linked and work. */
	wb_buffer_init(&buffer);
	error = wb_buffer_append_string(&buffer, "zedBSD");
	CHECK_EQ(error, 0);
	CHECK_EQ(buffer.length, 6);
	wb_buffer_release(&buffer);

	/* The current directory is the test's own and can be written. */
	file = fopen("tools-test.txt", "wb");
	CHECK(file != NULL);
	if (file != NULL)
		fclose(file);

	/* Reports the result. */
	return check_finish("tools");
}
