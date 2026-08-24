/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int
dump(int fd, const char *name)
{
	unsigned char b[16];
	unsigned long long off = 0;
	int failed = 0;
	for (;;) {
		ssize_t n = read(fd, b, sizeof(b));
		size_t i;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			command_error("od", name);
			return 1;
		}
		if (n == 0)
			break;
		printf("%07llo", off);
		for (i = 0; i < (size_t)n; ++i)
			printf(" %03o", b[i]);
		putchar('\n');
		off += (unsigned long long)n;
	}
	printf("%07llo\n", off);
	return failed;
}
int
main(int argc, char **argv)
{
	int status = 0, i;
	if (argc == 1)
		return dump(STDIN_FILENO, "standard input");
	for (i = 1; i < argc; ++i) {
		int fd = !strcmp(argv[i], "-") ? STDIN_FILENO
					       : open(argv[i], O_RDONLY);
		if (fd < 0) {
			command_error("od", argv[i]);
			status = 1;
			continue;
		}
		status |= dump(fd, argv[i]);
		if (fd != STDIN_FILENO && close(fd) != 0) {
			command_error("od", argv[i]);
			status = 1;
		}
	}
	return status;
}
