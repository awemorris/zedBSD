/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a command name is (POSIX XCU 2.9.1.1): the table of functions, the
 * search of PATH with the remembered paths of commands, and the builtins
 * that act on the shell's own state: ., eval, exec, exit, return, break,
 * continue, shift, command, type, hash, unset, export, readonly and local.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/alias.h"
#include "userland/base/sh/vars.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The number of chains of the table of remembered command paths. */
#define HASH_BUCKETS 64U

/* The PATH command -p searches: where the standard utilities are. */
#define DEFAULT_PATH "/bin:/sbin:/usr/bin:/usr/sbin"

/*
 * A command's path, remembered so that PATH is searched once per name.
 * Forgotten when PATH changes, and by hash -r.
 */
struct hash_entry {
	struct hash_entry *next;
	char *name;
	char *path;
};

/* The reserved words, which command -v and type name as such. */
static const char *const reserved_words[] = {
	"!", "case", "do", "done", "elif", "else", "esac", "fi", "for", "if",
	"in", "then", "until", "while", "{", "}", NULL
};

/*
 * The defined functions, newest first.  Each holds the block its body was
 * parsed into until it is replaced or unset.
 */
static struct sh_function *functions;

/* The remembered paths of commands found on PATH. */
static struct hash_entry *hash_table[HASH_BUCKETS];

static int search_path(const char *name, const char *path, char *result, size_t capacity);
static int search_path_for(const char *name, const char *path, char *result, size_t capacity, int readable);
static int is_executable(const char *path);
static int is_readable_file(const char *path);
static int is_reserved_word(const char *name);
static int describe_found(const char *name, const struct sh_command *entry, int verbose);
static unsigned int command_hash(const char *name);
static const char *hash_lookup(const char *name);
static int find_without_remembering(const char *name, char *result, size_t capacity);
static void hash_store(const char *name, const char *path);
static void hash_clear(const char *name);
static int declare_names(int argc, char **argv, int flags, const char *command);
static char *operand_name(const char *operand, const char **equals);
static int parse_count(const char *text, int *count);
static int is_double_dash(const char *word);
static void skip_double_dash(int *argc, char ***argv);
static int declare_variables(int argc, char **argv, const char *command, int local);
static int declare_options(int argc, char **argv, const char *command, int *index, int *add, int *remove, int *mode);
static int declare_one(const char *operand, const char *command, int local, int add, int remove);
static int declare_functions(int argc, char **argv, int index, int names_only);
static int compare_names(const void *left, const void *right);
static void dot_parameters_restore(struct sh_parameters *saved, int generation);

/* What declare does besides setting: -p prints, -f and -F print functions. */
#define DECLARE_PRINT		0x01
#define DECLARE_FUNCTIONS	0x02
#define DECLARE_FUNCTION_NAMES	0x04
#define DECLARE_GLOBAL		0x08

/*
 * Finds a defined function.
 */
struct sh_function *
sh_function_find(
	const char *name)
{
	struct sh_function *function;
	int compare;

	/* Walks the list. */
	for (function = functions; function != NULL; function = function->next) {
		compare = strcmp(function->name, name);
		if (compare == 0)
			return function;
	}

	/* No function has that name. */
	return NULL;
}

/*
 * Defines a function, replacing any of the same name.
 */
void
sh_function_define(
	const char *name,
	struct sh_node *body,
	struct sh_arena *arena)
{
	struct sh_function *function;

	/* Finds the definition to replace, or makes one at the head. */
	function = sh_function_find(name);
	if (function == NULL) {
		function = sh_malloc(sizeof(*function));
		function->name = sh_strdup(name);
		function->arena = NULL;
		function->next = functions;
		functions = function;
	}

	/* The function holds the block its body lives in, and drops the old. */
	if (arena != NULL)
		sh_arena_hold(arena);
	sh_arena_release(function->arena);
	function->arena = arena;
	function->body = body;
}

/*
 * Removes a function.  Returns 0, whether or not there was one.
 */
int
sh_function_unset(
	const char *name)
{
	struct sh_function **link;
	struct sh_function *function;
	int compare;

	/* Finds the link to it. */
	link = &functions;
	while (*link != NULL) {
		compare = strcmp((*link)->name, name);
		if (compare == 0)
			break;
		link = &(*link)->next;
	}

	/* No function of that name is no error. */
	function = *link;
	if (function == NULL)
		return 0;

	/* Unlinks and frees it; a call in progress holds its own block. */
	*link = function->next;
	sh_arena_release(function->arena);
	free(function->name);
	free(function);

	/* Succeeded: the name has no function now. */
	return 0;
}

/*
 * Prints a function's definition, as it would be read back.
 */
void
sh_function_print(
	const char *name)
{
	struct sh_function *function;
	char *text;

	/* A name with no function prints nothing. */
	function = sh_function_find(name);
	if (function == NULL)
		return;

	/* The name, then the body. */
	text = sh_node_text(function->body);
	printf("%s() %s\n", name, text);
	free(text);
}

/*
 * Finds what a command name is: a special builtin, then (unless
 * SH_FIND_NO_FUNCTIONS) a function, then a builtin, then a file on the PATH
 * given (a PATH= assignment of the command) or on PATH.
 */
void
sh_find_command(
	const char *name,
	int flags,
	const char *path,
	struct sh_command *entry)
{
	const struct sh_builtin *builtin;
	struct sh_function *function;
	const char *slash;
	int found;

