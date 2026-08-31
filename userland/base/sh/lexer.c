/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland lexer component.
 */

#include "userland/base/sh/lexer.h"

#include <stdlib.h>
#include <string.h>

struct word_buffer {
	char *data;
	unsigned char *quote;
	size_t length;
	size_t capacity;
};

static int is_operator(char value);
static int lex_word(const char **cursor, struct sh_token_list *list, const char **error_text);
static int word_append(struct word_buffer *word, char value, enum sh_quote_type quote);
static int word_reserve(struct word_buffer *word);
static int token_append(struct sh_token_list *list, enum sh_token_type type, char *text, unsigned char *quote, size_t length);

/*
 * Implements the sh lex operation.
 */
int
sh_lex(
	const char *text,
	struct sh_token_list *list,
	const char **error_text)
{
	enum sh_token_type type;

	memset(list, 0, sizeof(*list));

	/* Continue while the operation condition remains true. */
	*error_text = NULL;
	while (*text != '\0') {
		/* Continue while the operation condition remains true. */
		while (*text == ' ' || *text == '\t')
			text++;

		/* Validates the current text. */
		if (*text == '\0' || *text == '#')
			break;

		/* Handles a failed operator operation. */
		if (!is_operator(*text)) {
			/* Handles an operation failure. */
			if (!lex_word(&text, list, error_text))
				goto failed;
			continue;
		}

		/* Dispatch the selected operation case. */
		switch (*text++) {
		case ';':
			type = SH_TOKEN_SEMI;
			break;
		case '&':
			/* Validates the current text. */
			if (*text == '&') {
				text++;
				type = SH_TOKEN_AND_IF;
			} else
				type = SH_TOKEN_AMP;
			break;
		case '|':
			/* Validates the current text. */
			if (*text == '|') {
				text++;
				type = SH_TOKEN_OR_IF;
			} else
				type = SH_TOKEN_PIPE;
			break;
		case '<':
			type = SH_TOKEN_INPUT;
			break;
		default:
			/* Validates the current text. */
			if (*text == '>') {
				text++;
				type = SH_TOKEN_APPEND;
			} else
				type = SH_TOKEN_OUTPUT;
			break;
		}

		/* Handles a failed token append operation. */
		if (!token_append(list, type, NULL, NULL, 0)) {
			*error_text = "out of memory";
			goto failed;
		}
	}

	/* Handles a failed token append operation. */
	if (!token_append(list, SH_TOKEN_END, NULL, NULL, 0)) {
		*error_text = "out of memory";
		goto failed;
	}

	/* Reports operation failure. */
	return 1;
failed:
	sh_tokens_free(list);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sh tokens free operation.
 */
void
sh_tokens_free(
	struct sh_token_list *list)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < list->count; index++)
		free(list->tokens[index].text);

	/* Process each remaining element. */
	for (index = 0; index < list->count; index++)
		free(list->tokens[index].quote);
	free(list->tokens);
	list->tokens = NULL;
	list->count = 0;
}

/* Supports the is operator operation. */
static int
is_operator(
	char value)
{
	/* Returns the computed result. */
	return value == ';' || value == '&' || value == '|' || value == '<' ||
	       value == '>';
}

