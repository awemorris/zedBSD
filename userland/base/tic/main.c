/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD tic userland command.
 */

#include "userland/base/common/terminfo.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENTRY_CAPACITY 32768U
#define ALIAS_CAPACITY 32U

static int compile_stream(FILE *stream, const char *pathname, const char *directory);
static char *trim(char *text);
static int compile_entry(const char *pathname, unsigned long line, char *entry, const char *directory);
static char *next_field(char **cursor);
static int name_valid(const char *name);
static int write_entry_file(const char *directory, const char *alias, const char *longname, char *capabilities);

/*
 * Runs the tic command.
 */
int
main(
	int argc,
	char **argv)
{
	FILE *stream;
	const char *directory;
	int option;
	int status;

	directory = "/lib/terminfo";
	status = 0;

	/* Parse each command-line option. */
	while ((option = getopt(argc, argv, "o:")) != -1) {
		/* Handles the option condition. */
		if (option != 'o')
			goto usage;
		directory = optarg;
	}

	/* Validates the command-line arguments. */
	if (optind == argc || *directory == '\0')
		goto usage;

	/* Handles the reported system error. */
	if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "tic: %s: %s\n", directory, strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (; optind < argc; optind++) {
		/* Handles the selected command-line operation. */
		if (strcmp(argv[optind], "-") == 0)
			stream = stdin;
		else
			stream = fopen(argv[optind], "r");

		/* Handles the stream availability. */
		if (stream == NULL) {
			fprintf(stderr, "tic: %s: %s\n", argv[optind],
				strerror(errno));
			status = 1;
			continue;
		}

		/* Validates the command-line arguments. */
		if (compile_stream(stream, argv[optind], directory) != 0) {
			/* Handles the reported system error. */
			if (errno != EINVAL && errno != ENOTSUP)
				fprintf(stderr, "tic: %s: %s\n", argv[optind],
					strerror(errno));
			status = 1;
		}

		/* Handles the end-of-file condition. */
		if (stream != stdin && fclose(stream) == EOF)
			status = 1;
	}

	/* Returns the computed result. */
	return status;

usage:
	fprintf(stderr, "usage: tic [-o directory] source ...\n");

	/* Reports operation failure. */
	return 2;
}

