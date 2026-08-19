/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static void
skip_space(struct arithmetic_parser *parser)
{
	while (isspace((unsigned char)*parser->cursor))
		parser->cursor++;
}

static int
accept(struct arithmetic_parser *parser, const char *operator)
{
	size_t length;
	skip_space(parser);
	length = strlen(operator);
	if (strncmp(parser->cursor, operator, length) != 0)
		return 0;
	parser->cursor += length;
	return 1;
}

static int parse_conditional(struct arithmetic_parser *, long *);

static int
parse_primary(struct arithmetic_parser *parser, long *result)
{
	char *end;
	skip_space(parser);
	if (accept(parser, "(")) {
		if (!parse_conditional(parser, result))
			return 0;
		if (!accept(parser, ")")) {
			parser->error = "missing ')' in arithmetic expansion";
			return 0;
		}
		return 1;
	}
	if ((*parser->cursor >= '0' && *parser->cursor <= '9')) {
		*result = strtol(parser->cursor, &end, 0);
		if (end == parser->cursor) {
			parser->error = "invalid number in arithmetic expansion";
			return 0;
		}
		parser->cursor = end;
		return 1;
	}
	if ((*parser->cursor >= 'A' && *parser->cursor <= 'Z') ||
	    (*parser->cursor >= 'a' && *parser->cursor <= 'z') ||
	    *parser->cursor == '_') {
		const char *start = parser->cursor;
		const char *value;
		char *name;
		size_t length;
		while ((*parser->cursor >= 'A' && *parser->cursor <= 'Z') ||
		    (*parser->cursor >= 'a' && *parser->cursor <= 'z') ||
		    (*parser->cursor >= '0' && *parser->cursor <= '9') ||
		    *parser->cursor == '_')
			parser->cursor++;
		length = (size_t)(parser->cursor - start);
		name = malloc(length + 1U);
		if (name == NULL) {
			parser->error = "out of memory";
			return 0;
		}
		memcpy(name, start, length);
		name[length] = '\0';
		value = parser->lookup == NULL ? getenv(name) :
		    parser->lookup(parser->context, name);
		free(name);
		if (value == NULL || *value == '\0') {
			*result = 0;
			return 1;
		}
		*result = strtol(value, &end, 0);
		if (*end != '\0') {
			parser->error = "variable is not an integer";
			return 0;
		}
		return 1;
	}
	parser->error = "expected arithmetic operand";
	return 0;
}

static int
parse_unary(struct arithmetic_parser *parser, long *result)
{
	if (accept(parser, "+"))
		return parse_unary(parser, result);
	if (accept(parser, "-")) {
		if (!parse_unary(parser, result)) return 0;
		*result = -*result;
		return 1;
	}
	if (accept(parser, "!")) {
		if (!parse_unary(parser, result)) return 0;
		*result = !*result;
		return 1;
	}
	if (accept(parser, "~")) {
		if (!parse_unary(parser, result)) return 0;
		*result = ~*result;
		return 1;
	}
	return parse_primary(parser, result);
}

#define BINARY_LEVEL(name, lower, op1, op2, body1, body2) \
static int name(struct arithmetic_parser *p, long *r) \
{ \
	long v; \
	if (!lower(p, r)) return 0; \
	for (;;) { \
		if (accept(p, op1)) { if (!lower(p, &v)) { return 0; } body1; } \
		else if ((op2)[0] != '\0' && accept(p, op2)) { \
			if (!lower(p, &v)) { return 0; } body2; \
		} else return 1; \
	} \
}

static int
parse_multiply(struct arithmetic_parser *p, long *r)
{
	long v;
	if (!parse_unary(p, r)) return 0;
	for (;;) {
		if (accept(p, "*")) {
			if (!parse_unary(p, &v)) return 0;
			*r *= v;
		} else if (accept(p, "/")) {
			if (!parse_unary(p, &v)) return 0;
			if (v == 0) { p->error = "division by zero"; return 0; }
			*r /= v;
		} else if (accept(p, "%")) {
			if (!parse_unary(p, &v)) return 0;
			if (v == 0) { p->error = "division by zero"; return 0; }
			*r %= v;
		} else return 1;
	}
}

BINARY_LEVEL(parse_add, parse_multiply, "+", "-", *r += v, *r -= v)
BINARY_LEVEL(parse_shift, parse_add, "<<", ">>", *r <<= v, *r >>= v)

static int
parse_relation(struct arithmetic_parser *p, long *r)
{
	long v;
	if (!parse_shift(p, r)) return 0;
	for (;;) {
		if (accept(p, "<=")) { if (!parse_shift(p, &v)) return 0; *r = *r <= v; }
		else if (accept(p, ">=")) { if (!parse_shift(p, &v)) return 0; *r = *r >= v; }
		else if (accept(p, "<")) { if (!parse_shift(p, &v)) return 0; *r = *r < v; }
		else if (accept(p, ">")) { if (!parse_shift(p, &v)) return 0; *r = *r > v; }
		else return 1;
	}
}

BINARY_LEVEL(parse_equal, parse_relation, "==", "!=", *r = *r == v, *r = *r != v)
static int
parse_bit_and(struct arithmetic_parser *p, long *r)
{
	long v;
	if (!parse_equal(p, r)) return 0;
	for (;;) {
		skip_space(p);
		if (p->cursor[0] != '&' || p->cursor[1] == '&') return 1;
		p->cursor++;
		if (!parse_equal(p, &v)) return 0;
		*r &= v;
	}
}
BINARY_LEVEL(parse_bit_xor, parse_bit_and, "^", "", *r ^= v, (void)v)
static int
parse_bit_or(struct arithmetic_parser *p, long *r)
{
	long v;
	if (!parse_bit_xor(p, r)) return 0;
	for (;;) {
		skip_space(p);
		if (p->cursor[0] != '|' || p->cursor[1] == '|') return 1;
		p->cursor++;
		if (!parse_bit_xor(p, &v)) return 0;
		*r |= v;
	}
}
BINARY_LEVEL(parse_logical_and, parse_bit_or, "&&", "", *r = (*r && v), (void)v)
BINARY_LEVEL(parse_logical_or, parse_logical_and, "||", "", *r = (*r || v), (void)v)

static int
parse_conditional(struct arithmetic_parser *parser, long *result)
{
	long true_value, false_value;
	if (!parse_logical_or(parser, result))
		return 0;
	if (!accept(parser, "?"))
		return 1;
	if (!parse_conditional(parser, &true_value) || !accept(parser, ":") ||
	    !parse_conditional(parser, &false_value)) {
		if (parser->error == NULL)
			parser->error = "invalid conditional arithmetic expression";
		return 0;
	}
	*result = *result ? true_value : false_value;
	return 1;
}

int
sh_arithmetic_eval(const char *text, const char *(*lookup)(void *, const char *),
    void *context, long *result, const char **error_text)
{
	struct arithmetic_parser parser;
	parser.cursor = text;
	parser.lookup = lookup;
	parser.context = context;
	parser.error = NULL;
	if (!parse_conditional(&parser, result)) {
		*error_text = parser.error == NULL ? "invalid arithmetic expression" :
		    parser.error;
		return 0;
	}
	skip_space(&parser);
	if (*parser.cursor != '\0') {
		*error_text = "trailing text in arithmetic expansion";
		return 0;
	}
	*error_text = NULL;
	return 1;
}
