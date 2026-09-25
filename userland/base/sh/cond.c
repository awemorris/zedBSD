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

static int eval_node(const struct sh_cond *cond);
static int eval_binary(const struct sh_cond *cond);
static int eval_unary(const struct sh_cond *cond);
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
	int result;

	/* The expansions' allocations go when it ends. */
	mark = sh_temp_mark();
	result = eval_node(cond);
	sh_temp_release(mark);

	/* 1 (true) is status 0; 0 (false) is 1; an error is 2. */
	if (result < 0)
		return 2;
	return result ? 0 : 1;
}

/*
 * Evaluates the text of an arithmetic command, expanded as $(( )) is.
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

	/* An empty expression is 0. */
	sh_expand_context_fill(&context);
	ok = sh_expand_arithmetic(text, &context, value, &error_text);
	if (!ok) {
		sh_warn("%s", error_text);
		return 0;
	}

	/* Succeeded. */
	return 1;
}

/*
 * Implements let (bash): evaluates each operand as an arithmetic
 * expression; the status is 0 when the last one is not 0.
 */
int
sh_builtin_let(
	int argc,
	char **argv)
{
	long value;
	int index;
	int ok;

	/* At least one expression. */
	if (argc < 2) {
		fprintf(stderr, "let: expression expected\n");
		return 1;
	}

	/* Each in turn; an error stops them with status 1. */
	value = 0;
	for (index = 1; index < argc; index++) {
		ok = sh_eval_arith_text(argv[index], &value);
		if (!ok)
			return 1;
	}

	/* Succeeded: whether the last was true. */
	return value != 0 ? 0 : 1;
}

/* Evaluates a part of the expression: 1, 0, or -1 on an error. */
static int
eval_node(
	const struct sh_cond *cond)
{
	char *text;
	int result;

	/* Dispatches on the kind of part. */
	switch (cond->kind) {
	case SH_COND_NOT:
		result = eval_node(cond->first);
		if (result < 0)
			return result;
		return !result;
	case SH_COND_AND:
		result = eval_node(cond->first);
		if (result <= 0)
			return result;
		return eval_node(cond->second);
	case SH_COND_OR:
		result = eval_node(cond->first);
		if (result != 0)
			return result;
		return eval_node(cond->second);
	case SH_COND_UNARY:
		return eval_unary(cond);
	case SH_COND_BINARY:
		return eval_binary(cond);
	default:
		break;
	}

	/* A word alone is true when it is not empty. */
	text = expand_operand(cond->left);
	return text[0] != '\0';
}

/* Evaluates a unary test: 1, 0, or -1. */
static int
eval_unary(
	const struct sh_cond *cond)
{
	char *operand;
	int result;

	/* The operand, expanded as one word. */
	operand = expand_operand(cond->left);

	/* -o: an option; -v: a variable that is set. */
	if (cond->op[1] == 'o') {
		result = sh_option_named(operand);
		return result > 0;
	}
	if (cond->op[1] == 'v')
		return sh_var_get(operand) != NULL;

	/* The tests test knows. */
	result = sh_test_unary(cond->op, operand);
	if (result < 0) {
		sh_warn("[[: %s: unary operator not supported", cond->op);
		return -1;
	}

	/* Succeeded. */
	return result;
}

/* Evaluates a binary test: 1, 0, or -1. */
static int
eval_binary(
	const struct sh_cond *cond)
{
	const char *op;
	char *left;
	char *right;
	long left_number;
	long right_number;
	int result;

	/* The left operand, expanded as one word. */
	op = cond->op;
	left = expand_operand(cond->left);

	/* == = and != match a pattern; =~ a regular expression. */
	if (strcmp(op, "==") == 0 || strcmp(op, "=") == 0)
		return match_pattern(cond->right, left);
	if (strcmp(op, "!=") == 0) {
		result = match_pattern(cond->right, left);
		if (result < 0)
			return result;
		return !result;
	}
	if (strcmp(op, "=~") == 0)
		return match_regex(cond->right, left);

	/* The string order, and the file comparisons. */
	right = expand_operand(cond->right);
	if (strcmp(op, "<") == 0)
		return strcmp(left, right) < 0;
	if (strcmp(op, ">") == 0)
		return strcmp(left, right) > 0;
	result = sh_test_file_compare(op, left, right);
	if (result >= 0)
		return result;

	/* The integer comparisons, of arithmetic expressions. */
	if (!integer_operand(left, &left_number) ||
	    !integer_operand(right, &right_number))
		return -1;
	if (strcmp(op, "-eq") == 0)
		return left_number == right_number;
	if (strcmp(op, "-ne") == 0)
		return left_number != right_number;
	if (strcmp(op, "-lt") == 0)
		return left_number < right_number;
	if (strcmp(op, "-le") == 0)
		return left_number <= right_number;
	if (strcmp(op, "-gt") == 0)
		return left_number > right_number;

	/* -ge. */
	return left_number >= right_number;
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

	/* One word; an error stops the command as any expansion's would. */
	sh_expand_context_fill(&context);
	ok = sh_expand_word(word, &context, &text, &error_text);
	if (!ok)
		sh_error("%s", error_text);

	/* Succeeded: the word, freed with the command. */
	return sh_temp_own(text);
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

	/* The pattern, with its quoted characters marked. */
	sh_expand_context_fill(&context);
	ok = sh_expand_pattern(word, &context, &pattern, &quoted, &error_text);
	if (!ok)
		sh_error("%s", error_text);
	matched = sh_glob_match(pattern, quoted, subject);
	free(pattern);
	free(quoted);

	/* Succeeded. */
	return matched != 0;
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
	unsigned char *quoted;
	char *pattern;
	char *expression;
	regex_t regex;
	size_t length;
	size_t index;
	size_t out;
	int ok;
	int matched;

	/* The expression, with its quoted characters marked. */
	sh_expand_context_fill(&context);
	ok = sh_expand_pattern(word, &context, &pattern, &quoted, &error_text);
	if (!ok)
		sh_error("%s", error_text);

	/* A quoted character that is special in an ERE gets a backslash. */
	length = strlen(pattern);
	expression = sh_malloc(length * 2U + 1U);
	out = 0;
	for (index = 0; index < length; index++) {
		if (quoted != NULL && quoted[index] &&
		    strchr("\\.[]()*+?{}|^$", pattern[index]) != NULL)
			expression[out++] = '\\';
		expression[out++] = pattern[index];
	}
	expression[out] = '\0';
	free(pattern);
	free(quoted);

	/* The expression; an invalid one is status 2, as in bash. */
	ok = regcomp(&regex, expression, REG_EXTENDED | REG_NOSUB);
	free(expression);
	if (ok != 0)
		return -1;
	matched = regexec(&regex, subject, 0, NULL, 0) == 0;
	regfree(&regex);

	/* Succeeded. */
	return matched;
}

/* Evaluates an operand of an integer comparison as an arithmetic expression. */
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
	ok = sh_eval_arith_text(text, value);

	/* Succeeded or not. */
	return ok;
}
