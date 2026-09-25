/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The evaluator: runs the tree of a parsed command (POSIX XCU 2.9).
 *
 * A simple command is expanded when it runs: its words, then its
 * redirections, then its assignments.  The name is looked up (command.c) as
 * a special builtin, a function, a builtin, and a file on PATH, in that
 * order.  The status of every command is an integer, kept in sh_status.
 *
 * break, continue and return set sh_skip, which every list and loop between
 * the command and the construct it names stops at.  An error is thrown to the
 * innermost handler; the shell's own state is put back by the handler.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/expand.h"
#include "userland/base/sh/glob.h"
#include "userland/base/sh/vars.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * A growing array of the words a command expanded to.  Every array it grows
 * through is a temporary allocation, freed with the command.
 */
/*
 * A process substitution whose pipe the shell holds for the command it
 * was written in: the shell's end, and the child running its commands.
 */
struct process_substitution {
	int descriptor;
	pid_t child;
};

/* The most bytes echo or printf may write into a pipe from the shell itself. */
#define SH_INLINE_OUTPUT_MAX	4096U

struct word_list {
	char **words;
	size_t count;
	size_t capacity;
};

/* The process substitutions open now, newest last, and children not yet reaped. */
static struct process_substitution *process_substitutions;
static size_t process_substitution_count;
static size_t process_substitution_capacity;
static pid_t *process_children;
static size_t process_child_count;

/* A growing line of trace output, written in one piece. */
struct trace_buffer {
	char *text;
	size_t length;
	size_t capacity;
};

/* The status of the last command; $? expands to it. */
int sh_status;

/*
 * What a break, a continue or a return is carrying out (SH_SKIP_*), and
 * through how many loops.  Set by the builtins; cleared by the loop or the
 * function that answers it.
 */
int sh_skip;
int sh_skip_count;

/* How many loops, function calls and dot scripts are running. */
int sh_loop_nest;
int sh_function_nest;
int sh_dot_nest;

/*
 * The status of the last command substitution of the command being run, or
 * -1 when it ran none: a command with no name takes it as its own.
 */
int sh_last_substitution_status;

/* The line of the command being run; LINENO and messages say it. */
int sh_command_line;

/* The status an exception carries out to its handler. */
int sh_exception_status;

/* The positional parameters, $1 on. */
struct sh_parameters sh_parameters;

/*
 * The block the command being run was parsed into, where a function it
 * defines finds its body.  A function's call makes its own block current.
 */
static struct sh_arena *current_arena;

static int eval_simple(struct sh_node *node, int flags);
static size_t command_prefix(struct word_list *arguments, int *find_flags);
static int run_external(struct sh_node *node, struct sh_command *entry, struct word_list *arguments, char **assignments, size_t assignment_count, int flags);
static int run_in_shell(struct sh_command *entry, struct word_list *arguments, char **assignments, size_t assignment_count, int flags, int special);
static void trace_command(char **assignments, size_t assignment_count, struct word_list *arguments);
static int eval_pipeline(struct sh_node *node, int background, int flags);
static void *pipeline_start(struct sh_node *node, int background, int flags, int output, int output_other);
static pid_t pipeline_spawn(struct sh_node *node, int input, int output, int other, int output_other, void *job);
static int word_is_pure(const struct sh_token *word);
static int expand_process_substitution(void *context, const struct sh_token *token, char **result);
static void process_substitutions_close(size_t mark);
static int substitution_in_shell(const char *text, char **output, size_t *length, int *status, int depth);
static int substitution_simple(struct sh_node *node, char **output, size_t *length, int *status, int depth);
static int pipeline_inline(struct sh_node *node, int output);
static int output_builtin(struct word_list *arguments, int output, int *status);
static size_t output_bound(struct word_list *arguments);
static struct sh_node *parse_quietly(const char *text, struct sh_arena *arena);
static int subshell_unset_trial(struct sh_node *node, int *status);
static int unset_trial(struct word_list *arguments, struct sh_node *subshell, int *status, int depth);
static void pipeline_child(struct sh_node **commands, size_t index, int previous, int *descriptors, int output, int output_other, int background, int flags) __attribute__((noreturn));
static int eval_background(struct sh_node *node);
static int eval_subshell(struct sh_node *node, int flags);
static int eval_redirected(struct sh_node *node, int flags, int *failed);
static int eval_body(struct sh_node *node, int flags);
static int eval_if(struct sh_node *node, int flags);
static int eval_loop(struct sh_node *node, int flags);
static int eval_for(struct sh_node *node, int flags);
static int eval_case(struct sh_node *node, int flags);
static int case_matches(struct sh_case_item *item, const char *word);
static int loop_should_stop(void);
static int eval_arith(const char *text);
static int eval_arith_for(struct sh_node *node, int flags);
static int arith_blank(const char *text);
static void expand_words(struct sh_token **words, size_t count, struct word_list *list, int detect);
static void expand_one(struct sh_token *word, struct word_list *list, int as_assignment);
static int declaration_named(struct word_list *list, int *detect);
static char *expand_assignment(struct sh_token *word);
static void expansion_error(const char *text) __attribute__((noreturn));
static void list_add(struct word_list *list, char *word);
static int call_function(struct sh_function *function, int argc, char **argv, int flags);
static void assign_permanently(char **assignments, size_t count, int flags);
static void assign_temporarily(char **assignments, size_t count);
static char *read_output(int descriptor, size_t *length);
static void trace_append(struct trace_buffer *buffer, const char *text, size_t length);
static void xtrace_word(struct trace_buffer *buffer, const char *word);
static int is_plain_word(const char *word);
static const char *expand_lookup(void *context, const char *name);
static int expand_assign(void *context, const char *name, const char *value);
static int expand_substitute(void *context, const char *text, char **result);

/*
 * Runs a command tree, and returns its status.
 */
int
sh_eval(
	struct sh_node *node,
	int flags)
{
	size_t substitution_mark;
	int status;
	int check_exit;

	/* Runs the traps of signals that arrived since the last command. */
	if (sh_trap_pending_any)
		sh_trap_run_pending();

	/* Nothing to run succeeds; -n reads commands without running them. */
	if (node == NULL || sh_option[SH_OPT_NOEXEC]) {
		if ((flags & SH_EV_EXIT) != 0)
			sh_exit(sh_status);
		return 0;
	}

	/* Dispatches on the kind of command. */
	status = 0;
	check_exit = 0;
	substitution_mark = process_substitution_count;
	switch (node->kind) {
	case SH_NODE_SEQUENCE:
		status = sh_eval(node->u.binary.left, flags & SH_EV_TESTED);
		if (sh_skip != SH_SKIP_NONE || node->u.binary.right == NULL)
			break;
		status = sh_eval(node->u.binary.right, flags);
		break;
	case SH_NODE_AND:
		status = sh_eval(node->u.binary.left, SH_EV_TESTED);
		if (sh_skip != SH_SKIP_NONE || status != 0)
			break;
		status = sh_eval(node->u.binary.right, flags);
		break;
	case SH_NODE_OR:
		status = sh_eval(node->u.binary.left, SH_EV_TESTED);
		if (sh_skip != SH_SKIP_NONE || status == 0)
			break;
		status = sh_eval(node->u.binary.right, flags);
		break;
	case SH_NODE_NOT:
		status = sh_eval(node->u.body, SH_EV_TESTED);
		if (status == 0)
			status = 1;
		else
			status = 0;
		break;
	case SH_NODE_BACKGROUND:
		status = eval_background(node);
		break;
	case SH_NODE_PIPELINE:
		status = eval_pipeline(node, 0, flags);
		check_exit = 1;
		break;
	case SH_NODE_SIMPLE:
		status = eval_simple(node, flags);
		check_exit = 1;
		break;
	case SH_NODE_SUBSHELL:
		status = eval_subshell(node, flags);
		check_exit = 1;
		break;
	case SH_NODE_FUNCTION:
		sh_function_define(node->u.function.name, node->u.function.body,
				   current_arena);
		status = 0;
		break;
	case SH_NODE_COND:
	case SH_NODE_ARITH:
		/* A false [[ ]] or (( )) counts for errexit, as in bash. */
		status = eval_redirected(node, flags, &check_exit);
		check_exit = 1;
		break;
	default:
		status = eval_redirected(node, flags, &check_exit);
		break;
	}

	/* The pipes of the process substitutions the command used are closed. */
	if (process_substitution_count > substitution_mark)
		process_substitutions_close(substitution_mark);

	/* The status of the command is $?. */
	sh_status = status;

	/* A trap whose signal came while the command ran runs now. */
	if (sh_trap_pending_any)
		sh_trap_run_pending();

	/* errexit ends the shell where a command failed untested. */
	if (check_exit && status != 0 && sh_option[SH_OPT_ERREXIT] &&
	    (flags & SH_EV_TESTED) == 0 && sh_skip == SH_SKIP_NONE)
		sh_exit(status);

	/* A command the shell was to end after ends it. */
	if ((flags & SH_EV_EXIT) != 0)
		sh_exit(status);

	/* Succeeded: the status. */
	return status;
}

/*
 * Parses and runs the commands of a string, one at a time: eval, a trap's
 * action, sh -c.  Returns the status of the last, or 0 for none.
 */
int
sh_eval_string(
	const char *text,
	int flags)
{
	int status;

	/* Reads the string as the input until it runs out. */
	sh_input_push_string(text, strlen(text), sh_command_line);
	status = sh_eval_input(flags);
	sh_input_pop();

	/* Succeeded: the status of the last command. */
	return status;
}

/*
 * Parses and runs the commands of the innermost input until it ends, or
 * until a break, a continue or a return stops them.  With SH_EV_EXIT the
 * shell ends after the last, which may then replace the shell.
 */
