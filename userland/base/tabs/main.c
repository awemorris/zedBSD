/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/common/command.h"
#include "userland/base/common/terminfo.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAB_STOP_COUNT 64U

static int
append_stop(unsigned stops[TAB_STOP_COUNT], size_t *count, unsigned stop,
	    unsigned width)
{
	if (stop == 0 || stop > width || *count == TAB_STOP_COUNT ||
	    (*count != 0 && stop <= stops[*count - 1U]))
		return 0;
	stops[(*count)++] = stop;
	return 1;
}

static int
uniform_stops(unsigned stops[TAB_STOP_COUNT], size_t *count, unsigned every,
	      unsigned width)
{
	unsigned stop;

	if (every == 0)
		return 0;
	for (stop = every + 1U; stop <= width; stop += every)
		if (!append_stop(stops, count, stop, width) ||
		    stop > UINT_MAX - every)
			return 0;
	return 1;
}

static int
explicit_stops(char *text, unsigned stops[TAB_STOP_COUNT], size_t *count,
	       unsigned width)
{
	char *cursor = text;
	unsigned previous = 1;

	while (*cursor != '\0') {
		char *end;
		unsigned long value;
		int relative;

		while (*cursor == ',' || isspace((unsigned char)*cursor))
			cursor++;
		if (*cursor == '\0')
			break;
		relative = *cursor == '+';
		if (relative)
			cursor++;
		errno = 0;
		value = strtoul(cursor, &end, 10);
		if (errno != 0 || end == cursor || value > UINT_MAX)
			return 0;
		if (*end != '\0' && *end != ',' &&
		    !isspace((unsigned char)*end))
			return 0;
		if (relative) {
			if (value > UINT_MAX - previous)
				return 0;
			value += previous;
		}
		if (!append_stop(stops, count, (unsigned)value, width))
			return 0;
		previous = (unsigned)value;
		cursor = end;
	}
	return *count != 0;
}

static int
emit_capability(const struct terminfo_capability *capability, long parameter)
{
	long parameters[9] = {parameter, 0};
	char expanded[1024];

	if (capability == NULL || capability->kind != TERMINFO_STRING ||
	    terminfo_expand(capability->string, parameters, expanded,
			    sizeof(expanded)) < 0)
		return -1;
	return command_write_all(STDOUT_FILENO, expanded, strlen(expanded));
}

int
main(int argc, char **argv)
{
	static const unsigned assembler[] = {1, 10, 16, 36, 72};
	static const unsigned assembler2[] = {1, 10, 16, 40, 72};
	static const unsigned cobol[] = {1, 8, 12, 16, 20, 55};
	static const unsigned cobol2[] = {1, 6, 10, 14, 49};
	static const unsigned cobol3[] = {1,  6,  10, 14, 18, 22, 26, 30, 34,
					  38, 42, 46, 50, 54, 58, 62, 67};
	static const unsigned fortran[] = {1, 7, 11, 15, 19, 23};
	static const unsigned pl1[] = {1,  5,  9,  13, 17, 21, 25, 29,
				       33, 37, 41, 45, 49, 53, 57, 61};
	static const unsigned snobol[] = {1, 10, 55};
	static const unsigned univac[] = {1, 12, 20, 44};
	struct terminfo terminal;
	const struct terminfo_capability *columns;
	const char *type = NULL;
	const char *directory = getenv("TERMINFO");
	const unsigned *predefined = NULL;
	size_t predefined_count = 0;
	unsigned stops[TAB_STOP_COUNT];
	size_t stop_count = 0;
	unsigned uniform = 8;
	unsigned width;
	unsigned position = 1;
	int index = 1;

	while (index < argc && argv[index][0] == '-' &&
	       argv[index][1] != '\0') {
		if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		}
		if (strcmp(argv[index], "-T") == 0) {
			if (++index >= argc)
				goto usage;
			type = argv[index++];
			continue;
		}
		if (strcmp(argv[index], "-a") == 0) {
			predefined = assembler;
			predefined_count =
			    sizeof(assembler) / sizeof(assembler[0]);
		} else if (strcmp(argv[index], "-a2") == 0) {
			predefined = assembler2;
			predefined_count =
			    sizeof(assembler2) / sizeof(assembler2[0]);
		} else if (strcmp(argv[index], "-c") == 0) {
			predefined = cobol;
			predefined_count = sizeof(cobol) / sizeof(cobol[0]);
		} else if (strcmp(argv[index], "-c2") == 0) {
			predefined = cobol2;
			predefined_count = sizeof(cobol2) / sizeof(cobol2[0]);
		} else if (strcmp(argv[index], "-c3") == 0) {
			predefined = cobol3;
			predefined_count = sizeof(cobol3) / sizeof(cobol3[0]);
		} else if (strcmp(argv[index], "-f") == 0) {
			predefined = fortran;
			predefined_count = sizeof(fortran) / sizeof(fortran[0]);
		} else if (strcmp(argv[index], "-p") == 0) {
			predefined = pl1;
			predefined_count = sizeof(pl1) / sizeof(pl1[0]);
		} else if (strcmp(argv[index], "-s") == 0) {
			predefined = snobol;
			predefined_count = sizeof(snobol) / sizeof(snobol[0]);
		} else if (strcmp(argv[index], "-u") == 0) {
			predefined = univac;
			predefined_count = sizeof(univac) / sizeof(univac[0]);
		} else if (argv[index][1] >= '1' && argv[index][1] <= '9' &&
			   argv[index][2] == '\0')
			uniform = (unsigned)(argv[index][1] - '0');
		else
			goto usage;
		index++;
	}
	if (type == NULL)
		type = getenv("TERM");
	if (type == NULL || *type == '\0') {
		fprintf(stderr, "tabs: TERM is not set\n");
		return 2;
	}
	if (terminfo_load(&terminal, type, directory) != 0) {
		fprintf(stderr, "tabs: %s: unknown or invalid terminal\n",
			type);
		return 3;
	}
	columns = terminfo_find(&terminal, "cols");
	if (columns == NULL || columns->kind != TERMINFO_NUMBER ||
	    columns->number <= 0 || (unsigned long)columns->number > UINT_MAX)
		return 3;
	width = (unsigned)columns->number;
	if (index < argc) {
		if (index + 1 != argc ||
		    !explicit_stops(argv[index], stops, &stop_count, width))
			goto usage;
	} else if (predefined != NULL) {
		size_t item;

		for (item = 0; item < predefined_count; item++)
			if (predefined[item] > 1U &&
			    !append_stop(stops, &stop_count, predefined[item],
					 width))
				goto usage;
	} else if (!uniform_stops(stops, &stop_count, uniform, width))
		goto usage;
	if (emit_capability(terminfo_find(&terminal, "cr"), 0) != 0 ||
	    emit_capability(terminfo_find(&terminal, "tbc"), 0) != 0)
		goto terminal_error;
	for (index = 0; (size_t)index < stop_count; index++) {
		unsigned distance = stops[index] - position;

		if (emit_capability(terminfo_find(&terminal, "cuf"),
				    distance) != 0 ||
		    emit_capability(terminfo_find(&terminal, "hts"), 0) != 0)
			goto terminal_error;
		position = stops[index];
	}
	if (emit_capability(terminfo_find(&terminal, "cr"), 0) != 0)
		goto terminal_error;
	return 0;

terminal_error:
	fprintf(stderr, "tabs: terminal does not support tab programming\n");
	return 1;

usage:
	fprintf(stderr, "usage: tabs [-1..-9|-a|-a2|-c|-c2|-c3|-f|-p|-s|-u] "
			"[-T terminal] [tabstops]\n");
	return 2;
}
