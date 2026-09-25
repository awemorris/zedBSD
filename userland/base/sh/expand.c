/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Word expansion (POSIX XCU 2.6).
 *
 * The text of a word is read once, left to right.  What it produces goes into
 * a buffer that keeps, for each character, whether it was quoted (it is then
 * never split, and stands for itself in a pattern) and whether it came from
 * an unquoted expansion (only such characters are split at IFS).  Two marks
 * that are not characters travel in the same buffer: one where "$@" puts the
 * boundary between two parameters, and one that says a field exists even if
 * it ends up empty, as "" or "$empty" does.  Field splitting then reads the
 * buffer, and quote removal has already happened: the quotes never entered
 * it.
 *
 * Memory comes from sh_malloc, which raises the shell's error when there is
 * none; a function here returns 0 only for a fault of the expansion itself,
 * with the message in the expander.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/expand.h"
#include "userland/base/sh/arithmetic.h"
#include "userland/base/sh/glob.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How expand_text reads its text (the mode of a reader). */
#define M_HEREDOC	0x01	/* a here-document body: " is ordinary */
#define M_BRACE		0x02	/* the word of ${...} in "...": \} is } */
#define M_SPLIT		0x04	/* the word of ${...} unquoted: split it too */

/* The attributes of one entry of an expansion buffer. */
#define X_QUOTED	0x01	/* quoted: not split, literal in a pattern */
#define X_SPLIT		0x02	/* from an unquoted expansion: split at IFS */
#define X_BREAK		0x04	/* not a character: a boundary of "$@" */
#define X_KEEP		0x08	/* not a character: the field exists */

/* What a character is to field splitting. */
#define SPLIT_NONE	0	/* part of a field */
#define SPLIT_WHITE	1	/* IFS white space */
#define SPLIT_OTHER	2	/* another IFS character */

/* The longest parameter name copied for a lookup or an assignment. */
#define NAME_MAX_LENGTH 256

/* An expansion buffer: characters and their attributes. */
struct xbuf {
	char *data;
	unsigned char *attr;
	size_t length;
	size_t capacity;
};

/* The state of one expansion. */
struct expander {
	const struct sh_expand_context *context;
	const char *error;

	/* Set while expanding an assignment, for the tilde rules of one. */
	int assignment;
};

/* Text being expanded, and how it is read. */
struct reader {
	const char *text;
	size_t length;
	size_t position;

	/*
	 * in_double: the text is inside a double quotation, where a
	 * backslash protects only $ ` " \ and a newline, and a single quote
	 * is ordinary.  quoted: what is produced is quoted.  mode: the M_
	 * flags.
	 */
	int in_double;
	int quoted;
	int mode;
};

/* A parsed ${...}: the parameter, the operator and the word. */
struct brace {
	const char *name;
	size_t name_length;
	int special;		/* @ or *, which are the positionals */
	int length_wanted;	/* ${#name} */
	char op[3];		/* "", "-", "=", "?", "+", "%", "%%", "#", "##" */
	int colon;		/* the operator had a : before it */
	const char *word;
	size_t word_length;

	/* The value, and whether the parameter is set. */
	const char *value;
	int set;
};

/* The message of the last fault, which outlives the call. */
static char expand_message[512];

int sh_expand_fatal;

static int expand_token(struct expander *x, const struct sh_token *token, struct xbuf *out);
static int expand_text(struct expander *x, const char *text, size_t length, int in_double, int quoted, int mode, struct xbuf *out);
static int expand_next(struct expander *x, struct reader *reader, struct xbuf *out);
static int tilde_starts(const struct expander *x, const struct reader *reader);
static void expand_backslash(struct reader *reader, struct xbuf *out);
static int backslash_protects(const struct reader *reader, char next);
static void expand_single(struct reader *reader, struct xbuf *out);
static int expand_double(struct expander *x, struct reader *reader, struct xbuf *out);
static int is_bare_at(const char *text, size_t length);
static int expand_backquote(struct expander *x, struct reader *reader, struct xbuf *out);
static int expand_dollar(struct expander *x, struct reader *reader, struct xbuf *out);
static int expand_enclosed(struct expander *x, struct reader *reader, struct xbuf *out);
static int expand_simple(struct expander *x, struct reader *reader, size_t name_length, struct xbuf *out);
static int expand_brace(struct expander *x, const char *text, size_t length, const struct reader *reader, struct xbuf *out);
static int parse_brace(const char *text, size_t length, struct brace *brace);
static int parse_brace_operator(struct brace *brace);
static int brace_length(struct expander *x, const struct brace *brace, int quoted, struct xbuf *out);
static int brace_uses_word(struct expander *x, const struct brace *brace);
static int brace_is_null(struct expander *x, const struct brace *brace);
static int brace_word(struct expander *x, const struct brace *brace, const struct reader *reader, struct xbuf *out);
static int brace_alternative(struct expander *x, const struct brace *brace, const struct reader *reader, struct xbuf *out);
static int brace_assign(struct expander *x, const struct brace *brace, const struct reader *reader, struct xbuf *out);
static int brace_error(struct expander *x, const struct brace *brace, const struct reader *reader, struct xbuf *out);
static int brace_trim(struct expander *x, const struct brace *brace, int quoted, struct xbuf *out);
static int brace_value(struct expander *x, const struct brace *brace, int quoted, struct xbuf *out);
static int brace_word_string(struct expander *x, const struct brace *brace, const struct reader *reader, char **string);
static void trim(const char *value, const struct xbuf *pattern, const char *op, int quoted, struct xbuf *out);
static size_t trim_prefix(const char *value, size_t length, const char *text, const unsigned char *marks, int longest, char *candidate);
static size_t trim_suffix(const char *value, size_t length, const char *text, const unsigned char *marks, int longest, char *candidate);
static void expand_tilde(struct expander *x, struct reader *reader, struct xbuf *out);
static size_t tilde_end(const struct expander *x, const struct reader *reader, int *plain);
static const char *home_directory(struct expander *x, const char *user);
static int arithmetic_form(const char *start, const char *end);
static int command_output(struct expander *x, const char *source, int quoted, struct xbuf *out);
static int arithmetic(struct expander *x, const char *text, size_t length, int quoted, struct xbuf *out);
static int parameter(struct expander *x, const char *name, size_t length, const char **value, int *set);
static void special_parameter(struct expander *x, char name, const char **value, int *set);
static void positional_parameter(struct expander *x, const char *name, size_t length, const char **value, int *set);
static int unset_error(struct expander *x, const char *name, size_t length);
static void append_value(struct xbuf *out, const char *value, int quoted);
static void append_positionals(struct expander *x, char which, int quoted, struct xbuf *out);
static void append_char(struct xbuf *out, char value, unsigned char attr);
static void append_mark(struct xbuf *out, unsigned char mark);
static char *buffer_string(const struct xbuf *in, unsigned char **quoted);
static void buffer_free(struct xbuf *buffer);
static void split_fields(struct expander *x, const struct xbuf *in, struct sh_field_list *fields);
static int split_class(const char *ifs, const struct xbuf *in, size_t index);
static size_t skip_split_white(const char *ifs, const struct xbuf *in, size_t index);
static void field_add(struct sh_field_list *fields, const struct xbuf *in);
static int positional_null(struct expander *x);
static const char *lookup(struct expander *x, const char *name);
static const char *ifs_value(struct expander *x);
static int assignment_prefix(const struct sh_token *token);
static size_t scan_name(const char *text, size_t length);
static int is_special(char value);
static int name_start(char value);
static int name_char(char value);
static const char *failure(const struct expander *x);

