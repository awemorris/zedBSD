/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD bc userland command.
 */

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

static int run_file(struct runtime *runtime, const char *path);
static int read_stream(FILE *stream, char **result);
static int run_source(struct runtime *runtime, const char *source, const char *text);
static void lexer_next(struct lexer *lexer);
static struct bc_number parse_expression(struct parser *parser);
static void parser_error(struct parser *parser, const char *message);
static struct bc_number invalid_number(void);
static struct variable *runtime_find(struct runtime *runtime, const char *name, size_t length, int create);
static struct bc_number parse_sum(struct parser *parser);
static struct bc_number parse_product(struct parser *parser);
static struct bc_number parse_power(struct parser *parser);
static struct bc_number parse_unary(struct parser *parser);
static struct bc_number parse_primary(struct parser *parser);
static int parser_enter(struct parser *parser);
static void runtime_free(struct runtime *runtime);

/*
 * Runs the bc command.
 */
int
main(
	int argc,
	char **argv)
{
	struct runtime runtime = {0};
	int index;
	int status;

	index = 1;
	status = 0;

	/* Process each remaining command-line operand. */
	while (index < argc && argv[index][0] == '-') {
		/* Handles the selected command-line operation. */
		if (strcmp(argv[index], "--") == 0) {
			index++;
			break;
		}

		/* Handles the selected command-line operation. */
		if (strcmp(argv[index], "-l") == 0) {
			fprintf(
			    stderr,
			    "bc: -l math library is not implemented locally\n");

			/* Reports operation failure. */
			return 2;
		}
		fprintf(stderr, "usage: bc [-l] [file ...]\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Validates the command-line arguments. */
	if (index == argc) {
		status = run_file(&runtime, "-") != 0;
	} else {
		/* Process each remaining command-line operand. */
		for (; index < argc; index++) {
			/* Validates the command-line arguments. */
			if (run_file(&runtime, argv[index]) != 0) {
				status = 1;
				break;
			}
		}
	}
	runtime_free(&runtime);

	/* Returns the computed result. */
	return status;
}

/* Supports the run file operation. */
static int
run_file(
	struct runtime *runtime,
	const char *path)
{
	FILE *stream = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
	char *text;
	int result;

	/* Handles the stream availability. */
	if (stream == NULL) {
		fprintf(stderr, "bc: %s: %s\n", path, strerror(errno));

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed read stream operation. */
	if (read_stream(stream, &text) != 0) {
		fprintf(stderr, "bc: %s: %s\n", path, strerror(errno));

		/* Handles the stream condition. */
		if (stream != stdin)
			(void)fclose(stream);

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed fclose operation. */
	if (stream != stdin && fclose(stream) != 0) {
		fprintf(stderr, "bc: %s: %s\n", path, strerror(errno));
		free(text);

		/* Reports operation failure. */
		return -1;
	}
	result = run_source(runtime, path, text);
	free(text);

	/* Returns the computed result. */
	return result;
}

/* Supports the read stream operation. */
static int
read_stream(
	FILE *stream,
	char **result)
{
	size_t needed;
	char *grown;
	char *line;
	size_t line_capacity;
	char *text;
	size_t length;
	size_t capacity;
	long count;

	line = NULL;
	line_capacity = 0;
	text = NULL;
	length = 0;
	capacity = 0;

	/* Process each remaining element. */
	while ((count = command_read_line(stream, &line, &line_capacity)) > 0) {
		/* Checks the remaining item count. */
		if ((size_t)count > SIZE_MAX - length - 1) {
			errno = EOVERFLOW;
			goto fail;
		}
		needed = length + (size_t)count + 1;

		/* Handles the needed condition. */
		if (needed > capacity) {
			/* Continue while the operation condition remains true. */
			capacity = capacity == 0 ? 4096 : capacity;
			while (capacity < needed) {
				/* Handles the capacity condition. */
				if (capacity > SIZE_MAX / 2) {
					capacity = needed;
					break;
				}
				capacity *= 2;
			}
			grown = realloc(text, capacity);

			/* Handles the grown availability. */
			if (grown == NULL)
				goto fail;
			text = grown;
		}
		memcpy(text + length, line, (size_t)count);
		length += (size_t)count;
	}
	free(line);

	/* Checks the remaining item count. */
	if (count < 0)
		goto fail_text;

	/* Handles the text availability. */
	if (text == NULL) {
		text = malloc(1);

		/* Handles the text availability. */
		if (text == NULL)
			return -1;
	}
	text[length] = '\0';
	*result = text;
	/* Reports successful completion. */
	return 0;

fail:
	free(line);
fail_text:
	free(text);

	/* Reports operation failure. */
	return -1;
}

/* Supports the run source operation. */
static int
run_source(
	struct runtime *runtime,
	const char *source,
	const char *text)
{
	struct lexer lookahead;
	char *output;
	struct bc_number value;
	int assignment;
	struct parser parser;

	memset(&parser, 0, sizeof(parser));
	parser.lexer.text = text;
	parser.lexer.line = 1;
	parser.runtime = runtime;
	parser.source = source;
	lexer_next(&parser.lexer);

	/* Continue while the operation condition remains true. */
	while (parser.lexer.token.kind != TOKEN_END) {
		assignment = 0;

		/* Continue while the operation condition remains true. */
		while (parser.lexer.token.kind == TOKEN_SEPARATOR)
			lexer_next(&parser.lexer);

		/* Checks the parser state. */
		if (parser.lexer.token.kind == TOKEN_END)
			break;

		/* Checks the parser state. */
		if (parser.lexer.token.kind == TOKEN_IDENTIFIER) {
			lookahead = parser.lexer;

			lexer_next(&lookahead);
			assignment = lookahead.token.kind == TOKEN_ASSIGN;
		}
		value = parse_expression(&parser);

		/* Handles an operation failure. */
		if (!runtime->failed &&
		    parser.lexer.token.kind != TOKEN_SEPARATOR &&
		    parser.lexer.token.kind != TOKEN_END) {
			parser_error(&parser,
				     "unexpected token after expression");
		}

		/* Handles an operation failure. */
		if (!runtime->failed && !assignment) {
			output = bc_number_to_string(&value);

			/* Handles a failed command write all operation. */
			if (output == NULL ||
			    command_write_all(1, output, strlen(output)) != 0 ||
			    command_write_all(1, "\n", 1) != 0) {
				free(output);
				bc_number_free(&value);
				fprintf(stderr, "bc: output failed: %s\n",
					strerror(errno));

				/* Reports operation failure. */
				return -1;
			}
			free(output);
		}
		bc_number_free(&value);

		/* Handles an operation failure. */
		if (runtime->failed)
			return -1;

		/* Checks the parser state. */
		if (parser.lexer.token.kind == TOKEN_SEPARATOR)
			lexer_next(&parser.lexer);
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the lexer next operation. */
static void
lexer_next(
	struct lexer *lexer)
{
	char c;
	const char *text;

	text = lexer->text;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		c = text[lexer->offset];

		/* Continue while the operation condition remains true. */
		while (c == ' ' || c == '\t' || c == '\r')
			c = text[++lexer->offset];

		/* Classifies the current input character. */
		if (c == '/' && text[lexer->offset + 1] == '*') {
			lexer->offset += 2;

			/* Continue while the operation condition remains true. */
			while (text[lexer->offset] != '\0' &&
			       !(text[lexer->offset] == '*' &&
				 text[lexer->offset + 1] == '/')) {
				/* Validates the current text. */
				if (text[lexer->offset++] == '\n')
					lexer->line++;
			}

			/* Validates the current text. */
			if (text[lexer->offset] == '\0') {
				lexer->token.kind = TOKEN_ERROR;
				lexer->token.text = "unterminated comment";
				lexer->token.length = strlen(lexer->token.text);
				lexer->token.line = lexer->line;

				/* Returns the computed result. */
				return;
			}
			lexer->offset += 2;
			continue;
		}

		/* Classifies the current input character. */
		if (c == '#') {
			/* Continue while the operation condition remains true. */
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

	/* Dispatch the selected operation case. */
	switch (text[lexer->offset]) {
	case '\0':
		lexer->token.kind = TOKEN_END;
		lexer->token.length = 0;

		/* Returns the computed result. */
		return;
	case '\n':
		lexer->token.kind = TOKEN_SEPARATOR;
		lexer->offset++;
		lexer->line++;

		/* Returns the computed result. */
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
		/* Validates the current text. */
		if (text[lexer->offset] >= '0' && text[lexer->offset] <= '9') {
			/* Process each remaining element. */
			lexer->token.kind = TOKEN_NUMBER;
			while (text[lexer->offset + lexer->token.length] >=
				   '0' &&
			       text[lexer->offset + lexer->token.length] <= '9')
				lexer->token.length++;
			lexer->offset += lexer->token.length;

			/* Returns the computed result. */
			return;
		}

		/* Validates the current text. */
		if ((text[lexer->offset] >= 'A' &&
		     text[lexer->offset] <= 'Z') ||
		    (text[lexer->offset] >= 'a' &&
		     text[lexer->offset] <= 'z') ||
		    text[lexer->offset] == '_') {
			/* Process each remaining element. */
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

			/* Returns the computed result. */
			return;
		}
		lexer->token.kind = TOKEN_ERROR;
		break;
	}
	lexer->offset++;
}

/* Supports the parse expression operation. */
static struct bc_number
parse_expression(
	struct parser *parser)
{
	struct bc_number function_result;
	unsigned long long base;
	struct variable *variable;
	struct bc_number value;
	struct token name;
	struct lexer saved;

	saved = parser->lexer;

	/* Handles the saved condition. */
	if (saved.token.kind == TOKEN_IDENTIFIER) {
		name = saved.token;

		lexer_next(&saved);

		/* Handles the saved condition. */
		if (saved.token.kind == TOKEN_ASSIGN) {
			parser->lexer = saved;
			lexer_next(&parser->lexer);
			value = parse_expression(parser);

			/* Handles an operation failure. */
			if (parser->runtime->failed)
				return value;

			/* Handles a failed bc number is zero operation. */
			if (name.length == 5 &&
			    memcmp(name.text, "scale", 5) == 0 &&
			    !bc_number_is_zero(&value)) {
				bc_number_free(&value);
				parser_error(parser, "non-zero scale is not "
						     "implemented locally");

				/* Obtains the invalid number result. */
				function_result = invalid_number();

				/* Returns the computed result. */
				return function_result;
			}

			/* Validates the current name. */
			if ((name.length == 5 &&
			     memcmp(name.text, "ibase", 5) == 0) ||
			    (name.length == 5 &&
			     memcmp(name.text, "obase", 5) == 0)) {
				/* Handles a failed bc number to ull operation. */
				if (bc_number_to_ull(&value, &base) != 0 ||
				    base != 10) {
					bc_number_free(&value);
					parser_error(parser,
						     "only base 10 is "
						     "implemented locally");

					/* Obtains the invalid number result. */
					function_result = invalid_number();

					/* Returns the computed result. */
					return function_result;
				}
			}
			variable = runtime_find(parser->runtime, name.text,
						name.length, 1);

			/* Handles a failed bc number copy operation. */
			if (variable == NULL ||
			    bc_number_copy(&variable->value, &value) != 0) {
				bc_number_free(&value);
				parser_error(parser, "out of memory");

				/* Obtains the invalid number result. */
				function_result = invalid_number();

				/* Returns the computed result. */
				return function_result;
			}

			/* Returns the computed result. */
			return value;
		}
	}

	/* Obtains the parse sum result. */
	function_result = parse_sum(parser);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the parser error operation. */
static void
parser_error(
	struct parser *parser,
	const char *message)
{
	/* Handles an operation failure. */
	if (!parser->runtime->failed) {
		fprintf(stderr, "bc: %s:%u: %s\n", parser->source,
			parser->lexer.token.line, message);
	}
	parser->runtime->failed = 1;
}

/* Supports the invalid number operation. */
static struct bc_number
invalid_number(
	void)
{
	struct bc_number number;

	bc_number_init(&number);

	/* Returns the computed result. */
	return number;
}

/* Supports the runtime find operation. */
static struct variable *
runtime_find(
	struct runtime *runtime,
	const char *name,
	size_t length,
	int create)
{
	size_t capacity;
	struct variable *variable;
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < runtime->count; index++) {
		/* Handles a failed strlen operation. */
		if (strlen(runtime->variable[index].name) == length &&
		    memcmp(runtime->variable[index].name, name, length) == 0)

			/* Returns the computed result. */
			return &runtime->variable[index];
	}

	/* Handles the create condition. */
	if (!create)
		return NULL;

	/* Checks the current data length. */
	if (length > BC_IDENTIFIER_MAX) {
		errno = EOVERFLOW;

		/* Reports that no result is available. */
		return NULL;
	}

	/* Handles the runtime condition. */
	if (runtime->count == runtime->capacity) {
		capacity = runtime->capacity == 0 ? 16 : runtime->capacity * 2;

		/* Handles the capacity condition. */
		if (capacity < runtime->capacity ||
		    capacity > SIZE_MAX / sizeof(*variable)) {
			errno = EOVERFLOW;

			/* Reports that no result is available. */
			return NULL;
		}
		variable =
		    realloc(runtime->variable, capacity * sizeof(*variable));

		/* Handles the variable availability. */
		if (variable == NULL)
			return NULL;
		runtime->variable = variable;
		runtime->capacity = capacity;
	}
	memcpy(runtime->variable[runtime->count].name, name, length);
	runtime->variable[runtime->count].name[length] = '\0';
	bc_number_init(&runtime->variable[runtime->count].value);

	/* Returns the computed result. */
	return &runtime->variable[runtime->count++];
}

/* Supports the parse sum operation. */
static struct bc_number
parse_sum(
	struct parser *parser)
{
	enum token_kind operation;
	struct bc_number right;
	struct bc_number result;
	int status;
	struct bc_number left;

	left = parse_product(parser);

	/* Continue while the operation condition remains true. */
	while (!parser->runtime->failed &&
	       (parser->lexer.token.kind == TOKEN_ADD ||
		parser->lexer.token.kind == TOKEN_SUBTRACT)) {
		operation = parser->lexer.token.kind;
		result = invalid_number();

		lexer_next(&parser->lexer);
		right = parse_product(parser);
		status = operation == TOKEN_ADD
			     ? bc_number_add(&result, &left, &right)
			     : bc_number_subtract(&result, &left, &right);

		/* Handles an operation failure. */
		if (!parser->runtime->failed && status != 0)
			parser_error(parser, "arithmetic failed");
		bc_number_free(&left);
		bc_number_free(&right);
		left = result;
	}

	/* Returns the computed result. */
	return left;
}

/* Supports the parse product operation. */
static struct bc_number
parse_product(
	struct parser *parser)
{
	struct bc_number quotient;
	struct bc_number remainder;
	enum token_kind operation;
	struct bc_number right;
	struct bc_number result;
	struct bc_number left;

	left = parse_power(parser);

	/* Continue while the operation condition remains true. */
	while (!parser->runtime->failed &&
	       (parser->lexer.token.kind == TOKEN_MULTIPLY ||
		parser->lexer.token.kind == TOKEN_DIVIDE ||
		parser->lexer.token.kind == TOKEN_REMAINDER)) {
		operation = parser->lexer.token.kind;
		result = invalid_number();

		lexer_next(&parser->lexer);
		right = parse_power(parser);

		/* Handles an operation failure. */
		if (!parser->runtime->failed) {
			/* Validates the selected operation. */
			if (operation == TOKEN_MULTIPLY) {
				/* Handles a failed bc number multiply operation. */
				if (bc_number_multiply(&result, &left,
						       &right) != 0) {
					parser_error(parser,
						     "multiplication failed");
				}
			} else {
				quotient = invalid_number();
				remainder = invalid_number();

				/* Handles a failed bc number divide operation. */
				if (bc_number_divide(&quotient, &remainder,
						     &left, &right) != 0) {
					parser_error(parser,
						     bc_number_is_zero(&right)
							 ? "division by zero"
							 : "division failed");
				}

				/* Validates the selected operation. */
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

	/* Returns the computed result. */
	return left;
}

/* Supports the parse power operation. */
static struct bc_number
parse_power(
	struct parser *parser)
{
	struct bc_number right;
	struct bc_number result;
	struct bc_number left;

	left = parse_unary(parser);

	/* Handles an operation failure. */
	if (!parser->runtime->failed &&
	    parser->lexer.token.kind == TOKEN_POWER) {
		result = invalid_number();

		lexer_next(&parser->lexer);
		right = parse_power(parser);

		/* Handles an operation failure. */
		if (!parser->runtime->failed &&
		    bc_number_power(&result, &left, &right) != 0)
			parser_error(parser, "negative or excessive exponent");
		bc_number_free(&left);
		bc_number_free(&right);
		left = result;
	}

	/* Returns the computed result. */
	return left;
}

/* Supports the parse unary operation. */
static struct bc_number
parse_unary(
	struct parser *parser)
{
	struct bc_number function_result;
	struct bc_number value;
	struct bc_number result;

	/* Checks the parser state. */
	if (parser->lexer.token.kind == TOKEN_ADD) {
		lexer_next(&parser->lexer);

		/* Obtains the parse unary result. */
		function_result = parse_unary(parser);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the parser state. */
	if (parser->lexer.token.kind == TOKEN_SUBTRACT) {
		result = invalid_number();

		lexer_next(&parser->lexer);
		value = parse_unary(parser);

		/* Handles an operation failure. */
		if (!parser->runtime->failed &&
		    bc_number_negate(&result, &value) != 0)
			parser_error(parser, "out of memory");
		bc_number_free(&value);

		/* Returns the computed result. */
		return result;
	}

	/* Obtains the parse primary result. */
	function_result = parse_primary(parser);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the parse primary operation. */
static struct bc_number
parse_primary(
	struct parser *parser)
{
	struct variable *variable;
	struct bc_number result;
	struct token token;

	result = invalid_number();
	token = parser->lexer.token;

	/* Handles a failed parser enter operation. */
	if (!parser_enter(parser))
		return result;

	/* Handles the token condition. */
	if (token.kind == TOKEN_NUMBER) {
		/* Handles a failed bc number from decimal operation. */
		if (bc_number_from_decimal(&result, token.text, token.length) !=
		    0)
			parser_error(parser, "invalid or too large number");
		lexer_next(&parser->lexer);
	} else if (token.kind == TOKEN_IDENTIFIER) {
		variable = runtime_find(parser->runtime, token.text, token.length, 0);

		/* Handles a failed bc number copy operation. */
		if (variable != NULL &&
		    bc_number_copy(&result, &variable->value) != 0)
			parser_error(parser, "out of memory");
		lexer_next(&parser->lexer);
	} else if (token.kind == TOKEN_LEFT) {
		lexer_next(&parser->lexer);
		result = parse_expression(parser);

		/* Checks the parser state. */
		if (parser->lexer.token.kind != TOKEN_RIGHT) {
			bc_number_free(&result);
			parser_error(parser, "missing closing parenthesis");
		} else {
			lexer_next(&parser->lexer);
		}
	} else {
		parser_error(parser, token.kind == TOKEN_ERROR
					 ? "invalid input"
					 : "expected number, variable, or '('");
	}
	parser->depth--;

	/* Returns the computed result. */
	return result;
}

/* Supports the parser enter operation. */
static int
parser_enter(
	struct parser *parser)
{
	/* Checks the parser state. */
	if (++parser->depth > BC_PARSE_DEPTH_MAX) {
		parser_error(parser, "expression nesting limit exceeded");

		/* Reports successful completion. */
		return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the runtime free operation. */
static void
runtime_free(
	struct runtime *runtime)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < runtime->count; index++)
		bc_number_free(&runtime->variable[index].value);
	free(runtime->variable);
}
