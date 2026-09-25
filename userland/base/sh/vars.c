/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shell variables.
 *
 * Every variable lives in one hash table.  A variable that is exported but
 * has no value (export name) is kept as an entry with a NULL value.  The
 * environment of a command is built from the table when the command is run,
 * so the shell's own environ is only what the shell was started with.
 *
 * A function's local variables are recorded in a frame: making a name local
 * saves what the name held, and leaving the function puts every saved name
 * back, newest first.
 */

#include "userland/base/sh/vars.h"
#include "userland/base/sh/shell.h"
#include "userland/base/sh/expand.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The number of hash chains; a power of two. */
#define VAR_BUCKETS 256U

/* How many variables the shell watches (PATH, OPTIND). */
#define VAR_HOOK_MAX 8

/* The value LINENO reports is the line of the command being run. */
#define VAR_DYNAMIC_LINENO 0x100

/*
 * One variable: its name, its value (NULL when unset but exported), and its
 * attributes.  The strings belong to the entry.
 */
struct variable {
	struct variable *next;
	char *name;
	char *value;
	int flags;
};

/*
 * What a name held before it was made local, to be put back when the
 * function returns.  The entry named "-" saves the options instead.
 */
struct saved_variable {
	struct saved_variable *next;
	char *name;
	int existed;
	char *value;
	int flags;
	int options[SH_OPT_COUNT];
};

/*
 * The local variables of one function call (or of the temporary assignments
 * of a builtin), pushed and popped with the call.
 */
struct local_frame {
	struct local_frame *previous;
	struct saved_variable *saved;
};

/* A variable the shell reacts to when it changes. */
struct variable_hook {
	const char *name;
	void (*changed)(const char *);
};

/*
 * The hash table of every variable.  Entries are only removed by unset; a
 * variable made local keeps its entry and has its old state saved aside.
 */
static struct variable *variable_table[VAR_BUCKETS];

/* The frames of the function calls in progress, innermost first. */
static struct local_frame *local_frames;

/* How many frames there are: the depth handlers unwind to. */
static int local_depth;

/* The variables whose changes the shell hears about; set at startup. */
static struct variable_hook variable_hooks[VAR_HOOK_MAX];
static int variable_hook_count;

/* The text LINENO expands to, rebuilt on each lookup. */
static char lineno_text[24];

static int is_name_start(char value);
static int is_name_character(char value);
static unsigned int name_hash(const char *name);
static struct variable *find_variable(const char *name);
static struct variable *create_variable(const char *name);
static void variable_changed(const char *name);
static int compare_variables(const void *left, const void *right);
static struct variable **sorted_variables(size_t *count);
static void restore_saved(struct saved_variable *saved);
static int is_exported_value(const struct variable *variable);
static char *converted_value(const char *value, int flags);
static void print_declare(const struct variable *variable);
static void print_declare_value(const char *value);

/* The attributes declare can give, which sh_var_flags reports. */
#define VAR_ATTRIBUTES \
	(SH_VAR_EXPORT | SH_VAR_READONLY | SH_VAR_INTEGER | SH_VAR_LOWER | \
	 SH_VAR_UPPER)

/*
 * Reports whether a string is a name (XBD 3.216): a letter or underscore,
 * then letters, digits and underscores.
 */
int
sh_var_name(
	const char *name)
{
	size_t length;

	/* A name is all name, and not empty. */
	length = sh_var_name_length(name);
	if (length == 0 || name[length] != '\0')
		return 0;

	/* Succeeded: the whole string is a name. */
	return 1;
}

/*
 * Reports how long the name at the start of a string is (0 for none).
 */
size_t
sh_var_name_length(
	const char *text)
{
	size_t length;
	int valid;

	/* A name begins with a letter or an underscore. */
	valid = is_name_start(text[0]);
	if (!valid)
		return 0;

	/* It goes on through letters, digits and underscores. */
	length = 1;
	for (;;) {
		valid = is_name_character(text[length]);
		if (!valid)
			break;
		length++;
	}

	/* Succeeded: the length of the name. */
	return length;
}

/*
 * Returns a variable's value, or NULL when it is unset.
 */
const char *
sh_var_get(
	const char *name)
{
	struct variable *variable;

	/* An unknown name is unset. */
	variable = find_variable(name);
	if (variable == NULL)
		return NULL;

	/* LINENO says the line of the command being run. */
	if ((variable->flags & VAR_DYNAMIC_LINENO) != 0) {
		snprintf(lineno_text, sizeof(lineno_text), "%d",
			 sh_command_line);
		return lineno_text;
	}

	/* Succeeded: the value, or NULL for a name exported unset. */
	return variable->value;
}