int
sh_eval_input(
	int flags)
{
	struct sh_handler handler;
	struct sh_arena *volatile arena;
	struct sh_arena *saved_arena;
	struct sh_node *node;
	volatile int status;
	int kind;
	int eof;
	int depth;
	int last;

	/* A parse or a command that throws gives its block back first. */
	arena = NULL;
	status = 0;
	saved_arena = current_arena;
	sh_handler_push(&handler);
	kind = setjmp(handler.environment);
	if (kind != 0) {
		sh_handler_unwind(&handler);
		current_arena = saved_arena;
		sh_arena_release(arena);
		sh_raise(kind);
	}

	/* Parses a command, runs it, and drops its block. */
	depth = sh_input_depth();
	for (;;) {
		arena = sh_arena_new();
		node = sh_parse_command(arena, &eof);
		if (eof) {
			sh_arena_release(arena);
			arena = NULL;
			break;
		}

		/* Only the last command of the input may end the shell. */
		if (node != NULL) {
			current_arena = arena;
			last = sh_input_at_end(depth);
			if ((flags & SH_EV_EXIT) != 0 && !last)
				status = sh_eval(node, flags & ~SH_EV_EXIT);
			else
				status = sh_eval(node, flags);
			current_arena = saved_arena;
		}

		/* The command's memory goes with it. */
		sh_arena_release(arena);
		arena = NULL;

		/* A break, a continue or a return stops the input here. */
		if (sh_skip != SH_SKIP_NONE)
			break;
	}

	/* The handler for the input goes. */
	sh_handler_pop(&handler);

	/* An input that was to end the shell ends it. */
	if ((flags & SH_EV_EXIT) != 0)
		sh_exit(status);

	/* Succeeded: the status of the last command. */
	return status;
}

/*
 * Runs a command substitution: the commands of the text in a subshell whose
 * standard output is read.  The output, less NUL bytes and its trailing
 * newlines, is returned in *result.
 */
int
sh_run_command_substitution(
	const char *text,
	char **result)
{
	char *output;
	size_t length;
	pid_t child;
	int descriptors[2];
	int status;
	int piped;

	/* A pipeline or a single program runs from this shell, which forks no subshell for it. */
	if (substitution_in_shell(text, &output, &length, &status, 0)) {
		sh_last_substitution_status = status;
		while (length > 0 && output[length - 1] == '\n')
			length--;
		output[length] = '\0';
		*result = output;
		return 1;
	}

	/* A pipe carries the output back. */
	piped = pipe(descriptors);
	if (piped != 0)
		sh_error("cannot create pipe: %s", strerror(errno));

	/* The child runs the commands with the pipe as its output. */
	child = sh_fork(SH_FORK_NO_JOB, NULL);
	if (child == 0) {
		(void)close(descriptors[0]);
		if (descriptors[1] != 1) {
			(void)dup2(descriptors[1], 1);
			(void)close(descriptors[1]);
		}

		/* The child runs the text, its output into the pipe. */
		(void)sh_eval_string(text, SH_EV_EXIT);
		sh_exit(sh_status);
	}

	/* The shell reads the output from the other end. */
	(void)close(descriptors[1]);

	/* Reads everything the child writes. */
	output = read_output(descriptors[0], &length);
	(void)close(descriptors[0]);

	/* The substitution's status is the child's. */
	status = sh_wait_process(child, 0);
	sh_last_substitution_status = status;

	/* Trailing newlines are removed. */
	while (length > 0 && output[length - 1] == '\n')
		length--;
	output[length] = '\0';
	*result = output;

	/* Succeeded: the output, which the caller frees. */
	return 1;
}

/*
 * Fills an expansion context with the shell's state.
 */
void
sh_expand_context_fill(
	void *memory)
{
	struct sh_expand_context *context;

	/* Everything expansion asks the shell. */
	context = memory;
	memset(context, 0, sizeof(*context));
	context->status = sh_status;
	context->shell_pid = sh_root_pid;
	context->last_job = sh_last_background;
	context->lookup = expand_lookup;
	context->assign = expand_assign;
	context->command_substitute = expand_substitute;
	context->process_substitute = expand_process_substitution;
	context->lookup_context = NULL;
	context->shell_name = sh_arg0;
	context->positional_count = sh_parameters.count;
	context->positional = sh_parameters.values;
	context->unset_is_error = sh_option[SH_OPT_NOUNSET];
	context->options = sh_option_letters();
}

/*
 * Replaces the positional parameters with copies of argv.
 */
void
sh_parameters_set(
	int argc,
	char **argv)
{
	struct sh_parameters parameters;
	int index;

	/* Copies the words, since the vector goes with its command. */
	parameters.count = argc;
	parameters.owned = 1;
	parameters.values = sh_malloc(((size_t)argc + 1U) * sizeof(char *));
	for (index = 0; index < argc; index++)
		parameters.values[index] = sh_strdup(argv[index]);
	parameters.values[argc] = NULL;

	/* The old set goes. */
	sh_parameters_free(&sh_parameters);
	sh_parameters = parameters;
}

/*
 * Frees a set of positional parameters the shell owns.
 */
void
sh_parameters_free(
	struct sh_parameters *parameters)
{
	int index;

	/* The ones the shell was started with are not the shell's to free. */
	if (!parameters->owned || parameters->values == NULL)
		return;

	/* Each word, then the array. */
	for (index = 0; index < parameters->count; index++)
		free(parameters->values[index]);
	free(parameters->values);
	parameters->values = NULL;
	parameters->count = 0;
}

/*
 * Writes the trace of a command (set -x): PS4, then each word, quoted where
 * the shell would need it quoted to read it back.  It goes to standard error
 * as it was before the command's own redirections.
 */
void
sh_xtrace(
	int argc,
	char **argv,
	size_t assignment_count)
{
	struct sh_token token;
	struct sh_expand_context context;
	struct trace_buffer buffer;
	const char *prompt;
	const char *error_text;
	char *expanded;
	int index;
	int descriptor;
	int expanded_ok;

	/* PS4 is expanded like a word in double quotes. */
	(void)assignment_count;
	memset(&buffer, 0, sizeof(buffer));
	prompt = sh_var_get("PS4");
	if (prompt == NULL)
		prompt = "+ ";
	memset(&token, 0, sizeof(token));
	token.raw = (char *)prompt;
	token.raw_length = strlen(prompt);
	token.heredoc = SH_HEREDOC_EXPAND;
	sh_expand_context_fill(&context);
	expanded = NULL;
	expanded_ok = sh_expand_word(&token, &context, &expanded, &error_text);
	if (expanded_ok)
		trace_append(&buffer, expanded, strlen(expanded));
	else
		trace_append(&buffer, prompt, strlen(prompt));
	free(expanded);

	/* The assignments and the words, separated by spaces. */
	for (index = 0; index < argc; index++) {
		if (index > 0)
			trace_append(&buffer, " ", 1);
		xtrace_word(&buffer, argv[index]);
	}

	/* The trace ends the line. */
	trace_append(&buffer, "\n", 1);

	/* Written in one piece, to the standard error before redirection. */
	fflush(stderr);
	descriptor = sh_redirect_saved_descriptor(2);
	(void)write(descriptor, buffer.text, buffer.length);
	free(buffer.text);
}

/*
 * Replaces the process with a command: the assignments go into its
 * environment.  A file the system will not run is read as a shell script.
 */
void
sh_exec_external(
	const char *path,
	char **argv,
	char **assignments,
	size_t count)
{
	char **environment;
	char **script;
	size_t index;
	size_t argc;
	int error;

	/* The command's environment: the exports, with its assignments. */
	for (index = 0; index < count; index++)
		(void)sh_var_set_assignment(assignments[index], SH_VAR_EXPORT);
	environment = sh_var_environment();
	sh_signals_for_exec();

	/* Runs the file. */
	execve(path, argv, environment);
	error = errno;

	/* A file that is not a program is a script for this shell. */
	if (error == ENOEXEC) {
		for (argc = 0; argv[argc] != NULL; argc++)
			continue;
		script = sh_malloc((argc + 2U) * sizeof(*script));
		script[0] = (char *)"sh";
		script[1] = (char *)path;
		for (index = 1; index <= argc; index++)
			script[index + 1] = argv[index];
		execve("/bin/sh", script, environment);
		error = errno;
	}

	/* A file that is not there is 127; anything else, 126. */
	if (error == ENOENT || error == ENOTDIR || error == ENAMETOOLONG) {
		sh_warn("%s: not found", argv[0]);
		_exit(127);
	}

	/* A file that is there and cannot be run. */
	sh_warn("%s: %s", argv[0], strerror(error));
	_exit(126);
}

/*
 * Runs a simple command.
 *
 * The words are expanded first, then the redirections are applied in the
 * shell (a child the command runs in inherits them), then the assignments
 * are expanded, as dash does.
 */
