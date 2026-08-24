/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <stdio.h>
#include <string.h>

struct deferred_command {
	const char *name;
	const char *reason;
};

static const struct deferred_command commands[] = {
    {"at", "job scheduling service is unavailable"},
    {"batch", "batch scheduling service is unavailable"},
    {"crontab", "periodic scheduling service is unavailable"},
    {"logger", "system logging facility is unavailable"},
    {"mailx", "mail provider is not installed"},
    {"talk", "talk rendezvous service is unavailable"},
    {"lp", "no print destination is configured"},
};

static const char *
command_name(const char *path)
{
	const char *slash;

	if (path == NULL || *path == '\0')
		return "deferred-command";
	slash = strrchr(path, '/');
	return slash == NULL ? path : slash + 1;
}

int
main(int argc, char **argv)
{
	const char *name = command_name(argc > 0 ? argv[0] : NULL);
	size_t index;

	for (index = 0; index < sizeof(commands) / sizeof(commands[0]);
	     index++) {
		if (strcmp(name, commands[index].name) == 0) {
			fprintf(stderr, "%s: %s\n", name,
				commands[index].reason);
			return 1;
		}
	}
	fprintf(stderr, "%s: unavailable facility is not identified\n", name);
	return 1;
}
