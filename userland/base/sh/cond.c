/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bash conditional command [[ ... ]] and the arithmetic command
 * (( ... )), which XCU 2.4 and 2.6.3 leave unspecified and bash gives
 * these meanings.
 *
 * In [[ ... ]] the words are expanded without field splitting or
 * pathname expansion.  The right of == and != is a pattern (its quoted
 * characters stand for themselves); the right of =~ is an extended
 * regular expression (its quoted characters stand for themselves); the
 * integer comparisons evaluate their operands as arithmetic expressions.
 * The status is 0 when the expression is true, 1 when it is false, and 2
 * when it cannot be evaluated.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/expand.h"
#include "userland/base/sh/glob.h"
#include "userland/base/sh/vars.h"

#include <regex.h>
#include <stdlib.h>
#include <string.h>

/*
 * The integer comparisons of [[ ... ]], in the order of the values
 * integer_compare_names gives them.
 */
enum integer_compare {
	INTEGER_EQ,
	INTEGER_NE,
	INTEGER_LT,
	INTEGER_LE,
	INTEGER_GT,
	INTEGER_GE
};

/*
 * The operator words of the integer comparisons, indexed by enum
 * integer_compare and ended by NULL.  The parser accepts no other
 * operator that reaches the integer comparison.
 */
static const char *const integer_compare_names[] = {
	"-eq", "-ne", "-lt", "-le", "-gt", "-ge", NULL
};

static int eval_node(const struct sh_cond *cond);
static int eval_not(const struct sh_cond *cond);
static int eval_and(const struct sh_cond *cond);
static int eval_or(const struct sh_cond *cond);
static int eval_word(const struct sh_cond *cond);
static int eval_unary(const struct sh_cond *cond);
static int eval_binary(const struct sh_cond *cond);
static int eval_string_order(const char *op, const char *left, const char *right);
static int eval_integer_compare(const char *op, const char *left, const char *right);
static char *expand_operand(const struct sh_token *word);
static int match_pattern(const struct sh_token *word, const char *subject);
static int match_regex(const struct sh_token *word, const char *subject);
static int integer_operand(const char *text, long *value);

/*
 * Evaluates [[ ... ]]: 0 when the expression is true, 1 when false, and
 * 2 when it cannot be evaluated.
 */
int
sh_eval_cond(
	const struct sh_cond *cond)
{
	size_t mark;
	int truth;

	/* Evaluates the expression; its expansions are freed when it ends. */
	mark = sh_temp_mark();
	truth = eval_node(cond);
	sh_temp_release(mark);

	/* An expression that could not be evaluated is status 2. */
	if (truth < 0)
		return 2;

	/* A false expression is status 1. */
	if (truth == 0)
		return 1;

	/* Succeeded: the expression is true. */
	return 0;
}

/*
 * Evaluates the text of an arithmetic command, expanded as $(( )) is.
 *
 * Returns 1 with the value, or 0 after reporting the error.
 */
int
sh_eval_arith_text(
	const char *text,
	long *value)
{
	struct sh_expand_context context;
	const char *error_text;
	int ok;

	/* Expands and evaluates the text; an error is reported, not fatal. */
	sh_expand_context_fill(&context);
	ok = sh_expand_arithmetic(text, &context, value, &error_text);
	if (!ok) {
		sh_warn("%s", error_text);
		return 0;
	}

	/* Succeeded: the value is stored. */
	return 1;
}

/*
 * Implements let (bash).
 *
 * Each operand is evaluated as an arithmetic expression; the status is 0
 * when the last one is not 0.
 */
int
sh_builtin_let(
	int argc,
	char **argv)
{
	long value;
	int index;
	int ok;

	/* Refuses a let without an expression. */
	if (argc < 2) {
		fprintf(stderr, "let: expression expected\n");
		return 1;
	}

	/* Evaluates each expression in turn; an error stops them with status 1. */
	value = 0;
	for (index = 1; index < argc; index++) {
		ok = sh_eval_arith_text(argv[index], &value);
		if (!ok)
			return 1;
	}

	/* A last value of 0 is status 1. */
	if (value == 0)
		return 1;

	/* Succeeded: the last value was not 0. */
	return 0;
}

/* Evaluates a part of the expression: 1, 0, or -1 on an error. */
static int
eval_node(
	const struct sh_cond *cond)
{
	int truth;

	/* Evaluates the part by its kind. */
	switch (cond->kind) {
	case SH_COND_NOT:
		truth = eval_not(cond);
		break;
	case SH_COND_AND:
		truth = eval_and(cond);
		break;
	case SH_COND_OR:
		truth = eval_or(cond);
		break;
	case SH_COND_UNARY:
		truth = eval_unary(cond);
		break;
	case SH_COND_BINARY:
		truth = eval_binary(cond);
		break;
	default:
		truth = eval_word(cond);
		break;
	}

	/* Reports the part's truth, or -1 when it could not be evaluated. */
	return truth;
}

