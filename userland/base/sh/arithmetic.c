/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shell arithmetic.
 *
 * A precedence-climbing parser that evaluates as it parses.  The operand of
 * && and || that is not needed, and the arm of ?: that is not taken, are
 * parsed with evaluation switched off, so that an assignment in them has no
 * effect and a division by zero in them is no error.  A variable's value is
 * read as an integer constant, as the value of an unset or empty variable
 * is zero; anything else is an error.
 *
 * The binary operators by level, from the loosest:
 *
 *	0 ||   1 &&   2 |   3 ^   4 &   5 == !=   6 < > <= >=
 *	7 << >>   8 + -   9 * / %
 */

#include "userland/base/sh/arithmetic.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest variable name this reads. */
#define ARITHMETIC_NAME_MAX 256

/* The highest level of binary operators; above it are the unary ones. */
#define LEVEL_MAX 9

/* The operators. */
#define OP_NONE		0	/* plain = */
#define OP_OR		1	/* || */
#define OP_AND		2	/* && */
#define OP_BIT_OR	3	/* | */
#define OP_BIT_XOR	4	/* ^ */
#define OP_BIT_AND	5	/* & */
#define OP_EQUAL	6	/* == */
#define OP_NOT_EQUAL	7	/* != */
#define OP_LESS		8	/* < */
#define OP_GREATER	9	/* > */
#define OP_LESS_EQUAL	10	/* <= */
#define OP_GREATER_EQUAL 11	/* >= */
#define OP_SHIFT_LEFT	12	/* << */
#define OP_SHIFT_RIGHT	13	/* >> */
#define OP_ADD		14	/* + */
#define OP_SUBTRACT	15	/* - */
#define OP_MULTIPLY	16	/* * */
#define OP_DIVIDE	17	/* / */
#define OP_REMAINDER	18	/* % */

/* The state of one evaluation. */
struct arithmetic {
	const char *cursor;
	const char *(*lookup)(void *, const char *);
	int (*assign)(void *, const char *, const char *);
	void *context;
	const char *error;

	/* Cleared while parsing an operand whose value is not used. */
	int evaluate;
};

/* An operand: its value, and the variable it names when it is one. */
struct operand {
	long long value;
	char name[ARITHMETIC_NAME_MAX];
};

/* An operator as written, and what it does. */
struct operator_text {
	const char *text;
	int op;
};

/* The assignment operators, longest first; = is a plain assignment. */
static const struct operator_text assignment_operators[] = {
	{ "<<=", OP_SHIFT_LEFT },
	{ ">>=", OP_SHIFT_RIGHT },
	{ "*=", OP_MULTIPLY },
	{ "/=", OP_DIVIDE },
	{ "%=", OP_REMAINDER },
	{ "+=", OP_ADD },
	{ "-=", OP_SUBTRACT },
	{ "&=", OP_BIT_AND },
	{ "^=", OP_BIT_XOR },
	{ "|=", OP_BIT_OR },
	{ "=", OP_NONE },
	{ NULL, 0 }
};

static int parse_assignment(struct arithmetic *state, struct operand *result);
static int assignment_operator(const char *cursor, int *op, size_t *length);
static int assign_operand(struct arithmetic *state, struct operand *result, int op);
static int parse_conditional(struct arithmetic *state, struct operand *result);
static int parse_binary(struct arithmetic *state, int level, struct operand *result);
static int binary_operator(const char *c, int level, int *op, size_t *length);
static int logical_operator(const char *c, int level, int *op, size_t *length);
static int bitwise_operator(const char *c, int level, int *op);
static int comparison_operator(const char *c, int level, int *op, size_t *length);
static int arithmetic_operator(const char *c, int level, int *op);
static int apply(struct arithmetic *state, int op, long long left, long long right, long long *result);
static int apply_division(struct arithmetic *state, int op, long long left, long long right, long long *result);
static int parse_unary(struct arithmetic *state, struct operand *result);
static int parse_primary(struct arithmetic *state, struct operand *result);
static int parse_parenthesized(struct arithmetic *state, struct operand *result);
static int parse_number(struct arithmetic *state, struct operand *result);
static int parse_variable(struct arithmetic *state, struct operand *result);
static int variable_value(struct arithmetic *state, const char *value, long long *number);
static int read_constant(const char *text, long long *value, const char **end);
static int digit_value(char value);
static int is_name_start(char value);
static int is_name_char(char value);
static int is_space(char value);
static void skip_space(struct arithmetic *state);
static int store(struct arithmetic *state, const char *name, long long value);

