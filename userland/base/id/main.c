/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int
main(int argc, char **argv)
{
	int i, count;
	gid_t groups[32];
	if (argc == 2 && !strcmp(argv[1], "-u")) {
		printf("%u\n", (unsigned)geteuid());
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "-g")) {
		printf("%u\n", (unsigned)getegid());
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "-G")) {
		count = getgroups(32, groups);
		if (count < 0) {
			command_error("id", NULL);
			return 1;
		}
		for (i = 0; i < count; i++)
			printf("%s%u", i ? " " : "", (unsigned)groups[i]);
		putchar('\n');
		return 0;
	}
	if (argc != 1) {
		fprintf(stderr, "usage: id [-u|-g|-G]\n");
		return 1;
	}
	printf("uid=%u euid=%u gid=%u egid=%u", (unsigned)getuid(),
	       (unsigned)geteuid(), (unsigned)getgid(), (unsigned)getegid());
	count = getgroups(32, groups);
	if (count >= 0) {
		printf(" groups=");
		for (i = 0; i < count; i++)
			printf("%s%u", i ? "," : "", (unsigned)groups[i]);
	}
	putchar('\n');
	return 0;
}
