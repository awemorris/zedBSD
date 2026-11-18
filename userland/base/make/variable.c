/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The variables (macros) of make.
 *
 * A variable lives in a table: the global one, or a target's own.  A
 * definition only replaces one of the same or a lower origin, so that the
 * command line beats the makefiles and the makefiles beat the
 * environment (unless -e).  The environment of a recipe is built from
 * the variables that are exported: those from the environment and the
 * command line, and those an export directive named.
 */

#include "make.h"

#include <stdlib.h>
#include <string.h>

/* The global variables, which live until make ends. */
struct variable_set make_global_variables;

/* The scope of the global variables alone. */
const struct variable_scope make_global_scope = {&make_global_variables, NULL};

/*
 * Set by .EXPORT_ALL_VARIABLES: every variable with a valid name goes to
 * the environment of the recipes.
 */
static int export_all;

static unsigned hash_name(const char *name, size_t length);
static struct variable *find_in_set(struct variable_set *set, const char *name, size_t length);
static int origin_rank(enum variable_origin origin);
static int is_exported(const struct variable *variable);
static int is_valid_name(const char *name);
static void add_environment_entry(struct buffer *entries, const struct variable *variable, const struct expansion *context);
static char *shell_value(const struct expansion *context, const char *command);

/*
 * Finds a variable by name in a scope, innermost table first; returns
 * NULL when no table has it.  A variable of the command line (or an
 * override) wins over a target's own of a lower origin, as GNU make
 * does.
 */
struct variable *
variable_lookup(
	const struct variable_scope *scope,
	const char *name,
	size_t length)
{
	struct variable *found;
	struct variable *global;

	/* Each table outward. */
	for (;
	     scope != NULL;
	     scope = scope->parent) {
		found = find_in_set(scope->set, name, length);
		if (found == NULL)
			continue;

		/* A target's variable yields to the command line's. */
		if (scope->set != &make_global_variables && found->origin < ORIGIN_COMMAND_LINE) {
			global = find_in_set(&make_global_variables, name, length);
			if (global != NULL && global->origin >= ORIGIN_COMMAND_LINE)
				return global;
		}

		/* Succeeded: the target's variable. */
		return found;
	}

	/* No table has it. */
	return NULL;
}

/*
 * Finds a variable by name in the tables outside the one that holds a
 * given variable (for a target's += that adds to what it inherits);
 * returns NULL when none has it.
 */
struct variable *
variable_lookup_outer(
	const struct variable_scope *scope,
	const struct variable *inner)
{
	struct variable *found;
	size_t length;

	/* The table that holds the inner variable. */
	length = strlen(inner->name);
	for (;
	     scope != NULL;
	     scope = scope->parent) {
		found = find_in_set(scope->set, inner->name, length);
		if (found == inner)
			break;
	}

	/* The variable is in none of the tables. */
	if (scope == NULL)
		return NULL;

	/* Succeeded or not: the tables after it. */
	found = variable_lookup(scope->parent, inner->name, length);
	return found;
}

/*
 * Defines a variable in a table, unless it is there already from an
 * origin that takes precedence; returns the variable as it stands.
 */
struct variable *
variable_set_value(
	struct variable_set *set,
	const char *name,
	const char *value,
	enum variable_flavor flavor,
	enum variable_origin origin)
{
	struct variable *variable;
	size_t length;
	unsigned chain;
	int old_rank;
	int new_rank;

	/* An existing variable is replaced only from the same or a higher origin. */
	length = strlen(name);
	variable = find_in_set(set, name, length);
	if (variable != NULL) {
		old_rank = origin_rank(variable->origin);
		new_rank = origin_rank(origin);
		if (new_rank < old_rank)
			return variable;
		free(variable->value);
		variable->value = make_strdup(value);
		variable->flavor = flavor;
		variable->origin = origin;
		return variable;
	}

	/* A new variable at the head of its chain. */
	chain = hash_name(name, length);
	variable = make_malloc(sizeof(*variable));
	memset(variable, 0, sizeof(*variable));
	variable->name = make_strdup(name);
	variable->value = make_strdup(value);
	variable->flavor = flavor;
	variable->origin = origin;
	variable->next = set->chains[chain];
	set->chains[chain] = variable;

	/* Succeeded: the new variable. */
	return variable;
}

/*
 * Carries out an assignment with one of the operators =, :=, +=, ?= and
 * != (the value is the text after the operator, unexpanded).
 */
