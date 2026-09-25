/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The alias table.  The parser expands an alias by reading its text as
 * input (see parser.c); this file only keeps the names and texts.
 */

#include "userland/base/sh/alias.h"
#include "userland/base/sh/shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One alias: a name and the text that replaces it.
 *
 * Both strings belong to the entry, which lives until unalias removes it or
 * an alias of the same name replaces its text.
 */
struct alias {
	struct alias *next;
	char *name;
	char *value;
};

/*
 * The aliases, newest first.
 *
 * Only the alias and unalias builtins change the list; the parser reads it
 * when a word stands where a command name may.  The parser copies the text
 * it reads, so an alias may be replaced while its text is being read.
 */
static struct alias *aliases;

static struct alias *find_alias(const char *name);
static int valid_name(const char *name);
static int compare_aliases(const void *left, const void *right);

/*
 * Returns an alias's text, or NULL.
 */
const char *
sh_alias_get(
	const char *name)
{
	struct alias *alias;

	/* Looks the name up; a name with no alias has no text. */
	alias = find_alias(name);
	if (alias == NULL)
		return NULL;

	/* Succeeded: the text. */
	return alias->value;
}

/*
 * Defines an alias, replacing one of the same name.
 */
int
sh_alias_set(
	const char *name,
	const char *value)
{
	struct alias *alias;
	int valid;

	/* Refuses a name holding characters the parser would split at. */
	valid = valid_name(name);
	if (!valid)
		return -1;

	/* Finds the alias of that name, or makes one at the head. */
	alias = find_alias(name);
	if (alias == NULL) {
		alias = sh_malloc(sizeof(*alias));
		alias->name = sh_strdup(name);
		alias->value = NULL;
		alias->next = aliases;
		aliases = alias;
	}

	/* Replaces its text. */
	free(alias->value);
	alias->value = sh_strdup(value);

	/* Succeeded: the alias holds the text. */
	return 0;
}

/*
 * Removes an alias.
 */
int
sh_alias_unset(
	const char *name)
{
	struct alias **link;
	struct alias *alias;
	int compare;

	/* Finds the link that points at the alias of that name. */
	link = &aliases;
	while (*link != NULL) {
		compare = strcmp((*link)->name, name);
		if (compare == 0)
			break;
		link = &(*link)->next;
	}

	/* Reports a name that has no alias. */
	alias = *link;
	if (alias == NULL)
		return -1;

	/* Unlinks and frees it. */
	*link = alias->next;
	free(alias->name);
	free(alias->value);
	free(alias);

	/* Succeeded: the name has no alias now. */
	return 0;
}

/*
 * Removes every alias.
 */
void
sh_alias_clear(
	void)
{
	struct alias *alias;

	/* Frees the list from its head. */
	while (aliases != NULL) {
		alias = aliases;
		aliases = alias->next;
		free(alias->name);
		free(alias->value);
		free(alias);
	}
}

/*
 * Prints one alias, or every alias in the order of their names, as
 * name='text' (as dash quotes it).
 */
void
sh_alias_print(
	const char *name)
{
	struct alias **sorted;
	struct alias *alias;
	size_t count;
	size_t index;

	/* One alias, when a name is given and has one. */
	if (name != NULL) {
		alias = find_alias(name);
		if (alias == NULL)
			return;
		printf("%s=", alias->name);
		sh_print_quoted_always(alias->value);
		putchar('\n');
		return;
	}

	/* Counts the aliases. */
	count = 0;
	for (alias = aliases; alias != NULL; alias = alias->next)
		count++;

	/* Collects them into an array. */
	sorted = sh_malloc((count + 1U) * sizeof(*sorted));
	count = 0;
	for (alias = aliases; alias != NULL; alias = alias->next)
		sorted[count++] = alias;

	/* Sorts them by name. */
	qsort(sorted, count, sizeof(*sorted), compare_aliases);

	/* Prints each one. */
	for (index = 0; index < count; index++) {
		printf("%s=", sorted[index]->name);
		sh_print_quoted_always(sorted[index]->value);
		putchar('\n');
	}

	/* The sorted copy was only for the listing. */
	free(sorted);
}

/* Finds an alias by name. */
static struct alias *
find_alias(
	const char *name)
{
	struct alias *alias;
	int compare;

	/* Walks the list for the name. */
	for (alias = aliases; alias != NULL; alias = alias->next) {
		compare = strcmp(alias->name, name);
		if (compare == 0)
			return alias;
	}

	/* No alias has that name. */
	return NULL;
}

/* Reports whether a word may name an alias. */
static int
valid_name(
	const char *name)
{
	const char *cursor;
	const char *special;

	/* An empty name names nothing. */
	if (*name == '\0')
		return 0;

	/* Refuses any character the parser treats specially. */
	for (cursor = name; *cursor != '\0'; cursor++) {
		special = strchr(" \t\n;&|<>()$`\\\"'=", *cursor);
		if (special != NULL)
			return 0;
	}

	/* Succeeded: the word may name an alias. */
	return 1;
}

/* Orders aliases by name, for qsort. */
static int
compare_aliases(
	const void *left,
	const void *right)
{
	const struct alias *const *a;
	const struct alias *const *b;
	int order;

	/* Compares the names bytewise. */
	a = left;
	b = right;
	order = strcmp((*a)->name, (*b)->name);

	/* Succeeded: their order. */
	return order;
}
