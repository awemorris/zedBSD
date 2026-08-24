/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/bc/number.h"
#include "userland/base/common/command.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BC_IDENTIFIER_MAX 63
#define BC_PARSE_DEPTH_MAX 128

enum token_kind {
	TOKEN_END,
	TOKEN_SEPARATOR,
	TOKEN_NUMBER,
	TOKEN_IDENTIFIER,
	TOKEN_ADD,
	TOKEN_SUBTRACT,
	TOKEN_MULTIPLY,
	TOKEN_DIVIDE,
	TOKEN_REMAINDER,
	TOKEN_POWER,
	TOKEN_ASSIGN,
	TOKEN_LEFT,
	TOKEN_RIGHT,
	TOKEN_ERROR,
};

struct token {
	enum token_kind kind;
	const char *text;
	size_t length;
	unsigned line;
};

struct lexer {
	const char *text;
	size_t offset;
	unsigned line;
	struct token token;
};

struct variable {
	char name[BC_IDENTIFIER_MAX + 1];
	struct bc_number value;
};

struct runtime {
	struct variable *variable;
	size_t count;
	size_t capacity;
	int failed;
};

struct parser {
	struct lexer lexer;
	struct runtime *runtime;
	const char *source;
	unsigned depth;
};

static void
lexer_next(struct lexer *lexer)
{
	const char *text = lexer->text;

	for (;;) {
		char c = text[lexer->offset];

		while (c == ' ' || c == '\t' || c == '\r')
			c = text[++lexer->offset];
		if (c == '/' && text[lexer->offset + 1] == '*') {
			lexer->offset += 2;
			while (text[lexer->offset] != '\0' &&
			       !(text[lexer->offset] == '*' &&
				 text[lexer->offset + 1] == '/')) {
				if (text[lexer->offset++] == '\n')
					lexer->line++;
			}
			if (text[lexer->offset] == '\0') {
				lexer->token.kind = TOKEN_ERROR;
				lexer->token.text = "unterminated comment";
				lexer->token.length = strlen(lexer->token.text);
				lexer->token.line = lexer->line;
				return;
			}
			lexer->offset += 2;
			continue;
		}
		if (c == '#') {
			while (text[lexer->offset] != '\0' &&
			       text[lexer->offset] != '\n')
				lexer->offset++;
			continue;
		}
		break;
	}
	lexer->token.text = text + lexer->offset;
	lexer->token.length = 1;
	lexer->token.line = lexer->line;
	switch (text[lexer->offset]) {
	case '\0':
		lexer->token.kind = TOKEN_END;
		lexer->token.length = 0;
		return;
	case '\n':
		lexer->token.kind = TOKEN_SEPARATOR;
		lexer->offset++;
		lexer->line++;
		return;
	case ';':
		lexer->token.kind = TOKEN_SEPARATOR;
		break;
	case '+':
		lexer->token.kind = TOKEN_ADD;
		break;
	case '-':
		lexer->token.kind = TOKEN_SUBTRACT;
		break;
	case '*':
		lexer->token.kind = TOKEN_MULTIPLY;
		break;
	case '/':
		lexer->token.kind = TOKEN_DIVIDE;
		break;
	case '%':
		lexer->token.kind = TOKEN_REMAINDER;
		break;
	case '^':
		lexer->token.kind = TOKEN_POWER;
		break;
	case '=':
		lexer->token.kind = TOKEN_ASSIGN;
		break;
	case '(':
		lexer->token.kind = TOKEN_LEFT;
		break;
	case ')':
		lexer->token.kind = TOKEN_RIGHT;
		break;
	default:
		if (text[lexer->offset] >= '0' && text[lexer->offset] <= '9') {
			lexer->token.kind = TOKEN_NUMBER;
			while (text[lexer->offset + lexer->token.length] >=
				   '0' &&
			       text[lexer->offset + lexer->token.length] <= '9')
				lexer->token.length++;
			lexer->offset += lexer->token.length;
			return;
		}
		if ((text[lexer->offset] >= 'A' &&
		     text[lexer->offset] <= 'Z') ||
		    (text[lexer->offset] >= 'a' &&
		     text[lexer->offset] <= 'z') ||
		    text[lexer->offset] == '_') {
			lexer->token.kind = TOKEN_IDENTIFIER;
			while (
			    (text[lexer->offset + lexer->token.length] >= 'A' &&
			     text[lexer->offset + lexer->token.length] <=
				 'Z') ||
			    (text[lexer->offset + lexer->token.length] >= 'a' &&
			     text[lexer->offset + lexer->token.length] <=
				 'z') ||
			    (text[lexer->offset + lexer->token.length] >= '0' &&
			     text[lexer->offset + lexer->token.length] <=
				 '9') ||
			    text[lexer->offset + lexer->token.length] == '_')
				lexer->token.length++;
			lexer->offset += lexer->token.length;
			return;
		}
		lexer->token.kind = TOKEN_ERROR;
		break;
	}
	lexer->offset++;
}

