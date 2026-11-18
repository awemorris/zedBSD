/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pattern matching and pathname expansion (POSIX XCU 2.13).
 *
 * A pattern comes with a mask that says which of its characters were
 * quoted; a quoted character always stands for itself.  * matches any
 * string, ? any character, and a bracket expression one character of a set
 * ([abc], [a-z], [!...], [[:class:]]).  A [ with no closing ] is an ordinary
 * character.
 *
 * Pathname expansion matches a field component by component against the
 * directories it names.  A component with no pattern character is taken as
 * it is; a leading period is matched only by a period written in the
 * pattern.  A field that matches nothing is left as it was, and the matches
 * of a field are sorted.
 */

#include "userland/base/sh/glob.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* A test of a character class, [:name:]. */
struct char_class {
	const char *name;
	int (*test)(int);
};

/* The paths a field matched. */
struct matches {
	char **items;
	size_t count;
};

/* The pattern of a field while its components are expanded. */
struct glob_pattern {
	const char *text;
	const unsigned char *quoted;
	size_t length;
	struct matches *matches;
};

/* The character classes a bracket expression names. */
static const struct char_class char_classes[] = {
	{ "alnum", isalnum },
	{ "alpha", isalpha },
	{ "blank", isblank },
	{ "cntrl", iscntrl },
	{ "digit", isdigit },
	{ "graph", isgraph },
	{ "lower", islower },
	{ "print", isprint },
	{ "punct", ispunct },
	{ "space", isspace },
	{ "upper", isupper },
	{ "xdigit", isxdigit },
	{ NULL, NULL }
};

static int glob_field(struct sh_field_list *output, struct sh_field_list *list, size_t field);
static int has_meta(const char *pattern, const unsigned char *quoted, size_t length);
static int output_append(struct sh_field_list *output, char *field);
static int expand_path(const struct glob_pattern *pattern, size_t position, const char *prefix);
static int expand_literal(const struct glob_pattern *pattern, size_t position, size_t end, const char *prefix);
static int expand_directory(const struct glob_pattern *pattern, size_t position, size_t end, const char *prefix);
static int entry_matches(const struct glob_pattern *pattern, size_t position, size_t end, const char *name);
static size_t next_component(const struct glob_pattern *pattern, size_t end);
static int match_end(const struct glob_pattern *pattern, const char *prefix);
static int match_append(struct matches *matches, const char *path);
static char *join_path(const char *prefix, const char *name, size_t name_length);
static int bracket_match(const char *pattern, const unsigned char *quoted, size_t length, unsigned char value, size_t *end);
static int class_match(const char *pattern, size_t length, size_t at, unsigned char value, size_t *end);
static int match_component(const char *pattern, const unsigned char *quoted, size_t length, const char *name);
static int match_star(const char *pattern, const unsigned char *quoted, size_t length, const char *name);
static void matches_free(struct matches *matches);
static int compare_paths(const void *left, const void *right);

/*
 * Expands the patterns among the fields into the paths they match.
 * Returns 1, or 0 with *error_text when memory ran out (the fields are
 * then left as they were).
 */
int
sh_glob_fields(
	struct sh_field_list *list,
	const char **error_text)
{
	struct sh_field_list output;
	size_t field;
	int appended;

	/* Each field, or what it matched, goes to a new list. */
	*error_text = NULL;
	memset(&output, 0, sizeof(output));
	for (field = 0; field < list->count; field++) {
		appended = glob_field(&output, list, field);
		if (!appended) {
			*error_text = "out of memory";
			sh_fields_free(&output);
			return 0;
		}
	}

	/* Succeeded: the new list replaces the old. */
	sh_fields_free(list);
	*list = output;
	return 1;
}

/*
 * Matches the whole of a string against a pattern.
 *
 * Unlike pathname expansion this matches the string as a whole: a slash and
 * a leading period are ordinary characters here, because what is matched is
 * a word rather than a path.  A null mask means nothing was quoted.
 */
