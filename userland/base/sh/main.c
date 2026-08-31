/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD sh userland command.
 */

#include "userland/base/sh/alias.h"
#include "userland/base/sh/builtins.h"
#include "userland/base/sh/expand.h"
#include "userland/base/sh/glob.h"
#include "userland/base/sh/lexer.h"
#include "userland/base/sh/vars.h"

#include <errno.h>
#include <fcntl.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define SHELL_LINE_MAX 256
#define ARG_MAX 64
#define SOURCE_MAX 8192
#define PIPELINE_MAX 16
#define SHELL_SIGNAL_MAX 32

static int command_background;
static int command_subshell;
static pid_t last_job;
static pid_t last_job_processes[PIPELINE_MAX];
static int last_job_process_count;
static const char *shell_name = "/bin/sh";
static int shell_positional_count;
static char **shell_positional;
static char *trap_action[SHELL_SIGNAL_MAX];
static volatile int trap_pending[SHELL_SIGNAL_MAX];
static int getopts_offset = 1;
static long getopts_last_index = 1;

struct pipeline_command {
	char *argv[ARG_MAX + 1];
	int argc;
	char *input;
	char *output;
	int append;
};

static int command(char *text);
static int run_pending_traps(void);
static int parse_pipeline(const struct sh_token_list *list, size_t *position, struct pipeline_command *items, int *item_count, enum sh_token_type *following, const struct sh_expand_context *context);
static int assignment_length(const char *text);
static void pipeline_free(struct pipeline_command *items, int count);
static int execute_pipeline(struct pipeline_command *items, int count, int background);
static int execute_parent_command(struct pipeline_command *item);
static int command_argv(int argc, char **argv);
static int special_builtin_name(const char *name);
static int apply_assignment(char *text);
static int temporary_assignment(char *text, struct sh_var_snapshot *snapshot);
static int command_dispatch(int argc, char **argv);
static int continue_foreground(pid_t pid, int *status);
static int shell_tcsetpgrp(int descriptor, pid_t pgrp);
static void remember_job(pid_t group, const pid_t *processes, int count);
static void forget_job(void);
static int resolve_command(const char *name, char *candidate, size_t capacity);
static int search_path(const char *name, const char *suffix, char *candidate, size_t capacity);
static int path_candidate(const char *path, size_t *position, const char *name, const char *suffix, char *candidate, size_t capacity, int *last);
static int is_executable_file(const char *path);
static int shell_getopts_builtin(int argc, char **argv);
static int set_decimal_variable(const char *name, long value);
static int signal_number(const char *name);
static int set_trap(const char *action, int number);
static int source_file(const char *path);
static int source_file_mode(const char *path, int continue_on_error);
static int join_arguments(int argc, char **argv, int first, char **result);
static int read_line(char *buffer, size_t capacity);
static int shell_wait_builtin(int argc, char **argv);
static int shell_builtin_name(const char *name);
static int is_elf(const char *path);
static int run_external(char *const argv[]);
static int spawn_wait(char *const argv[]);
static int spawn_foreground_tty(char *const argv[], int *status);
static ssize_t shell_write_nosigpipe(int descriptor, const void *buffer, size_t length);
static void remember_single_job(pid_t process);
static const char *signal_message(int number);
static int wait_foreground(pid_t pid, int *status);
static int run_shell_script(int argc, char **argv, const char *path);
static int run_search_path(int argc, char **argv);
static int run_resolved(int argc, char **argv, const char *path);
static int pipeline_child(struct pipeline_command *item);
static const char *shell_lookup(void *context, const char *name);
static int shell_assign(void *context, const char *name, const char *value);
static void shell_signal_handler(int signal_number);
static int shell_command_substitute(void *context, const char *source, char **result);

/*
 * Runs the sh command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	char cwd[256];
	char hostname[65];
	char prompt[sizeof(cwd) + sizeof(hostname) + 16U];
	char *line;

	/* Handles a failed sh var get operation. */
	if (sh_var_get("PATH") == NULL)
		(void)sh_var_set("PATH", "/bin:/sbin:/usr/bin", 1);

	/* Handles a failed sh var get operation. */
	if (sh_var_get("TERM") == NULL)
		(void)sh_var_set("TERM", "zed", 1);

	/* Handles the selected command-line operation. */
	if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
		shell_name = argc >= 4 ? argv[3] : argv[0];
		shell_positional_count = argc >= 4 ? argc - 4 : 0;
		shell_positional = argc >= 4 ? argv + 4 : NULL;

		/* Computes the function result. */
		function_result = command(argv[2]) ? 0 : 1;

		/* Returns the computed result. */
		return function_result;
	}

	/* Validates the command-line arguments. */
	if (argc > 1) {
		shell_name = argv[1];
		shell_positional_count = argc - 2;
		shell_positional = argv + 2;

		/* Computes the function result. */
		function_result = source_file_mode(argv[1], 0) ? 0 : 1;

		/* Returns the computed result. */
		return function_result;
	}

	/* Validates the command-line arguments. */
	if (argc > 0 && argv[0] != NULL)
		shell_name = argv[0];
	using_history();

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed getcwd operation. */
		if (getcwd(cwd, sizeof(cwd)) == NULL)
			strcpy(cwd, "/");

		/* Handles a failed gethostname operation. */
		if (gethostname(hostname, sizeof(hostname)) != 0)
			strcpy(hostname, "zedbsd");
		(void)snprintf(prompt, sizeof(prompt), "root@%s:%s$ ", hostname,
			       cwd);
		line = readline(prompt);

		/* Handles the line availability. */
		if (line == NULL) {
			(void)putchar('\n');

			/* Reports successful completion. */
			return 0;
		}

		/* Handles the line condition. */
		if (line[0] != '\0')
			add_history(line);
		(void)command(line);
		free(line);
	}
}

