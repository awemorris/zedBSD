/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int
main(int argc, char **argv)
{
	unsigned long long number = 1, inc = 1;
	int all = 0, i = 1;
	char *l = NULL;
	size_t cap = 0;
	long n;
	for (; i < argc; i++) {
		if (!strcmp(argv[i], "-ba"))
			all = 1;
		else if (!strcmp(argv[i], "-bt"))
			all = 0;
		else if (!strcmp(argv[i], "-v") && ++i < argc) {
			if (command_parse_ull(argv[i], &number))
				return 2;
		} else if (!strcmp(argv[i], "-i") && ++i < argc) {
			if (command_parse_ull(argv[i], &inc))
				return 2;
		} else
			break;
	}
	do {
		FILE *f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");
		if (!f) {
			command_error("nl", argv[i]);
			return 1;
		}
		while ((n = command_read_line(f, &l, &cap)) > 0) {
			int nonempty = n > 1 || (n == 1 && l[0] != '\n');
			if (all || nonempty) {
				printf("%6llu\t", number);
				number += inc;
			} else
				printf("       ");
			fwrite(l, 1, (size_t)n, stdout);
		}
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);
	free(l);
	return ferror(stdout);
}