	/* A name with a slash is a path, and nothing else. */
	memset(entry, 0, sizeof(*entry));
	slash = strchr(name, '/');
	if (slash != NULL) {
		entry->kind = SH_COMMAND_EXTERNAL;
		snprintf(entry->path, sizeof(entry->path), "%s", name);
		return;
	}

	/* A special builtin comes first. */
	builtin = sh_builtin_find(name);
	if (builtin != NULL && (builtin->flags & SH_BUILTIN_SPECIAL) != 0) {
		entry->kind = SH_COMMAND_SPECIAL;
		entry->builtin = builtin;
		return;
	}

	/* Then a function. */
	if ((flags & (SH_FIND_NO_FUNCTIONS | SH_FIND_BUILTIN_ONLY)) == 0) {
		function = sh_function_find(name);
		if (function != NULL) {
			entry->kind = SH_COMMAND_FUNCTION;
			entry->function = function;
			return;
		}
	}

	/* Then a builtin. */
	if (builtin != NULL) {
		entry->kind = SH_COMMAND_BUILTIN;
		entry->builtin = builtin;
		return;
	}

	/* builtin looks no further. */
	if ((flags & SH_FIND_BUILTIN_ONLY) != 0) {
		entry->kind = SH_COMMAND_NOT_FOUND;
		return;
	}

	/* Then a file on the PATH the command is given, or on PATH. */
	if (path != NULL && (flags & SH_FIND_DEFAULT_PATH) == 0)
		found = search_path(name, path, entry->path, sizeof(entry->path));
	else if ((flags & SH_FIND_NO_REMEMBER) != 0 &&
		 (flags & SH_FIND_DEFAULT_PATH) == 0)
		found = find_without_remembering(name, entry->path, sizeof(entry->path));
	else
		found = sh_find_command_path(name, entry->path, sizeof(entry->path), (flags & SH_FIND_DEFAULT_PATH) != 0);
	if (found)
		entry->kind = SH_COMMAND_EXTERNAL;
	else
		entry->kind = SH_COMMAND_NOT_FOUND;
}

/*
 * Finds a command on PATH the way a subshell would, using a remembered
 * path but remembering nothing new: the shell that asks runs the command
 * for a subshell (a pipeline's), whose lookups the shell does not keep.
 * Returns 1 with the path in result.
 */
static int
find_without_remembering(
	const char *name,
	char *result,
	size_t capacity)
{
	const char *remembered;
	const char *path;
	int found;

	/* A remembered path is used as it is. */
	remembered = hash_lookup(name);
	if (remembered != NULL) {
		snprintf(result, capacity, "%s", remembered);
		return 1;
	}

	/* Otherwise PATH is searched. */
	path = sh_var_get("PATH");
	if (path == NULL)
		path = "";
	found = search_path(name, path, result, capacity);

	/* Reports whether it was found. */
	return found;
}

/*
 * Searches PATH (or the default PATH) for a command, remembering what it
 * finds.  Returns 1 with the path in result.
 */
int
sh_find_command_path(
	const char *name,
	char *result,
	size_t capacity,
	int use_default_path)
{
	const char *path;
	const char *remembered;
	const char *slash;
	int found;

	/* A name with a slash is its own path. */
	slash = strchr(name, '/');
	if (slash != NULL) {
		snprintf(result, capacity, "%s", name);
		return 1;
	}

	/* A remembered path is used as it is, as dash does. */
	if (!use_default_path) {
		remembered = hash_lookup(name);
		if (remembered != NULL) {
			snprintf(result, capacity, "%s", remembered);
			return 1;
		}
	}

	/* Otherwise PATH is searched. */
	if (use_default_path)
		path = DEFAULT_PATH;
	else
		path = sh_var_get("PATH");
	if (path == NULL)
		path = "";
	found = search_path(name, path, result, capacity);
	if (!found)
		return 0;

	/* What was found is remembered. */
	if (!use_default_path)
		hash_store(name, result);

	/* Succeeded: the path is in result. */
	return 1;
}

/*
 * Searches the directories of PATH for a file that may be read, as the dot
 * builtin does.  Returns 1 with the path in result.
 */
int
sh_search_readable(
	const char *name,
	const char *path,
	char *result,
	size_t capacity)
{
	int found;

	/* The search of PATH, for a readable regular file. */
	found = search_path_for(name, path, result, capacity, 1);

	/* Succeeded: whether one was found. */
	return found;
}

/*
 * Tells how a name would be run (command -v with verbose 0, command -V and
 * type with verbose 1).  Returns 0 when it was found.
 */
int
sh_command_describe(
	const char *name,
	int verbose)
{
	struct sh_command entry;
	const char *alias;
	int reserved;
	int status;

	/* Reserved words come first, as the parser sees them. */
	reserved = is_reserved_word(name);
	if (reserved) {
		if (verbose)
			printf("%s is a shell keyword\n", name);
		else
			printf("%s\n", name);
		return 0;
	}

	/* Then aliases. */
	alias = sh_alias_get(name);
	if (alias != NULL) {
		if (verbose) {
			printf("%s is an alias for %s\n", name, alias);
		} else {
			printf("alias %s=", name);
			sh_print_quoted_always(alias);
			putchar('\n');
		}

		/* Succeeded: the alias was described. */
		return 0;
	}

	/* Then what running the name would find. */
	sh_find_command(name, 0, NULL, &entry);
	status = describe_found(name, &entry, verbose);

	/* Succeeded: 0 when it was found. */
	return status;
}