static int
eval_simple(
	struct sh_node *node,
	int flags)
{
	struct word_list arguments;
	struct sh_command entry;
	const struct sh_builtin *builtin;
	const char *search_path_value;
	char **assignments;
	size_t assignment_count;
	size_t mark;
	size_t index;
	int find_flags;
	int special;
	int status;
	int compare;

	/* The command's allocations go when it ends. */
	mark = sh_temp_mark();
	sh_command_line = node->line;
	sh_last_substitution_status = -1;

	/* Expands the words; after a declaration utility, as assignments. */
	memset(&arguments, 0, sizeof(arguments));
	expand_words(node->u.simple.words, node->u.simple.word_count,
		     &arguments, 1);

	/* command [-p] [--] name: looks name up without functions. */
	index = command_prefix(&arguments, &find_flags);
	arguments.words += index;
	arguments.count -= index;

	/*
	 * The redirections are applied here, before the assignments are
	 * expanded; a failed one ends a shell that is not interactive when
	 * the command is a special builtin.
	 */
	builtin = NULL;
	if (arguments.count > 0 && find_flags == 0)
		builtin = sh_builtin_find(arguments.words[0]);
	special = 0;
	if (builtin != NULL && (builtin->flags & SH_BUILTIN_SPECIAL) != 0)
		special = 1;
	status = sh_redirect(node->redirections, SH_REDIRECT_SAVE);
	if (status != 0) {
		sh_redirect_pop();
		sh_temp_release(mark);
		if (special && !sh_option[SH_OPT_INTERACTIVE])
			sh_raise_error(status);
		return status;
	}

	/* Expands the assignments; a PATH among them is the one searched. */
	assignment_count = node->u.simple.assignment_count;
	assignments = sh_temp_own(sh_malloc((assignment_count + 1U) *
					    sizeof(*assignments)));
	search_path_value = NULL;
	for (index = 0; index < assignment_count; index++) {
		assignments[index] =
		    expand_assignment(node->u.simple.assignments[index]);
		compare = strncmp(assignments[index], "PATH=", 5);
		if (compare == 0)
			search_path_value = assignments[index] + 5;
	}

	/* The assignments end with a null. */
	assignments[assignment_count] = NULL;

	/* Finds what the name is. */
	entry.kind = SH_COMMAND_NOT_FOUND;
	if (arguments.count > 0)
		sh_find_command(arguments.words[0], find_flags, search_path_value, &entry);

	/* Traces the command. */
	if (sh_option[SH_OPT_XTRACE])
		trace_command(assignments, assignment_count, &arguments);

	/* A command with no name: its assignments, which stay. */
	if (arguments.count == 0) {
		assign_permanently(assignments, assignment_count, 0);
		sh_redirect_pop();
		status = 0;
		if (sh_last_substitution_status >= 0)
			status = sh_last_substitution_status;
		sh_temp_release(mark);
		return status;
	}

	/* A file runs in a child; anything else in this shell. */
	if (entry.kind == SH_COMMAND_EXTERNAL ||
	    entry.kind == SH_COMMAND_NOT_FOUND)
		status = run_external(node, &entry, &arguments, assignments,
				      assignment_count, flags);
	else
		status = run_in_shell(&entry, &arguments, assignments, assignment_count, flags, special);
	sh_redirect_pop();
	sh_temp_release(mark);

	/* Succeeded: the command's status. */
	return status;
}

/*
 * Counts the leading words of command [-p] [--] that name a command to look
 * up without functions (and, with -p, on the default PATH), and sets the
 * flags of that lookup.  command with -v or -V is left as the command.
 */
static size_t
command_prefix(
	struct word_list *arguments,
	int *find_flags)
{
	struct sh_function *function;
	const char *word;
	size_t index;
	size_t next;
	int prefix_flags;
	int compare;

	/* A function named command is a function like any other. */
	*find_flags = 0;
	function = sh_function_find("command");
	if (function != NULL)
		return 0;

	/* Each "command", with its -p and --. */
	index = 0;
	while (index < arguments->count) {
		compare = strcmp(arguments->words[index], "command");
		if (compare != 0)
			break;

		/* Its options: -p and --; any other stops here. */
		next = index + 1;
		prefix_flags = SH_FIND_NO_FUNCTIONS;
		while (next < arguments->count &&
		       arguments->words[next][0] == '-') {
			word = arguments->words[next];
			if (word[1] == 'p' && word[2] == '\0') {
				prefix_flags |= SH_FIND_DEFAULT_PATH;
				next++;
				continue;
			}

			/* -- ends the options. */
			if (word[1] == '-' && word[2] == '\0')
				next++;
			else
				prefix_flags = 0;
			break;
		}

		/* command alone, or with other options, is the builtin. */
		if (prefix_flags == 0 || next >= arguments->count)
			break;
		*find_flags |= prefix_flags;
		index = next;
	}

	/* Succeeded: how many words the prefix took. */
	return index;
}

/*
 * Runs a command that is a file: in a child, or in place of the shell when
 * the shell ends after it and no trap is set.
 */
static int
run_external(
	struct sh_node *node,
	struct sh_command *entry,
	struct word_list *arguments,
	char **assignments,
	size_t assignment_count,
	int flags)
{
	pid_t child;
	int status;
	int trapped;
	void *job;

	/* The last command of a shell need not fork. */
	trapped = sh_trap_any_set();
	if ((flags & SH_EV_EXIT) != 0 && !trapped) {
		if (entry->kind == SH_COMMAND_NOT_FOUND) {
			sh_warn("%s: not found", arguments->words[0]);
			sh_exit(127);
		}

		/* The command replaces the shell. */
		sh_exec_external(entry->path, arguments->words, assignments,
				 assignment_count);
	}

	/* A command that was found and needs no assignments starts without a copy of the shell. */
	job = sh_job_new(0);
	child = -1;
	if (entry->kind != SH_COMMAND_NOT_FOUND && assignment_count == 0)
		child = sh_spawn(entry->path, arguments->words, job, NULL);

	/* Otherwise a child runs it; a name not found is 127. */
	if (child < 0)
		child = sh_fork(SH_FORK_FOREGROUND, job);
	if (child == 0) {
		if (entry->kind == SH_COMMAND_NOT_FOUND) {
			sh_warn("%s: not found", arguments->words[0]);
			_exit(127);
		}

		/* The child becomes the command. */
		sh_exec_external(entry->path, arguments->words, assignments,
				 assignment_count);
	}

	/* Waits for it. */
	sh_job_set_text(job, node);
	status = sh_job_wait_foreground(job);

	/* Succeeded: the command's status. */
	return status;
}

/*
 * Runs a builtin or a function in this shell.  A special builtin keeps the
 * command's assignments (exec exports them to the command it runs); a
 * function or another builtin keeps them for its length only.
 */
static int
run_in_shell(
	struct sh_command *entry,
	struct word_list *arguments,
	char **assignments,
	size_t assignment_count,
	int flags,
	int special)
{
	int status;
	int saved;
	int compare;

	/* A function: its assignments are local to the call. */
	if (entry->kind == SH_COMMAND_FUNCTION) {
		sh_var_local_push();
		assign_temporarily(assignments, assignment_count);
		status = call_function(entry->function, (int)arguments->count,
				       arguments->words, flags);
		sh_var_local_pop();
		return status;
	}

	/* A special builtin: its assignments stay. */
	if (special) {
		compare = strcmp(arguments->words[0], "exec");
		if (compare == 0 && arguments->count > 1)
			assign_permanently(assignments, assignment_count, SH_VAR_EXPORT);
		else
			assign_permanently(assignments, assignment_count, 0);
		status = sh_builtin_run(entry->builtin, (int)arguments->count,
					arguments->words);
		return status;
	}

	/* Another builtin: its assignments are put back after it. */
	saved = sh_var_local_depth();
	if (assignment_count > 0) {
		sh_var_local_push();
		assign_temporarily(assignments, assignment_count);
	}

	/* The builtin runs in the shell. */
	status = sh_builtin_run(entry->builtin, (int)arguments->count,
				arguments->words);
	sh_var_local_unwind(saved);

	/* Succeeded: the builtin's status. */
	return status;
}

/* Writes the trace of a simple command: its assignments, then its words. */
static void
trace_command(
	char **assignments,
	size_t assignment_count,
	struct word_list *arguments)
{
	char **traced;
	size_t index;

	/* An empty command is not traced. */
	if (arguments->count == 0 && assignment_count == 0)
		return;

	/* The assignments and the words, in one array. */
	traced = sh_temp_own(sh_malloc((assignment_count + arguments->count +
					1U) * sizeof(*traced)));
	for (index = 0; index < assignment_count; index++)
		traced[index] = assignments[index];
	for (index = 0; index < arguments->count; index++)
		traced[assignment_count + index] = arguments->words[index];

	/* Written to standard error. */
	sh_xtrace((int)(assignment_count + arguments->count), traced,
		  assignment_count);
}

/* Runs a pipeline: each command in a child, joined by pipes. */
static int
eval_pipeline(
	struct sh_node *node,
	int background,
	int flags)
{
	int status;
	void *job;

	/* Starts each command. */
	job = pipeline_start(node, background, flags, -1, -1);

	/* A background pipeline is left running. */
	if (background) {
		sh_job_background(job);
		return 0;
	}

	/* Waits for all of it; the status is the last command's. */
	status = sh_job_wait_foreground(job);

	/* Succeeded: the pipeline's status. */
	return status;
}

/*
 * Starts the commands of a pipeline as a job, each joined to the next by
 * a pipe; the last writes to output when it is not -1 (a command
 * substitution's pipe, whose other end output_other the commands close).
 * A command that is only a program to run starts with posix_spawn; any
 * other runs in a forked child.  Returns the job.
 */
static void *
pipeline_start(
	struct sh_node *node,
	int background,
	int flags,
	int output,
	int output_other)
{
	struct sh_node **commands;
	size_t count;
	size_t index;
	pid_t child;
	int previous;
	int descriptors[2];
	int piped;
	int last_output;
	void *job;

	/* Starts each command with the pipe before it as its input. */
	commands = node->u.pipeline.commands;
	count = node->u.pipeline.count;
	job = sh_job_new(background);
	previous = -1;
	for (index = 0; index < count; index++) {
		/* A pipe to the next command, unless this is the last. */
		descriptors[0] = -1;
		descriptors[1] = -1;
		if (index + 1 < count) {
			piped = pipe(descriptors);
			if (piped != 0)
				sh_error("cannot create pipe: %s", strerror(errno));
		}

		/*
		 * echo or printf before the last command writes into its pipe
		 * from this shell; a program in the foreground starts without a
		 * copy of the shell.
		 */
		child = -1;
		last_output = descriptors[1];
		if (last_output < 0)
			last_output = output;
		if (!background && index + 1 < count &&
		    pipeline_inline(commands[index], descriptors[1]))
			child = 0;
		if (!background && child < 0)
			child = pipeline_spawn(commands[index], previous,
					       last_output, descriptors[0],
					       output_other, job);

		/* Anything else runs in a child of the job. */
		if (child < 0) {
			if (background)
				child = sh_fork(SH_FORK_BACKGROUND, job);
			else
				child = sh_fork(SH_FORK_FOREGROUND, job);
			if (child == 0)
				pipeline_child(commands, index, previous, descriptors,
					       output, output_other, background, flags);
		}

		/* The shell keeps only the reading end for the next. */
		if (previous >= 0)
			(void)close(previous);
		if (descriptors[1] >= 0)
			(void)close(descriptors[1]);
		previous = descriptors[0];
	}

	/* Succeeded: the job, named after the pipeline. */
	sh_job_set_text(job, node);
	return job;
}