/*
 * Returns a variable's attributes, or -1 when there is no such variable.
 */
int
sh_var_flags(
	const char *name)
{
	struct variable *variable;

	/* An unknown name has no attributes to report. */
	variable = find_variable(name);
	if (variable == NULL)
		return -1;

	/* Succeeded: the attributes. */
	return variable->flags & VAR_ATTRIBUTES;
}

/*
 * Sets a variable, adding attributes, unless it is read-only.
 */
int
sh_var_set(
	const char *name,
	const char *value,
	int flags)
{
	struct variable *variable;
	char *copy;

	/* A read-only variable keeps its value. */
	variable = find_variable(name);
	if (variable != NULL && (variable->flags & SH_VAR_READONLY) != 0)
		return -1;

	/*
	 * The value, converted as the attributes say (evaluating it may set
	 * other variables, but frees no entry).
	 */
	copy = NULL;
	if (value != NULL)
		copy = converted_value(value, flags |
				       (variable != NULL ? variable->flags : 0));

	/* Makes the entry when there is none. */
	if (variable == NULL)
		variable = create_variable(name);

	/* Replaces the value. */
	free(variable->value);
	variable->value = copy;

	/*
	 * The allexport option exports every variable that is assigned; a
	 * value set here is no longer the line number LINENO follows.
	 */
	if (sh_option[SH_OPT_ALLEXPORT])
		flags |= SH_VAR_EXPORT;
	variable->flags |= flags;
	variable->flags &= ~VAR_DYNAMIC_LINENO;
	variable_changed(name);

	/* Succeeded: the variable holds the value. */
	return 0;
}

/*
 * Sets a variable from "name=value".
 */
int
sh_var_set_assignment(
	const char *assignment,
	int flags)
{
	const char *equals;
	char *name;
	int set;

	/* Splits the name from the value; without = there is no assignment. */
	equals = strchr(assignment, '=');
	if (equals == NULL)
		return -1;
	name = sh_strndup(assignment, (size_t)(equals - assignment));

	/* Sets the variable under that name. */
	set = sh_var_set(name, equals + 1, flags);
	free(name);

	/* Reports a read-only variable. */
	if (set != 0)
		return -1;

	/* Succeeded: the variable holds the value. */
	return 0;
}

/*
 * Unsets a variable, unless it is read-only.
 */
int
sh_var_unset(
	const char *name)
{
	struct variable **link;
	struct variable *variable;
	int compare;

	/* Finds the link that points at the entry. */
	link = &variable_table[name_hash(name)];
	while (*link != NULL) {
		compare = strcmp((*link)->name, name);
		if (compare == 0)
			break;
		link = &(*link)->next;
	}

	/* A name with no entry is already unset. */
	variable = *link;
	if (variable == NULL)
		return 0;

	/* A read-only variable stays. */
	if ((variable->flags & SH_VAR_READONLY) != 0)
		return -1;

	/* Removes and frees the entry. */
	*link = variable->next;
	free(variable->name);
	free(variable->value);
	free(variable);
	variable_changed(name);

	/* Succeeded: the name is unset. */
	return 0;
}

/*
 * Adds attributes to a variable, creating it unset when there is none.
 */
void
sh_var_add_flags(
	const char *name,
	int flags)
{
	struct variable *variable;

	/* Creates the name when it is not there. */
	variable = find_variable(name);
	if (variable == NULL)
		variable = create_variable(name);

	/* Adds the attributes. */
	variable->flags |= flags;
}

/*
 * Takes attributes off a variable; the read-only one is never taken off.
 */
void
sh_var_remove_flags(
	const char *name,
	int flags)
{
	struct variable *variable;

	/* A name that is not there has no attributes. */
	variable = find_variable(name);
	if (variable == NULL)
		return;

	/* Takes them off. */
	variable->flags &= ~(flags & ~SH_VAR_READONLY);
}

/*
 * Takes the export attribute off every variable (env -i, in a child).
 */
void
sh_var_clear_exports(
	void)
{
	struct variable *variable;
	unsigned int bucket;

	/* Every entry of every chain. */
	for (bucket = 0; bucket < VAR_BUCKETS; bucket++) {
		for (variable = variable_table[bucket];
		     variable != NULL;
		     variable = variable->next)
			variable->flags &= ~SH_VAR_EXPORT;
	}
}

