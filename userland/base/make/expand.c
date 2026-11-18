/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The expansion of make's text: $$, $x, $(name) and ${name}, the
 * substitution reference $(name:from=to), nested references such as
 * $(am__v_CC_$(V)), the automatic variables and their D and F forms, and
 * the functions (function.c).
 *
 * The name inside a reference is itself expanded first.  A recursive
 * variable's value is expanded each time it is read; a variable that
 * reaches itself again while it is being expanded ends make.
 */

#include "make.h"

#include <stdlib.h>
#include <string.h>

static const char *reference_end(const char *text, const char *end, char open, char close, const struct expansion *context);
static void expand_reference(const struct expansion *context, const char *text, size_t length, struct buffer *out);
static const char *top_level_char(const char *text, const char *end, char wanted);
static void expand_name(const struct expansion *context, const char *name, size_t length, struct buffer *out);
static void expand_variable_value(const struct expansion *context, struct variable *variable, struct buffer *out);
static int expand_automatic(const struct expansion *context, const char *name, size_t length, struct buffer *out);
static const char *automatic_value(const struct automatic *automatic, char name, int *known);
static void add_file_parts(struct buffer *out, const char *value, char part);
static void substitute_words(struct buffer *out, const char *value, const char *from, const char *to);

/*
 * Returns the expansion of a text, which the caller frees.
 */
char *
expand(
	const struct expansion *context,
	const char *text)
{
	struct buffer out;
	size_t length;

	/* The whole text into a new buffer. */
	memset(&out, 0, sizeof(out));
	length = strlen(text);
	expand_into(context, text, length, &out);

	/* Succeeded. */
	return buffer_finish(&out);
}

/*
 * Appends the expansion of the first length bytes of a text to a buffer.
 */
void
expand_into(
	const struct expansion *context,
	const char *text,
	size_t length,
	struct buffer *out)
{
	const char *cursor;
	const char *end;
	const char *dollar;
	const char *close;
	char open;
	char close_char;

	/* Each run of plain text, then the reference after it. */
	cursor = text;
	end = text + length;
	while (cursor < end) {
		dollar = memchr(cursor, '$', (size_t)(end - cursor));
		if (dollar == NULL) {
			buffer_add(out, cursor, (size_t)(end - cursor));
			break;
		}

		/* The plain text before the $. */
		buffer_add(out, cursor, (size_t)(dollar - cursor));

		/* A $ at the very end is nothing. */
		if (dollar + 1 >= end)
			break;

		/* $$ is a dollar. */
		open = dollar[1];
		if (open == '$') {
			buffer_add_char(out, '$');
			cursor = dollar + 2;
			continue;
		}

		/* $x: a name of one character. */
		if (open != '(' && open != '{') {
			expand_name(context, dollar + 1, 1, out);
			cursor = dollar + 2;
			continue;
		}

		/* $(...) or ${...}: to the bracket that closes it. */
		close_char = ')';
		if (open == '{')
			close_char = '}';
		close = reference_end(dollar + 2, end, open, close_char, context);
		expand_reference(context, dollar + 2, (size_t)(close - (dollar + 2)), out);
		cursor = close + 1;
	}
}

/*
 * Returns where the reference that starts at text closes: the close
 * bracket that balances the open one.  An unclosed reference ends make.
 */
static const char *
reference_end(
	const char *text,
	const char *end,
	char open,
	char close,
	const struct expansion *context)
{
	const char *cursor;
	int depth;

	/* Only brackets of the same kind nest. */
	depth = 0;
	for (cursor = text; cursor < end; cursor++) {
		if (*cursor == open) {
			depth++;
		} else if (*cursor == close) {
			if (depth == 0)
				return cursor;
			depth--;
		}
	}

	/* No bracket closes it. */
	if (context->file != NULL)
		make_file_fatal(context->file, context->line, "unterminated variable reference");
	make_fatal("unterminated variable reference");
	return end;
}

/*
 * Expands what is inside $(...): a function call, a substitution
 * reference, or a variable whose name is expanded first.
 */
static void
expand_reference(
	const struct expansion *context,
	const char *text,
	size_t length,
	struct buffer *out)
{
	struct buffer name;
	struct buffer from;
	struct buffer to;
	struct buffer value;
	const char *end;
	const char *colon;
	const char *equals;
	char *value_text;
	int handled;

