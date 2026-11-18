/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD nm userland command.
 */

#include "userland/base/common/archive.h"
#include "userland/base/common/elf_symbols.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct options {
	int prefix;
	int external;
	int numeric;
	int no_sort;
	int reverse;
	int portable;
	int undefined;
	int dynamic;
	char radix;
};

static struct options options = {.radix = 'x'};

static void usage(void);
static int process_file(const char *path, int multiple);
static int display_object(const void *data, size_t size, const char *file, const char *member, int multiple);
static void print_value(uint64_t value, unsigned bits);
static int compare_name(const void *left, const void *right);

/*
 * Runs the nm command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int i_index_for;
	int ch, failed;

	/* Parse each command-line option. */
	failed = 0;
	while ((ch = getopt(argc, argv, "ADAPglnopruvt:")) != -1) {
		/* Dispatch the selected operation case. */
		switch (ch) {
		case 'A':
		case 'o':
			options.prefix = 1;
			break;
		case 'D':
			options.dynamic = 1;
			break;
		case 'g':
			options.external = 1;
			break;
		case 'l':
			break;
		case 'n':
		case 'v':
			options.numeric = 1;
			break;
		case 'p':
			options.no_sort = 1;
			break;
		case 'P':
			options.portable = 1;
			break;
		case 'r':
			options.reverse = 1;
			break;
		case 'u':
			options.undefined = 1;
			break;
		case 't':
			/* Handles a failed strlen operation. */
			if (strlen(optarg) != 1 || !strchr("dox", optarg[0])) {
				usage();

				/* Reports operation failure. */
				return 2;
			}
			options.radix = optarg[0];
			break;
		default:
			usage();

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Validates the command-line arguments. */
	if (optind == argc) {
		/* Obtains the process file result. */
		function_result = process_file("a.out", 0);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining command-line operand. */
	for (i_index_for = optind; i_index_for < argc; i_index_for++)
		failed |= process_file(argv[i_index_for], argc - optind > 1);

	/* Returns the computed result. */
	return failed;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr, "usage: nm [-APglnopruv] [-t d|o|x] [file ...]\n");
}

/* Supports the process file operation. */
static int
process_file(
	const char *path,
	int multiple)
{
	ssize_t n;
	struct archive_file archive;
	size_t i_index_for;
	struct stat st;
	unsigned char *data;
	size_t done;
	int fd;
	int result;

	data = NULL;
	done = 0;
	fd = open(path, O_RDONLY);
	result = 0;

	/* Handles a failed fstat operation. */
	if (fd < 0 || fstat(fd, &st) || st.st_size < 0 ||
	    (uintmax_t)st.st_size > SIZE_MAX) {
		fprintf(stderr, "nm: %s: %s\n", path, strerror(errno));

		/* Checks the file descriptor. */
		if (fd >= 0)
			close(fd);

		/* Reports operation failure. */
		return 1;
	}
	data = malloc(st.st_size ? (size_t)st.st_size : 1);

	/* Handles the data condition. */
	if (!data) {
		close(fd);

		/* Reports operation failure. */
		return 1;
	}
	while (done < (size_t)st.st_size) {
		n = read(fd, data + done, (size_t)st.st_size - done);

		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Checks the current item count. */
		if (n <= 0) {
			fprintf(stderr, "nm: %s: short read\n", path);
			free(data);
			close(fd);

			/* Reports operation failure. */
			return 1;
		}
		done += (size_t)n;
	}
	close(fd);

	/* Handles the done condition. */
	if (done >= 8 && !memcmp(data, "!<arch>\n", 8)) {
		/* Handles the archive read memory condition. */
		if (archive_read_memory(data, done, &archive)) {
			fprintf(stderr, "nm: %s: invalid archive\n", path);
			result = 1;
		} else {
			/* Process each remaining element. */
			for (i_index_for = 0; i_index_for < archive.count; i_index_for++) {
				/* Handles the archive condition. */
				if (!archive.members[i_index_for].special) {
					result |= display_object(
					    archive.members[i_index_for].data,
					    archive.members[i_index_for].size, path,
					    archive.members[i_index_for].name, 1);
				}
			}
			archive_free(&archive);
		}
	} else {
		result = display_object(data, done, path, NULL, multiple);
	}
	free(data);

	/* Returns the computed result. */
	return result;
}

/* Supports the display object operation. */
static int
display_object(
	const void *data,
	size_t size,
	const char *file,
	const char *member,
	int multiple)
{
	const struct elf_symbol_record *symbol;
	size_t i_index_for;
	struct elf_symbol_table table;
	int result;

	result = 0;

	/* Handles the elf symbols read condition. */
	if (elf_symbols_read(data, size, options.dynamic, &table)) {
		fprintf(stderr, "nm: %s%s%s: %s\n", file, member ? "(" : "",
			member ? member : "",
			errno == ENOENT ? "no symbols" : "invalid object");

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the selected options. */
	if (!options.no_sort && table.count > 1) {
		qsort(table.symbols, table.count, sizeof(*table.symbols),
		      compare_name);
	}

	/* Handles the multiple condition. */
	if (multiple && !options.prefix && !options.portable) {
		printf("\n%s%s%s%s:\n", file, member ? "(" : "",
		       member ? member : "", member ? ")" : "");
	}

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < table.count; i_index_for++) {
		symbol = &table.symbols[i_index_for];

		/* Handles the symbol condition. */
		if (!*symbol->name ||
		    (options.external && symbol->binding == 0) ||
		    (options.undefined && symbol->section != 0))
			continue;

		/* Checks the selected options. */
		if (options.prefix) {
			printf("%s%s%s%s: ", file, member ? "(" : "",
			       member ? member : "", member ? ")" : "");
		}

		/* Checks the selected options. */
		if (options.portable) {
			printf("%s %c ", symbol->name, symbol->letter);
			print_value(symbol->value, table.bits);
			putchar(' ');
			print_value(symbol->size, table.bits);
			putchar('\n');
		} else {
			/* Handles the symbol condition. */
			if (symbol->section == 0)
				printf("%*s", table.bits == 64 ? 16 : 8, "");
			else
				print_value(symbol->value, table.bits);
			printf(" %c %s\n", symbol->letter, symbol->name);
		}
	}
	elf_symbols_free(&table);

	/* Returns the computed result. */
	return result;
}

/* Supports the print value operation. */
static void
print_value(
	uint64_t value,
	unsigned bits)
{
	/* Checks the selected options. */
	if (options.radix == 'd') {
		printf("%0*llu", bits == 64 ? 20 : 10,
		       (unsigned long long)value);
	} else if (options.radix == 'o') {
		printf("%0*llo", bits == 64 ? 22 : 11,
		       (unsigned long long)value);
	} else {
		printf("%0*llx", bits == 64 ? 16 : 8,
		       (unsigned long long)value);
	}
}

/* Supports the compare name operation. */
static int
compare_name(
	const void *left,
	const void *right)
{
	const struct elf_symbol_record *a, *b;
	int result;

	a = left;
	b = right;

	/* Checks the selected options. */
	if (options.numeric) {
		/* Handles the a condition. */
		if (a->value < b->value)
			result = -1;
		else if (a->value > b->value)
			result = 1;
		else
			result = strcmp(a->name, b->name);
	} else {
		result = strcmp(a->name, b->name);
	}

	/* Returns the computed result. */
	return options.reverse ? -result : result;
}