/*
 * Tells the shell PATH changed: remembered command paths are forgotten.
 */
void
sh_path_changed(
	const char *name)
{
	/* Every remembered path may now be wrong. */
	(void)name;
	hash_clear(NULL);
}

/*
 * Implements the dot builtin (and source): reads and runs the commands of a
 * file in this shell.  Operands after the file (bash) are the positional
 * parameters while it runs, and are put back after it unless it set others.
 */
int
sh_builtin_dot(
	int argc,
	char **argv)
{
	struct sh_parameters saved_parameters;
	struct sh_handler handler;
	char path[PATH_MAX];
	int generation;
	int given;
	int kind;
	const char *search;
	const char *slash;
	int descriptor;
	int status;
	int found;

	/* -- may come first; the file is the operand. */
	skip_double_dash(&argc, &argv);
	if (argc < 2)
		sh_error(".: filename argument required");

	/* A name with a slash is a path; any other is searched for on PATH. */
	slash = strchr(argv[1], '/');
	if (slash != NULL) {
		snprintf(path, sizeof(path), "%s", argv[1]);
	} else {
		search = sh_var_get("PATH");
		if (search == NULL)
			search = "";
		found = sh_search_readable(argv[1], search, path, sizeof(path));
		if (!found)
			sh_error(".: %s: not found", argv[1]);
	}

	/* Opens it out of the way of the descriptors scripts use. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		sh_error(".: cannot open %s: %s", path, strerror(errno));
	descriptor = sh_descriptor_high(descriptor);

	/* Operands after the file are the positional parameters. */
	given = argc > 2;
	generation = sh_parameters_generation;
	if (given) {
		saved_parameters = sh_parameters;
		sh_parameters.count = 0;
		sh_parameters.values = NULL;
		sh_parameters.owned = 0;
		sh_parameters_set(argc - 2, argv + 2);

		/* An exception out of the file puts them back too. */
		sh_handler_push(&handler);
		kind = setjmp(handler.environment);
		if (kind != 0) {
			sh_handler_unwind(&handler);
			dot_parameters_restore(&saved_parameters, generation);
			sh_raise(kind);
		}
	}

	/* Runs its commands; a return ends it. */
	sh_input_push_file(descriptor, 0);
	sh_dot_nest++;
	status = sh_eval_input(0);
	sh_dot_nest--;
	sh_input_pop();
	if (sh_skip == SH_SKIP_RETURN) {
		sh_skip = SH_SKIP_NONE;
		status = sh_status;
	}

	/* The parameters before it, unless it set others. */
	if (given) {
		sh_handler_pop(&handler);
		dot_parameters_restore(&saved_parameters, generation);
	}

	/* Succeeded: the status of the last command. */
	return status;
}

/*
 * Implements the eval builtin: runs its operands, joined by spaces, as
 * commands.
 */
int
sh_builtin_eval(
	int argc,
	char **argv)
{
	char *text;
	size_t length;
	int index;
	int status;

	/* -- may come first. */
	skip_double_dash(&argc, &argv);

	/* Joins the operands with spaces. */
	length = 1;
	for (index = 1; index < argc; index++)
		length += strlen(argv[index]) + 1U;
	text = sh_temp_own(sh_malloc(length));
	text[0] = '\0';
	for (index = 1; index < argc; index++) {
		if (index > 1)
			strcat(text, " ");
		strcat(text, argv[index]);
	}

	/* Runs them; with none the status is 0. */
	status = sh_eval_string(text, 0);

	/* Succeeded: the status of the last command. */
	return status;
}

/*
 * Implements the exec builtin: with a command, replaces the shell with it;
 * without one, makes the redirections of this command permanent.
 */
int
sh_builtin_exec(
	int argc,
	char **argv)
{
	char path[PATH_MAX];
	int found;

	/* -- may come first. */
	skip_double_dash(&argc, &argv);

	/* With no command the redirections stay. */
	if (argc < 2) {
		sh_redirect_forget();
		return 0;
	}

	/* The command replaces the shell; one not found ends it with 127. */
	found = sh_find_command_path(argv[1], path, sizeof(path), 0);
	if (!found) {
		sh_warn("exec: %s: not found", argv[1]);
		sh_exit(127);
	}

	/* The command replaces the shell. */
	sh_exec_external(path, argv + 1, NULL, 0);
}

/*
 * Implements the exit builtin.
 */
int
sh_builtin_exit(
	int argc,
	char **argv)
{
	int status;
	int parsed;

	/* The status given, or the last command's. */
	status = sh_status;
	if (argc > 1) {
		parsed = parse_count(argv[1], &status);
		if (!parsed)
			sh_error("exit: Illegal number: %s", argv[1]);
	}

	/* The shell ends. */
	sh_exit(status);
}

/*
 * Implements the return builtin: ends a function or a dot script.
 */
int
sh_builtin_return(
	int argc,
	char **argv)
{
	int status;
	int parsed;

	/* The status given, or the last command's. */
	status = sh_status;
	if (argc > 1) {
		parsed = parse_count(argv[1], &status);
		if (!parsed)
			sh_error("return: Illegal number: %s", argv[1]);
	}

	/* The function or script around it stops here. */
	sh_skip = SH_SKIP_RETURN;
	sh_skip_count = 1;
	sh_status = status;

	/* Succeeded: the status the function ends with. */
	return status;
}