/* Evaluates ! expression: 1, 0, or -1. */
static int
eval_not(
	const struct sh_cond *cond)
{
	int truth;

	/* Evaluates the operand; an error passes through. */
	truth = eval_node(cond->first);
	if (truth < 0)
		return truth;

	/* A true operand makes the negation false. */
	if (truth != 0)
		return 0;

	/* Succeeded: the operand was false, so the negation is true. */
	return 1;
}

/* Evaluates expression && expression: 1, 0, or -1. */
static int
eval_and(
	const struct sh_cond *cond)
{
	int truth;

	/* A false or failed left side decides without the right side. */
	truth = eval_node(cond->first);
	if (truth <= 0)
		return truth;

	/* The left side was true, so the right side decides. */
	truth = eval_node(cond->second);

	/* Succeeded: the right side's truth, or -1. */
	return truth;
}

/* Evaluates expression || expression: 1, 0, or -1. */
static int
eval_or(
	const struct sh_cond *cond)
{
	int truth;

	/* A true or failed left side decides without the right side. */
	truth = eval_node(cond->first);
	if (truth != 0)
		return truth;

	/* The left side was false, so the right side decides. */
	truth = eval_node(cond->second);

	/* Succeeded: the right side's truth, or -1. */
	return truth;
}

/* Evaluates a word alone, which is true when it is not empty. */
static int
eval_word(
	const struct sh_cond *cond)
{
	char *text;

	/* Expands the word as one word. */
	text = expand_operand(cond->left);

	/* An empty word is false. */
	if (text[0] == '\0')
		return 0;

	/* Succeeded: the word is not empty. */
	return 1;
}

/* Evaluates a unary test: 1, 0, or -1. */
static int
eval_unary(
	const struct sh_cond *cond)
{
	const char *value;
	char *operand;
	int truth;

	/* Expands the operand as one word. */
	operand = expand_operand(cond->left);

	/* -o is true when the named option is on; an unknown name is false. */
	if (cond->op[1] == 'o') {
		truth = sh_option_named(operand);
		if (truth > 0)
			return 1;
		return 0;
	}

	/* -v is true when the named variable is set. */
	if (cond->op[1] == 'v') {
		value = sh_var_get(operand);
		if (value != NULL)
			return 1;
		return 0;
	}

	/* Runs the file and string tests that test knows. */
	truth = sh_test_unary(cond->op, operand);
	if (truth < 0) {
		sh_warn("[[: %s: unary operator not supported", cond->op);
		return -1;
	}

	/* Succeeded: the test's truth. */
	return truth;
}

/* Evaluates a binary test: 1, 0, or -1. */
static int
eval_binary(
	const struct sh_cond *cond)
{
	const char *op;
	char *left;
	char *right;
	int is_equal;
	int is_assign;
	int is_not_equal;
	int is_regex;
	int truth;

	/* Expands the left operand as one word and names the operator. */
	op = cond->op;
	left = expand_operand(cond->left);
	is_equal = strcmp(op, "==");
	is_assign = strcmp(op, "=");
	is_not_equal = strcmp(op, "!=");
	is_regex = strcmp(op, "=~");

	/* == and = are true when the left side matches the pattern on the right. */
	if (is_equal == 0 || is_assign == 0) {
		truth = match_pattern(cond->right, left);
		return truth;
	}

	/* != is true when the left side does not match the pattern. */
	if (is_not_equal == 0) {
		truth = match_pattern(cond->right, left);
		if (truth != 0)
			return 0;
		return 1;
	}

	/* =~ matches the left side against a regular expression. */
	if (is_regex == 0) {
		truth = match_regex(cond->right, left);
		return truth;
	}

	/* < and > order the two strings as strcmp does. */
	right = expand_operand(cond->right);
	truth = eval_string_order(op, left, right);
	if (truth >= 0)
		return truth;

	/* -nt, -ot and -ef compare two files. */
	truth = sh_test_file_compare(op, left, right);
	if (truth >= 0)
		return truth;

	/* What remains are the integer comparisons. */
	truth = eval_integer_compare(op, left, right);

	/* Succeeded: the comparison's truth, or -1. */
	return truth;
}

/*
 * Evaluates < or >, which order two strings as strcmp does: 1, 0, or -1
 * when the operator is neither.
 */
static int
eval_string_order(
	const char *op,
	const char *left,
	const char *right)
{
	int order;

	/* Refuses any operator other than a lone < or >. */
	if (op[0] != '<' && op[0] != '>')
		return -1;
	if (op[1] != '\0')
		return -1;

	/* Orders the two strings. */
	order = strcmp(left, right);

	/* < is true when the left string sorts first. */
	if (op[0] == '<') {
		if (order < 0)
			return 1;
		return 0;
	}

	/* > is true when the left string sorts last. */
	if (order > 0)
		return 1;

	/* Succeeded: the left string does not sort last. */
	return 0;
}

/*
 * Evaluates -eq, -ne, -lt, -le, -gt or -ge, whose operands are arithmetic
 * expressions: 1, 0, or -1 when an operand cannot be evaluated.
 */
