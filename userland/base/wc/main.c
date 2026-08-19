/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int count_fd(int fd, const char *name, int lines, int words, int bytes)
{
	unsigned char buffer[4096]; unsigned long long l = 0, w = 0, b = 0; int inword = 0;
	for (;;) { ssize_t n = read(fd, buffer, sizeof(buffer)); size_t i; if (n < 0) { command_error("wc", name); return -1; } if (!n) break; b += (unsigned long long)n; for (i = 0; i < (size_t)n; i++) { if (buffer[i] == '\n') l++; if (isspace(buffer[i])) inword = 0; else if (!inword) { inword = 1; w++; } } }
	if (lines)
		printf("%7llu", l);
	if (words)
		printf("%7llu", w);
	if (bytes)
		printf("%7llu", b);
	if (name)
		printf(" %s", name);
	putchar('\n');
	return 0;
}
int main(int argc, char **argv)
{
	int lines = 0, words = 0, bytes = 0, i = 1, failed = 0;
	for (; i < argc && argv[i][0] == '-'; i++) { const char *p = argv[i] + 1; if (!*p) break; while (*p) { if (*p == 'l') lines = 1; else if (*p == 'w') words = 1; else if (*p == 'c') bytes = 1; else goto usage; p++; } }
	if (!lines && !words && !bytes) lines = words = bytes = 1;
	if (i == argc) return count_fd(STDIN_FILENO, NULL, lines, words, bytes) != 0;
	for (; i < argc; i++) { int fd = !strcmp(argv[i], "-") ? STDIN_FILENO : open(argv[i], O_RDONLY); if (fd < 0) { command_error("wc", argv[i]); failed = 1; continue; } if (count_fd(fd, argv[i], lines, words, bytes)) failed = 1; if (fd != STDIN_FILENO) close(fd); }
	return failed;
usage: fprintf(stderr, "usage: wc [-clw] [file...]\n"); return 1;
}
