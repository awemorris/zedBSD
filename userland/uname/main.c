/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int
main(int argc, char **argv)
{
	struct utsname value;
	int all = 0, system = 0, node = 0, release = 0, version = 0, machine = 0;
	int index;
	for (index = 1; index < argc; index++) {
		const char *option = argv[index];
		if (option[0] != '-' || option[1] == '\0')
			goto usage;
		while (*++option != '\0') {
			if (*option == 'a') all = 1;
			else if (*option == 's') system = 1;
			else if (*option == 'n') node = 1;
			else if (*option == 'r') release = 1;
			else if (*option == 'v') version = 1;
			else if (*option == 'm') machine = 1;
			else goto usage;
		}
	}
	if (uname(&value) != 0) {
		command_error("uname", NULL);
		return 1;
	}
	if (!all && !system && !node && !release && !version && !machine)
		system = 1;
#define FIELD(selected, member) do { if ((selected) || all) { \
	printf("%s%s", printed++ ? " " : "", value.member); } } while (0)
	{
		int printed = 0;
		FIELD(system, sysname);
		FIELD(node, nodename);
		FIELD(release, release);
		FIELD(version, version);
		FIELD(machine, machine);
		putchar('\n');
	}
	return ferror(stdout) ? 1 : 0;
usage:
	fprintf(stderr, "usage: uname [-amnrsv]\n");
	return 1;
}
