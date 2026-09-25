/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws034-p055 guest test: getentropy(), /dev/random, /dev/urandom and
 * arc4random().  Prints "RANDOM ok|FAIL <case>", one "RANDOM SAMPLE <hex>"
 * line to compare across boots, and "RANDOM DONE n/m".
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed;
static int total;

static void
check(int ok, const char *name)
{
	total++;
	if (ok)
		passed++;
	printf("RANDOM %s %s\n", ok ? "ok" : "FAIL", name);
}

/* Counts the set bits of a buffer. */
static long
ones(const unsigned char *bytes, size_t length)
{
	long count = 0;
	size_t index;

	for (index = 0; index < length; index++)
		count += __builtin_popcount(bytes[index]);
	return count;
}

int
main(void)
{
	static unsigned char big[1048576];
	unsigned char first[32];
	unsigned char second[32];
	ssize_t got;
	long bits;
	int fd;
	int index;

	check(getentropy(first, sizeof(first)) == 0, "getentropy 32");
	check(getentropy(second, sizeof(second)) == 0 &&
	    memcmp(first, second, sizeof(first)) != 0, "getentropy differs");
	check(getentropy(big, 257) == -1, "getentropy 257 refused");
	printf("RANDOM SAMPLE ");
	for (index = 0; index < 16; index++)
		printf("%02x", first[index]);
	printf("\n");

	fd = open("/dev/urandom", O_RDONLY);
	got = fd < 0 ? -1 : read(fd, big, sizeof(big));
	check(got == (ssize_t)sizeof(big), "/dev/urandom 1 MiB in one read");
	bits = ones(big, sizeof(big));
	check(bits > 4194304 - 20000 && bits < 4194304 + 20000,
	    "/dev/urandom half the bits set");
	close(fd);

	fd = open("/dev/random", O_RDWR);
	check(fd >= 0 && read(fd, big, 4096) == 4096, "/dev/random 4 KiB");
	check(fd >= 0 && write(fd, "seed", 4) == 4, "/dev/random write");
	close(fd);

	check(arc4random() != arc4random() || arc4random() != arc4random(),
	    "arc4random varies");

	printf("RANDOM DONE %d/%d\n", passed, total);
	return passed == total ? 0 : 1;
}
