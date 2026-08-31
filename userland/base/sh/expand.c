/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland expand component.
 */

#include "userland/base/sh/expand.h"
#include "userland/base/sh/arithmetic.h"

#include <stdlib.h>
#include <string.h>

struct expand_buffer {
	char *data;
	unsigned char *quoted;
	size_t length;
	size_t capacity;
	int preserve_empty;
};

static int expand_raw(const struct sh_token *token, const struct sh_expand_context *context, struct expand_buffer *buffer, const char **error_text);
static int append_bytes(struct expand_buffer *buffer, const char *data, size_t length, int quoted);
static int buffer_reserve(struct expand_buffer *buffer, size_t additional);
static void buffer_free(struct expand_buffer *buffer);
static int append_number(struct expand_buffer *buffer, long value, int quoted);
static int append_positionals(struct expand_buffer *buffer, const struct sh_expand_context *context, int quoted);
static int name_character(char value);
static int name_start(char value);
static const char *lookup_parameter(const struct sh_expand_context *context, const char *name, size_t length, char **allocated_name);
static int append_buffer(struct expand_buffer *target, const struct expand_buffer *source);
static int append_parameter(struct expand_buffer *buffer, const char *name, size_t length, int quoted, const struct sh_expand_context *context);
static int field_append(struct sh_field_list *list, const char *data, const unsigned char *quoted_data, size_t length, int quoted_default);
static int is_ifs(char value, const char *ifs);

/*
 * Implements the sh expand word operation.
 */
int
sh_expand_word(
	const struct sh_token *token,
	const struct sh_expand_context *context,
	char **result,
	const char **error_text)
{
	struct expand_buffer buffer;

	*result = NULL;
	/* Handles an operation failure. */
	if (!expand_raw(token, context, &buffer, error_text))
		return 0;
	*result = buffer.data;
	free(buffer.quoted);

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the sh expand fields operation.
 */
int
sh_expand_fields(
	const struct sh_token *token,
	const struct sh_expand_context *context,
	struct sh_field_list *list,
	const char **error_text)
{
	int argument;
	size_t start;
	struct expand_buffer buffer;
	const char *ifs = context->lookup == NULL
			      ? getenv("IFS")
			      : context->lookup(context->lookup_context, "IFS");
	size_t position;

	position = 0;
	memset(list, 0, sizeof(*list));

	/* Handles the token condition. */
	if (token->length == 2 && token->text[0] == '$' &&
	    token->text[1] == '@' && token->quote[0] != SH_QUOTE_SINGLE &&
	    token->quote[0] != SH_QUOTE_ESCAPED) {
		/* Process each remaining element. */
		for (argument = 0; argument < context->positional_count;
		     argument++) {
			/* Handles a failed field append operation. */
			if (!field_append(list, context->positional[argument],
					  NULL,
					  strlen(context->positional[argument]),
					  token->quote[0] == SH_QUOTE_DOUBLE))
				goto direct_no_memory;
		}
		*error_text = NULL;
		/* Reports operation failure. */
		return 1;
	}

	/* Handles the ifs availability. */
	if (ifs == NULL)
		ifs = " \t\n";

	/* Handles an operation failure. */
	if (!expand_raw(token, context, &buffer, error_text))
		return 0;

	/* Process each remaining element. */
	while (position < buffer.length) {
		/* Process each remaining element. */
		while (position < buffer.length && !buffer.quoted[position] &&
		       is_ifs(buffer.data[position], ifs))
			position++;

		/* Process each remaining element. */
		start = position;
		while (position < buffer.length &&
		       (buffer.quoted[position] ||
			!is_ifs(buffer.data[position], ifs)))
			position++;

		/* Handles a failed field append operation. */
		if (position != start &&
		    !field_append(list, buffer.data + start,
				  buffer.quoted + start, position - start, 0))
			goto no_memory;
	}

	/* Handles a failed field append operation. */
	if (list->count == 0 && buffer.preserve_empty &&
	    !field_append(list, "", NULL, 0, 1))
		goto no_memory;
	buffer_free(&buffer);

	/* Reports operation failure. */
	return 1;
no_memory:
	buffer_free(&buffer);
	sh_fields_free(list);
	*error_text = "out of memory";
	/* Reports successful completion. */
	return 0;
direct_no_memory:
	sh_fields_free(list);
	*error_text = "out of memory";
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sh fields free operation.
 */
void
sh_fields_free(
	struct sh_field_list *list)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < list->count; index++)
		free(list->fields[index]);

	/* Process each remaining element. */
	for (index = 0; index < list->count; index++)
		free(list->quoted[index]);
	free(list->fields);
	free(list->quoted);
	list->fields = NULL;
	list->quoted = NULL;
	list->count = 0;
}