/*
 * Starts one command of a foreground pipeline with posix_spawn when it is
 * a program and nothing more: a simple command without assignments or
 * redirections whose words expand the same in this shell as in a
 * subshell, without changing anything and without failing.  input and
 * output (or -1) become its standard input and output; other and
 * output_other are closed in it.  Returns the child, or -1 when the
 * command must run in a forked subshell.
 */
static pid_t
pipeline_spawn(
	struct sh_node *node,
	int input,
	int output,
	int other,
	int output_other,
	void *job)
{
	posix_spawn_file_actions_t actions;
	struct word_list arguments;
	struct sh_command entry;
	size_t mark;
	size_t index;
	pid_t child;
	int pure;
	int error;

	/* Only a program with its words, and only where nothing needs the subshell. */
	if (node->kind != SH_NODE_SIMPLE ||
	    node->redirections != NULL ||
	    node->u.simple.assignment_count != 0 ||
	    node->u.simple.word_count == 0)
		return -1;
	if (sh_option[SH_OPT_NOUNSET] || sh_option[SH_OPT_XTRACE] ||
	    sh_option[SH_OPT_NOEXEC] || sh_job_control_active())
		return -1;

	/* Descriptors 0, 1 and 2 are where the command's own go. */
	if ((input >= 0 && input < 3) || (output >= 0 && output < 3))
		return -1;

	/* Every word expands without effects in this shell. */
	for (index = 0; index < node->u.simple.word_count; index++) {
		pure = word_is_pure(node->u.simple.words[index]);
		if (!pure)
			return -1;
	}

	/* The words, and what the name is; a subshell's lookup is not remembered. */
	mark = sh_temp_mark();
	memset(&arguments, 0, sizeof(arguments));
	expand_words(node->u.simple.words, node->u.simple.word_count,
		     &arguments, 0);
	if (arguments.count == 0) {
		sh_temp_release(mark);
		return -1;
	}
	sh_find_command(arguments.words[0], SH_FIND_NO_REMEMBER, NULL, &entry);
	if (entry.kind != SH_COMMAND_EXTERNAL) {
		sh_temp_release(mark);
		return -1;
	}

	/* Its descriptors, as a forked child of the pipeline would set them. */
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0) {
		sh_temp_release(mark);
		return -1;
	}
	if (input >= 0) {
		(void)posix_spawn_file_actions_adddup2(&actions, input, 0);
		(void)posix_spawn_file_actions_addclose(&actions, input);
	}
	if (output >= 0) {
		(void)posix_spawn_file_actions_adddup2(&actions, output, 1);
		(void)posix_spawn_file_actions_addclose(&actions, output);
	}
	if (other >= 0)
		(void)posix_spawn_file_actions_addclose(&actions, other);
	if (output_other >= 0)
		(void)posix_spawn_file_actions_addclose(&actions, output_other);

	/* The program. */
	child = sh_spawn(entry.path, arguments.words, job, &actions);
	(void)posix_spawn_file_actions_destroy(&actions);
	sh_temp_release(mark);

	/* Succeeded or not: the child, or -1 for a fork. */
	return child;
}

/*
 * Reports whether expanding a word can neither change the shell nor fail:
 * no command substitution, arithmetic or backquote, and no ${...} but a
 * name, a positional parameter, a special parameter, or a length.  Such a
 * word expands the same in the shell as in a subshell.
 */
static int
word_is_pure(
	const struct sh_token *word)
{
	const char *cursor;
	const char *end;
	const char *inner;
	const char *limit;

	/* A process substitution starts a process. */
	if (word->process != 0)
		return 0;

	/* Each $ and ` of the text as written; a quoted one only makes this cautious. */
	limit = word->raw + word->raw_length;
	for (cursor = word->raw; cursor < limit; cursor++) {
		if (*cursor == '`')
			return 0;
		if (*cursor != '$' || cursor + 1 >= limit)
			continue;
		if (cursor[1] == '(')
			return 0;
		if (cursor[1] != '{')
			continue;

		/* ${...}: a name, a length of a name, digits, or one special character. */
		inner = cursor + 2;
		end = inner;
		while (end < limit && *end != '}')
			end++;
		if (end >= limit || end == inner)
			return 0;
		if (*inner == '#' && end - inner > 1)
			inner++;
		if (end - inner == 1 && strchr("@*#?$!-0123456789", *inner) != NULL) {
			cursor = end;
			continue;
		}
		for (; inner < end; inner++) {
			if (!(*inner == '_' ||
			      (*inner >= 'a' && *inner <= 'z') ||
			      (*inner >= 'A' && *inner <= 'Z') ||
			      (*inner >= '0' && *inner <= '9')))
				return 0;
		}
		cursor = end;
	}

	/* Succeeded: nothing in it acts. */
	return 1;
}

/*
 * Runs a subshell whose body is only unset of variables (directly, or
 * through eval), as scripts do to learn whether unset would succeed
 * (libtool's func_unset: (eval unset $1) >/dev/null 2>&1).  The unset
 * runs in this shell under the subshell's redirections, and each
 * variable is put back as it was, so the shell is left as the subshell
 * would leave it.  Returns 1 with the status, or 0 to fork the subshell.
 */
static int
subshell_unset_trial(
	struct sh_node *node,
	int *status)
{
	struct word_list arguments;
	struct sh_node *body;
	size_t mark;
	size_t index;
	int pure;
	int done;

	/* The body is one simple command of pure words, with nothing traced. */
	body = node->u.body;
	if (body == NULL || body->kind != SH_NODE_SIMPLE ||
	    body->redirections != NULL ||
	    body->u.simple.assignment_count != 0 ||
	    body->u.simple.word_count < 2 ||
	    sh_option[SH_OPT_NOUNSET] || sh_option[SH_OPT_XTRACE] ||
	    sh_option[SH_OPT_NOEXEC] || sh_trap_any_set())
		return 0;
	for (index = 0; index < body->u.simple.word_count; index++) {
		pure = word_is_pure(body->u.simple.words[index]);
		if (!pure)
			return 0;
	}

	/* The words, and the trial. */
	mark = sh_temp_mark();
	memset(&arguments, 0, sizeof(arguments));
	expand_words(body->u.simple.words, body->u.simple.word_count,
		     &arguments, 0);
	done = 0;
	if (arguments.count > 0)
		done = unset_trial(&arguments, node, status, 0);
	sh_temp_release(mark);

	/* Reports whether it ran. */
	return done;
}

/*
 * Runs unset NAME... (or eval of words that make it, once) for a
 * subshell as subshell_unset_trial says.  Returns 1 with the status.
 */
static int
unset_trial(
	struct word_list *arguments,
	struct sh_node *subshell,
	int *status,
	int depth)
{
	const struct sh_builtin *builtin;
	struct word_list inner;
	struct sh_arena *arena;
	struct sh_node *node;
	char **values;
	int *flags;
	char *joined;
	size_t joined_length;
	size_t index;
	size_t count;
	int pure;
	int done;
	int redirected;
	int compare;

	/* eval: its words, joined by spaces, are the command (once). */
	compare = strcmp(arguments->words[0], "eval");
	if (compare == 0) {
		if (depth > 0 || arguments->count < 2)
			return 0;
		joined_length = 0;
		for (index = 1; index < arguments->count; index++)
			joined_length += strlen(arguments->words[index]) + 1U;
		joined = sh_malloc(joined_length);
		joined[0] = '\0';
		for (index = 1; index < arguments->count; index++) {
			if (index > 1)
				strcat(joined, " ");
			strcat(joined, arguments->words[index]);
		}

		/* The command the words make: one simple command of pure words. */
		arena = sh_arena_new();
		node = parse_quietly(joined, arena);
		free(joined);
		done = 0;
		if (node != NULL && node->kind == SH_NODE_SIMPLE &&
		    node->redirections == NULL &&
		    node->u.simple.assignment_count == 0 &&
		    node->u.simple.word_count > 0) {
			pure = 1;
			for (index = 0; index < node->u.simple.word_count; index++) {
				if (!word_is_pure(node->u.simple.words[index]))
					pure = 0;
			}
			if (pure) {
				memset(&inner, 0, sizeof(inner));
				expand_words(node->u.simple.words,
					     node->u.simple.word_count, &inner, 0);
				if (inner.count > 0)
					done = unset_trial(&inner, subshell, status,
							   depth + 1);
			}
		}
		sh_arena_release(arena);
		return done;
	}

	/* unset of names only: no options, no function, nothing the shell watches. */
	compare = strcmp(arguments->words[0], "unset");
	if (compare != 0 || arguments->count < 2)
		return 0;
	for (index = 1; index < arguments->count; index++) {
		if (!sh_var_name(arguments->words[index]) ||
		    sh_var_hooked(arguments->words[index]) ||
		    sh_function_find(arguments->words[index]) != NULL)
			return 0;

		/* A read-only one makes unset raise an error, which only a subshell may take. */
		if (sh_var_flags(arguments->words[index]) >= 0 &&
		    (sh_var_flags(arguments->words[index]) & SH_VAR_READONLY) != 0)
			return 0;
	}
	builtin = sh_builtin_find("unset");
	if (builtin == NULL)
		return 0;

	/* What each variable is now, to be put back. */
	count = arguments->count - 1U;
	values = sh_malloc(count * sizeof(*values));
	flags = sh_malloc(count * sizeof(*flags));
	for (index = 0; index < count; index++) {
		values[index] = NULL;
		if (sh_var_get(arguments->words[index + 1U]) != NULL)
			values[index] = sh_strdup(sh_var_get(arguments->words[index + 1U]));
		flags[index] = sh_var_flags(arguments->words[index + 1U]);
	}

	/* The unset, under the subshell's redirections. */
	redirected = sh_redirect(subshell->redirections, SH_REDIRECT_SAVE);
	if (redirected == 0)
		*status = sh_builtin_run(builtin, (int)arguments->count,
					 arguments->words);
	else
		*status = redirected;
	sh_redirect_pop();

	/* Each variable as it was. */
	for (index = 0; index < count; index++) {
		if (flags[index] >= 0) {
			if (values[index] != NULL)
				(void)sh_var_set(arguments->words[index + 1U],
						 values[index], flags[index]);
			else
				sh_var_add_flags(arguments->words[index + 1U],
						 flags[index]);
		}
		free(values[index]);
	}
	free(values);
	free(flags);

	/* Succeeded. */
	return 1;
}

