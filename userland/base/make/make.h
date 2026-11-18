/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shared types and functions of make.
 *
 * make reads the makefiles into a table of variables and a graph of
 * targets (read.c, variable.c, rule.c), then brings each goal up to date
 * (update.c), running the recipes through the shell (job.c).  Text is
 * expanded by expand.c.  util.c holds the allocation, the messages and the
 * growable buffer that everything uses.
 */

#ifndef USERLAND_BASE_MAKE_MAKE_H
#define USERLAND_BASE_MAKE_MAKE_H

#include <stddef.h>
#include <stdio.h>

/* The number of hash chains in a variable table or the target table. */
#define MAKE_HASH_SIZE 1024

/*
 * A text that grows as it is appended to.  text is always terminated
 * after its length bytes once anything was added; an empty buffer has a
 * NULL text.
 */
struct buffer {
	char *text;
	size_t length;
	size_t capacity;
};

/*
 * How a variable's value is used: expanded each time it is read
 * (recursive, defined with =), or expanded once when it was assigned
 * (simple, defined with := or ::=).
 */
enum variable_flavor {
	FLAVOR_RECURSIVE,
	FLAVOR_SIMPLE
};

/*
 * Where a variable's value came from, lowest precedence first.  A
 * definition from a lower origin does not replace one from a higher; -e
 * lifts the environment above the makefiles.
 */
enum variable_origin {
	ORIGIN_DEFAULT,
	ORIGIN_ENVIRONMENT,
	ORIGIN_FILE,
	ORIGIN_ENVIRONMENT_OVERRIDE,
	ORIGIN_COMMAND_LINE,
	ORIGIN_OVERRIDE,
	ORIGIN_AUTOMATIC
};

/* The operator of an assignment: =, := (or ::=), +=, ?=, !=. */
enum assign_kind {
	ASSIGN_RECURSIVE,
	ASSIGN_SIMPLE,
	ASSIGN_APPEND,
	ASSIGN_CONDITIONAL,
	ASSIGN_SHELL
};

/*
 * One variable of a table.  The table owns the name and the value; the
 * value of a recursive variable is its unexpanded text.
 */
struct variable {
	struct variable *next;		/* the next in its hash chain */
	char *name;
	char *value;
	enum variable_flavor flavor;
	enum variable_origin origin;
	int export_state;		/* 1 export, -1 unexport, 0 as the origin says */
	int expanding;			/* set while its value is being expanded, to catch a loop */
	int append;			/* a target's += that adds to the global value */
};

/* A table of variables: the global one, or the one of a target. */
struct variable_set {
	struct variable *chains[MAKE_HASH_SIZE];
};

/*
 * The tables a name is looked up in, innermost first: a target's own
 * variables, then those of the target that needed it, then the global
 * table.  A scope lives on the stack of the code that built it.
 */
struct variable_scope {
	struct variable_set *set;
	const struct variable_scope *parent;
};

/*
 * The automatic variables of the recipe being expanded ($@ $< $? $^ $+
 * $* $| and their D and F forms).  NULL fields read as empty.
 */
struct automatic {
	const char *target;
	const char *first;
	const char *newer;
	const char *unique;
	const char *all;
	const char *stem;
	const char *order_only;
};

/*
 * What an expansion reads: the variables in scope, the automatic
 * variables (NULL outside a recipe), and the place of the text in a
 * makefile for messages (file NULL when there is none).
 */
struct expansion {
	const struct variable_scope *scope;
	const struct automatic *automatic;
	const char *file;
	long line;
};

/*
 * The recipe of a rule: its lines, unexpanded, and where it was defined.
 * A line may hold backslash-newlines, which go to the shell as they are.
 */
struct recipe {
	char **lines;
	long *line_numbers;		/* the makefile line of each line */
	size_t count;
	size_t capacity;
	char *file;
	long line;
	int semicolon;			/* given after ; on the rule line, so present even if empty */
};

/*
 * A prerequisite of a target, in the order it was written.  An
 * order-only one is made first but never makes the target out of date.
 */
