/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>

int
main(int argc, char **argv)
{
	unsigned long long seconds;
	struct timespec request, remaining;
	if (argc != 2 || command_parse_ull(argv[1], &seconds) != 0 ||
	    (time_t)seconds < 0 || (unsigned long long)(time_t)seconds != seconds) {
		fprintf(stderr, "usage: sleep seconds\n");
		return 1;
	}
	request.tv_sec = (time_t)seconds;
	request.tv_nsec = 0;
	while (nanosleep(&request, &remaining) != 0) {
		if (errno != EINTR) {
			command_error("sleep", NULL);
			return 1;
		}
		request = remaining;
	}
	return 0;
}
