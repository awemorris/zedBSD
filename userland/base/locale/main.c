/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD locale userland command.
 */

#include <dirent.h>
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libc/include/zedbsd/locale-format.h"

struct key_metadata {
	int category;
	const char *keyword;
	enum zedbsd_locale_key key;
};

static const struct key_metadata metadata[] = {
#define LOCALE_METADATA(name, category, keyword, c_value, utf8_value)          \
	{category, keyword, ZEDBSD_LOCALE_KEY_##name},
    ZEDBSD_LOCALE_KEYS(LOCALE_METADATA)
#undef LOCALE_METADATA
};

static const char *const category_names[] = {
    "LC_CTYPE",	  "LC_NUMERIC",	 "LC_TIME",
    "LC_COLLATE", "LC_MONETARY", "LC_MESSAGES",
};

static int locale_list(void);
static void print_environment(void);
static int category_index(const char *name);
static int print_category(int category, int category_heading, int include_keyword);
static void print_value(const struct key_metadata *key, int include_keyword);
static int grouping_keyword(enum zedbsd_locale_key key);
static int numeric_keyword(enum zedbsd_locale_key key);
static void print_quoted(const char *value);
static const struct key_metadata *keyword_find(const char *name);
static int name_compare(const void *left, const void *right);

/*
 * Runs the locale command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	const char *option;
	int category;
	const struct key_metadata *key;
	int include_category;
	int include_keyword;
	int index;

	include_category = 0;
	include_keyword = 0;
	index = 1;

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "-a") == 0) {
		/* Computes the function result. */
		function_result = index + 1 == argc ? locale_list() : 2;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "-m") == 0) {
		/* Validates the command-line arguments. */
		if (index + 1 != argc)
			return 2;
		puts("US-ASCII");
		puts("UTF-8");

		/* Reports successful completion. */
		return 0;
	}
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
			if (*option == 'c')
				include_category = 1;
			else if (*option == 'k')
				include_keyword = 1;
			else
				goto usage;
			option++;
		}
		index++;
	}

	/* Handles a failed setlocale operation. */
	if (setlocale(LC_ALL, "") == NULL) {
		fprintf(stderr, "locale: cannot select locale: %s\n",
			strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Validates the command-line arguments. */
	if (index == argc) {
		/* Handles the include category condition. */
		if (include_category || include_keyword)
			goto usage;
		print_environment();

		/* Computes the function result. */
		function_result = ferror(stdout) ? 1 : 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining command-line operand. */
	for (; index < argc; index++) {
				category = category_index(argv[index]);

		/* Handles the category condition. */
		if (category >= 0) {
			/* Handles a failed print category operation. */
			if (print_category(category, include_category,
					   include_keyword) != 0)

				/* Reports operation failure. */
				return 1;
			continue;
		}
		key = keyword_find(argv[index]);

		/* Handles the key availability. */
		if (key == NULL) {
			fprintf(stderr,
				"locale: %s: unknown keyword or category\n",
				argv[index]);

			/* Reports operation failure. */
			return 1;
		}

		/* Handles the include category condition. */
		if (include_category)
			printf("%s\n", category_names[key->category]);
		print_value(key, include_keyword);
	}

	/* Computes the function result. */
	function_result = ferror(stdout) ? 1 : 0;

	/* Returns the computed result. */
	return function_result;

usage:
	fprintf(stderr, "usage: locale [-a|-m] | locale [-ck] [name ...]\n");

	/* Reports operation failure. */
	return 2;
}

/* Supports the locale list operation. */
static int
locale_list(
	void)
{
	int function_result;
	size_t wanted;
	void *replacement;
	char *copy;
	const char *path;
	const char *builtins[] = {"C", "C.UTF-8", "POSIX"};
	char **names;
	size_t count;
	size_t capacity;
	size_t index;
	DIR *directory;
	struct dirent *entry;

	path = getenv("LOCPATH");
	names = NULL;
	count = 0;
	capacity = 0;

	/* Handles a failed strchr operation. */
	if (path == NULL || strchr(path, ':') != NULL)
		path = "/usr/share/locale";
	directory = opendir(path);

	/* Handles the directory availability. */
	if (directory != NULL) {
		/* Process each directory entry. */
		while ((entry = readdir(directory)) != NULL) {
			/* Handles the entry condition. */
			if (entry->d_name[0] == '.')
				continue;

			/* Checks the remaining item count. */
			if (count == capacity) {
								wanted = capacity == 0 ? 16U : capacity * 2U;
								replacement = realloc(names, wanted * sizeof(*names));

				/* Handles the replacement availability. */
				if (replacement == NULL) {
					(void)closedir(directory);
					goto failed;
				}
				names = replacement;
				capacity = wanted;
			}
			copy = strdup(entry->d_name);

			/* Handles the copy availability. */
			if (copy == NULL) {
				(void)closedir(directory);
				goto failed;
			}
			names[count++] = copy;
		}
		(void)closedir(directory);
	}

	/* Process each remaining element. */
	for (index = 0; index < sizeof(builtins) / sizeof(builtins[0]); index++)
		puts(builtins[index]);
	qsort(names, count, sizeof(*names), name_compare);

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Selects the matching value. */
		if (strcmp(names[index], "C") != 0 &&
		    strcmp(names[index], "C.UTF-8") != 0 &&
		    strcmp(names[index], "POSIX") != 0)
			puts(names[index]);
		free(names[index]);
	}
	free(names);

	/* Computes the function result. */
	function_result = ferror(stdout) ? 1 : 0;

	/* Returns the computed result. */
	return function_result;

failed:

	/* Process each remaining element. */
	for (index = 0; index < count; index++)
		free(names[index]);
	free(names);
	fprintf(stderr, "locale: %s\n", strerror(errno));

	/* Reports operation failure. */
	return 1;
}

/* Supports the print environment operation. */
static void
print_environment(
	void)
{
	const char *environment;
	const char *lang;
	const char *all;
	int category;

	lang = getenv("LANG");
	all = getenv("LC_ALL");

	printf("LANG=%s\n", lang != NULL ? lang : "");

	/* Process each element required by the operation. */
	for (category = 0; category < 6; category++) {
				environment = getenv(category_names[category]);

		printf("%s=\"%s\"\n", category_names[category],
		       environment != NULL ? environment
					   : setlocale(category, NULL));
	}
	printf("LC_ALL=%s\n", all != NULL ? all : "");
}

/* Supports the category index operation. */
static int
category_index(
	const char *name)
{
	int category;

	/* Process each element required by the operation. */
	for (category = 0; category < 6; category++)

		/* Selects the matching value. */
		if (strcmp(category_names[category], name) == 0)
			return category;

	/* Reports operation failure. */
	return -1;
}

/* Supports the print category operation. */
static int
print_category(
	int category,
	int category_heading,
	int include_keyword)
{
	int function_result;
	size_t index;

	/* Handles the category heading condition. */
	if (category_heading)
		printf("%s\n", category_names[category]);

	/* Process each remaining element. */
	for (index = 0; index < sizeof(metadata) / sizeof(metadata[0]); index++)

		/* Handles the metadata condition. */
		if (metadata[index].category == category)
			print_value(&metadata[index], include_keyword);

	/* Computes the function result. */
	function_result = ferror(stdout) ? -1 : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the print value operation. */
static void
print_value(
	const struct key_metadata *key,
	int include_keyword)
{
	const unsigned char *group;
	int first;
	const char *value;

	value = nl_langinfo((nl_item)key->key);

	/* Handles the include keyword condition. */
	if (include_keyword)
		printf("%s=", key->keyword);

	/* Handles a failed grouping keyword operation. */
	if (grouping_keyword(key->key)) {
				group = (const unsigned char *)value;
				first = 1;

		/* Continue while the operation condition remains true. */
		while (*group != '\0') {
			/* Handles the first condition. */
			if (!first)
				putchar(';');
			printf("%u", (unsigned)*group++);
			first = 0;
		}
		putchar('\n');
	} else if (numeric_keyword(key->key))
		printf("%s\n", value);
	else
		print_quoted(value);
}

/* Supports the grouping keyword operation. */
static int
grouping_keyword(
	enum zedbsd_locale_key key)
{
	/* Returns the computed result. */
	return key == ZEDBSD_LOCALE_KEY_GROUPING ||
	       key == ZEDBSD_LOCALE_KEY_MON_GROUPING;
}

/* Supports the numeric keyword operation. */
static int
numeric_keyword(
	enum zedbsd_locale_key key)
{
	/* Returns the computed result. */
	return key >= ZEDBSD_LOCALE_KEY_INT_FRAC_DIGITS &&
	       key <= ZEDBSD_LOCALE_KEY_N_SIGN_POSN;
}

/* Supports the print quoted operation. */
static void
print_quoted(
	const char *value)
{
	unsigned char byte;

	putchar('"');

	/* Continue while the operation condition remains true. */
	while (*value != '\0') {

		byte = (unsigned char)*value++;

		/* Classifies the current byte. */
		if (byte == '"' || byte == '\\')
			putchar('\\');

		/* Classifies the current byte. */
		if (byte == '\n')
			fputs("\\n", stdout);
		else if (byte == '\t')
			fputs("\\t", stdout);
		else
			putchar(byte);
	}
	puts("\"");
}

/* Supports the keyword find operation. */
static const struct key_metadata *
keyword_find(
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < sizeof(metadata) / sizeof(metadata[0]); index++)

		/* Selects the matching value. */
		if (strcmp(metadata[index].keyword, name) == 0)
			return &metadata[index];

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the name compare operation. */
static int
name_compare(
	const void *left,
	const void *right)
{
	int function_result;
	const char *const *a = left;
	const char *const *b = right;

	/* Obtains the strcmp result. */
	function_result = strcmp(*a, *b);

	/* Returns the computed result. */
	return function_result;
}