/*
 * Runs a command substitution from this shell when its text is one
 * pipeline (each of whose commands is a subshell already), a single
 * program, echo or printf, or eval of words that make one of those:
 * none of them changes this shell, so the subshell the substitution
 * would fork first adds nothing.  Returns 1 with the output and status,
 * or 0 when the text must run in a forked subshell as usual.
 */
static int
substitution_in_shell(
	const char *text,
	char **output,
	size_t *length,
	int *status,
	int depth)
{
	struct sh_arena *arena;
	struct sh_node *node;
	int descriptors[2];
	int done;
	void *job;

	/* A here-document, and job control, are left to the forked subshell. */
	if (strstr(text, "<<") != NULL || sh_job_control_active())
		return 0;

	/* Parses the text; anything but one command is the subshell's. */
	arena = sh_arena_new();
	node = parse_quietly(text, arena);
	if (node == NULL || node->redirections != NULL) {
		sh_arena_release(arena);
		return 0;
	}

	/* A simple command: echo, printf, a program, or eval of one. */
	if (node->kind == SH_NODE_SIMPLE) {
		done = substitution_simple(node, output, length, status, depth);
		sh_arena_release(arena);
		return done;
	}
	if (node->kind != SH_NODE_PIPELINE) {
		sh_arena_release(arena);
		return 0;
	}

	/* The pipeline, its last command writing into the substitution's pipe. */
	if (pipe(descriptors) != 0)
		sh_error("cannot create pipe: %s", strerror(errno));
	job = pipeline_start(node, 0, 0, descriptors[1], descriptors[0]);
	(void)close(descriptors[1]);

	/* Reads everything, then waits for the commands. */
	*output = read_output(descriptors[0], length);
	(void)close(descriptors[0]);
	*status = sh_job_wait_foreground(job);
	sh_arena_release(arena);

	/* Succeeded. */
	return 1;
}

/*
 * Runs a command substitution whose text is one simple command from this
 * shell: echo or printf whose output fits in the pipe, a program started
 * with posix_spawn, or (once) eval of words that make one of those.
 * Returns 1 with the output and status, or 0 for the forked subshell.
 */
static int
substitution_simple(
	struct sh_node *node,
	char **output,
	size_t *length,
	int *status,
	int depth)
{
	struct word_list arguments;
	char *joined;
	size_t joined_length;
	size_t mark;
	size_t index;
	pid_t child;
	int descriptors[2];
	int pure;
	int done;
	int compare;
	void *job;

	/* Words that expand without effects, and no assignments. */
	if (node->u.simple.assignment_count != 0 ||
	    node->u.simple.word_count == 0 ||
	    sh_option[SH_OPT_NOUNSET] || sh_option[SH_OPT_XTRACE] ||
	    sh_option[SH_OPT_NOEXEC])
		return 0;
	for (index = 0; index < node->u.simple.word_count; index++) {
		pure = word_is_pure(node->u.simple.words[index]);
		if (!pure)
			return 0;
	}

	/* The words. */
	mark = sh_temp_mark();
	memset(&arguments, 0, sizeof(arguments));
	expand_words(node->u.simple.words, node->u.simple.word_count,
		     &arguments, 0);
	if (arguments.count == 0) {
		sh_temp_release(mark);
		return 0;
	}

	/* eval: its words, joined by spaces, are the text (one level deep). */
	compare = strcmp(arguments.words[0], "eval");
	if (compare == 0) {
		done = 0;
		if (depth == 0 && arguments.count > 1) {
			joined_length = 0;
			for (index = 1; index < arguments.count; index++)
				joined_length += strlen(arguments.words[index]) + 1U;
			joined = sh_malloc(joined_length);
			joined[0] = '\0';
			for (index = 1; index < arguments.count; index++) {
				if (index > 1)
					strcat(joined, " ");
				strcat(joined, arguments.words[index]);
			}
			done = substitution_in_shell(joined, output, length,
						     status, depth + 1);
			free(joined);
		}
		sh_temp_release(mark);
		return done;
	}

	/* echo or printf writes into the pipe from this shell. */
	if (pipe(descriptors) != 0)
		sh_error("cannot create pipe: %s", strerror(errno));
	done = output_builtin(&arguments, descriptors[1], status);
	sh_temp_release(mark);
	if (done) {
		(void)close(descriptors[1]);
		*output = read_output(descriptors[0], length);
		(void)close(descriptors[0]);
		return 1;
	}

	/* A program starts with posix_spawn, its output into the pipe. */
	job = sh_job_new(0);
	child = pipeline_spawn(node, -1, descriptors[1], -1, descriptors[0], job);
	(void)close(descriptors[1]);
	if (child < 0) {
		(void)close(descriptors[0]);
		(void)sh_job_wait_foreground(job);
		return 0;
	}

	/* Reads everything, then waits for it. */
	*output = read_output(descriptors[0], length);
	(void)close(descriptors[0]);
	*status = sh_job_wait_foreground(job);

	/* Succeeded. */
	return 1;
}

/*
 * Runs a command of a pipeline that is echo or printf, not the last, in
 * this shell, writing into the pipe after it: its words expand without
 * effects and its output fits in the pipe, so the write finishes before
 * the next command is started, and nothing of the shell changes.
 * Returns 1 when it ran, and 0 when it must run in a child.
 */
static int
pipeline_inline(
	struct sh_node *node,
	int output)
{
	struct word_list arguments;
	size_t mark;
	size_t index;
	int pure;
	int status;
	int done;

	/* A simple command of pure words, when pipefail does not ask for its status. */
	if (node->kind != SH_NODE_SIMPLE ||
	    node->redirections != NULL ||
	    node->u.simple.assignment_count != 0 ||
	    node->u.simple.word_count == 0 ||
	    sh_option[SH_OPT_PIPEFAIL] || sh_option[SH_OPT_NOUNSET] ||
	    sh_option[SH_OPT_XTRACE] || sh_option[SH_OPT_NOEXEC] ||
	    sh_job_control_active())
		return 0;
	for (index = 0; index < node->u.simple.word_count; index++) {
		pure = word_is_pure(node->u.simple.words[index]);
		if (!pure)
			return 0;
	}

	/* The words, then the builtin into the pipe. */
	mark = sh_temp_mark();
	memset(&arguments, 0, sizeof(arguments));
	expand_words(node->u.simple.words, node->u.simple.word_count,
		     &arguments, 0);
	done = 0;
	if (arguments.count > 0)
		done = output_builtin(&arguments, output, &status);
	sh_temp_release(mark);

	/* Reports whether it ran. */
	return done;
}

/*
 * Runs echo or printf (the builtins, not functions of those names) with
 * its standard output on a descriptor, when its output is known to fit
 * in a pipe.  Returns 1 with its status, or 0 when it did not run.
 */
static int
output_builtin(
	struct word_list *arguments,
	int output,
	int *status)
{
	struct sh_command entry;
	size_t bound;
	int saved;
	int echo;
	int printf_name;

	/* Only echo and printf, as the builtins. */
	echo = strcmp(arguments->words[0], "echo") == 0;
	printf_name = strcmp(arguments->words[0], "printf") == 0;
	if (!echo && !printf_name)
		return 0;
	sh_find_command(arguments->words[0], SH_FIND_NO_REMEMBER, NULL, &entry);
	if (entry.kind != SH_COMMAND_BUILTIN)
		return 0;

	/* The output must fit in a pipe, so that writing it cannot wait. */
	bound = output_bound(arguments);
	if (bound > SH_INLINE_OUTPUT_MAX)
		return 0;

	/* Its standard output is the descriptor while it runs. */
	fflush(stdout);
	saved = fcntl(1, F_DUPFD_CLOEXEC, 10);
	if (saved < 0)
		return 0;
	(void)dup2(output, 1);
	*status = sh_builtin_run(entry.builtin, (int)arguments->count,
				 arguments->words);
	(void)dup2(saved, 1);
	(void)close(saved);

	/* Succeeded. */
	return 1;
}

/*
 * Returns the most bytes echo or printf can write for its arguments, or
 * more than the limit when that cannot be told: echo writes its operands
 * and the spaces and newline (escapes only shorten them); printf with a
 * format of %s, %% and plain text writes the format once for each set of
 * arguments, and the arguments.
 */
static size_t
output_bound(
	struct word_list *arguments)
{
	const char *format;
	const char *cursor;
	size_t total;
	size_t first;
	size_t index;
	size_t conversions;
	size_t operands;
	size_t rounds;

	/* The operands' length. */
	first = 1;
	if (strcmp(arguments->words[0], "printf") == 0) {
		if (arguments->count > 1 && strcmp(arguments->words[1], "--") == 0)
			first = 2;
		if (first >= arguments->count)
			return 0;
		first++;
	}
	total = 0;
	for (index = first; index < arguments->count; index++)
		total += strlen(arguments->words[index]) + 1U;

	/* echo: the operands, spaces and the newline. */
	if (strcmp(arguments->words[0], "echo") == 0)
		return total + 1U;