/*
 * Evaluates an expression.
 */
int
sh_arithmetic_eval(
	const char *text,
	const char *(*lookup)(void *, const char *),
	int (*assign)(void *, const char *, const char *),
	void *context,
	long long *result,
	const char **error_text)
{
	struct arithmetic state;
	struct operand value;
	int parsed;

	/* The state, at the first non-blank. */
	state.cursor = text;
	state.lookup = lookup;
	state.assign = assign;
	state.context = context;
	state.error = NULL;
	state.evaluate = 1;
	skip_space(&state);

	/* An expression that is empty is an error, as in other shells. */
	if (*state.cursor == '\0') {
		*error_text = "arithmetic expression: expecting primary";
		return 0;
	}

	/* The expression. */
	parsed = parse_assignment(&state, &value);
	if (!parsed && state.error == NULL)
		state.error = "arithmetic expression: syntax error";
	if (!parsed) {
		*error_text = state.error;
		return 0;
	}

	/* Nothing may follow it. */
	skip_space(&state);
	if (*state.cursor != '\0') {
		*error_text = "arithmetic expression: unexpected text";
		return 0;
	}

	/* Succeeded. */
	*result = value.value;
	*error_text = NULL;
	return 1;
}

/* Parses an assignment, or a conditional expression when there is none. */
static int
parse_assignment(
	struct arithmetic *state,
	struct operand *result)
{
	size_t length;
	int found;
	int parsed;
	int op;

	/* The left side, which may be all there is. */
	parsed = parse_conditional(state, result);
	if (!parsed)
		return 0;
	skip_space(state);

	/* An assignment operator. */
	found = assignment_operator(state->cursor, &op, &length);
	if (!found)
		return 1;

	/* It needs a variable on its left. */
	if (result->name[0] == '\0') {
		state->error = "arithmetic expression: assignment to a "
		    "non-variable";
		return 0;
	}

	/* The name is the variable assigned to. */
	state->cursor += length;

	/* Succeeded: the assignment. */
	return assign_operand(state, result, op);
}

/*
 * Recognizes an assignment operator at the cursor; == is a comparison,
 * never an assignment.
 */
static int
assignment_operator(
	const char *cursor,
	int *op,
	size_t *length)
{
	int compare;
	int index;

	/* Each operator, longest first. */
	for (index = 0; assignment_operators[index].text != NULL; index++) {
		*length = strlen(assignment_operators[index].text);
		compare = strncmp(cursor, assignment_operators[index].text,
				  *length);
		if (compare != 0)
			continue;
		*op = assignment_operators[index].op;
		if (*op == OP_NONE && cursor[1] == '=')
			return 0;
		return 1;
	}

	/* None. */
	return 0;
}

/*
 * Parses the right side of an assignment (itself an assignment), and
 * assigns to the variable on the left; op combines the two first.
 */
static int
assign_operand(
	struct arithmetic *state,
	struct operand *result,
	int op)
{
	struct operand right;
	long long value;
	int ok;

	/* The right side. */
	ok = parse_assignment(state, &right);
	if (!ok)
		return 0;

	/* Combined with the left for an operator such as +=. */
	value = right.value;
	if (op != OP_NONE) {
		ok = apply(state, op, result->value, right.value, &value);
		if (!ok)
			return 0;
	}

	/* Stored, unless this part is not evaluated. */
	if (state->evaluate) {
		ok = store(state, result->name, value);
		if (!ok)
			return 0;
	}

	/* Succeeded: the value assigned, which is no longer a variable. */
	result->value = value;
	result->name[0] = '\0';
	return 1;
}

/* Parses cond ? a : b. */
static int
parse_conditional(
	struct arithmetic *state,
	struct operand *result)
{
	struct operand yes;
	struct operand no;
	int evaluate;
	int ok;

	/* The condition, which may be all there is. */
	ok = parse_binary(state, 0, result);
	if (!ok)
		return 0;
	skip_space(state);
	if (*state->cursor != '?')
		return 1;
	state->cursor++;
	evaluate = state->evaluate;

	/* The first arm, evaluated only when the condition is true. */
	state->evaluate = evaluate && result->value != 0;
	ok = parse_assignment(state, &yes);
	if (!ok)
		return 0;