/*
 * Implements break and continue.
 */
int
sh_builtin_break(
	int argc,
	char **argv)
{
	int count;
	int parsed;
	int compare;

	/* The number of loops, 1 when not given. */
	count = 1;
	if (argc > 1) {
		parsed = parse_count(argv[1], &count);
		if (!parsed || count < 1)
			sh_error("%s: Illegal number: %s", argv[0], argv[1]);
	}

	/* Outside a loop there is nothing to leave. */
	if (sh_loop_nest == 0)
		return 0;
	if (count > sh_loop_nest)
		count = sh_loop_nest;

	/*
	 * The loops the break or continue names stop at the skip; the count
	 * says how many of them it passes through.
	 */
	compare = strcmp(argv[0], "break");
	if (compare == 0)
		sh_skip = SH_SKIP_BREAK;
	else
		sh_skip = SH_SKIP_CONTINUE;
	sh_skip_count = count;

	/* Succeeded. */
	return 0;
}

/*
 * Implements the shift builtin.
 */
int
sh_builtin_shift(
	int argc,
	char **argv)
{
	struct sh_parameters shifted;
	int count;
	int index;
	int parsed;

	/* The number to shift, 1 when not given; more than there are is an error. */
	count = 1;
	if (argc > 1) {
		parsed = parse_count(argv[1], &count);
		if (!parsed)
			sh_error("shift: Illegal number: %s", argv[1]);
	}

	/* More than there are is an error. */
	if (count > sh_parameters.count)
		sh_error("shift: can't shift that many");
	if (count == 0)
		return 0;

	/* Keeps the rest. */
	shifted.count = sh_parameters.count - count;
	shifted.owned = 1;
	shifted.values = sh_malloc(((size_t)shifted.count + 1U) *
				   sizeof(char *));
	for (index = 0; index < shifted.count; index++)
		shifted.values[index] = sh_strdup(sh_parameters.values[index + count]);
	shifted.values[shifted.count] = NULL;

	/* They replace the old ones. */
	sh_parameters_free(&sh_parameters);
	sh_parameters = shifted;

	/* Succeeded. */
	return 0;
}

/*
 * Implements command -v and command -V; the running of a command through
 * command is done where simple commands are run.
 */
int
sh_builtin_command(
	int argc,
	char **argv)
{
	const char *word;
	int verbose;
	int index;
	int status;
	int option;
	int dashes;

	/* Reads -p, -v and -V. */
	verbose = -1;
	for (index = 1; index < argc; index++) {
		word = argv[index];
		if (word[0] != '-')
			break;
		dashes = is_double_dash(word);
		if (dashes) {
			index++;
			break;
		}

		/* The option letters. */
		for (option = 1; word[option] != '\0'; option++) {
			if (word[option] == 'v') {
				verbose = 0;
			} else if (word[option] == 'V') {
				verbose = 1;
			} else if (word[option] != 'p') {
				fprintf(stderr, "command: illegal option -%c\n",
					word[option]);
				return 2;
			}
		}
	}

	/* Without -v or -V there is nothing left to do. */
	if (verbose < 0 || index >= argc)
		return 0;

	/* Describes the name (only the first, as dash does). */
	status = sh_command_describe(argv[index], verbose);
	if (status != 0)
		return 127;

	/* Succeeded: the name was found. */
	return 0;
}

/*
 * Implements the type builtin.
 */
int
sh_builtin_type(
	int argc,
	char **argv)
{
	int index;
	int status;
	int described;

	/* Describes each name as command -V does. */
	status = 0;
	for (index = 1; index < argc; index++) {
		described = sh_command_describe(argv[index], 1);
		if (described != 0)
			status = 127;
	}

	/* Succeeded: 0 when every name was found. */
	return status;
}

/*
 * Implements the hash builtin: remembers the paths of commands, or lists
 * them, or (-r) forgets them.
 */
int
sh_builtin_hash(
	int argc,
	char **argv)
{
	const struct sh_builtin *builtin;
	struct sh_function *function;
	struct hash_entry *entry;
	char path[PATH_MAX];
	unsigned int bucket;
	int index;
	int status;
	int found;

	/* With no operands, lists what is remembered. */
	if (argc < 2) {
		for (bucket = 0; bucket < HASH_BUCKETS; bucket++) {
			for (entry = hash_table[bucket];
			     entry != NULL;
			     entry = entry->next)
				printf("%s\n", entry->path);
		}

		/* Succeeded: the table was listed. */
		return 0;
	}

	/* -r forgets everything. */
	index = 1;
	if (argv[1][0] == '-' && argv[1][1] == 'r' && argv[1][2] == '\0') {
		hash_clear(NULL);
		index = 2;
	}

	/* Looks each name up; builtins and functions are not remembered. */
	status = 0;
	for (; index < argc; index++) {
		builtin = sh_builtin_find(argv[index]);
		if (builtin != NULL)
			continue;
		function = sh_function_find(argv[index]);
		if (function != NULL)
			continue;
		hash_clear(argv[index]);
		found = sh_find_command_path(argv[index], path, sizeof(path), 0);
		if (!found) {
			fprintf(stderr, "hash: %s: not found\n", argv[index]);
			status = 1;
		}
	}

	/* Succeeded: 0 when every name was found. */
	return status;
}