/*
 * Expands a word with every expansion and field splitting.
 */
int
sh_expand_fields(
	const struct sh_token *token,
	const struct sh_expand_context *context,
	struct sh_field_list *fields,
	const char **error_text)
{
	struct expander x;
	struct xbuf out;
	int ok;

	/* The expansion. */
	memset(fields, 0, sizeof(*fields));
	memset(&x, 0, sizeof(x));
	memset(&out, 0, sizeof(out));
	x.context = context;
	sh_expand_fatal = 0;
	ok = expand_token(&x, token, &out);
	if (!ok) {
		*error_text = failure(&x);
		buffer_free(&out);
		return 0;
	}

	/* Succeeded: the fields it splits into. */
	split_fields(&x, &out, fields);
	buffer_free(&out);
	*error_text = NULL;
	return 1;
}

/*
 * Expands a word into one string without field splitting.
 */
int
sh_expand_word(
	const struct sh_token *token,
	const struct sh_expand_context *context,
	char **result,
	const char **error_text)
{
	struct expander x;
	struct xbuf out;
	int ok;

	/* The expansion, with the tilde rules of an assignment for one. */
	memset(&x, 0, sizeof(x));
	memset(&out, 0, sizeof(out));
	x.context = context;
	x.assignment = assignment_prefix(token);
	sh_expand_fatal = 0;
	*result = NULL;
	ok = expand_token(&x, token, &out);
	if (!ok) {
		*error_text = failure(&x);
		buffer_free(&out);
		return 0;
	}

	/* Succeeded: one string. */
	*result = buffer_string(&out, NULL);
	buffer_free(&out);
	*error_text = NULL;
	return 1;
}

/*
 * Expands a pattern into one string and the marks of its quoted characters.
 */
int
sh_expand_pattern(
	const struct sh_token *token,
	const struct sh_expand_context *context,
	char **result,
	unsigned char **quoted,
	const char **error_text)
{
	struct expander x;
	struct xbuf out;
	int ok;

	/* The expansion. */
	memset(&x, 0, sizeof(x));
	memset(&out, 0, sizeof(out));
	x.context = context;
	sh_expand_fatal = 0;
	*result = NULL;
	*quoted = NULL;
	ok = expand_token(&x, token, &out);
	if (!ok) {
		*error_text = failure(&x);
		buffer_free(&out);
		return 0;
	}

	/* Succeeded: one string and its marks. */
	*result = buffer_string(&out, quoted);
	buffer_free(&out);
	*error_text = NULL;
	return 1;
}

/*
 * Expands a string as an arithmetic expression and evaluates it.
 */
int
sh_expand_arithmetic(
	const char *text,
	const struct sh_expand_context *context,
	long *result,
	const char **error_text)
{
	struct expander x;
	struct xbuf out;
	char *expression;
	long long value;
	int ok;

	/* The expansions in it, as in a here-document. */
	memset(&x, 0, sizeof(x));
	memset(&out, 0, sizeof(out));
	x.context = context;
	ok = expand_text(&x, text, strlen(text), 1, 1, M_HEREDOC, &out);
	if (!ok) {
		*error_text = failure(&x);
		buffer_free(&out);
		return 0;
	}

	/* The expanded text of the expression. */
	expression = buffer_string(&out, NULL);
	buffer_free(&out);

	/* The expression. */
	value = 0;
	ok = sh_arithmetic_eval(expression, context->lookup, context->assign,
				context->lookup_context, &value, error_text);
	free(expression);
	*result = (long)value;

	/* Succeeded when the expression was valid. */
	return ok;
}

/*
 * Frees a field list.
 */
void
sh_fields_free(
	struct sh_field_list *fields)
{
	size_t index;

	/* Each field and its marks, then the arrays. */
	for (index = 0; index < fields->count; index++) {
		free(fields->fields[index]);
		if (fields->quoted != NULL)
			free(fields->quoted[index]);
	}

	/* The arrays themselves. */
	free(fields->fields);
	free(fields->quoted);
	fields->fields = NULL;
	fields->quoted = NULL;
	fields->count = 0;
}

/* Expands a token: a here-document body, or a word from its raw text. */
static int
expand_token(
	struct expander *x,
	const struct sh_token *token,
	struct xbuf *out)
{
	size_t index;

	/* A here-document with a quoted delimiter is taken as written. */
	if (token->heredoc == SH_HEREDOC_LITERAL) {
		for (index = 0; index < token->raw_length; index++)
			append_char(out, token->raw[index], X_QUOTED);
		append_mark(out, X_KEEP);
		return 1;
	}

	/* One with an unquoted delimiter is expanded, as if quoted. */
	if (token->heredoc == SH_HEREDOC_EXPAND) {
		append_mark(out, X_KEEP);
		return expand_text(x, token->raw, token->raw_length, 1, 1,
				   M_HEREDOC, out);
	}

	/* A word made without raw text is taken as it stands. */
	if (token->raw == NULL) {
		for (index = 0; index < token->length; index++)
			append_char(out, token->text[index], X_QUOTED);
		append_mark(out, X_KEEP);
		return 1;
	}

	/* Succeeded when the text expanded. */
	return expand_text(x, token->raw, token->raw_length, 0, 0, 0, out);
}

/*
 * Expands text into out.  in_double, quoted and mode are as in struct
 * reader.
 */
static int
expand_text(
	struct expander *x,
	const char *text,
	size_t length,
	int in_double,
	int quoted,
	int mode,
	struct xbuf *out)
{
	struct reader reader;
	int ok;

	/* The text, from its start. */
	reader.text = text;
	reader.length = length;
	reader.position = 0;
	reader.in_double = in_double;
	reader.quoted = quoted;
	reader.mode = mode;

	/* Each construct in turn. */
	while (reader.position < reader.length) {
		ok = expand_next(x, &reader, out);
		if (!ok)
			return 0;
	}

	/* Succeeded. */
	return 1;
}

/* Expands the construct, or the character, at the reader's position. */
static int
expand_next(
	struct expander *x,
	struct reader *reader,
	struct xbuf *out)
{
	unsigned char attr;
	char value;
	int tilde;

	/* A tilde that begins the word, or a part of an assignment. */
	tilde = tilde_starts(x, reader);
	if (tilde) {
		expand_tilde(x, reader, out);
		return 1;
	}

	/* The quoting and expansion characters. */
	value = reader->text[reader->position];
	switch (value) {
	case '\\':
		expand_backslash(reader, out);
		return 1;
	case '\'':
		if (reader->in_double)
			break;
		expand_single(reader, out);
		return 1;
	case '"':
		if ((reader->mode & M_HEREDOC) != 0)
			break;
		return expand_double(x, reader, out);
	case '`':
		return expand_backquote(x, reader, out);
	case '$':
		return expand_dollar(x, reader, out);
	default:
		break;
	}

	/* An ordinary character: quoted, split, or neither. */
	attr = 0;
	if (reader->quoted)
		attr = X_QUOTED;
	else if ((reader->mode & M_SPLIT) != 0)
		attr = X_SPLIT;
	append_char(out, value, attr);
	reader->position++;

	/* Succeeded. */
	return 1;
}

/*
 * Reports whether a tilde prefix starts at the position: an unquoted ~ at
 * the start of the word, or, in an assignment, after a : or after the first
 * =.
 */
