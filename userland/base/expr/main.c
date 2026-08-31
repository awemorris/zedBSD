/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD expr userland command.
 */

#include <limits.h>
#include <locale.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct value {
	char *text;
};

struct parser {
	int argc;
	char **argv;
	int index;
	int status;
};

static struct value parse_or(struct parser *parser);
static struct value parse_and(struct parser *parser);
static struct value parse_comparison(struct parser *parser);
static struct value parse_add(struct parser *parser);
static struct value parse_multiply(struct parser *parser);
static struct value parse_match(struct parser *parser);
static struct value parse_primary(struct parser *parser);
static int accept(struct parser *parser, const char *token);
static void parser_error(struct parser *parser, int status, const char *message);
static struct value invalid_value(void);
static struct value string_value(const char *text);
static struct value match_value(struct parser *parser, const char *text, const char *pattern);
static struct value integer_value(long long number);
static size_t character_count(const char *text, size_t byte_count);
static int replace_value(struct parser *parser, struct value *target, struct value replacement);
static int integer_parse(const char *text, long long *result);
static int is_comparison(const char *token);
static int comparison(const char *operation, const char *left, const char *right);
static int is_true(const struct value *value);

/*
 * Runs the expr command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	struct parser parser;
	struct value result;
	int status;

	(void)setlocale(LC_ALL, "");

	/* Handles the selected command-line operation. */
	if (argc > 1 && strcmp(argv[1], "--") == 0) {
		argc--;
		argv++;
	}
	parser.argc = argc;
	parser.argv = argv;
	parser.index = 1;
	parser.status = 0;
	result = parse_or(&parser);

	/* Handles the text availability. */
	if (result.text == NULL)
		return parser.status != 0 ? parser.status : 3;

	/* Validates the command-line arguments. */
	if (parser.index != parser.argc) {
		free(result.text);
		parser_error(&parser, 2, "unexpected operand or operator");

		/* Returns the computed result. */
		return parser.status;
	}
	puts(result.text);
	status = is_true(&result) ? 0 : 1;
	free(result.text);

	/* Computes the function result. */
	function_result = ferror(stdout) ? 3 : status;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the parse or operation. */
static struct value
parse_or(
	struct parser *parser)
{
	struct value function_result;
	struct value right;
	struct value result;
	struct value left;

	left = parse_and(parser);

	/* Continue while the operation condition remains true. */
	while (left.text != NULL && accept(parser, "|")) {
		right = parse_and(parser);

		/* Handles the text availability. */
		if (right.text == NULL) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}
		result = is_true(&left)	   ? string_value(left.text)
			 : is_true(&right) ? string_value(right.text)
					   : integer_value(0);
		free(right.text);

		/* Handles a failed replace value operation. */
		if (!replace_value(parser, &left, result)) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Returns the computed result. */
	return left;
}

/* Supports the parse and operation. */
static struct value
parse_and(
	struct parser *parser)
{
	struct value function_result;
	struct value right;
	struct value result;
	struct value left;

	left = parse_comparison(parser);

	/* Continue while the operation condition remains true. */
	while (left.text != NULL && accept(parser, "&")) {
		right = parse_comparison(parser);

		/* Handles the text availability. */
		if (right.text == NULL) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}
		result = is_true(&left) && is_true(&right)
			     ? string_value(left.text)
			     : integer_value(0);
		free(right.text);

		/* Handles a failed replace value operation. */
		if (!replace_value(parser, &left, result)) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Returns the computed result. */
	return left;
}

/* Supports the parse comparison operation. */
static struct value
parse_comparison(
	struct parser *parser)
{
	struct value function_result;
	const char *operation;
	struct value right;
	struct value result;
	struct value left;

	left = parse_add(parser);

	/* Process each remaining command-line operand. */
	while (left.text != NULL && parser->index < parser->argc &&
	       is_comparison(parser->argv[parser->index])) {
		operation = parser->argv[parser->index++];
		right = parse_add(parser);

		/* Handles the text availability. */
		if (right.text == NULL) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}
		result =
		    integer_value(comparison(operation, left.text, right.text));
		free(right.text);

