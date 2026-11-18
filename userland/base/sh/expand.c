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

#include <ctype.h>
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
	int indirect;		/* ${!name}: the parameter named by the value (bash) */
	/*
	 * "", "-", "=", "?", "+", "%", "%%", "#", "##"; and bash's ":" (a
	 * substring), "/" "//" "/#" "/%" (a replacement), "^" "^^" "," ",,"
	 * (a change of case), which POSIX calls bad substitutions.
	 */
	char op[3];
	int colon;		/* the operator had a : before it */
	const char *word;
	size_t word_length;

	/* The value, and whether the parameter is set. */
	const char *value;
	int set;
};

/*
 * The bounds of ${name:offset:length} (bash), evaluated as arithmetic
 * expressions: the offset (from the end when negative), and the length
 * when there is one (up to that many from the end when negative).
 */
struct substring {
	long offset;
	long length;
	int has_length;
};

/*
 * Where ${name/pattern/string} looks for the longest match: the value,
 * the operator and the pattern with its quoted characters marked, the
 * place in the value, and the room a candidate is copied into.  It lives
 * for one replace_pattern.
 */
struct pattern_place {
	const char *value;
	size_t length;
	const char *op;
	const char *text;
	const unsigned char *marks;
	char *candidate;
	size_t start;
};

/* The message of the last fault, which outlives the call. */
static char expand_message[512];

int sh_expand_fatal;

static int expand_process(struct expander *x, const struct sh_token *token, struct xbuf *out);
static size_t character_count(struct expander *x, const char *value);
static int locale_is_utf8(struct expander *x);
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
static void append_list(struct expander *x, char which, const char *const *values, int count, int quoted, struct xbuf *out);
static int brace_is_indirect(const char *text, size_t length);
static int codeset_is_utf8(const char *name);
static int utf8_at(const char *text);
static int indirect_parameter(struct expander *x, struct brace *brace);
static int brace_substring(struct expander *x, const struct brace *brace, int quoted, struct xbuf *out);
static int substring_positionals(struct expander *x, const struct brace *brace, const struct substring *bounds, int quoted, struct xbuf *out);
static int substring_value(struct expander *x, const struct brace *brace, const struct substring *bounds, int quoted, struct xbuf *out);
static int substring_bounds(struct expander *x, const struct brace *brace, struct substring *bounds);
static size_t substring_split(const char *word, size_t length);
static int arithmetic_part(struct expander *x, const char *text, size_t length, long *value);
static int brace_transform(struct expander *x, const struct brace *brace, int quoted, struct xbuf *out);
static int transform_words(struct expander *x, const struct brace *brace, struct xbuf *pattern, struct xbuf *replacement);
static void transform_positionals(struct expander *x, const struct brace *brace, const struct xbuf *pattern, const struct xbuf *replacement, int quoted, struct xbuf *out);
static char *transform_one(const struct brace *brace, const char *value, const struct xbuf *pattern, const struct xbuf *replacement);
static char *replace_pattern(const char *value, const char *op, const char *text, const unsigned char *marks, const char *with, const unsigned char *with_marks);
static int longest_match(const struct pattern_place *place, size_t *match);
static void append_replacement(struct xbuf *out, const char *with, const unsigned char *with_marks, const char *matched);
static char *change_case(const char *value, const char *op, const char *text, const unsigned char *marks);
static char changed_case(char value, int upper);
static size_t split_replacement(const char *word, size_t length);
static size_t character_offset(struct expander *x, const char *value, size_t characters);
static const char **positional_slice(struct expander *x, const struct substring *bounds, int *count);
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

/*
 * Expands a process substitution: the shell starts the command with a
 * pipe and the word is the /dev/fd name of the shell's end.
 */
static int
expand_process(
	struct expander *x,
	const struct sh_token *token,
	struct xbuf *out)
{
	const char *cursor;
	char *path;
	int ran;

	/* Has the shell start the command with its pipe and name the pipe. */
	path = NULL;
	ran = 0;
	if (x->context->process_substitute != NULL) {
		ran = x->context->process_substitute(x->context->lookup_context,
						     token,
						     &path);
	}

