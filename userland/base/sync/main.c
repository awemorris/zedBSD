/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: t; -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/* Flushes all mounts or the explicitly named files and directories. */
#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	int index, descriptor, error, failed;

	index = 1;
	failed = 0;
	if (index < argc && !strcmp(argv[index], "--"))
		index++;
	else if (index < argc && argv[index][0] == '-') {
		fprintf(stderr, "usage: sync [--] [file ...]\n");
		return 1;
	}

	/* zedBSD libc preserves errors from its synchronous mount flush. */
	if (index == argc) {
		errno = 0;
		sync();
		if (errno != 0) {
			command_error("sync", "filesystems");
			return 1;
		}
		return 0;
	}

	/* Nonblocking open prevents an accidental FIFO operand from hanging. */
	for (; index < argc; index++) {
		descriptor = open(argv[index], O_RDONLY | O_NONBLOCK);
		if (descriptor < 0) {
			command_error("sync", argv[index]);
			failed = 1;
			continue;
		}
		error = 0;
		if (fsync(descriptor) != 0)
			error = errno;
		if (close(descriptor) != 0 && error == 0)
			error = errno;
		if (error != 0) {
			errno = error;
			command_error("sync", argv[index]);
			failed = 1;
		}
	}
	return failed;
}
