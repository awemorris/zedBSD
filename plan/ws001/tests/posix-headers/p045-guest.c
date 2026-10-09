/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws001-p045's calls on a guest (T1): dprintf to a pipe, if_nameindex and
 * if_freenameindex (lo0 among the names), posix_fadvise on a file, on a
 * pipe (ESPIPE) and with a bad advice (EINVAL).  Compiled on the guest
 * with its clang: cc -o /tmp/p045 p045-guest.c && /tmp/p045.  The last
 * line is "p045-guest: PASS" or "p045-guest: FAIL (N)".
 */

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The checks that failed. */
static unsigned failures;

static void expect(int condition, const char *what);

/*
 * Runs the checks; the exit status says whether they all held.
 */
int
main(void)
{
	struct if_nameindex *names;
	char text[64];
	ssize_t got;
	int pipes[2];
	int descriptor;
	int written;
	int found;
	int error;
	int index;

	/* dprintf through a pipe: the whole text, read back. */
	error = pipe(pipes);
	expect(error == 0, "pipe");
	written = dprintf(pipes[1], "kei %d", 42);
	got = read(pipes[0], text, sizeof(text) - 1U);
	if (got < 0)
		got = 0;
	text[got] = '\0';
	expect(written == 6 && strcmp(text, "kei 42") == 0, "dprintf writes the whole text");

	/* posix_fadvise: a pipe refused, a bad advice refused, a file taken. */
	expect(posix_fadvise(pipes[0], 0, 0, POSIX_FADV_NORMAL) == ESPIPE, "posix_fadvise on a pipe is ESPIPE");
	descriptor = open("/etc/passwd", O_RDONLY);
	expect(descriptor >= 0, "open /etc/passwd");
	expect(posix_fadvise(descriptor, 0, 0, 99) == EINVAL, "posix_fadvise with a bad advice is EINVAL");
	expect(posix_fadvise(descriptor, 0, 0, POSIX_FADV_SEQUENTIAL) == 0, "posix_fadvise on a file is 0");
	(void)close(descriptor);
	(void)close(pipes[0]);
	(void)close(pipes[1]);

	/* if_nameindex: lo0 among the names, each with an index. */
	names = if_nameindex();
	expect(names != NULL, "if_nameindex");
	found = 0;
	for (index = 0; names != NULL && names[index].if_index != 0U; index++) {
		printf("if %u %s\n", names[index].if_index, names[index].if_name);
		if (strcmp(names[index].if_name, "lo0") == 0)
			found = 1;
	}
	expect(found, "lo0 among the interfaces");
	if (names != NULL)
		if_freenameindex(names);

	/* The verdict. */
	if (failures != 0U) {
		printf("p045-guest: FAIL (%u)\n", failures);
		return 1;
	}
	printf("p045-guest: PASS\n");
	return 0;
}

/* Counts and reports a check that does not hold. */
static void
expect(
	int condition,
	const char *what)
{
	/* A check that holds says nothing. */
	if (condition)
		return;
	failures++;
	printf("FAIL: %s\n", what);
}