/* Supports the lex word operation. */
static int
lex_word(
	const char **cursor,
	struct sh_token_list *list,
	const char **error_text)
{
	char value;
	int depth;
	char inner_quote;
	int escaped;
	char quote;
	const char *text;
	struct word_buffer word = {0};
	int quoted;
	int produced;

	/* Continue while the operation condition remains true. */
	text = *cursor;
	quoted = 0;
	produced = 0;
	while (*text != '\0' && *text != ' ' && *text != '\t' &&
	       !is_operator(*text)) {
		/* Validates the current text. */
		if (text[0] == '$' && text[1] == '(') {
						depth = 1;
						inner_quote = '\0';

			/* Handles a failed word append operation. */
			if (!word_append(&word, *text++, SH_QUOTE_UNQUOTED) ||
			    !word_append(&word, *text++, SH_QUOTE_UNQUOTED))
				goto no_memory;

			/* Continue while the operation condition remains true. */
			while (*text != '\0' && depth != 0) {

				value = *text++;

				/* Validates the current value. */
				if (value == '\\' && *text != '\0') {
					/* Handles a failed word append operation. */
					if (!word_append(&word, value,
							 SH_QUOTE_UNQUOTED) ||
					    !word_append(&word, *text++,
							 SH_QUOTE_UNQUOTED))
						goto no_memory;
					continue;
				}

				/* Validates the current value. */
				if ((value == '\'' || value == '"') &&
				    (inner_quote == '\0' ||
				     inner_quote == value))
					inner_quote =
					    inner_quote == '\0' ? value : '\0';

				/* Handles the inner quote condition. */
				if (inner_quote == '\0') {
					/* Validates the current value. */
					if (value == '(')
						depth++;

					/* Validates the current value. */
					if (value == ')')
						depth--;
				}

				/* Handles a failed word append operation. */
				if (!word_append(&word, value,
						 SH_QUOTE_UNQUOTED))
					goto no_memory;
			}

			/* Handles the depth condition. */
			if (depth != 0) {
				*error_text =
				    "unterminated command substitution";
				free(word.data);
				free(word.quote);

				/* Reports successful completion. */
				return 0;
			}
			produced = 1;
			continue;
		}

		/* Validates the current text. */
		if (*text == '\\') {
			text++;

			/* Validates the current text. */
			if (*text == '\0') {
				*error_text = "trailing backslash";
				free(word.data);
				free(word.quote);

				/* Reports successful completion. */
				return 0;
			}

			/* Handles a failed word append operation. */
			if (!word_append(&word, *text++, SH_QUOTE_ESCAPED))
				goto no_memory;
			produced = 1;
			continue;
		}

		/* Validates the current text. */
		if (*text == '\'' || *text == '"') {
			/* Continue while the operation condition remains true. */
						quote = *text++;
			quoted = 1;
			while (*text != '\0' && *text != quote) {

				escaped = 0;

				/* Handles the quote condition. */
				if (quote == '"' && *text == '\\') {
					/* Validates the current text. */
					if (text[1] == '$' || text[1] == '`' ||
					    text[1] == '"' || text[1] == '\\') {
						text++;
						escaped = 1;
					} else if (text[1] == '\n') {
						text += 2;
						continue;
					} else {
						/* Handles a failed word append operation. */
						if (!word_append(
							&word, *text++,
							SH_QUOTE_DOUBLE))
							goto no_memory;
						produced = 1;
						continue;
					}
				}

				/* Handles a failed word append operation. */
				if (!word_append(&word, *text++,
						 escaped ? SH_QUOTE_ESCAPED
						 : quote == '\''
						     ? SH_QUOTE_SINGLE
						     : SH_QUOTE_DOUBLE))
					goto no_memory;
				produced = 1;
			}

			/* Validates the current text. */
			if (*text != quote) {
				*error_text = "unterminated quote";
				free(word.data);
				free(word.quote);

				/* Reports successful completion. */
				return 0;
			}
			text++;
			continue;
		}

		/* Handles a failed word append operation. */
		if (!word_append(&word, *text++, SH_QUOTE_UNQUOTED))
			goto no_memory;
		produced = 1;
	}

	/* Handles the produced condition. */
	if (!produced && !quoted) {
		*error_text = "empty word";
		free(word.data);
		free(word.quote);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed word reserve operation. */
	if (!word_reserve(&word))
		goto no_memory;
	word.data[word.length] = '\0';

	/* Handles a failed token append operation. */
	if (!token_append(list, SH_TOKEN_WORD, word.data, word.quote,
			  word.length))
		goto no_memory;
	*cursor = text;
	/* Reports operation failure. */
	return 1;
no_memory:
	*error_text = "out of memory";
	free(word.data);
	free(word.quote);

	/* Reports successful completion. */
	return 0;
}

/* Supports the word append operation. */
static int
word_append(
	struct word_buffer *word,
	char value,
	enum sh_quote_type quote)
{
	/* Handles a failed word reserve operation. */
	if (!word_reserve(word))
		return 0;
	word->data[word->length++] = value;
	word->quote[word->length - 1U] = (unsigned char)quote;

	/* Reports operation failure. */
	return 1;
}

/* Supports the word reserve operation. */
static int
word_reserve(
	struct word_buffer *word)
{
	char *larger;
	unsigned char *quote;
	size_t capacity;

	/* Handles the word condition. */
	if (word->length + 1U < word->capacity)
		return 1;
	capacity = word->capacity == 0 ? 16U : word->capacity * 2U;

	/* Handles the capacity condition. */
	if (capacity <= word->capacity)
		return 0;
	larger = realloc(word->data, capacity);

	/* Handles the larger availability. */
	if (larger == NULL)
		return 0;
	word->data = larger;
	quote = realloc(word->quote, capacity);

	/* Handles the quote availability. */
	if (quote == NULL)
		return 0;
	word->data = larger;
	word->quote = quote;
	word->capacity = capacity;

	/* Reports operation failure. */
	return 1;
}

/* Supports the token append operation. */
static int
token_append(
	struct sh_token_list *list,
	enum sh_token_type type,
	char *text,
	unsigned char *quote,
	size_t length)
{
	struct sh_token *larger;

	/* Handles the list condition. */
	if (list->count == (size_t)-1 / sizeof(*list->tokens))
		return 0;
	larger =
	    realloc(list->tokens, (list->count + 1U) * sizeof(*list->tokens));

	/* Handles the larger availability. */
	if (larger == NULL)
		return 0;
	list->tokens = larger;
	list->tokens[list->count].type = type;
	list->tokens[list->count].text = text;
	list->tokens[list->count].quote = quote;
	list->tokens[list->count].length = length;
	list->count++;

	/* Reports operation failure. */
	return 1;
}
