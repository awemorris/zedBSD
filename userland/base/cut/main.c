/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
selected(const char *list, unsigned long pos)
{
	const char *p = list;
	while (*p) {
		char *e;
		unsigned long a = strtoul(p, &e, 10), b = a;
		if (e == p || !a)
			return -1;
		p = e;
		if (*p == '-') {
			++p;
			if (*p == ',' || !*p)
				b = (unsigned long)-1;
			else {
				b = strtoul(p, &e, 10);
				if (e == p || b < a)
					return -1;
				p = e;
			}
		}
		if (pos >= a && pos <= b)
			return 1;
		if (*p == ',')
			++p;
		else if (*p)
			return -1;
	}
	return 0;
}
int
main(int argc, char **argv)
{
	int fields = 0, suppress = 0, i;
	const char *list = NULL;
	int delim = '\t';
	char *line = NULL;
	size_t cap = 0;
	long n;
	for (i = 1; i < argc; ++i) {
		if ((!strcmp(argv[i], "-b") || !strcmp(argv[i], "-c")) &&
		    ++i < argc)
			list = argv[i];
		else if (!strcmp(argv[i], "-f") && ++i < argc) {
			fields = 1;
			list = argv[i];
		} else if (!strcmp(argv[i], "-d") && ++i < argc && argv[i][0] &&
			   !argv[i][1])
			delim = argv[i][0];
		else if (!strcmp(argv[i], "-s"))
			suppress = 1;
		else
			break;
	}
	if (!list) {
		fprintf(stderr, "usage: cut -b list | -c list | -f list [-d "
				"char] [-s] [file ...]\n");
		return 2;
	}
	do {
		FILE *f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");
		if (!f) {
			command_error("cut", argv[i]);
			return 1;
		}
		while ((n = command_read_line(f, &line, &cap)) > 0) {
			long k;
			unsigned long pos = 1;
			int has = memchr(line, delim, (size_t)n) != NULL;
			if (fields && !has) {
				if (!suppress)
					fwrite(line, 1, (size_t)n, stdout);
				continue;
			}
			for (k = 0; k < n; ++k) {
				int yes;
				if (!fields && line[k] == '\n') {
					fputc('\n', stdout);
					continue;
				}
				yes = selected(list, pos);
				if (yes < 0) {
					fprintf(stderr, "cut: invalid list\n");
					return 2;
				}
				if (yes)
					fputc(line[k], stdout);
				if (fields && line[k] == delim)
					++pos;
				else if (!fields)
					++pos;
			}
		}
		if (n < 0) {
			command_error("cut", i == argc ? NULL : argv[i]);
			return 1;
		}
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);
	free(line);
	return ferror(stdout) ? 1 : 0;
}
