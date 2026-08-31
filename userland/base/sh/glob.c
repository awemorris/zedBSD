/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland glob component.
 */

#include "userland/base/sh/glob.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

struct matches {
	char **items;
	size_t count;
};

static int has_meta(const char *pattern, const unsigned char *quoted, size_t length);
static int output_append(struct sh_field_list *output, char *field);
static int expand_path(const char *pattern, const unsigned char *quoted, size_t length, size_t position, const char *prefix, struct matches *matches);
static int match_append(struct matches *matches, const char *path);
static char *join_path(const char *prefix, const char *name, size_t name_length);
static int match_component(const char *pattern, const unsigned char *quoted, size_t length, const char *name);
static void matches_free(struct matches *matches);
static void sort_matches(struct matches *matches);

/*
 * Implements the sh glob fields operation.
 */
int
sh_glob_fields(
	struct sh_field_list *list,
	const char **error_text)
{
	size_t length;
	size_t match;
	struct sh_field_list output = {0};
	size_t field;
	struct matches matches;

	/* Process each remaining element. */
	*error_text = NULL;
	for (field = 0; field < list->count; field++) {
		memset(&matches, 0, sizeof(matches));
				length = strlen(list->fields[field]);

		/* Handles a failed meta operation. */
		if (!has_meta(list->fields[field], list->quoted[field],
			      length)) {
			/* Handles a failed output append operation. */
			if (!output_append(&output, list->fields[field]))
				goto no_memory;
			list->fields[field] = NULL;
			continue;
		}

		/* Handles a failed expand path operation. */
		if (!expand_path(
			list->fields[field], list->quoted[field], length,
			list->fields[field][0] == '/' ? 1U : 0U,
			list->fields[field][0] == '/' ? "/" : "", &matches)) {
			matches_free(&matches);
			goto no_memory;
		}

		/* Handles the matches condition. */
		if (matches.count == 0) {
			free(matches.items);

			/* Handles a failed output append operation. */
			if (!output_append(&output, list->fields[field]))
				goto no_memory;
			list->fields[field] = NULL;
			continue;
		}
		sort_matches(&matches);

		/* Process each remaining element. */
		for (match = 0; match < matches.count; match++) {
			/* Handles a failed output append operation. */
			if (!output_append(&output, matches.items[match]))
				goto no_memory;
			matches.items[match] = NULL;
		}
		free(matches.items);
	}
	sh_fields_free(list);
	*list = output;
	/* Reports operation failure. */
	return 1;
no_memory:
	*error_text = "out of memory";
	sh_fields_free(&output);

	/* Reports successful completion. */
	return 0;
}