	/* The :. */
	skip_space(state);
	if (*state->cursor != ':') {
		state->error = "arithmetic expression: expecting ':'";
		return 0;
	}

	/* The : between the two choices. */
	state->cursor++;

	/* The second arm, evaluated only when it is false. */
	state->evaluate = evaluate && result->value == 0;
	ok = parse_conditional(state, &no);
	if (!ok)
		return 0;
	state->evaluate = evaluate;

	/* Succeeded: the value of the arm taken. */
	if (result->value != 0)
		result->value = yes.value;
	else
		result->value = no.value;
	result->name[0] = '\0';
	return 1;
}

/* Parses the binary operators from a precedence level upwards. */
static int
parse_binary(
	struct arithmetic *state,
	int level,
	struct operand *result)
{
	struct operand right;
	size_t length;
	int evaluate;
	int found;
	int ok;
	int op;

	/* Above the binary levels are the unary operators. */
	if (level > LEVEL_MAX)
		return parse_unary(state, result);

	/* The first operand, of the next level up. */
	ok = parse_binary(state, level + 1, result);
	if (!ok)
		return 0;

	/* Each operator of this level and the operand after it, left to right. */
	for (;;) {
		/* An operator of this level, or the end of it. */
		skip_space(state);
		found = binary_operator(state->cursor, level, &op, &length);
		if (!found)
			return 1;
		state->cursor += length;
		evaluate = state->evaluate;

		/* The right of && and || is not evaluated when not needed. */
		if (op == OP_AND)
			state->evaluate = evaluate && result->value != 0;
		else if (op == OP_OR)
			state->evaluate = evaluate && result->value == 0;

		/* The next operand. */
		ok = parse_binary(state, level + 1, &right);
		if (!ok)
			return 0;
		state->evaluate = evaluate;

		/* Combined. */
		ok = apply(state, op, result->value, right.value,
			   &result->value);
		if (!ok)
			return 0;
		result->name[0] = '\0';
	}
}

/*
 * Recognizes the operator of one level at c.  An operator that is the start
 * of an assignment operator, or of a longer operator, is not one.
 */
static int
binary_operator(
	const char *c,
	int level,
	int *op,
	size_t *length)
{
	/* Dispatches on the level. */
	*length = 1;
	switch (level) {
	case 0:
	case 1:
		return logical_operator(c, level, op, length);
	case 2:
	case 3:
	case 4:
		return bitwise_operator(c, level, op);
	case 5:
	case 6:
	case 7:
		return comparison_operator(c, level, op, length);
	default:
		break;
	}

	/* The additive and multiplicative levels. */
	return arithmetic_operator(c, level, op);
}

/* Recognizes || (level 0) or && (level 1). */
static int
logical_operator(
	const char *c,
	int level,
	int *op,
	size_t *length)
{
	/* || */
	if (level == 0 && c[0] == '|' && c[1] == '|') {
		*op = OP_OR;
		*length = 2;
		return 1;
	}

	/* && */
	if (level == 1 && c[0] == '&' && c[1] == '&') {
		*op = OP_AND;
		*length = 2;
		return 1;
	}

	/* Neither. */
	return 0;
}

/* Recognizes | (level 2), ^ (level 3) or & (level 4). */
static int
bitwise_operator(
	const char *c,
	int level,
	int *op)
{
	/* Not at the end, nor before = (an assignment). */
	if (c[0] == '\0' || c[1] == '=')
		return 0;

	/* |, not ||. */
	if (level == 2 && c[0] == '|' && c[1] != '|') {
		*op = OP_BIT_OR;
		return 1;
	}

	/* ^. */
	if (level == 3 && c[0] == '^') {
		*op = OP_BIT_XOR;
		return 1;
	}

	/* &, not &&. */
	if (level == 4 && c[0] == '&' && c[1] != '&') {
		*op = OP_BIT_AND;
		return 1;
	}

	/* None of them. */
	return 0;
}

/*
 * Recognizes == != (level 5), < > <= >= (level 6), or << >> (level 7, not
 * before =).
 */