int
sh_glob_match(
	const char *pattern,
	const unsigned char *quoted,
	const char *name)
{
	unsigned char *plain;
	size_t length;
	int result;

	/* The pattern with its mask. */
	length = strlen(pattern);
	if (quoted != NULL)
		return match_component(pattern, quoted, length, name);

	/* A pattern with no mask was written with nothing quoted in it. */
	plain = calloc(length + 1U, sizeof(*plain));
	if (plain == NULL)
		return 0;
	result = match_component(pattern, plain, length, name);
	free(plain);

	/* Succeeded: whether it matched. */
	return result;
}

/*
 * Appends a field, or the paths it matches, to the output.  The field (or
 * each path) moves to the output.  Returns 0 when memory ran out.
 */
static int
glob_field(
	struct sh_field_list *output,
	struct sh_field_list *list,
	size_t field)
{
	struct glob_pattern pattern;
	struct matches matches;
	const char *text;
	size_t match;
	int meta;
	int ok;

	/* A field with no pattern character stands for itself. */
	text = list->fields[field];
	memset(&matches, 0, sizeof(matches));
	pattern.text = text;
	pattern.quoted = list->quoted[field];
	pattern.length = strlen(text);
	pattern.matches = &matches;
	meta = has_meta(pattern.text, pattern.quoted, pattern.length);

	/* The paths it matches, from the root or from here. */
	if (meta && text[0] == '/')
		ok = expand_path(&pattern, 1, "/");
	else if (meta)
		ok = expand_path(&pattern, 0, "");
	else
		ok = 1;
	if (!ok) {
		matches_free(&matches);
		return 0;
	}

	/* No match: the field itself. */
	if (matches.count == 0) {
		free(matches.items);
		ok = output_append(output, list->fields[field]);
		if (!ok)
			return 0;
		list->fields[field] = NULL;
		return 1;
	}

	/* The matches, sorted. */
	qsort(matches.items, matches.count, sizeof(*matches.items),
	      compare_paths);
	for (match = 0; match < matches.count; match++) {
		ok = output_append(output, matches.items[match]);
		if (!ok) {
			matches_free(&matches);
			return 0;
		}

		/* The output owns the match now. */
		matches.items[match] = NULL;
	}

	/* The list itself goes. */
	free(matches.items);

	/* Succeeded. */
	return 1;
}

/*
 * Reports whether a pattern has an unquoted pattern character.  * and ?
 * are; [ is one only when an unquoted ] closes it (in the same component),
 * which keeps a word like "[" (the test command) from reading the
 * directory at all.
 */
static int
has_meta(
	const char *pattern,
	const unsigned char *quoted,
	size_t length)
{
	size_t index;
	size_t close;

	/* Looks for a *, a ? or a [ that closes, none of them quoted. */
	for (index = 0; index < length; index++) {
		/* Quoted characters and ordinary ones. */
		if (quoted[index])
			continue;
		if (pattern[index] == '*' || pattern[index] == '?')
			return 1;
		if (pattern[index] != '[')
			continue;

		/* A ! and a ] first in the set do not close it. */
		close = index + 1;
		if (close < length && !quoted[close] && pattern[close] == '!')
			close++;
		if (close < length && pattern[close] == ']')
			close++;

		/* An unquoted ] before the next slash does. */
		for (; close < length; close++) {
			if (pattern[close] == '/')
				break;
			if (pattern[close] == ']' && !quoted[close])
				return 1;
		}
	}

	/* No pattern: the word stands for itself. */
	return 0;
}

/* Appends a field to a list; the field moves to it. */
static int
output_append(
	struct sh_field_list *output,
	char *field)
{
	char **fields;
	unsigned char **quoted;
	size_t count;

	/* Room for one more field and its (empty) mask. */
	count = output->count + 1U;
	fields = realloc(output->fields, count * sizeof(*output->fields));
	if (fields == NULL)
		return 0;
	output->fields = fields;
	quoted = realloc(output->quoted, count * sizeof(*output->quoted));
	if (quoted == NULL)
		return 0;
	output->quoted = quoted;

	/* The field. */
	output->fields[output->count] = field;
	output->quoted[output->count] = NULL;
	output->count = count;

	/* Succeeded. */
	return 1;
}