void
variable_assign(
	struct variable_set *set,
	const char *name,
	const char *value,
	enum assign_kind kind,
	enum variable_origin origin,
	const struct expansion *context)
{
	struct variable *existing;
	struct buffer joined;
	char *expanded;
	char *addition;
	size_t length;

	/* Chooses what the operator does with the value. */
	length = strlen(name);
	existing = find_in_set(set, name, length);
	switch (kind) {
	case ASSIGN_RECURSIVE:
		/* The text as it is, expanded when it is used. */
		variable_set_value(set, name, value, FLAVOR_RECURSIVE, origin);
		break;
	case ASSIGN_SIMPLE:
		/* The text expanded now. */
		expanded = expand(context, value);
		variable_set_value(set, name, expanded, FLAVOR_SIMPLE, origin);
		free(expanded);
		break;
	case ASSIGN_CONDITIONAL:
		/* Only a variable that is not defined at all, empty or not. */
		if (existing == NULL)
			variable_set_value(set, name, value, FLAVOR_RECURSIVE, origin);
		break;
	case ASSIGN_APPEND:
		/* A new variable is an ordinary one. */
		if (existing == NULL) {
			variable_set_value(set, name, value, FLAVOR_RECURSIVE, origin);
			break;
		}

		/* The new text, expanded now for a simple variable. */
		if (existing->flavor == FLAVOR_SIMPLE) {
			addition = expand(context, value);
		} else {
			addition = make_strdup(value);
		}

		/* The old value and the new text, with a space only between two that are not empty. */
		memset(&joined, 0, sizeof(joined));
		buffer_add_string(&joined, existing->value);
		if (existing->value[0] != '\0' && addition[0] != '\0')
			buffer_add_char(&joined, ' ');
		buffer_add_string(&joined, addition);
		free(addition);
		expanded = buffer_finish(&joined);
		variable_set_value(set, name, expanded, existing->flavor, origin);
		free(expanded);
		break;
	case ASSIGN_SHELL:
		/* The output of the command, its newlines made spaces. */
		expanded = shell_value(context, value);
		variable_set_value(set, name, expanded, FLAVOR_RECURSIVE, origin);
		free(expanded);
		break;
	default:
		break;
	}
}

/*
 * Makes a variable of each NAME=value of the environment.  SHELL is left
 * out: a makefile's shell never comes from the environment.
 */
void
variable_import_environment(
	char **environment)
{
	enum variable_origin origin;
	const char *equals;
	char *name;
	size_t index;
	int compare;

	/* -e puts the environment above the makefiles. */
	origin = ORIGIN_ENVIRONMENT;
	if (make_options.environment_overrides)
		origin = ORIGIN_ENVIRONMENT_OVERRIDE;

	/* Each entry with a name before its =. */
	for (index = 0; environment[index] != NULL; index++) {
		equals = strchr(environment[index], '=');
		if (equals == NULL || equals == environment[index])
			continue;
		name = make_strndup(environment[index], (size_t)(equals - environment[index]));
		compare = strcmp(name, "SHELL");
		if (compare != 0)
			variable_set_value(&make_global_variables, name, equals + 1, FLAVOR_RECURSIVE, origin);
		free(name);
	}
}

/*
 * Builds the environment of a recipe: every exported variable visible in
 * the scope, expanded, with MAKELEVEL one deeper.  The caller frees it
 * with variable_free_environment().
 */
char **
variable_environment(
	const struct variable_scope *scope,
	const struct automatic *automatic)
{
	const struct variable_scope *table;
	struct expansion context;
	struct buffer entries;
	struct variable *variable;
	struct variable *visible;
	char **environment;
	char *cursor;
	size_t count;
	size_t index;
	size_t length;
	unsigned chain;
	int exported;

	/* Variables expand in the scope of the recipe. */
	context.scope = scope;
	context.automatic = automatic;
	context.file = NULL;
	context.line = 0;

	/* Each exported variable that no inner table hides, as NAME=value and a null. */
	memset(&entries, 0, sizeof(entries));
	count = 0;
	for (table = scope; table != NULL; table = table->parent) {
		for (chain = 0; chain < MAKE_HASH_SIZE; chain++) {
			for (variable = table->set->chains[chain]; variable != NULL; variable = variable->next) {
				length = strlen(variable->name);
				visible = variable_lookup(scope, variable->name, length);
				if (visible != variable)
					continue;
				exported = is_exported(variable);
				if (!exported)
					continue;
				add_environment_entry(&entries, variable, &context);
				count++;
			}
		}
	}

	/* The array points into one block of entries; the block follows the pointers. */
	environment = make_malloc((count + 1U) * sizeof(*environment) + entries.length + 1U);
	cursor = (char *)(environment + count + 1U);
	if (entries.length > 0)
		memcpy(cursor, entries.text, entries.length);
	for (index = 0; index < count; index++) {
		environment[index] = cursor;
		length = strlen(cursor);
		cursor += length + 1U;
	}

	/* The array ends with a null pointer. */
	environment[count] = NULL;
	free(entries.text);

	/* Succeeded. */
	return environment;
}

/*
 * Frees an environment from variable_environment().
 */
void
variable_free_environment(
	char **environment)
{
	/* One block holds the array and the entries. */
	free(environment);
}

/*
 * Returns a new, empty table of variables, for a target.
 */
struct variable_set *
variable_new_set(
	void)
{
	struct variable_set *set;

	/* Every chain empty. */
	set = make_malloc(sizeof(*set));
	memset(set, 0, sizeof(*set));

	/* Succeeded. */
	return set;
}

/*
 * Frees a table of variables that variable_new_set() made, with every
 * variable in it.
 */
void
variable_free_set(
	struct variable_set *set)
{
	struct variable *variable;
	struct variable *next;
	unsigned chain;

