/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws034-p054: copies stdin to stdout 4096 bytes at a time with the buffer on
 * the stack, in static storage, from malloc, or page aligned, to compare the
 * cost of one read and one write by where the user buffer is.
 *   copybench stack|static|heap|aligned < in > out
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char static_buffer[4096];

static int
copy(unsigned char *buffer)
{
	ssize_t count;

	while ((count = read(0, buffer, 4096)) > 0) {
		if (write(1, buffer, (size_t)count) != count)
			return 1;
	}
	return count < 0;
}

int
main(int argc, char **argv)
{
	unsigned char stack_buffer[4096];
	void *aligned;

	if (argc < 2)
		return 2;
	if (strcmp(argv[1], "stack") == 0)
		return copy(stack_buffer);
	if (strcmp(argv[1], "static") == 0)
		return copy(static_buffer);
	if (strcmp(argv[1], "heap") == 0)
		return copy(malloc(4096));
	if (strcmp(argv[1], "aligned") == 0 &&
	    posix_memalign(&aligned, 4096, 4096) == 0)
		return copy(aligned);
	return 2;
}
