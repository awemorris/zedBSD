/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland arithmetic component.
 */

#include "userland/base/sh/arithmetic.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct arithmetic_parser {
	const char *cursor;
	const char *(*lookup)(void *, const char *);
	void *context;
	const char *error;
};

static int parse_conditional(struct arithmetic_parser *parser, long *result);
static int parse_logical_or(struct arithmetic_parser *p, long *r);
static int parse_logical_and(struct arithmetic_parser *p, long *r);
static int parse_bit_or(struct arithmetic_parser *p, long *r);
static int parse_bit_xor(struct arithmetic_parser *p, long *r);
static int parse_bit_and(struct arithmetic_parser *p, long *r);
static int parse_equal(struct arithmetic_parser *p, long *r);
static int parse_relation(struct arithmetic_parser *p, long *r);
static int parse_shift(struct arithmetic_parser *p, long *r);
static int parse_add(struct arithmetic_parser *p, long *r);
static int parse_multiply(struct arithmetic_parser *p, long *r);
static int parse_unary(struct arithmetic_parser *parser, long *result);
static int accept(struct arithmetic_parser *parser, const char *operator);
static void skip_space(struct arithmetic_parser *parser);
static int parse_primary(struct arithmetic_parser *parser, long *result);

/*
 * Implements the sh arithmetic eval operation.
 */
int
sh_arithmetic_eval(
	const char *text,
	const char *(*lookup)(void *, const char *),
	void *context,
	long *result,
	const char **error_text)
{
	struct arithmetic_parser parser;

	parser.cursor = text;
	parser.lookup = lookup;
	parser.context = context;
	parser.error = NULL;

	/* Handles a failed parse conditional operation. */
	if (!parse_conditional(&parser, result)) {
		*error_text = parser.error == NULL
				  ? "invalid arithmetic expression"
				  : parser.error;

		/* Reports successful completion. */
		return 0;
	}
	skip_space(&parser);

	/* Checks the parser state. */
	if (*parser.cursor != '\0') {
		*error_text = "trailing text in arithmetic expansion";
		/* Reports successful completion. */
		return 0;
	}
	*error_text = NULL;
	/* Reports operation failure. */
	return 1;
}

/* Supports the parse conditional operation. */
static int
parse_conditional(
	struct arithmetic_parser *parser,
	long *result)
{
	long true_value, false_value;

	/* Handles a failed parse logical or operation. */
	if (!parse_logical_or(parser, result))
		return 0;

	/* Handles a failed accept operation. */
	if (!accept(parser, "?"))
		return 1;

	/* Handles a failed parse conditional operation. */
	if (!parse_conditional(parser, &true_value) || !accept(parser, ":") ||
	    !parse_conditional(parser, &false_value)) {
		/* Handles an operation failure. */
		if (parser->error == NULL) {
			parser->error =
			    "invalid conditional arithmetic expression";
		}

		/* Reports successful completion. */
		return 0;
	}
	*result = *result ? true_value : false_value;
	/* Reports operation failure. */
	return 1;
}

/* Supports the parse logical or operation. */
static int
parse_logical_or(
	struct arithmetic_parser *p,
	long *r)
{
	long v;

	/* Handles a failed parse logical and operation. */
	if (!parse_logical_and(p, r))
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the accept condition. */
		if (accept(p, "||")) {
			/* Handles a failed parse logical and operation. */
			if (!parse_logical_and(p, &v))
				return 0;
			*r = *r || v;
		} else {
			/* Reports operation failure. */
			return 1;
		}
	}
}

/* Supports the parse logical and operation. */
static int
parse_logical_and(
	struct arithmetic_parser *p,
	long *r)
{
	long v;

	/* Handles a failed parse bit or operation. */
	if (!parse_bit_or(p, r))
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the accept condition. */
		if (accept(p, "&&")) {
			/* Handles a failed parse bit or operation. */
			if (!parse_bit_or(p, &v))
				return 0;
			*r = *r && v;
		} else {
			/* Reports operation failure. */
			return 1;
		}
	}
}

/* Supports the parse bit or operation. */
static int
parse_bit_or(
	struct arithmetic_parser *p,
	long *r)
{
	long v;

	/* Handles a failed parse bit xor operation. */
	if (!parse_bit_xor(p, r))
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		skip_space(p);

		/* Checks the current pointer. */
		if (p->cursor[0] != '|' || p->cursor[1] == '|')
			return 1;
		p->cursor++;

		/* Handles a failed parse bit xor operation. */
		if (!parse_bit_xor(p, &v))
			return 0;
		*r |= v;
	}
}

