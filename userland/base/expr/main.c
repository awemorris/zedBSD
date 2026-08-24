/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

static struct value
invalid_value(void)
{
	struct value result = {NULL};

	return result;
}

static struct value
string_value(const char *text)
{
	struct value result;

	result.text = strdup(text);
	return result;
}

static struct value
integer_value(long long number)
{
	char buffer[64];

	(void)snprintf(buffer, sizeof(buffer), "%lld", number);
	return string_value(buffer);
}

static void
parser_error(struct parser *parser, int status, const char *message)
{
	if (parser->status == 0) {
		fprintf(stderr, "expr: %s\n", message);
		parser->status = status;
	}
}

static int
replace_value(struct parser *parser, struct value *target,
	      struct value replacement)
{
	if (replacement.text == NULL) {
		parser_error(parser, 3, "out of memory");
		return 0;
	}
	free(target->text);
	*target = replacement;
	return 1;
}

static int
integer_parse(const char *text, long long *result)
{
	const unsigned char *cursor = (const unsigned char *)text;
	unsigned long long value = 0;
	unsigned long long limit = (unsigned long long)LLONG_MAX;
	int negative = 0;

	if (*cursor == '-') {
		negative = 1;
		limit++;
		cursor++;
	}
	if (*cursor < '0' || *cursor > '9')
		return 0;
	while (*cursor >= '0' && *cursor <= '9') {
		unsigned digit = (unsigned)(*cursor++ - '0');

		if (value > (limit - digit) / 10U)
			return 0;
		value = value * 10U + digit;
	}
	if (*cursor != '\0')
		return 0;
	if (!negative)
		*result = (long long)value;
	else if (value == (unsigned long long)LLONG_MAX + 1U)
		*result = LLONG_MIN;
	else
		*result = -(long long)value;
	return 1;
}

static int
is_true(const struct value *value)
{
	long long number;

	if (value->text[0] == '\0')
		return 0;
	return !integer_parse(value->text, &number) || number != 0;
}

static int
accept(struct parser *parser, const char *token)
{
	if (parser->index < parser->argc &&
	    strcmp(parser->argv[parser->index], token) == 0) {
		parser->index++;
		return 1;
	}
	return 0;
}

static struct value parse_or(struct parser *parser);

static struct value
parse_primary(struct parser *parser)
{
	struct value result;

	if (accept(parser, "(")) {
		result = parse_or(parser);
		if (result.text != NULL && !accept(parser, ")")) {
			free(result.text);
			parser_error(parser, 2, "missing closing parenthesis");
			return invalid_value();
		}
		return result;
	}
	if (parser->index >= parser->argc ||
	    strcmp(parser->argv[parser->index], ")") == 0) {
		parser_error(parser, 2, "missing operand");
		return invalid_value();
	}
	result = string_value(parser->argv[parser->index++]);
	if (result.text == NULL)
		parser_error(parser, 3, "out of memory");
	return result;
}

static size_t
character_count(const char *text, size_t byte_count)
{
	mbstate_t state = {0};
	size_t offset = 0;
	size_t count = 0;

	while (offset < byte_count) {
		wchar_t character;
		size_t used = mbrtowc(&character, text + offset,
				      byte_count - offset, &state);

		if (used == (size_t)-1 || used == (size_t)-2) {
			memset(&state, 0, sizeof(state));
			used = 1;
		} else if (used == 0) {
			used = 1;
		}
		offset += used;
		count++;
	}
	return count;
}

