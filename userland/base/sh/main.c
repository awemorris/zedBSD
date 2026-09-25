/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The zedBSD sh command: startup (options, the variables the shell sets, the
 * profile of a login shell), and the loop that reads and runs commands from
 * a string (-c), a script, standard input or the terminal.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/expand.h"
#include "userland/base/sh/vars.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* How many end-of-file characters ignoreeof lets pass before exiting. */
#define IGNOREEOF_LIMIT 10

/* The environment the shell was started with, read once at startup. */
extern char **environ;

/*
 * The shell's own process, which $$ names in every subshell too.
 *
 * Set once at startup and never changed, so a subshell keeps its parent's.
 */
long sh_root_pid;

/* The last process started in the background, which $! names; 0 for none. */
long sh_last_background;

/* How many forks deep the shell is; 0 in the shell itself. */
int sh_subshell;

/* $0: the shell's name, or the script's or the -c command's name operand. */
const char *sh_arg0 = "sh";

/*
 * Set when the shell was started interactive without PS1: the prompt then
 * shows the host and the directory, as the zedBSD shell always has.
 */
static int default_prompt;

static int scan_startup_options(int argc, char **argv, int *login);
static int run_interactive(void);
static void run_profile(const char *path);
static void run_login_profiles(void);
static void set_default_variables(void);
static int open_script(const char *path);

/*
 * Runs the sh command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *command;
	const char *script;
	const char *environment_file;
	int index;
	int command_mode;
	int descriptor;
	int login;
	int parsed;
	int status;
	int input_terminal;
	int error_terminal;
	int job_control;

	/* The shell's identity, the signals it was given, and its variables. */
	sh_root_pid = (long)getpid();
	sh_trap_init();
	sh_var_import_environment(environ);
	sh_var_hook("PATH", sh_path_changed);
	sh_var_hook("OPTIND", sh_getopts_reset);
	if (argc > 0 && argv[0] != NULL)
		sh_arg0 = argv[0];

	/* -c and -l are startup options; the rest are set's. */
	command_mode = scan_startup_options(argc, argv, &login);
	index = 1;
	parsed = sh_options_parse(argc, argv, &index, 1);
	if (parsed != 0)
		return 2;

	/* The operands: the command string, or the script, then $0 and $@. */
	command = NULL;
	script = NULL;
	if (command_mode) {
		if (index >= argc) {
			fprintf(stderr, "sh: -c requires an argument\n");
			return 2;
		}

		/* The command string, then $0 and the positional parameters. */
		command = argv[index++];
		if (index < argc)
			sh_arg0 = argv[index++];
	} else if (!sh_option[SH_OPT_STDIN] && index < argc) {
		script = argv[index++];
		sh_arg0 = script;
	} else {
		sh_option[SH_OPT_STDIN] = 1;
	}

	/* The rest of the arguments are the positional parameters. */
	sh_parameters.count = argc - index;
	sh_parameters.values = argv + index;
	sh_parameters.owned = 0;

	/* A shell reading commands from a terminal is interactive. */
	if (command == NULL && script == NULL) {
		input_terminal = isatty(0);
		error_terminal = isatty(2);
		if (input_terminal && error_terminal)
			sh_option[SH_OPT_INTERACTIVE] = 1;
	}

	/* An interactive shell has job control. */
	if (sh_option[SH_OPT_INTERACTIVE])
		sh_option[SH_OPT_MONITOR] = 1;
	set_default_variables();

	/* An interactive shell handles signals and jobs itself. */
	if (sh_option[SH_OPT_INTERACTIVE]) {
		sh_job_control_init();
		job_control = sh_job_control_active();
		sh_signals_for_interactive(job_control);
		using_history();
	}

	/* A login shell reads the profiles; an interactive one reads $ENV. */
	if (login)
		run_login_profiles();
	environment_file = sh_var_get("ENV");
	if (sh_option[SH_OPT_INTERACTIVE] && environment_file != NULL)
		run_profile(environment_file);

	/* Commands from the terminal have a loop of their own. */
	if (command == NULL && script == NULL && sh_option[SH_OPT_INTERACTIVE]) {
		status = run_interactive();
		sh_exit(status);
	}

	/* Otherwise from the string, the script, or standard input. */
	if (command != NULL) {
		sh_input_push_string(command, strlen(command), 1);
	} else if (script != NULL) {
		descriptor = open_script(script);
		sh_input_push_file(descriptor, 0);
	} else {
		sh_input_push_file(0, 0);
	}

	/* Runs them; an error ends a shell that is not interactive. */
	status = sh_run_noninteractive();
	sh_exit(status);
}

/*
 * Runs the commands of the input, ending the shell on an error: the loop of
 * a shell that is not interactive.
 */