/*
 * Implements the unset builtin: variables (-v, the default) or functions (-f).
 */
int
sh_builtin_unset(
	int argc,
	char **argv)
{
	const char *word;
	int functions_only;
	int variables_only;
	int index;
	int option;
	int valid;
	int unset;
	int dashes;

	/* Reads -f and -v. */
	functions_only = 0;
	variables_only = 0;
	for (index = 1; index < argc; index++) {
		word = argv[index];
		if (word[0] != '-')
			break;
		dashes = is_double_dash(word);
		if (dashes) {
			index++;
			break;
		}

		/* The option letters. */
		for (option = 1; word[option] != '\0'; option++) {
			if (word[option] == 'f')
				functions_only = 1;
			else if (word[option] == 'v')
				variables_only = 1;
			else
				sh_error("unset: Illegal option -%c", word[option]);
		}
	}

	/* Unsets each name: a function with -f, a variable otherwise. */
	for (; index < argc; index++) {
		if (functions_only && !variables_only) {
			(void)sh_function_unset(argv[index]);
			continue;
		}

		/* A variable name is unset as a variable, or else as a function. */
		valid = sh_var_name(argv[index]);
		if (!valid)
			sh_error("unset: %s: bad variable name", argv[index]);
		unset = sh_var_unset(argv[index]);
		if (unset != 0)
			sh_error("unset: %s: is read only", argv[index]);
	}

	/* Succeeded. */
	return 0;
}

/*
 * Implements export and readonly: sets the attribute on each name (with a
 * value when one is given), or with -p (or no operands) lists the names.
 */
int
sh_builtin_export(
	int argc,
	char **argv)
{
	int flags;
	int status;
	int compare;

	/* The attribute is the command's. */
	compare = strcmp(argv[0], "export");
	if (compare == 0)
		flags = SH_VAR_EXPORT;
	else
		flags = SH_VAR_READONLY;

	/* Applies it to the names. */
	status = declare_names(argc, argv, flags, argv[0]);

	/* Succeeded: 0 unless a name was bad. */
	return status;
}

/*
 * Implements the local builtin: makes names local to the function.
 */
int
sh_builtin_local(
	int argc,
	char **argv)
{
	const char *equals;
	char *name;
	int index;
	int set;

	/* Only a function has locals. */
	if (sh_function_nest == 0)
		sh_error("local: not in a function");

	/* Attributes as declare takes them (bash): -i, -r, -x and the like. */
	if (argc > 1 && (argv[1][0] == '-' || argv[1][0] == '+') &&
	    argv[1][1] != '\0')
		return declare_variables(argc, argv, "local", 1);

	/* Each operand is a name, with an optional value, or -. */
	for (index = 1; index < argc; index++) {
		/* - saves the options. */
		if (argv[index][0] == '-' && argv[index][1] == '\0') {
			sh_var_local_options();
			continue;
		}

		/* The name becomes local, and takes the value when one is given. */
		name = operand_name(argv[index], &equals);
		if (name == NULL)
			sh_error("local: %s: bad variable name", argv[index]);
		(void)sh_var_make_local(name);
		if (equals == NULL)
			continue;
		set = sh_var_set(name, equals + 1, 0);
		if (set != 0)
			sh_error("%s: is read only", name);
	}

	/* Succeeded. */
	return 0;
}

/*
 * Implements declare and typeset (bash): gives names attributes (-i -l -u
 * -r -x, taken off with +), with values where given, local to a function
 * unless -g; -p prints variables as declare would set them again, -f and
 * -F print functions.  Arrays (-a, -A) and references (-n) are not kept.
 */
int
sh_builtin_declare(
	int argc,
	char **argv)
{
	int status;

	/* Local in a function, as bash makes them. */
	status = declare_variables(argc, argv, argv[0], sh_function_nest > 0);

	/* Succeeded: 1 when a name could not be declared. */
	return status;
}

/*
 * Implements builtin (bash): runs a builtin, even when a function has its
 * name.
 */
int
sh_builtin_builtin(
	int argc,
	char **argv)
{
	const struct sh_builtin *builtin;
	int status;

	/* With no name, there is nothing to run. */
	skip_double_dash(&argc, &argv);
	if (argc < 2)
		return 0;

	/* The name must be a builtin's. */
	builtin = sh_builtin_find(argv[1]);
	if (builtin == NULL) {
		fprintf(stderr, "builtin: %s: not a shell builtin\n", argv[1]);
		return 1;
	}

	/* Succeeded: the builtin's status. */
	status = builtin->run(argc - 1, argv + 1);
	return status;
}

/*
 * Declares names for declare, typeset and local with options.  local is 1
 * when the names become local to the function (unless -g).
 */