/*
 * Imports the environment the shell was started with; each entry becomes an
 * exported variable.
 */
void
sh_var_import_environment(
	char **environment)
{
	struct variable *lineno;
	const char *equals;
	char *name;
	int valid;

	/* Walks the environment, skipping entries that name no variable. */
	for (;
	     environment != NULL && *environment != NULL;
	     environment++) {
		equals = strchr(*environment, '=');
		if (equals == NULL)
			continue;
		name = sh_strndup(*environment,
				  (size_t)(equals - *environment));
		valid = sh_var_name(name);
		if (valid)
			(void)sh_var_set(name, equals + 1, SH_VAR_EXPORT);
		free(name);
	}

	/* LINENO follows the command being run until something sets it. */
	(void)sh_var_set("LINENO", "", 0);
	lineno = find_variable("LINENO");
	lineno->flags |= VAR_DYNAMIC_LINENO;
}

/*
 * Builds the environment of a command from the exported variables.
 */
char **
sh_var_environment(
	void)
{
	struct variable *variable;
	char **environment;
	size_t count;
	size_t index;
	size_t length;
	unsigned int bucket;
	const char *value;
	int exported;

	/* Counts the exported variables that have a value. */
	count = 0;
	for (bucket = 0; bucket < VAR_BUCKETS; bucket++) {
		for (variable = variable_table[bucket];
		     variable != NULL;
		     variable = variable->next) {
			exported = is_exported_value(variable);
			if (exported)
				count++;
		}
	}

	/* Makes one "name=value" string for each. */
	environment = sh_malloc((count + 1U) * sizeof(*environment));
	index = 0;
	for (bucket = 0; bucket < VAR_BUCKETS; bucket++) {
		for (variable = variable_table[bucket];
		     variable != NULL;
		     variable = variable->next) {
			exported = is_exported_value(variable);
			if (!exported)
				continue;
			value = sh_var_get(variable->name);
			length = strlen(variable->name) + strlen(value) + 2U;
			environment[index] = sh_malloc(length);
			snprintf(environment[index], length, "%s=%s",
				 variable->name, value);
			index++;
		}
	}

	/* The array ends with a null pointer. */
	environment[index] = NULL;

	/* Succeeded: the environment, which the caller frees. */
	return environment;
}

/*
 * Frees an environment sh_var_environment built.
 */
void
sh_var_environment_free(
	char **environment)
{
	size_t index;

	/* Frees each string, then the array. */
	for (index = 0; environment[index] != NULL; index++)
		free(environment[index]);
	free(environment);
}

/*
 * Prints variables in a form the shell reads back.
 *
 * With an attribute, prints each variable that has it as the command named
 * would set it again ("export name='value'"); with none, prints every set
 * variable as an assignment, which is what set with no operands does.
 */
void
sh_var_print(
	int flags,
	const char *command)
{
	struct variable **sorted;
	struct variable *variable;
	size_t count;
	size_t index;

	/* Walks the variables in the order of their names. */
	sorted = sorted_variables(&count);
	for (index = 0; index < count; index++) {
		variable = sorted[index];

		/* Only those with the attribute, or only set ones for set. */
		if (flags != 0 && (variable->flags & flags) == 0)
			continue;
		if (flags == 0 && variable->value == NULL)
			continue;

		/* The command, the name, and the quoted value when there is one. */
		if (command != NULL)
			printf("%s ", command);
		fputs(variable->name, stdout);
		if (variable->value != NULL) {
			putchar('=');
			sh_print_quoted_always(sh_var_get(variable->name));
		}

		/* Each variable is a line of its own. */
		putchar('\n');
	}

	/* The sorted copy was only for the listing. */
	free(sorted);
}

/*
 * Prints one variable as declare -p does.  Returns 0 when there is no such
 * variable.
 */
int
sh_var_print_declare(
	const char *name)
{
	struct variable *variable;

	/* A name that is not there is not printed. */
	variable = find_variable(name);
	if (variable == NULL)
		return 0;

	/* Succeeded: the line. */
	print_declare(variable);
	return 1;
}

/*
 * Prints, as declare -p does, every variable that has all the attributes
 * given (every variable for none).
 */