	/* A command that could not be started leaves no word. */
	if (!ran)
		return 0;

	/* Adds the name as one quoted word. */
	for (cursor = path; *cursor != '\0'; cursor++)
		append_char(out, *cursor, X_QUOTED);
	append_mark(out, X_KEEP);
	free(path);

	/* Succeeded: the word is the name of the pipe. */
	return 1;
}

/* Expands a token: a here-document body, or a word from its raw text. */
static int
expand_token(
	struct expander *x,
	const struct sh_token *token,
	struct xbuf *out)
{
	size_t index;
	int ok;

	/* <( ) and >( ) become the name of a pipe to a command (bash). */
	if (token->process != 0) {
		ok = expand_process(x, token, out);
		return ok;
	}

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

	/* ${!name}: the value names the parameter whose value is used. */
	if (brace.indirect) {
		ok = indirect_parameter(x, &brace);
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
	case ':':
		return brace_substring(x, &brace, reader->quoted, out);
	case '/':
	case '^':
	case ',':
		return brace_transform(x, &brace, reader->quoted, out);
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
	int indirect;

	/* Nothing is known about the expansion yet. */
	memset(brace, 0, sizeof(*brace));

	/*
	 * ${!name}, ${!1} and ${!#}: indirection (bash); ${!} and ${!-word}
	 * and the like are $!.
	 */
	if (length > 1 && text[0] == '!') {
		indirect = brace_is_indirect(text, length);
		if (indirect) {
			brace->indirect = 1;
			text++;
			length--;
		}
	}

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

	/* bash's :offset:length takes the rest as its word. */
	if (first == ':') {
		brace->op[0] = ':';
		brace->word++;
		brace->word_length--;
		return 1;
	}

	/* bash's /pattern/string, //, /# and /%. */
	if (first == '/') {
		brace->op[0] = '/';
		brace->word++;
		brace->word_length--;

		/* A second /, # or % says which matches are replaced. */
		if (second == '/' || second == '#' || second == '%') {
			brace->op[1] = second;
			brace->word++;
			brace->word_length--;
		}

		/* Succeeded: a replacement. */
		return 1;
	}

	/* bash's ^ ^^ , ,, (a change of case). */
	if (first == '^' || first == ',') {
		brace->op[0] = first;
		brace->word++;
		brace->word_length--;

		/* A doubled operator changes every character, not the first. */
		if (second == first) {
			brace->op[1] = first;
			brace->word++;
			brace->word_length--;
		}

		/* Succeeded: a change of case. */
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

/*
 * Reports whether the text of ${!...} after its ! names a parameter to
 * take indirectly (bash): a name, a positional parameter, or # alone.
 * ${!} and ${!-word} and the like are $!.
 */
static int
brace_is_indirect(
	const char *text,
	size_t length)
{
	size_t name_length;
	int alphanumeric;

	/* Refuses a ! that no parameter's name follows. */
	name_length = scan_name(text + 1, length - 1U);
	if (name_length == 0)
		return 0;

	/* A name or a digit follows. */
	if (text[1] == '_')
		return 1;
	alphanumeric = isalnum((unsigned char)text[1]);
	if (alphanumeric)
		return 1;

	/* ${!#} names the last positional parameter. */
	if (text[1] == '#' && length == 2)
		return 1;

	/* Anything else is $! and an operator. */
	return 0;
}

/*
 * Reports whether the shell's locale for characters is UTF-8: the first
 * of LC_ALL, LC_CTYPE and LANG that is set names it.
 */
static int
locale_is_utf8(
	struct expander *x)
{
	static const char *const names[] = { "LC_ALL", "LC_CTYPE", "LANG" };
	const char *value;
	size_t index;
	int utf8;

	/* The first of the variables that is set decides. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		value = NULL;
		if (x->context->lookup != NULL) {
			value = x->context->lookup(x->context->lookup_context,
						   names[index]);
		}

		/* A variable that is unset or empty leaves it to the next. */
		if (value == NULL || value[0] == '\0')
			continue;

		/* Reports whether the locale's name has a UTF-8 codeset. */
		utf8 = codeset_is_utf8(value);
		return utf8;
	}

	/* Nothing is set: the POSIX locale, whose characters are bytes. */
	return 0;
}

/* Reports whether a locale's name holds UTF-8 or utf8, in any case. */
static int
codeset_is_utf8(
	const char *name)
{
	const char *cursor;
	int found;

	/* Looks for the codeset at every place of the name. */
	for (cursor = name; *cursor != '\0'; cursor++) {
		found = utf8_at(cursor);
		if (found)
			return 1;
	}

	/* The name has another codeset, or none. */
	return 0;
}

/* Reports whether a text starts with UTF-8 or UTF8, in any case. */
static int
utf8_at(
	const char *text)
{
	/* Refuses anything but U, T and F first, in either case. */
	if (text[0] != 'U' && text[0] != 'u')
		return 0;
	if (text[1] != 'T' && text[1] != 't')
		return 0;
	if (text[2] != 'F' && text[2] != 'f')
		return 0;

	/* UTF8 and UTF-8 are both the codeset. */
	if (text[3] == '8')
		return 1;
	if (text[3] == '-' && text[4] == '8')
		return 1;

	/* Anything else after UTF is not. */
	return 0;
}

/*
 * Counts the characters of a value (XCU 2.6.2, ${#parameter}): bytes,
 * or in a UTF-8 locale the bytes that start a character.
 */
static size_t
character_count(
	struct expander *x,
	const char *value)
{
	const unsigned char *cursor;
	size_t count;
	int utf8;

	/* Outside a UTF-8 locale a character is a byte. */
	utf8 = locale_is_utf8(x);
	if (!utf8) {
		count = strlen(value);
		return count;
	}

	/* Each byte that is not a continuation byte starts a character. */
	count = 0;
	for (cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
		if ((*cursor & 0xc0U) != 0x80U)
			count++;
	}

	/* Succeeded: the number of characters. */
	return count;
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
		count = character_count(x, brace->value);
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
 * Replaces the value of ${!name} with the value of the parameter it
 * names (bash): a name, a positional parameter, or a special one.
 */
static int
indirect_parameter(
	struct expander *x,
	struct brace *brace)
{
	const char *target;
	size_t scanned;
	size_t length;
	int ok;

	/* Refuses @ and *, an empty value, and a value that names nothing. */
	target = brace->value;
	length = strlen(target);
	scanned = 0;
	if (!brace->special)
		scanned = scan_name(target, length);
	if (length == 0 || scanned != length) {
		x->error = "bad substitution";
		return 0;
	}

	/* Takes the value of the parameter named. */
	ok = parameter(x, target, length, &brace->value, &brace->set);
	if (!ok)
		return 0;

	/* Succeeded: the brace holds that parameter's value. */
	return 1;
}

/*
 * Expands ${name:offset} and ${name:offset:length} (bash): characters of
 * the value, or of @ and * the parameters themselves, from offset (from
 * the end when it is negative), length of them (up to that many from the
 * end when it is negative).
 */
static int
brace_substring(
	struct expander *x,
	const struct brace *brace,
	int quoted,
	struct xbuf *out)
{
	struct substring bounds;
	int ok;

	/* An unset parameter is a fault under set -u. */
	if (!brace->set &&
	    !brace->special &&
	    x->context->unset_is_error) {
		ok = unset_error(x, brace->name, brace->name_length);
		return ok;
	}

	/* Evaluates the offset and the length. */
	ok = substring_bounds(x, brace, &bounds);
	if (!ok)
		return 0;

	/* @ and * take the parameters themselves. */
	if (brace->special) {
		ok = substring_positionals(x, brace, &bounds, quoted, out);
		return ok;
	}

	/* Any other parameter takes characters of its value. */
	ok = substring_value(x, brace, &bounds, quoted, out);
	if (!ok)
		return 0;

	/* Succeeded: the characters are added. */
	return 1;
}

/*
 * Adds the parameters ${@:offset:length} or ${*:offset:length} selects,
 * as "$@" or "$*" would add them.
 */
static int
substring_positionals(
	struct expander *x,
	const struct brace *brace,
	const struct substring *bounds,
	int quoted,
	struct xbuf *out)
{
	const char **values;
	int count;

	/* Refuses a negative length, which counts from the end only in a value. */
	if (bounds->has_length && bounds->length < 0) {
		x->error = "substring expression < 0";
		return 0;
	}

	/* Adds the parameters in range. */
	values = positional_slice(x, bounds, &count);
	append_list(x, brace->name[0], values, count, quoted, out);
	free(values);

	/* Succeeded: the parameters are added. */
	return 1;
}

/*
 * Adds the characters of a value that ${name:offset:length} selects,
 * counted in characters in a UTF-8 locale.
 */
static int
substring_value(
	struct expander *x,
	const struct brace *brace,
	const struct substring *bounds,
	int quoted,
	struct xbuf *out)
{
	char *part;
	long characters;
	long offset;
	long end;
	size_t start_byte;
	size_t end_byte;

	/* Counts the characters; a negative offset counts from the end. */
	characters = (long)character_count(x, brace->value);
	offset = bounds->offset;
	if (offset < 0)
		offset += characters;

	/* An offset outside the value selects nothing. */
	if (offset < 0 || offset > characters)
		offset = characters;

	/*
	 * The part ends at the end of the value, length characters on, or, for
	 * a negative length, that many characters before the end.
	 */
	end = characters;
	if (bounds->has_length) {
		if (bounds->length < 0) {
			end = characters + bounds->length;
			if (end < offset) {
				x->error = "substring expression < 0";
				return 0;
			}
		} else if (bounds->length < characters - offset) {
			end = offset + bounds->length;
		}
	}

	/* Copies the characters out. */
	start_byte = character_offset(x, brace->value, (size_t)offset);
	end_byte = character_offset(x, brace->value, (size_t)end);
	part = sh_strndup(brace->value + start_byte, end_byte - start_byte);

	/* Adds them; quoted, the field exists even when it is empty. */
	if (quoted)
		append_mark(out, X_KEEP);
	append_value(out, part, quoted);
	free(part);

	/* Succeeded: the characters are added. */
	return 1;
}

/*
 * Evaluates the offset and the length of ${name:offset:length}, each an
 * arithmetic expression; the length is after the first : that no ? or
 * parenthesis holds.
 */
static int
substring_bounds(
	struct expander *x,
	const struct brace *brace,
	struct substring *bounds)
{
	const char *word;
	size_t split;
	int ok;

	/* Finds where the length begins. */
	word = brace->word;
	split = substring_split(word, brace->word_length);

	/* Evaluates the offset, which must be there. */
	ok = arithmetic_part(x, word, split, &bounds->offset);
	if (!ok)
		return 0;

	/* Evaluates the length, when there is one (an empty one is 0). */
	bounds->has_length = 0;
	bounds->length = 0;
	if (split < brace->word_length) {
		bounds->has_length = 1;
		ok = arithmetic_part(x,
				     word + split + 1,
				     brace->word_length - split - 1U,
				     &bounds->length);
		if (!ok)
			return 0;
	}

	/* Succeeded: the bounds are evaluated. */
	return 1;
}

/*
 * Returns where the : that splits the offset from the length of
 * ${name:offset:length} is, or the length of the word when there is none.
 * A : that closes a ?, or one inside parentheses, does not split.
 */
static size_t
substring_split(
	const char *word,
	size_t length)
{
	size_t index;
	int depth;
	int pending;

	/* Walks the word, keeping count of parentheses and of open ?. */
	depth = 0;
	pending = 0;
	for (index = 0; index < length; index++) {
		/* What parentheses hold belongs to the expression. */
		if (word[index] == '(') {
			depth++;
			continue;
		}

		/* A ) closes the innermost parenthesis. */
		if (word[index] == ')') {
			depth--;
			continue;
		}

		/* Anything else inside parentheses does not split. */
		if (depth != 0)
			continue;

		/* A ? opens a conditional whose : is its own. */
		if (word[index] == '?') {
			pending++;
			continue;
		}

		/* Only a : can split. */
		if (word[index] != ':')
			continue;

		/* A : that closes an open ? belongs to the conditional. */
		if (pending > 0) {
			pending--;
			continue;
		}

		/* The first free : splits the word. */
		return index;
	}

	/* No length. */
	return length;
}

/* Evaluates part of a word as an arithmetic expression; blank is 0. */
static int
arithmetic_part(
	struct expander *x,
	const char *text,
	size_t length,
	long *value)
{
	const char *error_text;
	char *expression;
	size_t index;
	int blank;
	int ok;

	/* Looks for anything but blanks. */
	blank = 1;
	for (index = 0; index < length; index++) {
		if (text[index] != ' ' && text[index] != '\t')
			blank = 0;
	}

	/* A part with nothing but blanks is 0. */
	*value = 0;
	if (blank)
		return 1;

	/* Evaluates the expression, expanded as $(( )) is. */
	expression = sh_strndup(text, length);
	ok = sh_expand_arithmetic(expression, x->context, value, &error_text);
	free(expression);
	if (!ok) {
		x->error = error_text;
		return 0;
	}

	/* Succeeded: the value is stored. */
	return 1;
}

/*
 * Returns the parameters of ${@:offset:length}: $0 is at offset 0 and the
 * positional parameters after it; a negative offset counts back from the
 * end.  The caller frees the array (not the values).
 */
static const char **
positional_slice(
	struct expander *x,
	const struct substring *bounds,
	int *count)
{
	const char **values;
	long offset;
	long total;
	long index;
	long taken;

	/* Makes room for $0 and the positionals. */
	total = (long)x->context->positional_count + 1;
	values = sh_malloc((size_t)total * sizeof(*values));
	*count = 0;

	/* A negative offset counts from the end; one outside selects none. */
	offset = bounds->offset;
	if (offset < 0)
		offset += total;
	if (offset < 0 || offset >= total)
		return values;

	/* Takes the ones in range, $0 first when the offset is 0. */
	taken = total - offset;
	if (bounds->has_length && bounds->length < taken)
		taken = bounds->length;
	for (index = offset; index < offset + taken; index++) {
		if (index == 0)
			values[(*count)++] = x->context->shell_name;
		else
			values[(*count)++] = x->context->positional[index - 1];
	}

	/* Succeeded: the parameters in range. */
	return values;
}

/* Returns the byte offset of a character of a value (UTF-8 aware). */
static size_t
character_offset(
	struct expander *x,
	const char *value,
	size_t characters)
{
	const unsigned char *cursor;
	size_t seen;
	int utf8;

	/* Outside a UTF-8 locale a character is a byte, up to the end. */
	utf8 = locale_is_utf8(x);
	if (!utf8) {
		seen = strlen(value);
		if (characters < seen)
			return characters;
		return seen;
	}

	/* Finds the byte where the character begins. */
	seen = 0;
	for (cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
		if ((*cursor & 0xc0U) != 0x80U) {
			if (seen == characters)
				break;
			seen++;
		}
	}

	/* Succeeded: the byte offset, or the end of the value. */
	return (size_t)(cursor - (const unsigned char *)value);
}

/*
 * Expands ${name/pattern/string} and the like, and ${name^pattern} and
 * the like (bash): each parameter of @ and *, or the value, changed.
 */
static int
brace_transform(
	struct expander *x,
	const struct brace *brace,
	int quoted,
	struct xbuf *out)
{
	struct xbuf pattern;
	struct xbuf replacement;
	char *result;
	int ok;

	/* An unset parameter is a fault under set -u. */
	if (!brace->set &&
	    !brace->special &&
	    x->context->unset_is_error) {
		ok = unset_error(x, brace->name, brace->name_length);
		return ok;
	}

	/* Expands the pattern and, for /, the string. */
	ok = transform_words(x, brace, &pattern, &replacement);
	if (!ok)
		return 0;

	/* @ and * change each parameter; any other parameter its value. */
	if (brace->special) {
		transform_positionals(x, brace, &pattern, &replacement, quoted, out);
	} else {
		result = transform_one(brace, brace->value, &pattern, &replacement);
		if (quoted)
			append_mark(out, X_KEEP);
		append_value(out, result, quoted);
		free(result);
	}

	/* The expanded words were only for the change. */
	buffer_free(&pattern);
	buffer_free(&replacement);

	/* Succeeded: the changed values are added. */
	return 1;
}

/*
 * Expands the pattern of a change and, for /, the string after the / that
 * ends the pattern; both are read as the pattern of a trim is.  Returns 0
 * with both buffers freed when an expansion fails.
 */
static int
transform_words(
	struct expander *x,
	const struct brace *brace,
	struct xbuf *pattern,
	struct xbuf *replacement)
{
	size_t split;
	int ok;

	/* Finds where the pattern ends. */
	memset(pattern, 0, sizeof(*pattern));
	memset(replacement, 0, sizeof(*replacement));
	split = brace->word_length;
	if (brace->op[0] == '/')
		split = split_replacement(brace->word, brace->word_length);

	/* Expands the pattern. */
	ok = expand_text(x, brace->word, split, 0, 0, 0, pattern);
	if (!ok) {
		buffer_free(pattern);
		buffer_free(replacement);
		return 0;
	}

	/* Expands the string, when there is one. */
	if (brace->op[0] == '/' && split < brace->word_length) {
		ok = expand_text(x,
				 brace->word + split + 1,
				 brace->word_length - split - 1U,
				 0,
				 0,
				 0,
				 replacement);
		if (!ok) {
			buffer_free(pattern);
			buffer_free(replacement);
			return 0;
		}
	}

	/* Succeeded: both words are expanded. */
	return 1;
}

/* Adds each positional parameter changed, as "$@" or "$*" would add them. */
static void
transform_positionals(
	struct expander *x,
	const struct brace *brace,
	const struct xbuf *pattern,
	const struct xbuf *replacement,
	int quoted,
	struct xbuf *out)
{
	const char **values;
	char **changed;
	size_t size;
	int count;
	int index;

	/* Allocates the changed values and the list that adds them. */
	count = x->context->positional_count;
	size = (size_t)count + 1U;
	values = sh_malloc(size * sizeof(*values));
	changed = sh_malloc(size * sizeof(*changed));

	/* Changes each parameter. */
	for (index = 0; index < count; index++) {
		changed[index] = transform_one(brace,
					       x->context->positional[index],
					       pattern,
					       replacement);
		values[index] = changed[index];
	}

	/* Adds them as a list. */
	append_list(x, brace->name[0], values, count, quoted, out);

	/* Frees the changed values and the list. */
	for (index = 0; index < count; index++)
		free(changed[index]);
	free(changed);
	free(values);
}

/* Changes one value by the operator of a brace; the caller frees it. */
static char *
transform_one(
	const struct brace *brace,
	const char *value,
	const struct xbuf *pattern,
	const struct xbuf *replacement)
{
	unsigned char *marks;
	unsigned char *with_marks;
	char *text;
	char *with;
	char *result;

	/* Takes the pattern and the string, with their quoted characters marked. */
	text = buffer_string(pattern, &marks);
	with = buffer_string(replacement, &with_marks);

	/* / replaces a match; ^ and , change the case. */
	if (brace->op[0] == '/') {
		result = replace_pattern(value,
					 brace->op,
					 text,
					 marks,
					 with,
					 with_marks);
	} else {
		result = change_case(value, brace->op, text, marks);
	}

	/* The pattern and the string were only for this value. */
	free(text);
	free(marks);
	free(with);
	free(with_marks);

	/* Succeeded: the changed value. */
	return result;
}

/*
 * Replaces the longest match of a pattern: the first (/), every one (//),
 * one at the start (/#) or at the end (/%).  An & of the string that no
 * quote holds stands for the match (bash's patsub_replacement).
 */
static char *
replace_pattern(
	const char *value,
	const char *op,
	const char *text,
	const unsigned char *marks,
	const char *with,
	const unsigned char *with_marks)
{
	struct pattern_place place;
	struct xbuf out;
	char *candidate;
	char *result;
	size_t length;
	size_t match;
	int found;

	/* Allocates the room each candidate match is copied into. */
	length = strlen(value);
	memset(&out, 0, sizeof(out));
	candidate = sh_malloc(length + 1U);

	/* Describes where matches are looked for. */
	place.value = value;
	place.length = length;
	place.op = op;
	place.text = text;
	place.marks = marks;
	place.candidate = candidate;

	/* Walks the value, replacing matches and keeping the rest. */
	place.start = 0;
	while (place.start <= length) {
		found = longest_match(&place, &match);
		if (found) {
			/* Adds the string in place of the match. */
			memcpy(candidate, value + place.start, match);
			candidate[match] = '\0';
			append_replacement(&out, with, with_marks, candidate);
			place.start += match;

			/* Only // goes on: the rest of the value stays as it is. */
			if (op[1] != '/') {
				append_value(&out, value + place.start, 1);
				break;
			}

			/* After an empty match, the character here is kept first. */
			if (match > 0)
				continue;
		}

		/* The character here stays. */
		if (place.start < length)
			append_char(&out, value[place.start], X_QUOTED);
		place.start++;
	}

	/* The candidates were only for the matching. */
	free(candidate);

	/* Takes the changed value out of the buffer. */
	result = buffer_string(&out, NULL);
	buffer_free(&out);

	/* Succeeded: the changed value. */
	return result;
}

/*
 * Finds the longest match of the pattern at a place of the value (only at
 * the start for /#, only reaching the end for /%).  Returns 1 with its
 * length, or 0 when there is none.  An empty pattern matches only at an
 * anchor.
 */
static int
longest_match(
	const struct pattern_place *place,
	size_t *match)
{
	size_t size;
	int anchored;
	int matched;

	/* /# matches only at the start. */
	if (place->op[1] == '#' && place->start > 0)
		return 0;

	/* An empty pattern matches nothing, except at an anchor (/# and /%). */
	anchored = 0;
	if (place->op[1] == '#' || place->op[1] == '%')
		anchored = 1;
	if (place->text[0] == '\0' && !anchored)
		return 0;

	/* Tries the longest candidate first; the size counts its NUL. */
	for (size = place->length - place->start + 1U; size > 0; size--) {
		/* /% takes only a candidate that reaches the end. */
		if (place->op[1] == '%' &&
		    place->start + size - 1U != place->length)
			continue;

		/* Outside an anchor the pattern never matches an empty candidate. */
		if (size == 1U && !anchored)
			break;

		/* Matches the candidate of this size. */
		memcpy(place->candidate, place->value + place->start, size - 1U);
		place->candidate[size - 1U] = '\0';
		matched = sh_glob_match(place->text, place->marks, place->candidate);
		if (matched) {
			*match = size - 1U;
			return 1;
		}
	}

	/* No candidate matches. */
	return 0;
}

/*
 * Adds the string of a replacement, with each & that no quote holds
 * standing for the match.
 */
static void
append_replacement(
	struct xbuf *out,
	const char *with,
	const unsigned char *with_marks,
	const char *matched)
{
	size_t index;
	int unquoted;

	/* Adds each character of the string, or the match for &. */
	for (index = 0; with[index] != '\0'; index++) {
		unquoted = 0;
		if (with[index] == '&') {
			if (with_marks == NULL || !with_marks[index])
				unquoted = 1;
		}

		/* Adds the match for an unquoted &, and any other character as it is. */
		if (unquoted)
			append_value(out, matched, 1);
		else
			append_char(out, with[index], X_QUOTED);
	}
}

/*
 * Changes the case of the first character (^ ,) or of every one (^^ ,,)
 * that the pattern matches (any character when there is none).
 */
static char *
change_case(
	const char *value,
	const char *op,
	const char *text,
	const unsigned char *marks)
{
	char *result;
	char single[2];
	size_t index;
	int matched;
	int upper;

	/* ^ makes upper case, and , lower case. */
	upper = 0;
	if (op[0] == '^')
		upper = 1;

	/* Changes a copy in place, character by character. */
	result = sh_strdup(value);
	for (index = 0; result[index] != '\0'; index++) {
		/* A character changes when the pattern matches it, or there is none. */
		matched = 1;
		if (text[0] != '\0') {
			single[0] = result[index];
			single[1] = '\0';
			matched = sh_glob_match(text, marks, single);
		}

		/* Changes a matched character. */
		if (matched)
			result[index] = changed_case(result[index], upper);

		/* ^ and , change only the first character. */
		if (op[1] == '\0')
			break;
	}

	/* Succeeded: the changed copy. */
	return result;
}

/* Returns an ASCII letter in upper or lower case; anything else as it is. */
static char
changed_case(
	char value,
	int upper)
{
	/* Makes a lower-case letter upper case. */
	if (upper) {
		if (value >= 'a' && value <= 'z')
			return (char)(value - 'a' + 'A');
		return value;
	}

	/* Makes an upper-case letter lower case. */
	if (value >= 'A' && value <= 'Z')
		return (char)(value - 'A' + 'a');

	/* Succeeded: anything else stays. */
	return value;
}

/*
 * Returns where the / that ends the pattern of ${name/pattern/string}
 * is, or the length when there is none; quoted and escaped slashes and
 * those inside expansions do not count.
 */
static size_t
split_replacement(
	const char *word,
	size_t length)
{
	const char *end;
	size_t index;
	int expansion;

	/* Walks the word, skipping what is quoted or inside an expansion. */
	for (index = 0; index < length; index++) {
		/* A backslash escapes the character after it. */
		if (word[index] == '\\') {
			index++;
			continue;
		}

		/* A quotation is skipped whole; an unclosed one hides any /. */
		if (word[index] == '\'' || word[index] == '"') {
			if (word[index] == '\'')
				end = sh_skip_single(word + index);
			else
				end = sh_skip_double(word + index);
			if (end == NULL)
				return length;
			index = (size_t)(end - word) - 1U;
			continue;
		}

		/* So is an expansion in braces or parentheses. */
		expansion = 0;
		if (word[index] == '$' && index + 1 < length) {
			if (word[index + 1] == '{' || word[index + 1] == '(')
				expansion = 1;
		}

		/* Skips the expansion whole; an unclosed one hides any /. */
		if (expansion) {
			end = sh_skip_expansion(word + index, 0);
			if (end == NULL)
				return length;
			index = (size_t)(end - word) - 1U;
			continue;
		}

		/* The first other / ends the pattern. */
		if (word[index] == '/')
			return index;
	}

	/* No string: the match is deleted. */
	return length;
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
	/* Adds the positional parameters as a list of values. */
	append_list(x,
		    which,
		    (const char *const *)x->context->positional,
		    x->context->positional_count,
		    quoted,
		    out);
}

/* Adds a list of values as "$@" (which is @) or "$*" (*) adds the positionals. */
static void
append_list(
	struct expander *x,
	char which,
	const char *const *values,
	int count,
	int quoted,
	struct xbuf *out)
{
	const char *ifs;
	int index;

	/* "$*": one field, joined. */
	if (quoted && which == '*') {
		ifs = ifs_value(x);
		append_mark(out, X_KEEP);
		for (index = 0; index < count; index++) {
			if (index > 0 && ifs[0] != '\0')
				append_char(out, ifs[0], X_QUOTED);
			append_value(out, values[index], 1);
		}

		/* The values are written. */
		return;
	}

	/* Otherwise a field each, with a boundary between them. */
	for (index = 0; index < count; index++) {
		if (index > 0)
			append_mark(out, X_BREAK);
		if (quoted)
			append_mark(out, X_KEEP);
		append_value(out, values[index], quoted);
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
