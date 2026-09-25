/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shell's internal interfaces: the tree a command is parsed into, the
 * state every part of the shell shares, and the operations the parser, the
 * evaluator, the builtins and the job table offer one another.
 */

#ifndef KERN_USERLAND_SH_SHELL_H
#define KERN_USERLAND_SH_SHELL_H

#include "userland/base/sh/lexer.h"

#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stddef.h>
#include <sys/types.h>

/* The kinds of exception sh_raise throws (0 is what setjmp returns first). */
#define SH_EX_ERROR	1	/* an error that ends a shell that is not interactive */
#define SH_EX_EXIT	2	/* the exit builtin, or the end of the shell */
#define SH_EX_INT	3	/* an interrupt from the terminal */

/* The flags sh_eval takes. */
#define SH_EV_EXIT	0x01	/* the shell ends after the command */
#define SH_EV_TESTED	0x02	/* the status is tested, so errexit sleeps */

/* What a break, a continue or a return asks the constructs around it for. */
#define SH_SKIP_NONE	0
#define SH_SKIP_BREAK	1
#define SH_SKIP_CONTINUE 2
#define SH_SKIP_RETURN	3

/* The kinds of redirection (struct sh_redirection op). */
#define SH_REDIR_INPUT		1	/* < */
#define SH_REDIR_OUTPUT		2	/* > */
#define SH_REDIR_CLOBBER	3	/* >| */
#define SH_REDIR_APPEND		4	/* >> */
#define SH_REDIR_READ_WRITE	5	/* <> */
#define SH_REDIR_DUP_INPUT	6	/* <& */
#define SH_REDIR_DUP_OUTPUT	7	/* >& */
#define SH_REDIR_HEREDOC	8	/* << and <<- */
#define SH_REDIR_HERESTRING	9	/* <<<, a bash extension */

/* What sh_fork starts (its mode). */
#define SH_FORK_NO_JOB		0	/* a command substitution, waited for directly */
#define SH_FORK_FOREGROUND	1	/* a process of a job run in the foreground */
#define SH_FORK_BACKGROUND	2	/* a process of a job run in the background */

/* How sh_redirect applies a list (its flags). */
#define SH_REDIRECT_SAVE	0x01	/* keep what each descriptor held */

/* What a command name is found to be (struct sh_command kind). */
#define SH_COMMAND_NOT_FOUND	0
#define SH_COMMAND_SPECIAL	1
#define SH_COMMAND_FUNCTION	2
#define SH_COMMAND_BUILTIN	3
#define SH_COMMAND_EXTERNAL	4

/* How sh_find_command looks a name up (its flags). */
#define SH_FIND_NO_FUNCTIONS	0x01	/* command: functions are skipped */
#define SH_FIND_DEFAULT_PATH	0x02	/* command -p: the default PATH */
#define SH_FIND_NO_REMEMBER	0x04	/* a lookup for a subshell: the path is not remembered */

/* The builtin flags (struct sh_builtin flags). */
#define SH_BUILTIN_SPECIAL	0x01	/* a special builtin (XCU 2.14) */
#define SH_BUILTIN_DECLARATION	0x02	/* its operands are assignments */

/* The options, indexes of sh_option, in the order set -o lists them. */
enum sh_option_index {
	SH_OPT_ERREXIT,
	SH_OPT_NOGLOB,
	SH_OPT_IGNOREEOF,
	SH_OPT_INTERACTIVE,
	SH_OPT_MONITOR,
	SH_OPT_NOEXEC,
	SH_OPT_STDIN,
	SH_OPT_XTRACE,
	SH_OPT_VERBOSE,
	SH_OPT_VI,
	SH_OPT_EMACS,
	SH_OPT_NOCLOBBER,
	SH_OPT_ALLEXPORT,
	SH_OPT_NOTIFY,
	SH_OPT_NOUNSET,
	SH_OPT_PRIVILEGED,
	SH_OPT_NOLOG,
	SH_OPT_PIPEFAIL,
	SH_OPT_DEBUG,
	SH_OPT_HASHALL,
	SH_OPT_COUNT
};

/* The kinds of node of a parsed command. */
enum sh_node_kind {
	SH_NODE_SIMPLE,
	SH_NODE_PIPELINE,
	SH_NODE_AND,
	SH_NODE_OR,
	SH_NODE_SEQUENCE,
	SH_NODE_BACKGROUND,
	SH_NODE_NOT,
	SH_NODE_IF,
	SH_NODE_WHILE,
	SH_NODE_UNTIL,
	SH_NODE_FOR,
	SH_NODE_CASE,
	SH_NODE_GROUP,
	SH_NODE_SUBSHELL,
	SH_NODE_FUNCTION,
	SH_NODE_COND,		/* [[ ... ]], a bash extension */
	SH_NODE_ARITH,		/* (( ... )), a bash extension */
	SH_NODE_ARITH_FOR	/* for (( ...; ...; ... )), a bash extension */
};

