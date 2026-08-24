/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

static int
category_index(const char *name)
{
	int category;

	for (category = 0; category < 6; category++)
		if (strcmp(category_names[category], name) == 0)
			return category;
	return -1;
}

static const struct key_metadata *
keyword_find(const char *name)
{
	size_t index;

	for (index = 0; index < sizeof(metadata) / sizeof(metadata[0]); index++)
		if (strcmp(metadata[index].keyword, name) == 0)
			return &metadata[index];
	return NULL;
}

static int
numeric_keyword(enum zedbsd_locale_key key)
{
	return key >= ZEDBSD_LOCALE_KEY_INT_FRAC_DIGITS &&
	       key <= ZEDBSD_LOCALE_KEY_N_SIGN_POSN;
}

static int
grouping_keyword(enum zedbsd_locale_key key)
{
	return key == ZEDBSD_LOCALE_KEY_GROUPING ||
	       key == ZEDBSD_LOCALE_KEY_MON_GROUPING;
}

static void
print_quoted(const char *value)
{
	putchar('"');
	while (*value != '\0') {
		unsigned char byte = (unsigned char)*value++;

		if (byte == '"' || byte == '\\')
			putchar('\\');
		if (byte == '\n')
			fputs("\\n", stdout);
		else if (byte == '\t')
			fputs("\\t", stdout);
		else
			putchar(byte);
	}
	puts("\"");
}

static void
print_value(const struct key_metadata *key, int include_keyword)
{
	const char *value = nl_langinfo((nl_item)key->key);

	if (include_keyword)
		printf("%s=", key->keyword);
	if (grouping_keyword(key->key)) {
		const unsigned char *group = (const unsigned char *)value;
		int first = 1;

		while (*group != '\0') {
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

static int
print_category(int category, int category_heading, int include_keyword)
{
	size_t index;

	if (category_heading)
		printf("%s\n", category_names[category]);
	for (index = 0; index < sizeof(metadata) / sizeof(metadata[0]); index++)
		if (metadata[index].category == category)
			print_value(&metadata[index], include_keyword);
	return ferror(stdout) ? -1 : 0;
}

static int
name_compare(const void *left, const void *right)
{
	const char *const *a = left;
	const char *const *b = right;

	return strcmp(*a, *b);
}

static int
locale_list(void)
{
	const char *path = getenv("LOCPATH");
	const char *builtins[] = {"C", "C.UTF-8", "POSIX"};
	char **names = NULL;
	size_t count = 0;
	size_t capacity = 0;
	size_t index;
	DIR *directory;
	struct dirent *entry;

	if (path == NULL || strchr(path, ':') != NULL)
		path = "/usr/share/locale";
	directory = opendir(path);
	if (directory != NULL) {
		while ((entry = readdir(directory)) != NULL) {
			char *copy;

			if (entry->d_name[0] == '.')
				continue;
			if (count == capacity) {
				size_t wanted =
				    capacity == 0 ? 16U : capacity * 2U;
				void *replacement =
				    realloc(names, wanted * sizeof(*names));

				if (replacement == NULL) {
					(void)closedir(directory);
					goto failed;
				}
				names = replacement;
				capacity = wanted;
			}
			copy = strdup(entry->d_name);
			if (copy == NULL) {
				(void)closedir(directory);
				goto failed;
			}
			names[count++] = copy;
		}
		(void)closedir(directory);
	}
	for (index = 0; index < sizeof(builtins) / sizeof(builtins[0]); index++)
		puts(builtins[index]);
	qsort(names, count, sizeof(*names), name_compare);
	for (index = 0; index < count; index++) {
		if (strcmp(names[index], "C") != 0 &&
		    strcmp(names[index], "C.UTF-8") != 0 &&
		    strcmp(names[index], "POSIX") != 0)
			puts(names[index]);
		free(names[index]);
	}
	free(names);
	return ferror(stdout) ? 1 : 0;

failed:
	for (index = 0; index < count; index++)
		free(names[index]);
	free(names);
	fprintf(stderr, "locale: %s\n", strerror(errno));
	return 1;
}

static void
print_environment(void)
{
	const char *lang = getenv("LANG");
	const char *all = getenv("LC_ALL");
	int category;

	printf("LANG=%s\n", lang != NULL ? lang : "");
	for (category = 0; category < 6; category++) {
		const char *environment = getenv(category_names[category]);

		printf("%s=\"%s\"\n", category_names[category],
		       environment != NULL ? environment
					   : setlocale(category, NULL));
	}
	printf("LC_ALL=%s\n", all != NULL ? all : "");
}

int
main(int argc, char **argv)
{
	int include_category = 0;
	int include_keyword = 0;
	int index = 1;

	if (index < argc && strcmp(argv[index], "-a") == 0)
		return index + 1 == argc ? locale_list() : 2;
	if (index < argc && strcmp(argv[index], "-m") == 0) {
		if (index + 1 != argc)
			return 2;
		puts("US-ASCII");
		puts("UTF-8");
		return 0;
	}
	while (index < argc && argv[index][0] == '-' &&
	       argv[index][1] != '\0') {
		const char *option = argv[index] + 1;

		if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		}
		while (*option != '\0') {
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
	if (setlocale(LC_ALL, "") == NULL) {
		fprintf(stderr, "locale: cannot select locale: %s\n",
			strerror(errno));
		return 1;
	}
	if (index == argc) {
		if (include_category || include_keyword)
			goto usage;
		print_environment();
		return ferror(stdout) ? 1 : 0;
	}
	for (; index < argc; index++) {
		int category = category_index(argv[index]);
		const struct key_metadata *key;

		if (category >= 0) {
			if (print_category(category, include_category,
					   include_keyword) != 0)
				return 1;
			continue;
		}
		key = keyword_find(argv[index]);
		if (key == NULL) {
			fprintf(stderr,
				"locale: %s: unknown keyword or category\n",
				argv[index]);
			return 1;
		}
		if (include_category)
			printf("%s\n", category_names[key->category]);
		print_value(key, include_keyword);
	}
	return ferror(stdout) ? 1 : 0;

usage:
	fprintf(stderr, "usage: locale [-a|-m] | locale [-ck] [name ...]\n");
	return 2;
}