/* Supports the expand raw operation. */
static int
expand_raw(
	const struct sh_token *token,
	const struct sh_expand_context *context,
	struct expand_buffer *buffer,
	const char **error_text)
{
	const char *home;
	long number;
	const char *argument;
	size_t scan_local1;
	size_t start_local2;
	int depth_local3;
	size_t scan_local;
	size_t start_local;
	int depth_local;
	size_t start_local6;
	size_t depth_local4;
	size_t scan_local5;
	size_t start_local7;
	size_t end;
	char *expression;
	long arithmetic_value;
	char current;
	char inner_quote;
	char *source, *substitution;
	size_t name_end;
	int colon;
	char operation;
	char *name;
	const char *parameter;
	int set, use_word;
	struct expand_buffer word;
	struct sh_token word_token;
	unsigned char quote;
	char value;
	size_t index;
	int output_quoted;

	index = 0;
	memset(buffer, 0, sizeof(*buffer));

	/* Process each remaining element. */
	buffer->preserve_empty = token->length == 0;
	*error_text = NULL;
	while (index < token->length) {
		quote = token->quote[index];
		value = token->text[index];
		output_quoted = quote != SH_QUOTE_UNQUOTED;

		/* Validates the current value. */
		if (value == '~' && quote == SH_QUOTE_UNQUOTED &&
		    (index == 0 ||
		     (token->text[index - 1U] == '=' &&
		      token->quote[index - 1U] == SH_QUOTE_UNQUOTED)) &&
		    (index + 1U == token->length ||
		     token->text[index + 1U] == '/')) {
			home = context->lookup == NULL
		? getenv("HOME")
		: context->lookup(context->lookup_context,
			  "HOME");

			/* Handles a failed append bytes operation. */
			if (home != NULL &&
			    !append_bytes(buffer, home, strlen(home), 0))
				goto no_memory;

			/* Handles a failed append bytes operation. */
			if (home == NULL && !append_bytes(buffer, "~", 1U, 0))
				goto no_memory;
			index++;
			continue;
		}

		/* Handles the output quoted condition. */
		if (output_quoted)
			buffer->preserve_empty = 1;

		/* Validates the current value. */
		if (value != '$' || quote == SH_QUOTE_SINGLE ||
		    quote == SH_QUOTE_ESCAPED || index + 1U == token->length) {
			/* Handles a failed append bytes operation. */
			if (!append_bytes(buffer, &value, 1U, output_quoted))
				goto no_memory;
			index++;
			continue;
		}
		output_quoted = quote == SH_QUOTE_DOUBLE;
		value = token->text[index + 1U];

		/* Validates the current value. */
		if (value == '(') {
			/* Checks the current index. */
			if (index + 2U < token->length &&
			    token->text[index + 2U] == '(') {
				scan_local = index + 3U;
				start_local = scan_local;
				depth_local = 1;

				/* Process each remaining element. */
				while (scan_local < token->length && depth_local != 0) {
					/* Handles the token condition. */
					if (token->text[scan_local] == '(')
						depth_local++;
					else if (token->text[scan_local] == ')')
						depth_local--;

					/* Handles the depth local condition. */
					if (depth_local != 0)
						scan_local++;
				}

				/* Handles the depth local condition. */
				if (depth_local != 0 || scan_local + 1U >= token->length ||
				    token->text[scan_local + 1U] != ')') {
					*error_text =
					    "unterminated arithmetic expansion";
					buffer_free(buffer);

					/* Reports successful completion. */
					return 0;
				}
				expression = malloc(scan_local - start_local + 1U);

				/* Handles the expression availability. */
				if (expression == NULL)
					goto no_memory;
				memcpy(expression, token->text + start_local,
				       scan_local - start_local);
				expression[scan_local - start_local] = '\0';

				/* Handles an operation failure. */
				if (!sh_arithmetic_eval(
					expression, context->lookup,
					context->lookup_context,
					&arithmetic_value, error_text)) {
					free(expression);
					buffer_free(buffer);

					/* Reports successful completion. */
					return 0;
				}
				free(expression);

				/* Handles a failed append number operation. */
				if (!append_number(buffer, arithmetic_value,
						   output_quoted))
					goto no_memory;
				index = scan_local + 2U;
				continue;
			}

			/* Process each remaining element. */
			scan_local1 = index + 2U;
			start_local2 = scan_local1;
			depth_local3 = 1;
			inner_quote = '\0';
			substitution = NULL;
			while (scan_local1 < token->length && depth_local3 != 0) {
				current = token->text[scan_local1++];

				/* Handles the current condition. */
				if (current == '\\' && scan_local1 < token->length) {
					scan_local1++;
					continue;
				}

				/* Handles the current condition. */
				if ((current == '\'' || current == '"') &&
				    (inner_quote == '\0' ||
				     inner_quote == current)) {
					inner_quote = inner_quote == '\0'
							  ? current
							  : '\0';
				}

				/* Handles the inner quote condition. */
				if (inner_quote == '\0') {
					/* Handles the current condition. */
					if (current == '(')
						depth_local3++;

					/* Handles the current condition. */
					if (current == ')')
						depth_local3--;
				}
			}

			/* Handles the depth local3 condition. */
			if (depth_local3 != 0) {
				*error_text =
				    "unterminated command substitution";
				buffer_free(buffer);

				/* Reports successful completion. */
				return 0;
			}
			source = malloc(scan_local1 - start_local2);

			/* Handles the source availability. */
			if (source == NULL)
				goto no_memory;
			memcpy(source, token->text + start_local2, scan_local1 - start_local2 - 1U);
			source[scan_local1 - start_local2 - 1U] = '\0';

			/* Handles a failed command substitute operation. */
			if (context->command_substitute == NULL ||
			    !context->command_substitute(
				context->lookup_context, source,
				&substitution)) {
				free(source);
				free(substitution);
				*error_text = "command substitution failed";
				buffer_free(buffer);

				/* Reports successful completion. */
				return 0;
			}
			free(source);

			/* Handles a failed append bytes operation. */
			if (!append_bytes(buffer, substitution,
					  strlen(substitution),
					  output_quoted)) {
				free(substitution);
				goto no_memory;
			}
			free(substitution);
			index = scan_local1;
			continue;
		}

		/* Validates the current value. */
		if (value == '?' || value == '$' || value == '!' ||
		    value == '#') {
			number = value == '?'   ? context->status
		      : value == '$' ? context->shell_pid
		      : value == '!'
		  ? context->last_job
		  : context->positional_count;

			/* Handles a failed append number operation. */
			if (!append_number(buffer, number, output_quoted))
				goto no_memory;
			index += 2U;
			continue;
		}

		/* Validates the current value. */
		if (value == '*' || value == '@') {
			/* Handles a failed append positionals operation. */
			if (!append_positionals(buffer, context, output_quoted))
				goto no_memory;
			index += 2U;
			continue;
		}

		/* Validates the current value. */
		if (value >= '0' && value <= '9') {
			argument = value == '0' ? context->shell_name
		    : value - '1' < context->positional_count
		? context->positional[value - '1']
		: NULL;

			/* Handles a failed append bytes operation. */
			if (argument != NULL &&
			    !append_bytes(buffer, argument, strlen(argument),
					  output_quoted))
				goto no_memory;
			index += 2U;
			continue;
		}

		/* Validates the current value. */
		if (value == '{') {
			start_local6 = index + 2U;
			name_end = start_local6;
			colon = 0;
			operation = '\0';
			name = NULL;

			use_word = 0;

			/* Process each remaining element. */
			while (name_end < token->length &&
			       name_character(token->text[name_end]))
				name_end++;

			/* Handles a failed name start operation. */
			if (name_end == start_local6 ||
			    !name_start(token->text[start_local6])) {
				*error_text = "invalid parameter name";
				buffer_free(buffer);

				/* Reports successful completion. */
				return 0;
			}
			end = name_end;

			/* Checks the current endpoint. */
			if (end < token->length && token->text[end] == ':') {
				colon = 1;
				end++;
			}

			/* Handles a failed strchr operation. */
			if (end < token->length &&
			    strchr("-+=?", token->text[end]) != NULL)
				operation = token->text[end++];
			else if (colon) {
				*error_text = "unsupported parameter expansion";
				buffer_free(buffer);

				/* Reports successful completion. */
				return 0;
			}

			/* Validates the selected operation. */
			if (operation == '\0' &&
			    (end >= token->length || token->text[end] != '}')) {
				*error_text = "unsupported parameter expansion";
				buffer_free(buffer);

				/* Reports successful completion. */
				return 0;
			}

			/* Process each remaining element. */
			depth_local4 = 1U;
			scan_local5 = end;
			while (scan_local5 < token->length && depth_local4 != 0) {
				/* Handles the scan local5 condition. */
				if (scan_local5 + 1U < token->length &&
				    token->text[scan_local5] == '$' &&
				    token->text[scan_local5 + 1U] == '{') {
					depth_local4++;
					scan_local5 += 2U;
					continue;
				}

				/* Handles the token condition. */
				if (token->text[scan_local5] == '}')
					depth_local4--;

				/* Handles the depth local4 condition. */
				if (depth_local4 != 0)
					scan_local5++;
			}

			/* Handles the depth local4 condition. */
			if (depth_local4 != 0) {
				*error_text =
				    "unterminated parameter expansion";
				buffer_free(buffer);

				/* Reports successful completion. */
				return 0;
			}
			name_end = scan_local5;
			parameter = lookup_parameter(
			    context, token->text + start_local6,
			    (size_t)(strchr(token->text + start_local6,
					    operation == '\0' ? '}'
					    : colon	      ? ':'
							      : operation) -
				     (token->text + start_local6)),
			    &name);

			/* Handles the name availability. */
			if (name == NULL)
				goto no_memory;
			set = parameter != NULL &&
			      (!colon || parameter[0] != '\0');

			/* Validates the selected operation. */
			if (operation == '\0') {
				/* Handles a failed append bytes operation. */
				if (parameter != NULL &&
				    !append_bytes(buffer, parameter,
						  strlen(parameter),
						  output_quoted)) {
					free(name);
					goto no_memory;
				}
				free(name);
				index = name_end + 1U;
				continue;
			}
			use_word = operation == '+' ? set : !set;
			memset(&word_token, 0, sizeof(word_token));
			word_token.type = SH_TOKEN_WORD;
			word_token.text = token->text + end;
			word_token.quote = token->quote + end;
			word_token.length = name_end - end;

			/* Handles an operation failure. */
			if (!expand_raw(&word_token, context, &word,
					error_text)) {
				free(name);
				buffer_free(buffer);

				/* Reports successful completion. */
				return 0;
			}

			/* Validates the selected operation. */
			if (operation == '?' && !set) {
				*error_text = word.length == 0
						  ? "parameter is unset or null"
						  : "parameter expansion "
						    "requested an error";
				buffer_free(&word);
				free(name);
				buffer_free(buffer);

				/* Reports successful completion. */
				return 0;
			}

			/* Validates the selected operation. */
			if (operation == '=' && !set) {
				/* Handles a failed assign operation. */
				if (context->assign == NULL ||
				    context->assign(context->lookup_context,
						    name, word.data) != 0) {
					*error_text =
					    "parameter assignment failed";
					buffer_free(&word);
					free(name);
					buffer_free(buffer);

					/* Reports successful completion. */
					return 0;
				}
				use_word = 1;
			}

			/* Handles the use word condition. */
			if (use_word) {
				/* Handles a failed append buffer operation. */
				if (!append_buffer(buffer, &word)) {
					buffer_free(&word);
					free(name);
					goto no_memory;
				}
			} else if (parameter != NULL && operation != '+' &&
				   !append_bytes(buffer, parameter,
						 strlen(parameter),
						 output_quoted)) {
				buffer_free(&word);
				free(name);
				goto no_memory;
			}
			buffer_free(&word);
			free(name);
			index = name_end + 1U;
			continue;
		}

		/* Handles the name start condition. */
		if (name_start(value)) {
			/* Process each remaining element. */
			start_local7 = index + 1U;
			end = start_local7 + 1U;
			while (end < token->length &&
			       name_character(token->text[end]))
				end++;

			/* Handles a failed append parameter operation. */
			if (!append_parameter(buffer, token->text + start_local7,
					      end - start_local7, output_quoted,
					      context))
				goto no_memory;
			index = end;
			continue;
		}

		/* Handles a failed append bytes operation. */
		if (!append_bytes(buffer, "$", 1U, output_quoted))
			goto no_memory;
		index++;
	}

	/* Handles a failed buffer reserve operation. */
	if (!buffer_reserve(buffer, 0))
		goto no_memory;
	buffer->data[buffer->length] = '\0';

	/* Reports operation failure. */
	return 1;
no_memory:
	buffer_free(buffer);
	*error_text = "out of memory";
	/* Reports successful completion. */
	return 0;
}

