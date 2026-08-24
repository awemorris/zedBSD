/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
read_fd(int fd, char **output, size_t *output_size)
{
	char *data = NULL;
	size_t size = 0, capacity = 0;
	for (;;) {
		ssize_t n;
		if (capacity - size < 4096) {
			size_t next = capacity ? capacity * 2 : 8192;
			char *replacement;
			if (next < capacity || next == SIZE_MAX) {
				free(data);
				errno = EOVERFLOW;
				return -1;
			}
			replacement = realloc(data, next + 1);
			if (!replacement) {
				free(data);
				return -1;
			}
			data = replacement;
			capacity = next;
		}
		n = read(fd, data + size, capacity - size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			free(data);
			return -1;
		}
		if (!n)
			break;
		size += (size_t)n;
	}
	if (!data) {
		data = malloc(1);
		if (!data)
			return -1;
	}
	data[size] = '\0';
	*output = data;
	*output_size = size;
	return 0;
}

static int
token_add(struct token_list *list, enum token_kind kind, const char *text,
	  size_t length, size_t line)
{
	struct token token;
	struct token *tokens;
	if (list->count == SIZE_MAX / sizeof(*tokens)) {
		errno = EOVERFLOW;
		return -1;
	}
	token.text = malloc(length + 1);
	if (!token.text)
		return -1;
	memcpy(token.text, text, length);
	token.text[length] = '\0';
	token.line = line;
	token.kind = kind;
	tokens = realloc(list->tokens, (list->count + 1) * sizeof(*tokens));
	if (!tokens) {
		free(token.text);
		return -1;
	}
	list->tokens = tokens;
	list->tokens[list->count++] = token;
	return 0;
}

static int
tokenize(const char *source, size_t size, struct token_list *list)
{
	size_t i = 0, line = 1;
	int beginning = 1;
	while (i < size) {
		if (source[i] == '\n') {
			line++;
			i++;
			beginning = 1;
			continue;
		}
		if (isspace((unsigned char)source[i])) {
			i++;
			continue;
		}
		if (beginning && source[i] == '#') {
			int continued;
			do {
				continued = 0;
				while (i < size && source[i] != '\n')
					i++;
				if (i && source[i - 1] == '\\')
					continued = 1;
				if (i < size) {
					i++;
					line++;
				}
			} while (continued && i < size);
			beginning = 1;
			continue;
		}
		beginning = 0;
		if (i + 1 < size && source[i] == '/' && source[i + 1] == '/') {
			i += 2;
			while (i < size && source[i] != '\n')
				i++;
			continue;
		}
		if (i + 1 < size && source[i] == '/' && source[i + 1] == '*') {
			i += 2;
			while (i + 1 < size &&
			       !(source[i] == '*' && source[i + 1] == '/')) {
				if (source[i++] == '\n')
					line++;
			}
			if (i + 1 == size) {
				errno = EINVAL;
				return -1;
			}
			i += 2;
			continue;
		}
		if (isalpha((unsigned char)source[i]) || source[i] == '_') {
			size_t start = i++;
			while (i < size && (isalnum((unsigned char)source[i]) ||
					    source[i] == '_'))
				i++;
			if (token_add(list, TOKEN_NAME, source + start,
				      i - start, line))
				return -1;
			continue;
		}
		if (isdigit((unsigned char)source[i])) {
			size_t start = i++;
			while (i < size &&
			       (isalnum((unsigned char)source[i]) ||
				source[i] == '.' || source[i] == '_'))
				i++;
			if (token_add(list, TOKEN_NUMBER, source + start,
				      i - start, line))
				return -1;
			continue;
		}
		if (source[i] == '"' || source[i] == '\'') {
			char quote = source[i];
			size_t start = i++;
			while (i < size && source[i] != quote) {
				if (source[i] == '\\' && i + 1 < size)
					i += 2;
				else {
					if (source[i] == '\n')
						line++;
					i++;
				}
			}
			if (i == size) {
				errno = EINVAL;
				return -1;
			}
			i++;
			if (token_add(list, TOKEN_STRING, source + start,
				      i - start, line))
				return -1;
			continue;
		}
		if (i + 2 < size && !memcmp(source + i, "...", 3)) {
			if (token_add(list, TOKEN_PUNCT, source + i, 3, line))
				return -1;
			i += 3;
			continue;
		}
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
			if (token_add(list, TOKEN_PUNCT, source + i, 2, line))
				return -1;
			i += 2;
			continue;
		}
		if (token_add(list, TOKEN_PUNCT, source + i, 1, line))
			return -1;
		i++;
	}
	return 0;
}

static int
is_keyword(const char *name)
{
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
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		if (!strcmp(name, words[i]))
			return 1;
	return 0;
}

