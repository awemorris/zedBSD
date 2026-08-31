/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland builtins component.
 */

#include "userland/base/sh/builtins.h"

#include <zedbsd/console.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define COPY_BUFFER_SIZE 512U
#define LS_INITIAL_CAPACITY 16U
#define PATH_BUFFER_SIZE 256U

struct hash_entry {
	struct hash_entry *next;
	char *name;
	char *path;
};

static struct hash_entry *hash_entries;
static char *hash_path;

struct ls_entry {
	char name[256];
	uint8_t type;
	struct stat status;
	int status_valid;
};

struct ls_options {
	int all;
	int human;
	int long_format;
};

struct ulimit_kind {
	char option;
	int resource;
	rlim_t scale;
	const char *label;
};

static const struct ulimit_kind ulimit_kinds[] = {
    {'c', RLIMIT_CORE, 512U, "core file size (blocks)"},
    {'d', RLIMIT_DATA, 1024U, "data segment size (kbytes)"},
    {'f', RLIMIT_FSIZE, 512U, "file size (blocks)"},
    {'n', RLIMIT_NOFILE, 1U, "open files"},
    {'s', RLIMIT_STACK, 1024U, "stack size (kbytes)"},
    {'t', RLIMIT_CPU, 1U, "cpu time (seconds)"},
    {'v', RLIMIT_AS, 1024U, "address space (kbytes)"},
};

struct ls_widths {
	size_t links;
	size_t user;
	size_t group;
	size_t size;
};

static int builtin_echo(int argc, char **argv);
static int builtin_printf(int argc, char **argv);
static int printf_escape(const char **cursor, int *stop);
static int builtin_test(int argc, char **argv);
static int test_integer(const char *left, const char *operation, const char *right, int *valid);
static int builtin_pwd(int argc, char **argv);
static int builtin_cd(int argc, char **argv);
static int builtin_cat(int argc, char **argv);
static int write_all(int descriptor, const void *buffer, size_t length);
static int builtin_ls(int argc, char **argv);
static void print_long_entries(const char *directory, const struct ls_entry *entries, size_t count, const struct ls_options *options, int show_total);
static void long_widths(const struct ls_entry *entries, size_t count, const struct ls_options *options, struct ls_widths *widths);
static void human_size(off_t value, char result[24]);
static const char *user_name(uid_t id, char result[24]);
static const char *group_name(gid_t id, char result[24]);
static void print_long_entry(const char *directory, const struct ls_entry *entry, const struct ls_options *options, const struct ls_widths *widths);
static void mode_text(mode_t mode, char result[11]);
static char type_character(mode_t mode);
static void long_time_text(time_t value, char result[32]);
static int leap_year(long long year);
static int join_path(const char *directory, const char *name, char *result, size_t capacity);
static void print_entry_name(const struct ls_entry *entry);
static int entry_is_directory(const struct ls_entry *entry);
static int load_directory(const char *path, const struct ls_options *options, struct ls_entry **result, size_t *result_count);
static void sort_entries(struct ls_entry *entries, size_t count);
static void print_column_entries(const struct ls_entry *entries, size_t count);
static size_t entry_display_length(const struct ls_entry *entry);
static int builtin_cp(int argc, char **argv);
static const char *path_basename(const char *path);
static int builtin_stat(int argc, char **argv);
static const char *type_name(mode_t mode);
static int builtin_touch(int argc, char **argv);
static int builtin_ulimit(int argc, char **argv);
static const struct ulimit_kind *ulimit_kind_find(char option);
static void ulimit_print(rlim_t value, rlim_t scale);
static int ulimit_parse(const char *text, rlim_t scale, rlim_t *result);

/*
 * Implements the sh hash clear operation.
 */
void
sh_hash_clear(
	void)
{
	struct hash_entry *entry;

	/* Continue while the operation condition remains true. */
	while ((entry = hash_entries) != NULL) {
		hash_entries = entry->next;
		free(entry->name);
		free(entry->path);
		free(entry);
	}
}

/*
 * Implements the sh hash sync path operation.
 */
int
sh_hash_sync_path(
	const char *path)
{
	char *copy;

	/* Handles the path availability. */
	if (path == NULL)
		path = "/bin:/usr/bin";

	/* Handles the hash path availability. */
	if (hash_path != NULL && strcmp(hash_path, path) == 0)
		return 0;
	copy = strdup(path);

	/* Handles the copy availability. */
	if (copy == NULL)
		return -1;
	sh_hash_clear();
	free(hash_path);
	hash_path = copy;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sh hash lookup operation.
 */
const char *
sh_hash_lookup(
	const char *name)
{
	struct hash_entry *entry;

	/* Process each linked entry. */
	for (entry = hash_entries; entry != NULL; entry = entry->next)

		/* Selects the matching value. */
		if (strcmp(entry->name, name) == 0)
			return entry->path;

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Implements the sh hash store operation.
 */
int
sh_hash_store(
	const char *name,
	const char *path)
{
	struct hash_entry *entry;
	char *copy;

	/* Process each linked entry. */
	for (entry = hash_entries; entry != NULL; entry = entry->next)

		/* Selects the matching value. */
		if (strcmp(entry->name, name) == 0) {
			copy = strdup(path);

			/* Handles the copy availability. */
			if (copy == NULL)
				return -1;
			free(entry->path);
			entry->path = copy;

			/* Reports successful completion. */
			return 0;
		}
	entry = calloc(1, sizeof(*entry));

	/* Handles the entry availability. */
	if (entry == NULL)
		return -1;
	entry->name = strdup(name);
	entry->path = strdup(path);

	/* Handles the name availability. */
	if (entry->name == NULL || entry->path == NULL) {
		free(entry->name);
		free(entry->path);
		free(entry);

		/* Reports operation failure. */
		return -1;
	}
	entry->next = hash_entries;
	hash_entries = entry;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sh hash print operation.
 */
void
sh_hash_print(
	void)
{
	struct hash_entry *entry;

	/* Process each linked entry. */
	for (entry = hash_entries; entry != NULL; entry = entry->next)
		printf("%s=%s\n", entry->name, entry->path);
}

/*
 * Implements the sh builtin dispatch operation.
 */
int
sh_builtin_dispatch(
	int argc,
	char **argv,
	int *handled)
{
	int function_result;

	*handled = 1;
	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "echo")) {
		/* Obtains the builtin echo result. */
		function_result = builtin_echo(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "printf")) {
		/* Obtains the builtin printf result. */
		function_result = builtin_printf(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "test") || !strcmp(argv[0], "[")) {
		/* Obtains the builtin test result. */
		function_result = builtin_test(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "pwd")) {
		/* Obtains the builtin pwd result. */
		function_result = builtin_pwd(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "cd")) {
		/* Obtains the builtin cd result. */
		function_result = builtin_cd(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "cat")) {
		/* Obtains the builtin cat result. */
		function_result = builtin_cat(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "ls")) {
		/* Obtains the builtin ls result. */
		function_result = builtin_ls(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "cp")) {
		/* Obtains the builtin cp result. */
		function_result = builtin_cp(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "stat")) {
		/* Obtains the builtin stat result. */
		function_result = builtin_stat(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "touch")) {
		/* Obtains the builtin touch result. */
		function_result = builtin_touch(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "ulimit")) {
		/* Obtains the builtin ulimit result. */
		function_result = builtin_ulimit(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "clear")) {
		/* Validates the command-line arguments. */
		if (argc != 1) {
			fprintf(stderr, "usage: clear\n");

			/* Reports successful completion. */
			return 0;
		}

		/* Computes the function result. */
		function_result = ioctl(1, ZEDBSD_CONSOLE_CLEAR) == 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "true"))
		return 1;

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "false"))
		return 0;
	*handled = 0;
	/* Reports successful completion. */
	return 0;
}

