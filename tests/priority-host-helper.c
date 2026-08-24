/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

int
main(int argc, char **argv)
{
	char *end;
	long identifier = 0;
	int priority;

	if (argc == 2) {
		identifier = strtol(argv[1], &end, 10);
		if (*argv[1] == '\0' || *end != '\0')
			return 2;
	} else if (argc != 1)
		return 2;
	errno = 0;
	priority = getpriority(PRIO_PROCESS, (id_t)identifier);
	if (priority == -1 && errno != 0)
		return 1;
	printf("%d\n", priority);
	return 0;
}