		/* Handles a failed replace value operation. */
		if (!replace_value(parser, &left, result)) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Returns the computed result. */
	return left;
}

/* Supports the parse add operation. */
static struct value
parse_add(
	struct parser *parser)
{
	struct value function_result;
	const char *operation;
	struct value right;
	long long a, b, number;
	int overflow;
	struct value left;

	left = parse_multiply(parser);

	/* Process each remaining command-line operand. */
	while (left.text != NULL && parser->index < parser->argc) {
		operation = parser->argv[parser->index];

		/* Selects the matching value. */
		if (strcmp(operation, "+") != 0 && strcmp(operation, "-") != 0)
			break;
		parser->index++;
		right = parse_multiply(parser);

		/* Handles the text availability. */
		if (right.text == NULL) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles a failed integer parse operation. */
		if (!integer_parse(left.text, &a) ||
		    !integer_parse(right.text, &b)) {
			parser_error(parser, 2, "non-integer argument");
			free(right.text);
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}
		overflow = strcmp(operation, "+") == 0
			       ? __builtin_add_overflow(a, b, &number)
			       : __builtin_sub_overflow(a, b, &number);
		free(right.text);

		/* Handles the overflow condition. */
		if (overflow) {
			parser_error(parser, 3, "integer overflow");
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles a failed replace value operation. */
		if (!replace_value(parser, &left, integer_value(number))) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Returns the computed result. */
	return left;
}

/* Supports the parse multiply operation. */
static struct value
parse_multiply(
	struct parser *parser)
{
	struct value function_result;
	const char *operation;
	struct value right;
	long long a, b, number;
	int failed;
	struct value left;

	left = parse_match(parser);

	/* Process each remaining command-line operand. */
	while (left.text != NULL && parser->index < parser->argc) {
		operation = parser->argv[parser->index];

		failed = 0;

		/* Selects the matching value. */
		if (strcmp(operation, "*") != 0 &&
		    strcmp(operation, "/") != 0 && strcmp(operation, "%") != 0)
			break;
		parser->index++;
		right = parse_match(parser);

		/* Handles the text availability. */
		if (right.text == NULL) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles a failed integer parse operation. */
		if (!integer_parse(left.text, &a) ||
		    !integer_parse(right.text, &b)) {
			parser_error(parser, 2, "non-integer argument");
			failed = 1;
		} else if (strcmp(operation, "*") == 0) {
			failed = __builtin_mul_overflow(a, b, &number);
		} else if (b == 0) {
			parser_error(parser, 3, "division by zero");
			failed = 1;
		} else if (a == LLONG_MIN && b == -1) {
			parser_error(parser, 3, "integer overflow");
			failed = 1;
		} else if (strcmp(operation, "/") == 0)
			number = a / b;
		else
			number = a % b;
		free(right.text);

		/* Handles an operation failure. */
		if (failed) {
			/* Checks the parser state. */
			if (parser->status == 0)
				parser_error(parser, 3, "integer overflow");
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles a failed replace value operation. */
		if (!replace_value(parser, &left, integer_value(number))) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Returns the computed result. */
	return left;
}

/* Supports the parse match operation. */
static struct value
parse_match(
	struct parser *parser)
{
	struct value function_result;
	struct value right;
	struct value result;
	struct value left;

	left = parse_primary(parser);

	/* Continue while the operation condition remains true. */
	while (left.text != NULL && accept(parser, ":")) {
		right = parse_primary(parser);

		/* Handles the text availability. */
		if (right.text == NULL) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}
		result = match_value(parser, left.text, right.text);
		free(right.text);

		/* Handles a failed replace value operation. */
		if (!replace_value(parser, &left, result)) {
			free(left.text);

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Returns the computed result. */
	return left;
}

/* Supports the parse primary operation. */
static struct value
parse_primary(
	struct parser *parser)
{
	struct value function_result;
	struct value result;

	/* Handles the accept condition. */
	if (accept(parser, "(")) {
		result = parse_or(parser);

		/* Handles a failed accept operation. */
		if (result.text != NULL && !accept(parser, ")")) {
			free(result.text);
			parser_error(parser, 2, "missing closing parenthesis");

			/* Obtains the invalid value result. */
			function_result = invalid_value();

			/* Returns the computed result. */
			return function_result;
		}

		/* Returns the computed result. */
		return result;
	}

	/* Handles the selected command-line operation. */
	if (parser->index >= parser->argc ||
	    strcmp(parser->argv[parser->index], ")") == 0) {
		parser_error(parser, 2, "missing operand");

		/* Obtains the invalid value result. */
		function_result = invalid_value();

		/* Returns the computed result. */
		return function_result;
	}
	result = string_value(parser->argv[parser->index++]);

	/* Handles the text availability. */
	if (result.text == NULL)
		parser_error(parser, 3, "out of memory");

	/* Returns the computed result. */
	return result;
}

/* Supports the accept operation. */
static int
accept(
	struct parser *parser,
	const char *token)
{
	/* Handles the selected command-line operation. */
	if (parser->index < parser->argc &&
	    strcmp(parser->argv[parser->index], token) == 0) {
		parser->index++;

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the parser error operation. */
static void
parser_error(
	struct parser *parser,
	int status,
	const char *message)
{
	/* Checks the parser state. */
	if (parser->status == 0) {
		fprintf(stderr, "expr: %s\n", message);
		parser->status = status;
	}
}

/* Supports the invalid value operation. */
static struct value
invalid_value(
	void)
{
	struct value result = {NULL};

	/* Returns the computed result. */
	return result;
}

/* Supports the string value operation. */
static struct value
string_value(
	const char *text)
{
	struct value result;

	result.text = strdup(text);

	/* Returns the computed result. */
	return result;
}

/* Supports the match value operation. */
static struct value
match_value(
	struct parser *parser,
	const char *text,
	const char *pattern)
{
	char message_local[128];
	char message_local1[128];
	size_t length;
	regex_t expression;
	regmatch_t *matches;
	struct value result;
	size_t match_count;
	int status;

	result = invalid_value();

	status = regcomp(&expression, pattern, 0);

	/* Checks the operation status. */
	if (status != 0) {
		(void)regerror(status, &expression, message_local, sizeof(message_local));
		fprintf(stderr, "expr: regular expression: %s\n", message_local);
		parser->status = 2;

		/* Returns the computed result. */
		return result;
	}
	match_count = expression.re_nsub + 1U;

	/* Handles a failed calloc operation. */
	if (match_count > SIZE_MAX / sizeof(*matches) ||
	    (matches = calloc(match_count, sizeof(*matches))) == NULL) {
		regfree(&expression);
		parser_error(parser, 3, "out of memory");

		/* Returns the computed result. */
		return result;
	}
	status = regexec(&expression, text, match_count, matches, 0);

	/* Checks the operation status. */
	if (status == REG_NOMATCH || (status == 0 && matches[0].rm_so != 0)) {
		result = expression.re_nsub != 0 ? string_value("")
						 : integer_value(0);
	} else if (status != 0) {
		(void)regerror(status, &expression, message_local1, sizeof(message_local1));
		fprintf(stderr, "expr: regular expression: %s\n", message_local1);
		parser->status = 3;
	} else if (expression.re_nsub != 0) {
		/* Handles the matches condition. */
		if (matches[1].rm_so < 0) {
			result = string_value("");
		} else {
			length = (size_t)(matches[1].rm_eo - matches[1].rm_so);

			result.text = malloc(length + 1U);

			/* Handles the text availability. */
			if (result.text != NULL) {
				memcpy(result.text, text + matches[1].rm_so,
				       length);
				result.text[length] = '\0';
			}
		}
	} else {
		result = integer_value((long long)character_count(
		    text, (size_t)(matches[0].rm_eo - matches[0].rm_so)));
	}

	/* Handles the text availability. */
	if (result.text == NULL && parser->status == 0)
		parser_error(parser, 3, "out of memory");
	free(matches);
	regfree(&expression);

	/* Returns the computed result. */
	return result;
}

/* Supports the integer value operation. */
static struct value
integer_value(
	long long number)
{
	struct value function_result;
	char buffer[64];

	(void)snprintf(buffer, sizeof(buffer), "%lld", number);

	/* Obtains the string value result. */
	function_result = string_value(buffer);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the character count operation. */
static size_t
character_count(
	const char *text,
	size_t byte_count)
{
	wchar_t character;
	size_t used;
	mbstate_t state = {0};
	size_t offset;
	size_t count;

	offset = 0;
	count = 0;

	/* Process each remaining element. */
	while (offset < byte_count) {
		used = mbrtowc(&character, text + offset,
				      byte_count - offset, &state);

		/* Checks the current capacity usage. */
		if (used == (size_t)-1 || used == (size_t)-2) {
			memset(&state, 0, sizeof(state));
			used = 1;
		} else if (used == 0) {
			used = 1;
		}
		offset += used;
		count++;
	}

	/* Returns the computed result. */
	return count;
}

/* Supports the replace value operation. */
static int
replace_value(
	struct parser *parser,
	struct value *target,
	struct value replacement)
{
	/* Handles the text availability. */
	if (replacement.text == NULL) {
		parser_error(parser, 3, "out of memory");

		/* Reports successful completion. */
		return 0;
	}
	free(target->text);
	*target = replacement;
	/* Reports operation failure. */
	return 1;
}

/* Supports the integer parse operation. */
static int
integer_parse(
	const char *text,
	long long *result)
{
	unsigned digit;
	const unsigned char *cursor;
	unsigned long long value;
	unsigned long long limit;
	int negative;

	cursor = (const unsigned char *)text;
	value = 0;
	limit = (unsigned long long)LLONG_MAX;
	negative = 0;

	/* Checks the current cursor position. */
	if (*cursor == '-') {
		negative = 1;
		limit++;
		cursor++;
	}

	/* Checks the current cursor position. */
	if (*cursor < '0' || *cursor > '9')
		return 0;

	/* Continue while the operation condition remains true. */
	while (*cursor >= '0' && *cursor <= '9') {
		digit = (unsigned)(*cursor++ - '0');

		/* Validates the current value. */
		if (value > (limit - digit) / 10U)
			return 0;
		value = value * 10U + digit;
	}

	/* Checks the current cursor position. */
	if (*cursor != '\0')
		return 0;

	/* Handles the negative condition. */
	if (!negative)
		*result = (long long)value;
	else if (value == (unsigned long long)LLONG_MAX + 1U)
		*result = LLONG_MIN;
	else
		*result = -(long long)value;
	/* Reports operation failure. */
	return 1;
}

/* Supports the is comparison operation. */
static int
is_comparison(
	const char *token)
{
	int function_result;

	/* Computes the function result. */
	function_result = strcmp(token, "=") == 0 || strcmp(token, "!=") == 0 ||
	       strcmp(token, "<") == 0 || strcmp(token, "<=") == 0 ||
	       strcmp(token, ">") == 0 || strcmp(token, ">=") == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the comparison operation. */
static int
comparison(
	const char *operation,
	const char *left,
	const char *right)
{
	long long a, b;
	int order;

	/* Handles the integer parse condition. */
	if (integer_parse(left, &a) && integer_parse(right, &b))
		order = (a > b) - (a < b);
	else
		order = strcoll(left, right);

	/* Selects the matching value. */
	if (strcmp(operation, "=") == 0)
		return order == 0;

	/* Selects the matching value. */
	if (strcmp(operation, "!=") == 0)
		return order != 0;

	/* Selects the matching value. */
	if (strcmp(operation, "<") == 0)
		return order < 0;

	/* Selects the matching value. */
	if (strcmp(operation, "<=") == 0)
		return order <= 0;

	/* Selects the matching value. */
	if (strcmp(operation, ">") == 0)
		return order > 0;

	/* Returns the computed result. */
	return order >= 0;
}

/* Supports the is true operation. */
static int
is_true(
	const struct value *value)
{
	int function_result;
	long long number;

	/* Validates the current value. */
	if (value->text[0] == '\0')
		return 0;

	/* Computes the function result. */
	function_result = !integer_parse(value->text, &number) || number != 0;

	/* Returns the computed result. */
	return function_result;
}