/* Supports the builtin echo operation. */
static int
builtin_echo(
	int argc,
	char **argv)
{
	int index;

	/* Process each remaining command-line operand. */
	for (index = 1; index < argc; index++)
		printf("%s%s", index == 1 ? "" : " ", argv[index]);
	putchar('\n');

	/* Reports operation failure. */
	return 1;
}

/* Supports the builtin printf operation. */
static int
builtin_printf(
	int argc,
	char **argv)
{
	int function_result;
	int value_local;
	const char *value_local1;
	int byte;
	const char *bytes;
	char *end;
	long number;
	const char *cursor;
	int stop;
	int argument_before;
	const char *format;
	int argument;

	argument = 2;

	/* Validates the command-line arguments. */
	if (argc < 2) {
		fprintf(stderr, "usage: printf FORMAT [ARGUMENT...]\n");

		/* Reports successful completion. */
		return 0;
	}
	format = argv[1];
	do {
		/* Continue while the operation condition remains true. */
		cursor = format;
		stop = 0;
		argument_before = argument;
		while (*cursor != '\0' && !stop) {
			/* Checks the current cursor position. */
			if (*cursor == '\\') {
				cursor++;

				value_local = printf_escape(&cursor, &stop);

				/* Handles the stop condition. */
				if (!stop)
					putchar(value_local);
				continue;
			}

			/* Checks the current cursor position. */
			if (*cursor != '%') {
				putchar((unsigned char)*cursor++);
				continue;
			}
			cursor++;

			/* Checks the current cursor position. */
			if (*cursor == '%') {
				putchar('%');
				cursor++;
				continue;
			}
							value_local1 = argument < argc ? argv[argument++] : "";

			/* Dispatch the selected operation case. */
			switch (*cursor++) {
			case 's':
				printf("%s", value_local1);
				break;
			case 'b':

			/* Continue while the operation condition remains true. */
			bytes = value_local1;
			while (*bytes != '\0' && !stop) {
				/* Handles the bytes condition. */
				if (*bytes == '\\') {
					bytes++;

					byte = printf_escape(
						&bytes,
						&stop);

					/* Handles the stop condition. */
					if (!stop)
						putchar(
						    byte);
				} else
					putchar((
					    unsigned char)*bytes++);
			}
			break;
			case 'c':
				/* Handles the value local1 condition. */
				if (*value_local1 != '\0')
					putchar((unsigned char)*value_local1);
				break;
			case 'd':
			case 'i':
				number = strtol(value_local1, &end, 0);

				/* Handles the value local1 condition. */
				if (*value_local1 == '\0' || *end != '\0')
					return 0;
				printf("%ld", number);
				break;
			case 'u':
				number = strtol(value_local1, &end, 0);

				/* Handles the value local1 condition. */
				if (*value_local1 == '\0' || *end != '\0')
					return 0;
				printf("%lu", (unsigned long)number);
				break;
			case 'o':
				number = strtol(value_local1, &end, 0);

				/* Handles the value local1 condition. */
				if (*value_local1 == '\0' || *end != '\0')
					return 0;
				printf("%lo", (unsigned long)number);
				break;
			case 'x':
				number = strtol(value_local1, &end, 0);

				/* Handles the value local1 condition. */
				if (*value_local1 == '\0' || *end != '\0')
					return 0;
				printf("%lx", (unsigned long)number);
				break;
			default:
				fprintf(
				    stderr,
				    "printf: unsupported conversion\n");

				/* Reports successful completion. */
				return 0;
			}
		}

		/* Handles the stop condition. */
		if (stop)
			break;

		/* Handles the argument condition. */
		if (argument == argument_before)
			break;
	} while (argument < argc);

	/* Computes the function result. */
	function_result = ferror(stdout) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the printf escape operation. */
static int
printf_escape(
	const char **cursor,
	int *stop)
{
	int count;
	const char *text;
	int value;

	text = *cursor;

	/* Validates the current text. */
	if (*text == '\0')
		return '\\';

	/* Dispatch the selected operation case. */
	switch (*text++) {
	case 'a':
		value = '\a';
		break;
	case 'b':
		value = '\b';
		break;
	case 'c':
		*stop = 1;
		value = 0;
		break;
	case 'f':
		value = '\f';
		break;
	case 'n':
		value = '\n';
		break;
	case 'r':
		value = '\r';
		break;
	case 't':
		value = '\t';
		break;
	case 'v':
		value = '\v';
		break;
	case '\\':
		value = '\\';
		break;
	case '0':

	/* Process each remaining element. */
	count = 0;
	value = 0;
	while (count < 3 && *text >= '0' && *text <= '7') {
		value = value * 8 + (*text++ - '0');
		count++;
	}
	break;
	default:
		putchar('\\');
		value = (unsigned char)text[-1];
		break;
	}
	*cursor = text;
	/* Returns the computed result. */
	return value;
}

/* Supports the builtin test operation. */
static int
builtin_test(
	int argc,
	char **argv)
{
	int function_result;
	int result;
	struct stat status;
	int valid;
	char *nested[3];

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "[")) {
		/* Handles the selected command-line operation. */
		if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
			fprintf(stderr, "[: missing ]\n");

			/* Reports successful completion. */
			return 0;
		}
		argc--;
	}
	argc--;
	argv++;

	/* Validates the command-line arguments. */
	if (argc == 0)
		return 0;

	/* Validates the command-line arguments. */
	if (argc == 1)
		return argv[0][0] != '\0';

	/* Validates the command-line arguments. */
	if (argc == 2) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[0], "!"))
			return argv[1][0] == '\0';

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[0], "-n"))
			return argv[1][0] != '\0';

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[0], "-z"))
			return argv[1][0] == '\0';

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[0], "-e")) {
			/* Computes the function result. */
			function_result = stat(argv[1], &status) == 0;

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[0], "-f")) {
			/* Computes the function result. */
			function_result = stat(argv[1], &status) == 0 &&
			       S_ISREG(status.st_mode);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[0], "-d")) {
			/* Computes the function result. */
			function_result = stat(argv[1], &status) == 0 &&
			       S_ISDIR(status.st_mode);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[0], "-r")) {
			/* Computes the function result. */
			function_result = access(argv[1], R_OK) == 0;

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[0], "-w")) {
			/* Computes the function result. */
			function_result = access(argv[1], W_OK) == 0;

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[0], "-x")) {
			/* Computes the function result. */
			function_result = access(argv[1], X_OK) == 0;

			/* Returns the computed result. */
			return function_result;
		}

		/* Reports successful completion. */
		return 0;
	}

	/* Validates the command-line arguments. */
	if (argc == 3) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[0], "!")) {
			nested[0] = "test";
			nested[1] = argv[1];
			nested[2] = argv[2];

			/* Computes the function result. */
			function_result = !builtin_test(3, nested);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[1], "=")) {
			/* Computes the function result. */
			function_result = strcmp(argv[0], argv[2]) == 0;

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the selected command-line operation. */
		if (!strcmp(argv[1], "!=")) {
			/* Computes the function result. */
			function_result = strcmp(argv[0], argv[2]) != 0;

			/* Returns the computed result. */
			return function_result;
		}
		result = test_integer(argv[0], argv[1], argv[2], &valid);

		/* Handles the valid condition. */
		if (valid)
			return result;
	}
	fprintf(stderr, "test: invalid expression\n");

	/* Reports successful completion. */
	return 0;
}