static int
declare_variables(
	int argc,
	char **argv,
	const char *command,
	int local)
{
	int index;
	int add;
	int remove;
	int mode;
	int status;
	int valid;

	/* The options. */
	valid = declare_options(argc, argv, command, &index, &add, &remove, &mode);
	if (!valid)
		return 2;
	if ((mode & DECLARE_GLOBAL) != 0)
		local = 0;

	/* -f and -F are about functions. */
	if ((mode & (DECLARE_FUNCTIONS | DECLARE_FUNCTION_NAMES)) != 0)
		return declare_functions(argc, argv, index,
					 (mode & DECLARE_FUNCTION_NAMES) != 0);

	/* No names: a listing, as declare -p, or as set for bare declare. */
	if (index >= argc) {
		if ((mode & DECLARE_PRINT) != 0 || add != 0)
			sh_var_print_declared(add);
		else
			sh_var_print(0, NULL);
		return 0;
	}

	/* -p with names prints them. */
	status = 0;
	if ((mode & DECLARE_PRINT) != 0) {
		for (; index < argc; index++) {
			if (!sh_var_print_declare(argv[index])) {
				fprintf(stderr, "%s: %s: not found\n", command,
					argv[index]);
				status = 1;
			}
		}
		return status;
	}

	/* Each name, with its value when one is given. */
	for (; index < argc; index++) {
		if (declare_one(argv[index], command, local, add, remove) != 0)
			status = 1;
	}

	/* Succeeded: 1 when a name could not be declared. */
	return status;
}

/*
 * Reads the options of declare: attributes to add (-) or take off (+), and
 * what else to do.  Returns 0 after reporting an option it does not take.
 */
static int
declare_options(
	int argc,
	char **argv,
	const char *command,
	int *index,
	int *add,
	int *remove,
	int *mode)
{
	const char *cursor;
	int flag;
	int plus;

	/* Words that begin with - or + and have more, up to --. */
	*add = 0;
	*remove = 0;
	*mode = 0;
	for (*index = 1; *index < argc; (*index)++) {
		cursor = argv[*index];
		if ((cursor[0] != '-' && cursor[0] != '+') || cursor[1] == '\0')
			break;
		if (strcmp(cursor, "--") == 0) {
			(*index)++;
			break;
		}

		/* Each letter. */
		plus = cursor[0] == '+';
		for (cursor++; *cursor != '\0'; cursor++) {
			flag = 0;
			switch (*cursor) {
			case 'i':
				flag = SH_VAR_INTEGER;
				break;
			case 'l':
				flag = SH_VAR_LOWER;
				break;
			case 'u':
				flag = SH_VAR_UPPER;
				break;
			case 'r':
				flag = SH_VAR_READONLY;
				break;
			case 'x':
				flag = SH_VAR_EXPORT;
				break;
			case 'p':
				*mode |= DECLARE_PRINT;
				break;
			case 'f':
				*mode |= DECLARE_FUNCTIONS;
				break;
			case 'F':
				*mode |= DECLARE_FUNCTION_NAMES;
				break;
			case 'g':
				*mode |= DECLARE_GLOBAL;
				break;
			case 'a':
			case 'A':
			case 'n':
				fprintf(stderr, "%s: -%c: not supported by this shell\n",
					command, *cursor);
				return 0;
			default:
				fprintf(stderr, "%s: -%c: invalid option\n", command,
					*cursor);
				return 0;
			}
			if (plus)
				*remove |= flag;
			else
				*add |= flag;
		}
	}

	/* -l and -u exclude each other; the last given wins in bash. */
	if ((*add & SH_VAR_LOWER) != 0 && (*add & SH_VAR_UPPER) != 0)
		*add &= ~SH_VAR_LOWER;

	/* Succeeded. */
	return 1;
}

/*
 * Declares one name: local when asked, the attributes taken off and given,
 * then the value, then read-only last.  Returns 1 after reporting a name
 * that is not a name, or a read-only variable given a value.
 */
static int
declare_one(
	const char *operand,
	const char *command,
	int local,
	int add,
	int remove)
{
	const char *equals;
	char *name;
	int flags;
	int set;

	/* The name. */
	name = operand_name(operand, &equals);
	if (name == NULL) {
		if (strcmp(command, "local") == 0)
			sh_error("local: %s: bad variable name", operand);
		fprintf(stderr, "%s: `%s': not a valid identifier\n", command,
			operand);
		return 1;
	}

	/* A read-only one keeps its value and its attributes. */
	flags = sh_var_flags(name);
	if (flags >= 0 && (flags & SH_VAR_READONLY) != 0 &&
	    (equals != NULL || (remove & SH_VAR_READONLY) != 0)) {
		fprintf(stderr, "%s: %s: readonly variable\n", command, name);
		return 1;
	}

	/* Local to the function, when it is one. */
	if (local)
		(void)sh_var_make_local(name);

	/* The attributes, so that the value is converted as they say. */
	if ((add & SH_VAR_LOWER) != 0)
		remove |= SH_VAR_UPPER;
	if ((add & SH_VAR_UPPER) != 0)
		remove |= SH_VAR_LOWER;
	sh_var_remove_flags(name, remove);
	sh_var_add_flags(name, add & ~SH_VAR_READONLY);

	/* The value. */
	if (equals != NULL) {
		set = sh_var_set(name, equals + 1, 0);
		if (set != 0) {
			fprintf(stderr, "%s: %s: readonly variable\n", command,
				name);
			return 1;
		}
	}

	/* Read-only last, so that the value above could be set. */
	sh_var_add_flags(name, add & SH_VAR_READONLY);

	/* Succeeded. */
	return 0;
}

/*
 * Prints functions for declare -f (their definitions) and -F (their
 * names): the ones named, or all in the order of their names.  Returns 1
 * when a name has no function.
 */
