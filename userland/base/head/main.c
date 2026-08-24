/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int
copy_head(int input, unsigned long long limit, int bytes)
{
	unsigned char buffer[4096];
	unsigned long long count = 0;
	while (count < limit) {
		size_t wanted = sizeof(buffer);
		ssize_t got;
		if (bytes && limit - count < wanted)
			wanted = (size_t)(limit - count);
		got = read(input, buffer, wanted);
		if (got == 0)
			return 0;
		if (got < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (bytes) {
			if (command_write_all(STDOUT_FILENO, buffer,
					      (size_t)got) != 0)
				return -1;
			count += (unsigned long long)got;
		} else {
			size_t used = 0;
			while (used < (size_t)got && count < limit) {
				size_t end = used;
				while (end < (size_t)got && buffer[end] != '\n')
					end++;
				if (end < (size_t)got) {
					end++;
					count++;
				}
				if (command_write_all(STDOUT_FILENO,
						      buffer + used,
						      end - used) != 0)
					return -1;
				used = end;
			}
			/* Bytes after the requested newline were read
			 * speculatively.  This is harmless for named files but
			 * violates pipeline semantics, so use one-byte reads
			 * for line mode below instead. */
			if (used < (size_t)got) {
				errno = ESPIPE;
				return -1;
			}
		}
	}
	return 0;
}

static int
copy_head_lines(int input, unsigned long long limit)
{
	unsigned char byte;
	unsigned long long lines = 0;
	while (lines < limit) {
		ssize_t got = read(input, &byte, 1);
		if (got == 0)
			return 0;
		if (got < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (command_write_all(STDOUT_FILENO, &byte, 1) != 0)
			return -1;
		if (byte == '\n')
			lines++;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	unsigned long long limit = 10;
	int bytes = 0, index = 1, failed = 0, files;
	if (index < argc &&
	    (!strcmp(argv[index], "-n") || !strcmp(argv[index], "-c"))) {
		bytes = argv[index][1] == 'c';
		if (++index == argc ||
		    command_parse_ull(argv[index++], &limit) != 0)
			goto usage;
	}
	if (index < argc && !strcmp(argv[index], "--"))
		index++;
	files = argc - index;
	if (files == 0)
		return (bytes ? copy_head(STDIN_FILENO, limit, 1)
			      : copy_head_lines(STDIN_FILENO, limit)) != 0;
	for (; index < argc; index++) {
		int descriptor = !strcmp(argv[index], "-")
				     ? STDIN_FILENO
				     : open(argv[index], O_RDONLY);
		if (descriptor < 0) {
			command_error("head", argv[index]);
			failed = 1;
			continue;
		}
		if (files > 1)
			printf("%s==> %s <==\n",
			       index == argc - files ? "" : "\n", argv[index]);
		if ((bytes ? copy_head(descriptor, limit, 1)
			   : copy_head_lines(descriptor, limit)) != 0) {
			command_error("head", argv[index]);
			failed = 1;
		}
		if (descriptor != STDIN_FILENO && close(descriptor) != 0) {
			command_error("head", argv[index]);
			failed = 1;
		}
	}
	return failed;
usage:
	fprintf(stderr, "usage: head [-n number | -c bytes] [file...]\n");
	return 1;
}