/* Supports the compile stream operation. */
static int
compile_stream(
	FILE *stream,
	const char *pathname,
	const char *directory)
{
	int function_result;
	char *text;
	size_t length;
	int continuation;
	char entry[ENTRY_CAPACITY];
	char line_buffer[2048];
	size_t used;
	unsigned long line;
	unsigned long entry_line;

	used = 0;
	line = 0;
	entry_line = 1;

	/* Process input until it is exhausted. */
	entry[0] = '\0';
	while (fgets(line_buffer, sizeof(line_buffer), stream) != NULL) {

		line++;

		/* Handles a failed strchr operation. */
		if (strchr(line_buffer, '\n') == NULL && !feof(stream)) {
			errno = EOVERFLOW;
			goto invalid;
		}
		continuation = isspace((unsigned char)line_buffer[0]);
		text = trim(line_buffer);

		/* Validates the current text. */
		if (*text == '\0' || *text == '#')
			continue;

		/* Handles the continuation condition. */
		if (!continuation && used != 0) {
			/* Handles a failed compile entry operation. */
			if (compile_entry(pathname, entry_line, entry,
					  directory) != 0)

				/* Reports operation failure. */
				return -1;
			used = 0;
			entry[0] = '\0';
		}

		/* Checks the current capacity usage. */
		if (used == 0)
			entry_line = line;
		length = strlen(text);

		/* Checks the current capacity usage. */
		if (used + length + 1U >= sizeof(entry)) {
			errno = EOVERFLOW;
			goto invalid;
		}
		(void)memcpy(entry + used, text, length + 1U);
		used += length;
	}

	/* Handles an operation failure. */
	if (ferror(stream))
		return -1;

	/* Checks the current capacity usage. */
	if (used != 0) {
		/* Obtains the compile entry result. */
		function_result = compile_entry(pathname, entry_line, entry, directory);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;

invalid:
	fprintf(stderr, "tic: %s:%lu: %s\n", pathname, line, strerror(errno));

	/* Reports operation failure. */
	return -1;
}

/* Supports the trim operation. */
static char *
trim(
	char *text)
{
	char *end;

	/* Continue while the operation condition remains true. */
	while (isspace((unsigned char)*text))
		text++;

	/* Continue while the operation condition remains true. */
	end = text + strlen(text);
	while (end != text && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	/* Returns the computed result. */
	return text;
}

/* Supports the compile entry operation. */
static int
compile_entry(
	const char *pathname,
	unsigned long line,
	char *entry,
	const char *directory)
{
	char copy[ENTRY_CAPACITY];
	size_t index_for;
	char *cursor;
	char *names;
	char *aliases[ALIAS_CAPACITY];
	char *part;
	char *longname;
	size_t count;

	cursor = entry;
	names = trim(next_field(&cursor));
	longname = NULL;
	count = 0;

	/* Continue while the operation condition remains true. */
	while ((part = strsep(&names, "|")) != NULL) {
		part = trim(part);

		/* Handles the part condition. */
		if (*part == '\0')
			continue;

		/* Handles a failed strchr operation. */
		if (strchr(part, ' ') != NULL) {
			longname = part;
			continue;
		}

		/* Handles a failed name valid operation. */
		if (!name_valid(part) || count == ALIAS_CAPACITY) {
			errno = EINVAL;
			goto invalid;
		}
		aliases[count++] = part;
	}

	/* Checks the remaining item count. */
	if (count == 0)
		goto invalid;

	/* Handles the longname availability. */
	if (longname == NULL)

	/* Process each remaining element. */
		longname = aliases[0];
	for (index_for = 0; index_for < count; index_for++) {
		/* Handles a failed strlen operation. */
		if (strlen(cursor) >= sizeof(copy)) {
			errno = EOVERFLOW;
			goto invalid;
		}
		(void)strcpy(copy, cursor);

		/* Handles a failed write entry file operation. */
		if (write_entry_file(directory, aliases[index_for], longname,
				     copy) != 0)
			goto invalid;
	}

	/* Reports successful completion. */
	return 0;

invalid:
	fprintf(stderr, "tic: %s:%lu: invalid entry: %s\n", pathname, line,
		strerror(errno));

	/* Reports operation failure. */
	return -1;
}

/* Supports the next field operation. */
static char *
next_field(
	char **cursor)
{
	char *field;
	char *scan;
	int escaped;

	field = *cursor;
	scan = field;
	escaped = 0;

	/* Continue while the operation condition remains true. */
	while (*scan != '\0') {
		/* Handles the escaped condition. */
		if (!escaped && *scan == ',') {
			*scan = '\0';
			*cursor = scan + 1;
			/* Returns the computed result. */
			return field;
		}

		/* Handles the escaped condition. */
		if (!escaped && *scan == '\\')
			escaped = 1;
		else
			escaped = 0;
		scan++;
	}
	*cursor = scan;
	/* Returns the computed result. */
	return field;
}

/* Supports the name valid operation. */
static int
name_valid(
	const char *name)
{
	/* Validates the current name. */
	if (*name == '\0')
		return 0;

	/* Process each element required by the operation. */
	for (; *name != '\0'; name++)

		/* Handles a failed isalnum operation. */
		if (!isalnum((unsigned char)*name) && *name != '-' &&
		    *name != '_' && *name != '.' && *name != '+')
			/* Reports successful completion. */
			return 0;

	/* Reports operation failure. */
	return 1;
}

/* Supports the write entry file operation. */
static int
write_entry_file(
	const char *directory,
	const char *alias,
	const char *longname,
	char *capabilities)
{
	char *field;
	char *operator;
	const char *kind;
	char temporary[PATH_MAX + 1U];
	char destination[PATH_MAX + 1U];
	char temporary_name[256];
	char *cursor;
	FILE *stream;
	struct terminfo checked;
	int length;

	cursor = capabilities;

	length = snprintf(temporary_name, sizeof(temporary_name), "%s.tmp.%ld",
			  alias, (long)getpid());

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= sizeof(temporary_name))
		goto name_too_long;
	length = snprintf(temporary, sizeof(temporary), "%s/%s.zti", directory,
			  temporary_name);

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= sizeof(temporary))
		goto name_too_long;
	length = snprintf(destination, sizeof(destination), "%s/%s.zti",
			  directory, alias);

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= sizeof(destination))
		goto name_too_long;
	stream = fopen(temporary, "w");

	/* Handles the stream availability. */
	if (stream == NULL)
		return -1;

	/* Handles a failed fprintf operation. */
	if (fprintf(stream, "ZEDTERM 1\nname:longname=%s\n", longname) < 0)
		goto write_error;

	/* Continue while the operation condition remains true. */
	while (*cursor != '\0') {

		field = trim(next_field(&cursor));

		/* Handles the field condition. */
		if (*field == '\0')
			continue;

		/* Selects the matching prefix. */
		if (strncmp(field, "use=", 4) == 0) {
			fprintf(stderr,
				"tic: use= inheritance is not supported; "
				"provide a complete entry\n");
			errno = ENOTSUP;
			goto write_error;
		}
		operator= strpbrk(field, "#=@");

		/* Handles the operator availability. */
		if (operator!= NULL && * operator== '@')
			continue;
		kind = "bool";

		/* Handles the operator availability. */
		if (operator!= NULL) {
			/* Handles the operator condition. */
			if (*operator== '#')
				kind = "num";
			else if (*operator== '=')
				kind = "str";
			*operator++ = '\0';
		}

		/* Handles a failed name valid operation. */
		if (!name_valid(field)) {
			errno = EINVAL;
			goto write_error;
		}

		/* Handles a failed fprintf operation. */
		if (fprintf(stream, "%s:%s=", kind, field) < 0)
			goto write_error;

		/* Handles the operator availability. */
		if (operator== NULL) {
			/* Handles the end-of-file condition. */
			if (fputc('1', stream) == EOF)
				goto write_error;
		} else if (*operator== '\0' || fputs(operator, stream) == EOF)
			goto invalid;

		/* Handles the end-of-file condition. */
		if (fputc('\n', stream) == EOF)
			goto write_error;
	}

	/* Handles the end-of-file condition. */
	if (fflush(stream) == EOF || fsync(fileno(stream)) != 0)
		goto write_error;

	/* Handles the end-of-file condition. */
	if (fclose(stream) == EOF) {
		stream = NULL;
		goto failed;
	}
	stream = NULL;

	/* Handles a failed terminfo load operation. */
	if (terminfo_load(&checked, temporary_name, directory) != 0)
		goto failed;

	/* Handles a failed rename operation. */
	if (rename(temporary, destination) != 0)
		goto failed;

	/* Reports successful completion. */
	return 0;

invalid:
	errno = EINVAL;
write_error:
	(void)fclose(stream);
failed:
	(void)unlink(temporary);

	/* Reports operation failure. */
	return -1;
name_too_long:
	errno = ENAMETOOLONG;

	/* Reports operation failure. */
	return -1;
}
