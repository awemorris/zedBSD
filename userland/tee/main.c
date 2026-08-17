/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	int append = 0, i = 1, count, failed = 0; int descriptors[32]; unsigned char buffer[4096];
	if (i < argc && !strcmp(argv[i], "-a")) { append = 1; i++; }
	if (argc - i > 32) { fprintf(stderr, "tee: too many files\n"); return 1; }
	count = argc - i;
	for (int n = 0; n < count; n++) {
		descriptors[n] = open(argv[i + n], O_WRONLY | O_CREAT |
		    (append ? O_APPEND : O_TRUNC), 0666);
		if (descriptors[n] < 0) {
			command_error("tee", argv[i + n]);
			failed = 1;
		}
	}
	for (;;) {
		ssize_t got = read(STDIN_FILENO, buffer, sizeof(buffer));
		if (!got)
			break;
		if (got < 0) {
			if (errno == EINTR)
				continue;
			command_error("tee", NULL);
			failed = 1;
			break;
		}
		if (command_write_all(STDOUT_FILENO, buffer, (size_t)got))
			failed = 1;
		for (int n = 0; n < count; n++) {
			if (descriptors[n] >= 0 && command_write_all(descriptors[n],
			    buffer, (size_t)got)) {
				command_error("tee", argv[i + n]);
				close(descriptors[n]);
				descriptors[n] = -1;
				failed = 1;
			}
		}
	}
	for (int n = 0; n < count; n++) {
		if (descriptors[n] >= 0 && close(descriptors[n])) {
			command_error("tee", argv[i + n]);
			failed = 1;
		}
	}
	return failed;
}