	/* printf: only %s and %% among its conversions. */
	format = arguments->words[first - 1U];
	conversions = 0;
	for (cursor = format; *cursor != '\0'; cursor++) {
		if (*cursor != '%')
			continue;
		cursor++;
		if (*cursor == '%')
			continue;
		if (*cursor != 's')
			return SH_INLINE_OUTPUT_MAX + 1U;
		conversions++;
	}

	/* The format is used again while operands are left. */
	operands = arguments->count - first;
	rounds = 1;
	if (conversions > 0 && operands > conversions)
		rounds = (operands + conversions - 1U) / conversions;
	total += rounds * strlen(format);

	/* Succeeded. */
	return total;
}

/*
 * Parses text that is to be one command, without a message for an error:
 * the subshell that runs the text if the shell does not reports it.
 * Returns the command, or NULL when the text is not exactly one command
 * or does not parse.
 */
static struct sh_node *
parse_quietly(
	const char *text,
	struct sh_arena *arena)
{
	struct sh_handler handler;
	struct sh_node *volatile node;
	struct sh_node *next;
	volatile int pushed;
	int kind;
	int eof;

	/* An error of the parse gives up the trial. */
	node = NULL;
	pushed = 0;
	sh_error_quiet++;
	sh_handler_push(&handler);
	kind = setjmp(handler.environment);
	if (kind != 0) {
		sh_handler_unwind(&handler);
		sh_error_quiet--;
		if (pushed)
			sh_input_pop();
		if (kind != SH_EX_ERROR)
			sh_raise(kind);
		return NULL;
	}

	/* One command, then the end of the text. */
	sh_input_push_string(text, strlen(text), sh_command_line);
	pushed = 1;
	node = sh_parse_command(arena, &eof);
	if (!eof) {
		next = sh_parse_command(arena, &eof);
		if (!eof || next != NULL)
			node = NULL;
	}
	sh_handler_pop(&handler);
	sh_error_quiet--;
	sh_input_pop();

	/* Succeeded or not: the command. */
	return node;
}

/* Runs one command of a pipeline in its child, joined to its neighbours. */
static void
pipeline_child(
	struct sh_node **commands,
	size_t index,
	int previous,
	int *descriptors,
	int output,
	int output_other,
	int background,
	int flags)
{
	int job_control;

	/* The pipe before it is its input; a background start reads nothing. */
	if (previous >= 0) {
		(void)dup2(previous, 0);
		(void)close(previous);
	} else if (background) {
		job_control = sh_job_control_active();
		if (!job_control)
			sh_redirect_stdin_null(commands[0]);
	}

	/* The pipe after it is its output; the last writes to the given output. */
	if (descriptors[1] >= 0) {
		(void)close(descriptors[0]);
		(void)dup2(descriptors[1], 1);
		(void)close(descriptors[1]);
	} else if (output >= 0) {
		(void)dup2(output, 1);
		(void)close(output);
	}

	/* The other end of a substitution's pipe is the shell's. */
	if (output_other >= 0)
		(void)close(output_other);

	/* Runs it, and ends. */
	(void)sh_eval(commands[index], SH_EV_EXIT | (flags & SH_EV_TESTED));
	sh_exit(sh_status);
}

/* Starts a command in the background. */
static int
eval_background(
	struct sh_node *node)
{
	struct sh_node *body;
	pid_t child;
	int status;
	int job_control;
	void *job;

	/* A pipeline is started as a job of its own processes. */
	body = node->u.body;
	if (body->kind == SH_NODE_PIPELINE) {
		status = eval_pipeline(body, 1, 0);
		return status;
	}

	/* Anything else runs in one child. */
	job = sh_job_new(1);
	sh_job_set_text(job, body);
	child = sh_fork(SH_FORK_BACKGROUND, job);
	if (child == 0) {
		job_control = sh_job_control_active();
		if (!job_control)
			sh_redirect_stdin_null(body);
		(void)sh_eval(body, SH_EV_EXIT);
		sh_exit(sh_status);
	}

	/* The shell goes on without waiting. */
	sh_job_background(job);

	/* Succeeded: starting it is the status. */
	return 0;
}

/* Runs a subshell: the body in a child, redirected there. */
static int
eval_subshell(
	struct sh_node *node,
	int flags)
{
	pid_t child;
	int status;
	int trapped;
	int redirected;
	void *job;

	/* The last command of a shell need not fork again. */
	trapped = sh_trap_any_set();
	if ((flags & SH_EV_EXIT) != 0 && !trapped) {
		redirected = sh_redirect(node->redirections, 0);
		if (redirected != 0)
			sh_exit(2);
		(void)sh_eval(node->u.body,
			      SH_EV_EXIT | (flags & SH_EV_TESTED));
		sh_exit(sh_status);
	}

	/* A subshell that only tries unset is tried in this shell and undone. */
	if (subshell_unset_trial(node, &status))
		return status;

	/* A child runs the body. */
	job = sh_job_new(0);
	child = sh_fork(SH_FORK_FOREGROUND, job);
	if (child == 0) {
		redirected = sh_redirect(node->redirections, 0);
		if (redirected != 0)
			sh_exit(2);
		(void)sh_eval(node->u.body,
			      SH_EV_EXIT | (flags & SH_EV_TESTED));
		sh_exit(sh_status);
	}

	/* The shell waits for the subshell. */
	status = sh_job_wait_foreground(job);

	/* Succeeded: the subshell's status. */
	return status;
}

/*
 * Runs a compound command under its redirections.  A failed redirection
 * sets *failed, which errexit counts as a failed command.
 */
static int
eval_redirected(
	struct sh_node *node,
	int flags,
	int *failed)
{
	int status;

	/* Without redirections, the command itself. */
	if (node->redirections == NULL) {
		status = eval_body(node, flags);
		return status;
	}

	/* A failed redirection fails the command without running it. */
	status = sh_redirect(node->redirections, SH_REDIRECT_SAVE);
	if (status == 0)
		status = eval_body(node, flags & ~SH_EV_EXIT);
	else
		*failed = 1;
	sh_redirect_pop();

	/* Succeeded: the command's status. */
	return status;
}

/* Runs a compound command that runs in this shell. */
static int
eval_body(
	struct sh_node *node,
	int flags)
{
	int status;

	/* Dispatches on the kind of compound command. */
	switch (node->kind) {
	case SH_NODE_GROUP:
		status = sh_eval(node->u.body, flags);
		break;
	case SH_NODE_IF:
		status = eval_if(node, flags);
		break;
	case SH_NODE_WHILE:
	case SH_NODE_UNTIL:
		status = eval_loop(node, flags);
		break;
	case SH_NODE_FOR:
		status = eval_for(node, flags);
		break;
	case SH_NODE_CASE:
		status = eval_case(node, flags);
		break;
	case SH_NODE_COND:
		sh_command_line = node->line;
		status = sh_eval_cond(node->u.cond);
		break;
	case SH_NODE_ARITH:
		status = eval_arith(node->u.arith);
		break;
	case SH_NODE_ARITH_FOR:
		status = eval_arith_for(node, flags);
		break;
	default:
		status = sh_eval(node, flags);
		break;
	}

	/* Succeeded: the status. */
	return status;
}

/*
 * Runs (( expression )): status 0 when the value is not 0, 1 when it is,
 * and 1 after an error in the expression (which does not end the shell).
 */
static int
eval_arith(
	const char *text)
{
	long value;
	int ok;

	/* The expression, expanded and evaluated. */
	ok = sh_eval_arith_text(text, &value);
	if (!ok)
		return 1;

	/* Succeeded. */
	return value != 0 ? 0 : 1;
}

/* Reports whether an expression of for (( )) is left out (only blanks). */
static int
arith_blank(
	const char *text)
{
	/* Blanks only. */
	while (*text == ' ' || *text == '\t' || *text == '\n')
		text++;
	return *text == '\0';
}

/*
 * Runs for (( init; test; step )) body: init once, then the body while
 * test is not 0 (an empty test is true), step after each pass.
 */
static int
eval_arith_for(
	struct sh_node *node,
	int flags)
{
	long value;
	int status;
	int stop;
	int ok;

	/* The first expression. */
	status = 0;
	if (!arith_blank(node->u.arith_for.init) &&
	    !sh_eval_arith_text(node->u.arith_for.init, &value))
		return 1;

	/* The loop. */
	sh_loop_nest++;
	for (;;) {
		if (!arith_blank(node->u.arith_for.test)) {
			ok = sh_eval_arith_text(node->u.arith_for.test, &value);
			if (!ok) {
				status = 1;
				break;
			}
			if (value == 0)
				break;
		}

		/* The body; break and continue count here. */
		status = sh_eval(node->u.arith_for.body, flags & ~SH_EV_EXIT);
		if (sh_skip != SH_SKIP_NONE) {
			stop = loop_should_stop();
			if (stop)
				break;
		}

		/* The step. */
		if (!arith_blank(node->u.arith_for.step) &&
		    !sh_eval_arith_text(node->u.arith_for.step, &value)) {
			status = 1;
			break;
		}
	}
	sh_loop_nest--;

	/* Succeeded: the status of the last pass of the body. */
	return status;
}

/* Runs an if command. */
static int
eval_if(
	struct sh_node *node,
	int flags)
{
	int status;

	/* The condition is tested; a break or a return in it stops here. */
	status = sh_eval(node->u.branch.condition, SH_EV_TESTED);
	if (sh_skip != SH_SKIP_NONE)
		return status;

	/* The part the condition selects; with no else, 0. */
	if (status == 0)
		status = sh_eval(node->u.branch.then_part, flags);
	else if (node->u.branch.else_part != NULL)
		status = sh_eval(node->u.branch.else_part, flags);
	else
		status = 0;

	/* Succeeded: the status of the part that ran. */
	return status;
}