/* Supports the command operation. */
static int
command(
	char *text)
{
	struct pipeline_command items[PIPELINE_MAX];
	struct sh_expand_context context;
	enum sh_token_type next;
	int item_count;
	int execute;
	struct sh_token_list list;
	const char *error_text;
	enum sh_token_type connector;
	size_t index;
	int result;
	int any;

	connector = SH_TOKEN_SEMI;
	index = 0;
	result = 1;
	any = 0;

	/* Handles an operation failure. */
	if (!sh_lex(text, &list, &error_text)) {
		fprintf(stderr, "sh: syntax error: %s\n", error_text);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles an operation failure. */
	if (!sh_alias_expand(&list, &error_text)) {
		fprintf(stderr, "sh: alias: %s\n", error_text);
		sh_tokens_free(&list);

		/* Reports successful completion. */
		return 0;
	}
	while (list.tokens[index].type != SH_TOKEN_END) {
		/* Handles a failed run pending traps operation. */
		if (!run_pending_traps())
			result = 0;
		context.status = result ? 0 : 1;
		context.shell_pid = (long)getpid();
		context.last_job = (long)last_job;
		context.lookup = shell_lookup;
		context.assign = shell_assign;
		context.command_substitute = shell_command_substitute;
		context.lookup_context = NULL;
		context.shell_name = shell_name;
		context.positional_count = shell_positional_count;
		context.positional = shell_positional;

		/* Handles a failed parse pipeline operation. */
		if (!parse_pipeline(&list, &index, items, &item_count, &next,
				    &context)) {
			result = 0;
			goto done;
		}

		/* Handles the next condition. */
		if (next != SH_TOKEN_END && next != SH_TOKEN_SEMI &&
		    next != SH_TOKEN_AMP && next != SH_TOKEN_AND_IF &&
		    next != SH_TOKEN_OR_IF) {
			fprintf(stderr, "sh: invalid operator\n");
			pipeline_free(items, item_count);
			result = 0;
			goto done;
		}
		execute = connector == SH_TOKEN_SEMI ||
			  connector == SH_TOKEN_AMP ||
			  (connector == SH_TOKEN_AND_IF && result) ||
			  (connector == SH_TOKEN_OR_IF && !result);

		/* Handles the execute condition. */
		if (execute) {
			result = execute_pipeline(items, item_count,
						  next == SH_TOKEN_AMP);
			any = 1;
		}
		pipeline_free(items, item_count);
		connector = next;

		/* Handles the next condition. */
		if (next == SH_TOKEN_END)
			break;
		index++;

		/* Handles the list condition. */
		if (list.tokens[index].type == SH_TOKEN_END &&
		    (next == SH_TOKEN_AND_IF || next == SH_TOKEN_OR_IF)) {
			fprintf(stderr, "sh: syntax error after operator\n");
			result = 0;
			goto done;
		}
	}

	/* Handles the any condition. */
	if (!any)
		result = 1;

	/* Handles a failed run pending traps operation. */
	if (!run_pending_traps())
		result = 0;
done:
	command_background = 0;
	sh_tokens_free(&list);

	/* Returns the computed result. */
	return result;
}

/* Supports the run pending traps operation. */
static int
run_pending_traps(
	void)
{
	char *action;
	int number;
	int result;

	/* Process each element required by the operation. */
	result = 1;
	for (number = 1; number < SHELL_SIGNAL_MAX; number++) {
		/* Handles the trap pending condition. */
		if (!trap_pending[number] || trap_action[number] == NULL)
			continue;
		trap_pending[number] = 0;
		action = malloc(strlen(trap_action[number]) + 1U);

		/* Handles the action availability. */
		if (action == NULL)
			return 0;
		strcpy(action, trap_action[number]);

		/* Handles a failed command operation. */
		if (!command(action))
			result = 0;
		free(action);
	}

	/* Returns the computed result. */
	return result;
}

/* Supports the parse pipeline operation. */
static int
parse_pipeline(
	const struct sh_token_list *list,
	size_t *position,
	struct pipeline_command *items,
	int *item_count,
	enum sh_token_type *following,
	const struct sh_expand_context *context)
{
	int assignment;
	struct sh_field_list fields_local;
	struct sh_field_list fields_local1;
	char *word;
	size_t field;
	enum sh_token_type type;
	int count;
	struct pipeline_command *item;
	const char *error_text;

	count = 1;
	item = &items[0];
	memset(items, 0, PIPELINE_MAX * sizeof(*items));

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
				type = list->tokens[*position].type;

		/* Handles the type condition. */
		if (type == SH_TOKEN_WORD) {

						assignment = assignment_length(
					     list->tokens[*position].text) >= 0;

			/* Handles the assignment condition. */
			if (assignment) {
				/* Handles an operation failure. */
				if (!sh_expand_word(&list->tokens[*position],
						    context, &word,
						    &error_text)) {
					fprintf(stderr, "sh: expansion: %s\n",
						error_text);
					pipeline_free(items, count);

					/* Reports successful completion. */
					return 0;
				}
				memset(&fields_local, 0, sizeof(fields_local));
				fields_local.fields = malloc(sizeof(*fields_local.fields));
				fields_local.quoted =
				    calloc(1, sizeof(*fields_local.quoted));

				/* Handles the fields availability. */
				if (fields_local.fields == NULL ||
				    fields_local.quoted == NULL) {
					free(word);
					free(fields_local.fields);
					free(fields_local.quoted);
					pipeline_free(items, count);

					/* Reports successful completion. */
					return 0;
				}
				fields_local.fields[0] = word;
				fields_local.quoted[0] = NULL;
				fields_local.count = 1;
			} else if (!sh_expand_fields(&list->tokens[*position],
						     context, &fields_local,
						     &error_text)) {
				fprintf(stderr, "sh: expansion: %s\n",
					error_text);
				pipeline_free(items, count);

				/* Reports successful completion. */
				return 0;
			}

			/* Handles an operation failure. */
			if (!assignment &&
			    !sh_glob_fields(&fields_local, &error_text)) {
				fprintf(stderr, "sh: pathname expansion: %s\n",
					error_text);
				sh_fields_free(&fields_local);
				pipeline_free(items, count);

				/* Reports successful completion. */
				return 0;
			}

			/* Validates the command-line arguments. */
			if (fields_local.count > (size_t)(ARG_MAX - item->argc)) {
				fprintf(stderr, "sh: too many arguments\n");
				sh_fields_free(&fields_local);
				pipeline_free(items, count);

				/* Reports successful completion. */
				return 0;
			}

			/* Process each remaining element. */
			for (field = 0; field < fields_local.count; field++) {
				item->argv[item->argc++] = fields_local.fields[field];
				free(fields_local.quoted[field]);
			}
			free(fields_local.fields);
			free(fields_local.quoted);
			(*position)++;
			continue;
		}

		/* Handles the type condition. */
		if (type == SH_TOKEN_INPUT || type == SH_TOKEN_OUTPUT ||
		    type == SH_TOKEN_APPEND) {

			(*position)++;

			/* Handles the list condition. */
			if (list->tokens[*position].type != SH_TOKEN_WORD) {
				fprintf(stderr,
					"sh: redirection requires a path\n");
				pipeline_free(items, count);

				/* Reports successful completion. */
				return 0;
			}

			/* Handles an operation failure. */
			if (!sh_expand_fields(&list->tokens[*position], context,
					      &fields_local1, &error_text)) {
				fprintf(stderr, "sh: expansion: %s\n",
					error_text);
				pipeline_free(items, count);

				/* Reports successful completion. */
				return 0;
			}

			/* Handles an operation failure. */
			if (!sh_glob_fields(&fields_local1, &error_text)) {
				fprintf(stderr, "sh: pathname expansion: %s\n",
					error_text);
				sh_fields_free(&fields_local1);
				pipeline_free(items, count);

				/* Reports successful completion. */
				return 0;
			}
			(*position)++;

			/* Handles the fields local1 condition. */
			if (fields_local1.count != 1) {
				fprintf(stderr, "sh: ambiguous redirection\n");
				sh_fields_free(&fields_local1);
				pipeline_free(items, count);

				/* Reports successful completion. */
				return 0;
			}

			/* Handles the type condition. */
			if (type == SH_TOKEN_INPUT) {
				free(item->input);
				item->input = fields_local1.fields[0];
			} else {
				free(item->output);
				item->output = fields_local1.fields[0];
				item->append = type == SH_TOKEN_APPEND;
			}
			free(fields_local1.quoted[0]);
			free(fields_local1.fields);
			free(fields_local1.quoted);
			continue;
		}

		/* Validates the command-line arguments. */
		if (item->argc == 0) {
			fprintf(stderr, "sh: empty pipeline command\n");
			pipeline_free(items, count);

			/* Reports successful completion. */
			return 0;
		}
		item->argv[item->argc] = NULL;

		/* Handles the type condition. */
		if (type != SH_TOKEN_PIPE) {
			*following = type;
			*item_count = count;
			/* Reports operation failure. */
			return 1;
		}

		/* Checks the remaining item count. */
		if (count == PIPELINE_MAX) {
			fprintf(stderr, "sh: pipeline is too long\n");
			pipeline_free(items, count);

			/* Reports successful completion. */
			return 0;
		}
		(*position)++;
		item = &items[count++];
	}
}

/* Supports the assignment length operation. */
static int
assignment_length(
	const char *text)
{
	const char *cursor;

	cursor = text;

	/* Checks the current cursor position. */
	if (!((*cursor >= 'A' && *cursor <= 'Z') ||
	      (*cursor >= 'a' && *cursor <= 'z') || *cursor == '_'))

		/* Reports operation failure. */
		return -1;
	cursor++;

	/* Continue while the operation condition remains true. */
	while ((*cursor >= 'A' && *cursor <= 'Z') ||
	       (*cursor >= 'a' && *cursor <= 'z') || *cursor == '_' ||
	       (*cursor >= '0' && *cursor <= '9'))
		cursor++;

	/* Returns the computed result. */
	return *cursor == '=' ? (int)(cursor - text) : -1;
}

/* Supports the pipeline free operation. */
static void
pipeline_free(
	struct pipeline_command *items,
	int count)
{
	int command_index, argument;

	/* Process each remaining element. */
	for (command_index = 0; command_index < count; command_index++) {
		/* Process each remaining command-line operand. */
		for (argument = 0; argument < items[command_index].argc;
		     argument++)
			free(items[command_index].argv[argument]);
		free(items[command_index].input);
		free(items[command_index].output);
	}
}

