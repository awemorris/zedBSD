/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int set(const char *s, unsigned char *out, size_t *count)
{
	size_t n = 0, i;
	for (i = 0; s[i]; ++i) {
		unsigned char a = (unsigned char)s[i], z = a;
		if (s[i + 1] == '-' && s[i + 2]) { z = (unsigned char)s[i + 2]; i += 2; if (z < a) return -1; }
		for (;;) { if (n == 256) return -1; out[n++] = a; if (a == z) break; ++a; }
	}
	*count = n; return 0;
}
int main(int argc, char **argv)
{
	int del = 0, squeeze = 0, ai = 1; unsigned char a[256], b[256], map[256], mark[256] = {0}, last = 0; size_t na, nb = 0, i; int have = 0;
	while (ai < argc && argv[ai][0] == '-') { if (!strcmp(argv[ai], "-d")) del = 1; else if (!strcmp(argv[ai], "-s")) squeeze = 1; else break; ++ai; }
	if (ai >= argc || (!del && ai + 1 >= argc) || set(argv[ai], a, &na) != 0 || (!del && set(argv[ai + 1], b, &nb) != 0)) { fprintf(stderr, "usage: tr [-d] [-s] string1 [string2]\n"); return 2; }
	for (i = 0; i < 256; ++i) map[i] = (unsigned char)i;
	for (i = 0; i < na; ++i) { mark[a[i]] = 1; if (!del) map[a[i]] = b[i < nb ? i : nb - 1]; }
	for (;;) { unsigned char buf[4096], out[4096]; ssize_t n = read(0, buf, sizeof(buf)); size_t used = 0;
		if (n < 0) { if (errno == EINTR) continue; command_error("tr", NULL); return 1; } if (!n) break;
		for (i = 0; i < (size_t)n; ++i) { unsigned char c = buf[i]; if (del && mark[c]) continue; c = map[c]; if (squeeze && have && c == last && (mark[c] || (!del && strchr(argv[ai + 1], c)))) continue; out[used++] = c; last = c; have = 1; }
		if (command_write_all(1, out, used) != 0) { command_error("tr", NULL); return 1; }
	} return 0;
}