	/* A function: a known name, then a blank. */
	end = text + length;
	handled = function_call(context, text, length, out);
	if (handled)
		return;

	/* $(name:from=to): the colon and the = at the top level. */
	colon = top_level_char(text, end, ':');
	equals = NULL;
	if (colon != NULL)
		equals = top_level_char(colon + 1, end, '=');
	if (equals == NULL) {
		memset(&name, 0, sizeof(name));
		expand_into(context, text, length, &name);
		expand_name(context, buffer_text(&name), name.length, out);
		free(name.text);
		return;
	}

	/* The name, the suffix to replace and its replacement, each expanded. */
	memset(&name, 0, sizeof(name));
	memset(&from, 0, sizeof(from));
	memset(&to, 0, sizeof(to));
	memset(&value, 0, sizeof(value));
	expand_into(context, text, (size_t)(colon - text), &name);
	expand_into(context, colon + 1, (size_t)(equals - (colon + 1)), &from);
	expand_into(context, equals + 1, (size_t)(end - (equals + 1)), &to);

	/* The value of the variable, with each word's suffix replaced. */
	expand_name(context, buffer_text(&name), name.length, &value);
	value_text = buffer_finish(&value);
	substitute_words(out, value_text, buffer_text(&from), buffer_text(&to));
	free(value_text);
	free(name.text);
	free(from.text);
	free(to.text);
}

/*
 * Returns the first occurrence of a character outside any $(...) or
 * ${...} in a text, or NULL.
 */
static const char *
top_level_char(
	const char *text,
	const char *end,
	char wanted)
{
	const char *cursor;
	int depth;

	/* Brackets of either kind hide what is inside them. */
	depth = 0;
	for (cursor = text; cursor < end; cursor++) {
		if (*cursor == '(' || *cursor == '{') {
			depth++;
		} else if (*cursor == ')' || *cursor == '}') {
			if (depth > 0)
				depth--;
		} else if (*cursor == wanted && depth == 0) {
			return cursor;
		}
	}

	/* None outside the brackets. */
	return NULL;
}

/*
 * Appends the value of a variable named by an already expanded name: an
 * automatic variable in a recipe, or a variable of the scope.
 */
static void
expand_name(
	const struct expansion *context,
	const char *name,
	size_t length,
	struct buffer *out)
{
	struct variable *variable;
	struct variable *outer;
	size_t before;
	int automatic;
	int appended;

	/* The automatic variables. */
	automatic = expand_automatic(context, name, length, out);
	if (automatic)
		return;

	/* A variable that is not defined is empty. */
	variable = variable_lookup(context->scope, name, length);
	if (variable == NULL)
		return;

	/* A simple variable's value is ready. */
	if (variable->flavor == FLAVOR_SIMPLE) {
		buffer_add_string(out, variable->value);
		return;
	}

	/* A target's += adds to the value it inherits. */
	if (variable->append) {
		outer = variable_lookup_outer(context->scope, variable);
		appended = 0;
		if (outer != NULL) {
			before = out->length;
			expand_variable_value(context, outer, out);
			if (out->length > before)
				appended = 1;
		}

		/* A space before the target's own text, when both have something. */
		if (appended && variable->value[0] != '\0')
			buffer_add_char(out, ' ');
	}

	/* A recursive one is expanded now, unless it is already being expanded. */
	if (variable->expanding) {
		if (context->file != NULL)
			make_file_fatal(context->file, context->line, "Recursive variable '%s' references itself (eventually)", variable->name);
		make_fatal("Recursive variable '%s' references itself (eventually)", variable->name);
	}

	/* Succeeded: the expansion of its value. */
	variable->expanding = 1;
	expand_into(context, variable->value, strlen(variable->value), out);
	variable->expanding = 0;
}

/* Appends a variable's value: as it is when simple, expanded when recursive. */
static void
expand_variable_value(
	const struct expansion *context,
	struct variable *variable,
	struct buffer *out)
{
	/* A simple variable's value is ready. */
	if (variable->flavor == FLAVOR_SIMPLE) {
		buffer_add_string(out, variable->value);
		return;
	}

	/* A variable that reaches itself again ends make. */
	if (variable->expanding)
		make_fatal("Recursive variable '%s' references itself (eventually)", variable->name);

	/* Succeeded: the expansion of its value. */
	variable->expanding = 1;
	expand_into(context, variable->value, strlen(variable->value), out);
	variable->expanding = 0;
}