/* Supports the has meta operation. */
static int
has_meta(
	const char *pattern,
	const unsigned char *quoted,
	size_t length)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < length; index++) {
		/* Handles the quoted condition. */
		if (!quoted[index] &&
		    (pattern[index] == '*' || pattern[index] == '?' ||
		     pattern[index] == '['))

			/* Reports operation failure. */
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the output append operation. */
static int
output_append(
	struct sh_field_list *output,
	char *field)
{
	char **fields;
	unsigned char **quoted;

	fields = realloc(output->fields,
			 (output->count + 1U) * sizeof(*output->fields));

	/* Handles the fields availability. */
	if (fields == NULL)
		return 0;
	output->fields = fields;
	quoted = realloc(output->quoted,
			 (output->count + 1U) * sizeof(*output->quoted));

	/* Handles the quoted availability. */
	if (quoted == NULL)
		return 0;
	output->quoted = quoted;
	output->fields[output->count] = field;
	output->quoted[output->count] = NULL;
	output->count++;

	/* Reports operation failure. */
	return 1;
}

/* Supports the expand path operation. */
static int
expand_path(
	const char *pattern,
	const unsigned char *quoted,
	size_t length,
	size_t position,
	const char *prefix,
	struct matches *matches)
{
	int function_result;
	char *next_local;
	int result_local;
	char *next_local1;
	int result_local2;
	DIR *directory;
	struct dirent *entry;
	size_t end;

	/* Process each remaining element. */
	end = position;
	while (end < length && pattern[end] != '/')
		end++;

	/* Handles the position condition. */
	if (position == length) {
		/* Obtains the match append result. */
		function_result = match_append(matches, prefix[0] == '\0' ? "." : prefix);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed meta operation. */
	if (!has_meta(pattern + position, quoted + position, end - position)) {
				next_local = join_path(prefix, pattern + position, end - position);

		/* Handles the next local availability. */
		if (next_local == NULL)
			return 0;
		result_local =
		    expand_path(pattern, quoted, length,
				end < length ? end + 1U : end, next_local, matches);
		free(next_local);

		/* Returns the computed result. */
		return result_local;
	}

	directory = opendir(prefix[0] == '\0' ? "." : prefix);

	/* Handles the directory availability. */
	if (directory == NULL)
		return 1;

	/* Process each directory entry. */
	while ((entry = readdir(directory)) != NULL) {
		/* Handles the entry condition. */
		if (entry->d_name[0] == '.' &&
		    (end == position || pattern[position] != '.'))
			continue;

		/* Handles a failed match component operation. */
		if (!match_component(pattern + position,
				     quoted + position, end - position,
				     entry->d_name))
			continue;
		next_local1 = join_path(prefix, entry->d_name,
				 strlen(entry->d_name));

		/* Handles the next local1 availability. */
		if (next_local1 == NULL) {
			(void)closedir(directory);

			/* Reports successful completion. */
			return 0;
		}
		result_local2 = expand_path(pattern, quoted, length,
				     end < length ? end + 1U : end,
				     next_local1, matches);
		free(next_local1);

		/* Handles the result local2 condition. */
		if (!result_local2) {
			(void)closedir(directory);

			/* Reports successful completion. */
			return 0;
		}
	}
	(void)closedir(directory);

	/* Reports operation failure. */
	return 1;
}

/* Supports the match append operation. */
static int
match_append(
	struct matches *matches,
	const char *path)
{
	char **larger;
	char *copy;

	copy = join_path("", path, strlen(path));

	/* Handles the copy availability. */
	if (copy == NULL)
		return 0;
	larger = realloc(matches->items,
			 (matches->count + 1U) * sizeof(*matches->items));

	/* Handles the larger availability. */
	if (larger == NULL) {
		free(copy);

		/* Reports successful completion. */
		return 0;
	}
	matches->items = larger;
	matches->items[matches->count++] = copy;

	/* Reports operation failure. */
	return 1;
}

/* Supports the join path operation. */
static char *
join_path(
	const char *prefix,
	const char *name,
	size_t name_length)
{
	size_t prefix_length = strlen(prefix);
	int slash = prefix_length != 0 && prefix[prefix_length - 1U] != '/';
	char *path = malloc(prefix_length + (size_t)slash + name_length + 1U);

	/* Handles the path availability. */
	if (path == NULL)
		return NULL;
	memcpy(path, prefix, prefix_length);

	/* Handles the slash condition. */
	if (slash)
		path[prefix_length++] = '/';
	memcpy(path + prefix_length, name, name_length);
	path[prefix_length + name_length] = '\0';

	/* Returns the computed result. */
	return path;
}

/* Supports the match component operation. */
static int
match_component(
	const char *pattern,
	const unsigned char *quoted,
	size_t length,
	const char *name)
{
	int function_result;
	char first;
	char last;
	size_t index;
	int negate, matched;

	/* Checks the current data length. */
	if (length == 0)
		return *name == '\0';

	/* Handles the quoted condition. */
	if (!quoted[0] && pattern[0] == '*') {
		do {
			/* Handles the match component condition. */
			if (match_component(pattern + 1, quoted + 1,
					    length - 1U, name))

				/* Reports operation failure. */
				return 1;
		} while (*name++ != '\0');

		/* Reports successful completion. */
		return 0;
	}

	/* Validates the current name. */
	if (*name == '\0')
		return 0;

	/* Handles the quoted condition. */
	if (!quoted[0] && pattern[0] == '?') {
		/* Obtains the match component result. */
		function_result = match_component(pattern + 1, quoted + 1, length - 1U,
				       name + 1);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the quoted condition. */
	if (!quoted[0] && pattern[0] == '[') {
				index = 1;
				negate = 0;
		matched = 0;

		/* Checks the current index. */
		if (index < length &&
		    (pattern[index] == '!' || pattern[index] == '^')) {
			negate = 1;
			index++;
		}
		while (index < length && pattern[index] != ']') {
						first = pattern[index++];
						last = first;

			/* Checks the current index. */
			if (index + 1U < length && pattern[index] == '-' &&
			    pattern[index + 1U] != ']') {
				last = pattern[index + 1U];
				index += 2U;
			}

			/* Validates the current name. */
			if (*name >= first && *name <= last)
				matched = 1;
		}

		/* Checks the current index. */
		if (index < length && pattern[index] == ']' &&
		    matched != negate) {
			/* Obtains the match component result. */
			function_result = match_component(pattern + index + 1U,
					       quoted + index + 1U,
					       length - index - 1U, name + 1);

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Computes the function result. */
	function_result = pattern[0] == *name &&
	       match_component(pattern + 1, quoted + 1, length - 1U, name + 1);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the matches free operation. */
static void
matches_free(
	struct matches *matches)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < matches->count; index++)
		free(matches->items[index]);
	free(matches->items);
	matches->items = NULL;
	matches->count = 0;
}

/* Supports the sort matches operation. */
static void
sort_matches(
	struct matches *matches)
{
	char *value;
	size_t position;
	size_t index;

	/* Process each remaining element. */
	for (index = 1; index < matches->count; index++) {
		/* Continue while the operation condition remains true. */
				value = matches->items[index];
				position = index;
		while (position != 0 &&
		       strcmp(matches->items[position - 1U], value) > 0) {
			matches->items[position] =
			    matches->items[position - 1U];
			position--;
		}
		matches->items[position] = value;
	}
}