struct dependency {
	struct dependency *next;
	struct target *target;
	int order_only;
	int wait;			/* .WAIT stood before it: the ones before it are made first */
	int dropped;			/* it closed a loop and is no longer followed */
};

/*
 * The state of a target in one run of the update.  A pending target has
 * been looked at and waits for its prerequisites or for a free job; a
 * running one has its recipe running.
 */
enum target_state {
	TARGET_UNVISITED,
	TARGET_PENDING,
	TARGET_RUNNING,
	TARGET_DONE
};

/*
 * A file or a name that make may bring up to date.  Targets are made
 * once and live until make ends.  A target with double-colon rules keeps
 * each rule as a target of its own on double_colon, with the same name.
 */
struct target {
	struct target *next;		/* the next in its hash chain */
	char *name;
	struct dependency *dependencies;
	struct dependency **dependencies_tail;
	struct recipe *recipe;
	struct target *double_colon;	/* the rules of a double-colon target */
	int is_double_colon;
	int is_target;			/* named as a target of some rule */
	int builtin;			/* its recipe came from the built-in rules */
	int phony;
	int precious;
	int ignore_errors;		/* .IGNORE named it */
	int silent;			/* .SILENT named it */
	char *stem;			/* $* when an implicit rule made it */
	char *path;			/* where VPATH found its file, or NULL for its name */
	struct target *implicit_first;	/* the prerequisite an implicit rule gave it ($<) */
	struct variable_set *variables;	/* its target-specific variables, or NULL */
	const struct variable_scope *scope;	/* the variables its recipe sees, from its first visit */
	struct variable_scope own_scope;	/* its own table in front of its parent's, when it has one */
	int in_walk;			/* on the path of the walk now: reaching it again is a loop */
	int not_parallel;		/* .NOTPARALLEL named it: its prerequisites are made one at a time */
	enum target_state state;
	int failed;			/* it could not be made */
	int remade;			/* its recipe ran (or would have, with -n) */
	int exists;			/* the file existed when last looked at */
	long long seconds;		/* its modification time */
	long nanoseconds;
};

/*
 * A pattern rule (%.o: %.c), or a suffix rule converted to one.  A rule
 * with no recipe and terminal set only cancels a built-in rule.
 */
struct pattern_rule {
	struct pattern_rule *next;
	char *target_pattern;
	char **prerequisites;
	size_t prerequisite_count;
	struct target *holder;		/* receives the rule's recipe while it is read */
	int terminal;			/* written with :: */
};

/*
 * The options of one run.  main() fills them from the command line and
 * MAKEFLAGS; everything else reads them.
 */
struct make_options {
	int keep_going;			/* -k */
	int dry_run;			/* -n */
	int silent;			/* -s */
	int ignore_errors;		/* -i */
	int question;			/* -q */
	int touch;			/* -t */
	int environment_overrides;	/* -e */
	int print_directory;		/* -w (1), --no-print-directory (0), unset (-1) */
	int no_builtin_rules;		/* -r */
	int no_builtin_variables;	/* -R */
	int jobs;			/* -j: the most recipes at once, 0 for no limit */
};

/*
 * What an update reports: done (0), failed (1), or waiting for recipes
 * that are still running.
 */
#define UPDATE_PENDING 2

/*
 * The automatic variables of a recipe with the lists they point into; a
 * running recipe owns them.
 */
struct recipe_variables {
	struct automatic automatic;
	struct buffer newer;
	struct buffer unique;
	struct buffer all;
	struct buffer order_only;
};

/* The options of this run (main.c). */
extern struct make_options make_options;

/* The level of this make among recursive ones: 0 at the top (main.c). */
extern int make_level;

/* The global variables (variable.c). */
extern struct variable_set make_global_variables;

/* The global scope, which holds only the global variables (variable.c). */
extern const struct variable_scope make_global_scope;

/* Set when -q finds a target that is not up to date (update.c). */
extern int make_question_failed;

