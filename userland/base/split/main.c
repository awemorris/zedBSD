/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int
main(int argc, char **argv)
{
	unsigned long long limit = 1000, used = 0, index = 0;
	int bytes = 0, i = 1, in, out = -1;
	const char *prefix = "x";
	char name[512];
	if (i < argc && (!strcmp(argv[i], "-l") || !strcmp(argv[i], "-b"))) {
		bytes = argv[i][1] == 'b';
		if (++i >= argc || command_parse_ull(argv[i++], &limit) ||
		    !limit)
			return 2;
	}
	in = i < argc && !strcmp(argv[i], "-") ? 0
	     : i < argc			       ? open(argv[i++], O_RDONLY)
					       : 0;
	if (in < 0) {
		command_error("split", argv[i - 1]);
		return 1;
	}
	if (i < argc)
		prefix = argv[i++];
	if (i != argc) {
		fprintf(stderr, "usage: split [-l n|-b n] [file [prefix]]\n");
		return 2;
	}
	for (;;) {
		unsigned char c;
		ssize_t n = read(in, &c, 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			command_error("split", NULL);
			return 1;
		}
		if (!n)
			break;
		if (out < 0) {
			size_t p = strlen(prefix);
			if (p + 3 > sizeof(name) || index >= 26ULL * 26ULL) {
				fprintf(stderr, "split: too many files\n");
				return 1;
			}
			memcpy(name, prefix, p);
			name[p] = (char)('a' + index / 26);
			name[p + 1] = (char)('a' + index % 26);
			name[p + 2] = 0;
			out = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			if (out < 0) {
				command_error("split", name);
				return 1;
			}
			index++;
			used = 0;
		}
		if (command_write_all(out, &c, 1)) {
			command_error("split", name);
			return 1;
		}
		used += bytes ? 1 : (c == '\n');
		if (used >= limit) {
			if (close(out)) {
				command_error("split", name);
				return 1;
			}
			out = -1;
		}
	}
	if (out >= 0 && close(out)) {
		command_error("split", name);
		return 1;
	}
	if (in)
		close(in);
	return 0;
}