static int
comparison_operator(
	const char *c,
	int level,
	int *op,
	size_t *length)
{
	/* The operator. */
	*op = OP_NONE;
	if (level == 5 && c[0] == '=' && c[1] == '=')
		*op = OP_EQUAL;
	else if (level == 5 && c[0] == '!' && c[1] == '=')
		*op = OP_NOT_EQUAL;
	else if (level == 6 && c[0] == '<' && c[1] == '=')
		*op = OP_LESS_EQUAL;
	else if (level == 6 && c[0] == '>' && c[1] == '=')
		*op = OP_GREATER_EQUAL;
	else if (level == 7 && c[0] == '<' && c[1] == '<' && c[2] != '=')
		*op = OP_SHIFT_LEFT;
	else if (level == 7 && c[0] == '>' && c[1] == '>' && c[2] != '=')
		*op = OP_SHIFT_RIGHT;

	/* The two-character ones. */
	if (*op != OP_NONE) {
		*length = 2;
		return 1;
	}

	/* < and >, which are not << and >>. */
	if (level == 6 && c[0] == '<' && c[1] != '<') {
		*op = OP_LESS;
		return 1;
	}

	/* > likewise. */
	if (level == 6 && c[0] == '>' && c[1] != '>') {
		*op = OP_GREATER;
		return 1;
	}

	/* None of them. */
	return 0;
}

/* Recognizes + - (level 8) or * / % (level 9), not before =. */
static int
arithmetic_operator(
	const char *c,
	int level,
	int *op)
{
	/* Not at the end, nor before = (an assignment). */
	if (c[0] == '\0' || c[1] == '=')
		return 0;

	/* The operator of the level. */
	*op = OP_NONE;
	if (level == 8 && c[0] == '+')
		*op = OP_ADD;
	else if (level == 8 && c[0] == '-')
		*op = OP_SUBTRACT;
	else if (level == 9 && c[0] == '*')
		*op = OP_MULTIPLY;
	else if (level == 9 && c[0] == '/')
		*op = OP_DIVIDE;
	else if (level == 9 && c[0] == '%')
		*op = OP_REMAINDER;

	/* Succeeded: whether there was one. */
	if (*op == OP_NONE)
		return 0;
	return 1;
}

/*
 * Applies a binary operator.  Addition, subtraction, multiplication and
 * shifts wrap as the unsigned values do.
 */
static int
apply(
	struct arithmetic *state,
	int op,
	long long left,
	long long right,
	long long *result)
{
	unsigned long long bits;

	/* Dispatches on the operator. */
	switch (op) {
	case OP_OR:
		*result = left != 0 || right != 0;
		return 1;
	case OP_AND:
		*result = left != 0 && right != 0;
		return 1;
	case OP_BIT_OR:
		*result = left | right;
		return 1;
	case OP_BIT_XOR:
		*result = left ^ right;
		return 1;
	case OP_BIT_AND:
		*result = left & right;
		return 1;
	case OP_EQUAL:
		*result = left == right;
		return 1;
	case OP_NOT_EQUAL:
		*result = left != right;
		return 1;
	case OP_LESS:
		*result = left < right;
		return 1;
	case OP_GREATER:
		*result = left > right;
		return 1;
	case OP_LESS_EQUAL:
		*result = left <= right;
		return 1;
	case OP_GREATER_EQUAL:
		*result = left >= right;
		return 1;
	case OP_SHIFT_LEFT:
		bits = (unsigned long long)left << (right & 63);
		*result = (long long)bits;
		return 1;
	case OP_SHIFT_RIGHT:
		*result = left >> (right & 63);
		return 1;
	case OP_ADD:
		bits = (unsigned long long)left + (unsigned long long)right;
		*result = (long long)bits;
		return 1;
	case OP_SUBTRACT:
		bits = (unsigned long long)left - (unsigned long long)right;
		*result = (long long)bits;
		return 1;
	case OP_MULTIPLY:
		bits = (unsigned long long)left * (unsigned long long)right;
		*result = (long long)bits;
		return 1;
	default:
		break;
	}

	/* Succeeded: division and remainder. */
	return apply_division(state, op, left, right, result);
}

/*
 * Divides, or takes the remainder.  Division by zero is an error when
 * evaluated (and 0 when not); the one overflowing division wraps.
 */