int
sh_run_noninteractive(
	void)
{
	struct sh_handler handler;
	int kind;
	int status;

	/* An exception out of a command ends the shell with its status. */
	sh_handler_push(&handler);
	kind = setjmp(handler.environment);
	if (kind != 0) {
		sh_handler_unwind(&handler);
		if (kind == SH_EX_EXIT || sh_exception_status != 0)
			sh_exit(sh_exception_status);
		sh_exit(2);
	}

	/* The commands, up to the end, a top-level return, or an exit. */
	status = sh_eval_input(SH_EV_EXIT);
	sh_handler_pop(&handler);

	/* Succeeded: the status of the last command. */
	return status;
}

/*
 * Supplies the text of a prompt: PS1 (expanded) at the start of a command,
 * PS2 within one.
 */
const char *
sh_prompt_text(
	int which)
{
	static char text[PATH_MAX + 128];
	struct sh_expand_context context;
	struct sh_token token;
	const char *error_text;
	const char *prompt;
	char *expanded;
	char cwd[PATH_MAX];
	char host[65];
	char *named;
	int host_error;
	int expanded_ok;

	/* The prompt variable of the kind asked for. */
	if (which == 1)
		prompt = sh_var_get("PS1");
	else
		prompt = sh_var_get("PS2");

	/* Without PS1, the zedBSD prompt: user, host and directory. */
	if (which == 1 && default_prompt && prompt == NULL) {
		named = getcwd(cwd, sizeof(cwd));
		if (named == NULL)
			strcpy(cwd, "/");
		host_error = gethostname(host, sizeof(host));
		if (host_error != 0)
			strcpy(host, "zedbsd");
		host[sizeof(host) - 1] = '\0';
		snprintf(text, sizeof(text), "root@%s:%s$ ", host, cwd);
		return text;
	}

	/* PS1 as it was set, or nothing. */
	if (prompt == NULL)
		return "";

	/* The prompt is expanded as a word in double quotes would be. */
	memset(&token, 0, sizeof(token));
	token.raw = (char *)prompt;
	token.raw_length = strlen(prompt);
	token.heredoc = SH_HEREDOC_EXPAND;
	sh_expand_context_fill(&context);
	expanded_ok = sh_expand_word(&token, &context, &expanded, &error_text);
	if (!expanded_ok)
		return prompt;
	snprintf(text, sizeof(text), "%s", expanded);
	free(expanded);

	/* Succeeded: the prompt. */
	return text;
}

/*
 * Looks through the leading option words for -c, which takes the command
 * string, and -l, which makes a login shell; returns whether -c was given.
 * The option words themselves are read by sh_options_parse.
 */
static int
scan_startup_options(
	int argc,
	char **argv,
	int *login)
{
	const char *word;
	int command_mode;
	int position;
	int skip;
	int index;

	/* A name starting with - also makes a login shell. */
	*login = 0;
	if (argc > 0 && argv[0] != NULL && argv[0][0] == '-')
		*login = 1;

	/* Each word that starts with - or +, up to the first operand or --. */
	command_mode = 0;
	for (index = 1; index < argc; index++) {
		word = argv[index];
		if (word[0] != '-' && word[0] != '+')
			break;
		if (word[1] == '\0')
			break;
		if (word[0] == '-' && word[1] == '-' && word[2] == '\0')
			break;

		/* Each letter; every o takes the next word as its name. */
		skip = 0;
		for (position = 1; word[position] != '\0'; position++) {
			if (word[position] == 'c')
				command_mode = 1;
			else if (word[position] == 'l')
				*login = 1;
			else if (word[position] == 'o')
				skip++;
		}

		/* The arguments that the options took are skipped. */
		index += skip;
	}

	/* Succeeded: whether -c was given. */
	return command_mode;
}

