/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/sh/expand.h"

#include <stdlib.h>
#include <string.h>

struct expand_buffer {
	char *data;
	size_t length;
	size_t capacity;
};

static int
append_bytes(struct expand_buffer *buffer, const char *data, size_t length)
{
	char *larger;
	size_t needed = buffer->length + length + 1U;
	size_t capacity = buffer->capacity == 0 ? 32U : buffer->capacity;
	if (needed < buffer->length)
		return 0;
	while (capacity < needed) {
		if (capacity > (size_t)-1 / 2U)
			return 0;
		capacity *= 2U;
	}
	if (capacity != buffer->capacity) {
		larger = realloc(buffer->data, capacity);
		if (larger == NULL)
			return 0;
		buffer->data = larger;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->length, data, length);
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
append_number(struct expand_buffer *buffer, long value)
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
	return append_bytes(buffer, digits + position, sizeof(digits) - position);
}

static int
append_parameter(struct expand_buffer *buffer, const char *name,
    size_t length)
{
	char *copy;
	const char *value;
	copy = malloc(length + 1U);
	if (copy == NULL)
		return 0;
	memcpy(copy, name, length);
	copy[length] = '\0';
	value = getenv(copy);
	free(copy);
	return value == NULL || append_bytes(buffer, value, strlen(value));
}

int
sh_expand_word(const struct sh_token *token,
    const struct sh_expand_context *context, char **result,
    const char **error_text)
{
	struct expand_buffer buffer = { 0 };
	size_t index = 0;
	*result = NULL;
	*error_text = NULL;
	while (index < token->length) {
		unsigned char quote = token->quote[index];
		char value = token->text[index];
		if (value != '$' || quote == SH_QUOTE_SINGLE ||
		    quote == SH_QUOTE_ESCAPED || index + 1U == token->length) {
			if (!append_bytes(&buffer, &value, 1U))
				goto no_memory;
			index++;
			continue;
		}
		value = token->text[index + 1U];
		if (value == '?' || value == '$' || value == '!') {
			long number = value == '?' ? context->status :
			    value == '$' ? context->shell_pid : context->last_job;
			if (!append_number(&buffer, number))
				goto no_memory;
			index += 2U;
			continue;
		}
		if (value == '{') {
			size_t start = index + 2U;
			size_t end = start;
			while (end < token->length && token->text[end] != '}')
				end++;
			if (end == token->length) {
				*error_text = "unterminated parameter expansion";
				free(buffer.data);
				return 0;
			}
			if (end == start || !name_start(token->text[start])) {
				*error_text = "invalid parameter name";
				free(buffer.data);
				return 0;
			}
			{
				size_t scan;
				for (scan = start + 1U; scan < end; scan++) {
					if (!name_character(token->text[scan])) {
						*error_text = "unsupported parameter expansion";
						free(buffer.data);
						return 0;
					}
				}
			}
			if (!append_parameter(&buffer, token->text + start,
			    end - start))
				goto no_memory;
			index = end + 1U;
			continue;
		}
		if (name_start(value)) {
			size_t start = index + 1U;
			size_t end = start + 1U;
			while (end < token->length &&
			    name_character(token->text[end]))
				end++;
			if (!append_parameter(&buffer, token->text + start,
			    end - start))
				goto no_memory;
			index = end;
			continue;
		}
		if (!append_bytes(&buffer, "$", 1U))
			goto no_memory;
		index++;
	}
	if (buffer.data == NULL) {
		buffer.data = malloc(1U);
		if (buffer.data == NULL)
			goto no_memory;
		buffer.data[0] = '\0';
	}
	*result = buffer.data;
	return 1;
no_memory:
	free(buffer.data);
	*error_text = "out of memory";
	return 0;
}
