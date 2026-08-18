/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/sh/expand.h"
#include "userland/sh/arithmetic.h"

#include <stdlib.h>
#include <string.h>

struct expand_buffer {
	char *data;
	unsigned char *quoted;
	size_t length;
	size_t capacity;
	int preserve_empty;
};

static void
buffer_free(struct expand_buffer *buffer)
{
	free(buffer->data);
	free(buffer->quoted);
	memset(buffer, 0, sizeof(*buffer));
}

static int
buffer_reserve(struct expand_buffer *buffer, size_t additional)
{
	char *data;
	unsigned char *quoted;
	size_t needed = buffer->length + additional + 1U;
	size_t capacity = buffer->capacity == 0 ? 32U : buffer->capacity;
	if (needed < buffer->length)
		return 0;
	while (capacity < needed) {
		if (capacity > (size_t)-1 / 2U)
			return 0;
		capacity *= 2U;
	}
	if (capacity == buffer->capacity)
		return 1;
	data = realloc(buffer->data, capacity);
	if (data == NULL)
		return 0;
	buffer->data = data;
	quoted = realloc(buffer->quoted, capacity);
	if (quoted == NULL)
		return 0;
	buffer->quoted = quoted;
	buffer->capacity = capacity;
	return 1;
}

static int
append_bytes(struct expand_buffer *buffer, const char *data, size_t length,
    int quoted)
{
	if (!buffer_reserve(buffer, length))
		return 0;
	memcpy(buffer->data + buffer->length, data, length);
	memset(buffer->quoted + buffer->length, quoted != 0, length);
	buffer->length += length;
	buffer->data[buffer->length] = '\0';
	return 1;
}

static int
name_start(char value)
{
	return (value >= 'A' && value <= 'Z') ||
	    (value >= 'a' && value <= 'z') || value == '_';
}

static int
name_character(char value)
{
	return name_start(value) || (value >= '0' && value <= '9');
}

static int
append_number(struct expand_buffer *buffer, long value, int quoted)
{
	char digits[32];
	unsigned long magnitude;
	size_t position = sizeof(digits);
	int negative = value < 0;
	if (negative)
		magnitude = (unsigned long)(-(value + 1L)) + 1UL;
	else
		magnitude = (unsigned long)value;
	do {
		digits[--position] = (char)('0' + magnitude % 10UL);
		magnitude /= 10UL;
	} while (magnitude != 0);
	if (negative)
		digits[--position] = '-';
	return append_bytes(buffer, digits + position,
	    sizeof(digits) - position, quoted);
}

static int
append_positionals(struct expand_buffer *buffer,
    const struct sh_expand_context *context, int quoted)
{
	const char *ifs = context->lookup == NULL ? getenv("IFS") :
	    context->lookup(context->lookup_context, "IFS");
	char separator = ifs == NULL ? ' ' : ifs[0];
	int index;
	for (index = 0; index < context->positional_count; index++) {
		if (index != 0 && separator != '\0' &&
		    !append_bytes(buffer, &separator, 1U, quoted))
			return 0;
		if (!append_bytes(buffer, context->positional[index],
		    strlen(context->positional[index]), quoted))
			return 0;
	}
	return 1;
}

static int
append_parameter(struct expand_buffer *buffer, const char *name,
    size_t length, int quoted, const struct sh_expand_context *context)
{
	char *copy;
	const char *value;
	copy = malloc(length + 1U);
	if (copy == NULL)
		return 0;
	memcpy(copy, name, length);
	copy[length] = '\0';
	value = context->lookup == NULL ? getenv(copy) :
	    context->lookup(context->lookup_context, copy);
	free(copy);
	return value == NULL || append_bytes(buffer, value, strlen(value), quoted);
}

static const char *
lookup_parameter(const struct sh_expand_context *context, const char *name,
    size_t length, char **allocated_name)
{
	const char *value;
	*allocated_name = malloc(length + 1U);
	if (*allocated_name == NULL)
		return NULL;
	memcpy(*allocated_name, name, length);
	(*allocated_name)[length] = '\0';
	value = context->lookup == NULL ? getenv(*allocated_name) :
	    context->lookup(context->lookup_context, *allocated_name);
	return value;
}