static int
tilde_starts(
	const struct expander *x,
	const struct reader *reader)
{
	const char *text;
	const char *equals;
	size_t position;

	/* An unquoted tilde. */
	text = reader->text;
	position = reader->position;
	if (text[position] != '~' || reader->in_double)
		return 0;

	/* The start of the word. */
	if (position == 0)
		return 1;

	/* In an assignment, after a colon. */
	if (!x->assignment)
		return 0;
	if (text[position - 1] == ':')
		return 1;

	/* Or right after the = that ends the name. */
	if (text[position - 1] != '=')
		return 0;
	equals = memchr(text, '=', position - 1);
	if (equals != NULL)
		return 0;

	/* Succeeded: it does. */
	return 1;
}

/*
 * Expands a backslash: it quotes the next character, removes itself and a
 * newline, or (inside double quotes, before a character it does not
 * protect) stands for itself.
 */
static void
expand_backslash(
	struct reader *reader,
	struct xbuf *out)
{
	unsigned char attr;
	char next;
	int protects;

	/* A backslash that ends the text stands for itself. */
	attr = 0;
	if (reader->quoted)
		attr = X_QUOTED;
	else if ((reader->mode & M_SPLIT) != 0)
		attr = X_SPLIT;
	if (reader->position + 1 >= reader->length) {
		append_char(out, '\\', attr);
		reader->position++;
		return;
	}

	/* Inside double quotes it protects only some characters. */
	next = reader->text[reader->position + 1];
	protects = backslash_protects(reader, next);
	if (!protects) {
		append_char(out, '\\', attr);
		reader->position++;
		return;
	}

	/* A backslash and a newline are removed. */
	reader->position += 2;
	if (next == '\n')
		return;

	/* Any other character is quoted. */
	append_char(out, next, X_QUOTED);
}

/* Reports whether a backslash quotes the character after it. */
static int
backslash_protects(
	const struct reader *reader,
	char next)
{
	const char *protected_characters;
	const char *found;

	/* Outside double quotes, every character. */
	if (!reader->in_double)
		return 1;

	/* Inside, $ ` \ newline, and " (not in a here-document) or }. */
	if ((reader->mode & M_HEREDOC) != 0)
		protected_characters = "$`\\\n";
	else if ((reader->mode & M_BRACE) != 0)
		protected_characters = "$`\"\\\n}";
	else
		protected_characters = "$`\"\\\n";
	found = strchr(protected_characters, next);
	if (found == NULL)
		return 0;

	/* Succeeded: it does. */
	return 1;
}

/* Expands a single quotation: its characters, all quoted. */
static void
expand_single(
	struct reader *reader,
	struct xbuf *out)
{
	const char *end;
	size_t stop;

	/* Where the closing quote is (the end, when there is none). */
	end = sh_skip_single(reader->text + reader->position);
	stop = reader->length;
	if (end != NULL)
		stop = (size_t)(end - reader->text) - 1U;

	/* The field exists, even when the quotation is empty. */
	append_mark(out, X_KEEP);
	for (reader->position++; reader->position < stop; reader->position++)
		append_char(out, reader->text[reader->position], X_QUOTED);
	reader->position = stop + 1U;
}

/* Expands a double quotation: its contents, quoted. */
static int
expand_double(
	struct expander *x,
	struct reader *reader,
	struct xbuf *out)
{
	const char *end;
	const char *inside;
	size_t stop;
	size_t length;
	int bare;
	int ok;

	/* Where the closing quote is (the end, when there is none). */
	end = sh_skip_double(reader->text + reader->position);
	stop = reader->length;
	if (end != NULL)
		stop = (size_t)(end - reader->text) - 1U;
	inside = reader->text + reader->position + 1;
	length = stop - reader->position - 1U;
	reader->position = stop + 1U;

	/* "$@" with no parameters is no field at all. */
	bare = is_bare_at(inside, length);
	if (bare && x->context->positional_count == 0)
		return 1;

	/* Succeeded when the contents expanded; the field exists. */
	append_mark(out, X_KEEP);
	ok = expand_text(x, inside, length, 1, 1, 0, out);
	return ok;
}

/* Reports whether text is exactly $@ or ${@}. */
static int
is_bare_at(
	const char *text,
	size_t length)
{
	int compare;

	/* $@ */
	if (length == 2) {
		compare = memcmp(text, "$@", 2);
		if (compare == 0)
			return 1;
	}

	/* ${@} */
	if (length == 4) {
		compare = memcmp(text, "${@}", 4);
		if (compare == 0)
			return 1;
	}

	/* Anything else. */
	return 0;
}

/*
 * Runs a backquoted command: inside it a backslash protects only $ ` \ (and
 * " inside a double quotation), and is removed before the command is read.
 */
static int
expand_backquote(
	struct expander *x,
	struct reader *reader,
	struct xbuf *out)
{
	const char *text;
	const char *end;
	char *source;
	char next;
	size_t length;
	size_t index;
	size_t used;
	int ok;

	/* The text between the backquotes (to the end, when unclosed). */
	text = reader->text + reader->position + 1;
	end = sh_skip_expansion(reader->text + reader->position,
				reader->in_double);
	if (end == NULL) {
		length = reader->length - reader->position - 1U;
		reader->position = reader->length;
	} else {
		length = (size_t)(end - text) - 1U;
		reader->position = (size_t)(end - reader->text);
	}

	/* The command, with its protecting backslashes removed. */
	source = sh_malloc(length + 1U);
	used = 0;
	for (index = 0; index < length; index++) {
		next = '\0';
		if (text[index] == '\\' && index + 1 < length)
			next = text[index + 1];
		if (next == '$' || next == '`' || next == '\\')
			index++;
		else if (next == '"' && reader->in_double)
			index++;
		source[used++] = text[index];
	}

	/* The source ends with a null. */
	source[used] = '\0';

	/* Succeeded when its output was added. */
	ok = command_output(x, source, reader->quoted, out);
	free(source);
	return ok;
}

/* Expands one dollar sign, and moves past what it began. */
static int
expand_dollar(
	struct expander *x,
	struct reader *reader,
	struct xbuf *out)
{
	unsigned char attr;
	size_t name_length;
	char next;
	int start;
	int special;

	/* A dollar sign that ends the text stands for itself. */
	attr = 0;
	if (reader->quoted)
		attr = X_QUOTED;
	if (reader->position + 1 >= reader->length) {
		reader->position++;
		append_char(out, '$', attr);
		return 1;
	}

	/* ${...}, $(...) and $((...)). */
	next = reader->text[reader->position + 1];
	if (next == '{' || next == '(')
		return expand_enclosed(x, reader, out);

	/* $@ and $*. */
	if (next == '@' || next == '*') {
		reader->position += 2;
		append_positionals(x, next, reader->quoted, out);
		return 1;
	}

	/* A name, or one digit or special character. */
	name_length = 0;
	start = name_start(next);
	special = is_special(next);
	if (start) {
		name_length = scan_name(reader->text + reader->position + 1,
					reader->length - reader->position - 1);
	} else if (special || (next >= '0' && next <= '9')) {
		name_length = 1;
	}

	/* A dollar sign that begins nothing stands for itself. */
	if (name_length == 0) {
		reader->position++;
		append_char(out, '$', attr);
		return 1;
	}

	/* Succeeded when the parameter was added. */
	return expand_simple(x, reader, name_length, out);
}

/* Expands ${...}, $(...) or $((...)) at the position. */
static int
expand_enclosed(
	struct expander *x,
	struct reader *reader,
	struct xbuf *out)
{
	const char *start;
	const char *end;
	char *source;
	size_t inner;
	int form;
	int ok;

