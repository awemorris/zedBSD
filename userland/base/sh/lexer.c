/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/sh/lexer.h"

#include <stdlib.h>
#include <string.h>

struct word_buffer {
	char *data;
	unsigned char *quote;
	size_t length;
	size_t capacity;
};

static int
word_reserve(struct word_buffer *word)
{
	char *larger;
	unsigned char *quote;
	size_t capacity;
	if (word->length + 1U < word->capacity)
		return 1;
	capacity = word->capacity == 0 ? 16U : word->capacity * 2U;
	if (capacity <= word->capacity)
		return 0;
	larger = realloc(word->data, capacity);
	if (larger == NULL)
		return 0;
	word->data = larger;
	quote = realloc(word->quote, capacity);
	if (quote == NULL)
		return 0;
	word->data = larger;
	word->quote = quote;
	word->capacity = capacity;
	return 1;
}

static int
word_append(struct word_buffer *word, char value, enum sh_quote_type quote)
{
	if (!word_reserve(word))
		return 0;
	word->data[word->length++] = value;
	word->quote[word->length - 1U] = (unsigned char)quote;
	return 1;
}

static int
token_append(struct sh_token_list *list, enum sh_token_type type, char *text,
    unsigned char *quote, size_t length)
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
	list->tokens[list->count].quote = quote;
	list->tokens[list->count].length = length;
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
		if (text[0] == '$' && text[1] == '(') {
			int depth = 1;
			char inner_quote = '\0';
			if (!word_append(&word, *text++, SH_QUOTE_UNQUOTED) ||
			    !word_append(&word, *text++, SH_QUOTE_UNQUOTED))
				goto no_memory;
			while (*text != '\0' && depth != 0) {
				char value = *text++;
				if (value == '\\' && *text != '\0') {
					if (!word_append(&word, value, SH_QUOTE_UNQUOTED) ||
					    !word_append(&word, *text++, SH_QUOTE_UNQUOTED))
						goto no_memory;
					continue;
				}
				if ((value == '\'' || value == '"') &&
				    (inner_quote == '\0' || inner_quote == value))
					inner_quote = inner_quote == '\0' ? value : '\0';
				if (inner_quote == '\0') {
					if (value == '(') depth++;
					if (value == ')') depth--;
				}
				if (!word_append(&word, value, SH_QUOTE_UNQUOTED))
					goto no_memory;
			}
			if (depth != 0) {
				*error_text = "unterminated command substitution";
				free(word.data);
				free(word.quote);
				return 0;
			}
			produced = 1;
			continue;
		}
		if (*text == '\\') {
			text++;
			if (*text == '\0') {
				*error_text = "trailing backslash";
				free(word.data);
				free(word.quote);
				return 0;
			}
			if (!word_append(&word, *text++, SH_QUOTE_ESCAPED))
				goto no_memory;
			produced = 1;
			continue;
		}
		if (*text == '\'' || *text == '"') {
			char quote = *text++;
			quoted = 1;
			while (*text != '\0' && *text != quote) {
				int escaped = 0;
				if (quote == '"' && *text == '\\') {
					if (text[1] == '$' || text[1] == '`' ||
					    text[1] == '"' || text[1] == '\\') {
						text++;
						escaped = 1;
					} else if (text[1] == '\n') {
						text += 2;
						continue;
					} else {
						if (!word_append(&word, *text++,
						    SH_QUOTE_DOUBLE))
							goto no_memory;
						produced = 1;
						continue;
					}
				}
				if (!word_append(&word, *text++, escaped ?
				    SH_QUOTE_ESCAPED : quote == '\'' ?
				    SH_QUOTE_SINGLE : SH_QUOTE_DOUBLE))
					goto no_memory;
				produced = 1;
			}
			if (*text != quote) {
				*error_text = "unterminated quote";
				free(word.data);
				free(word.quote);
				return 0;
			}
			text++;
			continue;
		}
		if (!word_append(&word, *text++, SH_QUOTE_UNQUOTED))
			goto no_memory;
		produced = 1;
	}
	if (!produced && !quoted) {
		*error_text = "empty word";
		free(word.data);
		free(word.quote);
		return 0;
	}
	if (!word_reserve(&word))
		goto no_memory;
	word.data[word.length] = '\0';
	if (!token_append(list, SH_TOKEN_WORD, word.data, word.quote,
	    word.length))
		goto no_memory;
	*cursor = text;
	return 1;
no_memory:
	*error_text = "out of memory";
	free(word.data);
	free(word.quote);
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
		if (!token_append(list, type, NULL, NULL, 0)) {
			*error_text = "out of memory";
			goto failed;
		}
	}
	if (!token_append(list, SH_TOKEN_END, NULL, NULL, 0)) {
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
	for (index = 0; index < list->count; index++)
		free(list->tokens[index].quote);
	free(list->tokens);
	list->tokens = NULL;
	list->count = 0;
}