static int
is_type_word(const char *name)
{
	static const char *const words[] = {
	    "auto",	    "char",	"const",  "double",   "enum",
	    "extern",	    "float",	"inline", "int",      "long",
	    "register",	    "restrict", "short",  "signed",   "static",
	    "struct",	    "typedef",	"union",  "unsigned", "void",
	    "volatile",	    "_Atomic",	"_Bool",  "_Complex", "_Noreturn",
	    "_Thread_local"};
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		if (!strcmp(name, words[i]))
			return 1;
	return 0;
}

static int
event_add(struct c_parse_result *result, enum c_symbol_kind kind,
	  const struct token *token, const char *function, const char *file)
{
	struct c_symbol_event event;
	struct c_symbol_event *events;
	memset(&event, 0, sizeof(event));
	event.name = strdup(token->text);
	event.function = function ? strdup(function) : NULL;
	event.file = strdup(file);
	event.line = token->line;
	event.kind = kind;
	if (!event.name || !event.file || (function && !event.function))
		goto fail;
	if (result->count == SIZE_MAX / sizeof(*events)) {
		errno = EOVERFLOW;
		goto fail;
	}
	events = realloc(result->events, (result->count + 1) * sizeof(*events));
	if (!events)
		goto fail;
	result->events = events;
	result->events[result->count++] = event;
	return 0;
fail:
	free(event.name);
	free(event.function);
	free(event.file);
	return -1;
}

static ssize_t
matching_left(const struct token_list *list, size_t right)
{
	unsigned depth = 0;
	for (size_t i = right + 1; i-- > 0;) {
		if (!strcmp(list->tokens[i].text, ")"))
			depth++;
		else if (!strcmp(list->tokens[i].text, "(")) {
			if (!--depth)
				return (ssize_t)i;
		}
	}
	return -1;
}

static int
parse_tokens(const struct token_list *list, const char *file,
	     struct c_parse_result *result)
{
	unsigned braces = 0;
	char *function = NULL;
	int declaration = 0;
	for (size_t i = 0; i < list->count; i++) {
		const struct token *token = &list->tokens[i];
		if (token->kind == TOKEN_NAME && is_type_word(token->text))
			declaration = 1;
		if (!strcmp(token->text, "{") && braces == 0) {
			ssize_t right = (ssize_t)i - 1;
			while (right >= 0 &&
			       strcmp(list->tokens[right].text, ")"))
				right--;
			if (right >= 0) {
				ssize_t left =
				    matching_left(list, (size_t)right);
				if (left > 0 &&
				    list->tokens[left - 1].kind == TOKEN_NAME &&
				    !is_keyword(list->tokens[left - 1].text)) {
					free(function);
					function =
					    strdup(list->tokens[left - 1].text);
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
		if (!strcmp(token->text, "{") && braces) {
			braces++;
			continue;
		}
		if (!strcmp(token->text, "}") && braces) {
			if (!--braces) {
				free(function);
				function = NULL;
			}
			declaration = 0;
			continue;
		}
		if (!strcmp(token->text, ";")) {
			declaration = 0;
			continue;
		}
		if (token->kind != TOKEN_NAME || is_keyword(token->text))
			continue;
		if (i + 1 < list->count &&
		    !strcmp(list->tokens[i + 1].text, "(") &&
		    (i == 0 || (strcmp(list->tokens[i - 1].text, ".") &&
				strcmp(list->tokens[i - 1].text, "->")))) {
			if (braces && event_add(result, C_SYMBOL_CALL, token,
						function, file))
				goto fail;
			continue;
		}
		if (event_add(result,
			      declaration ? C_SYMBOL_DECLARATION
					  : C_SYMBOL_REFERENCE,
			      token, function, file))
			goto fail;
	}
	free(function);
	return 0;
fail:
	free(function);
	return -1;
}

int
c_parse_stream(const char *name, int fd, struct c_parse_result *result)
{
	struct token_list list = {0};
	char *source = NULL;
	size_t size;
	int status = -1;
	memset(result, 0, sizeof(*result));
	if (read_fd(fd, &source, &size) || tokenize(source, size, &list) ||
	    parse_tokens(&list, name, result))
		goto out;
	status = 0;
out:
	for (size_t i = 0; i < list.count; i++)
		free(list.tokens[i].text);
	free(list.tokens);
	free(source);
	if (status)
		c_parse_free(result);
	return status;
}

int
c_parse_path(const char *path, struct c_parse_result *result)
{
	int fd = open(path, O_RDONLY);
	int status;
	if (fd < 0)
		return -1;
	status = c_parse_stream(path, fd, result);
	close(fd);
	return status;
}

void
c_parse_free(struct c_parse_result *result)
{
	for (size_t i = 0; i < result->count; i++) {
		free(result->events[i].name);
		free(result->events[i].function);
		free(result->events[i].file);
	}
	free(result->events);
	memset(result, 0, sizeof(*result));
}
