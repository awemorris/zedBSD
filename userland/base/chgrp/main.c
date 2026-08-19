/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	unsigned long long group; int i, failed = 0;
	if (argc < 3 || command_parse_ull(argv[1], &group) || group > (unsigned long long)(gid_t)-1) { fprintf(stderr, "usage: chgrp gid file...\n"); return 1; }
	for (i = 2; i < argc; i++) if (chown(argv[i], (uid_t)-1, (gid_t)group)) { command_error("chgrp", argv[i]); failed = 1; }
	return failed;
}
