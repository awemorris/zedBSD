/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland vars component.
 */

#define _POSIX_C_SOURCE 200809L
#include "userland/base/sh/vars.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct shell_variable {
	struct shell_variable *next;
	char *name;
	char *value;
	int exported;
	int readonly;
};

static struct shell_variable *variables;

static int name_start(char value);
static int name_character(char value);
static struct shell_variable *find_variable(const char *name);
static char *copy_string(const char *value);

/*
 * Implements the sh var name operation.
 */
int
sh_var_name(
	const char *name)
{
	/* Handles a failed name start operation. */
	if (name == NULL || !name_start(*name++))
		return 0;

	/* Continue while the operation condition remains true. */
	while (*name != '\0') {
		/* Handles a failed name character operation. */
		if (!name_character(*name++))
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the sh var get operation.
 */
const char *
sh_var_get(
	const char *name)
{
	const char *function_result;
	struct shell_variable *variable;

	variable = find_variable(name);

	/* Computes the function result. */
	function_result = variable == NULL ? getenv(name) : variable->value;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sh var set operation.
 */
int
sh_var_set(
	const char *name,
	const char *value,
	int exported)
{
	struct shell_variable *variable;
	char *copy;

	/* Handles a failed sh var name operation. */
	if (!sh_var_name(name)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	variable = find_variable(name);

	/* Handles the variable availability. */
	if (variable != NULL && variable->readonly) {
		errno = EPERM;

		/* Reports operation failure. */
		return -1;
	}
	copy = copy_string(value);

	/* Handles the copy availability. */
	if (copy == NULL)
		return -1;

	/* Handles the variable availability. */
	if (variable == NULL) {
		variable = calloc(1, sizeof(*variable));

		/* Handles the variable availability. */
		if (variable == NULL) {
			free(copy);

			/* Reports operation failure. */
			return -1;
		}
		variable->name = copy_string(name);

		/* Handles the name availability. */
		if (variable->name == NULL) {
			free(copy);
			free(variable);

			/* Reports operation failure. */
			return -1;
		}
		variable->next = variables;
		variables = variable;

		/* Handles a failed getenv operation. */
		if (exported < 0 && getenv(name) != NULL)
			variable->exported = 1;
	}
	free(variable->value);
	variable->value = copy;

	/* Handles the exported condition. */
	if (exported >= 0)
		variable->exported = exported != 0;

	/* Handles a failed setenv operation. */
	if (variable->exported && setenv(name, value, 1) != 0)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sh var export operation.
 */
int
sh_var_export(
	const char *name)
{
	int function_result;
	struct shell_variable *variable;
	const char *value;

	variable = find_variable(name);

	/* Handles a failed sh var name operation. */
	if (!sh_var_name(name)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the variable availability. */
	if (variable == NULL) {
		value = getenv(name);

		/* Handles a failed sh var set operation. */
		if (sh_var_set(name, value == NULL ? "" : value, 1) != 0)
			return -1;

		/* Reports successful completion. */
		return 0;
	}
	variable->exported = 1;

	/* Obtains the setenv result. */
	function_result = setenv(name, variable->value, 1);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sh var readonly operation.
 */
int
sh_var_readonly(
	const char *name)
{
	struct shell_variable *variable;
	const char *value;

	variable = find_variable(name);

	/* Handles a failed sh var name operation. */
	if (!sh_var_name(name)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the variable availability. */
	if (variable == NULL) {
		value = getenv(name);

		/* Handles a failed sh var set operation. */
		if (sh_var_set(name, value == NULL ? "" : value, 0) != 0)
			return -1;
		variable = find_variable(name);
	}
	variable->readonly = 1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sh var unset operation.
 */
int
sh_var_unset(
	const char *name)
{
	int function_result;
	struct shell_variable **link;
	struct shell_variable *variable;

	link = &variables;

	/* Handles a failed sh var name operation. */
	if (!sh_var_name(name)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	while ((variable = *link) != NULL) {
		/* Selects the matching value. */
		if (strcmp(variable->name, name) != 0) {
			link = &variable->next;
			continue;
		}

		/* Handles the variable condition. */
		if (variable->readonly) {
			errno = EPERM;

			/* Reports operation failure. */
			return -1;
		}
		*link = variable->next;
		free(variable->name);
		free(variable->value);
		free(variable);
		break;
	}

	/* Obtains the unsetenv result. */
	function_result = unsetenv(name);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sh var snapshot operation.
 */
int
sh_var_snapshot(
	const char *name,
	struct sh_var_snapshot *snapshot)
{
	struct shell_variable *variable;
	const char *environment;

	memset(snapshot, 0, sizeof(*snapshot));

	/* Handles a failed sh var name operation. */
	if (!sh_var_name(name)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	snapshot->name = copy_string(name);

	/* Handles the name availability. */
	if (snapshot->name == NULL)
		return -1;
	variable = find_variable(name);

	/* Handles the variable availability. */
	if (variable != NULL) {
		snapshot->value = copy_string(variable->value);

		/* Handles the value availability. */
		if (snapshot->value == NULL)
			goto failed;
		snapshot->existed = 1;
		snapshot->exported = variable->exported;
		snapshot->readonly = variable->readonly;
	}
	environment = getenv(name);

	/* Handles the environment availability. */
	if (environment != NULL) {
		snapshot->environment = copy_string(environment);

		/* Handles the environment availability. */
		if (snapshot->environment == NULL)
			goto failed;
		snapshot->environment_existed = 1;
	}

	/* Reports successful completion. */
	return 0;
failed:
	free(snapshot->name);
	free(snapshot->value);
	free(snapshot->environment);
	memset(snapshot, 0, sizeof(*snapshot));

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the sh var restore operation.
 */
int
sh_var_restore(
	struct sh_var_snapshot *snapshot)
{
	const char *name;
	struct shell_variable **link;
	struct shell_variable *variable;
	int result;

	/* Continue while the operation condition remains true. */
	link = &variables;
	result = 0;
	while ((variable = *link) != NULL) {
		/* Selects the matching value. */
		if (strcmp(variable->name, snapshot->name) == 0) {
			*link = variable->next;
			free(variable->name);
			free(variable->value);
			free(variable);
			break;
		}
		link = &variable->next;
	}

	/* Handles the snapshot condition. */
	if (snapshot->existed) {
		variable = calloc(1, sizeof(*variable));

		/* Handles the variable availability. */
		if (variable == NULL) {
			result = -1;
			goto environment;
		}
		variable->name = snapshot->name;
		variable->value = snapshot->value;
		variable->exported = snapshot->exported;
		variable->readonly = snapshot->readonly;
		variable->next = variables;
		variables = variable;
		snapshot->name = NULL;
		snapshot->value = NULL;
	}
environment:

	/* Handles the snapshot condition. */
	if (snapshot->environment_existed) {
		/* Handles a failed setenv operation. */
		if (setenv(snapshot->name == NULL ? variable->name
						  : snapshot->name,
			   snapshot->environment, 1) != 0)
			result = -1;
	} else {
		name = snapshot->name == NULL && variable != NULL
		       ? variable->name
		       : snapshot->name;

		/* Handles a failed unsetenv operation. */
		if (name != NULL && unsetenv(name) != 0)
			result = -1;
	}
	free(snapshot->name);
	free(snapshot->value);
	free(snapshot->environment);
	memset(snapshot, 0, sizeof(*snapshot));

	/* Returns the computed result. */
	return result;
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

/* Supports the find variable operation. */
static struct shell_variable *
find_variable(
	const char *name)
{
	struct shell_variable *variable;

	/* Process each linked entry. */
	for (variable = variables; variable != NULL;
	     variable = variable->next) {
		/* Selects the matching value. */
		if (strcmp(variable->name, name) == 0)
			return variable;
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the copy string operation. */
static char *
copy_string(
	const char *value)
{
	size_t length;
	char *copy;

	length = strlen(value) + 1U;
	copy = malloc(length);

	/* Handles the copy availability. */
	if (copy != NULL)
		memcpy(copy, value, length);

	/* Returns the computed result. */
	return copy;
}