static int
apply_division(
	struct arithmetic *state,
	int op,
	long long left,
	long long right,
	long long *result)
{
	/* By zero. */
	if (right == 0 && !state->evaluate) {
		*result = 0;
		return 1;
	}

	/* Division by zero is an error where it is evaluated. */
	if (right == 0) {
		state->error = "arithmetic expression: division by zero";
		return 0;
	}

	/* The smallest number by -1, which does not fit. */
	if (left == LLONG_MIN && right == -1) {
		if (op == OP_DIVIDE)
			*result = LLONG_MIN;
		else
			*result = 0;
		return 1;
	}

	/* Succeeded: the quotient or the remainder. */
	if (op == OP_DIVIDE)
		*result = left / right;
	else
		*result = left % right;
	return 1;
}

/* Parses the unary operators: + - ~ !. */
static int
parse_unary(
	struct arithmetic *state,
	struct operand *result)
{
	unsigned long long bits;
	char op;
	int ok;

	/* A primary, when there is no unary operator. */
	skip_space(state);
	op = *state->cursor;
	if (op != '+' && op != '-' && op != '~' && op != '!')
		return parse_primary(state, result);

	/* ++ and -- are not operators of the shell. */
	if ((op == '+' || op == '-') && state->cursor[1] == op) {
		state->error = "arithmetic expression: expecting primary";
		return 0;
	}

	/* The operand. */
	state->cursor++;
	ok = parse_unary(state, result);
	if (!ok)
		return 0;

	/* The operator applied. */
	if (op == '-') {
		bits = 0ULL - (unsigned long long)result->value;
		result->value = (long long)bits;
	} else if (op == '~') {
		result->value = ~result->value;
	} else if (op == '!') {
		result->value = result->value == 0;
	}

	/* Succeeded: a value, which is no longer a variable. */
	result->name[0] = '\0';
	return 1;
}

/* Parses a constant, a variable or a parenthesized expression. */
static int
parse_primary(
	struct arithmetic *state,
	struct operand *result)
{
	int digit;
	int name;

	/* A parenthesized expression. */
	skip_space(state);
	result->name[0] = '\0';
	if (*state->cursor == '(')
		return parse_parenthesized(state, result);

	/* A constant. */
	digit = digit_value(*state->cursor);
	if (digit >= 0 && digit <= 9)
		return parse_number(state, result);

	/* A variable. */
	name = is_name_start(*state->cursor);
	if (name)
		return parse_variable(state, result);

	/* Anything else. */
	state->error = "arithmetic expression: expecting primary";
	return 0;
}

/* Parses ( expression ). */
static int
parse_parenthesized(
	struct arithmetic *state,
	struct operand *result)
{
	int ok;

	/* The expression inside. */
	state->cursor++;
	ok = parse_assignment(state, result);
	if (!ok)
		return 0;

	/* The ). */
	skip_space(state);
	if (*state->cursor != ')') {
		state->error = "arithmetic expression: expecting ')'";
		return 0;
	}

	/* The closing parenthesis. */
	state->cursor++;

	/* Succeeded: a value, not a variable. */
	result->name[0] = '\0';
	return 1;
}

/* Parses an integer constant, which no name character may follow. */
static int
parse_number(
	struct arithmetic *state,
	struct operand *result)
{
	const char *end;
	int ok;
	int name;

	/* The constant, all of it. */
	ok = read_constant(state->cursor, &result->value, &end);
	name = is_name_char(*end);
	if (!ok || name) {
		state->error = "arithmetic expression: bad number";
		return 0;
	}

	/* Succeeded. */
	state->cursor = end;
	return 1;
}

/*
 * Parses a variable: its name is kept (for an assignment), and its value,
 * when evaluated, read as a constant.
 */
static int
parse_variable(
	struct arithmetic *state,
	struct operand *result)
{
	const char *start;
	const char *value;
	size_t length;
	int name;

	/* The name. */
	start = state->cursor;
	for (;;) {
		name = is_name_char(*state->cursor);
		if (!name)
			break;
		state->cursor++;
	}

	/* The name must fit the buffer. */
	length = (size_t)(state->cursor - start);
	if (length >= sizeof(result->name)) {
		state->error = "arithmetic expression: name too long";
		return 0;
	}

	/* The name, with its terminator. */
	memcpy(result->name, start, length);
	result->name[length] = '\0';
	result->value = 0;

	/* The value is not read where it is not used. */
	if (!state->evaluate)
		return 1;

	/* The value: from the shell, or from the environment. */
	if (state->lookup != NULL)
		value = state->lookup(state->context, result->name);
	else
		value = getenv(result->name);

	/* An unset variable is zero. */
	if (value == NULL)
		return 1;