	/* Each chain's variables. */
	for (chain = 0; chain < MAKE_HASH_SIZE; chain++) {
		for (variable = set->chains[chain]; variable != NULL; variable = next) {
			next = variable->next;
			free(variable->name);
			free(variable->value);
			free(variable);
		}
	}

	/* The table itself. */
	free(set);
}

/*
 * Exports every variable with a valid name (.EXPORT_ALL_VARIABLES).
 */
void
variable_export_all(
	void)
{
	/* is_exported() reads it. */
	export_all = 1;
}

/* Returns the hash chain of a name. */
static unsigned
hash_name(
	const char *name,
	size_t length)
{
	unsigned hash;
	size_t index;

	/* A multiplicative hash of the bytes. */
	hash = 5381U;
	for (index = 0; index < length; index++)
		hash = hash * 33U + (unsigned char)name[index];

	/* Succeeded: the chain. */
	return hash % MAKE_HASH_SIZE;
}

/* Finds a variable in one table; returns NULL when it is not there. */
static struct variable *
find_in_set(
	struct variable_set *set,
	const char *name,
	size_t length)
{
	struct variable *variable;
	unsigned chain;
	int compare;

	/* The chain of the name. */
	chain = hash_name(name, length);
	for (variable = set->chains[chain]; variable != NULL; variable = variable->next) {
		compare = strncmp(variable->name, name, length);
		if (compare == 0 && variable->name[length] == '\0')
			return variable;
	}

	/* Not in this table. */
	return NULL;
}

/* Returns the precedence of an origin: a higher one is not replaced by a lower. */
static int
origin_rank(
	enum variable_origin origin)
{
	/* The order of the enumeration, which is the order of precedence. */
	return (int)origin;
}

/* Reports whether a variable goes to the environment of the recipes. */
static int
is_exported(
	const struct variable *variable)
{
	int valid;

	/* An export or unexport directive decides. */
	if (variable->export_state > 0)
		return 1;
	if (variable->export_state < 0)
		return 0;

	/* What came from the environment or the command line goes back out. */
	if (variable->origin == ORIGIN_ENVIRONMENT || variable->origin == ORIGIN_ENVIRONMENT_OVERRIDE)
		return 1;
	if (variable->origin == ORIGIN_COMMAND_LINE)
		return 1;

	/* .EXPORT_ALL_VARIABLES takes any name the shell can hold. */
	valid = is_valid_name(variable->name);
	if (export_all && valid && variable->origin != ORIGIN_DEFAULT)
		return 1;

	/* Anything else stays in make. */
	return 0;
}

/* Reports whether a name is one the shell can hold: letters, digits and _. */
static int
is_valid_name(
	const char *name)
{
	const char *cursor;
	char character;

	/* A letter or _ first, then letters, digits and _. */
	if (name[0] >= '0' && name[0] <= '9')
		return 0;
	for (cursor = name; *cursor != '\0'; cursor++) {
		character = *cursor;
		if (character >= 'a' && character <= 'z')
			continue;
		if (character >= 'A' && character <= 'Z')
			continue;
		if (character >= '0' && character <= '9')
			continue;
		if (character == '_')
			continue;
		return 0;
	}

	/* Succeeded: an empty name is not one. */
	if (name[0] == '\0')
		return 0;
	return 1;
}

/* Appends NAME=value and a null for a variable, its value expanded. */
static void
add_environment_entry(
	struct buffer *entries,
	const struct variable *variable,
	const struct expansion *context)
{
	char level[32];
	char *value;
	char *reference;
	int compare;

	/* The name and the =. */
	buffer_add_string(entries, variable->name);
	buffer_add_char(entries, '=');

	/* A recursive make runs one level deeper. */
	compare = strcmp(variable->name, "MAKELEVEL");
	if (compare == 0) {
		snprintf(level, sizeof(level), "%d", make_level + 1);
		buffer_add_string(entries, level);
		buffer_add(entries, "", 1);
		return;
	}

	/* The value as a reference to it would read (a target's += included), then the null. */
	reference = make_malloc(strlen(variable->name) + 4U);
	strcpy(reference, "$(");
	strcat(reference, variable->name);
	strcat(reference, ")");
	value = expand(context, reference);
	buffer_add_string(entries, value);
	free(value);
	free(reference);

	/* The null after the entry. */
	buffer_add(entries, "", 1);
}

/*
 * Runs a command for !=, and returns its output with trailing newlines
 * dropped and the others made spaces.
 */
static char *
shell_value(
	const struct expansion *context,
	const char *command)
{
	char *expanded;
	char *output;
	size_t length;
	size_t index;
	int status;

	/* The command, expanded, run by the shell. */
	expanded = expand(context, command);
	output = job_shell_output(expanded, &status);
	free(expanded);

	/* Trailing newlines go. */
	length = strlen(output);
	while (length > 0 && output[length - 1U] == '\n') {
		length--;
		output[length] = '\0';
	}

	/* Each other newline becomes a space. */
	for (index = 0; index < length; index++) {
		if (output[index] == '\n')
			output[index] = ' ';
	}

	/* Succeeded: the output. */
	return output;
}