void
sh_var_print_declared(
	int flags)
{
	struct variable **sorted;
	size_t count;
	size_t index;

	/* In the order of their names. */
	sorted = sorted_variables(&count);
	for (index = 0; index < count; index++) {
		if ((sorted[index]->flags & flags) != flags)
			continue;
		print_declare(sorted[index]);
	}

	/* The sorted copy was only for the listing. */
	free(sorted);
}

/*
 * Starts the local variables of a function call.
 */
void
sh_var_local_push(
	void)
{
	struct local_frame *frame;

	/* Pushes an empty frame. */
	frame = sh_malloc(sizeof(*frame));
	frame->saved = NULL;
	frame->previous = local_frames;
	local_frames = frame;
	local_depth++;
}

/*
 * Ends the local variables of a function call, putting back each name it
 * made local.
 */
void
sh_var_local_pop(
	void)
{
	struct local_frame *frame;
	struct saved_variable *saved;

	/* Nothing to pop. */
	frame = local_frames;
	if (frame == NULL)
		return;
	local_frames = frame->previous;
	local_depth--;

	/* Restores newest first, so a name made local twice ends as it began. */
	while (frame->saved != NULL) {
		saved = frame->saved;
		frame->saved = saved->next;
		restore_saved(saved);
	}

	/* The frame itself goes last. */
	free(frame);
}

/*
 * Reports how many local frames there are.
 */
int
sh_var_local_depth(
	void)
{
	/* Succeeded: the depth. */
	return local_depth;
}

/*
 * Pops local frames down to a depth.
 */
void
sh_var_local_unwind(
	int depth)
{
	/* Pops the frames above the depth. */
	while (local_depth > depth)
		sh_var_local_pop();
}

/*
 * Makes a name local to the innermost function call.  Returns -1 outside a
 * function.
 */
int
sh_var_make_local(
	const char *name)
{
	struct saved_variable *saved;
	struct variable *variable;
	int compare;

	/* Only a function has locals. */
	if (local_frames == NULL)
		return -1;

	/* A name already local to this call is left as it is. */
	for (saved = local_frames->saved; saved != NULL; saved = saved->next) {
		compare = strcmp(saved->name, name);
		if (compare == 0)
			return 0;
	}

	/* Records the name. */
	saved = sh_malloc(sizeof(*saved));
	memset(saved, 0, sizeof(*saved));
	saved->name = sh_strdup(name);

	/* Saves what the name holds now; it keeps holding it, as in dash. */
	variable = find_variable(name);
	if (variable != NULL) {
		saved->existed = 1;
		if (variable->value != NULL)
			saved->value = sh_strdup(variable->value);
		saved->flags = variable->flags;
	}

	/* The newest save is restored first. */
	saved->next = local_frames->saved;
	local_frames->saved = saved;

	/* Succeeded: the name is local. */
	return 0;
}

/*
 * Saves the options, to be put back when the function returns (local -).
 */
void
sh_var_local_options(
	void)
{
	struct saved_variable *saved;

	/* Outside a function there is nothing to put them back at. */
	if (local_frames == NULL)
		return;

	/* Saves them under the name "-", which no variable has. */
	saved = sh_malloc(sizeof(*saved));
	memset(saved, 0, sizeof(*saved));
	saved->name = sh_strdup("-");
	memcpy(saved->options, sh_option, sizeof(saved->options));
	saved->next = local_frames->saved;
	local_frames->saved = saved;
}

/*
 * Registers a function to hear about changes to a variable.
 */
void
sh_var_hook(
	const char *name,
	void (*changed)(const char *))
{
	/* The table is small and fixed; the shell registers few names. */
	if (variable_hook_count == VAR_HOOK_MAX)
		return;
	variable_hooks[variable_hook_count].name = name;
	variable_hooks[variable_hook_count].changed = changed;
	variable_hook_count++;
}

/*
 * Reports whether the shell watches a variable, so that a change of it
 * does more than change its value.
 */
int
sh_var_hooked(
	const char *name)
{
	int index;
	int compare;

	/* Each registered name. */
	for (index = 0; index < variable_hook_count; index++) {
		compare = strcmp(variable_hooks[index].name, name);
		if (compare == 0)
			return 1;
	}

	/* Not watched. */
	return 0;
}

/* Reports whether a character may begin a name. */
static int
is_name_start(
	char value)
{
	/* A letter or an underscore. */
	if (value == '_')
		return 1;
	if (value >= 'a' && value <= 'z')
		return 1;
	if (value >= 'A' && value <= 'Z')
		return 1;

	/* Anything else. */
	return 0;
}

