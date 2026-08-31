/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland c parser support.
 */

#include "userland/base/common/c_parser.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum token_kind { TOKEN_NAME, TOKEN_NUMBER, TOKEN_STRING, TOKEN_PUNCT };

struct token {
	char *text;
	size_t line;
	enum token_kind kind;
};

struct token_list {
	struct token *tokens;
	size_t count;
};

static int read_fd(int fd, char **output, size_t *output_size);
static int tokenize(const char *source, size_t size, struct token_list *list);
static int token_add(struct token_list *list, enum token_kind kind, const char *text, size_t length, size_t line);
static int parse_tokens(const struct token_list *list, const char *file, struct c_parse_result *result);
static int is_type_word(const char *name);
static ssize_t matching_left(const struct token_list *list, size_t right);
static int is_keyword(const char *name);
static int event_add(struct c_parse_result *result, enum c_symbol_kind kind, const struct token *token, const char *function, const char *file);

/*
 * Implements the c parse stream operation.
 */
int
c_parse_stream(
	const char *name,
	int fd,
	struct c_parse_result *result)
{
	size_t i_index_for;
	struct token_list list = {0};
	char *source;
	size_t size;
	int status;

	source = NULL;
	status = -1;
	memset(result, 0, sizeof(*result));

	/* Handles the read fd condition. */
	if (read_fd(fd, &source, &size) || tokenize(source, size, &list) ||
	    parse_tokens(&list, name, result))
		goto out;
	status = 0;
out:

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < list.count; i_index_for++)
		free(list.tokens[i_index_for].text);
	free(list.tokens);
	free(source);

	/* Checks the operation status. */
	if (status)
		c_parse_free(result);

	/* Returns the computed result. */
	return status;
}

/*
 * Implements the c parse path operation.
 */
int
c_parse_path(
	const char *path,
	struct c_parse_result *result)
{
	int fd;
	int status;

	fd = open(path, O_RDONLY);

	/* Checks the file descriptor. */
	if (fd < 0)
		return -1;
	status = c_parse_stream(path, fd, result);
	close(fd);

	/* Returns the computed result. */
	return status;
}

/*
 * Implements the c parse free operation.
 */
void
c_parse_free(
	struct c_parse_result *result)
{
	size_t i_index_for;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < result->count; i_index_for++) {
		free(result->events[i_index_for].name);
		free(result->events[i_index_for].function);
		free(result->events[i_index_for].file);
	}
	free(result->events);
	memset(result, 0, sizeof(*result));
}

/* Supports the read fd operation. */
static int
read_fd(
	int fd,
	char **output,
	size_t *output_size)
{
	size_t next;
	char *replacement;
	ssize_t n;
	char *data;
	size_t size, capacity;

	/* Continue until the operation reaches a terminal state. */
	data = NULL;
	size = 0;
	capacity = 0;
	for (;;) {
		/* Handles the capacity condition. */
		if (capacity - size < 4096) {
						next = capacity ? capacity * 2 : 8192;

			/* Handles the next condition. */
			if (next < capacity || next == SIZE_MAX) {
				free(data);
				errno = EOVERFLOW;

				/* Reports operation failure. */
				return -1;
			}
			replacement = realloc(data, next + 1);

			/* Handles the replacement condition. */
			if (!replacement) {
				free(data);

				/* Reports operation failure. */
				return -1;
			}
			data = replacement;
			capacity = next;
		}
		n = read(fd, data + size, capacity - size);

		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Checks the current item count. */
		if (n < 0) {
			free(data);

			/* Reports operation failure. */
			return -1;
		}

		/* Checks the current item count. */
		if (!n)
			break;
		size += (size_t)n;
	}

	/* Handles the data condition. */
	if (!data) {
		data = malloc(1);

		/* Handles the data condition. */
		if (!data)
			return -1;
	}
	data[size] = '\0';
	*output = data;
	*output_size = size;
	/* Reports successful completion. */
	return 0;
}