static void
runtime_free(struct runtime *runtime)
{
	size_t index;

	for (index = 0; index < runtime->count; index++)
		bc_number_free(&runtime->variable[index].value);
	free(runtime->variable);
}

static struct variable *
runtime_find(struct runtime *runtime, const char *name, size_t length,
	     int create)
{
	size_t index;

	for (index = 0; index < runtime->count; index++)
		if (strlen(runtime->variable[index].name) == length &&
		    memcmp(runtime->variable[index].name, name, length) == 0)
			return &runtime->variable[index];
	if (!create)
		return NULL;
	if (length > BC_IDENTIFIER_MAX) {
		errno = EOVERFLOW;
		return NULL;
	}
	if (runtime->count == runtime->capacity) {
		size_t capacity =
		    runtime->capacity == 0 ? 16 : runtime->capacity * 2;
		struct variable *variable;

		if (capacity < runtime->capacity ||
		    capacity > SIZE_MAX / sizeof(*variable)) {
			errno = EOVERFLOW;
			return NULL;
		}
		variable =
		    realloc(runtime->variable, capacity * sizeof(*variable));
		if (variable == NULL)
			return NULL;
		runtime->variable = variable;
		runtime->capacity = capacity;
	}
	memcpy(runtime->variable[runtime->count].name, name, length);
	runtime->variable[runtime->count].name[length] = '\0';
	bc_number_init(&runtime->variable[runtime->count].value);
	return &runtime->variable[runtime->count++];
}

static void
parser_error(struct parser *parser, const char *message)
{
	if (!parser->runtime->failed)
		fprintf(stderr, "bc: %s:%u: %s\n", parser->source,
			parser->lexer.token.line, message);
	parser->runtime->failed = 1;
}

static int
parser_enter(struct parser *parser)
{
	if (++parser->depth > BC_PARSE_DEPTH_MAX) {
		parser_error(parser, "expression nesting limit exceeded");
		return 0;
	}
	return 1;
}

static struct bc_number parse_expression(struct parser *);

static struct bc_number
invalid_number(void)
{
	struct bc_number number;

	bc_number_init(&number);
	return number;
}

static struct bc_number
parse_primary(struct parser *parser)
{
	struct bc_number result = invalid_number();
	struct token token = parser->lexer.token;

	if (!parser_enter(parser))
		return result;
	if (token.kind == TOKEN_NUMBER) {
		if (bc_number_from_decimal(&result, token.text, token.length) !=
		    0)
			parser_error(parser, "invalid or too large number");
		lexer_next(&parser->lexer);
	} else if (token.kind == TOKEN_IDENTIFIER) {
		struct variable *variable =
		    runtime_find(parser->runtime, token.text, token.length, 0);

		if (variable != NULL &&
		    bc_number_copy(&result, &variable->value) != 0)
			parser_error(parser, "out of memory");
		lexer_next(&parser->lexer);
	} else if (token.kind == TOKEN_LEFT) {
		lexer_next(&parser->lexer);
		result = parse_expression(parser);
		if (parser->lexer.token.kind != TOKEN_RIGHT) {
			bc_number_free(&result);
			parser_error(parser, "missing closing parenthesis");
		} else
			lexer_next(&parser->lexer);
	} else {
		parser_error(parser, token.kind == TOKEN_ERROR
					 ? "invalid input"
					 : "expected number, variable, or '('");
	}
	parser->depth--;
	return result;
}

static struct bc_number
parse_unary(struct parser *parser)
{
	if (parser->lexer.token.kind == TOKEN_ADD) {
		lexer_next(&parser->lexer);
		return parse_unary(parser);
	}
	if (parser->lexer.token.kind == TOKEN_SUBTRACT) {
		struct bc_number value;
		struct bc_number result = invalid_number();

		lexer_next(&parser->lexer);
		value = parse_unary(parser);
		if (!parser->runtime->failed &&
		    bc_number_negate(&result, &value) != 0)
			parser_error(parser, "out of memory");
		bc_number_free(&value);
		return result;
	}
	return parse_primary(parser);
}

