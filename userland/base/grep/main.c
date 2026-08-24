/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int
atom(const char *p, char c)
{
	return *p == '.' || *p == c;
}
static int
here(const char *p, const char *s)
{
	if (!*p)
		return 1;
	if (p[1] == '*') {
		do {
			if (here(p + 2, s))
				return 1;
		} while (*s && atom(p, *s++));
		return 0;
	}
	if (*p == '$' && !p[1])
		return !*s;
	if (*s && atom(p, *s))
		return here(p + 1, s + 1);
	return 0;
}
static int
match(const char *p, const char *s)
{
	if (*p == '^')
		return here(p + 1, s);
	do {
		if (here(p, s))
			return 1;
	} while (*s++);
	return 0;
}
int
main(int argc, char **argv)
{
	int inv = 0, num = 0, count = 0, list = 0, quiet = 0, fixed = 0, i = 1,
	    status = 1, multi;
	const char *pat;
	for (; i < argc && argv[i][0] == '-'; i++) {
		const char *p = argv[i] + 1;
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}
		while (*p) {
			if (*p == 'v')
				inv = 1;
			else if (*p == 'n')
				num = 1;
			else if (*p == 'c')
				count = 1;
			else if (*p == 'l')
				list = 1;
			else if (*p == 'q')
				quiet = 1;
			else if (*p == 'F')
				fixed = 1;
			else if (*p == 'E') {
			} else {
				fprintf(stderr, "grep: invalid option\n");
				return 2;
			}
			p++;
		}
	}
	if (i >= argc) {
		fprintf(stderr, "usage: grep [-vnlcqFE] pattern [file ...]\n");
		return 2;
	}
	pat = argv[i++];
	multi = argc - i > 1;
	do {
		FILE *f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");
		char *l = NULL;
		size_t cap = 0;
		long n, ln = 0, hits = 0;
		const char *name = i == argc ? "(standard input)" : argv[i];
		if (!f) {
			command_error("grep", name);
			status = 2;
			++i;
			continue;
		}
		while ((n = command_read_line(f, &l, &cap)) > 0) {
			int yes;
			ln++;
			if (n && l[n - 1] == '\n')
				l[n - 1] = 0;
			yes = fixed ? strstr(l, pat) != NULL : match(pat, l);
			if (inv)
				yes = !yes;
			if (yes) {
				hits++;
				status = 0;
				if (quiet)
					break;
				if (list) {
					puts(name);
					break;
				}
				if (!count) {
					if (multi)
						printf("%s:", name);
					if (num)
						printf("%ld:", ln);
					fwrite(l, 1, strlen(l), stdout);
					putchar('\n');
				}
			}
		}
		if (count) {
			if (multi)
				printf("%s:", name);
			printf("%ld\n", hits);
		}
		free(l);
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);
	return status;
}