/* Supports the tokenize operation. */
static int
tokenize(
	const char *source,
	size_t size,
	struct token_list *list)
{
	size_t start_local;
	size_t start_local1;
	int continued;
	char quote;
	size_t i, line;
	int beginning;

	/* Process each remaining element. */
	i = 0;
	line = 1;
	beginning = 1;
	while (i < size) {
		/* Handles the source condition. */
		if (source[i] == '\n') {
			line++;
			i++;
			beginning = 1;
			continue;
		}

		/* Handles the isspace condition. */
		if (isspace((unsigned char)source[i])) {
			i++;
			continue;
		}

		/* Handles the beginning condition. */
		if (beginning && source[i] == '#') {

			do {
				/* Process each remaining element. */
				continued = 0;
				while (i < size && source[i] != '\n')
					i++;

				/* Checks the current index. */
				if (i && source[i - 1] == '\\')
					continued = 1;

				/* Checks the current index. */
				if (i < size) {
					i++;
					line++;
				}
			} while (continued && i < size);
			beginning = 1;
			continue;
		}
		beginning = 0;

		/* Checks the current index. */
		if (i + 1 < size && source[i] == '/' && source[i + 1] == '/') {
			i += 2;

			/* Process each remaining element. */
			while (i < size && source[i] != '\n')
				i++;
			continue;
		}

		/* Checks the current index. */
		if (i + 1 < size && source[i] == '/' && source[i + 1] == '*') {
			i += 2;

			/* Process each remaining element. */
			while (i + 1 < size &&
			       !(source[i] == '*' && source[i + 1] == '/')) {
				/* Handles the source condition. */
				if (source[i++] == '\n')
					line++;
			}

			/* Checks the current index. */
			if (i + 1 == size) {
				errno = EINVAL;

				/* Reports operation failure. */
				return -1;
			}
			i += 2;
			continue;
		}

		/* Handles a failed isalpha operation. */
		if (isalpha((unsigned char)source[i]) || source[i] == '_') {
			/* Process each remaining element. */
						start_local = i++;
			while (i < size && (isalnum((unsigned char)source[i]) ||
					    source[i] == '_'))
				i++;

			/* Handles the token add condition. */
			if (token_add(list, TOKEN_NAME, source + start_local,
				      i - start_local, line))

				/* Reports operation failure. */
				return -1;
			continue;
		}

		/* Handles the isdigit condition. */
		if (isdigit((unsigned char)source[i])) {
			/* Process each remaining element. */
						start_local1 = i++;
			while (i < size &&
			       (isalnum((unsigned char)source[i]) ||
				source[i] == '.' || source[i] == '_'))
				i++;

			/* Handles the token add condition. */
			if (token_add(list, TOKEN_NUMBER, source + start_local1,
				      i - start_local1, line))

				/* Reports operation failure. */
				return -1;
			continue;
		}

		/* Handles the source condition. */
		if (source[i] == '"' || source[i] == '\'') {
			/* Process each remaining element. */
						quote = source[i];
			size_t start = i++;
			while (i < size && source[i] != quote) {
				/* Handles the source condition. */
				if (source[i] == '\\' && i + 1 < size)
					i += 2;
				else {
					/* Handles the source condition. */
					if (source[i] == '\n')
						line++;
					i++;
				}
			}

			/* Checks the current index. */
			if (i == size) {
				errno = EINVAL;

				/* Reports operation failure. */
				return -1;
			}
			i++;

			/* Handles the token add condition. */
			if (token_add(list, TOKEN_STRING, source + start,
				      i - start, line))

				/* Reports operation failure. */
				return -1;
			continue;
		}

		/* Checks the current index. */
		if (i + 2 < size && !memcmp(source + i, "...", 3)) {
			/* Handles the token add condition. */
			if (token_add(list, TOKEN_PUNCT, source + i, 3, line))
				return -1;
			i += 3;
			continue;
		}

		/* Checks the current index. */
		if (i + 1 < size && (!memcmp(source + i, "->", 2) ||
				     !memcmp(source + i, "++", 2) ||
				     !memcmp(source + i, "--", 2) ||
				     !memcmp(source + i, "&&", 2) ||
				     !memcmp(source + i, "||", 2) ||
				     !memcmp(source + i, "==", 2) ||
				     !memcmp(source + i, "!=", 2) ||
				     !memcmp(source + i, "<=", 2) ||
				     !memcmp(source + i, ">=", 2) ||
				     !memcmp(source + i, "<<", 2) ||
				     !memcmp(source + i, ">>", 2))) {
			/* Handles the token add condition. */
			if (token_add(list, TOKEN_PUNCT, source + i, 2, line))
				return -1;
			i += 2;
			continue;
		}

		/* Handles the token add condition. */
		if (token_add(list, TOKEN_PUNCT, source + i, 1, line))
			return -1;
		i++;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the token add operation. */
static int
token_add(
	struct token_list *list,
	enum token_kind kind,
	const char *text,
	size_t length,
	size_t line)
{
	struct token token;
	struct token *tokens;

	/* Handles the list condition. */
	if (list->count == SIZE_MAX / sizeof(*tokens)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	token.text = malloc(length + 1);

	/* Handles the token condition. */
	if (!token.text)
		return -1;
	memcpy(token.text, text, length);
	token.text[length] = '\0';
	token.line = line;
	token.kind = kind;
	tokens = realloc(list->tokens, (list->count + 1) * sizeof(*tokens));

	/* Handles the tokens condition. */
	if (!tokens) {
		free(token.text);

		/* Reports operation failure. */
		return -1;
	}
	list->tokens = tokens;
	list->tokens[list->count++] = token;

	/* Reports successful completion. */
	return 0;
}

/* Supports the parse tokens operation. */
static int
parse_tokens(
	const struct token_list *list,
	const char *file,
	struct c_parse_result *result)
{
	ssize_t left;
	ssize_t right;
	const struct token *token;
	size_t i_index_for;
	unsigned braces;
	char *function;
	int declaration;

	/* Process each remaining element. */
	braces = 0;
	function = NULL;
	declaration = 0;
	for (i_index_for = 0; i_index_for < list->count; i_index_for++) {
				token = &list->tokens[i_index_for];

		/* Handles a failed type word operation. */
		if (token->kind == TOKEN_NAME && is_type_word(token->text))
			declaration = 1;

		/* Selects the matching value. */
		if (!strcmp(token->text, "{") && braces == 0) {
			/* Continue while the operation condition remains true. */
						right = (ssize_t)i_index_for - 1;
			while (right >= 0 &&
			       strcmp(list->tokens[right].text, ")"))
				right--;

			/* Handles the right condition. */
			if (right >= 0) {
								left = matching_left(list, (size_t)right);

				/* Handles a failed keyword operation. */
				if (left > 0 &&
				    list->tokens[left - 1].kind == TOKEN_NAME &&
				    !is_keyword(list->tokens[left - 1].text)) {
					free(function);
					function =
					    strdup(list->tokens[left - 1].text);

					/* Handles a failed event add operation. */
					if (!function ||
					    event_add(result, C_SYMBOL_FUNCTION,
						      &list->tokens[left - 1],
						      NULL, file))
						goto fail;
				}
			}
			braces++;
			declaration = 0;
			continue;
		}

		/* Selects the matching value. */
		if (!strcmp(token->text, "{") && braces) {
			braces++;
			continue;
		}

		/* Selects the matching value. */
		if (!strcmp(token->text, "}") && braces) {
			/* Handles the braces condition. */
			if (!--braces) {
				free(function);
				function = NULL;
			}
			declaration = 0;
			continue;
		}

		/* Selects the matching value. */
		if (!strcmp(token->text, ";")) {
			declaration = 0;
			continue;
		}

		/* Handles a failed keyword operation. */
		if (token->kind != TOKEN_NAME || is_keyword(token->text))
			continue;

		/* Handles the i index for condition. */
		if (i_index_for + 1 < list->count &&
		    !strcmp(list->tokens[i_index_for + 1].text, "(") &&
		    (i_index_for == 0 || (strcmp(list->tokens[i_index_for - 1].text, ".") &&
				strcmp(list->tokens[i_index_for - 1].text, "->")))) {
			/* Handles the braces condition. */
			if (braces && event_add(result, C_SYMBOL_CALL, token,
						function, file))
				goto fail;
			continue;
		}

		/* Handles the event add condition. */
		if (event_add(result,
			      declaration ? C_SYMBOL_DECLARATION
					  : C_SYMBOL_REFERENCE,
			      token, function, file))
			goto fail;
	}
	free(function);

	/* Reports successful completion. */
	return 0;
fail:
	free(function);

	/* Reports operation failure. */
	return -1;
}

/* Supports the is type word operation. */
static int
is_type_word(
	const char *name)
{
	size_t i_index_for;
	static const char *const words[] = {
	    "auto",	    "char",	"const",  "double",   "enum",
	    "extern",	    "float",	"inline", "int",      "long",
	    "register",	    "restrict", "short",  "signed",   "static",
	    "struct",	    "typedef",	"union",  "unsigned", "void",
	    "volatile",	    "_Atomic",	"_Bool",  "_Complex", "_Noreturn",
	    "_Thread_local"};

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < sizeof(words) / sizeof(words[0]); i_index_for++)

		/* Selects the matching value. */
		if (!strcmp(name, words[i_index_for]))
			return 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the matching left operation. */
static ssize_t
matching_left(
	const struct token_list *list,
	size_t right)
{
	size_t i_index_for;
	unsigned depth;

	/* Process each remaining element. */
	depth = 0;
	for (i_index_for = right + 1; i_index_for-- > 0;) {
		/* Selects the matching value. */
		if (!strcmp(list->tokens[i_index_for].text, ")"))
			depth++;
		else if (!strcmp(list->tokens[i_index_for].text, "(")) {
			/* Handles the depth condition. */
			if (!--depth)
				return (ssize_t)i_index_for;
		}
	}

	/* Reports operation failure. */
	return -1;
}

/* Supports the is keyword operation. */
static int
is_keyword(
	const char *name)
{
	size_t i_index_for;
	static const char *const words[] = {
	    "auto",	  "break",     "case",		 "char",
	    "const",	  "continue",  "default",	 "do",
	    "double",	  "else",      "enum",		 "extern",
	    "float",	  "for",       "goto",		 "if",
	    "inline",	  "int",       "long",		 "register",
	    "restrict",	  "return",    "short",		 "signed",
	    "sizeof",	  "static",    "struct",	 "switch",
	    "typedef",	  "union",     "unsigned",	 "void",
	    "volatile",	  "while",     "_Alignas",	 "_Alignof",
	    "_Atomic",	  "_Bool",     "_Complex",	 "_Generic",
	    "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local"};

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < sizeof(words) / sizeof(words[0]); i_index_for++)

		/* Selects the matching value. */
		if (!strcmp(name, words[i_index_for]))
			return 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the event add operation. */
static int
event_add(
	struct c_parse_result *result,
	enum c_symbol_kind kind,
	const struct token *token,
	const char *function,
	const char *file)
{
	struct c_symbol_event event;
	struct c_symbol_event *events;

	memset(&event, 0, sizeof(event));
	event.name = strdup(token->text);
	event.function = function ? strdup(function) : NULL;
	event.file = strdup(file);
	event.line = token->line;
	event.kind = kind;

	/* Handles the event condition. */
	if (!event.name || !event.file || (function && !event.function))
		goto fail;

	/* Checks the operation result. */
	if (result->count == SIZE_MAX / sizeof(*events)) {
		errno = EOVERFLOW;
		goto fail;
	}
	events = realloc(result->events, (result->count + 1) * sizeof(*events));

	/* Handles the events condition. */
	if (!events)
		goto fail;
	result->events = events;
	result->events[result->count++] = event;

	/* Reports successful completion. */
	return 0;
fail:
	free(event.name);
	free(event.function);
	free(event.file);

	/* Reports operation failure. */
	return -1;
}