	/* Where it ends, which must be within the text. */
	start = reader->text + reader->position;
	end = sh_skip_expansion(start, reader->in_double);
	if (end == NULL || (size_t)(end - reader->text) > reader->length) {
		if (start[1] == '{')
			x->error = "unterminated parameter expansion";
		else
			x->error = "unterminated command substitution";
		return 0;
	}

	/* The reader moves past the expansion. */
	reader->position = (size_t)(end - reader->text);
	inner = (size_t)(end - start);

	/* ${...}. */
	if (start[1] == '{')
		return expand_brace(x, start + 2, inner - 3U, reader, out);

	/* $((...)). */
	form = arithmetic_form(start, end);
	if (form)
		return arithmetic(x, start + 3, inner - 5U, reader->quoted, out);

	/* $(...): the command. */
	source = sh_strndup(start + 2, inner - 3U);
	ok = command_output(x, source, reader->quoted, out);
	free(source);

	/* Succeeded when the command's output was added. */
	return ok;
}

/* Expands $name, $digit or a special parameter of name_length characters. */
static int
expand_simple(
	struct expander *x,
	struct reader *reader,
	size_t name_length,
	struct xbuf *out)
{
	const char *name;
	const char *value;
	int set;
	int ok;

	/* The value. */
	name = reader->text + reader->position + 1;
	reader->position += 1U + name_length;
	ok = parameter(x, name, name_length, &value, &set);
	if (!ok)
		return 0;

	/* An unset parameter is a fault under set -u. */
	if (!set && x->context->unset_is_error)
		return unset_error(x, name, name_length);

	/* Succeeded: the value; quoted, the field exists. */
	if (reader->quoted)
		append_mark(out, X_KEEP);
	append_value(out, value, reader->quoted);
	return 1;
}

/*
 * Expands the inside of ${...}: the name, the operator, and the word, which
 * is only expanded when the operator uses it.
 */
static int
expand_brace(
	struct expander *x,
	const char *text,
	size_t length,
	const struct reader *reader,
	struct xbuf *out)
{
	struct brace brace;
	int valid;
	int ok;

	/* The parts. */
	valid = parse_brace(text, length, &brace);
	if (!valid) {
		x->error = "bad substitution";
		return 0;
	}

	/* The value; @ and * are read where they are used. */
	brace.value = NULL;
	brace.set = 1;
	if (!brace.special) {
		ok = parameter(x, brace.name, brace.name_length, &brace.value,
			       &brace.set);
		if (!ok)
			return 0;
	}

	/* ${#name}. */
	if (brace.length_wanted)
		return brace_length(x, &brace, reader->quoted, out);

	/* Dispatches on the operator. */
	switch (brace.op[0]) {
	case '-':
	case '+':
		return brace_alternative(x, &brace, reader, out);
	case '=':
		return brace_assign(x, &brace, reader, out);
	case '?':
		return brace_error(x, &brace, reader, out);
	case '%':
	case '#':
		return brace_trim(x, &brace, reader->quoted, out);
	default:
		break;
	}

	/* Succeeded: the plain value. */
	return brace_value(x, &brace, reader->quoted, out);
}

/*
 * Parses the inside of ${...}.  Returns 0 for a bad substitution.
 */
static int
parse_brace(
	const char *text,
	size_t length,
	struct brace *brace)
{
	size_t name_length;

	/* Nothing is known about the expansion yet. */
	memset(brace, 0, sizeof(*brace));

	/* ${#name} is the length; ${#} alone, and ${#-...}, are about $#. */
	if (length > 1 && text[0] == '#') {
		name_length = scan_name(text + 1, length - 1U);
		if (name_length != 0 && 1U + name_length == length) {
			brace->length_wanted = 1;
			text++;
			length--;
		}
	}

	/* The name: a name, a run of digits, or one special character. */
	name_length = scan_name(text, length);
	if (name_length == 0)
		return 0;
	brace->name = text;
	brace->name_length = name_length;
	if (text[0] == '@' || text[0] == '*')
		brace->special = 1;

	/* The operator and the word. */
	brace->word = text + name_length;
	brace->word_length = length - name_length;
	if (brace->word_length == 0)
		return 1;
	if (brace->length_wanted)
		return 0;

	/* Succeeded when the operator is one. */
	return parse_brace_operator(brace);
}

/* Parses the operator that begins the word of ${...}. */
static int
parse_brace_operator(
	struct brace *brace)
{
	const char *word;
	char first;
	char second;

	/* The first two characters after the name, which spell the operator. */
	word = brace->word;
	first = word[0];
	second = '\0';
	if (brace->word_length > 1)
		second = word[1];

	/* :- := :? :+ */
	if (first == ':' &&
	    (second == '-' || second == '=' || second == '?' || second == '+')) {
		brace->colon = 1;
		brace->op[0] = second;
		brace->word += 2;
		brace->word_length -= 2;
		return 1;
	}

	/* - = ? + */
	if (first == '-' || first == '=' || first == '?' || first == '+') {
		brace->op[0] = first;
		brace->word++;
		brace->word_length--;
		return 1;
	}

	/* Anything else but % %% # ## is bad. */
	if (first != '%' && first != '#')
		return 0;
	brace->op[0] = first;
	brace->word++;
	brace->word_length--;
	if (second == first) {
		brace->op[1] = first;
		brace->word++;
		brace->word_length--;
	}

	/* Succeeded. */
	return 1;
}

/* Expands ${#name}: the length of the value, or the number of positionals. */
static int
brace_length(
	struct expander *x,
	const struct brace *brace,
	int quoted,
	struct xbuf *out)
{
	char number[32];
	size_t count;

	/* ${#@} and ${#*} count the positionals. */
	if (brace->special) {
		count = (size_t)x->context->positional_count;
	} else {
		/* An unset parameter is a fault under set -u. */
		if (!brace->set && x->context->unset_is_error)
			return unset_error(x, brace->name, brace->name_length);
		count = strlen(brace->value);
	}

	/* Succeeded: the number. */
	(void)snprintf(number, sizeof(number), "%zu", count);
	append_value(out, number, quoted);
	return 1;
}

/*
 * Reports whether the operator uses its word: - = ? when the parameter is
 * unset (or null, with :), + when it is set (and not null, with :).
 */
static int
brace_uses_word(
	struct expander *x,
	const struct brace *brace)
{
	int null;

	/* Null counts only with a colon. */
	null = 0;
	if (brace->colon)
		null = brace_is_null(x, brace);

	/* + uses it for a set, non-null parameter. */
	if (brace->op[0] == '+') {
		if (!brace->set || null)
			return 0;
		return 1;
	}

	/* The others for an unset or null one. */
	if (!brace->set || null)
		return 1;
	return 0;
}

/* Reports whether the parameter's value is empty. */
static int
brace_is_null(
	struct expander *x,
	const struct brace *brace)
{
	/* $@ and $* joined. */
	if (brace->special)
		return positional_null(x);

	/* Any other parameter. */
	if (brace->value[0] == '\0')
		return 1;
	return 0;
}

/*
 * Expands the word of ${...} as the operator reads it: quoted inside double
 * quotes (where \} is }), split otherwise.
 */
static int
brace_word(
	struct expander *x,
	const struct brace *brace,
	const struct reader *reader,
	struct xbuf *out)
{
	int mode;