/* Supports the parse bit xor operation. */
static int
parse_bit_xor(
	struct arithmetic_parser *p,
	long *r)
{
	long v;

	/* Handles a failed parse bit and operation. */
	if (!parse_bit_and(p, r))
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the accept condition. */
		if (accept(p, "^")) {
			/* Handles a failed parse bit and operation. */
			if (!parse_bit_and(p, &v))
				return 0;
			*r ^= v;
		} else {
			/* Reports operation failure. */
			return 1;
		}
	}
}

/* Supports the parse bit and operation. */
static int
parse_bit_and(
	struct arithmetic_parser *p,
	long *r)
{
	long v;

	/* Handles a failed parse equal operation. */
	if (!parse_equal(p, r))
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		skip_space(p);

		/* Checks the current pointer. */
		if (p->cursor[0] != '&' || p->cursor[1] == '&')
			return 1;
		p->cursor++;

		/* Handles a failed parse equal operation. */
		if (!parse_equal(p, &v))
			return 0;
		*r &= v;
	}
}

/* Supports the parse equal operation. */
static int
parse_equal(
	struct arithmetic_parser *p,
	long *r)
{
	long v;

	/* Handles a failed parse relation operation. */
	if (!parse_relation(p, r))
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed accept operation. */
		if (accept(p, "==")) {
			/* Handles a failed parse relation operation. */
			if (!parse_relation(p, &v))
				return 0;
			*r = *r == v;
		} else if (accept(p, "!=")) {
			/* Handles a failed parse relation operation. */
			if (!parse_relation(p, &v))
				return 0;
			*r = *r != v;
		} else {
			/* Reports operation failure. */
			return 1;
		}
	}
}

/* Supports the parse relation operation. */
static int
parse_relation(
	struct arithmetic_parser *p,
	long *r)
{
	long v;

	/* Handles a failed parse shift operation. */
	if (!parse_shift(p, r))
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed accept operation. */
		if (accept(p, "<=")) {
			/* Handles a failed parse shift operation. */
			if (!parse_shift(p, &v))
				return 0;
			*r = *r <= v;
		} else if (accept(p, ">=")) {
			/* Handles a failed parse shift operation. */
			if (!parse_shift(p, &v))
				return 0;
			*r = *r >= v;
		} else if (accept(p, "<")) {
			/* Handles a failed parse shift operation. */
			if (!parse_shift(p, &v))
				return 0;
			*r = *r < v;
		} else if (accept(p, ">")) {
			/* Handles a failed parse shift operation. */
			if (!parse_shift(p, &v))
				return 0;
			*r = *r > v;
		} else {
			/* Reports operation failure. */
			return 1;
		}
	}
}

/* Supports the parse shift operation. */
static int
parse_shift(
	struct arithmetic_parser *p,
	long *r)
{
	long v;

	/* Handles a failed parse add operation. */
	if (!parse_add(p, r))
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed accept operation. */
		if (accept(p, "<<")) {
			/* Handles a failed parse add operation. */
			if (!parse_add(p, &v))
				return 0;
			*r <<= v;
		} else if (accept(p, ">>")) {
			/* Handles a failed parse add operation. */
			if (!parse_add(p, &v))
				return 0;
			*r >>= v;
		} else {
			/* Reports operation failure. */
			return 1;
		}
	}
}

/* Supports the parse add operation. */
static int
parse_add(
	struct arithmetic_parser *p,
	long *r)
{
	long v;

	/* Handles a failed parse multiply operation. */
	if (!parse_multiply(p, r))
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the accept condition. */
		if (accept(p, "+")) {
			/* Handles a failed parse multiply operation. */
			if (!parse_multiply(p, &v))
				return 0;
			*r += v;
		} else if (accept(p, "-")) {
			/* Handles a failed parse multiply operation. */
			if (!parse_multiply(p, &v))
				return 0;
			*r -= v;
		} else {
			/* Reports operation failure. */
			return 1;
		}
	}
}

/* Supports the parse multiply operation. */
static int
parse_multiply(
	struct arithmetic_parser *p,
	long *r)
{
	long v;

	/* Handles a failed parse unary operation. */
	if (!parse_unary(p, r))
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the accept condition. */
		if (accept(p, "*")) {
			/* Handles a failed parse unary operation. */
			if (!parse_unary(p, &v))
				return 0;
			*r *= v;
		} else if (accept(p, "/")) {
			/* Handles a failed parse unary operation. */
			if (!parse_unary(p, &v))
				return 0;

			/* Handles the v condition. */
			if (v == 0) {
				p->error = "division by zero";

				/* Reports successful completion. */
				return 0;
			}
			*r /= v;
		} else if (accept(p, "%")) {
			/* Handles a failed parse unary operation. */
			if (!parse_unary(p, &v))
				return 0;

			/* Handles the v condition. */
			if (v == 0) {
				p->error = "division by zero";

				/* Reports successful completion. */
				return 0;
			}
			*r %= v;
		} else {
			/* Reports operation failure. */
			return 1;
		}
	}
}

