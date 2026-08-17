/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdio.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	char *name;
	(void)argv;
	if (argc != 1) {
		fprintf(stderr, "usage: tty\n");
		return 2;
	}
	name = ttyname(STDIN_FILENO);
	if (name == NULL) {
		puts("not a tty");
		return 1;
	}
	puts(name);
	return ferror(stdout) ? 2 : 0;
}