	/* The mode of the word. */
	mode = 0;
	if (reader->in_double)
		mode = M_BRACE;
	else if (!reader->quoted)
		mode = M_SPLIT;

	/* Succeeded when it expanded. */
	return expand_text(x, brace->word, brace->word_length,
			   reader->in_double, reader->quoted, mode, out);
}

/* Expands ${name-word} or ${name+word} (with or without :). */
static int
brace_alternative(
	struct expander *x,
	const struct brace *brace,
	const struct reader *reader,
	struct xbuf *out)
{
	int use;

	/* The word, when the operator uses it. */
	use = brace_uses_word(x, brace);
	if (use)
		return brace_word(x, brace, reader, out);

	/* + with the parameter unset is nothing (an empty field, quoted). */
	if (brace->op[0] == '+') {
		if (reader->quoted)
			append_mark(out, X_KEEP);
		return 1;
	}

	/* Succeeded: - with the parameter set is its value. */
	return brace_value(x, brace, reader->quoted, out);
}

/* Expands ${name=word}: assigns the word when the parameter is unset. */
static int
brace_assign(
	struct expander *x,
	const struct brace *brace,
	const struct reader *reader,
	struct xbuf *out)
{
	char name[NAME_MAX_LENGTH];
	char *string;
	int assigned;
	int variable;
	int use;
	int ok;

	/* A set parameter is its value. */
	use = brace_uses_word(x, brace);
	if (!use)
		return brace_value(x, brace, reader->quoted, out);

	/* Only a variable can be assigned. */
	variable = name_start(brace->name[0]);
	if (brace->special || !variable) {
		(void)snprintf(expand_message, sizeof(expand_message),
			       "%.*s: cannot assign in this way",
			       (int)brace->name_length, brace->name);
		x->error = expand_message;
		sh_expand_fatal = 1;
		return 0;
	}

	/* The word, as one string. */
	ok = brace_word_string(x, brace, reader, &string);
	if (!ok)
		return 0;

	/* Assigned. */
	(void)snprintf(name, sizeof(name), "%.*s", (int)brace->name_length,
		       brace->name);
	assigned = 0;
	if (x->context->assign != NULL)
		assigned = x->context->assign(x->context->lookup_context, name, string);
	if (!assigned) {
		free(string);
		x->error = "cannot assign";
		sh_expand_fatal = 1;
		return 0;
	}

	/* Succeeded: the value assigned. */
	if (reader->quoted)
		append_mark(out, X_KEEP);
	append_value(out, string, reader->quoted);
	free(string);
	return 1;
}

/*
 * Expands ${name?word}: an unset (or null) parameter is a fault with the
 * word as its message (and 0 is returned); a set one is its value.
 */
static int
brace_error(
	struct expander *x,
	const struct brace *brace,
	const struct reader *reader,
	struct xbuf *out)
{
	const char *message;
	char *string;
	int use;
	int ok;

	/* A set parameter is its value. */
	use = brace_uses_word(x, brace);
	if (!use)
		return brace_value(x, brace, reader->quoted, out);

	/* The message: the word, or what is wrong. */
	string = NULL;
	if (brace->word_length > 0) {
		ok = brace_word_string(x, brace, reader, &string);
		if (!ok)
			return 0;
	}

	/* The message is the word, or a default one. */
	message = string;
	if (message == NULL && brace->set)
		message = "parameter null";
	if (message == NULL)
		message = "parameter not set";

	/* The fault. */
	(void)snprintf(expand_message, sizeof(expand_message), "%.*s: %s",
		       (int)brace->name_length, brace->name, message);
	free(string);
	x->error = expand_message;
	sh_expand_fatal = 1;
	return 0;
}

/*
 * Expands ${name%word} and the rest: the value without the prefix or suffix
 * the pattern matches.
 */
static int
brace_trim(
	struct expander *x,
	const struct brace *brace,
	int quoted,
	struct xbuf *out)
{
	struct xbuf pattern;
	struct xbuf joined;
	char *string;
	int ok;

	/* An unset parameter is a fault under set -u. */
	if (!brace->set && !brace->special && x->context->unset_is_error)
		return unset_error(x, brace->name, brace->name_length);

	/*
	 * The pattern is read as if it stood on its own: quotes in it quote
	 * even inside a double quotation.
	 */
	memset(&pattern, 0, sizeof(pattern));
	ok = expand_text(x, brace->word, brace->word_length, 0, 0, 0,
			 &pattern);
	if (!ok) {
		buffer_free(&pattern);
		return 0;
	}

	/* The value; for @ and *, "$*" as one string. */
	if (brace->special) {
		memset(&joined, 0, sizeof(joined));
		append_positionals(x, '*', 1, &joined);
		string = buffer_string(&joined, NULL);
		buffer_free(&joined);
		trim(string, &pattern, brace->op, quoted, out);
		free(string);
	} else {
		trim(brace->value, &pattern, brace->op, quoted, out);
	}

	/* The pattern is no longer needed. */
	buffer_free(&pattern);

	/* Succeeded. */
	return 1;
}

/* Expands a ${...} to the parameter's own value. */
static int
brace_value(
	struct expander *x,
	const struct brace *brace,
	int quoted,
	struct xbuf *out)
{
	/* An unset parameter with no operator is a fault under set -u. */
	if (brace->op[0] == '\0' && !brace->set && !brace->special &&
	    x->context->unset_is_error)
		return unset_error(x, brace->name, brace->name_length);

	/* @ and *. */
	if (brace->special) {
		append_positionals(x, brace->name[0], quoted, out);
		return 1;
	}

	/* Succeeded: the value; quoted, the field exists. */
	if (quoted)
		append_mark(out, X_KEEP);
	append_value(out, brace->value, quoted);
	return 1;
}

/* Expands the word of ${...} quoted, as one string, which the caller frees. */
static int
brace_word_string(
	struct expander *x,
	const struct brace *brace,
	const struct reader *reader,
	char **string)
{
	struct xbuf word;
	int mode;
	int ok;

	/* Quoted; inside double quotes, \} is }. */
	mode = 0;
	if (reader->in_double)
		mode = M_BRACE;
	memset(&word, 0, sizeof(word));
	ok = expand_text(x, brace->word, brace->word_length, reader->in_double,
			 1, mode, &word);
	if (!ok) {
		buffer_free(&word);
		return 0;
	}

	/* Succeeded. */
	*string = buffer_string(&word, NULL);
	buffer_free(&word);
	return 1;
}

/*
 * Removes the shortest or longest prefix (#, ##) or suffix (%, %%) that the
 * pattern matches from value, and adds the rest to out.
 */
static void
trim(
	const char *value,
	const struct xbuf *pattern,
	const char *op,
	int quoted,
	struct xbuf *out)
{
	unsigned char *marks;
	char *text;
	char *candidate;
	char *rest;
	size_t length;
	size_t start;
	size_t end;
	int longest;

	/* The pattern and its marks, and room for a part of the value. */
	text = buffer_string(pattern, &marks);
	length = strlen(value);
	candidate = sh_malloc(length + 1U);
	longest = 0;
	if (op[1] != '\0')
		longest = 1;

	/* What is kept. */
	start = 0;
	end = length;
	if (op[0] == '#')
		start = trim_prefix(value, length, text, marks, longest, candidate);
	else
		end = trim_suffix(value, length, text, marks, longest, candidate);
	free(candidate);
	free(text);
	free(marks);

	/* The rest; quoted, the field exists. */
	if (quoted)
		append_mark(out, X_KEEP);
	rest = sh_strndup(value + start, end - start);
	append_value(out, rest, quoted);
	free(rest);
}