static struct value
match_value(struct parser *parser, const char *text, const char *pattern)
{
	regex_t expression;
	regmatch_t *matches;
	struct value result = invalid_value();
	size_t match_count;
	int status;

	status = regcomp(&expression, pattern, 0);
	if (status != 0) {
		char message[128];

		(void)regerror(status, &expression, message, sizeof(message));
		fprintf(stderr, "expr: regular expression: %s\n", message);
		parser->status = 2;
		return result;
	}
	match_count = expression.re_nsub + 1U;
	if (match_count > SIZE_MAX / sizeof(*matches) ||
	    (matches = calloc(match_count, sizeof(*matches))) == NULL) {
		regfree(&expression);
		parser_error(parser, 3, "out of memory");
		return result;
	}
	status = regexec(&expression, text, match_count, matches, 0);
	if (status == REG_NOMATCH || (status == 0 && matches[0].rm_so != 0)) {
		result = expression.re_nsub != 0 ? string_value("")
						 : integer_value(0);
	} else if (status != 0) {
		char message[128];

		(void)regerror(status, &expression, message, sizeof(message));
		fprintf(stderr, "expr: regular expression: %s\n", message);
		parser->status = 3;
	} else if (expression.re_nsub != 0) {
		if (matches[1].rm_so < 0)
			result = string_value("");
		else {
			size_t length =
			    (size_t)(matches[1].rm_eo - matches[1].rm_so);

			result.text = malloc(length + 1U);
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
	if (result.text == NULL && parser->status == 0)
		parser_error(parser, 3, "out of memory");
	free(matches);
	regfree(&expression);
	return result;
}

static struct value
parse_match(struct parser *parser)
{
	struct value left = parse_primary(parser);

	while (left.text != NULL && accept(parser, ":")) {
		struct value right = parse_primary(parser);
		struct value result;

		if (right.text == NULL) {
			free(left.text);
			return invalid_value();
		}
		result = match_value(parser, left.text, right.text);
		free(right.text);
		if (!replace_value(parser, &left, result)) {
			free(left.text);
			return invalid_value();
		}
	}
	return left;
}

static struct value
parse_multiply(struct parser *parser)
{
	struct value left = parse_match(parser);

	while (left.text != NULL && parser->index < parser->argc) {
		const char *operation = parser->argv[parser->index];
		struct value right;
		long long a, b, number;
		int failed = 0;

		if (strcmp(operation, "*") != 0 &&
		    strcmp(operation, "/") != 0 && strcmp(operation, "%") != 0)
			break;
		parser->index++;
		right = parse_match(parser);
		if (right.text == NULL) {
			free(left.text);
			return invalid_value();
		}
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
		if (failed) {
			if (parser->status == 0)
				parser_error(parser, 3, "integer overflow");
			free(left.text);
			return invalid_value();
		}
		if (!replace_value(parser, &left, integer_value(number))) {
			free(left.text);
			return invalid_value();
		}
	}
	return left;
}

static struct value
parse_add(struct parser *parser)
{
	struct value left = parse_multiply(parser);

	while (left.text != NULL && parser->index < parser->argc) {
		const char *operation = parser->argv[parser->index];
		struct value right;
		long long a, b, number;
		int overflow;

		if (strcmp(operation, "+") != 0 && strcmp(operation, "-") != 0)
			break;
		parser->index++;
		right = parse_multiply(parser);
		if (right.text == NULL) {
			free(left.text);
			return invalid_value();
		}
		if (!integer_parse(left.text, &a) ||
		    !integer_parse(right.text, &b)) {
			parser_error(parser, 2, "non-integer argument");
			free(right.text);
			free(left.text);
			return invalid_value();
		}
		overflow = strcmp(operation, "+") == 0
			       ? __builtin_add_overflow(a, b, &number)
			       : __builtin_sub_overflow(a, b, &number);
		free(right.text);
		if (overflow) {
			parser_error(parser, 3, "integer overflow");
			free(left.text);
			return invalid_value();
		}
		if (!replace_value(parser, &left, integer_value(number))) {
			free(left.text);
			return invalid_value();
		}
	}
	return left;
}

static int
comparison(const char *operation, const char *left, const char *right)
{
	long long a, b;
	int order;

	if (integer_parse(left, &a) && integer_parse(right, &b))
		order = (a > b) - (a < b);
	else
		order = strcoll(left, right);
	if (strcmp(operation, "=") == 0)
		return order == 0;
	if (strcmp(operation, "!=") == 0)
		return order != 0;
	if (strcmp(operation, "<") == 0)
		return order < 0;
	if (strcmp(operation, "<=") == 0)
		return order <= 0;
	if (strcmp(operation, ">") == 0)
		return order > 0;
	return order >= 0;
}

static int
is_comparison(const char *token)
{
	return strcmp(token, "=") == 0 || strcmp(token, "!=") == 0 ||
	       strcmp(token, "<") == 0 || strcmp(token, "<=") == 0 ||
	       strcmp(token, ">") == 0 || strcmp(token, ">=") == 0;
}

static struct value
parse_comparison(struct parser *parser)
{
	struct value left = parse_add(parser);

	while (left.text != NULL && parser->index < parser->argc &&
	       is_comparison(parser->argv[parser->index])) {
		const char *operation = parser->argv[parser->index++];
		struct value right = parse_add(parser);
		struct value result;

		if (right.text == NULL) {
			free(left.text);
			return invalid_value();
		}
		result =
		    integer_value(comparison(operation, left.text, right.text));
		free(right.text);
		if (!replace_value(parser, &left, result)) {
			free(left.text);
			return invalid_value();
		}
	}
	return left;
}

static struct value
parse_and(struct parser *parser)
{
	struct value left = parse_comparison(parser);

	while (left.text != NULL && accept(parser, "&")) {
		struct value right = parse_comparison(parser);
		struct value result;

		if (right.text == NULL) {
			free(left.text);
			return invalid_value();
		}
		result = is_true(&left) && is_true(&right)
			     ? string_value(left.text)
			     : integer_value(0);
		free(right.text);
		if (!replace_value(parser, &left, result)) {
			free(left.text);
			return invalid_value();
		}
	}
	return left;
}

static struct value
parse_or(struct parser *parser)
{
	struct value left = parse_and(parser);

	while (left.text != NULL && accept(parser, "|")) {
		struct value right = parse_and(parser);
		struct value result;

		if (right.text == NULL) {
			free(left.text);
			return invalid_value();
		}
		result = is_true(&left)	   ? string_value(left.text)
			 : is_true(&right) ? string_value(right.text)
					   : integer_value(0);
		free(right.text);
		if (!replace_value(parser, &left, result)) {
			free(left.text);
			return invalid_value();
		}
	}
	return left;
}

int
main(int argc, char **argv)
{
	struct parser parser;
	struct value result;
	int status;

	(void)setlocale(LC_ALL, "");
	if (argc > 1 && strcmp(argv[1], "--") == 0) {
		argc--;
		argv++;
	}
	parser.argc = argc;
	parser.argv = argv;
	parser.index = 1;
	parser.status = 0;
	result = parse_or(&parser);
	if (result.text == NULL)
		return parser.status != 0 ? parser.status : 3;
	if (parser.index != parser.argc) {
		free(result.text);
		parser_error(&parser, 2, "unexpected operand or operator");
		return parser.status;
	}
	puts(result.text);
	status = is_true(&result) ? 0 : 1;
	free(result.text);
	return ferror(stdout) ? 3 : status;
}