/* Runs a while or an until loop. */
static int
eval_loop(
	struct sh_node *node,
	int flags)
{
	int status;
	int condition;
	int stop;

	/* Runs the body while the condition says so. */
	status = 0;
	sh_loop_nest++;
	for (;;) {
		/* The condition; a break or a continue in it counts here. */
		condition = sh_eval(node->u.loop.condition, SH_EV_TESTED);
		if (sh_skip != SH_SKIP_NONE) {
			stop = loop_should_stop();
			if (stop)
				break;
			continue;
		}

		/* while stops at a failure, until at a success. */
		if (node->kind == SH_NODE_WHILE && condition != 0)
			break;
		if (node->kind == SH_NODE_UNTIL && condition == 0)
			break;

		/* The body. */
		status = sh_eval(node->u.loop.body, flags & ~SH_EV_EXIT);
		if (sh_skip != SH_SKIP_NONE) {
			stop = loop_should_stop();
			if (stop)
				break;
		}
	}

	/* The loop is left. */
	sh_loop_nest--;

	/* Succeeded: the status of the last pass of the body. */
	return status;
}

/* Runs a for loop. */
static int
eval_for(
	struct sh_node *node,
	int flags)
{
	struct word_list values;
	size_t mark;
	size_t index;
	int status;
	int count;
	int set;
	int stop;

	/* The words after in, expanded, or the positional parameters. */
	mark = sh_temp_mark();
	memset(&values, 0, sizeof(values));
	sh_command_line = node->line;
	if (node->u.iterate.has_in) {
		expand_words(node->u.iterate.words, node->u.iterate.word_count,
			     &values, 0);
	} else {
		for (count = 0; count < sh_parameters.count; count++)
			list_add(&values, sh_temp_own(sh_strdup(sh_parameters.values[count])));
	}

	/* Runs the body once for each value. */
	status = 0;
	sh_loop_nest++;
	for (index = 0; index < values.count; index++) {
		set = sh_var_set(node->u.iterate.name->raw, values.words[index],
				 0);
		if (set != 0)
			sh_error("%s: is read only", node->u.iterate.name->raw);
		status = sh_eval(node->u.iterate.body, flags & ~SH_EV_EXIT);
		if (sh_skip != SH_SKIP_NONE) {
			stop = loop_should_stop();
			if (stop)
				break;
		}
	}

	/* The loop is left, and its temporary memory freed. */
	sh_loop_nest--;
	sh_temp_release(mark);

	/* Succeeded: the status of the last pass of the body. */
	return status;
}

/* Runs a case command. */
static int
eval_case(
	struct sh_node *node,
	int flags)
{
	struct sh_expand_context context;
	struct sh_case_item *item;
	const char *error_text;
	char *word;
	int expanded;
	int matched;
	int status;

	/* The word is expanded without splitting. */
	sh_command_line = node->line;
	sh_expand_context_fill(&context);
	expanded = sh_expand_word(node->u.select.word, &context, &word,
				  &error_text);
	if (!expanded)
		expansion_error(error_text);
	sh_temp_own(word);

	/* The first arm with a matching pattern runs. */
	for (item = node->u.select.items; item != NULL; item = item->next) {
		matched = case_matches(item, word);
		if (!matched)
			continue;
		if (item->body == NULL)
			return 0;
		status = sh_eval(item->body, flags);
		return status;
	}

	/* No arm matched. */
	return 0;
}

/* Reports whether any pattern of a case arm matches the word. */
static int
case_matches(
	struct sh_case_item *item,
	const char *word)
{
	struct sh_expand_context context;
	const char *error_text;
	unsigned char *quoted;
	char *pattern;
	size_t index;
	int expanded;
	int matched;

	/* Each pattern, expanded with its quoted characters marked. */
	for (index = 0; index < item->pattern_count; index++) {
		sh_expand_context_fill(&context);
		expanded = sh_expand_pattern(item->patterns[index], &context,
					     &pattern, &quoted, &error_text);
		if (!expanded)
			expansion_error(error_text);
		matched = sh_glob_match(pattern, quoted, word);
		free(pattern);
		free(quoted);
		if (matched)
			return 1;
	}

	/* No pattern matched. */
	return 0;
}

/*
 * Reports whether a loop stops for the break, continue or return that is
 * being carried out, consuming the level the loop answers for.
 */
static int
loop_should_stop(
	void)
{
	/* A continue for this loop goes on with the next pass. */
	if (sh_skip == SH_SKIP_CONTINUE) {
		sh_skip_count--;
		if (sh_skip_count <= 0) {
			sh_skip = SH_SKIP_NONE;
			return 0;
		}

		/* Still more levels to leave. */
		return 1;
	}

	/* A break ends this loop, and the outer ones it counts. */
	if (sh_skip == SH_SKIP_BREAK) {
		sh_skip_count--;
		if (sh_skip_count <= 0)
			sh_skip = SH_SKIP_NONE;
		return 1;
	}

	/* A return ends every loop of the function. */
	return 1;
}

/*
 * Expands words into fields: parameter expansion, command substitution,
 * arithmetic, field splitting and pathname expansion.  With detect, once the
 * command name is known (past any "command" and its options) and it is a
 * declaration utility, a later word that is an assignment is expanded as
 * one: no splitting, no pathname expansion, the tilde of an assignment.
 */
static void
expand_words(
	struct sh_token **words,
	size_t count,
	struct word_list *list,
	int detect)
{
	size_t index;
	int declaration;
	int assignment;

	/* Expands each word in turn. */
	declaration = 0;
	for (index = 0; index < count; index++) {
		assignment = 0;
		if (declaration)
			assignment = sh_token_is_assignment(words[index]);
		expand_one(words[index], list, assignment);

		/* Once the name is there, whether it declares is known. */
		if (detect && list->count > 0)
			declaration = declaration_named(list, &detect);
	}

	/* The list ends with NULL, as an argument vector does. */
	list_add(list, NULL);
	list->count--;
}

/* Expands one word into the list, as fields or as an assignment. */
static void
expand_one(
	struct sh_token *word,
	struct word_list *list,
	int as_assignment)
{
	struct sh_expand_context context;
	struct sh_field_list fields;
	const char *error_text;
	char *expanded;
	size_t field;
	int ok;

	/* An assignment operand is one word. */
	sh_expand_context_fill(&context);
	if (as_assignment) {
		ok = sh_expand_word(word, &context, &expanded, &error_text);
		if (!ok)
			expansion_error(error_text);
		list_add(list, sh_temp_own(expanded));
		return;
	}

	/* Anything else is fields, then pathname expansion. */
	memset(&fields, 0, sizeof(fields));
	ok = sh_expand_fields(word, &context, &fields, &error_text);
	if (!ok)
		expansion_error(error_text);
	if (!sh_option[SH_OPT_NOGLOB]) {
		ok = sh_glob_fields(&fields, &error_text);
		if (!ok) {
			sh_fields_free(&fields);
			sh_error("%s", error_text);
		}
	}

	/* The fields become the list's, freed with the command. */
	for (field = 0; field < fields.count; field++) {
		list_add(list, sh_temp_own(fields.fields[field]));
		fields.fields[field] = NULL;
	}

	/* The field array itself goes. */
	sh_fields_free(&fields);
}

/*
 * Once the command name is among the words expanded so far, says whether it
 * is a declaration utility (and stops looking, through *detect).
 */
static int
declaration_named(
	struct word_list *list,
	int *detect)
{
	const struct sh_builtin *builtin;
	struct sh_function *function;
	size_t name;
	int compare;

	/* The name comes after any "command" and its options. */
	name = 0;
	while (name < list->count) {
		compare = strcmp(list->words[name], "command");
		if (compare != 0 && !(name > 0 && list->words[name][0] == '-'))
			break;
		name++;
	}

	/* Only options followed command. */
	if (name >= list->count)
		return 0;

	/* The name is known: a declaration builtin, not shadowed by a function. */
	*detect = 0;
	builtin = sh_builtin_find(list->words[name]);
	if (builtin == NULL || (builtin->flags & SH_BUILTIN_DECLARATION) == 0)
		return 0;
	function = sh_function_find(list->words[name]);
	if (function != NULL)
		return 0;

	/* Succeeded: later assignments are declared. */
	return 1;
}

/* Expands an assignment word into "name=value". */
static char *
expand_assignment(
	struct sh_token *word)
{
	struct sh_expand_context context;
	const char *error_text;
	char *expanded;
	int ok;

	/* No splitting, no pathname expansion, and the tilde of an assignment. */
	sh_expand_context_fill(&context);
	ok = sh_expand_word(word, &context, &expanded, &error_text);
	if (!ok)
		expansion_error(error_text);
	sh_temp_own(expanded);

	/* Succeeded: the assignment, freed with the command. */
	return expanded;
}

/* Throws an expansion's error. */
static void
expansion_error(
	const char *text)
{
	/* Every expansion error ends a shell that is not interactive. */
	sh_error("%s", text);
}

/* Adds a word to a list. */
static void
list_add(
	struct word_list *list,
	char *word)
{
	char **grown;
	size_t capacity;

	/*
	 * Grows the list in powers of two, into a new temporary array: the
	 * old one stays registered, and goes with the command.
	 */
	if (list->count == list->capacity) {
		if (list->capacity == 0)
			capacity = 8U;
		else
			capacity = list->capacity * 2U;
		grown = sh_temp_own(sh_malloc(capacity * sizeof(*grown)));
		if (list->count > 0)
			memcpy(grown, list->words, list->count * sizeof(*grown));
		list->words = grown;
		list->capacity = capacity;
	}

	/* Appends the word. */
	list->words[list->count++] = word;
}