/* What a part of a [[ ... ]] expression is. */
#define SH_COND_WORD	0	/* a word: true when it is not empty */
#define SH_COND_UNARY	1	/* -f word and the like */
#define SH_COND_BINARY	2	/* word == word and the like */
#define SH_COND_NOT	3	/* ! expression */
#define SH_COND_AND	4	/* expression && expression */
#define SH_COND_OR	5	/* expression || expression */

/* A part of a [[ ... ]] expression, as the parser read it. */
struct sh_cond {
	int kind;
	char op[4];			/* the operator: -f, ==, =~, <, -eq ... */
	struct sh_token *left;		/* the word, or the left operand */
	struct sh_token *right;		/* the right operand of a binary one */
	struct sh_cond *first;		/* the operand of !, or the left of && and || */
	struct sh_cond *second;		/* the right of && and || */
};


/*
 * A block of memory that one parse allocates its tree and its words from.
 *
 * Everything a command was parsed into lives and dies together: the tree is
 * run and then dropped, unless a function was defined in it, in which case the
 * function holds a reference and the block stays until the function is
 * replaced or unset.
 */
struct sh_arena;

/*
 * One redirection as written: the descriptor, what is done to it, and the
 * word that names the file or the other descriptor.  A here-document's word
 * carries its body, with heredoc saying whether it is expanded.
 */
struct sh_redirection {
	struct sh_redirection *next;
	int op;
	int descriptor;
	struct sh_token *word;
};

/* One arm of a case command: its patterns and the list they select. */
struct sh_case_item {
	struct sh_case_item *next;
	struct sh_token **patterns;
	size_t pattern_count;
	struct sh_node *body;
};

/*
 * One node of a parsed command.
 *
 * The words stay as they were written until the command runs: a loop's body is
 * parsed once and run many times, and each pass must see the values of that
 * pass.  Redirections written after a compound command belong to the node, and
 * apply to all of it.
 */
struct sh_node {
	enum sh_node_kind kind;
	int line;
	struct sh_redirection *redirections;
	union {
		/* A simple command: its assignments and its words. */
		struct {
			struct sh_token **assignments;
			size_t assignment_count;
			struct sh_token **words;
			size_t word_count;
		} simple;

		/* A pipeline of two commands or more. */
		struct {
			struct sh_node **commands;
			size_t count;
		} pipeline;

		/* &&, || and ; (the right may be NULL after ; or &). */
		struct {
			struct sh_node *left;
			struct sh_node *right;
		} binary;

		/* &, !, { ... } and ( ... ). */
		struct sh_node *body;

		/* if: the condition, what it selects, and the else (or NULL). */
		struct {
			struct sh_node *condition;
			struct sh_node *then_part;
			struct sh_node *else_part;
		} branch;

		/* while and until. */
		struct {
			struct sh_node *condition;
			struct sh_node *body;
		} loop;

		/* for: without "in", the positional parameters are walked. */
		struct {
			struct sh_token *name;
			struct sh_token **words;
			size_t word_count;
			int has_in;
			struct sh_node *body;
		} iterate;

		/* case. */
		struct {
			struct sh_token *word;
			struct sh_case_item *items;
		} select;

		/* A function definition. */
		struct {
			const char *name;
			struct sh_node *body;
		} function;

		/* [[ ... ]]. */
		struct sh_cond *cond;

		/* (( ... )): the expression as written. */
		const char *arith;

		/* for (( init; test; step )) body. */
		struct {
			const char *init;
			const char *test;
			const char *step;
			struct sh_node *body;
		} arith_for;
	} u;
};

/*
 * One place an exception may be caught.
 *
 * What the shell had pushed when the handler was set up is recorded with it:
 * the handler that catches an exception puts the redirections, the input
 * sources, the local variables and the temporary memory back to those depths,
 * whatever was between the throw and the catch.
 */
struct sh_handler {
	jmp_buf environment;
	struct sh_handler *previous;
	int redirect_depth;
	int input_depth;
	int capture_depth;
	int local_depth;
	size_t temp_mark;
	int loop_nest;
	int function_nest;
	int dot_nest;
};

