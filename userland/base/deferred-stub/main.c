/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD deferred stub userland command.
 */

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
};

static const char *command_name(const char *path);

/*
 * Runs the deferred stub command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *name;
	size_t index;

	name = command_name(argc > 0 ? argv[0] : NULL);

	/* Process each remaining element. */
	for (index = 0; index < sizeof(commands) / sizeof(commands[0]);
	     index++) {
		/* Selects the matching value. */
		if (strcmp(name, commands[index].name) == 0) {
			fprintf(stderr, "%s: %s\n", name,
				commands[index].reason);

			/* Reports operation failure. */
			return 1;
		}
	}
	fprintf(stderr, "%s: unavailable facility is not identified\n", name);

	/* Reports operation failure. */
	return 1;
}

/* Supports the command name operation. */
static const char *
command_name(
	const char *path)
{
	const char *slash;

	/* Handles the path availability. */
	if (path == NULL || *path == '\0')
		return "deferred-command";
	slash = strrchr(path, '/');

	/* Returns the computed result. */
	return slash == NULL ? path : slash + 1;
}