/* Reads and runs commands from the terminal until the end of the input. */
static int
run_interactive(
	void)
{
	struct sh_handler handler;
	struct sh_arena *volatile arena;
	struct sh_node *node;
	volatile int eof_count;
	int kind;
	int eof;

	/* The terminal is the input. */
	sh_input_push_file(0, 1);
	arena = NULL;
	eof_count = 0;

	/*
	 * An error abandons the command and the rest of its line; the
	 * handler is installed again, and the loop goes on.
	 */
	sh_handler_push(&handler);
	kind = setjmp(handler.environment);
	if (kind == SH_EX_EXIT)
		sh_exit(sh_exception_status);
	if (kind != 0) {
		sh_handler_unwind(&handler);
		sh_handler_push(&handler);
		sh_arena_release(arena);
		arena = NULL;
		sh_status = sh_exception_status;
		if (sh_status == 0)
			sh_status = 2;
		sh_skip = SH_SKIP_NONE;
		sh_input_discard_line();
		if (kind == SH_EX_INT)
			fputc('\n', stderr);
	}

	/* Reads a command, runs it, and reports jobs before the next prompt. */
	for (;;) {
		sh_job_notify();
		sh_input_set_prompt(1);
		sh_interrupted = 0;
		arena = sh_arena_new();
		node = sh_parse_command(arena, &eof);

		/* The end of the input ends the shell, unless ignoreeof. */
		if (eof) {
			sh_arena_release(arena);
			arena = NULL;
			eof_count++;
			if (sh_option[SH_OPT_IGNOREEOF] &&
			    eof_count < IGNOREEOF_LIMIT) {
				fprintf(stderr, "Use \"exit\" to leave shell.\n");
				continue;
			}

			/* End of input ends the shell on a line of its own. */
			fputc('\n', stderr);
			break;
		}

		/* A command is run; a break or a return out of it is dropped. */
		eof_count = 0;
		if (node != NULL)
			(void)sh_eval(node, 0);
		sh_arena_release(arena);
		arena = NULL;
		sh_skip = SH_SKIP_NONE;
	}

	/* The shell's own handler goes. */
	sh_handler_pop(&handler);

	/* Succeeded: the status of the last command. */
	return sh_status;
}

/* Reads and runs the profiles of a login shell. */
static void
run_login_profiles(
	void)
{
	char path[PATH_MAX];
	const char *home;

	/* The system's profile, then the user's. */
	run_profile("/etc/profile");
	home = sh_var_get("HOME");
	if (home == NULL)
		return;
	snprintf(path, sizeof(path), "%s/.profile", home);
	run_profile(path);
}

/* Reads and runs a profile, if it is there; an error in it is reported. */
static void
run_profile(
	const char *path)
{
	struct sh_handler handler;
	int descriptor;
	int kind;

	/* A missing profile is not an error. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return;
	descriptor = sh_descriptor_high(descriptor);

	/* An error in it abandons the rest of it, not the shell. */
	sh_handler_push(&handler);
	kind = setjmp(handler.environment);
	if (kind != 0) {
		sh_handler_unwind(&handler);
		return;
	}

	/* Its commands run in this shell. */
	sh_input_push_file(descriptor, 0);
	(void)sh_eval_input(0);
	sh_input_pop();
	sh_skip = SH_SKIP_NONE;
	sh_handler_pop(&handler);
}

/* Sets the variables the shell provides. */
static void
set_default_variables(
	void)
{
	const char *value;
	char text[24];

	/* PATH and TERM, when the environment did not give them. */
	value = sh_var_get("PATH");
	if (value == NULL)
		(void)sh_var_set("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", SH_VAR_EXPORT);
	value = sh_var_get("TERM");
	if (value == NULL && sh_option[SH_OPT_INTERACTIVE])
		(void)sh_var_set("TERM", "zed", SH_VAR_EXPORT);

	/* IFS is always the default at startup, whatever the environment says. */
	(void)sh_var_set("IFS", " \t\n", 0);

	/* The prompts; an interactive shell without PS1 shows its own. */
	value = sh_var_get("PS1");
	if (value == NULL) {
		if (sh_option[SH_OPT_INTERACTIVE])
			default_prompt = 1;
		else
			(void)sh_var_set("PS1", "$ ", 0);
	}

	/* PS2 and PS4, when not set. */
	value = sh_var_get("PS2");
	if (value == NULL)
		(void)sh_var_set("PS2", "> ", 0);
	value = sh_var_get("PS4");
	if (value == NULL)
		(void)sh_var_set("PS4", "+ ", 0);

	/* OPTIND starts at 1; PPID is the parent. */
	(void)sh_var_set("OPTIND", "1", 0);
	snprintf(text, sizeof(text), "%ld", (long)getppid());
	(void)sh_var_set("PPID", text, 0);

	/* PWD is kept when it names the current directory. */
	sh_cwd_init();
}

/* Opens a script, out of the way of the descriptors scripts use. */
static int
open_script(
	const char *path)
{
	int descriptor;
	int error;

	/* A script that cannot be opened ends the shell (127 when missing). */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		error = errno;
		if (error == ENOENT) {
			fprintf(stderr, "sh: 0: cannot open %s: No such file\n",
				path);
			exit(127);
		}

		/* Any other failure. */
		fprintf(stderr, "sh: 0: cannot open %s: %s\n", path,
			strerror(error));
		exit(126);
	}

	/* Succeeded: the descriptor, moved to 10 or more. */
	descriptor = sh_descriptor_high(descriptor);
	return descriptor;
}