/*
 * Expands the pattern from a position (the start of a component) below a
 * prefix already matched.  Returns 0 when memory ran out.
 */
static int
expand_path(
	const struct glob_pattern *pattern,
	size_t position,
	const char *prefix)
{
	size_t end;
	int meta;

	/* The whole pattern matched: the prefix is a match. */
	if (position == pattern->length)
		return match_end(pattern, prefix);

	/* The component, up to the next slash. */
	end = position;
	while (end < pattern->length && pattern->text[end] != '/')
		end++;

	/* A component with no pattern is taken as written. */
	meta = has_meta(pattern->text + position, pattern->quoted + position,
			end - position);
	if (!meta)
		return expand_literal(pattern, position, end, prefix);

	/* Succeeded: the entries of the directory it is matched against. */
	return expand_directory(pattern, position, end, prefix);
}

/* Expands below a component that has no pattern character. */
static int
expand_literal(
	const struct glob_pattern *pattern,
	size_t position,
	size_t end,
	const char *prefix)
{
	char *next;
	int ok;

	/* The component joined to the prefix. */
	next = join_path(prefix, pattern->text + position, end - position);
	if (next == NULL)
		return 0;

	/* Succeeded: the rest below it. */
	ok = expand_path(pattern, next_component(pattern, end), next);
	free(next);
	return ok;
}

/* Expands a pattern component against the entries of the prefix. */
static int
expand_directory(
	const struct glob_pattern *pattern,
	size_t position,
	size_t end,
	const char *prefix)
{
	struct dirent *entry;
	DIR *directory;
	char *next;
	int matched;
	int ok;

	/* The directory; one that cannot be read matches nothing. */
	if (prefix[0] == '\0')
		directory = opendir(".");
	else
		directory = opendir(prefix);
	if (directory == NULL)
		return 1;

	/* Each entry of the directory that the component matches. */
	for (;;) {
		/* The next entry. */
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* An entry the component does not match. */
		matched = entry_matches(pattern, position, end, entry->d_name);
		if (!matched)
			continue;

		/* The rest of the pattern below the entry. */
		next = join_path(prefix, entry->d_name, strlen(entry->d_name));
		if (next == NULL) {
			(void)closedir(directory);
			return 0;
		}

		/* The rest of the pattern is matched below the entry. */
		ok = expand_path(pattern, next_component(pattern, end), next);
		free(next);
		if (!ok) {
			(void)closedir(directory);
			return 0;
		}
	}

	/* Every entry has been tried. */
	(void)closedir(directory);

	/* Succeeded. */
	return 1;
}

/*
 * Reports whether a directory entry matches a component.  A leading period
 * is matched only by a period written first in the component.
 */
static int
entry_matches(
	const struct glob_pattern *pattern,
	size_t position,
	size_t end,
	const char *name)
{
	/* A hidden name needs a period in the pattern. */
	if (name[0] == '.' && end == position)
		return 0;
	if (name[0] == '.' && pattern->text[position] != '.')
		return 0;

	/* Succeeded: whether the component matches. */
	return match_component(pattern->text + position,
			       pattern->quoted + position, end - position,
			       name);
}

/* Returns the start of the component after one that ends at end. */
static size_t
next_component(
	const struct glob_pattern *pattern,
	size_t end)
{
	/* After the slash, unless the pattern ended. */
	if (end < pattern->length)
		return end + 1U;

	/* The end. */
	return end;
}

/*
 * Adds what the whole pattern matched.  A pattern that ends in a slash
 * matches only directories, and the match keeps the slash.
 */
