/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland buffer component.
 */

#include "userland/base/ed/editor.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int buffer_reserve(struct ed_buffer *buffer, size_t needed);

/*
 * Implements the ed buffer init operation.
 */
void
ed_buffer_init(
	struct ed_buffer *buffer)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->current = SIZE_MAX;
}

/*
 * Implements the ed buffer free operation.
 */
void
ed_buffer_free(
	struct ed_buffer *buffer)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < buffer->count; index++)
		free(buffer->line[index]);
	free(buffer->line);
	ed_buffer_init(buffer);
}

/*
 * Implements the ed buffer insert operation.
 */
int
ed_buffer_insert(
	struct ed_buffer *buffer,
	size_t position,
	const char *text)
{
	char *copy;

	/* Handles the position condition. */
	if (position > buffer->count) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	copy = strdup(text);

	/* Handles a failed buffer reserve operation. */
	if (copy == NULL || buffer_reserve(buffer, buffer->count + 1) != 0) {
		free(copy);

		/* Reports operation failure. */
		return -1;
	}
	memmove(buffer->line + position + 1, buffer->line + position,
		(buffer->count - position) * sizeof(*buffer->line));
	buffer->line[position] = copy;
	buffer->count++;
	buffer->current = position;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the ed buffer delete operation.
 */
int
ed_buffer_delete(
	struct ed_buffer *buffer,
	size_t first,
	size_t last)
{
	size_t index;
	size_t removed;

	/* Handles the first condition. */
	if (first > last || last >= buffer->count) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (index = first; index <= last; index++)
		free(buffer->line[index]);
	removed = last - first + 1;
	memmove(buffer->line + first, buffer->line + last + 1,
		(buffer->count - last - 1) * sizeof(*buffer->line));
	buffer->count -= removed;

	/* Handles the buffer condition. */
	if (buffer->count == 0)
		buffer->current = SIZE_MAX;
	else if (first < buffer->count)
		buffer->current = first;
	else
		buffer->current = buffer->count - 1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the ed buffer replace operation.
 */
int
ed_buffer_replace(
	struct ed_buffer *buffer,
	size_t index,
	const char *text)
{
	char *copy;

	/* Checks the current index. */
	if (index >= buffer->count) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	copy = strdup(text);

	/* Handles the copy availability. */
	if (copy == NULL)
		return -1;
	free(buffer->line[index]);
	buffer->line[index] = copy;
	buffer->current = index;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the ed buffer copy operation.
 */
int
ed_buffer_copy(
	struct ed_buffer *destination,
	const struct ed_buffer *source)
{
	struct ed_buffer result;
	size_t index;

	ed_buffer_init(&result);

	/* Handles a failed buffer reserve operation. */
	if (buffer_reserve(&result, source->count) != 0)
		return -1;

	/* Process each remaining element. */
	for (index = 0; index < source->count; index++) {
		result.line[index] = strdup(source->line[index]);

		/* Checks the operation result. */
		if (result.line[index] == NULL) {
			result.count = index;
			ed_buffer_free(&result);

			/* Reports operation failure. */
			return -1;
		}
	}
	result.count = source->count;
	result.current = source->current;
	ed_buffer_move(destination, &result);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the ed buffer move operation.
 */
void
ed_buffer_move(
	struct ed_buffer *destination,
	struct ed_buffer *source)
{
	ed_buffer_free(destination);
	*destination = *source;
	ed_buffer_init(source);
}

/* Supports the buffer reserve operation. */
static int
buffer_reserve(
	struct ed_buffer *buffer,
	size_t needed)
{
	size_t capacity;
	char **line;

	/* Handles the needed condition. */
	if (needed <= buffer->capacity)
		return 0;

	/* Handles the needed condition. */
	if (needed > SIZE_MAX / sizeof(*line)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}

	/* Continue while the operation condition remains true. */
	capacity = buffer->capacity == 0 ? 16 : buffer->capacity;
	while (capacity < needed) {
		/* Handles the capacity condition. */
		if (capacity > SIZE_MAX / 2) {
			capacity = needed;
			break;
		}
		capacity *= 2;
	}
	line = realloc(buffer->line, capacity * sizeof(*line));

	/* Handles the line availability. */
	if (line == NULL)
		return -1;
	buffer->line = line;
	buffer->capacity = capacity;

	/* Reports successful completion. */
	return 0;
}