	/* Succeeded: otherwise, a constant. */
	return variable_value(state, value, &result->value);
}

/*
 * Reads a variable's value as a constant, with blanks around it.  An empty
 * or blank value is zero; anything else is an error.
 */
static int
variable_value(
	struct arithmetic *state,
	const char *value,
	long long *number)
{
	const char *end;
	int space;
	int ok;

	/* Leading blanks. */
	for (;;) {
		space = is_space(*value);
		if (!space)
			break;
		value++;
	}

	/* A variable that is empty or blank is zero. */
	if (*value == '\0')
		return 1;

	/* The constant. */
	ok = read_constant(value, number, &end);
	if (!ok) {
		state->error = "arithmetic expression: illegal number";
		return 0;
	}

	/* Only blanks after it. */
	for (;;) {
		space = is_space(*end);
		if (!space)
			break;
		end++;
	}

	/* Anything else after the number is an error. */
	if (*end != '\0') {
		state->error = "arithmetic expression: illegal number";
		return 0;
	}

	/* Succeeded. */
	return 1;
}

/*
 * Reads an integer constant: decimal, octal with a leading 0, hexadecimal
 * with 0x, optionally signed.  Out-of-range values wrap as other shells do.
 */
static int
read_constant(
	const char *text,
	long long *value,
	const char **end)
{
	unsigned long long number;
	const char *start;
	int negative;
	int base;
	int digit;

	/* A sign. */
	negative = 0;
	if (*text == '-')
		negative = 1;
	if (*text == '-' || *text == '+')
		text++;

	/* 0x and a hexadecimal digit, or 0: the base. */
	base = 10;
	if (text[0] == '0')
		base = 8;
	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		digit = digit_value(text[2]);
		if (digit >= 0 && digit < 16) {
			base = 16;
			text += 2;
		}
	}

	/* The digits of the base. */
	start = text;
	number = 0;
	for (;;) {
		digit = digit_value(*text);
		if (digit < 0 || digit >= base)
			break;
		number = number * (unsigned long long)base +
		    (unsigned long long)digit;
		text++;
	}

	/* No digit at all is no number. */
	if (text == start)
		return 0;

	/* Succeeded: the value, negated as unsigned when it had a -. */
	if (negative)
		number = 0ULL - number;
	*value = (long long)number;
	*end = text;
	return 1;
}

/* Returns a character's value as a digit of base up to 36, or -1. */
static int
digit_value(
	char value)
{
	/* 0-9. */
	if (value >= '0' && value <= '9')
		return value - '0';

	/* a-z and A-Z are 10-35. */
	if (value >= 'a' && value <= 'z')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'Z')
		return value - 'A' + 10;

	/* Not a digit. */
	return -1;
}

/* Reports whether a character can start a name: a letter or _. */
static int
is_name_start(
	char value)
{
	/* A letter. */
	if (value >= 'a' && value <= 'z')
		return 1;
	if (value >= 'A' && value <= 'Z')
		return 1;

	/* Or _. */
	if (value == '_')
		return 1;
	return 0;
}

/* Reports whether a character can be in a name: a letter, digit or _. */
static int
is_name_char(
	char value)
{
	int start;

	/* A digit. */
	if (value >= '0' && value <= '9')
		return 1;

	/* Or what can start a name. */
	start = is_name_start(value);
	return start;
}

/* Reports whether a character is a blank or a newline. */
static int
is_space(
	char value)
{
	/* Space, tab, newline. */
	if (value == ' ' || value == '\t' || value == '\n')
		return 1;
	return 0;
}

/* Skips blanks and newlines. */
static void
skip_space(
	struct arithmetic *state)
{
	int space;

	/* Each one. */
	for (;;) {
		space = is_space(*state->cursor);
		if (!space)
			break;
		state->cursor++;
	}
}

/* Sets a variable to a number. */
static int
store(
	struct arithmetic *state,
	const char *name,
	long long value)
{
	char text[32];
	int assigned;

	/* The number in decimal. */
	(void)snprintf(text, sizeof(text), "%lld", value);

	/* Assigned by the shell. */
	assigned = 0;
	if (state->assign != NULL)
		assigned = state->assign(state->context, name, text);
	if (!assigned) {
		state->error = "arithmetic expression: cannot assign";
		return 0;
	}

	/* Succeeded. */
	return 1;
}