/* Reports whether a character may be in a name after its first. */
static int
is_name_character(
	char value)
{
	int start;

	/* A digit, or anything that may begin a name. */
	if (value >= '0' && value <= '9')
		return 1;
	start = is_name_start(value);

	/* Succeeded: whether it may. */
	return start;
}

/* Hashes a name into a chain of the table. */
static unsigned int
name_hash(
	const char *name)
{
	unsigned int hash;

	/* FNV-1a over the bytes of the name. */
	hash = 2166136261U;
	for (; *name != '\0'; name++) {
		hash ^= (unsigned char)*name;
		hash *= 16777619U;
	}

	/* Succeeded: the chain. */
	return hash & (VAR_BUCKETS - 1U);
}

/* Finds a variable's entry. */
static struct variable *
find_variable(
	const char *name)
{
	struct variable *variable;
	int compare;

	/* Walks the name's chain. */
	for (variable = variable_table[name_hash(name)];
	     variable != NULL;
	     variable = variable->next) {
		compare = strcmp(variable->name, name);
		if (compare == 0)
			return variable;
	}

	/* No entry: the name is unset. */
	return NULL;
}

/* Makes an unset entry for a name. */
static struct variable *
create_variable(
	const char *name)
{
	struct variable *variable;
	unsigned int bucket;

	/* Allocates the entry, unset and without attributes. */
	variable = sh_malloc(sizeof(*variable));
	variable->name = sh_strdup(name);
	variable->value = NULL;
	variable->flags = 0;

	/* Links it at the head of its chain. */
	bucket = name_hash(name);
	variable->next = variable_table[bucket];
	variable_table[bucket] = variable;

	/* Succeeded: the new entry. */
	return variable;
}

/* Tells whatever watches a variable that it changed. */
static void
variable_changed(
	const char *name)
{
	int index;
	int compare;

	/* Calls the hooks registered for the name. */
	for (index = 0; index < variable_hook_count; index++) {
		compare = strcmp(variable_hooks[index].name, name);
		if (compare == 0)
			variable_hooks[index].changed(name);
	}
}

/* Orders variables by name, for qsort. */
static int
compare_variables(
	const void *left,
	const void *right)
{
	const struct variable *const *a;
	const struct variable *const *b;
	int order;

	/* Compares the names bytewise. */
	a = left;
	b = right;
	order = strcmp((*a)->name, (*b)->name);

	/* Succeeded: their order. */
	return order;
}

/* Makes an array of every variable, sorted by name. */
static struct variable **
sorted_variables(
	size_t *count)
{
	struct variable **sorted;
	struct variable *variable;
	unsigned int bucket;
	size_t total;

	/* Counts the entries. */
	total = 0;
	for (bucket = 0; bucket < VAR_BUCKETS; bucket++) {
		for (variable = variable_table[bucket];
		     variable != NULL;
		     variable = variable->next)
			total++;
	}

	/* Collects them. */
	sorted = sh_malloc((total + 1U) * sizeof(*sorted));
	total = 0;
	for (bucket = 0; bucket < VAR_BUCKETS; bucket++) {
		for (variable = variable_table[bucket];
		     variable != NULL;
		     variable = variable->next)
			sorted[total++] = variable;
	}

	/* Sorts them by name. */
	qsort(sorted, total, sizeof(*sorted), compare_variables);
	*count = total;

	/* Succeeded: the array, which the caller frees. */
	return sorted;
}

/* Puts back what a name held before it was made local, and frees the record. */
static void
restore_saved(
	struct saved_variable *saved)
{
	struct variable *variable;
	int compare;

	/* Dispatches on what was saved: the options, a new name, an old one. */
	compare = strcmp(saved->name, "-");
	if (compare == 0) {
		/* "-" was the options. */
		memcpy(sh_option, saved->options, sizeof(saved->options));
	} else if (!saved->existed) {
		/* A name that was not there before goes, read-only or not. */
		variable = find_variable(saved->name);
		if (variable != NULL)
			variable->flags &= ~SH_VAR_READONLY;
		(void)sh_var_unset(saved->name);
	} else {
		/* A name that was there gets its value and attributes back. */
		variable = find_variable(saved->name);
		if (variable == NULL)
			variable = create_variable(saved->name);
		free(variable->value);
		variable->value = saved->value;
		saved->value = NULL;
		variable->flags = saved->flags;
		variable_changed(saved->name);
	}