static int
append_buffer(struct expand_buffer *target, const struct expand_buffer *source)
{
	if (!buffer_reserve(target, source->length))
		return 0;
	memcpy(target->data + target->length, source->data, source->length);
	memcpy(target->quoted + target->length, source->quoted, source->length);
	target->length += source->length;
	target->data[target->length] = '\0';
	if (source->preserve_empty)
		target->preserve_empty = 1;
	return 1;
}

static int
expand_raw(const struct sh_token *token,
    const struct sh_expand_context *context, struct expand_buffer *buffer,
    const char **error_text)
{
	size_t index = 0;
	memset(buffer, 0, sizeof(*buffer));
	buffer->preserve_empty = token->length == 0;
	*error_text = NULL;
	while (index < token->length) {
		unsigned char quote = token->quote[index];
		char value = token->text[index];
		int output_quoted = quote != SH_QUOTE_UNQUOTED;
		if (value == '~' && quote == SH_QUOTE_UNQUOTED &&
		    (index == 0 || (token->text[index - 1U] == '=' &&
		    token->quote[index - 1U] == SH_QUOTE_UNQUOTED)) &&
		    (index + 1U == token->length || token->text[index + 1U] == '/')) {
			const char *home = context->lookup == NULL ? getenv("HOME") :
			    context->lookup(context->lookup_context, "HOME");
			if (home != NULL && !append_bytes(buffer, home, strlen(home), 0))
				goto no_memory;
			if (home == NULL && !append_bytes(buffer, "~", 1U, 0))
				goto no_memory;
			index++;
			continue;
		}
		if (output_quoted)
			buffer->preserve_empty = 1;
		if (value != '$' || quote == SH_QUOTE_SINGLE ||
		    quote == SH_QUOTE_ESCAPED || index + 1U == token->length) {
			if (!append_bytes(buffer, &value, 1U, output_quoted))
				goto no_memory;
			index++;
			continue;
		}
		output_quoted = quote == SH_QUOTE_DOUBLE;
		value = token->text[index + 1U];
		if (value == '(') {
			if (index + 2U < token->length &&
			    token->text[index + 2U] == '(') {
				size_t scan = index + 3U;
				size_t start = scan;
				int depth = 1;
				char *expression;
				long arithmetic_value;
				while (scan < token->length && depth != 0) {
					if (token->text[scan] == '(') depth++;
					else if (token->text[scan] == ')') depth--;
					if (depth != 0) scan++;
				}
				if (depth != 0 || scan + 1U >= token->length ||
				    token->text[scan + 1U] != ')') {
					*error_text = "unterminated arithmetic expansion";
					buffer_free(buffer);
					return 0;
				}
				expression = malloc(scan - start + 1U);
				if (expression == NULL)
					goto no_memory;
				memcpy(expression, token->text + start, scan - start);
				expression[scan - start] = '\0';
				if (!sh_arithmetic_eval(expression, context->lookup,
				    context->lookup_context, &arithmetic_value, error_text)) {
					free(expression);
					buffer_free(buffer);
					return 0;
				}
				free(expression);
				if (!append_number(buffer, arithmetic_value, output_quoted))
					goto no_memory;
				index = scan + 2U;
				continue;
			}
			size_t scan = index + 2U;
			size_t start = scan;
			int depth = 1;
			char inner_quote = '\0';
			char *source, *substitution = NULL;
			while (scan < token->length && depth != 0) {
				char current = token->text[scan++];
				if (current == '\\' && scan < token->length) {
					scan++;
					continue;
				}
				if ((current == '\'' || current == '"') &&
				    (inner_quote == '\0' || inner_quote == current))
					inner_quote = inner_quote == '\0' ? current : '\0';
				if (inner_quote == '\0') {
					if (current == '(') depth++;
					if (current == ')') depth--;
				}
			}
			if (depth != 0) {
				*error_text = "unterminated command substitution";
				buffer_free(buffer);
				return 0;
			}
			source = malloc(scan - start);
			if (source == NULL)
				goto no_memory;
			memcpy(source, token->text + start, scan - start - 1U);
			source[scan - start - 1U] = '\0';
			if (context->command_substitute == NULL ||
			    !context->command_substitute(context->lookup_context, source,
			    &substitution)) {
				free(source);
				free(substitution);
				*error_text = "command substitution failed";
				buffer_free(buffer);
				return 0;
			}
			free(source);
			if (!append_bytes(buffer, substitution, strlen(substitution),
			    output_quoted)) {
				free(substitution);
				goto no_memory;
			}
			free(substitution);
			index = scan;
			continue;
		}
		if (value == '?' || value == '$' || value == '!' || value == '#') {
			long number = value == '?' ? context->status :
			    value == '$' ? context->shell_pid : value == '!' ?
			    context->last_job : context->positional_count;
			if (!append_number(buffer, number, output_quoted))
				goto no_memory;
			index += 2U;
			continue;
		}
		if (value == '*' || value == '@') {
			if (!append_positionals(buffer, context, output_quoted))
				goto no_memory;
			index += 2U;
			continue;
		}
		if (value >= '0' && value <= '9') {
			const char *argument = value == '0' ? context->shell_name :
			    value - '1' < context->positional_count ?
			    context->positional[value - '1'] : NULL;
			if (argument != NULL && !append_bytes(buffer, argument,
			    strlen(argument), output_quoted))
				goto no_memory;
			index += 2U;
			continue;
		}
		if (value == '{') {
			size_t start = index + 2U;
			size_t name_end = start;
			size_t end;
			int colon = 0;
			char operation = '\0';
			char *name = NULL;
			const char *parameter;
			int set, use_word = 0;
			struct expand_buffer word;
			struct sh_token word_token;
			while (name_end < token->length &&
			    name_character(token->text[name_end]))
				name_end++;
			if (name_end == start || !name_start(token->text[start])) {
				*error_text = "invalid parameter name";
				buffer_free(buffer);
				return 0;
			}
			end = name_end;
			if (end < token->length && token->text[end] == ':') {
				colon = 1;
				end++;
			}
			if (end < token->length && strchr("-+=?", token->text[end]) != NULL)
				operation = token->text[end++];
			else if (colon) {
				*error_text = "unsupported parameter expansion";
				buffer_free(buffer);
				return 0;
			}
			if (operation == '\0' && (end >= token->length ||
			    token->text[end] != '}')) {
				*error_text = "unsupported parameter expansion";
				buffer_free(buffer);
				return 0;
			}
			{
				size_t depth = 1U;
				size_t scan = end;
				while (scan < token->length && depth != 0) {
					if (scan + 1U < token->length &&
					    token->text[scan] == '$' &&
					    token->text[scan + 1U] == '{') {
						depth++;
						scan += 2U;
						continue;
					}
					if (token->text[scan] == '}') depth--;
					if (depth != 0) scan++;
				}
				if (depth != 0) {
					*error_text = "unterminated parameter expansion";
					buffer_free(buffer);
					return 0;
				}
				name_end = scan;
			}
			parameter = lookup_parameter(context, token->text + start,
			    (size_t)(strchr(token->text + start, operation == '\0' ? '}' :
			    colon ? ':' : operation) - (token->text + start)), &name);
			if (name == NULL)
				goto no_memory;
			set = parameter != NULL && (!colon || parameter[0] != '\0');
			if (operation == '\0') {
				if (parameter != NULL && !append_bytes(buffer, parameter,
				    strlen(parameter), output_quoted)) {
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
			if (!expand_raw(&word_token, context, &word, error_text)) {
				free(name);
				buffer_free(buffer);
				return 0;
			}
			if (operation == '?' && !set) {
				*error_text = word.length == 0 ?
				    "parameter is unset or null" :
				    "parameter expansion requested an error";
				buffer_free(&word);
				free(name);
				buffer_free(buffer);
				return 0;
			}
			if (operation == '=' && !set) {
				if (context->assign == NULL ||
				    context->assign(context->lookup_context, name,
				    word.data) != 0) {
					*error_text = "parameter assignment failed";
					buffer_free(&word);
					free(name);
					buffer_free(buffer);
					return 0;
				}
				use_word = 1;
			}
			if (use_word) {
				if (!append_buffer(buffer, &word)) {
					buffer_free(&word);
					free(name);
					goto no_memory;
				}
			} else if (parameter != NULL && operation != '+' &&
			    !append_bytes(buffer, parameter, strlen(parameter),
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
		if (name_start(value)) {
			size_t start = index + 1U;
			size_t end = start + 1U;
			while (end < token->length &&
			    name_character(token->text[end]))
				end++;
			if (!append_parameter(buffer, token->text + start,
			    end - start, output_quoted, context))
				goto no_memory;
			index = end;
			continue;
		}
		if (!append_bytes(buffer, "$", 1U, output_quoted))
			goto no_memory;
		index++;
	}
	if (!buffer_reserve(buffer, 0))
		goto no_memory;
	buffer->data[buffer->length] = '\0';
	return 1;
no_memory:
	buffer_free(buffer);
	*error_text = "out of memory";
	return 0;
}

int
sh_expand_word(const struct sh_token *token,
    const struct sh_expand_context *context, char **result,
    const char **error_text)
{
	struct expand_buffer buffer;
	*result = NULL;
	if (!expand_raw(token, context, &buffer, error_text))
		return 0;
	*result = buffer.data;
	free(buffer.quoted);
	return 1;
}

static int
is_ifs(char value, const char *ifs)
{
	return strchr(ifs, value) != NULL;
}

static int
field_append(struct sh_field_list *list, const char *data,
    const unsigned char *quoted_data, size_t length, int quoted_default)
{
	char **larger;
	unsigned char **larger_quoted;
	char *field = malloc(length + 1U);
	unsigned char *quoted = malloc(length == 0 ? 1U : length);
	if (field == NULL || quoted == NULL) {
		free(field);
		free(quoted);
		return 0;
	}
	memcpy(field, data, length);
	field[length] = '\0';
	if (quoted_data != NULL)
		memcpy(quoted, quoted_data, length);
	else
		memset(quoted, quoted_default != 0, length);
	larger = realloc(list->fields, (list->count + 1U) * sizeof(*larger));
	if (larger == NULL) {
		free(field);
		free(quoted);
		return 0;
	}
	list->fields = larger;
	larger_quoted = realloc(list->quoted,
	    (list->count + 1U) * sizeof(*larger_quoted));
	if (larger_quoted == NULL) {
		free(field);
		free(quoted);
		return 0;
	}
	list->quoted = larger_quoted;
	list->fields[list->count++] = field;
	list->quoted[list->count - 1U] = quoted;
	return 1;
}

int
sh_expand_fields(const struct sh_token *token,
    const struct sh_expand_context *context, struct sh_field_list *list,
    const char **error_text)
{
	struct expand_buffer buffer;
	const char *ifs = context->lookup == NULL ? getenv("IFS") :
	    context->lookup(context->lookup_context, "IFS");
	size_t position = 0;
	memset(list, 0, sizeof(*list));
	if (token->length == 2 && token->text[0] == '$' &&
	    token->text[1] == '@' && token->quote[0] != SH_QUOTE_SINGLE &&
	    token->quote[0] != SH_QUOTE_ESCAPED) {
		int argument;
		for (argument = 0; argument < context->positional_count; argument++) {
			if (!field_append(list, context->positional[argument], NULL,
			    strlen(context->positional[argument]),
			    token->quote[0] == SH_QUOTE_DOUBLE))
				goto direct_no_memory;
		}
		*error_text = NULL;
		return 1;
	}
	if (ifs == NULL)
		ifs = " \t\n";
	if (!expand_raw(token, context, &buffer, error_text))
		return 0;
	while (position < buffer.length) {
		size_t start;
		while (position < buffer.length && !buffer.quoted[position] &&
		    is_ifs(buffer.data[position], ifs))
			position++;
		start = position;
		while (position < buffer.length && (buffer.quoted[position] ||
		    !is_ifs(buffer.data[position], ifs)))
			position++;
		if (position != start && !field_append(list, buffer.data + start,
		    buffer.quoted + start, position - start, 0))
			goto no_memory;
	}
	if (list->count == 0 && buffer.preserve_empty &&
	    !field_append(list, "", NULL, 0, 1))
		goto no_memory;
	buffer_free(&buffer);
	return 1;
no_memory:
	buffer_free(&buffer);
	sh_fields_free(list);
	*error_text = "out of memory";
	return 0;
direct_no_memory:
	sh_fields_free(list);
	*error_text = "out of memory";
	return 0;
}

void
sh_fields_free(struct sh_field_list *list)
{
	size_t index;
	for (index = 0; index < list->count; index++)
		free(list->fields[index]);
	for (index = 0; index < list->count; index++)
		free(list->quoted[index]);
	free(list->fields);
	free(list->quoted);
	list->fields = NULL;
	list->quoted = NULL;
	list->count = 0;
}