/* Supports the append bytes operation. */
static int
append_bytes(
	struct expand_buffer *buffer,
	const char *data,
	size_t length,
	int quoted)
{
	/* Handles a failed buffer reserve operation. */
	if (!buffer_reserve(buffer, length))
		return 0;
	memcpy(buffer->data + buffer->length, data, length);
	memset(buffer->quoted + buffer->length, quoted != 0, length);
	buffer->length += length;
	buffer->data[buffer->length] = '\0';

	/* Reports operation failure. */
	return 1;
}

/* Supports the buffer reserve operation. */
static int
buffer_reserve(
	struct expand_buffer *buffer,
	size_t additional)
{
	char *data;
	unsigned char *quoted;
	size_t needed;
	size_t capacity = buffer->capacity == 0 ? 32U : buffer->capacity;

	needed = buffer->length + additional + 1U;

	/* Handles the needed condition. */
	if (needed < buffer->length)
		return 0;

	/* Continue while the operation condition remains true. */
	while (capacity < needed) {
		/* Handles the capacity condition. */
		if (capacity > (size_t)-1 / 2U)
			return 0;
		capacity *= 2U;
	}

	/* Handles the capacity condition. */
	if (capacity == buffer->capacity)
		return 1;
	data = realloc(buffer->data, capacity);

	/* Handles the data availability. */
	if (data == NULL)
		return 0;
	buffer->data = data;
	quoted = realloc(buffer->quoted, capacity);

	/* Handles the quoted availability. */
	if (quoted == NULL)
		return 0;
	buffer->quoted = quoted;
	buffer->capacity = capacity;

	/* Reports operation failure. */
	return 1;
}