static int
eval_integer_compare(
	const char *op,
	const char *left,
	const char *right)
{
	long left_number;
	long right_number;
	int compare;
	int which;
	int holds;
	int ok;

	/* Evaluates both operands, the left one first. */
	ok = integer_operand(left, &left_number);
	if (!ok)
		return -1;
	ok = integer_operand(right, &right_number);
	if (!ok)
		return -1;

	/* Finds which comparison the operator names; the last one is -ge. */
	for (which = INTEGER_EQ; which < INTEGER_GE; which++) {
		compare = strcmp(op, integer_compare_names[which]);
		if (compare == 0)
			break;
	}

	/* Compares the two values. */
	holds = 0;
	switch (which) {
	case INTEGER_EQ:
		if (left_number == right_number)
			holds = 1;
		break;
	case INTEGER_NE:
		if (left_number != right_number)
			holds = 1;
		break;
	case INTEGER_LT:
		if (left_number < right_number)
			holds = 1;
		break;
	case INTEGER_LE:
		if (left_number <= right_number)
			holds = 1;
		break;
	case INTEGER_GT:
		if (left_number > right_number)
			holds = 1;
		break;
	default:
		if (left_number >= right_number)
			holds = 1;
		break;
	}

	/* Succeeded: whether the comparison holds. */
	return holds;
}

/* Expands a word without splitting or pathname expansion. */
static char *
expand_operand(
	const struct sh_token *word)
{
	struct sh_expand_context context;
	const char *error_text;
	char *text;
	int ok;

	/* Expands one word; an error stops the command as any expansion's would. */
	sh_expand_context_fill(&context);
	ok = sh_expand_word(word, &context, &text, &error_text);
	if (!ok)
		sh_error("%s", error_text);

	/* Succeeded: the word, freed with the command. */
	text = sh_temp_own(text);
	return text;
}

/* Matches a subject against a word read as a pattern: 1 or 0. */
static int
match_pattern(
	const struct sh_token *word,
	const char *subject)
{
	struct sh_expand_context context;
	const char *error_text;
	unsigned char *quoted;
	char *pattern;
	int ok;
	int matched;

	/* Expands the pattern, with its quoted characters marked. */
	sh_expand_context_fill(&context);
	ok = sh_expand_pattern(word, &context, &pattern, &quoted, &error_text);
	if (!ok)
		sh_error("%s", error_text);

	/* Matches the subject and frees the pattern. */
	matched = sh_glob_match(pattern, quoted, subject);
	free(pattern);
	free(quoted);

	/* A subject that does not match is false. */
	if (matched == 0)
		return 0;

	/* Succeeded: the subject matches. */
	return 1;
}

/*
 * Matches a subject against a word read as an extended regular
 * expression, whose quoted characters stand for themselves: 1, 0, or -1
 * when the expression is not valid.
 */
static int
match_regex(
	const struct sh_token *word,
	const char *subject)
{
	struct sh_expand_context context;
	const char *error_text;
	const char *special;
	unsigned char *quoted;
	char *pattern;
	char *expression;
	regex_t regex;
	size_t length;
	size_t index;
	size_t out;
	int ok;
	int matched;

	/* Expands the expression, with its quoted characters marked. */
	sh_expand_context_fill(&context);
	ok = sh_expand_pattern(word, &context, &pattern, &quoted, &error_text);
	if (!ok)
		sh_error("%s", error_text);

	/* Gives each quoted character that is special in an ERE a backslash. */
	length = strlen(pattern);
	expression = sh_malloc(length * 2U + 1U);
	out = 0;
	for (index = 0; index < length; index++) {
		special = NULL;
		if (quoted != NULL && quoted[index])
			special = strchr("\\.[]()*+?{}|^$", pattern[index]);
		if (special != NULL)
			expression[out++] = '\\';
		expression[out++] = pattern[index];
	}

	/* Ends the expression and frees the expanded pattern. */
	expression[out] = '\0';
	free(pattern);
	free(quoted);

	/* Compiles the expression; an invalid one is status 2, as in bash. */
	ok = regcomp(&regex, expression, REG_EXTENDED | REG_NOSUB);
	free(expression);
	if (ok != 0)
		return -1;

	/* Matches the subject and frees the expression. */
	matched = regexec(&regex, subject, 0, NULL, 0);
	regfree(&regex);

	/* A subject that does not match is false. */
	if (matched != 0)
		return 0;

	/* Succeeded: the subject matches. */
	return 1;
}

/*
 * Evaluates an operand of an integer comparison as an arithmetic
 * expression.
 *
 * Returns 1 with the value, or 0 after reporting the error.
 */
static int
integer_operand(
	const char *text,
	long *value)
{
	int ok;

	/* An empty operand is 0, as in bash. */
	if (text[0] == '\0') {
		*value = 0;
		return 1;
	}

	/* Evaluates the operand; the error is already reported. */
	ok = sh_eval_arith_text(text, value);
	if (!ok)
		return 0;

	/* Succeeded: the value is stored. */
	return 1;
}
