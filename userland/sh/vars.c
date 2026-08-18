/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200809L
#include "userland/sh/vars.h"

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

static char *
copy_string(const char *value)
{
	size_t length = strlen(value) + 1U;
	char *copy = malloc(length);
	if (copy != NULL)
		memcpy(copy, value, length);
	return copy;
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

int
sh_var_name(const char *name)
{
	if (name == NULL || !name_start(*name++))
		return 0;
	while (*name != '\0') {
		if (!name_character(*name++))
			return 0;
	}
	return 1;
}

static struct shell_variable *
find_variable(const char *name)
{
	struct shell_variable *variable;
	for (variable = variables; variable != NULL; variable = variable->next) {
		if (strcmp(variable->name, name) == 0)
			return variable;
	}
	return NULL;
}

const char *
sh_var_get(const char *name)
{
	struct shell_variable *variable = find_variable(name);
	return variable == NULL ? getenv(name) : variable->value;
}

int
sh_var_set(const char *name, const char *value, int exported)
{
	struct shell_variable *variable;
	char *copy;
	if (!sh_var_name(name)) {
		errno = EINVAL;
		return -1;
	}
	variable = find_variable(name);
	if (variable != NULL && variable->readonly) {
		errno = EPERM;
		return -1;
	}
	copy = copy_string(value);
	if (copy == NULL)
		return -1;
	if (variable == NULL) {
		variable = calloc(1, sizeof(*variable));
		if (variable == NULL) {
			free(copy);
			return -1;
		}
		variable->name = copy_string(name);
		if (variable->name == NULL) {
			free(copy);
			free(variable);
			return -1;
		}
		variable->next = variables;
		variables = variable;
		if (exported < 0 && getenv(name) != NULL)
			variable->exported = 1;
	}
	free(variable->value);
	variable->value = copy;
	if (exported >= 0)
		variable->exported = exported != 0;
	if (variable->exported && setenv(name, value, 1) != 0)
		return -1;
	return 0;
}

int
sh_var_export(const char *name)
{
	struct shell_variable *variable = find_variable(name);
	const char *value;
	if (!sh_var_name(name)) {
		errno = EINVAL;
		return -1;
	}
	if (variable == NULL) {
		value = getenv(name);
		if (sh_var_set(name, value == NULL ? "" : value, 1) != 0)
			return -1;
		return 0;
	}
	variable->exported = 1;
	return setenv(name, variable->value, 1);
}

int
sh_var_readonly(const char *name)
{
	struct shell_variable *variable = find_variable(name);
	const char *value;
	if (!sh_var_name(name)) {
		errno = EINVAL;
		return -1;
	}
	if (variable == NULL) {
		value = getenv(name);
		if (sh_var_set(name, value == NULL ? "" : value, 0) != 0)
			return -1;
		variable = find_variable(name);
	}
	variable->readonly = 1;
	return 0;
}

int
sh_var_unset(const char *name)
{
	struct shell_variable **link = &variables;
	struct shell_variable *variable;
	if (!sh_var_name(name)) {
		errno = EINVAL;
		return -1;
	}
	while ((variable = *link) != NULL) {
		if (strcmp(variable->name, name) != 0) {
			link = &variable->next;
			continue;
		}
		if (variable->readonly) {
			errno = EPERM;
			return -1;
		}
		*link = variable->next;
		free(variable->name);
		free(variable->value);
		free(variable);
		break;
	}
	return unsetenv(name);
}

int
sh_var_snapshot(const char *name, struct sh_var_snapshot *snapshot)
{
	struct shell_variable *variable;
	const char *environment;
	memset(snapshot, 0, sizeof(*snapshot));
	if (!sh_var_name(name)) {
		errno = EINVAL;
		return -1;
	}
	snapshot->name = copy_string(name);
	if (snapshot->name == NULL)
		return -1;
	variable = find_variable(name);
	if (variable != NULL) {
		snapshot->value = copy_string(variable->value);
		if (snapshot->value == NULL)
			goto failed;
		snapshot->existed = 1;
		snapshot->exported = variable->exported;
		snapshot->readonly = variable->readonly;
	}
	environment = getenv(name);
	if (environment != NULL) {
		snapshot->environment = copy_string(environment);
		if (snapshot->environment == NULL)
			goto failed;
		snapshot->environment_existed = 1;
	}
	return 0;
failed:
	free(snapshot->name);
	free(snapshot->value);
	free(snapshot->environment);
	memset(snapshot, 0, sizeof(*snapshot));
	return -1;
}

int
sh_var_restore(struct sh_var_snapshot *snapshot)
{
	struct shell_variable **link = &variables;
	struct shell_variable *variable;
	int result = 0;
	while ((variable = *link) != NULL) {
		if (strcmp(variable->name, snapshot->name) == 0) {
			*link = variable->next;
			free(variable->name);
			free(variable->value);
			free(variable);
			break;
		}
		link = &variable->next;
	}
	if (snapshot->existed) {
		variable = calloc(1, sizeof(*variable));
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
	if (snapshot->environment_existed) {
		if (setenv(snapshot->name == NULL ? variable->name : snapshot->name,
		    snapshot->environment, 1) != 0)
			result = -1;
	} else {
		const char *name = snapshot->name == NULL && variable != NULL ?
		    variable->name : snapshot->name;
		if (name != NULL && unsetenv(name) != 0)
			result = -1;
	}
	free(snapshot->name);
	free(snapshot->value);
	free(snapshot->environment);
	memset(snapshot, 0, sizeof(*snapshot));
	return result;
}