/* Supports the parse unary operation. */
static int
parse_unary(
	struct arithmetic_parser *parser,
	long *result)
{
	int function_result;

	/* Handles the accept condition. */
	if (accept(parser, "+")) {
		/* Obtains the parse unary result. */
		function_result = parse_unary(parser, result);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the accept condition. */
	if (accept(parser, "-")) {
		/* Handles a failed parse unary operation. */
		if (!parse_unary(parser, result))
			return 0;
		*result = -*result;
		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed accept operation. */
	if (accept(parser, "!")) {
		/* Handles a failed parse unary operation. */
		if (!parse_unary(parser, result))
			return 0;
		*result = !*result;
		/* Reports operation failure. */
		return 1;
	}

	/* Handles the accept condition. */
	if (accept(parser, "~")) {
		/* Handles a failed parse unary operation. */
		if (!parse_unary(parser, result))
			return 0;
		*result = ~*result;
		/* Reports operation failure. */
		return 1;
	}

	/* Obtains the parse primary result. */
	function_result = parse_primary(parser, result);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the accept operation. */
static int
accept(
	struct arithmetic_parser *parser,
	const char *operator)
{
	size_t length;

	skip_space(parser);
	length = strlen(operator);

	/* Selects the matching prefix. */
	if (strncmp(parser->cursor, operator, length) != 0)
		return 0;
	parser->cursor += length;

	/* Reports operation failure. */
	return 1;
}

/* Supports the skip space operation. */
static void
skip_space(
	struct arithmetic_parser *parser)
{
	/* Continue while the operation condition remains true. */
	while (isspace((unsigned char)*parser->cursor))
		parser->cursor++;
}

/* Supports the parse primary operation. */
static int
parse_primary(
	struct arithmetic_parser *parser,
	long *result)
{
	const char *start;
	const char *value;
	char *name;
	size_t length;
	char *end;

	skip_space(parser);

	/* Handles the accept condition. */
	if (accept(parser, "(")) {
		/* Handles a failed parse conditional operation. */
		if (!parse_conditional(parser, result))
			return 0;

		/* Handles a failed accept operation. */
		if (!accept(parser, ")")) {
			parser->error = "missing ')' in arithmetic expansion";

			/* Reports successful completion. */
			return 0;
		}

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the parser state. */
	if ((*parser->cursor >= '0' && *parser->cursor <= '9')) {
		*result = strtol(parser->cursor, &end, 0);
		/* Checks the current endpoint. */
		if (end == parser->cursor) {
			parser->error =
			    "invalid number in arithmetic expansion";

			/* Reports successful completion. */
			return 0;
		}
		parser->cursor = end;

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the parser state. */
	if ((*parser->cursor >= 'A' && *parser->cursor <= 'Z') ||
	    (*parser->cursor >= 'a' && *parser->cursor <= 'z') ||
	    *parser->cursor == '_') {
		start = parser->cursor;

		/* Continue while the operation condition remains true. */
		while ((*parser->cursor >= 'A' && *parser->cursor <= 'Z') ||
		       (*parser->cursor >= 'a' && *parser->cursor <= 'z') ||
		       (*parser->cursor >= '0' && *parser->cursor <= '9') ||
		       *parser->cursor == '_')
			parser->cursor++;
		length = (size_t)(parser->cursor - start);
		name = malloc(length + 1U);

		/* Handles the name availability. */
		if (name == NULL) {
			parser->error = "out of memory";

			/* Reports successful completion. */
			return 0;
		}
		memcpy(name, start, length);
		name[length] = '\0';
		value = parser->lookup == NULL
			    ? getenv(name)
			    : parser->lookup(parser->context, name);
		free(name);

		/* Handles the value availability. */
		if (value == NULL || *value == '\0') {
			*result = 0;
			/* Reports operation failure. */
			return 1;
		}
		*result = strtol(value, &end, 0);
		/* Checks the current endpoint. */
		if (*end != '\0') {
			parser->error = "variable is not an integer";

			/* Reports successful completion. */
			return 0;
		}

		/* Reports operation failure. */
		return 1;
	}
	parser->error = "expected arithmetic operand";

	/* Reports successful completion. */
	return 0;
}