/* Supports the execute pipeline operation. */
static int
execute_pipeline(
	struct pipeline_command *items,
	int count,
	int background)
{
	int function_result;
	char release_local;
	ssize_t release_count_local;
	char release_local1[PIPELINE_MAX];
	ssize_t release_count_local2;
	pid_t waited_local;
	pid_t waited_local3;
	pid_t child;
	int status;
	pid_t children[PIPELINE_MAX];
	pid_t stopped[PIPELINE_MAX];
	pid_t group;
	pid_t shell_group;
	int terminal;
	int synchronize;
	int gate[2] = {-1, -1};
	int terminal_owned;
	int active[PIPELINE_MAX] = {0};
	int input;
	int index, created;
	int stopped_count;
	int last_status;
	int saved_errno;
	int descriptors[2];

	group = 0;
	shell_group = getpgrp();
	terminal = !command_subshell && isatty(STDIN_FILENO);
	synchronize = terminal && !background;
	terminal_owned = 0;
	input = -1;
	created = 0;
	stopped_count = 0;
	last_status = 0;

	/* Checks the remaining item count. */
	if (count == 1 && !background) {
		/* Obtains the execute parent command result. */
		function_result = execute_parent_command(&items[0]);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed pipe2 operation. */
	if (synchronize && pipe2(gate, O_CLOEXEC) != 0) {
		fprintf(stderr, "sh: pipeline: %s\n", strerror(errno));

		/* Reports successful completion. */
		return 0;
	}
	(void)fflush(NULL);

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		descriptors[0] = -1;
		descriptors[1] = -1;

		/* Handles a failed pipe operation. */
		if (index + 1 < count && pipe(descriptors) != 0)
			goto failed;
		child = fork();

		/* Checks the child process state. */
		if (child < 0) {
			/* Handles the descriptors condition. */
			if (descriptors[0] >= 0)
				(void)close(descriptors[0]);

			/* Handles the descriptors condition. */
			if (descriptors[1] >= 0)
				(void)close(descriptors[1]);
			goto failed;
		}

		/* Checks the child process state. */
		if (child == 0) {
			/* Handles the synchronize condition. */
			if (synchronize)
				(void)close(gate[1]);

			/* Handles a failed setpgid operation. */
			if (setpgid(0, group == 0 ? 0 : group) != 0)
				_exit(126);

			/* Handles the synchronize condition. */
			if (synchronize) {
				do
					release_count_local =
					    read(gate[0], &release_local, 1);

				/* Process each remaining element. */
				while (release_count_local < 0 && errno == EINTR);
				(void)close(gate[0]);

				/* Handles the release count local condition. */
				if (release_count_local != 1 || release_local != 'x')
					_exit(126);
			}

			/* Handles a failed dup2 operation. */
			if (input >= 0 && dup2(input, STDIN_FILENO) < 0)
				_exit(126);

			/* Handles a failed dup2 operation. */
			if (descriptors[1] >= 0 &&
			    dup2(descriptors[1], STDOUT_FILENO) < 0)
				_exit(126);

			/* Validates the current input. */
			if (input >= 0)
				(void)close(input);

			/* Handles the descriptors condition. */
			if (descriptors[0] >= 0)
				(void)close(descriptors[0]);

			/* Handles the descriptors condition. */
			if (descriptors[1] >= 0)
				(void)close(descriptors[1]);

			/* Handles a failed pipeline child operation. */
			if (!pipeline_child(&items[index])) {
				(void)fflush(NULL);
				_exit(1);
			}
			(void)fflush(NULL);
			_exit(0);
		}

		/* Handles the group condition. */
		if (group == 0)
			group = child;
		children[created] = child;
		active[created++] = 1;

		/* Validates the current input. */
		if (input >= 0)
			(void)close(input);

		/* Handles the descriptors condition. */
		if (descriptors[1] >= 0)
			(void)close(descriptors[1]);
		input = descriptors[0];

		/* Handles the synchronize condition. */
		if (synchronize) {
			/* Handles a failed setpgid operation. */
			if (setpgid(child, group) != 0)
				goto failed;

			/* Handles a failed getpgid operation. */
			if (getpgid(child) != group) {
				errno = EPERM;
				goto failed;
			}
		} else {
			(void)setpgid(child, group);
		}
	}

	/* Validates the current input. */
	if (input >= 0)
		(void)close(input);
	input = -1;

	/* Handles the background condition. */
	if (background) {
		remember_job(group, children, created);
		printf("[%d]\n", (int)group);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the synchronize condition. */
	if (synchronize) {

		(void)close(gate[0]);
		gate[0] = -1;

		/* Handles a failed shell tcsetpgrp operation. */
		if (shell_tcsetpgrp(STDIN_FILENO, group) != 0)
			goto failed;
		terminal_owned = 1;
		memset(release_local1, 'x', (size_t)created);
		release_count_local2 =
		    shell_write_nosigpipe(gate[1], release_local1, (size_t)created);

		/* Handles the release count local2 condition. */
		if (release_count_local2 != created) {
			/* Handles the release count local2 condition. */
			if (release_count_local2 >= 0)
				errno = EIO;
			goto failed;
		}
		(void)close(gate[1]);
		gate[1] = -1;
	}

	/* Process each remaining element. */
	for (index = 0; index < created; index++) {

		status = 0;

		do

		/* Continue while the operation condition remains true. */
			waited_local = waitpid(children[index], &status, WUNTRACED);
		while (waited_local < 0 && errno == EINTR);

		/* Handles the waited local condition. */
		if (waited_local < 0) {
			saved_errno = errno;
			goto wait_failed;
		}

		/* Handles the children condition. */
		if (children[index] == children[created - 1])
			last_status = status;

		/* Checks the operation status. */
		if (WIFSTOPPED(status)) {
			stopped[stopped_count++] = children[index];
		} else {
			active[index] = 0;
		}
	}

	/* Handles a failed shell tcsetpgrp operation. */
	if (terminal_owned && shell_tcsetpgrp(STDIN_FILENO, shell_group) != 0)
		fprintf(stderr,
			"sh: cannot restore foreground process group: %s\n",
			strerror(errno));

	/* Handles the stopped count condition. */
	if (stopped_count > 0) {
		remember_job(group, stopped, stopped_count);
		printf("[%d] stopped\n", (int)group);
	}

	/* Computes the function result. */
	function_result = WIFEXITED(last_status) && WEXITSTATUS(last_status) == 0;

	/* Returns the computed result. */
	return function_result;
wait_failed:
	errno = saved_errno;
failed:
	saved_errno = errno;

	/* Validates the current input. */
	if (input >= 0)
		(void)close(input);

	/* Handles the gate condition. */
	if (gate[0] >= 0)
		(void)close(gate[0]);

	/* Handles the gate condition. */
	if (gate[1] >= 0)
		(void)close(gate[1]);

	/* Process each remaining element. */
	for (index = 0; index < created; index++)

		/* Handles the active condition. */
		if (active[index]) {
			(void)kill(-group, SIGKILL);
			break;
		}

	/* Process each remaining element. */
	for (index = 0; index < created; index++)

		/* Handles the active condition. */
		if (active[index])
			(void)kill(children[index], SIGKILL);

	/* Process each remaining element. */
	for (index = 0; index < created; index++) {
		/* Handles the active condition. */
		if (!active[index])
			continue;
		do

		/* Continue while the operation condition remains true. */
			waited_local3 = waitpid(children[index], NULL, 0);
		while (waited_local3 < 0 && errno == EINTR);
	}

	/* Handles the terminal owned condition. */
	if (terminal_owned)
		(void)shell_tcsetpgrp(STDIN_FILENO, shell_group);
	errno = saved_errno;
	fprintf(stderr, "sh: pipeline: %s\n", strerror(saved_errno));

	/* Reports successful completion. */
	return 0;
}

/* Supports the execute parent command operation. */
static int
execute_parent_command(
	struct pipeline_command *item)
{
	int saved_input, saved_output;
	int descriptor;
	int result;

	saved_input = -1;
	saved_output = -1;
	descriptor = -1;
	result = 0;

	/* Handles the input availability. */
	if (item->input != NULL) {
		saved_input = dup(STDIN_FILENO);
		descriptor = open(item->input, O_RDONLY);

		/* Handles a failed dup2 operation. */
		if (saved_input < 0 || descriptor < 0 ||
		    dup2(descriptor, STDIN_FILENO) < 0) {
			fprintf(stderr, "%s: %s\n", item->input,
				strerror(errno));
			goto done;
		}
		(void)close(descriptor);
		descriptor = -1;
	}

	/* Handles the output availability. */
	if (item->output != NULL) {
		(void)fflush(stdout);
		saved_output = dup(STDOUT_FILENO);
		descriptor = open(item->output,
				  O_WRONLY | O_CREAT |
				      (item->append ? O_APPEND : O_TRUNC),
				  0666);

		/* Handles a failed dup2 operation. */
		if (saved_output < 0 || descriptor < 0 ||
		    dup2(descriptor, STDOUT_FILENO) < 0) {
			fprintf(stderr, "%s: %s\n", item->output,
				strerror(errno));
			goto done;
		}
		(void)close(descriptor);
		descriptor = -1;
	}
	command_background = 0;
	result = command_argv(item->argc, item->argv);
done:
	(void)fflush(NULL);

	/* Checks the file descriptor. */
	if (descriptor >= 0)
		(void)close(descriptor);

	/* Handles the saved output condition. */
	if (saved_output >= 0) {
		/* Handles a failed dup2 operation. */
		if (dup2(saved_output, STDOUT_FILENO) < 0)
			result = 0;
		(void)close(saved_output);
	}

	/* Handles the saved input condition. */
	if (saved_input >= 0) {
		/* Handles a failed dup2 operation. */
		if (dup2(saved_input, STDIN_FILENO) < 0)
			result = 0;
		(void)close(saved_input);
	}
	clearerr(stdin);
	clearerr(stdout);

	/* Returns the computed result. */
	return result;
}

/* Supports the command argv operation. */
static int
command_argv(
	int argc,
	char **argv)
{
	struct sh_var_snapshot snapshots[ARG_MAX];
	int assignments;
	int temporary;
	int index;
	int result;

	assignments = 0;
	temporary = 0;

	/* Validates the command-line arguments. */
	if (argc == 0)
		return 1;

	/* Process each remaining command-line operand. */
	while (assignments < argc && assignment_length(argv[assignments]) >= 0)
		assignments++;

	/* Validates the command-line arguments. */
	if (assignments == argc ||
	    (assignments != 0 && special_builtin_name(argv[assignments]))) {
		/* Process each remaining element. */
		for (index = 0; index < assignments; index++) {
			/* Validates the command-line arguments. */
			if (apply_assignment(argv[index]) < 0) {
				fprintf(stderr, "sh: %s: %s\n", argv[index],
					strerror(errno));

				/* Reports successful completion. */
				return 0;
			}
		}
	} else {
		/* Process each remaining element. */
		for (index = 0; index < assignments; index++) {
			/* Validates the command-line arguments. */
			if (temporary_assignment(argv[index],
						 &snapshots[index]) != 0) {
				/* Process each remaining element. */
				while (index-- > 0)
					(void)sh_var_restore(&snapshots[index]);
				fprintf(stderr, "sh: %s: %s\n", argv[index + 1],
					strerror(errno));

				/* Reports successful completion. */
				return 0;
			}
			temporary++;
		}
	}

	/* Validates the command-line arguments. */
	if (assignments == argc)
		return 1;

	/* Continue while the operation condition remains true. */
	result = command_dispatch(argc - assignments, argv + assignments);
	while (temporary-- > 0)

		/* Handles a failed sh var restore operation. */
		if (sh_var_restore(&snapshots[temporary]) != 0)
			result = 0;

	/* Returns the computed result. */
	return result;
}

/* Supports the special builtin name operation. */
static int
special_builtin_name(
	const char *name)
{
	static const char *const names[] = {
	    ":",    ".",     "break",  "continue", "eval",
	    "exec", "exit",  "export", "readonly", "return",
	    "set",  "shift", "trap",   "unset",	   NULL};
	int index;

	/* Process each remaining element. */
	for (index = 0; names[index] != NULL; index++)

		/* Selects the matching value. */
		if (strcmp(name, names[index]) == 0)
			return 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the apply assignment operation. */
static int
apply_assignment(
	char *text)
{
	int length;
	char saved;
	int result;

	length = assignment_length(text);

	/* Checks the current data length. */
	if (length < 0)
		return 0;
	saved = text[length];
	text[length] = '\0';
	result = sh_var_set(text, text + length + 1, -1) == 0;
	text[length] = saved;

	/* Returns the computed result. */
	return result ? 1 : -1;
}

/* Supports the temporary assignment operation. */
static int
temporary_assignment(
	char *text,
	struct sh_var_snapshot *snapshot)
{
	int length;
	char saved;
	int result;

	length = assignment_length(text);

	/* Checks the current data length. */
	if (length < 0)
		return -1;
	saved = text[length];
	text[length] = '\0';

	/* Handles a failed sh var snapshot operation. */
	if (sh_var_snapshot(text, snapshot) != 0) {
		text[length] = saved;

		/* Reports operation failure. */
		return -1;
	}
	result = sh_var_set(text, text + length + 1, 1);
	text[length] = saved;

	/* Checks the operation result. */
	if (result != 0) {
		(void)sh_var_restore(snapshot);

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the command dispatch operation. */
static int
command_dispatch(
	int argc,
	char **argv)
{
	int function_result;
	const char *name;
	int index_local;
	const char *value_local;
	int index_local1, result_local;
	char candidate_local[256];
	int index_local2;
	int result_local3;
	unsigned long value_local4;
	char candidate_local5[256];
	char candidate_local6[256];
	char **child_local;
	int result_local7;
	int i_local;
	int index_local8;
	int length_local;
	char saved_local;
	int index_local11;
	int length_local10;
	char saved_local9;
	char *child_local12[ARG_MAX + 1];
	int i_local13;
	char *end;
	int status;
	pid_t job;
	char *equals;
	const char *path;
	int number;
	const char *action;
	char *text;
	long count;
	mode_t old;
	char input[SHELL_LINE_MAX];
	int first;
	int handled;

	/* Validates the command-line arguments. */
	if (argc == 0)
		return 1;

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "jobs")) {
		/* Handles the last job condition. */
		if (last_job > 0)
			printf("[%d] active or stopped\n", (int)last_job);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "bg")) {
		/* Computes the function result. */
		function_result = last_job > 0 && kill(-last_job, SIGCONT) == 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "fg")) {
				status = 0;
				job = last_job;

		/* Handles the job condition. */
		if (job <= 0)
			return 0;

		/* Obtains the continue foreground result. */
		function_result = continue_foreground(job, &status);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "help")) {
		puts("help echo pwd cd true false jobs fg bg env set export "
		     "readonly unset wait source exit");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "alias")) {
		/* Validates the command-line arguments. */
		if (argc == 1) {
			sh_alias_print();

			/* Reports operation failure. */
			return 1;
		}

		/* Process each remaining command-line operand. */
		for (index_local = 1; index_local < argc; index_local++) {
						equals = strchr(argv[index_local], '=');

			/* Handles the equals availability. */
			if (equals == NULL) {
								value_local = sh_alias_get(argv[index_local]);

				/* Handles the value local availability. */
				if (value_local == NULL)
					return 0;
				printf("alias %s='%s'\n", argv[index_local], value_local);
				continue;
			}
			*equals = '\0';
			/* Validates the command-line arguments. */
			if (sh_alias_set(argv[index_local], equals + 1) != 0) {
				*equals = '=';
				/* Reports successful completion. */
				return 0;
			}
			*equals = '=';
		}

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "unalias")) {
				result_local = 1;

		/* Handles the selected command-line operation. */
		if (argc == 2 && !strcmp(argv[1], "-a")) {
			sh_alias_clear();

			/* Reports operation failure. */
			return 1;
		}

		/* Validates the command-line arguments. */
		if (argc < 2)
			return 0;

		/* Process each remaining command-line operand. */
		for (index_local1 = 1; index_local1 < argc; index_local1++)

			/* Validates the command-line arguments. */
			if (sh_alias_unset(argv[index_local1]) != 0)
				result_local = 0;

		/* Returns the computed result. */
		return result_local;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "hash")) {
				path = sh_var_get("PATH");
		int index;
		int result = 1;

		/* Handles the selected command-line operation. */
		if (argc == 2 && !strcmp(argv[1], "-r")) {
			sh_hash_clear();

			/* Reports operation failure. */
			return 1;
		}

		/* Validates the command-line arguments. */
		if (argc > 1 && argv[1][0] == '-') {
			fprintf(stderr, "usage: hash [-r] [utility ...]\n");

			/* Reports successful completion. */
			return 0;
		}

		/* Handles a failed sh hash sync path operation. */
		if (sh_hash_sync_path(path) != 0)
			return 0;

		/* Validates the command-line arguments. */
		if (argc == 1) {
			sh_hash_print();

			/* Reports operation failure. */
			return 1;
		}

		/* Process each remaining command-line operand. */
		for (index = 1; index < argc; index++) {
			/* Validates the command-line arguments. */
			if (strchr(argv[index], '/') != NULL ||
			    !resolve_command(argv[index], candidate_local,
					     sizeof(candidate_local))) {
				fprintf(stderr, "hash: %s: not found\n",
					argv[index]);
				result = 0;
			} else if (sh_hash_store(argv[index], candidate_local) != 0)

				/* Reports successful completion. */
				return 0;
		}

		/* Returns the computed result. */
		return result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "getopts")) {
		/* Obtains the shell getopts builtin result. */
		function_result = shell_getopts_builtin(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "trap")) {
		/* Validates the command-line arguments. */
		if (argc == 1) {
			/* Process each remaining element. */
			for (index_local2 = 1; index_local2 < SHELL_SIGNAL_MAX; index_local2++)

				/* Handles the trap action condition. */
				if (trap_action[index_local2] != NULL)
					printf("trap -- '%s' %d\n",
					       trap_action[index_local2], index_local2);

			/* Reports operation failure. */
			return 1;
		}

		/* Validates the command-line arguments. */
		if (argc < 3)
			return 0;

		/* Process each remaining command-line operand. */
		action = !strcmp(argv[1], "-") ? NULL : argv[1];
		for (index_local2 = 2; index_local2 < argc; index_local2++) {
						number = signal_number(argv[index_local2]);

			/* Handles a failed set trap operation. */
			if (number < 0 || !set_trap(action, number))
				return 0;
		}

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], ".") || !strcmp(argv[0], "source")) {
		/* Computes the function result. */
		function_result = argc == 2 && source_file(argv[1]);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "eval")) {
		/* Validates the command-line arguments. */
		if (!join_arguments(argc, argv, 1, &text))
			return 0;
		result_local3 = command(text);
		free(text);

		/* Returns the computed result. */
		return result_local3;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "shift")) {
				count = 1;
		char *end;

		/* Validates the command-line arguments. */
		if (argc > 2)
			return 0;

		/* Validates the command-line arguments. */
		if (argc == 2) {
			count = strtol(argv[1], &end, 10);

			/* Validates the command-line arguments. */
			if (*argv[1] == '\0' || *end != '\0' || count < 0)
				return 0;
		}

		/* Checks the remaining item count. */
		if (count > shell_positional_count)
			return 0;
		shell_positional += count;
		shell_positional_count -= (int)count;

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "umask")) {
		/* Validates the command-line arguments. */
		if (argc == 1) {
			old = umask(0);
			(void)umask(old);
			printf("%04o\n", (unsigned)old);

			/* Reports operation failure. */
			return 1;
		}

		/* Validates the command-line arguments. */
		if (argc == 2) {

						value_local4 = strtoul(argv[1], &end, 8);

			/* Validates the command-line arguments. */
			if (*argv[1] == '\0' || *end != '\0' || value_local4 > 0777UL)
				return 0;
			(void)umask((mode_t)value_local4);

			/* Reports operation failure. */
			return 1;
		}

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "read")) {

				name = argc == 2 ? argv[1] : "REPLY";

		/* Validates the command-line arguments. */
		if (argc > 2 || assignment_length(name) >= 0 ||
		    !(name[0] == '_' || (name[0] >= 'A' && name[0] <= 'Z') ||
		      (name[0] >= 'a' && name[0] <= 'z')))

			/* Reports successful completion. */
			return 0;

		/* Handles a failed read line operation. */
		if (read_line(input, sizeof(input)) < 0)
			return 0;

		/* Computes the function result. */
		function_result = sh_var_set(name, input, -1) == 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "wait")) {
		/* Obtains the shell wait builtin result. */
		function_result = shell_wait_builtin(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "type") || (!strcmp(argv[0], "command") &&
					 argc > 1 && !strcmp(argv[1], "-v"))) {
		/* Process each remaining command-line operand. */
				first = !strcmp(argv[0], "type") ? 1 : 2;
		int index, success = first < argc;
		for (index = first; index < argc; index++) {
			/* Validates the command-line arguments. */
			if (shell_builtin_name(argv[index]))
				printf("%s%s\n",
				       !strcmp(argv[0], "type")
					   ? "shell builtin: "
					   : "",
				       argv[index]);
			else if (strchr(argv[index], '/') != NULL &&
				 access(argv[index], F_OK) == 0)
				puts(argv[index]);
			else if (search_path(argv[index], "", candidate_local5,
					     sizeof(candidate_local5)))
				puts(candidate_local5);
			else
				success = 0;
		}

		/* Returns the computed result. */
		return success;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "command")) {
		/* Computes the function result. */
		function_result = argc > 1 && command_argv(argc - 1, argv + 1);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "exec")) {

				child_local = argv + 1;

		/* Validates the command-line arguments. */
		if (argc < 2)
			return 1;

		/* Handles a failed strchr operation. */
		if (strchr(child_local[0], '/') == NULL) {
			/* Handles a failed search path operation. */
			if (!search_path(child_local[0], "", candidate_local6,
					 sizeof(candidate_local6)))

				/* Reports successful completion. */
				return 0;
			child_local[0] = candidate_local6;
		}
		execve(child_local[0], child_local, environ);
		fprintf(stderr, "exec: %s: %s\n", child_local[0], strerror(errno));

		/* Reports successful completion. */
		return 0;
	}

	result_local7 = sh_builtin_dispatch(argc, argv, &handled);

	/* Handles the handled condition. */
	if (handled)
		return result_local7;

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "env")) {
		/* Process each element required by the operation. */
		for (i_local = 0; environ != NULL && environ[i_local] != NULL; i_local++)
			puts(environ[i_local]);

		/* Returns the computed result. */
		return argc == 1;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "set")) {
		/* Computes the function result. */
		function_result = argc == 3 && sh_var_set(argv[1], argv[2], -1) == 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "unset")) {
		/* Computes the function result. */
		function_result = argc == 2 && sh_var_unset(argv[1]) == 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "export")) {
		/* Validates the command-line arguments. */
		if (argc < 2)
			return 0;

		/* Process each remaining command-line operand. */
		for (index_local8 = 1; index_local8 < argc; index_local8++) {
						length_local = assignment_length(argv[index_local8]);

			/* Handles the length local condition. */
			if (length_local >= 0) {
								saved_local = argv[index_local8][length_local];
				argv[index_local8][length_local] = '\0';

				/* Validates the command-line arguments. */
				if (sh_var_set(argv[index_local8],
					       argv[index_local8] + length_local + 1,
					       1) != 0) {
					argv[index_local8][length_local] = saved_local;

					/* Reports successful completion. */
					return 0;
				}
				argv[index_local8][length_local] = saved_local;
			} else if (sh_var_export(argv[index_local8]) != 0)

				/* Reports successful completion. */
				return 0;
		}

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "readonly")) {
		/* Validates the command-line arguments. */
		if (argc < 2)
			return 0;

		/* Process each remaining command-line operand. */
		for (index_local11 = 1; index_local11 < argc; index_local11++) {
						length_local10 = assignment_length(argv[index_local11]);

			/* Handles the length local10 condition. */
			if (length_local10 >= 0) {
								saved_local9 = argv[index_local11][length_local10];
				argv[index_local11][length_local10] = '\0';

				/* Validates the command-line arguments. */
				if (sh_var_set(argv[index_local11],
					       argv[index_local11] + length_local10 + 1,
					       -1) != 0 ||
				    sh_var_readonly(argv[index_local11]) != 0) {
					argv[index_local11][length_local10] = saved_local9;

					/* Reports successful completion. */
					return 0;
				}
				argv[index_local11][length_local10] = saved_local9;
			} else if (sh_var_readonly(argv[index_local11]) != 0)

				/* Reports successful completion. */
				return 0;
		}

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], ":"))
		return 1;

	/* Handles the selected command-line operation. */
	if (!strcmp(argv[0], "exit"))
		exit(argc == 2 ? atoi(argv[1]) : 0);

	/* Validates the command-line arguments. */
	if (strchr(argv[0], '/') != NULL && access(argv[0], F_OK) == 0) {
		/* Process each remaining command-line operand. */
		for (i_local13 = 0; i_local13 < argc; i_local13++)
			child_local12[i_local13] = argv[i_local13];
		child_local12[argc] = NULL;

		/* Computes the function result. */
		function_result = is_elf(argv[0]) ? run_external(child_local12)
				       : run_shell_script(argc, argv, argv[0]);

		/* Returns the computed result. */
		return function_result;
	}

	/* Obtains the run search path result. */
	function_result = run_search_path(argc, argv);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the continue foreground operation. */
