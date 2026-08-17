/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	char *separator; unsigned long long user, group = (unsigned long long)(gid_t)-1; int i, failed = 0;
	if (argc < 3)
		goto usage;
	separator = strchr(argv[1], ':');
	if (separator)
		*separator = '\0';
	if (command_parse_ull(argv[1], &user) ||
	    user > (unsigned long long)(uid_t)-1)
		goto usage;
	if (separator && (command_parse_ull(separator + 1, &group) ||
	    group > (unsigned long long)(gid_t)-1))
		goto usage;
	for (i = 2; i < argc; i++) if (chown(argv[i], (uid_t)user, (gid_t)group)) { command_error("chown", argv[i]); failed = 1; }
	return failed;
usage: fprintf(stderr, "usage: chown uid[:gid] file...\n"); return 1;
}