static int
declare_functions(
	int argc,
	char **argv,
	int index,
	int names_only)
{
	struct sh_function *function;
	const char **names;
	size_t count;
	size_t at;
	int status;

	/* The ones named; a name with no function makes the status 1. */
	status = 0;
	if (index < argc) {
		for (; index < argc; index++) {
			function = sh_function_find(argv[index]);
			if (function == NULL)
				status = 1;
			else if (names_only)
				printf("%s\n", argv[index]);
			else
				sh_function_print(argv[index]);
		}
		return status;
	}

	/* All of them, sorted. */
	count = 0;
	for (function = functions; function != NULL; function = function->next)
		count++;
	names = sh_malloc((count + 1U) * sizeof(*names));
	at = 0;
	for (function = functions; function != NULL; function = function->next)
		names[at++] = function->name;
	qsort(names, count, sizeof(*names), compare_names);
	for (at = 0; at < count; at++) {
		if (names_only)
			printf("declare -f %s\n", names[at]);
		else
			sh_function_print(names[at]);
	}
	free(names);

	/* Succeeded. */
	return 0;
}

/*
 * Puts back the positional parameters after dot with operands: the ones
 * saved, unless set replaced them outside a function (then those stay, as
 * bash keeps them).
 */
static void
dot_parameters_restore(
	struct sh_parameters *saved,
	int generation)
{
	/* The file set its own: they stay, and the saved ones go. */
	if (sh_parameters_generation != generation && sh_function_nest == 0) {
		sh_parameters_free(saved);
		return;
	}

	/* Otherwise the ones before it come back. */
	sh_parameters_free(&sh_parameters);
	sh_parameters = *saved;
}

/* Orders names for qsort. */
static int
compare_names(
	const void *left,
	const void *right)
{
	const char *const *first;
	const char *const *second;

	/* By their bytes. */
	first = left;
	second = right;
	return strcmp(*first, *second);
}

/*
 * Searches the directories of a PATH value for an executable regular file.
 */
static int
search_path(
	const char *name,
	const char *path,
	char *result,
	size_t capacity)
{
	int found;

	/* The search, for a file that may be run. */
	found = search_path_for(name, path, result, capacity, 0);

	/* Succeeded: whether one was found. */
	return found;
}

/*
 * Searches the directories of a PATH value for a file: one that may be run,
 * or (readable) one that may be read.  An empty directory is the current
 * one.
 */
static int
search_path_for(
	const char *name,
	const char *path,
	char *result,
	size_t capacity,
	int readable)
{
	const char *start;
	const char *end;
	size_t length;
	int found;

	/* Tries each directory in turn. */
	start = path;
	for (;;) {
		end = strchr(start, ':');
		if (end == NULL)
			end = start + strlen(start);
		length = (size_t)(end - start);
		if (length == 0)
			snprintf(result, capacity, "%s", name);
		else
			snprintf(result, capacity, "%.*s/%s", (int)length, start, name);
		if (readable)
			found = is_readable_file(result);
		else
			found = is_executable(result);
		if (found)
			return 1;
		if (*end == '\0')
			break;
		start = end + 1;
	}

	/* No directory has it. */
	return 0;
}

/* Reports whether a path is a regular file that may be run. */
static int
is_executable(
	const char *path)
{
	struct stat status;
	int found;
	int regular;
	int allowed;

	/* A regular file ... */
	found = stat(path, &status);
	if (found != 0)
		return 0;
	regular = S_ISREG(status.st_mode);
	if (!regular)
		return 0;

	/* ... with execute permission. */
	allowed = access(path, X_OK);
	if (allowed != 0)
		return 0;

	/* Succeeded. */
	return 1;
}

/* Reports whether a path is a regular file that may be read. */
static int
is_readable_file(
	const char *path)
{
	struct stat status;
	int found;
	int regular;
	int allowed;

	/* A regular file ... */
	found = stat(path, &status);
	if (found != 0)
		return 0;
	regular = S_ISREG(status.st_mode);
	if (!regular)
		return 0;

	/* ... with read permission. */
	allowed = access(path, R_OK);
	if (allowed != 0)
		return 0;

	/* Succeeded. */
	return 1;
}

/* Reports whether a name is a reserved word. */
static int
is_reserved_word(
	const char *name)
{
	int index;
	int compare;

	/* Looks through the list. */
	for (index = 0; reserved_words[index] != NULL; index++) {
		compare = strcmp(reserved_words[index], name);
		if (compare == 0)
			return 1;
	}

	/* Not reserved. */
	return 0;
}

/* Tells what running a name finds; returns 0 when it found anything. */
static int
describe_found(
	const char *name,
	const struct sh_command *entry,
	int verbose)
{
	/* A name that finds nothing says so only when verbose, on stdout. */
	if (entry->kind == SH_COMMAND_NOT_FOUND) {
		if (verbose)
			printf("%s: not found\n", name);
		return 127;
	}

	/* command -v prints the name, or a file's path. */
	if (!verbose) {
		if (entry->kind == SH_COMMAND_EXTERNAL)
			printf("%s\n", entry->path);
		else
			printf("%s\n", name);
		return 0;
	}

	/* type and command -V say what kind of thing it is. */
	switch (entry->kind) {
	case SH_COMMAND_SPECIAL:
		printf("%s is a special shell builtin\n", name);
		break;
	case SH_COMMAND_FUNCTION:
		printf("%s is a shell function\n", name);
		break;
	case SH_COMMAND_BUILTIN:
		printf("%s is a shell builtin\n", name);
		break;
	default:
		printf("%s is %s\n", name, entry->path);
		break;
	}

	/* Succeeded: it was found. */
	return 0;
}

