/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int
main(int argc, char **argv)
{
	const char *prog;
	int field = 0, whole = 0, i = 1;
	if (argc < 2) {
		fprintf(stderr, "usage: awk program [file ...]\n");
		return 2;
	}
	prog = argv[i++];
	if (!strcmp(prog, "{print}") || !strcmp(prog, "{ print }"))
		whole = 1;
	else {
		const char *p = strstr(prog, "print $");
		if (p)
			field = atoi(p + 7);
		if (field < 1) {
			fprintf(stderr, "awk: selected implementation supports "
					"{ print $N }\n");
			return 2;
		}
	}
	do {
		FILE *f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");
		char *l = NULL;
		size_t cap = 0;
		long n;
		if (!f) {
			command_error("awk", argv[i]);
			return 1;
		}
		while ((n = command_read_line(f, &l, &cap)) > 0) {
			if (whole)
				fwrite(l, 1, (size_t)n, stdout);
			else {
				char *p = l;
				int k = 1;
				while (isspace((unsigned char)*p))
					p++;
				while (k < field && *p) {
					while (*p &&
					       !isspace((unsigned char)*p))
						p++;
					while (isspace((unsigned char)*p))
						p++;
					k++;
				}
				{
					char *e = p;
					while (*e &&
					       !isspace((unsigned char)*e))
						e++;
					fwrite(p, 1, (size_t)(e - p), stdout);
					putchar('\n');
				}
			}
		}
		free(l);
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);
	return ferror(stdout);
}
