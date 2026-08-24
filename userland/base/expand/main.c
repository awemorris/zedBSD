/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
int
main(int argc, char **argv)
{
	unsigned long long tab = 8, col = 0;
	int i = 1, c;
	if (i < argc && !strcmp(argv[i], "-t") && ++i < argc) {
		if (command_parse_ull(argv[i++], &tab) || !tab) {
			fprintf(stderr, "expand: invalid tab stop\n");
			return 2;
		}
	}
	do {
		FILE *f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");
		if (!f) {
			command_error("expand", argv[i]);
			return 1;
		}
		while ((c = fgetc(f)) != EOF) {
			if (c == '\t') {
				unsigned long long n = tab - col % tab;
				while (n--)
					putchar(' ');
				col += tab - col % tab;
			} else {
				putchar(c);
				if (c == '\n' || c == '\r')
					col = 0;
				else if (c == '\b' && col)
					col--;
				else
					col++;
			}
		}
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);
	return ferror(stdout);
}
