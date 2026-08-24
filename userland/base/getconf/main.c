/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

static const struct variable *
variable_find(const char *name)
{
	size_t index;

	for (index = 0; index < sizeof(variables) / sizeof(variables[0]);
	     index++)
		if (strcmp(variables[index].name, name) == 0)
			return &variables[index];
	return NULL;
}

static int
specification_supported(const char *name)
{
	return strcmp(name, GETCONF_MODEL_V7) == 0 ||
	       strcmp(name, GETCONF_MODEL_V8) == 0 ||
	       strcmp(name, "POSIX_V7_THREADS") == 0 ||
	       strcmp(name, "POSIX_V8_THREADS") == 0;
}

static int
print_variable(const struct variable *variable, const char *path, int show_name)
{
	long value;

	if (show_name)
		printf("%-38s ", variable->name);
	if (variable->kind == VARIABLE_TEXT) {
		puts(variable->text);
		return 1;
	}
	if (variable->kind == VARIABLE_CONSTANT) {
		printf("%ld\n", variable->number);
		return 1;
	}
	if (variable->kind == VARIABLE_CONFSTR) {
		size_t needed = confstr((int)variable->number, NULL, 0);
		char *text;

		if (needed == 0)
			goto failed;
		text = malloc(needed);
		if (text == NULL ||
		    confstr((int)variable->number, text, needed) != needed) {
			free(text);
			goto failed;
		}
		puts(text);
		free(text);
		return 1;
	}
	errno = 0;
	if (variable->kind == VARIABLE_PATHCONF)
		value = pathconf(path, (int)variable->number);
	else
		value = sysconf((int)variable->number);
	if (value == -1 && errno == 0) {
		puts("undefined");
		return 1;
	}
	if (value == -1)
		goto failed;
	printf("%ld\n", value);
	return 1;

failed:
	if (show_name)
		putchar('\n');
	return 0;
}

int
main(int argc, char **argv)
{
	const char *specification = NULL;
	const char *path = NULL;
	const struct variable *variable;
	int index = 1;

	if (index < argc && strcmp(argv[index], "-v") == 0) {
		if (++index >= argc)
			goto usage;
		specification = argv[index++];
		if (!specification_supported(specification)) {
			fprintf(stderr,
				"getconf: unsupported specification: %s\n",
				specification);
			return 1;
		}
	}
	if (index < argc && strcmp(argv[index], "-a") == 0) {
		size_t variable_index;
		int result = 1;

		index++;
		path = index < argc ? argv[index++] : ".";
		if (index != argc)
			goto usage;
		for (variable_index = 0;
		     variable_index < sizeof(variables) / sizeof(variables[0]);
		     variable_index++)
			if (!print_variable(&variables[variable_index], path,
					    1))
				result = 0;
		return result ? 0 : 1;
	}
	if (index >= argc)
		goto usage;
	variable = variable_find(argv[index++]);
	if (variable == NULL) {
		fprintf(stderr, "getconf: unknown variable: %s\n",
			argv[index - 1]);
		return 1;
	}
	if (variable->kind == VARIABLE_PATHCONF) {
		if (index >= argc)
			goto usage;
		path = argv[index++];
	} else if (index < argc)
		goto usage;
	if (index != argc)
		goto usage;
	if (!print_variable(variable, path, 0)) {
		fprintf(stderr, "getconf: %s: %s\n", variable->name,
			strerror(errno));
		return 1;
	}
	return 0;

usage:
	fprintf(stderr,
		"usage: getconf [-v specification] variable [pathname]\n"
		"       getconf [-v specification] -a [pathname]\n");
	return 2;
}
