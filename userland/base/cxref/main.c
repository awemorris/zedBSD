/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD cxref userland command.
 */

#include "userland/base/common/c_parser.h"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct reference {
	char *name;
	char *file;
	size_t line;
	enum c_symbol_kind kind;
};

static void usage(void);
static int append_result(struct reference **references, size_t *count, const struct c_parse_result *result);
static int reference_compare(const void *left, const void *right);

/*
 * Runs the cxref command.
 */
int
main(
	int argc,
	char **argv)
{
	struct c_parse_result result_local;
	struct c_parse_result result_local1;
	char *end;
	unsigned long value;
	char location[256];
	int length;
	size_t j;
	unsigned column;
	int i_index_for;
	size_t i_index_for1;
	size_t i_index_for2;
	struct reference *references;
	size_t count;
	const char *output_path;
	FILE *output;
	unsigned width;
	int declarations_only, symbols_only, failed, ch;

	/* Parse each command-line option. */
	references = NULL;
	count = 0;
	output_path = NULL;
	output = stdout;
	width = 80;
	declarations_only = 0;
	symbols_only = 0;
	failed = 0;
	while ((ch = getopt(argc, argv, "csD:I:o:U:w:")) != -1) {
		/* Dispatch the selected operation case. */
		switch (ch) {
		case 'c':
			declarations_only = 1;
			break;
		case 's':
			symbols_only = 1;
			break;
		case 'D':
		case 'I':
		case 'U':
			break;
		case 'o':
			output_path = optarg;
			break;
		case 'w':

		value = strtoul(optarg, &end, 10);

		/* Checks the current endpoint. */
		if (*end || value < 20 || value > 4096) {
			usage();

			/* Reports operation failure. */
			return 2;
		}
		width = (unsigned)value;
		break;
		default:
			usage();

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Handles the output path condition. */
	if (output_path) {
		output = fopen(output_path, "w");

		/* Handles the output condition. */
		if (!output) {
			fprintf(stderr, "cxref: %s: %s\n", output_path,
				strerror(errno));

			/* Reports operation failure. */
			return 1;
		}
	}

	/* Validates the command-line arguments. */
	if (optind == argc) {
		/* Handles a failed c parse stream operation. */
		if (c_parse_stream("<stdin>", STDIN_FILENO, &result_local) ||
		    append_result(&references, &count, &result_local))
			failed = 1;
		c_parse_free(&result_local);
	} else {
		/* Process each remaining command-line operand. */
		for (i_index_for = optind; i_index_for < argc; i_index_for++) {
			/* Validates the command-line arguments. */
			if (c_parse_path(argv[i_index_for], &result_local1)) {
				fprintf(stderr, "cxref: %s: %s\n", argv[i_index_for],
					strerror(errno));
				failed = 1;
				continue;
			}

			/* Handles the append result condition. */
			if (append_result(&references, &count, &result_local1))
				failed = 1;
			c_parse_free(&result_local1);
		}
	}
	qsort(references, count, sizeof(*references), reference_compare);

	/* Process each remaining element. */
	for (i_index_for1 = 0; i_index_for1 < count;) {
		j = i_index_for1;
		column = 0;

		/* Handles the declarations only condition. */
		if (declarations_only &&
		    references[i_index_for1].kind != C_SYMBOL_DECLARATION &&
		    references[i_index_for1].kind != C_SYMBOL_FUNCTION) {
			i_index_for1++;
			continue;
		}
		fprintf(output, "%-20s", references[i_index_for1].name);

		/* Process each remaining element. */
		column = 20;
		while (j < count &&
		       !strcmp(references[i_index_for1].name, references[j].name)) {
			/* Handles the symbols only condition. */
			if (symbols_only &&
			    references[j].kind == C_SYMBOL_REFERENCE) {
				j++;
				continue;
			}
			length = snprintf(
			    location, sizeof(location), " %s:%zu%s",
			    references[j].file, references[j].line,
			    references[j].kind == C_SYMBOL_FUNCTION ? "*" : "");

			/* Checks the current data length. */
			if (length > 0 && column + (unsigned)length > width) {
				fputs("\n                    ", output);
				column = 20;
			}
			fputs(location, output);
			column += length > 0 ? (unsigned)length : 0;
			j++;
		}
		fputc('\n', output);
		i_index_for1 = j;
	}

	/* Handles a failed fclose operation. */
	if (output != stdout && fclose(output))

	/* Process each remaining element. */
		failed = 1;
	for (i_index_for2 = 0; i_index_for2 < count; i_index_for2++) {
		free(references[i_index_for2].name);
		free(references[i_index_for2].file);
	}
	free(references);

	/* Returns the computed result. */
	return failed;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr,
		"usage: cxref [-cs] [-o file] [-w width] [-D name] [-I dir] "
		"[-U name] [file ...]\n");
}

/* Supports the append result operation. */
static int
append_result(
	struct reference **references,
	size_t *count,
	const struct c_parse_result *result)
{
	struct reference *replacement;
	size_t i_index_for;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < result->count; i_index_for++) {
		/* Checks the remaining item count. */
		if (*count == SIZE_MAX / sizeof(**references))
			return -1;
		replacement =
		    realloc(*references, (*count + 1) * sizeof(**references));

		/* Handles the replacement condition. */
		if (!replacement)
			return -1;
		*references = replacement;
		replacement[*count].name = strdup(result->events[i_index_for].name);
		replacement[*count].file = strdup(result->events[i_index_for].file);
		replacement[*count].line = result->events[i_index_for].line;
		replacement[*count].kind = result->events[i_index_for].kind;

		/* Handles the replacement condition. */
		if (!replacement[*count].name || !replacement[*count].file)
			return -1;
		(*count)++;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the reference compare operation. */
static int
reference_compare(
	const void *left,
	const void *right)
{
	const struct reference *a, *b;
	int result;

	a = left;
	b = right;
	result = strcmp(a->name, b->name);

	/* Checks the operation result. */
	if (!result)
		result = strcmp(a->file, b->file);

	/* Checks the operation result. */
	if (!result)
		result = a->line < b->line ? -1 : a->line > b->line;

	/* Returns the computed result. */
	return result;
}