/*
 * Appends the value of an automatic variable ($@ $< $? $^ $+ $* $| $%,
 * and $(@D) $(@F) and so on); returns 0 when the name is not one.
 */
static int
expand_automatic(
	const struct expansion *context,
	const char *name,
	size_t length,
	struct buffer *out)
{
	const char *value;
	char part;
	int known;

	/* One character, or one and D or F. */
	if (length != 1 && length != 2)
		return 0;
	part = '\0';
	if (length == 2) {
		part = name[1];
		if (part != 'D' && part != 'F')
			return 0;
	}

	/* The value by the first character. */
	value = automatic_value(context->automatic, name[0], &known);
	if (!known)
		return 0;

	/* An automatic variable outside a recipe is empty. */
	if (value == NULL)
		return 1;

	/* Succeeded: the whole value, or the directory or file part of each word. */
	if (part == '\0') {
		buffer_add_string(out, value);
		return 1;
	}

	/* The directory or file part of each word. */
	add_file_parts(out, value, part);
	return 1;
}

/*
 * Returns the value of the automatic variable named by one character,
 * NULL when it is empty (outside a recipe, or $%); *known is 0 when the
 * character names none.
 */
static const char *
automatic_value(
	const struct automatic *automatic,
	char name,
	int *known)
{
	/* The characters that name automatic variables. */
	*known = 1;
	if (name == '%')
		return NULL;
	if (name != '@' && name != '<' && name != '?' && name != '^' && name != '+' && name != '*' && name != '|') {
		*known = 0;
		return NULL;
	}

	/* Outside a recipe they are empty. */
	if (automatic == NULL)
		return NULL;

	/* Chooses the field by the character. */
	switch (name) {
	case '@':
		return automatic->target;
	case '<':
		return automatic->first;
	case '?':
		return automatic->newer;
	case '^':
		return automatic->unique;
	case '+':
		return automatic->all;
	case '*':
		return automatic->stem;
	default:
		break;
	}

	/* Succeeded: the order-only prerequisites, the one character left. */
	return automatic->order_only;
}

/*
 * Appends the directory part (D: without the last slash, . when there is
 * none) or the file part (F) of each word, separated by spaces.
 */
static void
add_file_parts(
	struct buffer *out,
	const char *value,
	char part)
{
	const char *cursor;
	const char *word;
	const char *slash;
	size_t length;
	size_t index;
	int first;

	/* Each word. */
	cursor = value;
	first = 1;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		if (!first)
			buffer_add_char(out, ' ');
		first = 0;

		/* The last slash of the word divides it. */
		slash = NULL;
		for (index = 0; index < length; index++) {
			if (word[index] == '/')
				slash = word + index;
		}

		/* The directory, without its trailing slash (/ stays /). */
		if (part == 'D') {
			if (slash == NULL) {
				buffer_add_char(out, '.');
			} else if (slash == word) {
				buffer_add_char(out, '/');
			} else {
				buffer_add(out, word, (size_t)(slash - word));
			}

			continue;
		}

		/* The file name after the directory. */
		if (slash == NULL) {
			buffer_add(out, word, length);
		} else {
			buffer_add(out, slash + 1, length - (size_t)(slash + 1 - word));
		}
	}
}

/*
 * Appends the words of a value with a suffix replaced, as
 * $(name:from=to) does: each word that ends with from gets to instead.
 * With a % in from, it is a pattern as in $(patsubst from,to,...).
 * The words are joined by single spaces.
 */
static void
substitute_words(
	struct buffer *out,
	const char *value,
	const char *from,
	const char *to)
{
	const char *percent;
	const char *cursor;
	const char *word;
	size_t length;
	size_t from_length;
	int compare;
	int first;

	/* A % in from makes it a pattern; otherwise it is a suffix. */
	percent = strchr(from, '%');
	if (percent != NULL) {
		function_patsubst_words(out, value, from, to);
		return;
	}

	/* Each word, its suffix replaced when it ends with from. */
	cursor = value;
	first = 1;
	from_length = strlen(from);
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		if (!first)
			buffer_add_char(out, ' ');
		first = 0;
		compare = -1;
		if (length >= from_length)
			compare = strncmp(word + length - from_length, from, from_length);
		if (compare == 0) {
			buffer_add(out, word, length - from_length);
			buffer_add_string(out, to);
		} else {
			buffer_add(out, word, length);
		}
	}
}