static int
continue_foreground(
	pid_t pid,
	int *status)
{
	pid_t result;
	pid_t processes[PIPELINE_MAX];
	pid_t retained[PIPELINE_MAX];
	pid_t shell_pgrp;
	int terminal;
	int foreground_set;
	int process_count;
	int retained_count;
	int index;
	int wait_failed;
	int saved_errno;

	shell_pgrp = getpgrp();
	terminal = isatty(STDIN_FILENO);
	foreground_set = 0;
	process_count = last_job_process_count;
	retained_count = 0;
	wait_failed = 0;

	/* Handles the process count condition. */
	if (process_count <= 0) {
		processes[0] = pid;
		process_count = 1;
	} else {
		/* Process each remaining element. */
		for (index = 0; index < process_count; index++)
			processes[index] = last_job_processes[index];
	}

	/*
 * A stopped terminal reader must own the terminal before it resumes.
	 * Keep last_job intact until both operations succeed so a failed
	 * handoff remains retryable. */
	if (terminal) {
		/* Handles a failed shell tcsetpgrp operation. */
		if (shell_tcsetpgrp(STDIN_FILENO, pid) != 0) {
			fprintf(stderr,
				"fg: cannot foreground process %d: %s\n",
				(int)pid, strerror(errno));

			/* Reports successful completion. */
			return 0;
		}
		foreground_set = 1;
	}

	/* Handles a failed kill operation. */
	if (kill(-pid, SIGCONT) != 0) {
		saved_errno = errno;

		/* Handles a failed shell tcsetpgrp operation. */
		if (foreground_set &&
		    shell_tcsetpgrp(STDIN_FILENO, shell_pgrp) != 0)
			fprintf(
			    stderr,
			    "sh: cannot restore foreground process group: %s\n",
			    strerror(errno));
		errno = saved_errno;
		fprintf(stderr, "fg: cannot continue process %d: %s\n",
			(int)pid, strerror(errno));

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining element. */
	for (index = 0; index < process_count; index++) {

		do

		/* Continue while the operation condition remains true. */
			result = waitpid(processes[index], status, WUNTRACED);
		while (result < 0 && errno == EINTR);

		/* Checks the operation result. */
		if (result < 0) {
			/* Handles the reported system error. */
			if (errno == ECHILD)
				continue;

			/* Process each remaining element. */
			saved_errno = errno;
			wait_failed = 1;
			for (; index < process_count; index++)
				retained[retained_count++] = processes[index];
			break;
		}

		/* Checks the operation status. */
		if (WIFSTOPPED(*status))
			retained[retained_count++] = processes[index];
	}

	/* Handles a failed shell tcsetpgrp operation. */
	if (foreground_set && shell_tcsetpgrp(STDIN_FILENO, shell_pgrp) != 0)
		fprintf(stderr,
			"sh: cannot restore foreground process group: %s\n",
			strerror(errno));

	/* Handles an operation failure. */
	if (wait_failed) {
		remember_job(pid, retained, retained_count);
		errno = saved_errno;
		fprintf(stderr, "fg: cannot wait for process %d: %s\n",
			(int)pid, strerror(errno));

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the retained count condition. */
	if (retained_count > 0) {
		remember_job(pid, retained, retained_count);
		printf("[%d] stopped\n", (int)pid);
	} else {
		forget_job();
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the shell tcsetpgrp operation. */
static int
shell_tcsetpgrp(
	int descriptor,
	pid_t pgrp)
{
	void (*previous)(int);
	int error, saved_errno;

	/*
 * A shell restoring itself from the background must not be stopped by
	 * the TIOCSPGRP operation which makes it foreground again. */
	previous = signal(SIGTTOU, (sighandler_t)SIG_IGN);

	/* Handles the previous condition. */
	if (previous == (sighandler_t)SIG_ERR)
		return -1;
	error = tcsetpgrp(descriptor, pgrp);
	saved_errno = errno;
	(void)signal(SIGTTOU, previous);
	errno = saved_errno;

	/* Returns the computed result. */
	return error;
}

/* Supports the remember job operation. */
static void
remember_job(
	pid_t group,
	const pid_t *processes,
	int count)
{
	int index;

	/* Process each remaining element. */
	last_job = group;
	last_job_process_count = count;
	for (index = 0; index < count; index++)
		last_job_processes[index] = processes[index];
}

/* Supports the forget job operation. */
static void
forget_job(
	void)
{
	last_job = 0;
	last_job_process_count = 0;
}

/* Supports the resolve command operation. */
static int
resolve_command(
	const char *name,
	char *candidate,
	size_t capacity)
{
	int function_result;

	/* Obtains the search path result. */
	function_result = search_path(name, "", candidate, capacity);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the search path operation. */
static int
search_path(
	const char *name,
	const char *suffix,
	char *candidate,
	size_t capacity)
{
	int last;
	int result;
	const char *path;
	size_t position;

	path = sh_var_get("PATH");
	position = 0;

	/* Handles the path availability. */
	if (path == NULL)

	/* Continue until the operation reaches a terminal state. */
		path = "/bin:/usr/bin";
	for (;;) {

		result = path_candidate(path, &position, name, suffix,
					    candidate, capacity, &last);

		/* Handles a failed executable file operation. */
		if (result > 0 && is_executable_file(candidate))
			return 1;

		/* Handles the last condition. */
		if (last)
			break;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the path candidate operation. */
static int
path_candidate(
	const char *path,
	size_t *position,
	const char *name,
	const char *suffix,
	char *candidate,
	size_t capacity,
	int *last)
{
	size_t start;
	size_t length;
	size_t name_length;
	size_t suffix_length;

	/* Continue while the operation condition remains true. */
	start = *position;
	name_length = strlen(name);
	suffix_length = strlen(suffix);
	while (path[*position] != '\0' && path[*position] != ':')
		(*position)++;
	length = *position - start;
	*last = path[*position] == '\0';
	/* Handles the last condition. */
	if (!*last)
		(*position)++;

	/* Checks the current data length. */
	if (length == 0) {
		/* Handles the name length condition. */
		if (2U + name_length + suffix_length > capacity)
			return -1;
		candidate[0] = '.';
		candidate[1] = '/';
		memcpy(candidate + 2, name, name_length);
		memcpy(candidate + 2 + name_length, suffix, suffix_length + 1U);

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the current data length. */
	if (length + 1U + name_length + suffix_length + 1U > capacity)
		return -1;
	memcpy(candidate, path + start, length);
	candidate[length] = '/';
	memcpy(candidate + length + 1U, name, name_length);
	memcpy(candidate + length + 1U + name_length, suffix,
	       suffix_length + 1U);

	/* Reports operation failure. */
	return 1;
}

/* Supports the is executable file operation. */
static int
is_executable_file(
	const char *path)
{
	int function_result;
	struct stat status;

	/* Handles a failed stat operation. */
	if (stat(path, &status) != 0)
		return 0;

	/* Handles a failed S ISREG operation. */
	if (!S_ISREG(status.st_mode)) {
		errno = EACCES;

		/* Reports successful completion. */
		return 0;
	}

	/* Computes the function result. */
	function_result = access(path, X_OK) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the shell getopts builtin operation. */
static int
shell_getopts_builtin(
	int argc,
	char **argv)
{
	int function_result;
	const char *argument;
	const char *value;
	const char *options;
	const char *index_text;
	char **arguments;
	int argument_count;
	char *end;
	long option_index;
	char option_name[2] = {0, 0};
	char bad[2];
	char missing[2];
	const char *definition;
	int silent;

	/* Validates the command-line arguments. */
	if (argc < 3 || !sh_var_name(argv[2]))
		return 0;
	options = argv[1];
	silent = options[0] == ':';

	/* Handles the silent condition. */
	if (silent)
		options++;
	arguments = argc > 3 ? argv + 3 : shell_positional;
	argument_count = argc > 3 ? argc - 3 : shell_positional_count;
	index_text = sh_var_get("OPTIND");
	option_index = index_text == NULL ? 1 : strtol(index_text, &end, 10);

	/* Handles the index text availability. */
	if (index_text != NULL && (*index_text == '\0' || *end != '\0'))
		option_index = 1;

	/* Handles the option index condition. */
	if (option_index < 1)
		option_index = 1;

	/* Handles the option index condition. */
	if (option_index != getopts_last_index)
		getopts_offset = 1;

	/* Handles the option index condition. */
	if (option_index > argument_count)
		return 0;

	/* Handles the getopts offset condition. */
	if (getopts_offset == 1) {
				argument = arguments[option_index - 1];

		/* Handles the argument condition. */
		if (argument[0] != '-' || argument[1] == '\0')
			return 0;

		/* Selects the matching value. */
		if (!strcmp(argument, "--")) {
			option_index++;
			getopts_last_index = option_index;
			(void)set_decimal_variable("OPTIND", option_index);

			/* Reports successful completion. */
			return 0;
		}
	}
	option_name[0] = arguments[option_index - 1][getopts_offset++];

	/* Handles the arguments condition. */
	if (arguments[option_index - 1][getopts_offset] == '\0') {
		option_index++;
		getopts_offset = 1;
	}
	definition = strchr(options, option_name[0]);

	/* Handles the definition availability. */
	if (definition == NULL) {
		bad[0] = option_name[0];
		bad[1] = '\0';
		(void)sh_var_set(argv[2], "?", -1);

		/* Handles the silent condition. */
		if (silent)
			(void)sh_var_set("OPTARG", bad, -1);
		else
			fprintf(stderr, "getopts: illegal option -- %c\n",
				option_name[0]);
		getopts_last_index = option_index;
		(void)set_decimal_variable("OPTIND", option_index);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the definition condition. */
	if (definition[1] == ':') {
		/* Handles the getopts offset condition. */
		if (getopts_offset != 1) {
			value = arguments[option_index - 1] + getopts_offset;
			option_index++;
			getopts_offset = 1;
		} else if (option_index <= argument_count) {
			value = arguments[option_index - 1];
			option_index++;
		} else {
			missing[0] = option_name[0];
			missing[1] = '\0';
			(void)sh_var_set(argv[2], silent ? ":" : "?", -1);

			/* Handles the silent condition. */
			if (silent)
				(void)sh_var_set("OPTARG", missing, -1);
			else
				fprintf(stderr,
					"getopts: option requires an argument "
					"-- %c\n",
					option_name[0]);
			getopts_last_index = option_index;
			(void)set_decimal_variable("OPTIND", option_index);

			/* Reports operation failure. */
			return 1;
		}
		(void)sh_var_set("OPTARG", value, -1);
	} else {
		(void)sh_var_unset("OPTARG");
	}
	(void)sh_var_set(argv[2], option_name, -1);
	getopts_last_index = option_index;

	/* Obtains the set decimal variable result. */
	function_result = set_decimal_variable("OPTIND", option_index);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the set decimal variable operation. */
static int
set_decimal_variable(
	const char *name,
	long value)
{
	int function_result;
	char buffer[32];
	int length;

	length = snprintf(buffer, sizeof(buffer), "%ld", value);

	/* Computes the function result. */
	function_result = length > 0 && (size_t)length < sizeof(buffer) &&
	       sh_var_set(name, buffer, -1) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the signal number operation. */
static int
signal_number(
	const char *name)
{
	static const struct {
		const char *name;
		int number;
	} names[] = {{"HUP", SIGHUP},	{"INT", SIGINT},   {"QUIT", SIGQUIT},
		     {"ILL", SIGILL},	{"TRAP", SIGTRAP}, {"ABRT", SIGABRT},
		     {"FPE", SIGFPE},	{"KILL", SIGKILL}, {"BUS", SIGBUS},
		     {"SEGV", SIGSEGV}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM},
		     {"TERM", SIGTERM}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2},
		     {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, {"STOP", SIGSTOP},
		     {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU}};
	char *end;
	long value;
	size_t index;

	/* Selects the matching prefix. */
	if (!strncmp(name, "SIG", 3))
		name += 3;
	value = strtol(name, &end, 10);

	/* Validates the current name. */
	if (*name != '\0' && *end == '\0' && value > 0 &&
	    value < SHELL_SIGNAL_MAX)

		/* Returns the computed result. */
		return (int)value;

	/* Process each remaining element. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++)

		/* Selects the matching value. */
		if (!strcmp(name, names[index].name))
			return names[index].number;

	/* Reports operation failure. */
	return -1;
}

/* Supports the set trap operation. */
static int
set_trap(
	const char *action,
	int number)
{
	struct sigaction disposition;
	char *copy;

	copy = NULL;

	/* Handles the number condition. */
	if (number <= 0 || number >= SHELL_SIGNAL_MAX || number == SIGKILL ||
	    number == SIGSTOP) {
		errno = EINVAL;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the action availability. */
	if (action != NULL && action[0] != '\0') {
		copy = malloc(strlen(action) + 1U);

		/* Handles the copy availability. */
		if (copy == NULL)
			return 0;
		strcpy(copy, action);
	}
	memset(&disposition, 0, sizeof(disposition));

	/* Handles the action availability. */
	if (action == NULL)
		disposition.sa_handler = SIG_DFL;
	else if (action[0] == '\0')
		disposition.sa_handler = SIG_IGN;
	else
		disposition.sa_handler =
		    (uint64_t)(uintptr_t)shell_signal_handler;
	disposition.sa_flags = SA_RESTART;
	sigemptyset(&disposition.sa_mask);

	/* Handles a failed sigaction operation. */
	if (sigaction(number, &disposition, NULL) != 0) {
		free(copy);

		/* Reports successful completion. */
		return 0;
	}
	free(trap_action[number]);
	trap_action[number] = copy;
	trap_pending[number] = 0;

	/* Reports operation failure. */
	return 1;
}

/* Supports the source file operation. */
static int
source_file(
	const char *path)
{
	int function_result;

	/* Obtains the source file mode result. */
	function_result = source_file_mode(path, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the source file mode operation. */
static int
source_file_mode(
	const char *path,
	int continue_on_error)
{
	char *end;
	FILE *file;
	char *buffer, *line;
	struct stat status;
	unsigned line_number;

	line_number = 0;

	/* Handles a failed stat operation. */
	if (stat(path, &status) != 0 || status.st_size < 0 ||
	    status.st_size >= SOURCE_MAX)

		/* Reports successful completion. */
		return 0;
	file = fopen(path, "rb");

	/* Handles the file availability. */
	if (file == NULL)
		return 0;
	buffer = malloc((size_t)status.st_size + 1U);

	/* Handles the buffer availability. */
	if (buffer == NULL) {
		fclose(file);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed fread operation. */
	if (fread(buffer, 1, (size_t)status.st_size, file) !=
	    (size_t)status.st_size) {
		fclose(file);
		free(buffer);

		/* Reports successful completion. */
		return 0;
	}
	fclose(file);

	/* Continue while the operation condition remains true. */
	buffer[status.st_size] = '\0';
	line = buffer;
	while (*line != '\0') {

		end = line;
		line_number++;

		/* Continue while the operation condition remains true. */
		while (*end != '\0' && *end != '\r' && *end != '\n')
			end++;

		/* Checks the current endpoint. */
		if (*end != '\0') {
			/* Continue while the operation condition remains true. */
			*end++ = '\0';
			while (*end == '\r' || *end == '\n')
				end++;
		}

		/* Handles a failed command operation. */
		if (!command(line)) {
			/* Handles an operation failure. */
			if (!continue_on_error) {
				free(buffer);

				/* Reports successful completion. */
				return 0;
			}
			fprintf(stderr, "%s:%u: command failed\n", path,
				line_number);
		}
		line = end;
	}
	free(buffer);

	/* Reports operation failure. */
	return 1;
}

/* Supports the join arguments operation. */
static int
join_arguments(
	int argc,
	char **argv,
	int first,
	char **result)
{
	size_t item_local;
	size_t item_local1;
	size_t length;
	int index;
	char *text, *cursor;

	/* Process each remaining command-line operand. */
	length = 0;
	for (index = first; index < argc; index++) {
				item_local = strlen(argv[index]);

		/* Checks the current data length. */
		if (length > (size_t)-1 - item_local - 2U)
			return 0;
		length += item_local + (index != first);
	}
	text = malloc(length + 1U);

	/* Handles the text availability. */
	if (text == NULL)
		return 0;

	/* Process each remaining command-line operand. */
	cursor = text;
	for (index = first; index < argc; index++) {
				item_local1 = strlen(argv[index]);

		/* Checks the current index. */
		if (index != first)
			*cursor++ = ' ';
		memcpy(cursor, argv[index], item_local1);
		cursor += item_local1;
	}
	*cursor = '\0';
	*result = text;
	/* Reports operation failure. */
	return 1;
}

/* Supports the read line operation. */
static int
read_line(
	char *buffer,
	size_t capacity)
{
	ssize_t length;

	/* Handles the capacity condition. */
	if (capacity < 2)
		return -1;
	length = read(0, buffer, capacity - 1U);

	/* Checks the current data length. */
	if (length <= 0)
		return -1;

	/* Process each remaining element. */
	while (length > 0 &&
	       (buffer[length - 1] == '\r' || buffer[length - 1] == '\n'))
		length--;
	buffer[length] = '\0';

	/* Returns the computed result. */
	return (int)length;
}

/* Supports the shell wait builtin operation. */
static int
shell_wait_builtin(
	int argc,
	char **argv)
{
	int function_result;
	char *end;
	long value;
	pid_t group;
	pid_t processes[PIPELINE_MAX];
	pid_t remaining[PIPELINE_MAX];
	int index;
	int count;
	int remaining_count;
	pid_t target;
	int status;

	status = 0;

	/* Validates the command-line arguments. */
	if (argc > 2) {
		fprintf(stderr, "usage: wait [PID]\n");

		/* Reports successful completion. */
		return 0;
	}

	/* Validates the command-line arguments. */
	if (argc == 2) {

				value = strtol(argv[1], &end, 10);

		/* Validates the command-line arguments. */
		if (*argv[1] == '\0' || *end != '\0' || value <= 0) {
			fprintf(stderr, "wait: invalid pid: %s\n", argv[1]);

			/* Reports successful completion. */
			return 0;
		}
		target = (pid_t)value;

		/* Handles a failed waitpid operation. */
		if (waitpid(target, &status, 0) != target)
			return 0;
	} else if (last_job > 0) {
				group = last_job;

				count = last_job_process_count;
				remaining_count = 0;

		/* Checks the remaining item count. */
		if (count <= 0) {
			processes[0] = group;
			count = 1;
		} else {
			/* Process each remaining element. */
			for (index = 0; index < count; index++)
				processes[index] = last_job_processes[index];
		}

		/* Process each remaining element. */
		for (index = 0; index < count; index++) {
			do

			/* Continue while the operation condition remains true. */
				target = waitpid(processes[index], &status, 0);
			while (target < 0 && errno == EINTR);

			/* Handles the reported system error. */
			if (target < 0 && errno != ECHILD) {
				/* Process each remaining element. */
				for (; index < count; index++)
					remaining[remaining_count++] =
					    processes[index];
				remember_job(group, remaining, remaining_count);

				/* Reports successful completion. */
				return 0;
			}
		}
		forget_job();
	} else {
		/* Continue while the operation condition remains true. */
		while (waitpid(-1, &status, 0) > 0)
			;

		/* Returns the computed result. */
		return errno == ECHILD;
	}

	/* Computes the function result. */
	function_result = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the shell builtin name operation. */
static int
shell_builtin_name(
	const char *name)
{
	static const char *const names[] = {
	    ":",       ".",	 "[",	  "alias",   "bg",	 "cd",
	    "command", "echo",	 "env",	  "eval",    "exec",	 "exit",
	    "export",  "false",	 "fg",	  "getopts", "hash",	 "help",
	    "jobs",    "printf", "pwd",	  "read",    "readonly", "set",
	    "shift",   "source", "true",  "type",    "test",	 "umask",
	    "unalias", "ulimit", "unset", "wait",    NULL};
	int index;

	/* Process each remaining element. */
	for (index = 0; names[index] != NULL; index++)

		/* Selects the matching value. */
		if (strcmp(name, names[index]) == 0)
			return 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the is elf operation. */
static int
is_elf(
	const char *path)
{
	int function_result;
	unsigned char magic[4];
	int fd;
	ssize_t count;

	fd = open(path, O_RDONLY);

	/* Checks the file descriptor. */
	if (fd < 0)
		return 0;
	count = read(fd, magic, sizeof(magic));
	close(fd);

	/* Computes the function result. */
	function_result = count == (ssize_t)sizeof(magic) && magic[0] == 0x7f &&
	       magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';

	/* Returns the computed result. */
	return function_result;
}

/* Supports the run external operation. */
static int
run_external(
	char *const argv[])
{
	int function_result;

	/* Obtains the spawn wait result. */
	function_result = spawn_wait(argv);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the spawn wait operation. */
static int
spawn_wait(
	char *const argv[])
{
	int function_result;
	pid_t waited;
	posix_spawnattr_t attributes;
	posix_spawnattr_t *attribute_pointer;
	pid_t pid;
	int error;
	int status;

	attribute_pointer = NULL;
	status = 0;

	/* Handles a failed isatty operation. */
	if (!command_subshell && !command_background && isatty(STDIN_FILENO)) {
		/* Validates the command-line arguments. */
		if (spawn_foreground_tty(argv, &status) != 0) {
			fprintf(stderr, "sh: %s: %s\n", argv[0],
				strerror(errno));

			/* Reports successful completion. */
			return 0;
		}

		/* Checks the operation status. */
		if (WIFSIGNALED(status)) {
			fprintf(stderr, "%s\n",
				signal_message(WTERMSIG(status)));

			/* Reports successful completion. */
			return 0;
		}

		/* Computes the function result. */
		function_result = WIFEXITED(status) && WEXITSTATUS(status) == 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the command subshell condition. */
	if (!command_subshell) {
		/* Handles a failed posix spawnattr init operation. */
		if (posix_spawnattr_init(&attributes) != 0 ||
		    posix_spawnattr_setflags(&attributes,
					     POSIX_SPAWN_SETPGROUP) != 0 ||
		    posix_spawnattr_setpgroup(&attributes, 0) != 0) {
			fprintf(stderr,
				"sh: unable to prepare process group\n");

			/* Reports successful completion. */
			return 0;
		}
		attribute_pointer = &attributes;
	}
	error =
	    posix_spawn(&pid, argv[0], NULL, attribute_pointer, argv, environ);

	/* Handles the attribute pointer availability. */
	if (attribute_pointer != NULL)
		(void)posix_spawnattr_destroy(attribute_pointer);

	/* Handles an operation failure. */
	if (error != 0) {
		fprintf(stderr, "sh: %s: %s\n", argv[0], strerror(error));

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the command subshell condition. */
	if (!command_subshell)
		(void)setpgid(pid, pid);

	/* Handles the command background condition. */
	if (command_background) {
		remember_single_job(pid);
		printf("[%d]\n", (int)pid);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the command subshell condition. */
	if (command_subshell) {

		do

		/* Continue while the operation condition remains true. */
			waited = waitpid(pid, &status, 0);
		while (waited < 0 && errno == EINTR);

		/* Handles the waited condition. */
		if (waited < 0) {
			fprintf(stderr, "wait: %d\n", errno);

			/* Reports successful completion. */
			return 0;
		}
	} else if (!wait_foreground(pid, &status)) {
		fprintf(stderr, "wait: %d\n", errno);

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the operation status. */
	if (WIFSIGNALED(status)) {
		fprintf(stderr, "%s\n", signal_message(WTERMSIG(status)));

		/* Reports successful completion. */
		return 0;
	}

	/* Computes the function result. */
	function_result = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the spawn foreground tty operation. */
static int
spawn_foreground_tty(
	char *const argv[],
	int *status)
{
	int saved_errno_local;
	char release;
	int gate[2];
	int saved_errno;
	pid_t child, waited;
	pid_t shell_pgrp;
	ssize_t count;

	release = 'x';
	shell_pgrp = getpgrp();

	/*
 * posix_spawn() returns only after the child has executed.  A child
	 * which reads the terminal can therefore receive SIGTTIN before the
	 * parent makes its new process group foreground.  Hold the pre-exec
	 * child behind a close-on-exec pipe until the terminal hand-off is
	 * complete. */
	if (pipe2(gate, O_CLOEXEC) != 0)
		return -1;
	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
				saved_errno_local = errno;

		(void)close(gate[0]);
		(void)close(gate[1]);
		errno = saved_errno_local;

		/* Reports operation failure. */
		return -1;
	}

	/* Checks the child process state. */
	if (child == 0) {
		(void)close(gate[1]);

		/* Handles a failed setpgid operation. */
		if (setpgid(0, 0) != 0)
			_exit(126);
		do

		/* Process each remaining element. */
			count = read(gate[0], &release, 1);
		while (count < 0 && errno == EINTR);
		(void)close(gate[0]);

		/* Checks the remaining item count. */
		if (count != 1)
			_exit(126);
		execve(argv[0], argv, environ);
		fprintf(stderr, "sh: %s: %s\n", argv[0], strerror(errno));
		(void)fflush(stderr);
		_exit(127);
	}

	(void)close(gate[0]);

	/* Handles a failed setpgid operation. */
	if (setpgid(child, child) != 0)
		goto setup_failed;

	/* Handles a failed shell tcsetpgrp operation. */
	if (shell_tcsetpgrp(STDIN_FILENO, child) != 0)
		goto setup_failed;
	count = shell_write_nosigpipe(gate[1], &release, 1);

	/* Checks the remaining item count. */
	if (count != 1) {
		/* Checks the remaining item count. */
		if (count >= 0)
			errno = EIO;
		goto setup_failed;
	}
	(void)close(gate[1]);
	do

	/* Continue while the operation condition remains true. */
		waited = waitpid(child, status, WUNTRACED);
	while (waited < 0 && errno == EINTR);

	/* Handles a failed shell tcsetpgrp operation. */
	if (shell_tcsetpgrp(STDIN_FILENO, shell_pgrp) != 0)
		fprintf(stderr,
			"sh: cannot restore foreground process group: %s\n",
			strerror(errno));

	/* Handles the waited condition. */
	if (waited < 0)
		return -1;

	/* Checks the operation status. */
	if (WIFSTOPPED(*status)) {
		remember_single_job(child);
		printf("[%d] stopped\n", (int)child);
	}

	/* Reports successful completion. */
	return 0;

setup_failed:
	saved_errno = errno;
	(void)close(gate[1]);
	(void)kill(child, SIGKILL);
	do

	/* Continue while the operation condition remains true. */
		waited = waitpid(child, status, 0);
	while (waited < 0 && errno == EINTR);
	(void)shell_tcsetpgrp(STDIN_FILENO, shell_pgrp);
	errno = saved_errno;

	/* Reports operation failure. */
	return -1;
}

/* Supports the shell write nosigpipe operation. */
static ssize_t
shell_write_nosigpipe(
	int descriptor,
	const void *buffer,
	size_t length)
{
	void (*previous)(int);
	ssize_t result;
	int saved_errno;

	previous = signal(SIGPIPE, (sighandler_t)SIG_IGN);

	/* Handles the previous condition. */
	if (previous == (sighandler_t)SIG_ERR)
		return -1;
	do

	/* Continue while the operation condition remains true. */
		result = write(descriptor, buffer, length);
	while (result < 0 && errno == EINTR);
	saved_errno = errno;

	/* Handles a failed signal operation. */
	if (signal(SIGPIPE, previous) == (sighandler_t)SIG_ERR)
		return -1;
	errno = saved_errno;

	/* Returns the computed result. */
	return result;
}

/* Supports the remember single job operation. */
static void
remember_single_job(
	pid_t process)
{
	remember_job(process, &process, 1);
}

/* Supports the signal message operation. */
static const char *
signal_message(
	int number)
{
	/* Dispatch the selected operation case. */
	switch (number) {
	case SIGHUP:
		/* Returns the computed result. */
		return "Hangup";
	case SIGINT:
		/* Returns the computed result. */
		return "Interrupt";
	case SIGQUIT:
		/* Returns the computed result. */
		return "Quit";
	case SIGILL:
		/* Returns the computed result. */
		return "Illegal instruction";
	case SIGTRAP:
		/* Returns the computed result. */
		return "Trace/BPT trap";
	case SIGABRT:
		/* Returns the computed result. */
		return "Abort trap";
	case SIGFPE:
		/* Returns the computed result. */
		return "Floating point exception";
	case SIGKILL:
		/* Returns the computed result. */
		return "Killed";
	case SIGBUS:
		/* Returns the computed result. */
		return "Bus error";
	case SIGSEGV:
		/* Returns the computed result. */
		return "Segmentation fault";
	case SIGPIPE:
		/* Returns the computed result. */
		return "Broken pipe";
	case SIGALRM:
		/* Returns the computed result. */
		return "Alarm clock";
	case SIGTERM:
		/* Returns the computed result. */
		return "Terminated";
	default:
		/* Returns the computed result. */
		return "Terminated by signal";
	}
}

/* Supports the wait foreground operation. */
static int
wait_foreground(
	pid_t pid,
	int *status)
{
	pid_t shell_pgrp;
	int terminal;
	int foreground_set;
	pid_t result;

	shell_pgrp = getpgrp();
	terminal = isatty(0);
	foreground_set = 0;

	/* Checks the terminal state. */
	if (terminal) {
		/* Handles a failed shell tcsetpgrp operation. */
		if (shell_tcsetpgrp(0, pid) == 0)
			foreground_set = 1;
		else
			fprintf(stderr,
				"sh: cannot foreground process %d: %s\n",
				(int)pid, strerror(errno));
	}
	do

	/* Continue while the operation condition remains true. */
		result = waitpid(pid, status, WUNTRACED);
	while (result < 0 && errno == EINTR);

	/* Handles a failed shell tcsetpgrp operation. */
	if (foreground_set && shell_tcsetpgrp(0, shell_pgrp) != 0)
		fprintf(stderr,
			"sh: cannot restore foreground process group: %s\n",
			strerror(errno));

	/* Checks the operation result. */
	if (result < 0)
		return 0;

	/* Checks the operation status. */
	if (WIFSTOPPED(*status)) {
		remember_single_job(pid);
		printf("[%d] stopped\n", (int)pid);
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the run shell script operation. */
static int
run_shell_script(
	int argc,
	char **argv,
	const char *path)
{
	int function_result;
	char *child[ARG_MAX + 1];
	int i;

	/* Validates the command-line arguments. */
	if (argc + 1 >= ARG_MAX)
		return 0;

	/* Process each remaining command-line operand. */
	child[0] = "/bin/sh";
	child[1] = (char *)path;
	for (i = 1; i < argc; i++)
		child[i + 1] = argv[i];
	child[argc + 1] = NULL;

	/* Obtains the run external result. */
	function_result = run_external(child);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the run search path operation. */
static int
run_search_path(
	int argc,
	char **argv)
{
	int function_result;
	char candidate[256];
	const char *path;
	const char *cached;

	path = sh_var_get("PATH");

	/* Handles a failed sh hash sync path operation. */
	if (sh_hash_sync_path(path) != 0)
		return 0;
	cached = sh_hash_lookup(argv[0]);

	/* Handles the cached availability. */
	if (cached != NULL) {
		/* Obtains the run resolved result. */
		function_result = run_resolved(argc, argv, cached);

		/* Returns the computed result. */
		return function_result;
	}

	/* Validates the command-line arguments. */
	if (resolve_command(argv[0], candidate, sizeof(candidate))) {
		/* Validates the command-line arguments. */
		if (sh_hash_store(argv[0], candidate) != 0)
			return 0;

		/* Obtains the run resolved result. */
		function_result = run_resolved(argc, argv, candidate);

		/* Returns the computed result. */
		return function_result;
	}
	fprintf(stderr, "sh: %s: not found\n", argv[0]);

	/* Reports successful completion. */
	return 0;
}

/* Supports the run resolved operation. */
static int
run_resolved(
	int argc,
	char **argv,
	const char *path)
{
	int function_result;
	char *child[ARG_MAX + 1];
	int index;

	/* Handles a failed executable file operation. */
	if (!is_executable_file(path)) {
		fprintf(stderr, "sh: %s: %s\n", path, strerror(errno));

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed elf operation. */
	if (!is_elf(path)) {
		/* Obtains the run shell script result. */
		function_result = run_shell_script(argc, argv, path);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining command-line operand. */
	child[0] = (char *)path;
	for (index = 1; index < argc; index++)
		child[index] = argv[index];
	child[argc] = NULL;

	/* Obtains the run external result. */
	function_result = run_external(child);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the pipeline child operation. */
static int
pipeline_child(
	struct pipeline_command *item)
{
	int function_result;
	int descriptor;

	/* Handles the input availability. */
	if (item->input != NULL) {
		descriptor = open(item->input, O_RDONLY);

		/* Handles a failed dup2 operation. */
		if (descriptor < 0 || dup2(descriptor, STDIN_FILENO) < 0) {
			fprintf(stderr, "%s: %s\n", item->input,
				strerror(errno));

			/* Checks the file descriptor. */
			if (descriptor >= 0)
				(void)close(descriptor);

			/* Reports successful completion. */
			return 0;
		}

		/* Checks the file descriptor. */
		if (descriptor != STDIN_FILENO)
			(void)close(descriptor);
	}

	/* Handles the output availability. */
	if (item->output != NULL) {
		descriptor = open(item->output,
				  O_WRONLY | O_CREAT |
				      (item->append ? O_APPEND : O_TRUNC),
				  0666);

		/* Handles a failed dup2 operation. */
		if (descriptor < 0 || dup2(descriptor, STDOUT_FILENO) < 0) {
			fprintf(stderr, "%s: %s\n", item->output,
				strerror(errno));

			/* Checks the file descriptor. */
			if (descriptor >= 0)
				(void)close(descriptor);

			/* Reports successful completion. */
			return 0;
		}

		/* Checks the file descriptor. */
		if (descriptor != STDOUT_FILENO)
			(void)close(descriptor);
	}
	command_subshell = 1;
	command_background = 0;

	/* Obtains the command argv result. */
	function_result = command_argv(item->argc, item->argv);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the shell lookup operation. */
static const char *
shell_lookup(
	void *context,
	const char *name)
{
	const char *function_result;

	(void)context;

	/* Obtains the sh var get result. */
	function_result = sh_var_get(name);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the shell assign operation. */
static int
shell_assign(
	void *context,
	const char *name,
	const char *value)
{
	int function_result;

	(void)context;

	/* Obtains the sh var set result. */
	function_result = sh_var_set(name, value, -1);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the shell signal handler operation. */
static void
shell_signal_handler(
	int signal_number)
{
	/* Handles the signal number condition. */
	if (signal_number > 0 && signal_number < SHELL_SIGNAL_MAX)
		trap_pending[signal_number] = 1;
}

/* Supports the shell command substitute operation. */
static int
shell_command_substitute(
	void *context,
	const char *source,
	char **result)
{
	int success;
	char chunk[256];
	ssize_t count;
	char *larger;
	int descriptors[2];
	pid_t child;
	char *output;
	size_t length, capacity;
	int status;

	output = NULL;
	length = 0;
	capacity = 0;
	(void)context;
	*result = NULL;
	/* Handles a failed pipe operation. */
	if (pipe(descriptors) != 0)
		return 0;
	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the child process state. */
	if (child == 0) {

		(void)close(descriptors[0]);

		/* Handles a failed dup2 operation. */
		if (dup2(descriptors[1], STDOUT_FILENO) < 0)
			_exit(1);
		(void)close(descriptors[1]);
		command_subshell = 1;
		success = command((char *)source);
		(void)fflush(NULL);
		_exit(success ? 0 : 1);
	}
	(void)close(descriptors[1]);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		count = read(descriptors[0], chunk, sizeof(chunk));

		/* Checks the remaining item count. */
		if (count < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;
			free(output);
			(void)close(descriptors[0]);
			(void)waitpid(child, NULL, 0);

			/* Reports successful completion. */
			return 0;
		}

		/* Checks the remaining item count. */
		if (count == 0)
			break;

		/* Checks the current data length. */
		if (length + (size_t)count + 1U < length) {
			free(output);
			(void)close(descriptors[0]);
			(void)waitpid(child, NULL, 0);

			/* Reports successful completion. */
			return 0;
		}

		/* Checks the current data length. */
		if (length + (size_t)count + 1U > capacity) {
			/* Process each remaining element. */
			capacity = capacity == 0 ? 512U : capacity;
			while (capacity < length + (size_t)count + 1U)
				capacity *= 2U;
			larger = realloc(output, capacity);

			/* Handles the larger availability. */
			if (larger == NULL) {
				free(output);
				(void)close(descriptors[0]);
				(void)waitpid(child, NULL, 0);

				/* Reports successful completion. */
				return 0;
			}
			output = larger;
		}
		memcpy(output + length, chunk, (size_t)count);
		length += (size_t)count;
	}
	(void)close(descriptors[0]);

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) < 0) {
		free(output);

		/* Reports successful completion. */
		return 0;
	}
	while (length != 0 && output[length - 1U] == '\n')
		length--;

	/* Handles the output availability. */
	if (output == NULL) {
		output = malloc(1U);

		/* Handles the output availability. */
		if (output == NULL)
			return 0;
	}
	output[length] = '\0';
	*result = output;
	/* Reports operation failure. */
	return 1;
}