/*
 * Returns the length of the prefix to remove: the shortest one the pattern
 * matches (trying lengths upwards), or the longest (downwards), or 0.
 */
static size_t
trim_prefix(
	const char *value,
	size_t length,
	const char *text,
	const unsigned char *marks,
	int longest,
	char *candidate)
{
	size_t cut;
	size_t size;
	int matched;

	/* Each length in turn. */
	for (cut = 0; cut <= length; cut++) {
		size = cut;
		if (longest)
			size = length - cut;
		memcpy(candidate, value, size);
		candidate[size] = '\0';
		matched = sh_glob_match(text, marks, candidate);
		if (matched)
			return size;
	}

	/* No prefix matches. */
	return 0;
}

/*
 * Returns where the suffix to remove starts: the shortest one the pattern
 * matches (trying the latest start first), or the longest (the earliest),
 * or the end of the value.
 */
static size_t
trim_suffix(
	const char *value,
	size_t length,
	const char *text,
	const unsigned char *marks,
	int longest,
	char *candidate)
{
	size_t cut;
	size_t from;
	int matched;

	/* Each start in turn. */
	for (cut = 0; cut <= length; cut++) {
		from = length - cut;
		if (longest)
			from = cut;
		memcpy(candidate, value + from, length - from);
		candidate[length - from] = '\0';
		matched = sh_glob_match(text, marks, candidate);
		if (matched)
			return from;
	}

	/* No suffix matches. */
	return length;
}

/*
 * Expands a tilde prefix: ~ is $HOME, ~name the home of the user.  A prefix
 * with anything quoted in it is not one, and stays as written.
 */
static void
expand_tilde(
	struct expander *x,
	struct reader *reader,
	struct xbuf *out)
{
	const char *home;
	char *user;
	size_t start;
	size_t end;
	size_t index;
	int plain;

	/* The prefix: up to a slash (or a colon, in an assignment). */
	start = reader->position;
	end = tilde_end(x, reader, &plain);
	if (plain) {
		reader->position++;
		append_char(out, '~', 0);
		return;
	}

	/* The home directory of the user it names. */
	user = sh_strndup(reader->text + start + 1, end - start - 1U);
	home = home_directory(x, user);
	free(user);
	reader->position = end;

	/* An unknown user leaves the prefix as written. */
	if (home == NULL) {
		for (index = start; index < end; index++)
			append_char(out, reader->text[index], 0);
		return;
	}

	/* The directory, quoted. */
	append_mark(out, X_KEEP);
	append_value(out, home, 1);
}

/*
 * Returns where a tilde prefix ends.  *plain is set when it is not one (a
 * quoting or expansion character in it, or a name too long), and the tilde
 * is then an ordinary character.
 */
static size_t
tilde_end(
	const struct expander *x,
	const struct reader *reader,
	int *plain)
{
	size_t end;
	char value;

	/* The prefix runs to a slash, or a colon in an assignment; it is plain until a quote is seen. */
	*plain = 0;
	for (end = reader->position + 1; end < reader->length; end++) {
		/* A slash ends it; so does a colon in an assignment. */
		value = reader->text[end];
		if (value == '/')
			break;
		if (value == ':' && x->assignment)
			break;

		/* A quote or an expansion makes it no prefix. */
		switch (value) {
		case '\\':
		case '\'':
		case '"':
		case '$':
		case '`':
			*plain = 1;
			return end;
		default:
			break;
		}
	}

	/* A user name longer than any is no prefix either. */
	if (end - reader->position - 1U >= NAME_MAX_LENGTH)
		*plain = 1;

	/* Succeeded: the end. */
	return end;
}

/* Returns the home directory of a user, or $HOME for "", or NULL. */
static const char *
home_directory(
	struct expander *x,
	const char *user)
{
	struct passwd *entry;

	/* ~ alone is $HOME. */
	if (user[0] == '\0')
		return lookup(x, "HOME");

	/* ~name is the user's directory. */
	entry = getpwnam(user);
	if (entry == NULL)
		return NULL;

	/* Succeeded. */
	return entry->pw_dir;
}

/*
 * Reports whether $( ... ) at start, ending before end, is an arithmetic
 * expansion: it opens with "((", closes with "))", and the parentheses
 * between them balance.  $((a);(b)) is a command that begins with a subshell.
 */
static int
arithmetic_form(
	const char *start,
	const char *end)
{
	const char *check;
	int depth;

	/* $(( and )). */
	if (end - start < 5)
		return 0;
	if (start[2] != '(' || end[-1] != ')' || end[-2] != ')')
		return 0;

	/* The parentheses between them never close more than they open. */
	depth = 0;
	for (check = start + 3; check < end - 2; check++) {
		if (*check == '(')
			depth++;
		if (*check == ')')
			depth--;
		if (depth < 0)
			return 0;
	}

	/* Succeeded: whether they balance. */
	if (depth != 0)
		return 0;
	return 1;
}

/* Runs a command substitution and adds its output. */
static int
command_output(
	struct expander *x,
	const char *source,
	int quoted,
	struct xbuf *out)
{
	char *output;
	int ran;

	/* The command, run by the shell. */
	output = NULL;
	ran = 0;
	if (x->context->command_substitute != NULL)
		ran = x->context->command_substitute(x->context->lookup_context, source, &output);
	if (!ran) {
		free(output);
		x->error = "command substitution failed";
		return 0;
	}

	/* Succeeded: its output; quoted, the field exists. */
	if (quoted)
		append_mark(out, X_KEEP);
	append_value(out, output, quoted);
	free(output);
	return 1;
}

/* Expands and evaluates the inside of $((...)). */
static int
arithmetic(
	struct expander *x,
	const char *text,
	size_t length,
	int quoted,
	struct xbuf *out)
{
	struct xbuf expression;
	const char *error_text;
	char *string;
	char number[32];
	long long value;
	int ok;

	/* The expansions in it, as in a here-document. */
	memset(&expression, 0, sizeof(expression));
	ok = expand_text(x, text, length, 1, 1, M_HEREDOC, &expression);
	if (!ok) {
		buffer_free(&expression);
		return 0;
	}

	/* The value of the expression. */
	string = buffer_string(&expression, NULL);
	buffer_free(&expression);

	/* The expression; a fault stops the shell. */
	ok = sh_arithmetic_eval(string, x->context->lookup, x->context->assign,
				x->context->lookup_context, &value,
				&error_text);
	free(string);
	if (!ok) {
		x->error = error_text;
		sh_expand_fatal = 1;
		return 0;
	}

	/* Succeeded: the number; quoted, the field exists. */
	(void)snprintf(number, sizeof(number), "%lld", value);
	if (quoted)
		append_mark(out, X_KEEP);
	append_value(out, number, quoted);
	return 1;
}

/*
 * Reads a parameter: a name, a positional number or a special parameter
 * other than @ and *.  *set is cleared for one that is not set, whose value
 * is then "".
 */
static int
parameter(
	struct expander *x,
	const char *name,
	size_t length,
	const char **value,
	int *set)
{
	char copy[NAME_MAX_LENGTH];
	int special;

	/* Empty and set until the parameter says otherwise. */
	*value = "";
	*set = 1;

	/* # ? $ ! -. */
	special = is_special(name[0]);
	if (length == 1 && special) {
		special_parameter(x, name[0], value, set);
		return 1;
	}

	/* A positional parameter, or $0. */
	if (name[0] >= '0' && name[0] <= '9') {
		positional_parameter(x, name, length, value, set);
		return 1;
	}

	/* A variable. */
	if (length >= sizeof(copy)) {
		x->error = "parameter name too long";
		return 0;
	}

	/* The name, with its terminator. */
	memcpy(copy, name, length);
	copy[length] = '\0';
	*value = lookup(x, copy);
	if (*value == NULL) {
		*set = 0;
		*value = "";
	}

	/* Succeeded. */
	return 1;
}