static int
match_end(
	const struct glob_pattern *pattern,
	const char *prefix)
{
	struct stat status;
	char *directory;
	int is_directory;
	int error;
	int ok;

	/* Without a trailing slash, the path itself. */
	if (pattern->text[pattern->length - 1U] != '/')
		return match_append(pattern->matches, prefix);

	/* With one, a directory only. */
	error = stat(prefix, &status);
	if (error != 0)
		return 1;
	is_directory = S_ISDIR(status.st_mode);
	if (!is_directory)
		return 1;

	/* Succeeded: the path and its slash. */
	directory = join_path(prefix, "", 0);
	if (directory == NULL)
		return 0;
	ok = match_append(pattern->matches, directory);
	free(directory);
	return ok;
}

/* Adds a copy of a path to the matches. */
static int
match_append(
	struct matches *matches,
	const char *path)
{
	char **larger;
	char *copy;

	/* The copy. */
	copy = join_path("", path, strlen(path));
	if (copy == NULL)
		return 0;

	/* Room for it. */
	larger = realloc(matches->items,
			 (matches->count + 1U) * sizeof(*matches->items));
	if (larger == NULL) {
		free(copy);
		return 0;
	}

	/* The copy joins the matches. */
	matches->items = larger;
	matches->items[matches->count] = copy;
	matches->count++;

	/* Succeeded. */
	return 1;
}

/* Joins a prefix and a name with a slash (unless the prefix ends in one). */
static char *
join_path(
	const char *prefix,
	const char *name,
	size_t name_length)
{
	size_t prefix_length;
	size_t slash;
	char *path;

	/* A slash is needed after a prefix that does not end in one. */
	prefix_length = strlen(prefix);
	slash = 0;
	if (prefix_length != 0 && prefix[prefix_length - 1U] != '/')
		slash = 1;

	/* The joined path. */
	path = malloc(prefix_length + slash + name_length + 1U);
	if (path == NULL)
		return NULL;
	memcpy(path, prefix, prefix_length);
	if (slash)
		path[prefix_length++] = '/';
	memcpy(path + prefix_length, name, name_length);
	path[prefix_length + name_length] = '\0';

	/* Succeeded: the path, which the caller frees. */
	return path;
}

/*
 * Matches one character against the bracket expression at the start of the
 * pattern: [abc], [a-z], [!...], [[:class:]], and a ] first in the set
 * stands for itself.  A quoted character in the set is literal.  Returns 1
 * or 0, with *end after the closing bracket, or -1 when the brackets do not
 * close, and the '[' is then an ordinary character.
 */
static int
bracket_match(
	const char *pattern,
	const unsigned char *quoted,
	size_t length,
	unsigned char value,
	size_t *end)
{
	size_t start;
	size_t close;
	unsigned char first;
	unsigned char last;
	int negate;
	int matched;
	int in_class;

	/* A ! first makes it the characters not in the set. */
	start = 1;
	negate = 0;
	if (start < length && !quoted[start] && pattern[start] == '!') {
		negate = 1;
		start++;
	}

	/* Each member of the set up to the closing bracket; a ] first is a member. */
	matched = 0;
	close = start;
	for (;;) {
		/* No closing bracket. */
		if (close >= length)
			return -1;

		/* An unquoted ], not first, closes the set. */
		if (pattern[close] == ']' && !quoted[close] && close > start)
			break;

		/* A character class, [:name:]. */
		if (pattern[close] == '[' && !quoted[close]) {
			in_class = class_match(pattern, length, close, value,
					       &close);
			if (in_class > 0)
				matched = 1;
			if (in_class >= 0)
				continue;
		}

		/* A character, or a range of them. */
		first = (unsigned char)pattern[close];
		last = first;
		close++;
		if (close + 1 < length && pattern[close] == '-' &&
		    !quoted[close] &&
		    !(pattern[close + 1] == ']' && !quoted[close + 1])) {
			last = (unsigned char)pattern[close + 1];
			close += 2;
		}

		/* The character is in the range. */
		if (value >= first && value <= last)
			matched = 1;
	}

	/* Succeeded: whether the character is in the set. */
	*end = close + 1;
	if (negate)
		return !matched;
	return matched;
}

