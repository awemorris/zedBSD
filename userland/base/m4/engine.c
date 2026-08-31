/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland engine component.
 */

#include "userland/base/common/command.h"
#include "userland/base/m4/m4.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M4_DEPTH_MAX 128
#define M4_ARGUMENT_MAX 128
#define M4_OUTPUT_MAX (16U * 1024U * 1024U)
#define M4_DIVERSION_COUNT 10

struct builder {
	char *text;
	size_t length;
	size_t capacity;
};

struct definition {
	char *name;
	char *value;
};

struct arguments {
	char **item;
	size_t count;
};

struct m4_context {
	struct definition *definition;
	size_t definition_count;
	size_t definition_capacity;
	struct builder output;
	struct builder diversion[M4_DIVERSION_COUNT];
	int current_diversion;
	char quote_open[16];
	char quote_close[16];
	char error[256];
	const char *source;
	unsigned line;
	int discard_line;
};

struct expression_parser {
	const char *text;
	int failed;
};

static struct definition *find_definition(struct m4_context *context, const char *name, size_t length);
static void builder_free(struct builder *builder);
static int expand_text(struct m4_context *context, const char *text, size_t length, struct builder *capture, unsigned depth);
static void set_error(struct m4_context *context, const char *message);
static int starts_with(const char *text, size_t length, size_t offset, const char *word);
static int expand_quoted(struct m4_context *context, const char *text, size_t length, size_t *offset, struct builder *capture);
static int emit(struct m4_context *context, struct builder *capture, const char *text, size_t length);
static int builder_append(struct m4_context *context, struct builder *builder, const char *text, size_t length);
static int is_name_start(char c);
static int is_name_character(char c);
static int is_builtin(const char *name, size_t length);
static int parse_arguments(struct m4_context *context, const char *text, size_t length, size_t *offset, struct arguments *arguments);
static int arguments_add(struct m4_context *context, struct arguments *arguments, const char *text, size_t length);
static void arguments_free(struct arguments *arguments);
static int emit_user_macro(struct m4_context *context, struct definition *definition, const char *name, size_t name_length, const struct arguments *arguments, struct builder *capture, unsigned depth);
static char *expanded_argument(struct m4_context *context, const struct arguments *arguments, size_t index, unsigned depth);
static int argument_expand(struct m4_context *context, const struct arguments *arguments, size_t index, struct builder *result, unsigned depth);
static int emit_builtin(struct m4_context *context, const char *name, size_t name_length, const struct arguments *arguments, struct builder *capture, unsigned depth);
static int read_file(const char *path, char **result, size_t *result_length);
static int builtin_eval(struct m4_context *context, const char *expression, struct builder *capture);
static long long expression_add(struct expression_parser *parser);
static long long expression_product(struct expression_parser *parser);
static long long expression_unary(struct expression_parser *parser);
static void expression_space(struct expression_parser *parser);
static long long expression_primary(struct expression_parser *parser);

/*
 * Implements the m4 error operation.
 */
const char *
m4_error(
	const struct m4_context *context)
{
	/* Returns the computed result. */
	return context->error[0] == '\0' ? NULL : context->error;
}

/*
 * Implements the m4 define operation.
 */
