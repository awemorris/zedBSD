/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int
main(int argc, char **argv)
{
	unsigned char a[4096], b[4096];
	unsigned long long offset = 0, line = 1;
	int fa, fb;
	if (argc != 3) {
		fprintf(stderr, "usage: cmp file1 file2\n");
		return 2;
	}
	fa = !strcmp(argv[1], "-") ? 0 : open(argv[1], O_RDONLY);
	fb = !strcmp(argv[2], "-") ? 0 : open(argv[2], O_RDONLY);
	if (fa < 0 || fb < 0) {
		command_error("cmp", fa < 0 ? argv[1] : argv[2]);
		return 2;
	}
	for (;;) {
		ssize_t na = read(fa, a, sizeof(a)),
			nb = read(fb, b, sizeof(b));
		size_t n, i;
		if (na < 0 || nb < 0) {
			command_error("cmp", na < 0 ? argv[1] : argv[2]);
			return 2;
		}
		n = (size_t)(na < nb ? na : nb);
		for (i = 0; i < n; i++) {
			if (a[i] != b[i]) {
				printf("%s %s differ: byte %llu, line %llu\n",
				       argv[1], argv[2], offset + i + 1, line);
				return 1;
			}
			if (a[i] == '\n')
				line++;
		}
		offset += n;
		if (na != nb) {
			printf("cmp: EOF on %s\n", na < nb ? argv[1] : argv[2]);
			return 1;
		}
		if (!na)
			return 0;
	}
}
