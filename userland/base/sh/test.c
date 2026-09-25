/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test and [ builtins (POSIX XCU test), with the grammar dash uses:
 *
 *	or:	 and ( -o and )*
 *	and:	 not ( -a not )*
 *	not:	 ! not | primary
 *	primary: ( or ) | unary operand | operand binary operand | operand
 *
 * The POSIX rules for three and four operands come first: a binary operator
 * in the middle of three words is taken as one, and an enclosing ( ) or a
 * leading ! is taken off before the grammar is applied.
 */

#include "userland/base/sh/shell.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* What a word is to test. */
#define TEST_END	0
#define TEST_OPERAND	1
#define TEST_UNARY	2
#define TEST_BINARY	3
#define TEST_NOT	4
#define TEST_AND	5
#define TEST_OR		6
#define TEST_LPAREN	7
#define TEST_RPAREN	8

/* The binary operators. */
#define TEST_OP_NONE		0
#define TEST_OP_STRING_EQ	1
#define TEST_OP_STRING_NE	2
#define TEST_OP_STRING_LT	3
#define TEST_OP_STRING_GT	4
#define TEST_OP_EQ		5
#define TEST_OP_NE		6
#define TEST_OP_LT		7
#define TEST_OP_LE		8
#define TEST_OP_GT		9
#define TEST_OP_GE		10
#define TEST_OP_NEWER		11
#define TEST_OP_OLDER		12
#define TEST_OP_SAME_FILE	13

/* An operator word, what it is, and which binary operator. */
struct test_word {
	const char *text;
	int kind;
	int op;
};

/* The state of one test: its operands and where it is among them. */
struct test_state {
	char **words;
	int count;
	int position;
	int error;
};

/* Every operator word but the unary ones, which are - and a letter. */
static const struct test_word test_words[] = {
	{ "!", TEST_NOT, TEST_OP_NONE },
	{ "-a", TEST_AND, TEST_OP_NONE },
	{ "-o", TEST_OR, TEST_OP_NONE },
	{ "(", TEST_LPAREN, TEST_OP_NONE },
	{ ")", TEST_RPAREN, TEST_OP_NONE },
	{ "=", TEST_BINARY, TEST_OP_STRING_EQ },
	{ "!=", TEST_BINARY, TEST_OP_STRING_NE },
	{ "<", TEST_BINARY, TEST_OP_STRING_LT },
	{ ">", TEST_BINARY, TEST_OP_STRING_GT },
	{ "-eq", TEST_BINARY, TEST_OP_EQ },
	{ "-ne", TEST_BINARY, TEST_OP_NE },
	{ "-lt", TEST_BINARY, TEST_OP_LT },
	{ "-le", TEST_BINARY, TEST_OP_LE },
	{ "-gt", TEST_BINARY, TEST_OP_GT },
	{ "-ge", TEST_BINARY, TEST_OP_GE },
	{ "-nt", TEST_BINARY, TEST_OP_NEWER },
	{ "-ot", TEST_BINARY, TEST_OP_OLDER },
	{ "-ef", TEST_BINARY, TEST_OP_SAME_FILE },
	{ NULL, 0, 0 }
};

static int test_bracket(int *argc, char **argv);
static int test_prepare(struct test_state *state, int *token);
static int test_lex(struct test_state *state, int offset);
static int test_word_kind(const char *word);
static int test_binary_op(const char *word);
static int test_or(struct test_state *state, int token);
static int test_and(struct test_state *state, int token);
static int test_not(struct test_state *state, int token);
static int test_primary(struct test_state *state, int token);
static int test_parenthesized(struct test_state *state);
static int test_unary(const char *op, const char *operand, struct test_state *state);
static int test_file(char letter, const char *operand, const struct stat *status);
static int test_binary(const char *left, const char *op, const char *right, struct test_state *state);
static int test_files(int op, const char *left, const char *right);
static int test_numbers(int op, intmax_t left, intmax_t right);
static int test_integer(const char *text, intmax_t *value, struct test_state *state);

/*
 * Implements test and [.
 */
int
sh_builtin_test(
	int argc,
	char **argv)
{
	struct test_state state;
	int token;
	int negate;
	int result;
	int valid;

	/* [ requires ] as its last operand. */
	valid = test_bracket(&argc, argv);
	if (!valid)
		return 2;

	/* No operand is false. */
	if (argc < 2)
		return 1;

	/* The three- and four-operand rules, then the first token. */
	state.words = argv + 1;
	state.count = argc - 1;
	state.position = 0;
	state.error = 0;
	negate = test_prepare(&state, &token);

	/* The expression. */
	result = test_or(&state, token);

	/* It must use every operand. */
	if (!state.error && state.position + 1 < state.count) {
		fprintf(stderr, "test: %s: unexpected operator\n",
			state.words[state.position]);
		state.error = 1;
	}

	/* A malformed expression is status 2. */
	if (state.error)
		return 2;

	/* Succeeded: 0 for true, 1 for false (the other way round after !). */
	if (negate)
		return result;
	return !result;
}

/*
 * Checks the ] that [ needs and takes it off.  Returns 0 (after a message)
 * when it is missing.
 */
static int
test_bracket(
	int *argc,
	char **argv)
{
	int compare;

	/* test takes no ]. */
	compare = strcmp(argv[0], "[");
	if (compare != 0)
		return 1;

	/* [ ends with ]. */
	if (*argc < 2) {
		fprintf(stderr, "[: missing ]\n");
		return 0;
	}

	/* The last word must be the closing bracket. */
	compare = strcmp(argv[*argc - 1], "]");
	if (compare != 0) {
		fprintf(stderr, "[: missing ]\n");
		return 0;
	}

	/* The bracket is not an operand. */
	(*argc)--;

	/* Succeeded. */
	return 1;
}

/*
 * Applies the rules for three and four operands, and finds the first token.
 * Returns 1 when a leading ! was taken off.
 */
static int
test_prepare(
	struct test_state *state,
	int *token)
{
	int negate;
	int first;
	int middle;
	int last;

	/* Applies the rules for three and four operands, taking off what they allow. */
	negate = 0;
	for (;;) {
		/* Only three or four operands have rules. */
		if (state->count != 3 && state->count != 4)
			break;
		first = test_word_kind(state->words[0]);
		middle = test_word_kind(state->words[1]);
		last = test_word_kind(state->words[state->count - 1]);

		/* A binary operator between two words is one. */
		if (state->count == 3 && middle == TEST_BINARY) {
			*token = TEST_OPERAND;
			return negate;
		}

		/* An enclosing ( ) is taken off. */
		if (first == TEST_LPAREN && last == TEST_RPAREN) {
			state->words++;
			state->count -= 2;
			break;
		}

		/* A leading ! is taken off, and the rules looked at again. */
		if (first == TEST_NOT) {
			negate = 1;
			state->words++;
			state->count--;
			continue;
		}

		break;
	}

	/* Succeeded: the grammar starts at the first word. */
	*token = test_lex(state, 0);
	return negate;
}

/*
 * Classifies the word at an offset from the position: the end, an operand,
 * or an operator.  A unary operator that is the last word, or that is
 * followed by a binary operator and another word, is an operand; so is a ( at
 * the end.
 */
static int
test_lex(
	struct test_state *state,
	int offset)
{
	int kind;
	int next;
	int at;

	/* Past the end. */
	at = state->position + offset;
	if (at >= state->count)
		return TEST_END;
	kind = test_word_kind(state->words[at]);

	/* A unary operator, last or before a binary one, is an operand. */
	if (kind == TEST_UNARY) {
		if (at + 1 >= state->count)
			return TEST_OPERAND;
		if (at + 2 < state->count) {
			next = test_word_kind(state->words[at + 1]);
			if (next == TEST_BINARY)
				return TEST_OPERAND;
		}
	}

	/* A ( at the end is an operand. */
	if (kind == TEST_LPAREN && at + 1 >= state->count)
		return TEST_OPERAND;

	/* Succeeded: the kind. */
	return kind;
}

/* Reports what a word is to test. */
static int
test_word_kind(
	const char *word)
{
	const char *letter;
	int compare;
	int index;

	/* The operator words. */
	for (index = 0; test_words[index].text != NULL; index++) {
		compare = strcmp(word, test_words[index].text);
		if (compare == 0)
			return test_words[index].kind;
	}

	/* - and a letter of a unary test. */
	if (word[0] != '-' || word[1] == '\0' || word[2] != '\0')
		return TEST_OPERAND;
	letter = strchr("rwxefdcbpugkstznhOGLS", word[1]);
	if (letter != NULL)
		return TEST_UNARY;

	/* Anything else is an operand. */
	return TEST_OPERAND;
}

/* Finds which binary operator a word is. */
static int
test_binary_op(
	const char *word)
{
	int compare;
	int index;

	/* The table's binary operators. */
	for (index = 0; test_words[index].text != NULL; index++) {
		compare = strcmp(word, test_words[index].text);
		if (compare == 0)
			return test_words[index].op;
	}

	/* Not one. */
	return TEST_OP_NONE;
}

/* or: and ( -o and )* */
static int
test_or(
	struct test_state *state,
	int token)
{
	int result;
	int value;

	/* Each and-expression, joined by -o. */
	result = 0;
	for (;;) {
		value = test_and(state, token);
		if (state->error)
			return 0;
		if (value)
			result = 1;

		/* Another -o, or the end of this or-expression. */
		token = test_lex(state, 1);
		if (token != TEST_OR)
			break;
		state->position += 2;
		token = test_lex(state, 0);
	}

	/* Succeeded: the value. */
	return result;
}

/* and: not ( -a not )* */
static int
test_and(
	struct test_state *state,
	int token)
{
	int result;
	int value;

	/* Each negation, joined by -a. */
	result = 1;
	for (;;) {
		value = test_not(state, token);
		if (state->error)
			return 0;
		if (!value)
			result = 0;

		/* Another -a, or the end of this and-expression. */
		token = test_lex(state, 1);
		if (token != TEST_AND)
			break;
		state->position += 2;
		token = test_lex(state, 0);
	}

	/* Succeeded: the value. */
	return result;
}

/* not: ! not | primary */
static int
test_not(
	struct test_state *state,
	int token)
{
	int result;

	/* ! turns the value round. */
	if (token == TEST_NOT) {
		state->position++;
		token = test_lex(state, 0);
		result = test_not(state, token);
		return !result;
	}

	/* Succeeded: the primary's value. */
	result = test_primary(state, token);
	return result;
}

/* primary: ( or ) | unary operand | operand binary operand | operand */
static int
test_primary(
	struct test_state *state,
	int token)
{
	const char *op;
	const char *left;
	int next;

	/* A missing expression is false. */
	if (token == TEST_END)
		return 0;

	/* A parenthesized expression. */
	if (token == TEST_LPAREN)
		return test_parenthesized(state);

	/* A unary operator and its operand. */
	if (token == TEST_UNARY) {
		op = state->words[state->position];
		state->position++;
		if (state->position >= state->count) {
			fprintf(stderr, "test: %s: argument expected\n", op);
			state->error = 1;
			return 0;
		}

		/* The operator applies to the next word. */
		return test_unary(op, state->words[state->position], state);
	}

	/* A binary operator after this word. */
	next = test_lex(state, 1);
	if (next == TEST_BINARY) {
		left = state->words[state->position];
		op = state->words[state->position + 1];
		if (state->position + 2 >= state->count) {
			fprintf(stderr, "test: %s: argument expected\n", op);
			state->error = 1;
			return 0;
		}

		/* The operator applies to the words on either side. */
		state->position += 2;
		return test_binary(left, op, state->words[state->position],
				   state);
	}

	/* Succeeded: a word is true when it is not empty. */
	if (state->words[state->position][0] == '\0')
		return 0;
	return 1;
}

/* ( or ): the position is at the (. */
static int
test_parenthesized(
	struct test_state *state)
{
	int result;
	int token;

	/* ( ) alone is false. */
	state->position++;
	token = test_lex(state, 0);
	if (token == TEST_RPAREN)
		return 0;

	/* The expression inside. */
	result = test_or(state, token);
	state->position++;
	if (state->error)
		return 0;

	/* The ) after it. */
	token = test_lex(state, 0);
	if (token != TEST_RPAREN) {
		fprintf(stderr, "test: closing paren expected\n");
		state->error = 1;
		return 0;
	}

	/* Succeeded: the value inside. */
	return result;
}

/* Evaluates a unary primary. */
static int
test_unary(
	const char *op,
	const char *operand,
	struct test_state *state)
{
	struct stat status;
	intmax_t number;
	int valid;
	int error;

	/* The string tests. */
	if (op[1] == 'n')
		return operand[0] != '\0';
	if (op[1] == 'z')
		return operand[0] == '\0';

	/* -t: whether a descriptor is a terminal. */
	if (op[1] == 't') {
		valid = test_integer(operand, &number, state);
		if (!valid)
			return 0;
		return isatty((int)number);
	}

	/* The file tests: -h and -L look at a link itself. */
	if (op[1] == 'h' || op[1] == 'L')
		error = lstat(operand, &status);
	else
		error = stat(operand, &status);
	if (error != 0)
		return 0;

	/* Succeeded: the test of the file. */
	return test_file(op[1], operand, &status);
}

/* Evaluates a file test on a file that exists. */
static int
test_file(
	char letter,
	const char *operand,
	const struct stat *status)
{
	/* Dispatches on the letter. */
	switch (letter) {
	case 'b':
		return S_ISBLK(status->st_mode);
	case 'c':
		return S_ISCHR(status->st_mode);
	case 'd':
		return S_ISDIR(status->st_mode);
	case 'e':
		return 1;
	case 'f':
		return S_ISREG(status->st_mode);
	case 'g':
		return (status->st_mode & S_ISGID) != 0;
	case 'h':
	case 'L':
		return S_ISLNK(status->st_mode);
	case 'k':
		return (status->st_mode & S_ISVTX) != 0;
	case 'p':
		return S_ISFIFO(status->st_mode);
	case 'r':
		return access(operand, R_OK) == 0;
	case 's':
		return status->st_size > 0;
	case 'S':
		return S_ISSOCK(status->st_mode);
	case 'u':
		return (status->st_mode & S_ISUID) != 0;
	case 'w':
		return access(operand, W_OK) == 0;
	case 'x':
		return access(operand, X_OK) == 0;
	case 'O':
		return status->st_uid == geteuid();
	case 'G':
		return status->st_gid == getegid();
	default:
		break;
	}

	/* Not a file test. */
	return 0;
}

/* Evaluates a binary primary. */
static int
test_binary(
	const char *left,
	const char *op,
	const char *right,
	struct test_state *state)
{
	intmax_t left_number;
	intmax_t right_number;
	int compare;
	int valid;
	int code;

	/* The string comparisons. */
	code = test_binary_op(op);
	compare = strcmp(left, right);
	switch (code) {
	case TEST_OP_STRING_EQ:
		return compare == 0;
	case TEST_OP_STRING_NE:
		return compare != 0;
	case TEST_OP_STRING_LT:
		return compare < 0;
	case TEST_OP_STRING_GT:
		return compare > 0;
	case TEST_OP_NEWER:
	case TEST_OP_OLDER:
	case TEST_OP_SAME_FILE:
		return test_files(code, left, right);
	default:
		break;
	}

	/* The integer comparisons, of two integers. */
	valid = test_integer(left, &left_number, state);
	if (!valid)
		return 0;
	valid = test_integer(right, &right_number, state);
	if (!valid)
		return 0;

	/* Succeeded: the comparison. */
	return test_numbers(code, left_number, right_number);
}

/* Evaluates -nt, -ot or -ef. */
static int
test_files(
	int op,
	const char *left,
	const char *right)
{
	struct stat left_status;
	struct stat right_status;
	int left_found;
	int right_found;
	int error;

	/* Which of the files exist. */
	error = stat(left, &left_status);
	left_found = error == 0;
	error = stat(right, &right_status);
	right_found = error == 0;

	/* -ef: the same file. */
	if (op == TEST_OP_SAME_FILE) {
		if (!left_found || !right_found)
			return 0;
		if (left_status.st_dev != right_status.st_dev)
			return 0;
		return left_status.st_ino == right_status.st_ino;
	}

	/* -nt: a file that exists is newer than one that does not. */
	if (op == TEST_OP_NEWER) {
		if (!left_found)
			return 0;
		if (!right_found)
			return 1;
		return left_status.st_mtime > right_status.st_mtime;
	}

	/* -ot, the other way round. */
	if (!right_found)
		return 0;
	if (!left_found)
		return 1;
	return left_status.st_mtime < right_status.st_mtime;
}

/* Evaluates an integer comparison. */
static int
test_numbers(
	int op,
	intmax_t left,
	intmax_t right)
{
	/* Dispatches on the operator. */
	switch (op) {
	case TEST_OP_EQ:
		return left == right;
	case TEST_OP_NE:
		return left != right;
	case TEST_OP_LT:
		return left < right;
	case TEST_OP_LE:
		return left <= right;
	case TEST_OP_GT:
		return left > right;
	default:
		break;
	}

	/* -ge. */
	return left >= right;
}

/*
 * Reads an integer operand of test: optional blanks and sign, digits,
 * optional blanks.  Anything else is an error.
 */
static int
test_integer(
	const char *text,
	intmax_t *value,
	struct test_state *state)
{
	const char *cursor;
	char *end;
	int error;
	int valid;

	/* Leading blanks, then a digit or a sign must start the number. */
	cursor = text;
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	valid = 1;
	if ((*cursor < '0' || *cursor > '9') && *cursor != '-' &&
	    *cursor != '+')
		valid = 0;

	/* The number, and only blanks after it. */
	errno = 0;
	*value = strtoimax(cursor, &end, 10);
	error = errno;
	if (end == cursor)
		valid = 0;
	while (*end == ' ' || *end == '\t')
		end++;
	if (*end != '\0')
		valid = 0;
	if (error == ERANGE)
		valid = 0;

	/* Not an integer: an error. */
	if (!valid) {
		fprintf(stderr, "test: Illegal number: %s\n", text);
		state->error = 1;
		return 0;
	}

	/* Succeeded. */
	return 1;
}
