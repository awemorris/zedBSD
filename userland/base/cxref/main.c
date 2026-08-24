/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
reference_compare(const void *left, const void *right)
{
	const struct reference *a = left, *b = right;
	int result = strcmp(a->name, b->name);
	if (!result)
		result = strcmp(a->file, b->file);
	if (!result)
		result = a->line < b->line ? -1 : a->line > b->line;
	return result;
}

static int
append_result(struct reference **references, size_t *count,
	      const struct c_parse_result *result)
{
	for (size_t i = 0; i < result->count; i++) {
		struct reference *replacement;
		if (*count == SIZE_MAX / sizeof(**references))
			return -1;
		replacement =
		    realloc(*references, (*count + 1) * sizeof(**references));
		if (!replacement)
			return -1;
		*references = replacement;
		replacement[*count].name = strdup(result->events[i].name);
		replacement[*count].file = strdup(result->events[i].file);
		replacement[*count].line = result->events[i].line;
		replacement[*count].kind = result->events[i].kind;
		if (!replacement[*count].name || !replacement[*count].file)
			return -1;
		(*count)++;
	}
	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: cxref [-cs] [-o file] [-w width] [-D name] [-I dir] "
		"[-U name] [file ...]\n");
}

int
main(int argc, char **argv)
{
	struct reference *references = NULL;
	size_t count = 0;
	const char *output_path = NULL;
	FILE *output = stdout;
	unsigned width = 80;
	int declarations_only = 0, symbols_only = 0, failed = 0, ch;
	while ((ch = getopt(argc, argv, "csD:I:o:U:w:")) != -1) {
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
		case 'w': {
			char *end;
			unsigned long value = strtoul(optarg, &end, 10);
			if (*end || value < 20 || value > 4096) {
				usage();
				return 2;
			}
			width = (unsigned)value;
			break;
		}
		default:
			usage();
			return 2;
		}
	}
	if (output_path) {
		output = fopen(output_path, "w");
		if (!output) {
			fprintf(stderr, "cxref: %s: %s\n", output_path,
				strerror(errno));
			return 1;
		}
	}
	if (optind == argc) {
		struct c_parse_result result;
		if (c_parse_stream("<stdin>", STDIN_FILENO, &result) ||
		    append_result(&references, &count, &result))
			failed = 1;
		c_parse_free(&result);
	} else {
		for (int i = optind; i < argc; i++) {
			struct c_parse_result result;
			if (c_parse_path(argv[i], &result)) {
				fprintf(stderr, "cxref: %s: %s\n", argv[i],
					strerror(errno));
				failed = 1;
				continue;
			}
			if (append_result(&references, &count, &result))
				failed = 1;
			c_parse_free(&result);
		}
	}
	qsort(references, count, sizeof(*references), reference_compare);
	for (size_t i = 0; i < count;) {
		size_t j = i;
		unsigned column = 0;
		if (declarations_only &&
		    references[i].kind != C_SYMBOL_DECLARATION &&
		    references[i].kind != C_SYMBOL_FUNCTION) {
			i++;
			continue;
		}
		fprintf(output, "%-20s", references[i].name);
		column = 20;
		while (j < count &&
		       !strcmp(references[i].name, references[j].name)) {
			char location[256];
			int length;
			if (symbols_only &&
			    references[j].kind == C_SYMBOL_REFERENCE) {
				j++;
				continue;
			}
			length = snprintf(
			    location, sizeof(location), " %s:%zu%s",
			    references[j].file, references[j].line,
			    references[j].kind == C_SYMBOL_FUNCTION ? "*" : "");
			if (length > 0 && column + (unsigned)length > width) {
				fputs("\n                    ", output);
				column = 20;
			}
			fputs(location, output);
			column += length > 0 ? (unsigned)length : 0;
			j++;
		}
		fputc('\n', output);
		i = j;
	}
	if (output != stdout && fclose(output))
		failed = 1;
	for (size_t i = 0; i < count; i++) {
		free(references[i].name);
		free(references[i].file);
	}
	free(references);
	return failed;
}
