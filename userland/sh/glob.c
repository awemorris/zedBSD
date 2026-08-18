/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/sh/glob.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

struct matches {
	char **items;
	size_t count;
};

static int
match_component(const char *pattern, const unsigned char *quoted, size_t length,
    const char *name)
{
	if (length == 0)
		return *name == '\0';
	if (!quoted[0] && pattern[0] == '*') {
		do {
			if (match_component(pattern + 1, quoted + 1, length - 1U,
			    name))
				return 1;
		} while (*name++ != '\0');
		return 0;
	}
	if (*name == '\0')
		return 0;
	if (!quoted[0] && pattern[0] == '?')
		return match_component(pattern + 1, quoted + 1, length - 1U,
		    name + 1);
	if (!quoted[0] && pattern[0] == '[') {
		size_t index = 1;
		int negate = 0, matched = 0;
		if (index < length && (pattern[index] == '!' ||
		    pattern[index] == '^')) {
			negate = 1;
			index++;
		}
		while (index < length && pattern[index] != ']') {
			char first = pattern[index++];
			char last = first;
			if (index + 1U < length && pattern[index] == '-' &&
			    pattern[index + 1U] != ']') {
				last = pattern[index + 1U];
				index += 2U;
			}
			if (*name >= first && *name <= last)
				matched = 1;
		}
		if (index < length && pattern[index] == ']' &&
		    matched != negate)
			return match_component(pattern + index + 1U,
			    quoted + index + 1U, length - index - 1U, name + 1);
	}
	return pattern[0] == *name && match_component(pattern + 1, quoted + 1,
	    length - 1U, name + 1);
}

static int
has_meta(const char *pattern, const unsigned char *quoted, size_t length)
{
	size_t index;
	for (index = 0; index < length; index++) {
		if (!quoted[index] && (pattern[index] == '*' || pattern[index] == '?' ||
		    pattern[index] == '['))
			return 1;
	}
	return 0;
}

static char *
join_path(const char *prefix, const char *name, size_t name_length)
{
	size_t prefix_length = strlen(prefix);
	int slash = prefix_length != 0 && prefix[prefix_length - 1U] != '/';
	char *path = malloc(prefix_length + (size_t)slash + name_length + 1U);
	if (path == NULL)
		return NULL;
	memcpy(path, prefix, prefix_length);
	if (slash) path[prefix_length++] = '/';
	memcpy(path + prefix_length, name, name_length);
	path[prefix_length + name_length] = '\0';
	return path;
}

static int
match_append(struct matches *matches, const char *path)
{
	char **larger;
	char *copy = join_path("", path, strlen(path));
	if (copy == NULL)
		return 0;
	larger = realloc(matches->items,
	    (matches->count + 1U) * sizeof(*matches->items));
	if (larger == NULL) {
		free(copy);
		return 0;
	}
	matches->items = larger;
	matches->items[matches->count++] = copy;
	return 1;
}

static int
expand_path(const char *pattern, const unsigned char *quoted, size_t length,
    size_t position, const char *prefix, struct matches *matches)
{
	size_t end = position;
	while (end < length && pattern[end] != '/') end++;
	if (position == length)
		return match_append(matches, prefix[0] == '\0' ? "." : prefix);
	if (!has_meta(pattern + position, quoted + position, end - position)) {
		char *next = join_path(prefix, pattern + position, end - position);
		int result;
		if (next == NULL) return 0;
		result = expand_path(pattern, quoted, length,
		    end < length ? end + 1U : end, next, matches);
		free(next);
		return result;
	}
	{
		DIR *directory = opendir(prefix[0] == '\0' ? "." : prefix);
		struct dirent *entry;
		if (directory == NULL)
			return 1;
		while ((entry = readdir(directory)) != NULL) {
			char *next;
			int result;
			if (entry->d_name[0] == '.' &&
			    (end == position || pattern[position] != '.'))
				continue;
			if (!match_component(pattern + position, quoted + position,
			    end - position, entry->d_name))
				continue;
			next = join_path(prefix, entry->d_name, strlen(entry->d_name));
			if (next == NULL) { (void)closedir(directory); return 0; }
			result = expand_path(pattern, quoted, length,
			    end < length ? end + 1U : end, next, matches);
			free(next);
			if (!result) { (void)closedir(directory); return 0; }
		}
		(void)closedir(directory);
	}
	return 1;
}

static void
sort_matches(struct matches *matches)
{
	size_t index;
	for (index = 1; index < matches->count; index++) {
		char *value = matches->items[index];
		size_t position = index;
		while (position != 0 &&
		    strcmp(matches->items[position - 1U], value) > 0) {
			matches->items[position] = matches->items[position - 1U];
			position--;
		}
		matches->items[position] = value;
	}
}

static void
matches_free(struct matches *matches)
{
	size_t index;
	for (index = 0; index < matches->count; index++)
		free(matches->items[index]);
	free(matches->items);
	matches->items = NULL;
	matches->count = 0;
}

static int
output_append(struct sh_field_list *output, char *field)
{
	char **fields;
	unsigned char **quoted;
	fields = realloc(output->fields,
	    (output->count + 1U) * sizeof(*output->fields));
	if (fields == NULL) return 0;
	output->fields = fields;
	quoted = realloc(output->quoted,
	    (output->count + 1U) * sizeof(*output->quoted));
	if (quoted == NULL) return 0;
	output->quoted = quoted;
	output->fields[output->count] = field;
	output->quoted[output->count] = NULL;
	output->count++;
	return 1;
}

int
sh_glob_fields(struct sh_field_list *list, const char **error_text)
{
	struct sh_field_list output = { 0 };
	size_t field;
	*error_text = NULL;
	for (field = 0; field < list->count; field++) {
		struct matches matches = { 0 };
		size_t length = strlen(list->fields[field]);
		size_t match;
		if (!has_meta(list->fields[field], list->quoted[field], length)) {
			if (!output_append(&output, list->fields[field]))
				goto no_memory;
			list->fields[field] = NULL;
			continue;
		}
		if (!expand_path(list->fields[field], list->quoted[field], length,
		    list->fields[field][0] == '/' ? 1U : 0U,
		    list->fields[field][0] == '/' ? "/" : "", &matches)) {
			matches_free(&matches);
			goto no_memory;
		}
		if (matches.count == 0) {
			free(matches.items);
			if (!output_append(&output, list->fields[field]))
				goto no_memory;
			list->fields[field] = NULL;
			continue;
		}
		sort_matches(&matches);
		for (match = 0; match < matches.count; match++) {
			if (!output_append(&output, matches.items[match]))
				goto no_memory;
			matches.items[match] = NULL;
		}
		free(matches.items);
	}
	sh_fields_free(list);
	*list = output;
	return 1;
no_memory:
	*error_text = "out of memory";
	sh_fields_free(&output);
	return 0;
}
