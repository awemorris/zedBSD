/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/sh/alias.h"

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

static char *copy_string(const char *source)
{
	size_t length = strlen(source) + 1U;
	char *copy = malloc(length);
	if (copy != NULL) memcpy(copy, source, length);
	return copy;
}

static int valid_name(const char *name)
{
	if (*name == '\0') return 0;
	while (*name != '\0') {
		if (*name == '/' || *name == '=' || *name == ' ' || *name == '\t' ||
		    *name == ';' || *name == '|' || *name == '&')
			return 0;
		name++;
	}
	return 1;
}

static struct shell_alias *find_alias(const char *name)
{
	struct shell_alias *item;
	for (item = aliases; item != NULL; item = item->next)
		if (strcmp(item->name, name) == 0) return item;
	return NULL;
}

const char *sh_alias_get(const char *name)
{
	struct shell_alias *item = find_alias(name);
	return item == NULL ? NULL : item->value;
}

int sh_alias_set(const char *name, const char *value)
{
	struct shell_alias *item;
	char *copy;
	if (!valid_name(name)) { errno = EINVAL; return -1; }
	copy = copy_string(value);
	if (copy == NULL) return -1;
	item = find_alias(name);
	if (item == NULL) {
		item = calloc(1, sizeof(*item));
		if (item == NULL) { free(copy); return -1; }
		item->name = copy_string(name);
		if (item->name == NULL) { free(copy); free(item); return -1; }
		item->next = aliases;
		aliases = item;
	}
	free(item->value);
	item->value = copy;
	return 0;
}

int sh_alias_unset(const char *name)
{
	struct shell_alias **link = &aliases;
	while (*link != NULL) {
		struct shell_alias *item = *link;
		if (strcmp(item->name, name) != 0) { link = &item->next; continue; }
		*link = item->next;
		free(item->name);
		free(item->value);
		free(item);
		return 0;
	}
	errno = ENOENT;
	return -1;
}

void sh_alias_clear(void)
{
	while (aliases != NULL) {
		struct shell_alias *next = aliases->next;
		free(aliases->name);
		free(aliases->value);
		free(aliases);
		aliases = next;
	}
}

void sh_alias_print(void)
{
	struct shell_alias *item;
	for (item = aliases; item != NULL; item = item->next)
		printf("alias %s='%s'\n", item->name, item->value);
}

static int token_unquoted(const struct sh_token *token)
{
	size_t index;
	for (index = 0; index < token->length; index++)
		if (token->quote[index] != SH_QUOTE_UNQUOTED) return 0;
	return 1;
}

static int assignment_word(const char *text)
{
	const char *cursor = text;
	if (!( (*cursor >= 'A' && *cursor <= 'Z') ||
	    (*cursor >= 'a' && *cursor <= 'z') || *cursor == '_')) return 0;
	while ((*++cursor >= 'A' && *cursor <= 'Z') ||
	    (*cursor >= 'a' && *cursor <= 'z') || *cursor == '_' ||
	    (*cursor >= '0' && *cursor <= '9')) ;
	return *cursor == '=';
}

static int splice_alias(struct sh_token_list *list, size_t position,
	const char *value, const char **error_text)
{
	struct sh_token_list replacement;
	struct sh_token *larger;
	size_t inserted, tail;
	if (!sh_lex(value, &replacement, error_text)) return 0;
	inserted = replacement.count - 1U;
	tail = list->count - position - 1U;
	if (inserted > (size_t)-1 - list->count + 1U) {
		sh_tokens_free(&replacement);
		*error_text = "alias expansion is too large";
		return 0;
	}
	larger = realloc(list->tokens,
	    (list->count - 1U + inserted) * sizeof(*larger));
	if (larger == NULL) {
		sh_tokens_free(&replacement);
		*error_text = "out of memory";
		return 0;
	}
	list->tokens = larger;
	free(list->tokens[position].text);
	free(list->tokens[position].quote);
	memmove(list->tokens + position + inserted, list->tokens + position + 1U,
	    tail * sizeof(*list->tokens));
	memcpy(list->tokens + position, replacement.tokens,
	    inserted * sizeof(*list->tokens));
	list->count = list->count - 1U + inserted;
	free(replacement.tokens[inserted].text);
	free(replacement.tokens[inserted].quote);
	free(replacement.tokens);
	return 1;
}

int sh_alias_expand(struct sh_token_list *list, const char **error_text)
{
	size_t position = 0;
	int command_position = 1;
	int skip_redirection_word = 0;
	unsigned expansions = 0;
	while (position + 1U < list->count) {
		struct sh_token *token = &list->tokens[position];
		if (token->type == SH_TOKEN_INPUT || token->type == SH_TOKEN_OUTPUT ||
		    token->type == SH_TOKEN_APPEND) {
			skip_redirection_word = 1;
			position++;
			continue;
		}
		if (token->type == SH_TOKEN_SEMI || token->type == SH_TOKEN_AMP ||
		    token->type == SH_TOKEN_AND_IF || token->type == SH_TOKEN_OR_IF ||
		    token->type == SH_TOKEN_PIPE) {
			command_position = 1;
			skip_redirection_word = 0;
			position++;
			continue;
		}
		if (token->type != SH_TOKEN_WORD) { position++; continue; }
		if (skip_redirection_word) {
			skip_redirection_word = 0;
			position++;
			continue;
		}
		if (command_position && assignment_word(token->text)) {
			position++;
			continue;
		}
		if (command_position && token_unquoted(token)) {
			const char *value = sh_alias_get(token->text);
			if (value != NULL) {
				if (++expansions > 32U) {
					*error_text = "recursive alias expansion";
					return 0;
				}
				if (!splice_alias(list, position, value, error_text)) return 0;
				continue;
			}
		}
		command_position = 0;
		position++;
	}
	*error_text = NULL;
	return 1;
}
