/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD sysctl userland command.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <zedbsd/sysctl.h>

#define NAME_MAX 64U

static int show_all(void);
static int show_name(const char *name);
static int set_name(const char *argument, const char *equal);

/*
 * Runs the sysctl command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	const char *equal;
	int error;

	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "-a") == 0) {
		/* Obtains the show all result. */
		function_result = show_all();

		/* Returns the computed result. */
		return function_result;
	}

	/* Validates the command-line arguments. */
	if (argc != 2) {
		fprintf(stderr, "usage: sysctl name[=value] | sysctl -a\n");

		/* Reports operation failure. */
		return 2;
	}
	equal = strchr(argv[1], '=');

	/* Handles the equal availability. */
	if (equal == NULL) {
		/* Validates the command-line arguments. */
		if (show_name(argv[1]) == 0)
			return 0;
		error = errno;
	} else {
		error = set_name(argv[1], equal);
	}
	fprintf(stderr, "sysctl: %s: %s\n", argv[1], strerror(error));

	/* Reports operation failure. */
	return 1;
}

/* Supports the show all operation. */
static int
show_all(
	void)
{
	static const char *const names[] = {
	    "vfs.bufcache.max_bytes",
	    "vfs.bufcache.current_bytes",
	    "vfs.bufcache.dirty_bytes",
	    "vfs.bufcache.stats",
	};
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		/* Handles a failed show name operation. */
		if (show_name(names[i]) != 0) {
			fprintf(stderr, "sysctl: %s: %s\n", names[i],
				strerror(errno));

			/* Reports operation failure. */
			return 1;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the show name operation. */
static int
show_name(
	const char *name)
{
	size_t length_local;
	size_t length_local1;
	struct bufcache_stats stats;
	uint64_t value;

	/* Selects the matching value. */
	if (strcmp(name, "vfs.bufcache.stats") == 0) {
		length_local = sizeof(stats);

		/* Handles a failed sysctlbyname operation. */
		if (sysctlbyname(name, &stats, &length_local, NULL, 0) != 0)
			return -1;
		printf("%s: buffers=%llu hits=%llu misses=%llu read_bios=%llu "
		       "write_bios=%llu evictions=%llu waits=%llu "
		       "writeback_errors=%llu\n",
		       name, (unsigned long long)stats.buffers,
		       (unsigned long long)stats.hits,
		       (unsigned long long)stats.misses,
		       (unsigned long long)stats.read_bios,
		       (unsigned long long)stats.write_bios,
		       (unsigned long long)stats.evictions,
		       (unsigned long long)stats.waits,
		       (unsigned long long)stats.writeback_errors);

		/* Reports successful completion. */
		return 0;
	} else {
		length_local1 = sizeof(value);

		/* Handles a failed sysctlbyname operation. */
		if (sysctlbyname(name, &value, &length_local1, NULL, 0) != 0)
			return -1;
		printf("%s: %llu\n", name, (unsigned long long)value);

		/* Reports successful completion. */
		return 0;
	}
}

/* Supports the set name operation. */
static int
set_name(
	const char *argument,
	const char *equal)
{
	int function_result;
	char name[NAME_MAX];
	char *end;
	unsigned long long parsed;
	uint64_t value;
	size_t name_length;

	name_length = (size_t)(equal - argument);

	/* Handles the name length condition. */
	if (name_length == 0 || name_length >= sizeof(name) || equal[1] == '\0')
		return EINVAL;
	memcpy(name, argument, name_length);
	name[name_length] = '\0';
	errno = 0;
	parsed = strtoull(equal + 1, &end, 10);

	/* Handles the reported system error. */
	if (errno != 0 || *end != '\0')
		return EINVAL;
	value = (uint64_t)parsed;

	/* Validates the current value. */
	if ((unsigned long long)value != parsed)
		return ERANGE;

	/* Handles a failed sysctlbyname operation. */
	if (sysctlbyname(name, NULL, NULL, &value, sizeof(value)) != 0)
		return errno;

	/* Computes the function result. */
	function_result = show_name(name) == 0 ? 0 : errno;

	/* Returns the computed result. */
	return function_result;
}