/* Supports the test integer operation. */
static int
test_integer(
	const char *left,
	const char *operation,
	const char *right,
	int *valid)
{
	char *end;
	long a;
	long b;

	a = strtol(left, &end, 10);

	/* Handles the left condition. */
	if (*left == '\0' || *end != '\0') {
		*valid = 0;
		/* Reports successful completion. */
		return 0;
	}
	b = strtol(right, &end, 10);

	/* Handles the right condition. */
	if (*right == '\0' || *end != '\0') {
		*valid = 0;
		/* Reports successful completion. */
		return 0;
	}
	*valid = 1;
	/* Selects the matching value. */
	if (!strcmp(operation, "-eq"))
		return a == b;

	/* Selects the matching value. */
	if (!strcmp(operation, "-ne"))
		return a != b;

	/* Selects the matching value. */
	if (!strcmp(operation, "-lt"))
		return a < b;

	/* Selects the matching value. */
	if (!strcmp(operation, "-le"))
		return a <= b;

	/* Selects the matching value. */
	if (!strcmp(operation, "-gt"))
		return a > b;

	/* Selects the matching value. */
	if (!strcmp(operation, "-ge"))
		return a >= b;
	*valid = 0;
	/* Reports successful completion. */
	return 0;
}

/* Supports the builtin pwd operation. */
static int
builtin_pwd(
	int argc,
	char **argv)
{
	char path[PATH_BUFFER_SIZE];

	(void)argv;

	/* Validates the command-line arguments. */
	if (argc != 1) {
		fprintf(stderr, "usage: pwd\n");

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed getcwd operation. */
	if (getcwd(path, sizeof(path)) == NULL) {
		fprintf(stderr, "pwd: %s\n", strerror(errno));

		/* Reports successful completion. */
		return 0;
	}
	puts(path);

	/* Reports operation failure. */
	return 1;
}

/* Supports the builtin cd operation. */
static int
builtin_cd(
	int argc,
	char **argv)
{
	const char *path;

	/* Validates the command-line arguments. */
	if (argc > 2) {
		fprintf(stderr, "usage: cd [DIRECTORY]\n");

		/* Reports successful completion. */
		return 0;
	}
	path = argc == 2 ? argv[1] : getenv("HOME");

	/* Handles the path availability. */
	if (path == NULL || path[0] == '\0')
		path = "/";

	/* Handles a failed chdir operation. */
	if (chdir(path) != 0) {
		fprintf(stderr, "cd: %s: %s\n", path, strerror(errno));

		/* Reports successful completion. */
		return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the builtin cat operation. */
static int
builtin_cat(
	int argc,
	char **argv)
{
	unsigned char buffer[COPY_BUFFER_SIZE];
	ssize_t count;
	int descriptor;
	int argument;

	/* Validates the command-line arguments. */
	if (argc < 2) {
		fprintf(stderr, "usage: cat FILE...\n");

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining command-line operand. */
	for (argument = 1; argument < argc; argument++) {

				descriptor = open(argv[argument], O_RDONLY);

		/* Checks the file descriptor. */
		if (descriptor < 0) {
			fprintf(stderr, "cat: %s: %s\n", argv[argument],
				strerror(errno));

			/* Reports successful completion. */
			return 0;
		}
		while ((count = read(descriptor, buffer, sizeof(buffer))) > 0) {
			/* Handles a failed write all operation. */
			if (!write_all(1, buffer, (size_t)count)) {
				fprintf(stderr, "cat: write: %s\n",
					strerror(errno));
				(void)close(descriptor);

				/* Reports successful completion. */
				return 0;
			}
		}

		/* Checks the remaining item count. */
		if (count < 0) {
			fprintf(stderr, "cat: %s: %s\n", argv[argument],
				strerror(errno));
			(void)close(descriptor);

			/* Reports successful completion. */
			return 0;
		}

		/* Handles a failed close operation. */
		if (close(descriptor) != 0) {
			fprintf(stderr, "cat: %s: %s\n", argv[argument],
				strerror(errno));

			/* Reports successful completion. */
			return 0;
		}
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the write all operation. */
static int
write_all(
	int descriptor,
	const void *buffer,
	size_t length)
{
	ssize_t count;
	const unsigned char *bytes;

	/* Process each remaining element. */
	bytes = buffer;
	while (length != 0) {

		count = write(descriptor, bytes, length);

		/* Checks the remaining item count. */
		if (count <= 0)
			return 0;
		bytes += count;
		length -= (size_t)count;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the builtin ls operation. */
static int
builtin_ls(
	int argc,
	char **argv)
{
	const char *option;
	struct ls_entry single;
	const char *path;
	struct ls_options options = {0, 0, 0};
	int path_set, argument;
	struct stat status;
	struct ls_entry *entries;
	size_t count;

	/* Process each remaining command-line operand. */
	path = ".";
	path_set = 0;
	for (argument = 1; argument < argc; argument++) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[argument], "--")) {
			argument++;
			break;
		}

		/* Validates the command-line arguments. */
		if (argv[argument][0] != '-' || argv[argument][1] == '\0')
			break;

		/* Process each remaining command-line operand. */
		for (option = argv[argument] + 1; *option != '\0'; option++) {
			/* Dispatch the selected command-line option. */
			switch (*option) {
			case 'a':
				options.all = 1;
				break;
			case 'h':
				options.human = 1;
				break;
			case 'l':
				options.long_format = 1;
				break;
			default:
				fprintf(
				    stderr,
				    "ls: invalid option -- '%c'\n"
				    "Try 'ls --help' for more information.\n",
				    *option);
				/* Reports successful completion. */
				return 0;
			}
		}
	}

	/* Process each remaining command-line operand. */
	for (; argument < argc; argument++) {
		/* Handles the path set condition. */
		if (!path_set) {
			path = argv[argument];
			path_set = 1;
		} else {
			fprintf(stderr, "usage: ls [-ahl] [PATH]\n");

			/* Reports successful completion. */
			return 0;
		}
	}

	/* Handles a failed lstat operation. */
	if (lstat(path, &status) != 0) {
		fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed S ISDIR operation. */
	if (!S_ISDIR(status.st_mode)) {

		memset(&single, 0, sizeof(single));
		strncpy(single.name, path, sizeof(single.name) - 1U);
		single.status = status;
		single.status_valid = 1;

		/* Checks the selected options. */
		if (options.long_format)
			print_long_entries("", &single, 1, &options, 0);
		else {
			print_entry_name(&single);
			putchar('\n');
		}

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed load directory operation. */
	if (!load_directory(path, &options, &entries, &count)) {
		fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the selected options. */
	if (options.long_format)
		print_long_entries(path, entries, count, &options, 1);
	else
		print_column_entries(entries, count);
	free(entries);

	/* Reports operation failure. */
	return 1;
}

/* Supports the print long entries operation. */
static void
print_long_entries(
	const char *directory,
	const struct ls_entry *entries,
	size_t count,
	const struct ls_options *options,
	int show_total)
{
	char total[24];
	struct ls_widths widths;
	unsigned long long blocks;
	size_t index;

	blocks = 0;

	long_widths(entries, count, options, &widths);

	/* Handles the show total condition. */
	if (show_total) {
		/* Process each remaining element. */
		for (index = 0; index < count; index++)

			/* Handles the entries condition. */
			if (entries[index].status_valid &&
			    entries[index].status.st_blocks > 0)
				blocks += (unsigned long long)entries[index]
					      .status.st_blocks;

		/* Checks the selected options. */
		if (options->human)
			human_size((off_t)(blocks * 512ULL), total);
		else
			snprintf(total, sizeof(total), "%llu",
				 (blocks + 1ULL) / 2ULL);
		printf("total %s\n", total);
	}

	/* Process each remaining element. */
	for (index = 0; index < count; index++)
		print_long_entry(directory, &entries[index], options, &widths);
}

/* Supports the long widths operation. */
static void
long_widths(
	const struct ls_entry *entries,
	size_t count,
	const struct ls_options *options,
	struct ls_widths *widths)
{
	char links[24], size[24], user_buffer[24], group_buffer[24];
	const char *user, *group;
	size_t index;

	memset(widths, 0, sizeof(*widths));

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Handles the entries condition. */
		if (!entries[index].status_valid)
			continue;
		snprintf(links, sizeof(links), "%lu",
			 (unsigned long)entries[index].status.st_nlink);

		/* Checks the selected options. */
		if (options->human)
			human_size(entries[index].status.st_size, size);
		else
			snprintf(size, sizeof(size), "%lld",
				 (long long)entries[index].status.st_size);
		user = user_name(entries[index].status.st_uid, user_buffer);
		group = group_name(entries[index].status.st_gid, group_buffer);

		/* Handles a failed strlen operation. */
		if (strlen(links) > widths->links)
			widths->links = strlen(links);

		/* Handles a failed strlen operation. */
		if (strlen(user) > widths->user)
			widths->user = strlen(user);

		/* Handles a failed strlen operation. */
		if (strlen(group) > widths->group)
			widths->group = strlen(group);

		/* Handles a failed strlen operation. */
		if (strlen(size) > widths->size)
			widths->size = strlen(size);
	}
}

/* Supports the human size operation. */
static void
human_size(
	off_t value,
	char result[24])
{
	unsigned long long whole;
	unsigned long long tenth;
	static const char suffixes[] = "BKMGTPE";
	unsigned unit;
	unsigned long long magnitude, scale;

	unit = 0;
	scale = 1;

	/* Validates the current value. */
	if (value < 0) {
		snprintf(result, 24, "%lld", (long long)value);

		/* Returns the computed result. */
		return;
	}

	/* Process each remaining element. */
	magnitude = (unsigned long long)value;
	while (unit + 1U < sizeof(suffixes) - 1U &&
	       magnitude >= scale * 1024ULL) {
		scale *= 1024ULL;
		unit++;
	}

	/* Handles the unit condition. */
	if (unit == 0) {
		snprintf(result, 24, "%llu", magnitude);

		/* Returns the computed result. */
		return;
	}

	/* Handles the magnitude condition. */
	if (magnitude / scale < 10ULL) {
				whole = magnitude / scale;
				tenth = ((magnitude % scale) * 10ULL + scale / 2ULL) / scale;

		/* Handles the tenth condition. */
		if (tenth == 10ULL) {
			whole++;
			tenth = 0;
		}
		snprintf(result, 24, "%llu.%llu%c", whole, tenth,
			 suffixes[unit]);
	} else {
		snprintf(result, 24, "%llu%c",
			 (magnitude + scale / 2ULL) / scale, suffixes[unit]);
	}
}

/* Supports the user name operation. */
static const char *
user_name(
	uid_t id,
	char result[24])
{
	struct passwd entry, *found;
	char buffer[512];

	found = NULL;

	/* Handles a failed getpwuid r operation. */
	if (getpwuid_r(id, &entry, buffer, sizeof(buffer), &found) == 0 &&
	    found != NULL && found->pw_name != NULL) {
		snprintf(result, 24, "%s", found->pw_name);

		/* Returns the computed result. */
		return result;
	}
	snprintf(result, 24, "%u", (unsigned)id);

	/* Returns the computed result. */
	return result;
}

/* Supports the group name operation. */
static const char *
group_name(
	gid_t id,
	char result[24])
{
	struct group entry, *found;
	char buffer[512];

	found = NULL;

	/* Handles a failed getgrgid r operation. */
	if (getgrgid_r(id, &entry, buffer, sizeof(buffer), &found) == 0 &&
	    found != NULL && found->gr_name != NULL) {
		snprintf(result, 24, "%s", found->gr_name);

		/* Returns the computed result. */
		return result;
	}
	snprintf(result, 24, "%u", (unsigned)id);

	/* Returns the computed result. */
	return result;
}

/* Supports the print long entry operation. */
static void
print_long_entry(
	const char *directory,
	const struct ls_entry *entry,
	const struct ls_options *options,
	const struct ls_widths *widths)
{
	char target[PATH_BUFFER_SIZE];
	ssize_t length;
	char mode[11], size[24], when[32], user_buffer[24], group_buffer[24];
	char path[PATH_BUFFER_SIZE];
	const char *user, *group;

	/* Handles the entry condition. */
	if (!entry->status_valid) {
		printf("?????????? %*s %-*s %-*s %*s %12s %s\n",
		       (int)widths->links, "?", (int)widths->user, "?",
		       (int)widths->group, "?", (int)widths->size, "?", "?",
		       entry->name);

		/* Returns the computed result. */
		return;
	}
	mode_text(entry->status.st_mode, mode);

	/* Checks the selected options. */
	if (options->human)
		human_size(entry->status.st_size, size);
	else
		snprintf(size, sizeof(size), "%lld",
			 (long long)entry->status.st_size);
	long_time_text(entry->status.st_mtime, when);
	user = user_name(entry->status.st_uid, user_buffer);
	group = group_name(entry->status.st_gid, group_buffer);
	printf("%s %*lu %-*s %-*s %*s %s %s", mode, (int)widths->links,
	       (unsigned long)entry->status.st_nlink, (int)widths->user, user,
	       (int)widths->group, group, (int)widths->size, size, when,
	       entry->name);

	/* Handles a failed S ISLNK operation. */
	if (S_ISLNK(entry->status.st_mode) &&
	    join_path(directory, entry->name, path, sizeof(path))) {

				length = readlink(path, target, sizeof(target) - 1U);

		/* Checks the current data length. */
		if (length >= 0) {
			target[length] = '\0';
			printf(" -> %s", target);
		}
	}
	putchar('\n');
}

/* Supports the mode text operation. */
static void
mode_text(
	mode_t mode,
	char result[11])
{
	static const mode_t bits[9] = {S_IRUSR, S_IWUSR, S_IXUSR,
				       S_IRGRP, S_IWGRP, S_IXGRP,
				       S_IROTH, S_IWOTH, S_IXOTH};
	static const char letters[3] = {'r', 'w', 'x'};
	unsigned index;

	/* Process each remaining element. */
	result[0] = type_character(mode);
	for (index = 0; index < 9; index++)
		result[index + 1U] =
		    mode & bits[index] ? letters[index % 3U] : '-';
	result[10] = '\0';
}

/* Supports the type character operation. */
static char
type_character(
	mode_t mode)
{
	/* Validates the selected mode. */
	if (S_ISDIR(mode))
		return 'd';

	/* Validates the selected mode. */
	if (S_ISCHR(mode))
		return 'c';

	/* Validates the selected mode. */
	if (S_ISBLK(mode))
		return 'b';

	/* Validates the selected mode. */
	if (S_ISFIFO(mode))
		return 'p';

	/* Validates the selected mode. */
	if (S_ISLNK(mode))
		return 'l';

	/* Validates the selected mode. */
	if (S_ISSOCK(mode))
		return 's';

	/* Returns the computed result. */
	return '-';
}

/* Supports the long time text operation. */
static void
long_time_text(
	time_t value,
	char result[32])
{
	static const int month_days[] = {31, 28, 31, 30, 31, 30,
					 31, 31, 30, 31, 30, 31};
	static const char *const month_names[] = {"Jan", "Feb", "Mar", "Apr",
						  "May", "Jun", "Jul", "Aug",
						  "Sep", "Oct", "Nov", "Dec"};
	long long days;
	long long seconds;
	long long year;
	time_t now;
	int month;

	days = value / 86400;
	seconds = value % 86400;
	year = 1970;
	now = time(NULL);
	month = 0;

	/* Handles the seconds condition. */
	if (seconds < 0) {
		seconds += 86400;
		days--;
	}
	while (days >= 365 + leap_year(year)) {
		days -= 365 + leap_year(year);
		year++;
	}
	while (days < 0) {
		year--;
		days += 365 + leap_year(year);
	}
	while (month < 11 &&
	       days >= month_days[month] + (month == 1 && leap_year(year))) {
		days -= month_days[month] + (month == 1 && leap_year(year));
		month++;
	}

	/* Handles the now condition. */
	if (now != (time_t)-1 && (value < now - 15552000 || value > now + 3600))
		snprintf(result, 32, "%s %2d  %4lld", month_names[month],
			 (int)days + 1, year);
	else
		snprintf(result, 32, "%s %2d %02lld:%02lld", month_names[month],
			 (int)days + 1, seconds / 3600, (seconds / 60) % 60);
}

/* Supports the leap year operation. */
static int
leap_year(
	long long year)
{
	/* Returns the computed result. */
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/* Supports the join path operation. */
static int
join_path(
	const char *directory,
	const char *name,
	char *result,
	size_t capacity)
{
	size_t directory_length = strlen(directory);
	size_t name_length = strlen(name);
	int slash =
	    directory_length != 0 && directory[directory_length - 1U] != '/';

	/* Handles the directory length condition. */
	if (directory_length + (size_t)slash + name_length + 1U > capacity)
		return 0;
	memcpy(result, directory, directory_length);

	/* Handles the slash condition. */
	if (slash)
		result[directory_length++] = '/';
	memcpy(result + directory_length, name, name_length + 1U);

	/* Reports operation failure. */
	return 1;
}

/* Supports the print entry name operation. */
static void
print_entry_name(
	const struct ls_entry *entry)
{
	printf("%s%s", entry->name, entry_is_directory(entry) ? "/" : "");
}

/* Supports the entry is directory operation. */
static int
entry_is_directory(
	const struct ls_entry *entry)
{
	int function_result;

	/* Computes the function result. */
	function_result = entry->status_valid ? S_ISDIR(entry->status.st_mode)
				   : entry->type == DT_DIR;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the load directory operation. */
static int
load_directory(
	const char *path,
	const struct ls_options *options,
	struct ls_entry **result,
	size_t *result_count)
{
	static const char *const dot_names[] = {".", ".."};
	char child_local[PATH_BUFFER_SIZE];
	struct ls_entry *item_local;
	struct ls_entry *item_local2;
	char child_local1[PATH_BUFFER_SIZE];
	unsigned dot;
	struct ls_entry *larger;
	DIR *directory;
	struct ls_entry *entries;
	struct dirent *entry;
	size_t count, capacity;

	directory = opendir(path);
	count = 0;
	capacity = LS_INITIAL_CAPACITY;

	/* Handles the directory availability. */
	if (directory == NULL)
		return 0;
	entries = malloc(capacity * sizeof(*entries));

	/* Handles the entries availability. */
	if (entries == NULL) {
		(void)closedir(directory);

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the selected options. */
	if (options->all) {
		/* Process each element required by the operation. */
		for (dot = 0; dot < 2U; dot++) {

						item_local = &entries[count++];
			strcpy(item_local->name, dot_names[dot]);
			item_local->type = DT_DIR;
			item_local->status_valid =
			    join_path(path, item_local->name, child_local, sizeof(child_local)) &&
			    lstat(child_local, &item_local->status) == 0;
		}
	}
	while ((entry = readdir(directory)) != NULL) {
		/* Checks the selected options. */
		if (!options->all && entry->d_name[0] == '.')
			continue;

		/* Checks the selected options. */
		if (options->all && (!strcmp(entry->d_name, ".") ||
				     !strcmp(entry->d_name, "..")))
			continue;

		/* Checks the remaining item count. */
		if (count == capacity) {
			/* Handles the capacity condition. */
			if (capacity > (size_t)-1 / 2U / sizeof(*entries)) {
				free(entries);
				(void)closedir(directory);
				errno = ENOMEM;

				/* Reports successful completion. */
				return 0;
			}
			capacity *= 2U;
			larger = realloc(entries, capacity * sizeof(*entries));

			/* Handles the larger availability. */
			if (larger == NULL) {
				free(entries);
				(void)closedir(directory);

				/* Reports successful completion. */
				return 0;
			}
			entries = larger;
		}
		item_local2 = &entries[count++];
		strncpy(item_local2->name, entry->d_name, sizeof(item_local2->name) - 1U);
		item_local2->name[sizeof(item_local2->name) - 1U] = '\0';
		item_local2->type = entry->d_type;
		item_local2->status_valid = 0;

		/* Checks the selected options. */
		if (options->long_format) {
			/* Handles a failed join path operation. */
			if (join_path(path, item_local2->name, child_local1, sizeof(child_local1)) &&
			    lstat(child_local1, &item_local2->status) == 0)
				item_local2->status_valid = 1;
		}
	}

	/* Handles a failed closedir operation. */
	if (closedir(directory) != 0) {
		free(entries);

		/* Reports successful completion. */
		return 0;
	}
	sort_entries(entries, count);
	*result = entries;
	*result_count = count;
	/* Reports operation failure. */
	return 1;
}

/* Supports the sort entries operation. */
static void
sort_entries(
	struct ls_entry *entries,
	size_t count)
{
	struct ls_entry current;
	size_t position;
	size_t index;

	/* Process each remaining element. */
	for (index = 1; index < count; index++) {
		/* Continue while the operation condition remains true. */
				current = entries[index];
				position = index;
		while (position != 0 &&
		       strcmp(entries[position - 1U].name, current.name) > 0) {
			entries[position] = entries[position - 1U];
			position--;
		}
		entries[position] = current;
	}
}

/* Supports the print column entries operation. */
static void
print_column_entries(
	const struct ls_entry *entries,
	size_t count)
{
	size_t length_local;
	size_t length_local1, spaces_local;
	size_t index;
	size_t next;
	struct console_size size = {0, 80};
	size_t maximum, column_width, columns, rows, row, column;

	maximum = 0;

	/* Checks the remaining item count. */
	if (count == 0)
		return;
	(void)ioctl(1, ZEDBSD_CONSOLE_GET_SIZE, &size);

	/* Checks the current data size. */
	if (size.columns == 0)

	/* Process each remaining element. */
		size.columns = 80;
	for (row = 0; row < count; row++) {
				length_local = entry_display_length(&entries[row]);

		/* Handles the length local condition. */
		if (length_local > maximum)
			maximum = length_local;
	}
	column_width = maximum + 2U;
	columns = size.columns / column_width;

	/* Handles the columns condition. */
	if (columns == 0)
		columns = 1;

	/* Handles the columns condition. */
	if (columns > count)

	/* Process each element required by the operation. */
		columns = count;
	rows = (count + columns - 1U) / columns;
	for (row = 0; row < rows; row++) {
		/* Process each element required by the operation. */
		for (column = 0; column < columns; column++) {

			index = row + column * rows;
			next = index + rows;

			/* Checks the current index. */
			if (index >= count)
				continue;
			print_entry_name(&entries[index]);
			length_local1 = entry_display_length(&entries[index]);

			/* Handles the column condition. */
			if (column + 1U >= columns || next >= count)
				continue;

			/* Continue while the operation condition remains true. */
			spaces_local = column_width - length_local1;
			while (spaces_local-- != 0)
				putchar(' ');
		}
		putchar('\n');
	}
}

/* Supports the entry display length operation. */
static size_t
entry_display_length(
	const struct ls_entry *entry)
{
	size_t function_result;

	/* Computes the function result. */
	function_result = strlen(entry->name) + (entry_is_directory(entry) ? 1U : 0U);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the builtin cp operation. */
static int
builtin_cp(
	int argc,
	char **argv)
{
	unsigned char buffer[COPY_BUFFER_SIZE];
	char destination_path[PATH_BUFFER_SIZE];
	const char *destination;
	struct stat source_status, destination_status;
	int source, target, success, destination_exists;
	ssize_t count;

	source = -1;
	target = -1;
	success = 0;
	destination_exists = 0;

	/* Validates the command-line arguments. */
	if (argc != 3) {
		fprintf(stderr, "usage: cp SOURCE DESTINATION\n");

		/* Reports successful completion. */
		return 0;
	}
	source = open(argv[1], O_RDONLY);

	/* Handles a failed fstat operation. */
	if (source < 0 || fstat(source, &source_status) != 0) {
		fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));

		/* Handles the source condition. */
		if (source >= 0)
			(void)close(source);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed S ISREG operation. */
	if (!S_ISREG(source_status.st_mode)) {
		fprintf(stderr, "cp: %s: not a regular file\n", argv[1]);
		(void)close(source);

		/* Reports successful completion. */
		return 0;
	}
	destination = argv[2];

	/* Handles a failed stat operation. */
	if (stat(destination, &destination_status) == 0) {
		destination_exists = 1;

		/* Handles the destination status condition. */
		if (S_ISDIR(destination_status.st_mode)) {
			/* Validates the command-line arguments. */
			if (!join_path(destination, path_basename(argv[1]),
				       destination_path,
				       sizeof(destination_path))) {
				fprintf(stderr,
					"cp: destination path is too long\n");
				(void)close(source);

				/* Reports successful completion. */
				return 0;
			}
			destination = destination_path;
			destination_exists =
			    stat(destination, &destination_status) == 0;
		}
	} else if (errno != ENOENT) {
		fprintf(stderr, "cp: %s: %s\n", destination, strerror(errno));
		(void)close(source);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the destination exists condition. */
	if (destination_exists &&
	    source_status.st_dev == destination_status.st_dev &&
	    source_status.st_ino == destination_status.st_ino) {
		fprintf(stderr,
			"cp: source and destination are the same file\n");
		(void)close(source);

		/* Reports successful completion. */
		return 0;
	}

	/*
 * Without rename/unlink syscalls a failed copy may leave a partial
	 * file. */
	target = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0666);

	/* Handles the target condition. */
	if (target < 0) {
		fprintf(stderr, "cp: %s: %s\n", destination, strerror(errno));
		(void)close(source);

		/* Reports successful completion. */
		return 0;
	}
	while ((count = read(source, buffer, sizeof(buffer))) > 0) {
		/* Handles a failed write all operation. */
		if (!write_all(target, buffer, (size_t)count)) {
			fprintf(stderr, "cp: %s: %s\n", destination,
				strerror(errno));
			goto done;
		}
	}

	/* Checks the remaining item count. */
	if (count < 0) {
		fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
		goto done;
	}
	success = 1;
done:

	/* Handles a failed close operation. */
	if (close(target) != 0) {
		fprintf(stderr, "cp: %s: %s\n", destination, strerror(errno));
		success = 0;
	}

	/* Handles a failed close operation. */
	if (close(source) != 0) {
		fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
		success = 0;
	}

	/* Returns the computed result. */
	return success;
}

/* Supports the path basename operation. */
static const char *
path_basename(
	const char *path)
{
	const char *slash;

	slash = strrchr(path, '/');

	/* Returns the computed result. */
	return slash == NULL ? path : slash + 1;
}

/* Supports the builtin stat operation. */
static int
builtin_stat(
	int argc,
	char **argv)
{
	struct stat status;
	int argument;

	/* Validates the command-line arguments. */
	if (argc < 2) {
		fprintf(stderr, "usage: stat PATH...\n");

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining command-line operand. */
	for (argument = 1; argument < argc; argument++) {
		/* Validates the command-line arguments. */
		if (stat(argv[argument], &status) != 0) {
			fprintf(stderr, "stat: %s: %s\n", argv[argument],
				strerror(errno));

			/* Reports successful completion. */
			return 0;
		}
		printf("%s: type=%s mode=%x dev=%u ino=%u links=%u "
		       "uid=%u gid=%u size=%lld\n",
		       argv[argument], type_name(status.st_mode),
		       (unsigned)status.st_mode, (unsigned)status.st_dev,
		       (unsigned)status.st_ino, (unsigned)status.st_nlink,
		       (unsigned)status.st_uid, (unsigned)status.st_gid,
		       (long long)status.st_size);
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the type name operation. */
static const char *
type_name(
	mode_t mode)
{
	/* Validates the selected mode. */
	if (S_ISREG(mode))
		return "regular";

	/* Validates the selected mode. */
	if (S_ISDIR(mode))
		return "directory";

	/* Validates the selected mode. */
	if (S_ISCHR(mode))
		return "character";

	/* Validates the selected mode. */
	if (S_ISBLK(mode))
		return "block";

	/* Validates the selected mode. */
	if (S_ISFIFO(mode))
		return "fifo";

	/* Validates the selected mode. */
	if (S_ISLNK(mode))
		return "symlink";

	/* Validates the selected mode. */
	if (S_ISSOCK(mode))
		return "socket";

	/* Returns the computed result. */
	return "unknown";
}

/* Supports the builtin touch operation. */
static int
builtin_touch(
	int argc,
	char **argv)
{
	int descriptor;
	int argument;

	/* Validates the command-line arguments. */
	if (argc < 2) {
		fprintf(stderr, "usage: touch FILE...\n");

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining command-line operand. */
	for (argument = 1; argument < argc; argument++) {
				descriptor = open(argv[argument], O_WRONLY | O_CREAT, 0666);

		/* Handles a failed close operation. */
		if (descriptor < 0 || close(descriptor) != 0) {
			fprintf(stderr, "touch: %s: %s\n", argv[argument],
				strerror(errno));

			/* Checks the file descriptor. */
			if (descriptor >= 0)
				(void)close(descriptor);

			/* Reports successful completion. */
			return 0;
		}
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the builtin ulimit operation. */
static int
builtin_ulimit(
	int argc,
	char **argv)
{
	struct rlimit limit_local;
	struct rlimit limit_local1;
	struct rlimit limit_local2;
	const struct ulimit_kind *selected;
	const char *option;
	size_t kind_index;
	rlim_t value;
	const struct ulimit_kind *kind;
	int hard;
	int soft;
	int all;
	int index;

	kind = ulimit_kind_find('f');
	hard = 0;
	soft = 0;
	all = 0;
	index = 1;

	/* Process each remaining command-line operand. */
	while (index < argc && argv[index][0] == '-' &&
	       argv[index][1] != '\0') {
				option = argv[index] + 1;

		/* Handles the selected command-line operation. */
		if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		}
		while (*option != '\0') {
			/* Handles the option condition. */
			if (*option == 'H')
				hard = 1;
			else if (*option == 'S')
				soft = 1;
			else if (*option == 'a')
				all = 1;
			else if ((selected = ulimit_kind_find(*option)) != NULL)
				kind = selected;
			else {
				fprintf(stderr,
					"ulimit: invalid option -- %c\n",
					*option);
				/* Reports successful completion. */
				return 0;
			}
			option++;
		}
		index++;
	}

	/* Handles the all condition. */
	if (all) {
		/* Validates the command-line arguments. */
		if (index != argc)
			return 0;

		/* Process each remaining element. */
		for (kind_index = 0; kind_index < sizeof(ulimit_kinds) /
						      sizeof(ulimit_kinds[0]);
		     kind_index++) {
			/* Handles a failed getrlimit operation. */
			if (getrlimit(ulimit_kinds[kind_index].resource,
				      &limit_local) != 0) {
				fprintf(stderr, "ulimit: %s\n",
					strerror(errno));

				/* Reports successful completion. */
				return 0;
			}
			printf("-%c: %-30s ", ulimit_kinds[kind_index].option,
			       ulimit_kinds[kind_index].label);
			ulimit_print(hard && !soft ? limit_local.rlim_max
						   : limit_local.rlim_cur,
				     ulimit_kinds[kind_index].scale);
		}

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (index == argc) {
		/* Handles a failed getrlimit operation. */
		if (getrlimit(kind->resource, &limit_local1) != 0) {
			fprintf(stderr, "ulimit: %s\n", strerror(errno));

			/* Reports successful completion. */
			return 0;
		}
		ulimit_print(hard && !soft ? limit_local1.rlim_max : limit_local1.rlim_cur,
			     kind->scale);

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (index + 1 != argc) {
		fprintf(stderr, "usage: ulimit [-HSa] [-cdfnstv] [limit]\n");

		/* Reports successful completion. */
		return 0;
	}

	/* Validates the command-line arguments. */
	if (!ulimit_parse(argv[index], kind->scale, &value)) {
		fprintf(stderr, "ulimit: invalid limit: %s\n",
			argv[index]);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed getrlimit operation. */
	if (getrlimit(kind->resource, &limit_local2) != 0) {
		fprintf(stderr, "ulimit: %s\n", strerror(errno));

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the hard condition. */
	if (!hard && !soft)
		hard = soft = 1;

	/* Handles the hard condition. */
	if (hard)
		limit_local2.rlim_max = value;

	/* Handles the soft condition. */
	if (soft)
		limit_local2.rlim_cur = value;

	/* Handles a failed setrlimit operation. */
	if (setrlimit(kind->resource, &limit_local2) != 0) {
		fprintf(stderr, "ulimit: %s\n", strerror(errno));

		/* Reports successful completion. */
		return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the ulimit kind find operation. */
static const struct ulimit_kind *
ulimit_kind_find(
	char option)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < sizeof(ulimit_kinds) / sizeof(ulimit_kinds[0]);
	     index++)

		/* Handles the ulimit kinds condition. */
		if (ulimit_kinds[index].option == option)
			return &ulimit_kinds[index];

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the ulimit print operation. */
static void
ulimit_print(
	rlim_t value,
	rlim_t scale)
{
	/* Validates the current value. */
	if (value == RLIM_INFINITY)
		puts("unlimited");
	else
		printf("%llu\n", (unsigned long long)(value / scale));
}

/* Supports the ulimit parse operation. */
static int
ulimit_parse(
	const char *text,
	rlim_t scale,
	rlim_t *result)
{
	char *end;
	unsigned long long value;

	/* Selects the matching value. */
	if (strcmp(text, "unlimited") == 0) {
		*result = RLIM_INFINITY;
		/* Reports operation failure. */
		return 1;
	}

	/* Validates the current text. */
	if (*text == '\0' || *text == '-')
		return 0;
	errno = 0;
	value = strtoull(text, &end, 10);

	/* Handles the reported system error. */
	if (errno == ERANGE || *end != '\0' ||
	    value > (unsigned long long)RLIM_INFINITY / scale)

		/* Reports successful completion. */
		return 0;
	*result = (rlim_t)value * scale;
	/* Reports operation failure. */
	return 1;
}