/* Calls a function with argv as its positional parameters. */
static int
call_function(
	struct sh_function *function,
	int argc,
	char **argv,
	int flags)
{
	struct sh_parameters saved_parameters;
	struct sh_arena *saved_arena;
	struct sh_arena *arena;
	struct sh_node *body;
	struct sh_handler handler;
	int kind;
	int status;
	int saved_loop_nest;

	/* The call holds the function's block, in case it is redefined. */
	arena = function->arena;
	body = function->body;
	if (arena != NULL)
		sh_arena_hold(arena);
	saved_arena = current_arena;
	current_arena = arena;

	/* The arguments become the positional parameters. */
	saved_parameters = sh_parameters;
	sh_parameters.count = 0;
	sh_parameters.values = NULL;
	sh_parameters.owned = 0;
	sh_parameters_set(argc - 1, argv + 1);
	saved_loop_nest = sh_loop_nest;

	/* An exception out of the function puts the caller's state back. */
	sh_handler_push(&handler);
	kind = setjmp(handler.environment);
	if (kind != 0) {
		sh_handler_unwind(&handler);
		sh_parameters_free(&sh_parameters);
		sh_parameters = saved_parameters;
		current_arena = saved_arena;
		sh_function_nest--;
		sh_loop_nest = saved_loop_nest;
		sh_arena_release(arena);
		sh_raise(kind);
	}

	/* Runs the body; a return ends it with its status. */
	sh_function_nest++;
	sh_loop_nest = 0;
	status = sh_eval(body, flags & SH_EV_TESTED);
	if (sh_skip == SH_SKIP_RETURN) {
		sh_skip = SH_SKIP_NONE;
		status = sh_status;
	}

	/* The function is left. */
	sh_function_nest--;
	sh_loop_nest = saved_loop_nest;
	sh_handler_pop(&handler);

	/* Puts the caller's parameters and block back. */
	sh_parameters_free(&sh_parameters);
	sh_parameters = saved_parameters;
	current_arena = saved_arena;
	sh_arena_release(arena);

	/* Succeeded: the function's status. */
	return status;
}

/* Makes assignments for good; a read-only variable is an error. */
static void
assign_permanently(
	char **assignments,
	size_t count,
	int flags)
{
	size_t index;
	char *equals;
	int set;

	/* Each assignment in the order written. */
	for (index = 0; index < count; index++) {
		set = sh_var_set_assignment(assignments[index], flags);
		if (set == 0)
			continue;
		equals = strchr(assignments[index], '=');
		*equals = '\0';
		sh_error("%s: is read only", assignments[index]);
	}
}

/*
 * Makes assignments for the length of a builtin or a function: each name is
 * made local to the frame the caller pushed, and exported.
 */
static void
assign_temporarily(
	char **assignments,
	size_t count)
{
	size_t index;
	char *equals;
	char *name;
	int flags;

	/* Saves each name, then sets it; a read-only one is an error. */
	for (index = 0; index < count; index++) {
		equals = strchr(assignments[index], '=');
		name = sh_temp_own(sh_strndup(assignments[index],
					      (size_t)(equals - assignments[index])));
		flags = sh_var_flags(name);
		if (flags >= 0 && (flags & SH_VAR_READONLY) != 0)
			sh_error("%s: is read only", name);
		(void)sh_var_make_local(name);
		(void)sh_var_set(name, equals + 1, SH_VAR_EXPORT);
	}
}

/*
 * Reads everything from a descriptor into a new buffer, leaving out NUL
 * bytes (a word cannot hold one; dash drops them too).
 */
static char *
read_output(
	int descriptor,
	size_t *length)
{
	char *output;
	size_t used;
	size_t start;
	size_t capacity;
	size_t index;
	ssize_t count;

	/* Reads until the end, growing the buffer. */
	capacity = 256;
	used = 0;
	output = sh_malloc(capacity);
	for (;;) {
		if (used + 256U > capacity) {
			capacity *= 2U;
			output = sh_realloc(output, capacity);
		}

		/* The next chunk of the output. */
		count = read(descriptor, output + used, capacity - used - 1U);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;

		/* Keeps the bytes of this read that are not NUL. */
		start = used;
		for (index = start; index < start + (size_t)count; index++) {
			if (output[index] != '\0')
				output[used++] = output[index];
		}
	}

	/* The length of the output. */
	*length = used;

	/* Succeeded: the output, which the caller frees. */
	return output;
}

/* Adds bytes to a trace line. */
static void
trace_append(
	struct trace_buffer *buffer,
	const char *text,
	size_t length)
{
	size_t capacity;

	/* Grows in powers of two, keeping room for a terminator. */
	if (buffer->length + length + 1U > buffer->capacity) {
		capacity = 128U;
		if (buffer->capacity != 0)
			capacity = buffer->capacity;
		while (buffer->length + length + 1U > capacity)
			capacity *= 2U;
		buffer->text = sh_realloc(buffer->text, capacity);
		buffer->capacity = capacity;
	}

	/* Appends the bytes. */
	memcpy(buffer->text + buffer->length, text, length);
	buffer->length += length;
}

/* Adds one traced word, in single quotes when it needs them. */
static void
xtrace_word(
	struct trace_buffer *buffer,
	const char *word)
{
	const char *cursor;
	int plain;

	/* Ordinary characters go bare. */
	plain = is_plain_word(word);
	if (plain) {
		trace_append(buffer, word, strlen(word));
		return;
	}

	/* Others go in single quotes, each ' written as '\''. */
	trace_append(buffer, "'", 1);
	for (cursor = word; *cursor != '\0'; cursor++) {
		if (*cursor == '\'')
			trace_append(buffer, "'\\''", 4);
		else
			trace_append(buffer, cursor, 1);
	}

	/* The closing quote. */
	trace_append(buffer, "'", 1);
}

/* Reports whether a word needs no quotes to be read back as itself. */
static int
is_plain_word(
	const char *word)
{
	const char *cursor;
	const char *found;

	/* An empty word needs quotes. */
	if (*word == '\0')
		return 0;

	/* Every character must be one that means nothing to the shell. */
	for (cursor = word; *cursor != '\0'; cursor++) {
		found = strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
			       "0123456789_/.,:+-=@%^", *cursor);
		if (found == NULL)
			return 0;
	}

	/* Succeeded: it is plain. */
	return 1;
}

/* Reads a variable for expansion. */
static const char *
expand_lookup(
	void *context,
	const char *name)
{
	const char *value;

	/* The variable table answers. */
	(void)context;
	value = sh_var_get(name);

	/* Succeeded: the value, or NULL when unset. */
	return value;
}

/* Sets a variable for expansion: ${name=word} and arithmetic. */
static int
expand_assign(
	void *context,
	const char *name,
	const char *value)
{
	int set;

	/* A read-only variable refuses. */
	(void)context;
	set = sh_var_set(name, value, 0);
	if (set != 0)
		return 0;

	/* Succeeded (nonzero). */
	return 1;
}

/*
 * Runs a process substitution: <( commands ) writes into a pipe whose
 * reading end the shell keeps, >( commands ) reads from one whose writing
 * end it keeps; the word is /dev/fd/N of that end, open until the command
 * it was written in ends (bash).
 */
static int
expand_process_substitution(
	void *context,
	const struct sh_token *token,
	char **result)
{
	struct process_substitution *entry;
	char *text;
	char path[32];
	pid_t child;
	int descriptors[2];
	int kept;

	/* The commands, between the parentheses. */
	(void)context;
	text = sh_temp_own(sh_malloc(token->raw_length));
	memcpy(text, token->raw + 2, token->raw_length - 3U);
	text[token->raw_length - 3U] = '\0';

	/* A pipe, and the child that runs the commands on one end of it. */
	if (pipe(descriptors) != 0)
		sh_error("cannot create pipe: %s", strerror(errno));
	child = sh_fork(SH_FORK_NO_JOB, NULL);
	if (child == 0) {
		if (token->process == '<') {
			(void)close(descriptors[0]);
			(void)dup2(descriptors[1], 1);
			(void)close(descriptors[1]);
		} else {
			(void)close(descriptors[1]);
			(void)dup2(descriptors[0], 0);
			(void)close(descriptors[0]);
		}
		(void)sh_eval_string(text, SH_EV_EXIT);
		sh_exit(sh_status);
	}

	/* The shell keeps the other end. */
	if (token->process == '<') {
		kept = descriptors[0];
		(void)close(descriptors[1]);
	} else {
		kept = descriptors[1];
		(void)close(descriptors[0]);
	}

	/* Remembered, to be closed when the command ends. */
	if (process_substitution_count == process_substitution_capacity) {
		process_substitution_capacity = process_substitution_capacity == 0 ?
		    4 : process_substitution_capacity * 2U;
		process_substitutions = sh_realloc(process_substitutions,
		    process_substitution_capacity * sizeof(*process_substitutions));
	}
	entry = &process_substitutions[process_substitution_count++];
	entry->descriptor = kept;
	entry->child = child;

	/* Succeeded: the name of the kept end. */
	snprintf(path, sizeof(path), "/dev/fd/%d", kept);
	*result = sh_strdup(path);
	return 1;
}

/*
 * Closes the pipes of the process substitutions newer than mark, and
 * reaps the children that have ended (the others are reaped later).
 */
static void
process_substitutions_close(
	size_t mark)
{
	struct process_substitution *entry;
	size_t index;
	size_t kept;
	pid_t done;
	int status;

	/* Each newer one: its pipe closes and its child is to be reaped. */
	while (process_substitution_count > mark) {
		entry = &process_substitutions[--process_substitution_count];
		(void)close(entry->descriptor);
		process_children = sh_realloc(process_children,
		    (process_child_count + 1U) * sizeof(*process_children));
		process_children[process_child_count++] = entry->child;
	}

	/* The children that have ended. */
	kept = 0;
	for (index = 0; index < process_child_count; index++) {
		done = waitpid(process_children[index], &status, WNOHANG);
		if (done == 0)
			process_children[kept++] = process_children[index];
	}
	process_child_count = kept;
}

/* Runs a command substitution for expansion. */
static int
expand_substitute(
	void *context,
	const char *text,
	char **result)
{
	int ran;

	/* The subshell's output is the result. */
	(void)context;
	ran = sh_run_command_substitution(text, result);

	/* Succeeded when it ran. */
	return ran;
}