/* Hashes a name into a chain of the command table. */
static unsigned int
command_hash(
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
	return hash & (HASH_BUCKETS - 1U);
}

/* Returns the remembered path of a command, or NULL. */
static const char *
hash_lookup(
	const char *name)
{
	struct hash_entry *entry;
	int compare;

	/* Walks the name's chain. */
	for (entry = hash_table[command_hash(name)];
	     entry != NULL;
	     entry = entry->next) {
		compare = strcmp(entry->name, name);
		if (compare == 0)
			return entry->path;
	}

	/* Not remembered. */
	return NULL;
}

/* Remembers a command's path. */
static void
hash_store(
	const char *name,
	const char *path)
{
	struct hash_entry *entry;
	unsigned int bucket;

	/* An entry of the same name is replaced. */
	hash_clear(name);

	/* A new entry at the head of its chain. */
	entry = sh_malloc(sizeof(*entry));
	entry->name = sh_strdup(name);
	entry->path = sh_strdup(path);
	bucket = command_hash(name);
	entry->next = hash_table[bucket];
	hash_table[bucket] = entry;
}

/* Forgets one remembered command, or every one with NULL. */
static void
hash_clear(
	const char *name)
{
	struct hash_entry **link;
	struct hash_entry *entry;
	unsigned int bucket;
	unsigned int only;
	int compare;

	/* The name's chain, or every chain. */
	only = HASH_BUCKETS;
	if (name != NULL)
		only = command_hash(name);

	/* Walks the chains, freeing the entries that go. */
	for (bucket = 0; bucket < HASH_BUCKETS; bucket++) {
		if (only != HASH_BUCKETS && bucket != only)
			continue;
		link = &hash_table[bucket];
		while (*link != NULL) {
			entry = *link;
			if (name != NULL) {
				compare = strcmp(entry->name, name);
				if (compare != 0) {
					link = &entry->next;
					continue;
				}
			}

			/* The entry is taken out of its chain and freed. */
			*link = entry->next;
			free(entry->name);
			free(entry->path);
			free(entry);
		}
	}
}

/* Gives names an attribute, with values where given; -p lists. */
static int
declare_names(
	int argc,
	char **argv,
	int flags,
	const char *command)
{
	const char *equals;
	char *name;
	int index;
	int set;
	int dashes;

	/* Reads -p and --; with no names, lists. */
	index = 1;
	if (index < argc && argv[index][0] == '-' && argv[index][1] == 'p' &&
	    argv[index][2] == '\0')
		index++;
	if (index < argc) {
		dashes = is_double_dash(argv[index]);
		if (dashes)
			index++;
	}

	/* With no operand, the variables with the attribute are listed. */
	if (index >= argc) {
		sh_var_print(flags, command);
		return 0;
	}

	/* Sets each name, then gives it the attribute. */
	for (; index < argc; index++) {
		name = operand_name(argv[index], &equals);
		if (name == NULL)
			sh_error("%s: %s: bad variable name", command, argv[index]);
		if (equals != NULL) {
			set = sh_var_set(name, equals + 1, 0);
			if (set != 0)
				sh_error("%s: is read only", name);
		}

		/* The name gets the attribute. */
		sh_var_add_flags(name, flags);
	}

	/* Succeeded. */
	return 0;
}

/*
 * Reads the name of an operand of export, readonly or local: the operand,
 * or the part before its =.  Sets *equals to the = (or NULL).  Returns NULL
 * when the name is not a name.
 */
static char *
operand_name(
	const char *operand,
	const char **equals)
{
	char *name;
	int valid;

	/* The part before =, or all of it. */
	*equals = strchr(operand, '=');
	if (*equals == NULL)
		name = sh_temp_own(sh_strdup(operand));
	else
		name = sh_temp_own(sh_strndup(operand, (size_t)(*equals - operand)));

	/* It must be a name. */
	valid = sh_var_name(name);
	if (!valid)
		return NULL;

	/* Succeeded: the name, freed with the command. */
	return name;
}

/* Reads a decimal count for exit, return, shift, break and continue. */
static int
parse_count(
	const char *text,
	int *count)
{
	char *end;
	long value;

	/* A decimal number, without a sign. */
	if (*text < '0' || *text > '9')
		return 0;
	value = strtol(text, &end, 10);
	if (*end != '\0' || value > INT_MAX)
		return 0;
	*count = (int)value;

	/* Succeeded. */
	return 1;
}

/* Drops a -- that comes first among a builtin's operands. */
static void
skip_double_dash(
	int *argc,
	char ***argv)
{
	int dashes;

	/* The first operand, when it is --. */
	if (*argc < 2)
		return;
	dashes = is_double_dash((*argv)[1]);
	if (!dashes)
		return;

	/* Dropped. */
	(*argc)--;
	(*argv)++;
}

/* Reports whether a word is "--". */
static int
is_double_dash(
	const char *word)
{
	/* Two dashes and nothing else. */
	if (word[0] == '-' && word[1] == '-' && word[2] == '\0')
		return 1;

	/* Anything else. */
	return 0;
}