/* The special targets that name no targets and so cover all (rule.c). */
extern int make_ignore_all;
extern int make_silent_all;
extern int make_posix;
extern int make_not_parallel;

/* util.c */
void *make_malloc(size_t size);
void *make_realloc(void *memory, size_t size);
char *make_strdup(const char *text);
char *make_strndup(const char *text, size_t length);
const char *make_program(void);
void make_message(const char *format, ...);
void make_fatal(const char *format, ...);
void make_file_fatal(const char *file, long line, const char *format, ...);
void buffer_add(struct buffer *buffer, const char *text, size_t length);
void buffer_add_string(struct buffer *buffer, const char *text);
void buffer_add_char(struct buffer *buffer, char character);
char *buffer_finish(struct buffer *buffer);
const char *buffer_text(const struct buffer *buffer);
int make_is_blank(char character);
const char *make_next_word(const char **cursor, size_t *length);

/* variable.c */
struct variable *variable_lookup(const struct variable_scope *scope, const char *name, size_t length);
struct variable *variable_lookup_outer(const struct variable_scope *scope, const struct variable *inner);
void variable_free_set(struct variable_set *set);
struct variable *variable_set_value(struct variable_set *set, const char *name, const char *value, enum variable_flavor flavor, enum variable_origin origin);
void variable_assign(struct variable_set *set, const char *name, const char *value, enum assign_kind kind, enum variable_origin origin, const struct expansion *context);
void variable_import_environment(char **environment);
char **variable_environment(const struct variable_scope *scope, const struct automatic *automatic);
void variable_free_environment(char **environment);
struct variable_set *variable_new_set(void);
void variable_export_all(void);

/* expand.c */
char *expand(const struct expansion *context, const char *text);
void expand_into(const struct expansion *context, const char *text, size_t length, struct buffer *out);

/* function.c */
int function_call(const struct expansion *context, const char *text, size_t length, struct buffer *out);
void function_patsubst_words(struct buffer *out, const char *value, const char *pattern, const char *replacement);

/* read.c */
int read_makefile(const char *path, int must_exist);
void read_builtin(const char *text);
void read_eval(const char *text, const char *file, long line);
void read_remake_list(char ***names, int **must_exist, size_t *count);
size_t read_makefile_count(void);
void read_add_include_directory(const char *directory);

/* rule.c */
struct target *target_lookup(const char *name);
struct target *target_enter(const char *name);
struct dependency *target_add_dependency(struct target *target, struct target *prerequisite, int order_only);
struct target **rule_define(char **targets, size_t target_count, char **prerequisites, size_t prerequisite_count, char **order_only, size_t order_only_count, int double_colon);
void rule_set_recipe(struct target **targets, size_t count, struct recipe *recipe);
int target_has_recipe(const struct target *target);
struct recipe *recipe_new(const char *file, long line);
void recipe_add_line(struct recipe *recipe, const char *text, size_t length, long line);
void rule_set_reading_builtin(int builtin);
void rule_finish_reading(void);
const char *rule_default_goal(void);
int rule_find_implicit(struct target *target);
struct recipe *rule_default_recipe(void);
void rule_define_variable(char **targets, size_t target_count, const char *name, const char *value, enum assign_kind kind, enum variable_origin origin, const struct expansion *context);
void rule_vpath_set(const char *pattern, char **directories, size_t count);
char *rule_vpath_search(const char *name);

/* update.c */
int update_goal(struct target *goal, int report);
void update_time_of(struct target *target);
void update_recipe_done(struct target *target, int failed);

/* job.c */
int job_start(struct target *target, const struct recipe *recipe, struct recipe_variables *variables, const struct variable_scope *scope, int ignore_errors, int silent);
int job_slot_free(void);
void job_want_slot(void);
void job_wait(void);
size_t job_running(void);
void job_server_start(const char *inherited, int jobs_on_command_line);
const char *job_server_flags(void);
void job_server_finish(void);
char *job_shell_output(const char *command, int *status);
void job_install_signals(void);

#endif