/* Reads $#, $?, $$, $! or $-. */
static void
special_parameter(
	struct expander *x,
	char name,
	const char **value,
	int *set)
{
	static char number[32];
	const struct sh_expand_context *context;

	/* Dispatches on the character. */
	context = x->context;
	switch (name) {
	case '#':
		(void)snprintf(number, sizeof(number), "%d",
			       context->positional_count);
		break;
	case '?':
		(void)snprintf(number, sizeof(number), "%d", context->status);
		break;
	case '$':
		(void)snprintf(number, sizeof(number), "%ld",
			       context->shell_pid);
		break;
	case '!':
		/* No background job yet: unset. */
		if (context->last_job <= 0) {
			*set = 0;
			return;
		}

		/* The process ID of the last background job. */
		(void)snprintf(number, sizeof(number), "%ld",
			       context->last_job);
		break;
	default:
		/* $-: the letters of the options. */
		if (context->options != NULL)
			*value = context->options;
		return;
	}

	/* The number. */
	*value = number;
}

/*
 * Reads $0 or a positional parameter, whose number is the length digits at
 * name; one past the last is unset.
 */
static void
positional_parameter(
	struct expander *x,
	const char *name,
	size_t length,
	const char **value,
	int *set)
{
	const struct sh_expand_context *context;
	size_t digit;
	long index;

	/* The number (one too large for any list is past the last). */
	context = x->context;
	index = 0;
	for (digit = 0; digit < length; digit++) {
		index = index * 10 + (name[digit] - '0');
		if (index > context->positional_count)
			break;
	}

	/* $0 is the name of the shell or the script. */
	if (index == 0) {
		*value = "sh";
		if (context->shell_name != NULL)
			*value = context->shell_name;
		return;
	}

	/* One past the last is unset. */
	if (index > context->positional_count) {
		*set = 0;
		return;
	}

	/* The parameter. */
	*value = context->positional[index - 1];
}

/* Reports an unset parameter under set -u; the shell stops.  Returns 0. */
static int
unset_error(
	struct expander *x,
	const char *name,
	size_t length)
{
	/* The message, which outlives the call. */
	(void)snprintf(expand_message, sizeof(expand_message),
		       "%.*s: parameter not set", (int)length, name);
	x->error = expand_message;
	sh_expand_fatal = 1;

	/* A fault. */
	return 0;
}

/* Adds a value: quoted, or split at IFS later. */
static void
append_value(
	struct xbuf *out,
	const char *value,
	int quoted)
{
	unsigned char attr;

	/* Each character, with the attribute. */
	attr = X_SPLIT;
	if (quoted)
		attr = X_QUOTED;
	for (; *value != '\0'; value++)
		append_char(out, *value, attr);
}

/*
 * Adds the positional parameters.  "$@" makes one field of each; "$*" makes
 * one field joined by the first character of IFS; unquoted, both make one
 * field of each, to be split further.
 */
static void
append_positionals(
	struct expander *x,
	char which,
	int quoted,
	struct xbuf *out)
{
	const char *ifs;
	int index;

	/* "$*": one field, joined. */
	if (quoted && which == '*') {
		ifs = ifs_value(x);
		append_mark(out, X_KEEP);
		for (index = 0; index < x->context->positional_count; index++) {
			if (index > 0 && ifs[0] != '\0')
				append_char(out, ifs[0], X_QUOTED);
			append_value(out, x->context->positional[index], 1);
		}

		/* The parameters are written. */
		return;
	}

	/* Otherwise a field each, with a boundary between them. */
	for (index = 0; index < x->context->positional_count; index++) {
		if (index > 0)
			append_mark(out, X_BREAK);
		if (quoted)
			append_mark(out, X_KEEP);
		append_value(out, x->context->positional[index], quoted);
	}
}

/*
 * Splits the buffer into fields (POSIX XCU 2.6.5).  IFS white space around
 * a field is dropped and a run of it is one separator; any other IFS
 * character is a separator of its own, with the white space beside it, so
 * two of them in a row have an empty field between them.
 */
static void
split_fields(
	struct expander *x,
	const struct xbuf *in,
	struct sh_field_list *fields)
{
	struct xbuf field;
	const char *ifs;
	size_t index;
	size_t scan;
	int have;
	int class;
	int white;

	/* Walks the expanded text, cutting fields at IFS characters that no quote protects. */
	memset(&field, 0, sizeof(field));
	ifs = ifs_value(x);
	have = 0;
	index = 0;
	while (index < in->length) {
		/* A boundary of "$@" ends a field. */
		if ((in->attr[index] & X_BREAK) != 0) {
			if (have)
				field_add(fields, &field);
			field.length = 0;
			have = 0;
			index++;
			continue;
		}

		/* A mark that the field exists. */
		if ((in->attr[index] & X_KEEP) != 0) {
			have = 1;
			index++;
			continue;
		}

		/* A character of the field. */
		class = split_class(ifs, in, index);
		if (class == SPLIT_NONE) {
			append_char(&field, in->data[index], in->attr[index]);
			have = 1;
			index++;
			continue;
		}

		/*
		 * A separator: white space, then at most one other IFS
		 * character and the white space after it.
		 */
		scan = skip_split_white(ifs, in, index);
		white = 1;
		class = split_class(ifs, in, scan);
		if (class == SPLIT_OTHER) {
			white = 0;
			scan = skip_split_white(ifs, in, scan + 1U);
		}

		/* It ends a field; one of white space alone needs a field. */
		if (have || !white)
			field_add(fields, &field);
		field.length = 0;
		have = 0;
		index = scan;
	}

	/* The last field. */
	if (have)
		field_add(fields, &field);
	buffer_free(&field);
}

/* Classifies an entry of the buffer for field splitting. */
static int
split_class(
	const char *ifs,
	const struct xbuf *in,
	size_t index)
{
	const char *found;
	char value;

	/* Only characters of unquoted expansions are split. */
	if (index >= in->length)
		return SPLIT_NONE;
	if ((in->attr[index] & X_SPLIT) == 0)
		return SPLIT_NONE;

	/* At the characters of IFS. */
	value = in->data[index];
	if (value == '\0' || ifs[0] == '\0')
		return SPLIT_NONE;
	found = strchr(ifs, value);
	if (found == NULL)
		return SPLIT_NONE;

	/* White space, or another separator. */
	if (value == ' ' || value == '\t' || value == '\n')
		return SPLIT_WHITE;
	return SPLIT_OTHER;
}

/* Returns the index after a run of IFS white space. */
static size_t
skip_split_white(
	const char *ifs,
	const struct xbuf *in,
	size_t index)
{
	int class;

	/* Each one. */
	for (;;) {
		class = split_class(ifs, in, index);
		if (class != SPLIT_WHITE)
			break;
		index++;
	}

	/* Succeeded: the first other entry. */
	return index;
}

