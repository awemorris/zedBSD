/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD persistent environment store
 */

#include "userland/packages/lang/noct/runtime/zedbsd-api.h"

#include <string.h>

static int valid_name(const char *name, size_t *length);
static int find_entry(const struct environment *environment, const char *name, size_t *offset, size_t *span);

/*
 * Implements the env name valid operation.
 */
int
env_name_valid(
	const char *name)
{
	int function_result;
	size_t length;

	/* Obtains the valid name result. */
	function_result = valid_name(name, &length);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the env init operation.
 */
void
env_init(
	struct environment *environment)
{
	/* Handles the environment availability. */
	if (environment != NULL)
		memset(environment, 0, sizeof(*environment));
}

/*
 * Implements the env get operation.
 */
const char *
env_get(
	const struct environment *environment,
	const char *name)
{
	const char *function_result;
	size_t offset;

	/* Handles a failed find entry operation. */
	if (environment == NULL || name == NULL ||
	    !find_entry(environment, name, &offset, NULL))

		/* Reports that no result is available. */
		return NULL;

	/* Computes the function result. */
	function_result = environment->storage + offset + strlen(name) + 1U;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the env set operation.
 */
int
env_set(
	struct environment *environment,
	const char *name,
	const char *value)
{
	size_t name_length;
	size_t value_length;
	size_t new_span;
	size_t offset;
	size_t old_span;
	size_t tail;
	int found;

	old_span = 0;

	/* Handles a failed valid name operation. */
	if (environment == NULL || value == NULL ||
	    !valid_name(name, &name_length))

		/* Reports successful completion. */
		return 0;
	value_length = strnlen(value, ZEDBSD_ENV_VALUE_MAX + 1U);

	/* Handles the value length condition. */
	if (value_length > ZEDBSD_ENV_VALUE_MAX)
		return 0;
	new_span = name_length + 1U + value_length + 1U;
	found = find_entry(environment, name, &offset, &old_span);

	/* Handles the found condition. */
	if (!found) {
		/* Handles the environment condition. */
		if (environment->count >= ZEDBSD_ENV_MAX_ENTRIES)
			return 0;
		offset = environment->used;
	}

	/* Handles the environment condition. */
	if ((size_t)environment->used - old_span + new_span >
	    ZEDBSD_ENV_STORAGE_SIZE)

		/* Reports successful completion. */
		return 0;

	tail = (size_t)environment->used - offset - old_span;

	/* Handles the old span condition. */
	if (old_span != new_span)
		memmove(environment->storage + offset + new_span,
			environment->storage + offset + old_span, tail);
	memcpy(environment->storage + offset, name, name_length + 1U);
	memcpy(environment->storage + offset + name_length + 1U, value,
	       value_length + 1U);
	environment->used =
	    (uint16_t)((size_t)environment->used - old_span + new_span);

	/* Handles the found condition. */
	if (!found)
		environment->count++;

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the env unset operation.
 */
int
env_unset(
	struct environment *environment,
	const char *name)
{
	size_t offset;
	size_t span;
	size_t tail;

	/* Handles a failed find entry operation. */
	if (environment == NULL || name == NULL ||
	    !find_entry(environment, name, &offset, &span))

		/* Reports successful completion. */
		return 0;
	tail = (size_t)environment->used - offset - span;
	memmove(environment->storage + offset,
		environment->storage + offset + span, tail);
	environment->used = (uint16_t)((size_t)environment->used - span);
	environment->count--;

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the env count operation.
 */
size_t
env_count(
	const struct environment *environment)
{
	/* Returns the computed result. */
	return environment != NULL ? environment->count : 0;
}

/*
 * Implements the env at operation.
 */
int
env_at(
	const struct environment *environment,
	size_t index,
	const char **name,
	const char **value)
{
	const char *entry_name;
	const char *entry_value;
	size_t position;
	size_t current;

	position = 0;
	current = 0;

	/* Handles the environment availability. */
	if (environment == NULL || name == NULL || value == NULL ||
	    index >= environment->count)

		/* Reports successful completion. */
		return 0;

	/* Continue while the operation condition remains true. */
	while (position < environment->used) {

		entry_name = environment->storage + position;
		entry_value = entry_name + strlen(entry_name) + 1U;

		/* Handles the current condition. */
		if (current++ == index) {
			*name = entry_name;
			*value = entry_value;
			/* Reports operation failure. */
			return 1;
		}
		position = (size_t)(entry_value - environment->storage) +
			   strlen(entry_value) + 1U;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the valid name operation. */
static int
valid_name(
	const char *name,
	size_t *length)
{
	char ch;
	size_t index;

	/* Handles the name availability. */
	if (name == NULL || name[0] == '\0' ||
	    !((name[0] >= 'A' && name[0] <= 'Z') ||
	      (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))

		/* Reports successful completion. */
		return 0;

	/* Process each remaining element. */
	for (index = 1; name[index] != '\0'; index++) {
				ch = name[index];

		/* Checks the current index. */
		if (index >= ZEDBSD_ENV_NAME_MAX ||
		    !((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
		      (ch >= '0' && ch <= '9') || ch == '_'))

			/* Reports successful completion. */
			return 0;
	}
	*length = index;
	/* Reports operation failure. */
	return 1;
}

/* Supports the find entry operation. */
static int
find_entry(
	const struct environment *environment,
	const char *name,
	size_t *offset,
	size_t *span)
{
	const char *entry_name;
	size_t name_length;
	const char *entry_value;
	size_t entry_span;
	size_t position;

	position = 0;

	/* Continue while the operation condition remains true. */
	while (position < environment->used) {

		entry_name = environment->storage + position;
		name_length = strlen(entry_name);
		entry_value = entry_name + name_length + 1U;
		entry_span = name_length + 1U + strlen(entry_value) + 1U;

		/* Selects the matching value. */
		if (strcmp(entry_name, name) == 0) {
			/* Handles the offset availability. */
			if (offset != NULL)
				*offset = position;
			/* Handles the span availability. */
			if (span != NULL)
				*span = entry_span;
			/* Reports operation failure. */
			return 1;
		}
		position += entry_span;
	}

	/* Reports successful completion. */
	return 0;
}