/* A builtin utility: its name, how it runs, and whether it is special. */
struct sh_builtin {
	const char *name;
	int (*run)(int, char **);
	int flags;
};

/* The positional parameters: $1 on, and whether their block is the shell's. */
struct sh_parameters {
	int count;
	char **values;
	int owned;
};

/*
 * What a command name was found to be: a builtin, a function, or the path of
 * a file (or nothing).  Filled by sh_find_command for one command.
 */
struct sh_command {
	int kind;
	const struct sh_builtin *builtin;
	struct sh_function *function;
	char path[4096];
};

/* A defined function, with the block its body was parsed into. */
struct sh_function {
	struct sh_function *next;
	char *name;
	struct sh_node *body;
	struct sh_arena *arena;
};

/* Shell state shared by every part of the shell. */
extern struct sh_handler *sh_handler;
extern int sh_option[SH_OPT_COUNT];
extern int sh_status;
extern int sh_error_quiet;
extern long sh_root_pid;
extern long sh_last_background;
extern int sh_subshell;
extern const char *sh_arg0;
extern struct sh_parameters sh_parameters;
extern int sh_skip;
extern int sh_skip_count;
extern int sh_loop_nest;
extern int sh_function_nest;
extern int sh_dot_nest;
extern int sh_last_substitution_status;
extern int sh_command_line;
extern int sh_exception_status;
extern volatile int sh_trap_pending_any;
extern int sh_in_trap;

/* Memory. */
void *sh_malloc(size_t);
void *sh_realloc(void *, size_t);
char *sh_strdup(const char *);
char *sh_strndup(const char *, size_t);
size_t sh_temp_mark(void);
void *sh_temp_own(void *);
void sh_temp_release(size_t);
void *sh_temp_grow(void *, size_t, size_t);
int sh_descriptor_high(int);
struct sh_arena *sh_arena_new(void);
void *sh_arena_alloc(struct sh_arena *, size_t);
char *sh_arena_strndup(struct sh_arena *, const char *, size_t);
void sh_arena_hold(struct sh_arena *);
void sh_arena_release(struct sh_arena *);

/* Errors and exceptions. */
void sh_handler_push(struct sh_handler *);
void sh_handler_pop(struct sh_handler *);
void sh_handler_unwind(struct sh_handler *);
void sh_raise(int) __attribute__((noreturn));
void sh_raise_error(int) __attribute__((noreturn));
void sh_error(const char *, ...) __attribute__((noreturn, format(printf, 1, 2)));
void sh_warn(const char *, ...) __attribute__((format(printf, 1, 2)));
void sh_exit(int) __attribute__((noreturn));

/* Input. */
void sh_input_push_string(const char *, size_t, int);
void sh_input_push_file(int, int);
void sh_input_push_alias(const char *, void *);
void sh_input_push_back_text(const char *, size_t);
void sh_input_give_back_text(const char *, size_t);
void sh_input_pop(void);
int sh_input_depth(void);
void sh_input_unwind(int);
int sh_input_getc(void);
void sh_input_ungetc(int);
int sh_input_alias_active(const void *);
int sh_input_line(void);
void sh_input_set_prompt(int);
void sh_input_capture_start(void);
char *sh_input_capture_stop(size_t *);
void sh_input_capture_drop(void);
int sh_input_capture_depth(void);
int sh_input_capture_suspend(void);
void sh_input_capture_resume(int);
void sh_input_capture_unwind(int);
int sh_input_interactive(void);
void sh_input_discard_line(void);
const char *sh_prompt_text(int);
extern int sh_input_alias_blank;

/* What sh_input_getc returns at the end of an alias's text. */
#define SH_INPUT_END_OF_ALIAS	(-2)

/* The parser. */
struct sh_node *sh_parse_command(struct sh_arena *, int *);

/* Evaluation (exec.c). */
int sh_eval(struct sh_node *, int);
int sh_eval_string(const char *, int);
int sh_eval_input(int);
int sh_run_command_substitution(const char *, char **);
void sh_expand_context_fill(void *);

/* cond.c: [[ ... ]] and (( ... )), bash extensions. */
int sh_eval_cond(const struct sh_cond *);
int sh_eval_arith_text(const char *, long *);

/* test.c: the tests [[ ... ]] shares with test. */
int sh_test_unary(const char *, const char *);
int sh_test_file_compare(const char *, const char *, const char *);

/* options.c: whether a named option is on ([[ -o name ]]). */
int sh_option_named(const char *);
void sh_parameters_set(int, char **);
void sh_parameters_free(struct sh_parameters *);
void sh_xtrace(int, char **, size_t);
void sh_exec_external(const char *, char **, char **, size_t) __attribute__((noreturn));
int sh_token_is_assignment(const struct sh_token *);
char *sh_node_text(struct sh_node *);