static struct bc_number
parse_power(struct parser *parser)
{
	struct bc_number left = parse_unary(parser);

	if (!parser->runtime->failed &&
	    parser->lexer.token.kind == TOKEN_POWER) {
		struct bc_number right;
		struct bc_number result = invalid_number();

		lexer_next(&parser->lexer);
		right = parse_power(parser);
		if (!parser->runtime->failed &&
		    bc_number_power(&result, &left, &right) != 0)
			parser_error(parser, "negative or excessive exponent");
		bc_number_free(&left);
		bc_number_free(&right);
		left = result;
	}
	return left;
}

static struct bc_number
parse_product(struct parser *parser)
{
	struct bc_number left = parse_power(parser);

	while (!parser->runtime->failed &&
	       (parser->lexer.token.kind == TOKEN_MULTIPLY ||
		parser->lexer.token.kind == TOKEN_DIVIDE ||
		parser->lexer.token.kind == TOKEN_REMAINDER)) {
		enum token_kind operation = parser->lexer.token.kind;
		struct bc_number right;
		struct bc_number result = invalid_number();

		lexer_next(&parser->lexer);
		right = parse_power(parser);
		if (!parser->runtime->failed) {
			if (operation == TOKEN_MULTIPLY) {
				if (bc_number_multiply(&result, &left,
						       &right) != 0)
					parser_error(parser,
						     "multiplication failed");
			} else {
				struct bc_number quotient = invalid_number();
				struct bc_number remainder = invalid_number();

				if (bc_number_divide(&quotient, &remainder,
						     &left, &right) != 0)
					parser_error(parser,
						     bc_number_is_zero(&right)
							 ? "division by zero"
							 : "division failed");
				if (operation == TOKEN_DIVIDE) {
					result = quotient;
					bc_number_free(&remainder);
				} else {
					result = remainder;
					bc_number_free(&quotient);
				}
			}
		}
		bc_number_free(&left);
		bc_number_free(&right);
		left = result;
	}
	return left;
}

static struct bc_number
parse_sum(struct parser *parser)
{
	struct bc_number left = parse_product(parser);

	while (!parser->runtime->failed &&
	       (parser->lexer.token.kind == TOKEN_ADD ||
		parser->lexer.token.kind == TOKEN_SUBTRACT)) {
		enum token_kind operation = parser->lexer.token.kind;
		struct bc_number right;
		struct bc_number result = invalid_number();
		int status;

		lexer_next(&parser->lexer);
		right = parse_product(parser);
		status = operation == TOKEN_ADD
			     ? bc_number_add(&result, &left, &right)
			     : bc_number_subtract(&result, &left, &right);
		if (!parser->runtime->failed && status != 0)
			parser_error(parser, "arithmetic failed");
		bc_number_free(&left);
		bc_number_free(&right);
		left = result;
	}
	return left;
}

static struct bc_number
parse_expression(struct parser *parser)
{
	struct lexer saved = parser->lexer;

	if (saved.token.kind == TOKEN_IDENTIFIER) {
		struct token name = saved.token;

		lexer_next(&saved);
		if (saved.token.kind == TOKEN_ASSIGN) {
			struct variable *variable;
			struct bc_number value;

			parser->lexer = saved;
			lexer_next(&parser->lexer);
			value = parse_expression(parser);
			if (parser->runtime->failed)
				return value;
			if (name.length == 5 &&
			    memcmp(name.text, "scale", 5) == 0 &&
			    !bc_number_is_zero(&value)) {
				bc_number_free(&value);
				parser_error(parser, "non-zero scale is not "
						     "implemented locally");
				return invalid_number();
			}
			if ((name.length == 5 &&
			     memcmp(name.text, "ibase", 5) == 0) ||
			    (name.length == 5 &&
			     memcmp(name.text, "obase", 5) == 0)) {
				unsigned long long base;

				if (bc_number_to_ull(&value, &base) != 0 ||
				    base != 10) {
					bc_number_free(&value);
					parser_error(parser,
						     "only base 10 is "
						     "implemented locally");
					return invalid_number();
				}
			}
			variable = runtime_find(parser->runtime, name.text,
						name.length, 1);
			if (variable == NULL ||
			    bc_number_copy(&variable->value, &value) != 0) {
				bc_number_free(&value);
				parser_error(parser, "out of memory");
				return invalid_number();
			}
			return value;
		}
	}
	return parse_sum(parser);
}