int
m4_define(
	struct m4_context *context,
	const char *name,
	const char *value)
{
	size_t capacity;
	struct definition *grown;
	struct definition *definition;
	char *copy;

	definition = find_definition(context, name, strlen(name));
	copy = strdup(value);

	/* Handles the copy availability. */
	if (copy == NULL)
		return -1;

	/* Handles the definition availability. */
	if (definition != NULL) {
		free(definition->value);
		definition->value = copy;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the context condition. */
	if (context->definition_count == context->definition_capacity) {
		capacity = context->definition_capacity == 0
		      ? 32
		      : context->definition_capacity * 2;

		/* Handles the capacity condition. */
		if (capacity < context->definition_capacity ||
		    capacity > SIZE_MAX / sizeof(*grown)) {
			free(copy);
			errno = EOVERFLOW;

			/* Reports operation failure. */
			return -1;
		}
		grown = realloc(context->definition, capacity * sizeof(*grown));

		/* Handles the grown availability. */
		if (grown == NULL) {
			free(copy);

			/* Reports operation failure. */
			return -1;
		}
		context->definition = grown;
		context->definition_capacity = capacity;
	}
	definition = &context->definition[context->definition_count];
	definition->name = strdup(name);

	/* Handles the name availability. */
	if (definition->name == NULL) {
		free(copy);

		/* Reports operation failure. */
		return -1;
	}
	definition->value = copy;
	context->definition_count++;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the m4 undefine operation.
 */
int
m4_undefine(
	struct m4_context *context,
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = context->definition_count; index != 0; index--) {
		/* Selects the matching value. */
		if (strcmp(context->definition[index - 1].name, name) == 0) {
			free(context->definition[index - 1].name);
			free(context->definition[index - 1].value);
			memmove(context->definition + index - 1,
				context->definition + index,
				(context->definition_count - index) *
				    sizeof(*context->definition));
			context->definition_count--;

			/* Reports successful completion. */
			return 0;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the m4 context create operation.
 */
struct m4_context *
m4_context_create(
	void)
{
	struct m4_context *context;

	context = calloc(1, sizeof(*context));

	/* Handles the context availability. */
	if (context != NULL) {
		strcpy(context->quote_open, "`");
		strcpy(context->quote_close, "'");
	}

	/* Returns the computed result. */
	return context;
}

/*
 * Implements the m4 context destroy operation.
 */
void
m4_context_destroy(
	struct m4_context *context)
{
	size_t index;

	/* Handles the context availability. */
	if (context == NULL)
		return;

	/* Process each remaining element. */
	for (index = 0; index < context->definition_count; index++) {
		free(context->definition[index].name);
		free(context->definition[index].value);
	}
	free(context->definition);
	builder_free(&context->output);

	/* Process each remaining element. */
	for (index = 0; index < M4_DIVERSION_COUNT; index++)
		builder_free(&context->diversion[index]);
	free(context);
}

/*
 * Implements the m4 process operation.
 */
int
m4_process(
	struct m4_context *context,
	const char *source,
	const char *text,
	size_t length)
{
	int function_result;

	context->source = source;
	context->line = 1;

	/* Obtains the expand text result. */
	function_result = expand_text(context, text, length, NULL, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the m4 finish operation.
 */
int
m4_finish(
	struct m4_context *context,
	int descriptor)
{
	int function_result;
	size_t index;

	/* Process each remaining element. */
	for (index = 1; index < M4_DIVERSION_COUNT; index++) {
		/* Handles a failed builder append operation. */
		if (context->diversion[index].length != 0 &&
		    builder_append(context, &context->output,
				   context->diversion[index].text,
				   context->diversion[index].length) != 0)

			/* Reports operation failure. */
			return -1;
	}

	/* Obtains the command write all result. */
	function_result = command_write_all(
	    descriptor,
	    context->output.text == NULL ? "" : context->output.text,
	    context->output.length);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the find definition operation. */
static struct definition *
find_definition(
	struct m4_context *context,
	const char *name,
	size_t length)
{
	size_t index;

	index = context->definition_count;

	/* Process each remaining element. */
	while (index != 0) {
		index--;

		/* Handles a failed strlen operation. */
		if (strlen(context->definition[index].name) == length &&
		    memcmp(context->definition[index].name, name, length) == 0)

			/* Returns the computed result. */
			return &context->definition[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the builder free operation. */
static void
builder_free(
	struct builder *builder)
{
	free(builder->text);
	memset(builder, 0, sizeof(*builder));
}

/* Supports the expand text operation. */
static int
expand_text(
	struct m4_context *context,
	const char *text,
	size_t length,
	struct builder *capture,
	unsigned depth)
{
	size_t start_local;
	size_t start_local1;
	size_t after_name;
	struct definition *definition;
	struct arguments arguments;
	int builtin;
	size_t offset;

	offset = 0;

	/* Handles the depth condition. */
	if (depth > M4_DEPTH_MAX) {
		set_error(context, "macro expansion depth exceeded");

		/* Reports operation failure. */
		return -1;
	}
	while (offset < length) {
		/* Handles the context condition. */
		if (context->discard_line) {
			/* Process each remaining element. */
			while (offset < length && text[offset] != '\n')
				offset++;

			/* Checks the current offset. */
			if (offset < length) {
				offset++;
				context->line++;
			}
			context->discard_line = 0;
			continue;
		}

		/* Handles a failed starts with operation. */
		if (starts_with(text, length, offset, context->quote_open)) {
			/* Handles a failed expand quoted operation. */
			if (expand_quoted(context, text, length, &offset,
					  capture) != 0)

				/* Reports operation failure. */
				return -1;
			continue;
		}

		/* Validates the current text. */
		if (text[offset] == '#') {
			start_local = offset;

			/* Process each remaining element. */
			while (offset < length && text[offset] != '\n')
				offset++;

			/* Checks the current offset. */
			if (offset < length)
				offset++;

			/* Handles a failed emit operation. */
			if (emit(context, capture, text + start_local,
				 offset - start_local) != 0)

				/* Reports operation failure. */
				return -1;
			context->line++;
			continue;
		}

		/* Handles the name start condition. */
		if (is_name_start(text[offset])) {
			start_local1 = offset++;

			/* Initializes the parsed argument list. */
			memset(&arguments, 0, sizeof(arguments));

			/* Process each remaining element. */
			while (offset < length &&
			       is_name_character(text[offset]))
				offset++;
			after_name = offset;
			definition = find_definition(context, text + start_local1,
						     offset - start_local1);
			builtin = is_builtin(text + start_local1, offset - start_local1);

			/* Handles the definition availability. */
			if (definition == NULL && !builtin) {
				/* Handles a failed emit operation. */
				if (emit(context, capture, text + start_local1,
					 offset - start_local1) != 0)

					/* Reports operation failure. */
					return -1;
				continue;
			}
			while (offset < length &&
			       (text[offset] == ' ' || text[offset] == '\t'))
				offset++;

			/* Checks the current offset. */
			if (offset < length && text[offset] == '(') {
				/* Handles a failed parse arguments operation. */
				if (parse_arguments(context, text, length,
						    &offset, &arguments) != 0)

					/* Reports operation failure. */
					return -1;
			} else {
				offset = after_name;
			}

			/* Handles the definition availability. */
			if (definition != NULL) {
				/* Handles a failed emit user macro operation. */
				if (emit_user_macro(
					context, definition, text + start_local1,
					after_name - start_local1, &arguments, capture,
					depth) != 0) {
					arguments_free(&arguments);

					/* Reports operation failure. */
					return -1;
				}
			} else if (emit_builtin(context, text + start_local1,
						after_name - start_local1, &arguments,
						capture, depth) != 0) {
				arguments_free(&arguments);

				/* Reports operation failure. */
				return -1;
			}
			arguments_free(&arguments);
			continue;
		}

		/* Handles a failed emit operation. */
		if (emit(context, capture, text + offset, 1) != 0)
			return -1;

		/* Validates the current text. */
		if (text[offset++] == '\n')
			context->line++;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the set error operation. */
static void
set_error(
	struct m4_context *context,
	const char *message)
{
	/* Handles an operation failure. */
	if (context->error[0] == '\0') {
		(void)snprintf(context->error, sizeof(context->error),
			       "%s:%u: %s",
			       context->source == NULL ? "-" : context->source,
			       context->line, message);
	}
}

/* Supports the starts with operation. */
static int
starts_with(
	const char *text,
	size_t length,
	size_t offset,
	const char *word)
{
	int function_result;
	size_t word_length;

	word_length = strlen(word);

	/* Computes the function result. */
	function_result = word_length != 0 && word_length <= length - offset &&
	       memcmp(text + offset, word, word_length) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the expand quoted operation. */
static int
expand_quoted(
	struct m4_context *context,
	const char *text,
	size_t length,
	size_t *offset,
	struct builder *capture)
{
	size_t cursor;
	size_t start;
	unsigned depth;

	cursor = *offset + strlen(context->quote_open);
	start = cursor;
	depth = 1;

	/* Process each remaining element. */
	while (cursor < length) {
		/* Handles a failed starts with operation. */
		if (starts_with(text, length, cursor, context->quote_open)) {
			/* Handles a failed emit operation. */
			if (emit(context, capture, text + start,
				 cursor - start) != 0 ||
			    emit(context, capture, context->quote_open,
				 strlen(context->quote_open)) != 0)

				/* Reports operation failure. */
				return -1;
			cursor += strlen(context->quote_open);
			start = cursor;
			depth++;
			continue;
		}

		/* Handles a failed starts with operation. */
		if (starts_with(text, length, cursor, context->quote_close)) {
			/* Handles the depth condition. */
			if (--depth == 0) {
				/* Handles a failed emit operation. */
				if (emit(context, capture, text + start,
					 cursor - start) != 0)

					/* Reports operation failure. */
					return -1;
				*offset = cursor + strlen(context->quote_close);
				/* Reports successful completion. */
				return 0;
			}

			/* Handles a failed emit operation. */
			if (emit(context, capture, text + start,
				 cursor - start) != 0 ||
			    emit(context, capture, context->quote_close,
				 strlen(context->quote_close)) != 0)

				/* Reports operation failure. */
				return -1;
			cursor += strlen(context->quote_close);
			start = cursor;
			continue;
		}

		/* Validates the current text. */
		if (text[cursor++] == '\n')
			context->line++;
	}
	set_error(context, "unterminated quote");

	/* Reports operation failure. */
	return -1;
}

/* Supports the emit operation. */
static int
emit(
	struct m4_context *context,
	struct builder *capture,
	const char *text,
	size_t length)
{
	int function_result;
	struct builder *target;

	target = capture;

	/* Handles the target availability. */
	if (target == NULL) {
		/* Handles the context condition. */
		if (context->current_diversion < 0)
			return 0;
		target = context->current_diversion == 0
			     ? &context->output
			     : &context->diversion[context->current_diversion];
	}

	/* Obtains the builder append result. */
	function_result = builder_append(context, target, text, length);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the builder append operation. */
static int
builder_append(
	struct m4_context *context,
	struct builder *builder,
	const char *text,
	size_t length)
{
	size_t capacity;
	size_t needed;
	char *grown;

	/* Checks the current data length. */
	if (length > M4_OUTPUT_MAX - builder->length) {
		set_error(context, "output or diversion limit exceeded");
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	needed = builder->length + length + 1;

	/* Handles the needed condition. */
	if (needed > builder->capacity) {
		capacity = builder->capacity == 0 ? 256 : builder->capacity;

		/* Continue while the operation condition remains true. */
		while (capacity < needed) {
			/* Handles the capacity condition. */
			if (capacity > M4_OUTPUT_MAX / 2) {
				capacity = needed;
				break;
			}
			capacity *= 2;
		}
		grown = realloc(builder->text, capacity);

		/* Handles the grown availability. */
		if (grown == NULL) {
			set_error(context, "out of memory");

			/* Reports operation failure. */
			return -1;
		}
		builder->text = grown;
		builder->capacity = capacity;
	}
	memcpy(builder->text + builder->length, text, length);
	builder->length += length;
	builder->text[builder->length] = '\0';

	/* Reports successful completion. */
	return 0;
}

/* Supports the is name start operation. */
static int
is_name_start(
	char c)
{
	/* Returns the computed result. */
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

/* Supports the is name character operation. */
static int
is_name_character(
	char c)
{
	int function_result;

	/* Computes the function result. */
	function_result = is_name_start(c) || (c >= '0' && c <= '9');

	/* Returns the computed result. */
	return function_result;
}

/* Supports the is builtin operation. */
static int
is_builtin(
	const char *name,
	size_t length)
{
	static const char *const names[] = {
	    "changequote", "decr",     "define", "divert",   "divnum",
	    "dnl",	   "errprint", "eval",	 "ifdef",    "ifelse",
	    "include",	   "incr",     "index",	 "len",	     "m4exit",
	    "sinclude",	   "shift",    "substr", "translit", "undefine",
	    "undivert",	   NULL};
	size_t index;

	/* Process each remaining element. */
	for (index = 0; names[index] != NULL; index++) {
		/* Handles a failed strlen operation. */
		if (strlen(names[index]) == length &&
		    memcmp(names[index], name, length) == 0)

			/* Reports operation failure. */
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the parse arguments operation. */
static int
parse_arguments(
	struct m4_context *context,
	const char *text,
	size_t length,
	size_t *offset,
	struct arguments *arguments)
{
	size_t start;
	size_t cursor;
	unsigned depth;
	unsigned quote_depth;

	depth = 1;
	quote_depth = 0;

	memset(arguments, 0, sizeof(*arguments));

	/* Process each remaining element. */
	start = cursor = *offset + 1;
	while (cursor < length) {
		/* Handles a failed starts with operation. */
		if (starts_with(text, length, cursor, context->quote_open)) {
			quote_depth++;
			cursor += strlen(context->quote_open);
			continue;
		}

		/* Handles a failed starts with operation. */
		if (quote_depth != 0 &&
		    starts_with(text, length, cursor, context->quote_close)) {
			quote_depth--;
			cursor += strlen(context->quote_close);
			continue;
		}

		/* Handles the quote depth condition. */
		if (quote_depth == 0) {
			/* Validates the current text. */
			if (text[cursor] == '(')
				depth++;
			else if (text[cursor] == ')') {
				/* Handles the depth condition. */
				if (--depth == 0) {
					/* Checks the current cursor position. */
					if (cursor != start ||
					    arguments->count != 0) {
						/* Handles a failed arguments add operation. */
						if (arguments_add(
							context, arguments,
							text + start,
							cursor - start) != 0)
							goto fail;
					}
					*offset = cursor + 1;
					/* Reports successful completion. */
					return 0;
				}
			} else if (text[cursor] == ',' && depth == 1) {
				/* Handles a failed arguments add operation. */
				if (arguments_add(context, arguments,
						  text + start,
						  cursor - start) != 0)
					goto fail;
				start = cursor + 1;
			}
		}

		/* Validates the current text. */
		if (text[cursor++] == '\n')
			context->line++;
	}
	set_error(context, "unterminated macro argument list");
fail:
	arguments_free(arguments);

	/* Reports operation failure. */
	return -1;
}

/* Supports the arguments add operation. */
static int
arguments_add(
	struct m4_context *context,
	struct arguments *arguments,
	const char *text,
	size_t length)
{
	char **grown;
	char *copy;

	/* Process each remaining element. */
	while (length != 0 && (*text == ' ' || *text == '\t')) {
		text++;
		length--;
	}
	while (length != 0 &&
	       (text[length - 1] == ' ' || text[length - 1] == '\t'))
		length--;

	/* Handles the arguments condition. */
	if (arguments->count >= M4_ARGUMENT_MAX) {
		set_error(context, "macro argument limit exceeded");

		/* Reports operation failure. */
		return -1;
	}
	copy = malloc(length + 1);

	/* Handles the copy availability. */
	if (copy == NULL)
		return -1;
	memcpy(copy, text, length);
	copy[length] = '\0';
	grown =
	    realloc(arguments->item, (arguments->count + 1) * sizeof(*grown));

	/* Handles the grown availability. */
	if (grown == NULL) {
		free(copy);

		/* Reports operation failure. */
		return -1;
	}
	arguments->item = grown;
	arguments->item[arguments->count++] = copy;

	/* Reports successful completion. */
	return 0;
}

/* Supports the arguments free operation. */
static void
arguments_free(
	struct arguments *arguments)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < arguments->count; index++)
		free(arguments->item[index]);
	free(arguments->item);
	memset(arguments, 0, sizeof(*arguments));
}

/* Supports the emit user macro operation. */
static int
emit_user_macro(
	struct m4_context *context,
	struct definition *definition,
	const char *name,
	size_t name_length,
	const struct arguments *arguments,
	struct builder *capture,
	unsigned depth)
{
	char *argument;
	size_t index;
	struct builder replaced = {0};
	const char *cursor;

	cursor = definition->value;

	/* Continue while the operation condition remains true. */
	while (*cursor != '\0') {
		/* Checks the current cursor position. */
		if (*cursor == '$' && cursor[1] >= '0' && cursor[1] <= '9') {
			index = (size_t)(cursor[1] - '0');

			/* Checks the current index. */
			if (index == 0) {
				/* Handles a failed builder append operation. */
				if (builder_append(context, &replaced, name,
						   name_length) != 0)
					goto fail;
			} else if (index <= arguments->count) {
				argument = expanded_argument(
			    context, arguments, index - 1, depth);

				/* Handles a failed builder append operation. */
				if (argument == NULL ||
				    builder_append(context, &replaced, argument,
						   strlen(argument)) != 0) {
					free(argument);
					goto fail;
				}
				free(argument);
			}
			cursor += 2;
		} else if (builder_append(context, &replaced, cursor++, 1) != 0)
			goto fail;
	}

	/* Handles a failed expand text operation. */
	if (expand_text(context, replaced.text == NULL ? "" : replaced.text,
			replaced.length, capture, depth + 1) != 0)
		goto fail;
	builder_free(&replaced);

	/* Reports successful completion. */
	return 0;

fail:
	builder_free(&replaced);

	/* Reports operation failure. */
	return -1;
}

/* Supports the expanded argument operation. */
static char *
expanded_argument(
	struct m4_context *context,
	const struct arguments *arguments,
	size_t index,
	unsigned depth)
{
	struct builder result = {0};

	/* Handles a failed argument expand operation. */
	if (argument_expand(context, arguments, index, &result, depth) != 0) {
		builder_free(&result);

		/* Reports that no result is available. */
		return NULL;
	}

	/* Handles the text availability. */
	if (result.text == NULL)
		result.text = strdup("");

	/* Returns the computed result. */
	return result.text;
}

/* Supports the argument expand operation. */
static int
argument_expand(
	struct m4_context *context,
	const struct arguments *arguments,
	size_t index,
	struct builder *result,
	unsigned depth)
{
	int function_result;

	/* Checks the current index. */
	if (index >= arguments->count)
		return 0;

	/* Obtains the expand text result. */
	function_result = expand_text(context, arguments->item[index],
			   strlen(arguments->item[index]), result, depth + 1);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the emit builtin operation. */
static int
emit_builtin(
	struct m4_context *context,
	const char *name,
	size_t name_length,
	const struct arguments *arguments,
	struct builder *capture,
	unsigned depth)
{
	char output_local[64];
	int length_local;
	char *end_local;
	long long value_local;
	char output_local1[64];
	int length_local2;
	char output_local3[64];
	int length_local4;
	char *end_local5;
	size_t index_local;
	char *end_local6;
	long diversion_value;
	char output_local7[32];
	int length_local8;
	char *end_local9;
	long value_local10;
	size_t index_local12;
	char *value_local11;
	char *file_text;
	size_t file_length;
	const char *saved_source;
	unsigned saved_line;
	const char *found;
	unsigned long long start;
	unsigned long long count;
	size_t available;
	const char *mapped;
	char *a;
	char *b;
	char *c;
	int result;
	struct builder translated;

	a = NULL;
	b = NULL;
	c = NULL;
	result = -1;

#define BUILTIN(word)                                                          \
	(name_length == sizeof(word) - 1 &&                                    \
	 memcmp(name, word, sizeof(word) - 1) == 0)

	/* Handles the dnl condition. */
	if (BUILTIN("dnl")) {
		context->discard_line = 1;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the define condition. */
	if (BUILTIN("define") || BUILTIN("undefine") || BUILTIN("ifdef") ||
	    BUILTIN("ifelse") || BUILTIN("include") || BUILTIN("sinclude") ||
	    BUILTIN("len") || BUILTIN("incr") || BUILTIN("decr") ||
	    BUILTIN("eval") || BUILTIN("index") || BUILTIN("substr") ||
	    BUILTIN("translit") || BUILTIN("divert") || BUILTIN("undivert") ||
	    BUILTIN("changequote") || BUILTIN("errprint") || BUILTIN("shift") ||
	    BUILTIN("m4exit")) {
		a = expanded_argument(context, arguments, 0, depth);

		/* Handles the a availability. */
		if (a == NULL)
			goto done;
	}

	/* Handles the define condition. */
	if (BUILTIN("define")) {
		b = expanded_argument(context, arguments, 1, depth);

		/* Handles the b availability. */
		if (b == NULL)
			goto done;
		result = m4_define(context, a, b);
	} else if (BUILTIN("undefine"))
		result = m4_undefine(context, a);
	else if (BUILTIN("ifdef")) {
		b = expanded_argument(
		    context, arguments,
		    find_definition(context, a, strlen(a)) != NULL ? 1 : 2,
		    depth);
		result = b == NULL ? -1 : emit(context, capture, b, strlen(b));
	} else if (BUILTIN("ifelse")) {
		b = expanded_argument(context, arguments, 1, depth);

		/* Handles the b availability. */
		if (b == NULL)
			goto done;
		c = expanded_argument(context, arguments,
				      strcmp(a, b) == 0 ? 2 : 3, depth);
		result = c == NULL ? -1 : emit(context, capture, c, strlen(c));
	} else if (BUILTIN("include") || BUILTIN("sinclude")) {
		saved_source = context->source;
		saved_line = context->line;

		/* Handles a failed read file operation. */
		if (read_file(a, &file_text, &file_length) != 0) {
			/* Handles the sinclude condition. */
			if (BUILTIN("sinclude"))
				result = 0;
			else
				set_error(context, "cannot read included file");
			goto done;
		}
		context->source = a;
		context->line = 1;
		result = expand_text(context, file_text, file_length, capture,
				     depth + 1);
		context->source = saved_source;
		context->line = saved_line;
		free(file_text);
	} else if (BUILTIN("len")) {
		length_local = snprintf(output_local, sizeof(output_local), "%zu", strlen(a));

		result = length_local < 0 || (size_t)length_local >= sizeof(output_local)
			     ? -1
			     : emit(context, capture, output_local, (size_t)length_local);
	} else if (BUILTIN("incr") || BUILTIN("decr")) {
		errno = 0;
		value_local = strtoll(a, &end_local, 10);

		/* Handles the reported system error. */
		if (errno != 0 || *a == '\0' || *end_local != '\0' ||
		    (BUILTIN("incr") && value_local == LLONG_MAX) ||
		    (BUILTIN("decr") && value_local == LLONG_MIN)) {
			set_error(context, "invalid integer");
			goto done;
		}
		value_local += BUILTIN("incr") ? 1 : -1;
		length_local2 = snprintf(output_local1, sizeof(output_local1), "%lld", value_local);
		result = length_local2 < 0 || (size_t)length_local2 >= sizeof(output_local1)
			     ? -1
			     : emit(context, capture, output_local1, (size_t)length_local2);
	} else if (BUILTIN("eval"))
		result = builtin_eval(context, a, capture);
	else if (BUILTIN("index")) {
		b = expanded_argument(context, arguments, 1, depth);

		/* Handles the b availability. */
		if (b == NULL)
			goto done;
		found = strstr(a, b);
		length_local4 =
		    snprintf(output_local3, sizeof(output_local3), "%lld",
			     found == NULL ? -1LL : (long long)(found - a));
		result = length_local4 < 0 || (size_t)length_local4 >= sizeof(output_local3)
			     ? -1
			     : emit(context, capture, output_local3, (size_t)length_local4);
	} else if (BUILTIN("substr")) {
		count = ULLONG_MAX;

		b = expanded_argument(context, arguments, 1, depth);
		c = expanded_argument(context, arguments, 2, depth);

		/* Handles the b availability. */
		if (b == NULL || c == NULL)
			goto done;
		errno = 0;
		start = strtoull(b, &end_local5, 10);

		/* Handles the reported system error. */
		if (errno != 0 || *b == '\0' || *end_local5 != '\0')
			goto done;

		/* Handles the arguments condition. */
		if (arguments->count > 2) {
			errno = 0;
			count = strtoull(c, &end_local5, 10);

			/* Handles the reported system error. */
			if (errno != 0 || *c == '\0' || *end_local5 != '\0')
				goto done;
		}
		available = strlen(a);

		/* Handles the start condition. */
		if (start >= available) {
			result = 0;
		} else {
			available -= (size_t)start;

			/* Checks the remaining item count. */
			if (count < available)
				available = (size_t)count;
			result = emit(context, capture, a + start, available);
		}
	} else if (BUILTIN("translit")) {
		memset(&translated, 0, sizeof(translated));
		b = expanded_argument(context, arguments, 1, depth);
		c = expanded_argument(context, arguments, 2, depth);

		/* Handles the b availability. */
		if (b == NULL || c == NULL)
			goto done;

		/* Process each remaining element. */
		for (index_local = 0; a[index_local] != '\0'; index_local++) {
			mapped = strchr(b, a[index_local]);

			/* Handles the mapped availability. */
			if (mapped == NULL) {
				/* Handles a failed builder append operation. */
				if (builder_append(context, &translated,
						   a + index_local, 1) != 0)
					goto translit_done;
			} else if ((size_t)(mapped - b) < strlen(c) &&
				   builder_append(context, &translated,
						  c + (mapped - b), 1) != 0)
				goto translit_done;
		}
		result = emit(context, capture,
			      translated.text == NULL ? "" : translated.text,
			      translated.length);
	translit_done:
		builder_free(&translated);
	} else if (BUILTIN("divert")) {
		diversion_value = *a == '\0' ? 0 : strtol(a, &end_local6, 10);

		/* Handles the a condition. */
		if ((*a != '\0' && *end_local6 != '\0') || diversion_value < -1 ||
		    diversion_value >= M4_DIVERSION_COUNT) {
			set_error(context, "invalid diversion");
			goto done;
		}
		context->current_diversion = (int)diversion_value;
		result = 0;
	} else if (BUILTIN("divnum")) {
		length_local8 = snprintf(output_local7, sizeof(output_local7), "%d",
		      context->current_diversion);

		result = emit(context, capture, output_local7, (size_t)length_local8);
	} else if (BUILTIN("undivert")) {
		value_local10 = strtol(a, &end_local9, 10);

		/* Handles the a condition. */
		if (*a == '\0') {
			/* Process each remaining element. */
			for (value_local10 = 1; value_local10 < M4_DIVERSION_COUNT; value_local10++) {
				/* Handles a failed emit operation. */
				if (emit(context, capture,
					 context->diversion[value_local10].text == NULL
					     ? ""
					     : context->diversion[value_local10].text,
					 context->diversion[value_local10].length) != 0)
					goto done;
				builder_free(&context->diversion[value_local10]);
			}
			result = 0;
		} else if (*end_local9 == '\0' && value_local10 > 0 &&
			   value_local10 < M4_DIVERSION_COUNT) {
			result = emit(context, capture,
				      context->diversion[value_local10].text == NULL
					  ? ""
					  : context->diversion[value_local10].text,
				      context->diversion[value_local10].length);
			builder_free(&context->diversion[value_local10]);
		}
	} else if (BUILTIN("changequote")) {
		b = expanded_argument(context, arguments, 1, depth);

		/* Handles a failed strlen operation. */
		if (b == NULL || strlen(a) >= sizeof(context->quote_open) ||
		    strlen(b) >= sizeof(context->quote_close))
			goto done;
		strcpy(context->quote_open, *a == '\0' ? "`" : a);
		strcpy(context->quote_close, *b == '\0' ? "'" : b);
		result = 0;
	} else if (BUILTIN("errprint")) {
		result = fputs(a, stderr) == EOF ? -1 : 0;
	} else if (BUILTIN("shift")) {
		/* Process each remaining element. */
		result = 0;
		for (index_local12 = 1; index_local12 < arguments->count; index_local12++) {
			value_local11 = expanded_argument(context, arguments, index_local12, depth);

			/* Handles a failed emit operation. */
			if (value_local11 == NULL ||
			    (index_local12 != 1 &&
			     emit(context, capture, ",", 1) != 0) ||
			    emit(context, capture, value_local11, strlen(value_local11)) != 0) {
				free(value_local11);
				result = -1;
				break;
			}
			free(value_local11);
		}
	} else if (BUILTIN("m4exit")) {
		set_error(context, "m4exit is not implemented locally");
		result = -1;
	} else {
		set_error(context, "unsupported builtin");
		result = -1;
	}
done:
	free(a);
	free(b);
	free(c);

	/* Handles an operation failure. */
	if (result != 0 && context->error[0] == '\0') {
		set_error(context, errno == ENOMEM
				       ? "out of memory"
				       : "invalid builtin arguments");
	}

	/* Returns the computed result. */
	return result;
#undef BUILTIN
}

/* Supports the read file operation. */
static int
read_file(
	const char *path,
	char **result,
	size_t *result_length)
{
	size_t capacity;
	char *grown;
	FILE *stream;
	struct builder builder;
	char buffer[4096];
	size_t count;

	stream = fopen(path, "r");
	memset(&builder, 0, sizeof(builder));

	/* Handles the stream availability. */
	if (stream == NULL)
		return -1;

	/* Process input until it is exhausted. */
	while ((count = fread(buffer, 1, sizeof(buffer), stream)) != 0) {
		/* Handles the builder condition. */
		if (builder.length > M4_OUTPUT_MAX - count) {
			errno = EOVERFLOW;
			goto fail;
		}

		/* Handles the builder condition. */
		if (builder.length + count + 1 > builder.capacity) {
			capacity = builder.capacity == 0 ? 4096 : builder.capacity;

			/* Process each remaining element. */
			while (capacity < builder.length + count + 1)
				capacity *= 2;
			grown = realloc(builder.text, capacity);

			/* Handles the grown availability. */
			if (grown == NULL)
				goto fail;
			builder.text = grown;
			builder.capacity = capacity;
		}
		memcpy(builder.text + builder.length, buffer, count);
		builder.length += count;
	}

	/* Handles an operation failure. */
	if (ferror(stream) || fclose(stream) != 0)
		goto fail_closed;

	/* Handles the text availability. */
	if (builder.text == NULL) {
		builder.text = malloc(1);

		/* Handles the text availability. */
		if (builder.text == NULL)
			return -1;
	}
	builder.text[builder.length] = '\0';
	*result = builder.text;
	*result_length = builder.length;
	/* Reports successful completion. */
	return 0;

fail:
	(void)fclose(stream);
fail_closed:
	builder_free(&builder);

	/* Reports operation failure. */
	return -1;
}

/* Supports the builtin eval operation. */
static int
builtin_eval(
	struct m4_context *context,
	const char *expression,
	struct builder *capture)
{
	int function_result;
	struct expression_parser parser = {expression, 0};
	long long value;
	char output[64];
	int length;

	value = expression_add(&parser);

	expression_space(&parser);

	/* Handles an operation failure. */
	if (parser.failed || *parser.text != '\0') {
		set_error(context, parser.failed == 2 ? "division by zero"
						      : "invalid expression");

		/* Reports operation failure. */
		return -1;
	}
	length = snprintf(output, sizeof(output), "%lld", value);

	/* Computes the function result. */
	function_result = length < 0 || (size_t)length >= sizeof(output)
		   ? -1
		   : emit(context, capture, output, (size_t)length);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the expression add operation. */
static long long
expression_add(
	struct expression_parser *parser)
{
	char operation;
	long long right;
	long long value;

	value = expression_product(parser);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		expression_space(parser);
		operation = *parser->text;

		/* Validates the selected operation. */
		if (operation != '+' && operation != '-')
			break;
		parser->text++;
		right = expression_product(parser);
		value = operation == '+' ? value + right : value - right;
	}

	/* Returns the computed result. */
	return value;
}

/* Supports the expression product operation. */
static long long
expression_product(
	struct expression_parser *parser)
{
	char operation;
	long long right;
	long long value;

	value = expression_unary(parser);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		expression_space(parser);
		operation = *parser->text;

		/* Validates the selected operation. */
		if (operation != '*' && operation != '/' && operation != '%')
			break;
		parser->text++;
		right = expression_unary(parser);

		/* Validates the selected operation. */
		if ((operation == '/' || operation == '%') && right == 0) {
			parser->failed = 2;

			/* Reports successful completion. */
			return 0;
		}

		/* Validates the selected operation. */
		if (operation == '*')
			value *= right;
		else if (operation == '/')
			value /= right;
		else
			value %= right;
	}

	/* Returns the computed result. */
	return value;
}

/* Supports the expression unary operation. */
static long long
expression_unary(
	struct expression_parser *parser)
{
	long long function_result;

	expression_space(parser);

	/* Checks the parser state. */
	if (*parser->text == '+') {
		parser->text++;

		/* Obtains the expression unary result. */
		function_result = expression_unary(parser);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the parser state. */
	if (*parser->text == '-') {
		parser->text++;

		/* Computes the function result. */
		function_result = -expression_unary(parser);

		/* Returns the computed result. */
		return function_result;
	}

	/* Obtains the expression primary result. */
	function_result = expression_primary(parser);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the expression space operation. */
static void
expression_space(
	struct expression_parser *parser)
{
	/* Continue while the operation condition remains true. */
	while (*parser->text == ' ' || *parser->text == '\t')
		parser->text++;
}

/* Supports the expression primary operation. */
static long long
expression_primary(
	struct expression_parser *parser)
{
	long long value;
	char *end;

	expression_space(parser);

	/* Checks the parser state. */
	if (*parser->text == '(') {
		parser->text++;
		value = expression_add(parser);
		expression_space(parser);

		/* Checks the parser state. */
		if (*parser->text++ != ')')
			parser->failed = 1;

		/* Returns the computed result. */
		return value;
	}
	errno = 0;
	value = strtoll(parser->text, &end, 0);

	/* Handles the reported system error. */
	if (end == parser->text || errno != 0) {
		parser->failed = 1;

		/* Reports successful completion. */
		return 0;
	}
	parser->text = end;

	/* Returns the computed result. */
	return value;
}
