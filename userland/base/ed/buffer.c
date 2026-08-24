/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/ed/editor.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void
ed_buffer_init(struct ed_buffer *buffer)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->current = SIZE_MAX;
}

void
ed_buffer_free(struct ed_buffer *buffer)
{
	size_t index;

	for (index = 0; index < buffer->count; index++)
		free(buffer->line[index]);
	free(buffer->line);
	ed_buffer_init(buffer);
}

static int
buffer_reserve(struct ed_buffer *buffer, size_t needed)
{
	size_t capacity;
	char **line;

	if (needed <= buffer->capacity)
		return 0;
	if (needed > SIZE_MAX / sizeof(*line)) {
		errno = EOVERFLOW;
		return -1;
	}
	capacity = buffer->capacity == 0 ? 16 : buffer->capacity;
	while (capacity < needed) {
		if (capacity > SIZE_MAX / 2) {
			capacity = needed;
			break;
		}
		capacity *= 2;
	}
	line = realloc(buffer->line, capacity * sizeof(*line));
	if (line == NULL)
		return -1;
	buffer->line = line;
	buffer->capacity = capacity;
	return 0;
}

int
ed_buffer_insert(struct ed_buffer *buffer, size_t position, const char *text)
{
	char *copy;

	if (position > buffer->count) {
		errno = EINVAL;
		return -1;
	}
	copy = strdup(text);
	if (copy == NULL || buffer_reserve(buffer, buffer->count + 1) != 0) {
		free(copy);
		return -1;
	}
	memmove(buffer->line + position + 1, buffer->line + position,
		(buffer->count - position) * sizeof(*buffer->line));
	buffer->line[position] = copy;
	buffer->count++;
	buffer->current = position;
	return 0;
}

int
ed_buffer_delete(struct ed_buffer *buffer, size_t first, size_t last)
{
	size_t index;
	size_t removed;

	if (first > last || last >= buffer->count) {
		errno = EINVAL;
		return -1;
	}
	for (index = first; index <= last; index++)
		free(buffer->line[index]);
	removed = last - first + 1;
	memmove(buffer->line + first, buffer->line + last + 1,
		(buffer->count - last - 1) * sizeof(*buffer->line));
	buffer->count -= removed;
	if (buffer->count == 0)
		buffer->current = SIZE_MAX;
	else if (first < buffer->count)
		buffer->current = first;
	else
		buffer->current = buffer->count - 1;
	return 0;
}

int
ed_buffer_replace(struct ed_buffer *buffer, size_t index, const char *text)
{
	char *copy;

	if (index >= buffer->count) {
		errno = EINVAL;
		return -1;
	}
	copy = strdup(text);
	if (copy == NULL)
		return -1;
	free(buffer->line[index]);
	buffer->line[index] = copy;
	buffer->current = index;
	return 0;
}

int
ed_buffer_copy(struct ed_buffer *destination, const struct ed_buffer *source)
{
	struct ed_buffer result;
	size_t index;

	ed_buffer_init(&result);
	if (buffer_reserve(&result, source->count) != 0)
		return -1;
	for (index = 0; index < source->count; index++) {
		result.line[index] = strdup(source->line[index]);
		if (result.line[index] == NULL) {
			result.count = index;
			ed_buffer_free(&result);
			return -1;
		}
	}
	result.count = source->count;
	result.current = source->current;
	ed_buffer_move(destination, &result);
	return 0;
}

void
ed_buffer_move(struct ed_buffer *destination, struct ed_buffer *source)
{
	ed_buffer_free(destination);
	*destination = *source;
	ed_buffer_init(source);
}