static int
read_stream(FILE *stream, char **result)
{
	char *line = NULL;
	size_t line_capacity = 0;
	char *text = NULL;
	size_t length = 0;
	size_t capacity = 0;
	long count;

	while ((count = command_read_line(stream, &line, &line_capacity)) > 0) {
		size_t needed;
		char *grown;

		if ((size_t)count > SIZE_MAX - length - 1) {
			errno = EOVERFLOW;
			goto fail;
		}
		needed = length + (size_t)count + 1;
		if (needed > capacity) {
			capacity = capacity == 0 ? 4096 : capacity;
			while (capacity < needed) {
				if (capacity > SIZE_MAX / 2) {
					capacity = needed;
					break;
				}
				capacity *= 2;
			}
			grown = realloc(text, capacity);
			if (grown == NULL)
				goto fail;
			text = grown;
		}
		memcpy(text + length, line, (size_t)count);
		length += (size_t)count;
	}
	free(line);
	if (count < 0)
		goto fail_text;
	if (text == NULL) {
		text = malloc(1);
		if (text == NULL)
			return -1;
	}
	text[length] = '\0';
	*result = text;
	return 0;

fail:
	free(line);
fail_text:
	free(text);
	return -1;
}

static int
run_source(struct runtime *runtime, const char *source, const char *text)
{
	struct parser parser;

	memset(&parser, 0, sizeof(parser));
	parser.lexer.text = text;
	parser.lexer.line = 1;
	parser.runtime = runtime;
	parser.source = source;
	lexer_next(&parser.lexer);
	while (parser.lexer.token.kind != TOKEN_END) {
		struct bc_number value;
		int assignment = 0;

		while (parser.lexer.token.kind == TOKEN_SEPARATOR)
			lexer_next(&parser.lexer);
		if (parser.lexer.token.kind == TOKEN_END)
			break;
		if (parser.lexer.token.kind == TOKEN_IDENTIFIER) {
			struct lexer lookahead = parser.lexer;

			lexer_next(&lookahead);
			assignment = lookahead.token.kind == TOKEN_ASSIGN;
		}
		value = parse_expression(&parser);
		if (!runtime->failed &&
		    parser.lexer.token.kind != TOKEN_SEPARATOR &&
		    parser.lexer.token.kind != TOKEN_END)
			parser_error(&parser,
				     "unexpected token after expression");
		if (!runtime->failed && !assignment) {
			char *output = bc_number_to_string(&value);

			if (output == NULL ||
			    command_write_all(1, output, strlen(output)) != 0 ||
			    command_write_all(1, "\n", 1) != 0) {
				free(output);
				bc_number_free(&value);
				fprintf(stderr, "bc: output failed: %s\n",
					strerror(errno));
				return -1;
			}
			free(output);
		}
		bc_number_free(&value);
		if (runtime->failed)
			return -1;
		if (parser.lexer.token.kind == TOKEN_SEPARATOR)
			lexer_next(&parser.lexer);
	}
	return 0;
}

static int
run_file(struct runtime *runtime, const char *path)
{
	FILE *stream = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
	char *text;
	int result;

	if (stream == NULL) {
		fprintf(stderr, "bc: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (read_stream(stream, &text) != 0) {
		fprintf(stderr, "bc: %s: %s\n", path, strerror(errno));
		if (stream != stdin)
			(void)fclose(stream);
		return -1;
	}
	if (stream != stdin && fclose(stream) != 0) {
		fprintf(stderr, "bc: %s: %s\n", path, strerror(errno));
		free(text);
		return -1;
	}
	result = run_source(runtime, path, text);
	free(text);
	return result;
}

int
main(int argc, char **argv)
{
	struct runtime runtime = {0};
	int index = 1;
	int status = 0;

	while (index < argc && argv[index][0] == '-') {
		if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		}
		if (strcmp(argv[index], "-l") == 0) {
			fprintf(
			    stderr,
			    "bc: -l math library is not implemented locally\n");
			return 2;
		}
		fprintf(stderr, "usage: bc [-l] [file ...]\n");
		return 2;
	}
	if (index == argc)
		status = run_file(&runtime, "-") != 0;
	else {
		for (; index < argc; index++) {
			if (run_file(&runtime, argv[index]) != 0) {
				status = 1;
				break;
			}
		}
	}
	runtime_free(&runtime);
	return status;
}
