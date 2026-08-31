/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland alias component.
 */

#include "userland/base/sh/alias.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct shell_alias {
	struct shell_alias *next;
	char *name;
	char *value;
};

static struct shell_alias *aliases;

static struct shell_alias *find_alias(const char *name);
static int valid_name(const char *name);
static char *copy_string(const char *source);
static int assignment_word(const char *text);
static int token_unquoted(const struct sh_token *token);
static int splice_alias(struct sh_token_list *list, size_t position, const char *value, const char **error_text);

/*
 * Implements the sh alias get operation.
 */
const char *
sh_alias_get(
	const char *name)
{
	struct shell_alias *item;

	item = find_alias(name);

	/* Returns the computed result. */
	return item == NULL ? NULL : item->value;
}

/*
 * Implements the sh alias set operation.
 */
int
sh_alias_set(
	const char *name,
	const char *value)
{
	struct shell_alias *item;
	char *copy;

	/* Handles a failed valid name operation. */
	if (!valid_name(name)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	copy = copy_string(value);

	/* Handles the copy availability. */
	if (copy == NULL)
		return -1;
	item = find_alias(name);

	/* Handles the item availability. */
	if (item == NULL) {
		item = calloc(1, sizeof(*item));

		/* Handles the item availability. */
		if (item == NULL) {
			free(copy);

			/* Reports operation failure. */
			return -1;
		}
		item->name = copy_string(name);

		/* Handles the name availability. */
		if (item->name == NULL) {
			free(copy);
			free(item);

			/* Reports operation failure. */
			return -1;
		}
		item->next = aliases;
		aliases = item;
	}
	free(item->value);
	item->value = copy;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sh alias unset operation.
 */
int
sh_alias_unset(
	const char *name)
{
	struct shell_alias *item;
	struct shell_alias **link;

	/* Continue while the operation condition remains true. */
	link = &aliases;
	while (*link != NULL) {

		item = *link;

		/* Selects the matching value. */
		if (strcmp(item->name, name) != 0) {
			link = &item->next;
			continue;
		}
		*link = item->next;
		free(item->name);
		free(item->value);
		free(item);

		/* Reports successful completion. */
		return 0;
	}
	errno = ENOENT;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the sh alias clear operation.
 */
void
sh_alias_clear(
	void)
{
	struct shell_alias *next;

	/* Continue while the operation condition remains true. */
	while (aliases != NULL) {

		next = aliases->next;
		free(aliases->name);
		free(aliases->value);
		free(aliases);
		aliases = next;
	}
}

/*
 * Implements the sh alias print operation.
 */
void
sh_alias_print(
	void)
{
	struct shell_alias *item;

	/* Process each linked entry. */
	for (item = aliases; item != NULL; item = item->next)
		printf("alias %s='%s'\n", item->name, item->value);
}

/*
 * Implements the sh alias expand operation.
 */
int
sh_alias_expand(
	struct sh_token_list *list,
	const char **error_text)
{
	const char *value;
	struct sh_token *token;
	size_t position;
	int command_position;
	int skip_redirection_word;
	unsigned expansions;

	/* Process each remaining element. */
	position = 0;
	command_position = 1;
	skip_redirection_word = 0;
	expansions = 0;
	while (position + 1U < list->count) {
				token = &list->tokens[position];

		/* Handles the token condition. */
		if (token->type == SH_TOKEN_INPUT ||
		    token->type == SH_TOKEN_OUTPUT ||
		    token->type == SH_TOKEN_APPEND) {
			skip_redirection_word = 1;
			position++;
			continue;
		}

		/* Handles the token condition. */
		if (token->type == SH_TOKEN_SEMI ||
		    token->type == SH_TOKEN_AMP ||
		    token->type == SH_TOKEN_AND_IF ||
		    token->type == SH_TOKEN_OR_IF ||
		    token->type == SH_TOKEN_PIPE) {
			command_position = 1;
			skip_redirection_word = 0;
			position++;
			continue;
		}

		/* Handles the token condition. */
		if (token->type != SH_TOKEN_WORD) {
			position++;
			continue;
		}

		/* Handles the skip redirection word condition. */
		if (skip_redirection_word) {
			skip_redirection_word = 0;
			position++;
			continue;
		}

		/* Handles a failed assignment word operation. */
		if (command_position && assignment_word(token->text)) {
			position++;
			continue;
		}

		/* Handles the command position condition. */
		if (command_position && token_unquoted(token)) {
						value = sh_alias_get(token->text);

			/* Handles the value availability. */
			if (value != NULL) {
				/* Handles the expansions condition. */
				if (++expansions > 32U) {
					*error_text =
					    "recursive alias expansion";

					/* Reports successful completion. */
					return 0;
				}

				/* Handles an operation failure. */
				if (!splice_alias(list, position, value,
						  error_text))

					/* Reports successful completion. */
					return 0;
				continue;
			}
		}
		command_position = 0;
		position++;
	}
	*error_text = NULL;
	/* Reports operation failure. */
	return 1;
}

/* Supports the find alias operation. */
static struct shell_alias *
find_alias(
	const char *name)
{
	struct shell_alias *item;

	/* Process each linked entry. */
	for (item = aliases; item != NULL; item = item->next)

		/* Selects the matching value. */
		if (strcmp(item->name, name) == 0)
			return item;

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the valid name operation. */
static int
valid_name(
	const char *name)
{
	/* Validates the current name. */
	if (*name == '\0')
		return 0;

	/* Continue while the operation condition remains true. */
	while (*name != '\0') {
		/* Validates the current name. */
		if (*name == '/' || *name == '=' || *name == ' ' ||
		    *name == '\t' || *name == ';' || *name == '|' ||
		    *name == '&')
			/* Reports successful completion. */
			return 0;
		name++;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the copy string operation. */
static char *
copy_string(
	const char *source)
{
	size_t length;
	char *copy;

	length = strlen(source) + 1U;
	copy = malloc(length);

	/* Handles the copy availability. */
	if (copy != NULL)
		memcpy(copy, source, length);

	/* Returns the computed result. */
	return copy;
}

/* Supports the assignment word operation. */
static int
assignment_word(
	const char *text)
{
	const char *cursor;

	cursor = text;

	/* Checks the current cursor position. */
	if (!((*cursor >= 'A' && *cursor <= 'Z') ||
	      (*cursor >= 'a' && *cursor <= 'z') || *cursor == '_'))

		/* Reports successful completion. */
		return 0;

	/* Continue while the operation condition remains true. */
	while ((*++cursor >= 'A' && *cursor <= 'Z') ||
	       (*cursor >= 'a' && *cursor <= 'z') || *cursor == '_' ||
	       (*cursor >= '0' && *cursor <= '9'))
		;

	/* Returns the computed result. */
	return *cursor == '=';
}

/* Supports the token unquoted operation. */
static int
token_unquoted(
	const struct sh_token *token)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < token->length; index++)

		/* Handles the token condition. */
		if (token->quote[index] != SH_QUOTE_UNQUOTED)
			return 0;

	/* Reports operation failure. */
	return 1;
}

/* Supports the splice alias operation. */
static int
splice_alias(
	struct sh_token_list *list,
	size_t position,
	const char *value,
	const char **error_text)
{
	struct sh_token_list replacement;
	struct sh_token *larger;
	size_t inserted, tail;

	/* Handles an operation failure. */
	if (!sh_lex(value, &replacement, error_text))
		return 0;
	inserted = replacement.count - 1U;
	tail = list->count - position - 1U;

	/* Handles the inserted condition. */
	if (inserted > (size_t)-1 - list->count + 1U) {
		sh_tokens_free(&replacement);
		*error_text = "alias expansion is too large";
		/* Reports successful completion. */
		return 0;
	}
	larger = realloc(list->tokens,
			 (list->count - 1U + inserted) * sizeof(*larger));

	/* Handles the larger availability. */
	if (larger == NULL) {
		sh_tokens_free(&replacement);
		*error_text = "out of memory";
		/* Reports successful completion. */
		return 0;
	}
	list->tokens = larger;
	free(list->tokens[position].text);
	free(list->tokens[position].quote);
	memmove(list->tokens + position + inserted,
		list->tokens + position + 1U, tail * sizeof(*list->tokens));
	memcpy(list->tokens + position, replacement.tokens,
	       inserted * sizeof(*list->tokens));
	list->count = list->count - 1U + inserted;
	free(replacement.tokens[inserted].text);
	free(replacement.tokens[inserted].quote);
	free(replacement.tokens);

	/* Reports operation failure. */
	return 1;
}
