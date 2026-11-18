/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tally of checks and the reference files of the host display tests
 * (see host-test.h).
 */

#include "host-test.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * Whether the run is verbose.
 *
 * Zero until the test reads its command line; read by the check tally and
 * by the host kernel log.
 */
int i915_host_verbose;

/*
 * The number of checks made and the number that failed.
 *
 * Both only ever increase during a run; the report prints them and the
 * exit status is taken from the failures.
 */
static unsigned i915_host_check_count;
static unsigned i915_host_failure_count;

/*
 * Counts one check and prints it when it failed.
 *
 * A verbose run prints the passed checks as well.
 */
void
i915_host_check(
	int passed,
	const char *what)
{
	/* Counts the check. */
	i915_host_check_count++;

	/* A failed check is counted and always shown. */
	if (!passed) {
		i915_host_failure_count++;
		printf("FAIL: %s\n", what);
		return;
	}

	/* A passed check is shown only in a verbose run. */
	if (i915_host_verbose)
		printf("ok   %s\n", what);
}

/*
 * Returns the number of checks made so far.
 */
unsigned
i915_host_checks(void)
{
	/* Reports the tally. */
	return i915_host_check_count;
}

/*
 * Returns the number of checks that failed so far.
 */
unsigned
i915_host_failures(void)
{
	/* Reports the tally. */
	return i915_host_failure_count;
}

/*
 * Prints the tally of a test and returns its exit status.
 *
 * The line has the form the scripts and the logs have always used:
 * "<test>: N checks, M failures".
 */
int
i915_host_report(
	const char *test)
{
	/* Prints the tally. */
	printf("%s: %u checks, %u failures\n", test, i915_host_check_count, i915_host_failure_count);

	/* A failed check fails the test. */
	if (i915_host_failure_count != 0U)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Reads a reference file taken on the target laptop.
 *
 * The file must hold at least size bytes; a missing or short file ends the
 * test with exit status 2, because every check depends on it.
 */
void
i915_host_read_reference(
	const char *directory,
	const char *name,
	uint8_t *buffer,
	size_t size)
{
	char path[512];
	FILE *file;
	size_t count;

	/* Opens the file in the reference directory. */
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	file = fopen(path, "rb");
	if (file == NULL) {
		perror(path);
		exit(2);
	}

	/* Reads the bytes the test needs. */
	count = fread(buffer, 1U, size, file);
	fclose(file);
	if (count != size) {
		fprintf(stderr, "%s: %zu of %zu bytes\n", path, count, size);
		exit(2);
	}
}