/* Adds the characters of a buffer as a field. */
static void
field_add(
	struct sh_field_list *fields,
	const struct xbuf *in)
{
	unsigned char *quoted;
	char *text;
	size_t count;
	size_t index;

	/* Room for one more. */
	count = fields->count + 1U;
	fields->fields = sh_realloc(fields->fields,
				    count * sizeof(*fields->fields));
	fields->quoted = sh_realloc(fields->quoted,
				    count * sizeof(*fields->quoted));

	/* The characters, and which of them were quoted. */
	text = sh_malloc(in->length + 1U);
	quoted = sh_malloc(in->length + 1U);
	for (index = 0; index < in->length; index++) {
		text[index] = in->data[index];
		quoted[index] = 0;
		if ((in->attr[index] & X_QUOTED) != 0)
			quoted[index] = 1;
	}

	/* The copies end with a null. */
	text[in->length] = '\0';
	quoted[in->length] = 0;

	/* The field. */
	fields->fields[fields->count] = text;
	fields->quoted[fields->count] = quoted;
	fields->count = count;
}

/*
 * Makes one string of a buffer without splitting: the fields of "$@" are
 * joined by a space.  quoted, when given, receives the marks; the caller
 * frees both.
 */
static char *
buffer_string(
	const struct xbuf *in,
	unsigned char **quoted)
{
	unsigned char *marks;
	char *text;
	size_t index;
	size_t used;
	int first;

	/* Copies the text without its marks, a space for each "$@" boundary. */
	text = sh_malloc(in->length + 1U);
	marks = sh_malloc(in->length + 1U);
	used = 0;
	first = 1;
	for (index = 0; index < in->length; index++) {
		/* A mark that the field exists adds nothing. */
		if ((in->attr[index] & X_KEEP) != 0)
			continue;

		/* A boundary of "$@" is a space (after the first field). */
		if ((in->attr[index] & X_BREAK) != 0) {
			if (!first) {
				text[used] = ' ';
				marks[used] = 1;
				used++;
			}

			continue;
		}

		/* A character. */
		first = 0;
		text[used] = in->data[index];
		marks[used] = 0;
		if ((in->attr[index] & X_QUOTED) != 0)
			marks[used] = 1;
		used++;
	}

	/* The copies end with a null. */
	text[used] = '\0';
	marks[used] = 0;

	/* Succeeded: the string, and the marks when they were asked for. */
	if (quoted != NULL)
		*quoted = marks;
	else
		free(marks);
	return text;
}

/* Adds one character. */
static void
append_char(
	struct xbuf *out,
	char value,
	unsigned char attr)
{
	size_t capacity;

	/* Room for it. */
	if (out->length == out->capacity) {
		capacity = 64;
		if (out->capacity != 0)
			capacity = out->capacity * 2U;
		out->data = sh_realloc(out->data, capacity);
		out->attr = sh_realloc(out->attr, capacity);
		out->capacity = capacity;
	}

	/* The character. */
	out->data[out->length] = value;
	out->attr[out->length] = attr;
	out->length++;
}

/* Adds a mark that is not a character. */
static void
append_mark(
	struct xbuf *out,
	unsigned char mark)
{
	/* An entry with no character. */
	append_char(out, '\0', mark);
}

/* Frees a buffer. */
static void
buffer_free(
	struct xbuf *buffer)
{
	/* The characters and the attributes. */
	free(buffer->data);
	free(buffer->attr);
	memset(buffer, 0, sizeof(*buffer));
}

/*
 * Reports whether $@ or $* joined is empty: no parameters, or empty ones
 * joined by nothing (IFS empty) or only one.
 */
static int
positional_null(
	struct expander *x)
{
	const char *ifs;
	int index;

	/* Any non-empty parameter makes it non-null. */
	for (index = 0; index < x->context->positional_count; index++) {
		if (x->context->positional[index][0] != '\0')
			return 0;
	}

	/* One parameter, or none, is null; more make at least separators. */
	if (x->context->positional_count <= 1)
		return 1;

	/* Several empty ones are joined by the first character of IFS. */
	ifs = ifs_value(x);
	if (ifs[0] != '\0')
		return 0;

	/* Succeeded: null, with no separator between them. */
	return 1;
}

/* Reads a variable: from the shell, or from the environment. */
static const char *
lookup(
	struct expander *x,
	const char *name)
{
	/* The shell's lookup. */
	if (x->context->lookup != NULL)
		return x->context->lookup(x->context->lookup_context, name);

	/* Succeeded: the environment. */
	return getenv(name);
}

/* Reports the separators: IFS, or blank, tab and newline when it is unset. */
static const char *
ifs_value(
	struct expander *x)
{
	const char *ifs;

	/* IFS. */
	ifs = lookup(x, "IFS");
	if (ifs == NULL)
		return " \t\n";

	/* Succeeded. */
	return ifs;
}

/* Reports whether a word is an assignment: an unquoted name and =. */
static int
assignment_prefix(
	const struct sh_token *token)
{
	size_t index;
	int start;
	int name;

	/* An unquoted name start. */
	if (token->text == NULL || token->length == 0 || token->quote == NULL)
		return 0;
	start = name_start(token->text[0]);
	if (!start || token->quote[0] != SH_QUOTE_UNQUOTED)
		return 0;

	/* Unquoted name characters up to an =. */
	for (index = 1; index < token->length; index++) {
		if (token->quote[index] != SH_QUOTE_UNQUOTED)
			return 0;
		if (token->text[index] == '=')
			return 1;
		name = name_char(token->text[index]);
		if (!name)
			return 0;
	}

	/* No equals sign. */
	return 0;
}

/*
 * Returns the length of the parameter name at text: a name, a run of
 * digits, or one special character (@ * # ? - $ !).  0 when it is none.
 */
static size_t
scan_name(
	const char *text,
	size_t length)
{
	size_t name_length;
	int start;
	int special;

	/* Nothing is no parameter. */
	if (length == 0)
		return 0;

	/* A name. */
	start = name_start(text[0]);
	if (start) {
		for (name_length = 1; name_length < length; name_length++) {
			start = name_char(text[name_length]);
			if (!start)
				break;
		}

		/* Succeeded: the length of the name. */
		return name_length;
	}

	/* A run of digits. */
	if (text[0] >= '0' && text[0] <= '9') {
		name_length = 1;
		while (name_length < length && text[name_length] >= '0' &&
		       text[name_length] <= '9')
			name_length++;
		return name_length;
	}

	/* One special character. */
	special = is_special(text[0]);
	if (special || text[0] == '@' || text[0] == '*')
		return 1;

	/* None. */
	return 0;
}

/* Reports whether a character is a special parameter # ? - $ !. */
static int
is_special(
	char value)
{
	/* The five. */
	switch (value) {
	case '#':
	case '?':
	case '-':
	case '$':
	case '!':
		return 1;
	default:
		break;
	}

	/* Anything else. */
	return 0;
}

/* Reports whether a character may begin a name. */
static int
name_start(
	char value)
{
	/* A letter or _. */
	if (value >= 'a' && value <= 'z')
		return 1;
	if (value >= 'A' && value <= 'Z')
		return 1;
	if (value == '_')
		return 1;
	return 0;
}

/* Reports whether a character may continue a name. */
static int
name_char(
	char value)
{
	int start;

	/* A digit, or what may begin one. */
	if (value >= '0' && value <= '9')
		return 1;
	start = name_start(value);
	return start;
}

/* Returns the message of a failed expansion. */
static const char *
failure(
	const struct expander *x)
{
	/* The fault recorded, or a general one. */
	if (x->error != NULL)
		return x->error;
	return "bad expansion";
}