	/* The record goes. */
	free(saved->name);
	free(saved->value);
	free(saved);
}

/*
 * Returns a copy of a value converted as the attributes say: evaluated as
 * an arithmetic expression (an error in it is the shell's error), or made
 * lower or upper case.
 */
static char *
converted_value(
	const char *value,
	int flags)
{
	struct sh_expand_context context;
	const char *error_text;
	char number[24];
	char *copy;
	size_t index;
	long result;
	int ok;

	/* An integer: the value of the expression. */
	if ((flags & SH_VAR_INTEGER) != 0) {
		sh_expand_context_fill(&context);
		ok = sh_expand_arithmetic(value, &context, &result, &error_text);
		if (!ok)
			sh_error("%s", error_text);
		snprintf(number, sizeof(number), "%ld", result);
		return sh_strdup(number);
	}

	/* Lower or upper case, letter by letter. */
	copy = sh_strdup(value);
	for (index = 0; copy[index] != '\0'; index++) {
		if ((flags & SH_VAR_LOWER) != 0)
			copy[index] = (char)tolower((unsigned char)copy[index]);
		else if ((flags & SH_VAR_UPPER) != 0)
			copy[index] = (char)toupper((unsigned char)copy[index]);
	}

	/* Succeeded: the value to hold. */
	return copy;
}

/*
 * Prints a variable as declare -p does: its attributes (-- for none), and
 * its value when it has one, in double quotes, or in $'...' when it holds
 * a control character.
 */
static void
print_declare(
	const struct variable *variable)
{
	const char *value;
	int flags;

	/* The attributes, in the order bash prints them. */
	flags = variable->flags & VAR_ATTRIBUTES;
	fputs("declare -", stdout);
	if (flags == 0)
		putchar('-');
	if ((flags & SH_VAR_INTEGER) != 0)
		putchar('i');
	if ((flags & SH_VAR_LOWER) != 0)
		putchar('l');
	if ((flags & SH_VAR_READONLY) != 0)
		putchar('r');
	if ((flags & SH_VAR_UPPER) != 0)
		putchar('u');
	if ((flags & SH_VAR_EXPORT) != 0)
		putchar('x');
	printf(" %s", variable->name);

	/* The value, when it is set. */
	value = sh_var_get(variable->name);
	if (value != NULL) {
		putchar('=');
		print_declare_value(value);
	}

	/* A line each. */
	putchar('\n');
}

/* Prints a value quoted as declare -p quotes it. */
static void
print_declare_value(
	const char *value)
{
	static const char escapes[] = "\a" "a" "\b" "b" "\033" "e" "\f" "f"
				      "\n" "n" "\r" "r" "\t" "t" "\v" "v";
	const char *cursor;
	const char *escape;
	int control;

	/* Is there a control character? */
	control = 0;
	for (cursor = value; *cursor != '\0'; cursor++) {
		if ((unsigned char)*cursor < 0x20 || *cursor == 0x7f)
			control = 1;
	}

	/* Without one: double quotes, with \ before " \ $ and `. */
	if (!control) {
		putchar('"');
		for (cursor = value; *cursor != '\0'; cursor++) {
			if (*cursor == '"' || *cursor == '\\' ||
			    *cursor == '$' || *cursor == '`')
				putchar('\\');
			putchar(*cursor);
		}
		putchar('"');
		return;
	}

	/* With one: $'...', with the control characters as escapes. */
	fputs("$'", stdout);
	for (cursor = value; *cursor != '\0'; cursor++) {
		escape = memchr(escapes, *cursor, sizeof(escapes) - 1U);
		if (escape != NULL && (escape - escapes) % 2 == 0) {
			printf("\\%c", escape[1]);
		} else if (*cursor == '\'' || *cursor == '\\') {
			printf("\\%c", *cursor);
		} else if ((unsigned char)*cursor < 0x20 || *cursor == 0x7f) {
			printf("\\%03o", (unsigned char)*cursor);
		} else {
			putchar(*cursor);
		}
	}
	putchar('\'');
}

/* Reports whether a variable goes into the environment: exported, with a value. */
static int
is_exported_value(
	const struct variable *variable)
{
	/* Exported, and set. */
	if ((variable->flags & SH_VAR_EXPORT) == 0)
		return 0;
	if (variable->value == NULL)
		return 0;

	/* Succeeded: it goes in. */
	return 1;
}