/*
 * Matches a character against a [:name:] at the position at, whose [ has
 * been seen.  Returns 1 or 0 with *end after it, or -1 when it is not a
 * class (and the [ is an ordinary character of the set).
 */
static int
class_match(
	const char *pattern,
	size_t length,
	size_t at,
	unsigned char value,
	size_t *end)
{
	const char *name;
	size_t name_length;
	size_t class_length;
	int compare;
	int index;
	int result;

	/* [: begins it. */
	if (at + 1 >= length || pattern[at + 1] != ':')
		return -1;

	/* :] ends it. */
	name = pattern + at + 2;
	name_length = 0;
	while (at + 2 + name_length + 1 < length && name[name_length] != ':')
		name_length++;
	if (at + 2 + name_length + 1 >= length)
		return -1;
	if (name[name_length + 1] != ']')
		return -1;
	*end = at + name_length + 4;

	/* A class the name matches; an unknown name matches nothing. */
	for (index = 0; char_classes[index].name != NULL; index++) {
		class_length = strlen(char_classes[index].name);
		if (class_length != name_length)
			continue;
		compare = strncmp(char_classes[index].name, name, name_length);
		if (compare != 0)
			continue;
		result = char_classes[index].test(value);
		if (result)
			return 1;
		return 0;
	}

	/* Succeeded: not in it. */
	return 0;
}

/* Matches a whole name against a pattern. */
static int
match_component(
	const char *pattern,
	const unsigned char *quoted,
	size_t length,
	const char *name)
{
	size_t index;
	int matched;

	/* An empty pattern matches only an empty name. */
	if (length == 0)
		return *name == '\0';

	/* * matches any string. */
	if (!quoted[0] && pattern[0] == '*')
		return match_star(pattern, quoted, length, name);

	/* Anything else needs a character. */
	if (*name == '\0')
		return 0;

	/* ? matches any character. */
	if (!quoted[0] && pattern[0] == '?') {
		matched = match_component(pattern + 1, quoted + 1, length - 1U, name + 1);
		return matched;
	}

	/* A bracket expression; one that does not close is a plain '['. */
	if (!quoted[0] && pattern[0] == '[') {
		matched = bracket_match(pattern, quoted, length,
					(unsigned char)*name, &index);
		if (matched == 0)
			return 0;
		if (matched == 1) {
			matched = match_component(pattern + index, quoted + index, length - index, name + 1);
			return matched;
		}
	}

	/* An ordinary character matches itself. */
	if (pattern[0] != *name)
		return 0;

	/* Succeeded: whether the rest matches. */
	matched = match_component(pattern + 1, quoted + 1, length - 1U, name + 1);
	return matched;
}

/* Matches a pattern that starts with *: the rest, at every position. */
static int
match_star(
	const char *pattern,
	const unsigned char *quoted,
	size_t length,
	const char *name)
{
	int matched;

	/* The * takes one more character of the name each time the rest fails. */
	for (;;) {
		/* The rest of the pattern here. */
		matched = match_component(pattern + 1, quoted + 1, length - 1U,
					  name);
		if (matched)
			return 1;

		/* Or one character further on. */
		if (*name == '\0')
			break;
		name++;
	}

	/* No position matched. */
	return 0;
}

/* Frees the matches. */
static void
matches_free(
	struct matches *matches)
{
	size_t index;

	/* Each path, then the array. */
	for (index = 0; index < matches->count; index++)
		free(matches->items[index]);
	free(matches->items);
	matches->items = NULL;
	matches->count = 0;
}

/* Orders two paths, for qsort. */
static int
compare_paths(
	const void *left,
	const void *right)
{
	const char *const *left_path;
	const char *const *right_path;

	/* Byte order, as dash sorts. */
	left_path = left;
	right_path = right;
	return strcmp(*left_path, *right_path);
}
