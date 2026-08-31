/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD getconf userland command.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if __SIZEOF_POINTER__ == 8
#define GETCONF_MODEL_V7 "POSIX_V7_LP64_OFF64"
#define GETCONF_MODEL_V8 "POSIX_V8_LP64_OFF64"
#define GETCONF_ILP32_CFLAGS ""
#define GETCONF_ILP32_LDFLAGS ""
#define GETCONF_LP64_CFLAGS "-m64"
#define GETCONF_LP64_LDFLAGS "-m64"
#else
#define GETCONF_MODEL_V7 "POSIX_V7_ILP32_OFF32"
#define GETCONF_MODEL_V8 "POSIX_V8_ILP32_OFF32"
#define GETCONF_ILP32_CFLAGS "-m32"
#define GETCONF_ILP32_LDFLAGS "-m32"
#define GETCONF_LP64_CFLAGS ""
#define GETCONF_LP64_LDFLAGS ""
#endif

enum variable_kind {
	VARIABLE_SYSCONF,
	VARIABLE_PATHCONF,
	VARIABLE_CONFSTR,
	VARIABLE_CONSTANT,
	VARIABLE_TEXT,
};

struct variable {
	const char *name;
	enum variable_kind kind;
	long number;
	const char *text;
};

static const struct variable variables[] = {
#define GETCONF_SYSCONF(name, selector)                                        \
	{name, VARIABLE_SYSCONF, selector, NULL},
#define GETCONF_PATHCONF(name, selector)                                       \
	{name, VARIABLE_PATHCONF, selector, NULL},
#define GETCONF_CONFSTR(name, selector)                                        \
	{name, VARIABLE_CONFSTR, selector, NULL},
#define GETCONF_CONSTANT(name, value) {name, VARIABLE_CONSTANT, value, NULL},
#define GETCONF_TEXT(name, value) {name, VARIABLE_TEXT, 0, value},
#include "userland/base/getconf/getconf-table.h"
#undef GETCONF_SYSCONF
#undef GETCONF_PATHCONF
#undef GETCONF_CONFSTR
#undef GETCONF_CONSTANT
#undef GETCONF_TEXT
};

static int specification_supported(const char *name);
static int print_variable(const struct variable *variable, const char *path, int show_name);
static const struct variable *variable_find(const char *name);

/*
 * Runs the getconf command.
 */
int
main(
	int argc,
	char **argv)
{
	size_t variable_index;
	int result;
	const char *specification;
	const char *path;
	const struct variable *variable;
	int index;

	specification = NULL;
	path = NULL;
	index = 1;

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "-v") == 0) {
		/* Validates the command-line arguments. */
		if (++index >= argc)
			goto usage;
		specification = argv[index++];

		/* Handles a failed specification supported operation. */
		if (!specification_supported(specification)) {
			fprintf(stderr,
				"getconf: unsupported specification: %s\n",
				specification);

			/* Reports operation failure. */
			return 1;
		}
	}

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "-a") == 0) {
		result = 1;

		index++;
		path = index < argc ? argv[index++] : ".";

		/* Validates the command-line arguments. */
		if (index != argc)
			goto usage;

		/* Process each remaining element. */
		for (variable_index = 0;
		     variable_index < sizeof(variables) / sizeof(variables[0]);
		     variable_index++) {
			/* Handles a failed print variable operation. */
			if (!print_variable(&variables[variable_index], path,
					    1))
				result = 0;
		}

		/* Returns the computed result. */
		return result ? 0 : 1;
	}

	/* Validates the command-line arguments. */
	if (index >= argc)
		goto usage;
	variable = variable_find(argv[index++]);

	/* Handles the variable availability. */
	if (variable == NULL) {
		fprintf(stderr, "getconf: unknown variable: %s\n",
			argv[index - 1]);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the variable condition. */
	if (variable->kind == VARIABLE_PATHCONF) {
		/* Validates the command-line arguments. */
		if (index >= argc)
			goto usage;
		path = argv[index++];
	} else if (index < argc)
		goto usage;

	/* Validates the command-line arguments. */
	if (index != argc)
		goto usage;

	/* Handles a failed print variable operation. */
	if (!print_variable(variable, path, 0)) {
		fprintf(stderr, "getconf: %s: %s\n", variable->name,
			strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;

usage:
	fprintf(stderr,
		"usage: getconf [-v specification] variable [pathname]\n"
		"       getconf [-v specification] -a [pathname]\n");

	/* Reports operation failure. */
	return 2;
}

/* Supports the specification supported operation. */
static int
specification_supported(
	const char *name)
{
	int function_result;

	/* Computes the function result. */
	function_result = strcmp(name, GETCONF_MODEL_V7) == 0 ||
	       strcmp(name, GETCONF_MODEL_V8) == 0 ||
	       strcmp(name, "POSIX_V7_THREADS") == 0 ||
	       strcmp(name, "POSIX_V8_THREADS") == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the print variable operation. */
static int
print_variable(
	const struct variable *variable,
	const char *path,
	int show_name)
{
	size_t needed;
	char *text;
	long value;

	/* Handles the show name condition. */
	if (show_name)
		printf("%-38s ", variable->name);

	/* Handles the variable condition. */
	if (variable->kind == VARIABLE_TEXT) {
		puts(variable->text);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the variable condition. */
	if (variable->kind == VARIABLE_CONSTANT) {
		printf("%ld\n", variable->number);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the variable condition. */
	if (variable->kind == VARIABLE_CONFSTR) {
		needed = confstr((int)variable->number, NULL, 0);

		/* Handles the needed condition. */
		if (needed == 0)
			goto failed;
		text = malloc(needed);

		/* Handles a failed confstr operation. */
		if (text == NULL ||
		    confstr((int)variable->number, text, needed) != needed) {
			free(text);
			goto failed;
		}
		puts(text);
		free(text);

		/* Reports operation failure. */
		return 1;
	}
	errno = 0;

	/* Handles the variable condition. */
	if (variable->kind == VARIABLE_PATHCONF)
		value = pathconf(path, (int)variable->number);
	else
		value = sysconf((int)variable->number);

	/* Handles the reported system error. */
	if (value == -1 && errno == 0) {
		puts("undefined");

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the current value. */
	if (value == -1)
		goto failed;
	printf("%ld\n", value);

	/* Reports operation failure. */
	return 1;

failed:

	/* Handles the show name condition. */
	if (show_name)
		putchar('\n');

	/* Reports successful completion. */
	return 0;
}

/* Supports the variable find operation. */
static const struct variable *
variable_find(
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < sizeof(variables) / sizeof(variables[0]);
	     index++) {
		/* Selects the matching value. */
		if (strcmp(variables[index].name, name) == 0)
			return &variables[index];
	}

	/* Reports that no result is available. */
	return NULL;
}
