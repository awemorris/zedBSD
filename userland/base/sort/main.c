/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int reverse, numeric;
static int
cmp(const char *x, const char *y)
{
	int c;
	if (numeric) {
		long long p = strtoll(x, NULL, 10), q = strtoll(y, NULL, 10);
		c = (p > q) - (p < q);
		if (!c)
			c = strcmp(x, y);
	} else
		c = strcmp(x, y);
	return reverse ? -c : c;
}
int
main(int argc, char **argv)
{
	int unique = 0, i = 1, status = 0;
	char **v = NULL, *line = NULL;
	size_t n = 0, cap = 0, lc = 0;
	long len;
	for (; i < argc && argv[i][0] == '-'; ++i) {
		const char *p = argv[i] + 1;
		while (*p) {
			if (*p == 'r')
				reverse = 1;
			else if (*p == 'n')
				numeric = 1;
			else if (*p == 'u')
				unique = 1;
			else {
				fprintf(stderr, "sort: invalid option\n");
				return 2;
			}
			++p;
		}
	}
	do {
		FILE *f = i == argc || !strcmp(argv[i], "-")
			      ? stdin
			      : fopen(argv[i], "r");
		if (!f) {
			command_error("sort", argv[i]);
			status = 1;
			++i;
			continue;
		}
		while ((len = command_read_line(f, &line, &lc)) > 0) {
			char *s = malloc((size_t)len + 1);
			if (!s)
				return 1;
			memcpy(s, line, (size_t)len + 1);
			if (n == cap) {
				size_t nc = cap ? cap * 2 : 64;
				char **nv = realloc(v, nc * sizeof(*v));
				if (!nv)
					return 1;
				v = nv;
				cap = nc;
			}
			v[n++] = s;
		}
		if (f != stdin)
			fclose(f);
		++i;
	} while (i < argc);
	for (i = 1; i < (int)n; ++i) {
		char *x = v[i];
		int j = i;
		while (j && cmp(v[j - 1], x) > 0) {
			v[j] = v[j - 1];
			--j;
		}
		v[j] = x;
	}
	for (i = 0; i < (int)n; ++i) {
		if (unique && i && !strcmp(v[i - 1], v[i]))
			continue;
		fwrite(v[i], 1, strlen(v[i]), stdout);
	}
	for (i = 0; i < (int)n; ++i)
		free(v[i]);
	free(v);
	free(line);
	return status || ferror(stdout);
}
