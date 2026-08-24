/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

static char *
trim(char *text)
{
	char *end;

	while (isspace((unsigned char)*text))
		text++;
	end = text + strlen(text);
	while (end != text && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return text;
}

static int
name_valid(const char *name)
{
	if (*name == '\0')
		return 0;
	for (; *name != '\0'; name++)
		if (!isalnum((unsigned char)*name) && *name != '-' &&
		    *name != '_' && *name != '.' && *name != '+')
			return 0;
	return 1;
}

static char *
next_field(char **cursor)
{
	char *field = *cursor;
	char *scan = field;
	int escaped = 0;

	while (*scan != '\0') {
		if (!escaped && *scan == ',') {
			*scan = '\0';
			*cursor = scan + 1;
			return field;
		}
		if (!escaped && *scan == '\\')
			escaped = 1;
		else
			escaped = 0;
		scan++;
	}
	*cursor = scan;
	return field;
}

static int
write_entry_file(const char *directory, const char *alias, const char *longname,
		 char *capabilities)
{
	char temporary[PATH_MAX + 1U];
	char destination[PATH_MAX + 1U];
	char temporary_name[256];
	char *cursor = capabilities;
	FILE *stream;
	struct terminfo checked;
	int length;

	length = snprintf(temporary_name, sizeof(temporary_name), "%s.tmp.%ld",
			  alias, (long)getpid());
	if (length < 0 || (size_t)length >= sizeof(temporary_name))
		goto name_too_long;
	length = snprintf(temporary, sizeof(temporary), "%s/%s.zti", directory,
			  temporary_name);
	if (length < 0 || (size_t)length >= sizeof(temporary))
		goto name_too_long;
	length = snprintf(destination, sizeof(destination), "%s/%s.zti",
			  directory, alias);
	if (length < 0 || (size_t)length >= sizeof(destination))
		goto name_too_long;
	stream = fopen(temporary, "w");
	if (stream == NULL)
		return -1;
	if (fprintf(stream, "ZEDTERM 1\nname:longname=%s\n", longname) < 0)
		goto write_error;
	while (*cursor != '\0') {
		char *field = trim(next_field(&cursor));
		char *operator;
		const char *kind;

		if (*field == '\0')
			continue;
		if (strncmp(field, "use=", 4) == 0) {
			fprintf(stderr,
				"tic: use= inheritance is not supported; "
				"provide a complete entry\n");
			errno = ENOTSUP;
			goto write_error;
		}
		operator= strpbrk(field, "#=@");
		if (operator!= NULL && * operator== '@')
			continue;
		kind = "bool";
		if (operator!= NULL) {
			if (*operator== '#')
				kind = "num";
			else if (*operator== '=')
				kind = "str";
			*operator++ = '\0';
		}
		if (!name_valid(field)) {
			errno = EINVAL;
			goto write_error;
		}
		if (fprintf(stream, "%s:%s=", kind, field) < 0)
			goto write_error;
		if (operator== NULL) {
			if (fputc('1', stream) == EOF)
				goto write_error;
		} else if (*operator== '\0' || fputs(operator, stream) == EOF)
			goto invalid;
		if (fputc('\n', stream) == EOF)
			goto write_error;
	}
	if (fflush(stream) == EOF || fsync(fileno(stream)) != 0)
		goto write_error;
	if (fclose(stream) == EOF) {
		stream = NULL;
		goto failed;
	}
	stream = NULL;
	if (terminfo_load(&checked, temporary_name, directory) != 0)
		goto failed;
	if (rename(temporary, destination) != 0)
		goto failed;
	return 0;

invalid:
	errno = EINVAL;
write_error:
	(void)fclose(stream);
failed:
	(void)unlink(temporary);
	return -1;
name_too_long:
	errno = ENAMETOOLONG;
	return -1;
}

static int
compile_entry(const char *pathname, unsigned long line, char *entry,
	      const char *directory)
{
	char *cursor = entry;
	char *names = trim(next_field(&cursor));
	char *aliases[ALIAS_CAPACITY];
	char *part;
	char *longname = NULL;
	size_t count = 0;

	while ((part = strsep(&names, "|")) != NULL) {
		part = trim(part);
		if (*part == '\0')
			continue;
		if (strchr(part, ' ') != NULL) {
			longname = part;
			continue;
		}
		if (!name_valid(part) || count == ALIAS_CAPACITY) {
			errno = EINVAL;
			goto invalid;
		}
		aliases[count++] = part;
	}
	if (count == 0)
		goto invalid;
	if (longname == NULL)
		longname = aliases[0];
	for (size_t index = 0; index < count; index++) {
		char copy[ENTRY_CAPACITY];

		if (strlen(cursor) >= sizeof(copy)) {
			errno = EOVERFLOW;
			goto invalid;
		}
		(void)strcpy(copy, cursor);
		if (write_entry_file(directory, aliases[index], longname,
				     copy) != 0)
			goto invalid;
	}
	return 0;

invalid:
	fprintf(stderr, "tic: %s:%lu: invalid entry: %s\n", pathname, line,
		strerror(errno));
	return -1;
}

static int
compile_stream(FILE *stream, const char *pathname, const char *directory)
{
	char entry[ENTRY_CAPACITY];
	char line_buffer[2048];
	size_t used = 0;
	unsigned long line = 0;
	unsigned long entry_line = 1;

	entry[0] = '\0';
	while (fgets(line_buffer, sizeof(line_buffer), stream) != NULL) {
		char *text;
		size_t length;
		int continuation;

		line++;
		if (strchr(line_buffer, '\n') == NULL && !feof(stream)) {
			errno = EOVERFLOW;
			goto invalid;
		}
		continuation = isspace((unsigned char)line_buffer[0]);
		text = trim(line_buffer);
		if (*text == '\0' || *text == '#')
			continue;
		if (!continuation && used != 0) {
			if (compile_entry(pathname, entry_line, entry,
					  directory) != 0)
				return -1;
			used = 0;
			entry[0] = '\0';
		}
		if (used == 0)
			entry_line = line;
		length = strlen(text);
		if (used + length + 1U >= sizeof(entry)) {
			errno = EOVERFLOW;
			goto invalid;
		}
		(void)memcpy(entry + used, text, length + 1U);
		used += length;
	}
	if (ferror(stream))
		return -1;
	if (used != 0)
		return compile_entry(pathname, entry_line, entry, directory);
	return 0;

invalid:
	fprintf(stderr, "tic: %s:%lu: %s\n", pathname, line, strerror(errno));
	return -1;
}

int
main(int argc, char **argv)
{
	const char *directory = "/lib/terminfo";
	int option;
	int status = 0;

	while ((option = getopt(argc, argv, "o:")) != -1) {
		if (option != 'o')
			goto usage;
		directory = optarg;
	}
	if (optind == argc || *directory == '\0')
		goto usage;
	if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "tic: %s: %s\n", directory, strerror(errno));
		return 1;
	}
	for (; optind < argc; optind++) {
		FILE *stream;

		if (strcmp(argv[optind], "-") == 0)
			stream = stdin;
		else
			stream = fopen(argv[optind], "r");
		if (stream == NULL) {
			fprintf(stderr, "tic: %s: %s\n", argv[optind],
				strerror(errno));
			status = 1;
			continue;
		}
		if (compile_stream(stream, argv[optind], directory) != 0) {
			if (errno != EINVAL && errno != ENOTSUP)
				fprintf(stderr, "tic: %s: %s\n", argv[optind],
					strerror(errno));
			status = 1;
		}
		if (stream != stdin && fclose(stream) == EOF)
			status = 1;
	}
	return status;

usage:
	fprintf(stderr, "usage: tic [-o directory] source ...\n");
	return 2;
}