/* Supports the buffer free operation. */
static void
buffer_free(
	struct expand_buffer *buffer)
{
	free(buffer->data);
	free(buffer->quoted);
	memset(buffer, 0, sizeof(*buffer));
}

/* Supports the append number operation. */
static int
append_number(
	struct expand_buffer *buffer,
	long value,
	int quoted)
{
	int function_result;
	char digits[32];
	unsigned long magnitude;
	size_t position;
	int negative;

	position = sizeof(digits);
	negative = value < 0;

	/* Handles the negative condition. */
	if (negative)
		magnitude = (unsigned long)(-(value + 1L)) + 1UL;
	else
		magnitude = (unsigned long)value;
	do {
		digits[--position] = (char)('0' + magnitude % 10UL);
		magnitude /= 10UL;
	} while (magnitude != 0);

	/* Handles the negative condition. */
	if (negative)
		digits[--position] = '-';

	/* Obtains the append bytes result. */
	function_result = append_bytes(buffer, digits + position,
			    sizeof(digits) - position, quoted);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the append positionals operation. */
static int
append_positionals(
	struct expand_buffer *buffer,
	const struct sh_expand_context *context,
	int quoted)
{
	const char *ifs = context->lookup == NULL
			      ? getenv("IFS")
			      : context->lookup(context->lookup_context, "IFS");
	char separator = ifs == NULL ? ' ' : ifs[0];
	int index;

	/* Process each remaining element. */
	for (index = 0; index < context->positional_count; index++) {
		/* Handles a failed append bytes operation. */
		if (index != 0 && separator != '\0' &&
		    !append_bytes(buffer, &separator, 1U, quoted))

			/* Reports successful completion. */
			return 0;

		/* Handles a failed append bytes operation. */
		if (!append_bytes(buffer, context->positional[index],
				  strlen(context->positional[index]), quoted))

			/* Reports successful completion. */
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the name character operation. */
static int
name_character(
	char value)
{
	int function_result;

	/* Computes the function result. */
	function_result = name_start(value) || (value >= '0' && value <= '9');

	/* Returns the computed result. */
	return function_result;
}

/* Supports the name start operation. */
static int
name_start(
	char value)
{
	/* Returns the computed result. */
	return (value >= 'A' && value <= 'Z') ||
	       (value >= 'a' && value <= 'z') || value == '_';
}

/* Supports the lookup parameter operation. */
static const char *
lookup_parameter(
	const struct sh_expand_context *context,
	const char *name,
	size_t length,
	char **allocated_name)
{
	const char *value;

	*allocated_name = malloc(length + 1U);
	/* Handles the allocated name availability. */
	if (*allocated_name == NULL)
		return NULL;
	memcpy(*allocated_name, name, length);
	(*allocated_name)[length] = '\0';
	value = context->lookup == NULL
		    ? getenv(*allocated_name)
		    : context->lookup(context->lookup_context, *allocated_name);

	/* Returns the computed result. */
	return value;
}

/* Supports the append buffer operation. */
static int
append_buffer(
	struct expand_buffer *target,
	const struct expand_buffer *source)
{
	/* Handles a failed buffer reserve operation. */
	if (!buffer_reserve(target, source->length))
		return 0;
	memcpy(target->data + target->length, source->data, source->length);
	memcpy(target->quoted + target->length, source->quoted, source->length);
	target->length += source->length;
	target->data[target->length] = '\0';

	/* Handles the source condition. */
	if (source->preserve_empty)
		target->preserve_empty = 1;

	/* Reports operation failure. */
	return 1;
}

/* Supports the append parameter operation. */
static int
append_parameter(
	struct expand_buffer *buffer,
	const char *name,
	size_t length,
	int quoted,
	const struct sh_expand_context *context)
{
	int function_result;
	char *copy;
	const char *value;

	copy = malloc(length + 1U);

	/* Handles the copy availability. */
	if (copy == NULL)
		return 0;
	memcpy(copy, name, length);
	copy[length] = '\0';
	value = context->lookup == NULL
		    ? getenv(copy)
		    : context->lookup(context->lookup_context, copy);
	free(copy);

	/* Computes the function result. */
	function_result = value == NULL ||
	       append_bytes(buffer, value, strlen(value), quoted);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the field append operation. */
static int
field_append(
	struct sh_field_list *list,
	const char *data,
	const unsigned char *quoted_data,
	size_t length,
	int quoted_default)
{
	char **larger;
	unsigned char **larger_quoted;
	char *field;
	unsigned char *quoted;

	field = malloc(length + 1U);
	quoted = malloc(length == 0 ? 1U : length);

	/* Handles the field availability. */
	if (field == NULL || quoted == NULL) {
		free(field);
		free(quoted);

		/* Reports successful completion. */
		return 0;
	}
	memcpy(field, data, length);
	field[length] = '\0';

	/* Handles the quoted data availability. */
	if (quoted_data != NULL)
		memcpy(quoted, quoted_data, length);
	else
		memset(quoted, quoted_default != 0, length);
	larger = realloc(list->fields, (list->count + 1U) * sizeof(*larger));

	/* Handles the larger availability. */
	if (larger == NULL) {
		free(field);
		free(quoted);

		/* Reports successful completion. */
		return 0;
	}
	list->fields = larger;
	larger_quoted =
	    realloc(list->quoted, (list->count + 1U) * sizeof(*larger_quoted));

	/* Handles the larger quoted availability. */
	if (larger_quoted == NULL) {
		free(field);
		free(quoted);

		/* Reports successful completion. */
		return 0;
	}
	list->quoted = larger_quoted;
	list->fields[list->count++] = field;
	list->quoted[list->count - 1U] = quoted;

	/* Reports operation failure. */
	return 1;
}

/* Supports the is ifs operation. */
static int
is_ifs(
	char value,
	const char *ifs)
{
	int function_result;

	/* Computes the function result. */
	function_result = strchr(ifs, value) != NULL;

	/* Returns the computed result. */
	return function_result;
}
