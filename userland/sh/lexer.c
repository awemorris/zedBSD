/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/sh/lexer.h"

#include <stdlib.h>
#include <string.h>

struct word_buffer {
	char *data;
	size_t length;
	size_t capacity;
};

static int
word_append(struct word_buffer *word, char value)
{
	char *larger;
	size_t capacity;
	if (word->length + 1U < word->capacity) {
		word->data[word->length++] = value;
		return 1;
	}
	capacity = word->capacity == 0 ? 16U : word->capacity * 2U;
	if (capacity <= word->capacity)
		return 0;
	larger = realloc(word->data, capacity);
	if (larger == NULL)
		return 0;
	word->data = larger;
	word->capacity = capacity;
	word->data[word->length++] = value;
	return 1;
}

static int
token_append(struct sh_token_list *list, enum sh_token_type type, char *text)
{
	struct sh_token *larger;
	if (list->count == (size_t)-1 / sizeof(*list->tokens))
		return 0;
	larger = realloc(list->tokens,
	    (list->count + 1U) * sizeof(*list->tokens));
	if (larger == NULL)
		return 0;
	list->tokens = larger;
	list->tokens[list->count].type = type;
	list->tokens[list->count].text = text;
	list->count++;
	return 1;
}

static int
is_operator(char value)
{
	return value == ';' || value == '&' || value == '|' ||
	    value == '<' || value == '>';
}

static int
lex_word(const char **cursor, struct sh_token_list *list,
    const char **error_text)
{
	const char *text = *cursor;
	struct word_buffer word = { 0 };
	int quoted = 0;
	int produced = 0;
	while (*text != '\0' && *text != ' ' && *text != '\t' &&
	    !is_operator(*text)) {
		if (*text == '\\') {
			text++;
			if (*text == '\0') {
				*error_text = "trailing backslash";
				free(word.data);
				return 0;
			}
			if (!word_append(&word, *text++))
				goto no_memory;
			produced = 1;
			continue;
		}
		if (*text == '\'' || *text == '"') {
			char quote = *text++;
			quoted = 1;
			while (*text != '\0' && *text != quote) {
				if (quote == '"' && *text == '\\') {
					text++;
					if (*text == '\0')
						break;
				}
				if (!word_append(&word, *text++))
					goto no_memory;
				produced = 1;
			}
			if (*text != quote) {
				*error_text = "unterminated quote";
				free(word.data);
				return 0;
			}
			text++;
			continue;
		}
		if (!word_append(&word, *text++))
			goto no_memory;
		produced = 1;
	}
	if (!produced && !quoted) {
		*error_text = "empty word";
		free(word.data);
		return 0;
	}
	if (!word_append(&word, '\0'))
		goto no_memory;
	if (!token_append(list, SH_TOKEN_WORD, word.data))
		goto no_memory;
	*cursor = text;
	return 1;
no_memory:
	*error_text = "out of memory";
	free(word.data);
	return 0;
}

int
sh_lex(const char *text, struct sh_token_list *list, const char **error_text)
{
	memset(list, 0, sizeof(*list));
	*error_text = NULL;
	while (*text != '\0') {
		enum sh_token_type type;
		while (*text == ' ' || *text == '\t')
			text++;
		if (*text == '\0' || *text == '#')
			break;
		if (!is_operator(*text)) {
			if (!lex_word(&text, list, error_text))
				goto failed;
			continue;
		}
		switch (*text++) {
		case ';': type = SH_TOKEN_SEMI; break;
		case '&':
			if (*text == '&') { text++; type = SH_TOKEN_AND_IF; }
			else type = SH_TOKEN_AMP;
			break;
		case '|':
			if (*text == '|') { text++; type = SH_TOKEN_OR_IF; }
			else type = SH_TOKEN_PIPE;
			break;
		case '<': type = SH_TOKEN_INPUT; break;
		default:
			if (*text == '>') { text++; type = SH_TOKEN_APPEND; }
			else type = SH_TOKEN_OUTPUT;
			break;
		}
		if (!token_append(list, type, NULL)) {
			*error_text = "out of memory";
			goto failed;
		}
	}
	if (!token_append(list, SH_TOKEN_END, NULL)) {
		*error_text = "out of memory";
		goto failed;
	}
	return 1;
failed:
	sh_tokens_free(list);
	return 0;
}

void
sh_tokens_free(struct sh_token_list *list)
{
	size_t index;
	for (index = 0; index < list->count; index++)
		free(list->tokens[index].text);
	free(list->tokens);
	list->tokens = NULL;
	list->count = 0;
}