/* Functions and command search (command.c). */
struct sh_function *sh_function_find(const char *);
int sh_function_unset(const char *);
void sh_function_define(const char *, struct sh_node *, struct sh_arena *);
void sh_function_print(const char *);
void sh_find_command(const char *, int, const char *, struct sh_command *);
int sh_find_command_path(const char *, char *, size_t, int);
int sh_search_readable(const char *, const char *, char *, size_t);
int sh_command_describe(const char *, int);
void sh_path_changed(const char *);
int sh_builtin_dot(int, char **);
int sh_builtin_eval(int, char **);
int sh_builtin_exec(int, char **);
int sh_builtin_exit(int, char **);
int sh_builtin_return(int, char **);
int sh_builtin_break(int, char **);
int sh_builtin_shift(int, char **);
int sh_builtin_command(int, char **);
int sh_builtin_type(int, char **);
int sh_builtin_hash(int, char **);
int sh_builtin_unset(int, char **);
int sh_builtin_export(int, char **);
int sh_builtin_local(int, char **);

/* Options (options.c). */
const char *sh_option_letters(void);
int sh_options_parse(int, char **, int *, int);
int sh_set_builtin(int, char **);
int sh_getopts_builtin(int, char **);
void sh_getopts_reset(const char *);

/* Input and the shell's startup (input.c, main.c). */
int sh_input_at_end(int);
int sh_run_noninteractive(void);

/* The directory (cd.c). */
int sh_builtin_cd(int, char **);
int sh_builtin_pwd(int, char **);
int sh_pwd_valid(const char *);
void sh_cwd_init(void);

/* The builtins with files of their own (printf.c, test.c, read.c, limits.c). */
int sh_builtin_echo(int, char **);
int sh_builtin_printf(int, char **);
int sh_builtin_test(int, char **);
int sh_builtin_read(int, char **);
int sh_builtin_umask(int, char **);
int sh_builtin_ulimit(int, char **);

/* The job table (jobs.c). */
void sh_job_poll(void);
void sh_job_set_text(void *, struct sh_node *);

/* Quoting for output that the shell reads back (util.c). */
void sh_print_quoted_always(const char *);

/* Redirections. */
int sh_redirect(struct sh_redirection *, int);
void sh_redirect_pop(void);
int sh_redirect_depth(void);
void sh_redirect_unwind(int);
void sh_redirect_forget(void);
void sh_redirect_stdin_null(struct sh_node *);
int sh_redirect_saved_descriptor(int);

/* Traps and signals. */
void sh_trap_init(void);
void sh_trap_run_pending(void);
void sh_trap_reset_subshell(void);
int sh_trap_builtin(int, char **);
int sh_trap_signal_number(const char *);
const char *sh_trap_signal_name(int);
void sh_trap_run_exit(void);
void sh_signals_for_interactive(int);
void sh_signals_for_child(int, int);
int sh_trap_ignored(int);
int sh_trap_any_set(void);
int sh_trap_set(int, const char *);
void sh_trap_list_signals(void);
extern volatile int sh_trap_last_signal;
extern volatile int sh_interrupted;
void sh_signals_for_exec(void);
void sh_signals_shell_set(sigset_t *);

/* Processes and jobs. */
pid_t sh_fork(int, void *);
pid_t sh_spawn(const char *, char **, void *, const posix_spawn_file_actions_t *);
void sh_fork_child(int, void *);
int sh_wait_process(pid_t, int);
int sh_job_wait_foreground(void *);
void *sh_job_new(int);
void sh_job_add_process(void *, pid_t);
void sh_job_background(void *);
void sh_job_notify(void);
int sh_status_of(int);
void sh_job_control_init(void);
int sh_job_control_active(void);
int sh_jobs_builtin(int, char **);
int sh_fg_builtin(int, char **);
int sh_bg_builtin(int, char **);
int sh_wait_builtin(int, char **);
int sh_kill_builtin(int, char **);
long sh_job_pid_of(const char *, int *);
void sh_job_release_all(void);
void sh_job_forget_all(void);
int sh_job_has_stopped(void);

/* History. */
void sh_history_add(const char *);
int sh_fc_builtin(int, char **);

/* Builtins. */
const struct sh_builtin *sh_builtin_find(const char *);
int sh_builtin_run(const struct sh_builtin *, int, char **);
extern int sh_builtin_exit_requested;

#endif
