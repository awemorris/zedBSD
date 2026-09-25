/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The table of builtin utilities, how a builtin is run, and the small
 * builtins that have no file of their own: :, true, false, alias, unalias,
 * times, clear, env and help.  Every builtin returns its exit status.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/alias.h"
#include "userland/base/sh/vars.h"

#include <uapi/console.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <unistd.h>

static int builtin_colon(int argc, char **argv);
static int builtin_false(int argc, char **argv);
static int builtin_alias(int argc, char **argv);
static int builtin_unalias(int argc, char **argv);
static int builtin_times(int argc, char **argv);
static int builtin_clear(int argc, char **argv);
static int builtin_env(int argc, char **argv);
static int builtin_help(int argc, char **argv);
static void alias_show(const char *name, int *status);
static void alias_define(const char *word, const char *equals, int *status);
static void print_time(clock_t value, long ticks, char after);
static void env_child(int argc, char **argv, int index, int clean);

/* Every builtin, sorted by name. */
static const struct sh_builtin builtins[] = {
	{ ".", sh_builtin_dot, SH_BUILTIN_SPECIAL },
	{ ":", builtin_colon, SH_BUILTIN_SPECIAL },
	{ "[", sh_builtin_test, 0 },
	{ "alias", builtin_alias, 0 },
	{ "bg", sh_bg_builtin, 0 },
	{ "break", sh_builtin_break, SH_BUILTIN_SPECIAL },
	{ "cd", sh_builtin_cd, 0 },
	{ "chdir", sh_builtin_cd, 0 },
	{ "clear", builtin_clear, 0 },
	{ "command", sh_builtin_command, 0 },
	{ "continue", sh_builtin_break, SH_BUILTIN_SPECIAL },
	{ "echo", sh_builtin_echo, 0 },
	{ "env", builtin_env, 0 },
	{ "eval", sh_builtin_eval, SH_BUILTIN_SPECIAL },
	{ "exec", sh_builtin_exec, SH_BUILTIN_SPECIAL },
	{ "exit", sh_builtin_exit, SH_BUILTIN_SPECIAL },
	{ "export", sh_builtin_export,
	  SH_BUILTIN_SPECIAL | SH_BUILTIN_DECLARATION },
	{ "false", builtin_false, 0 },
	{ "fc", sh_fc_builtin, 0 },
	{ "fg", sh_fg_builtin, 0 },
	{ "getopts", sh_getopts_builtin, 0 },
	{ "hash", sh_builtin_hash, 0 },
	{ "help", builtin_help, 0 },
	{ "jobs", sh_jobs_builtin, 0 },
	{ "kill", sh_kill_builtin, 0 },
	{ "local", sh_builtin_local,
	  SH_BUILTIN_SPECIAL | SH_BUILTIN_DECLARATION },
	{ "printf", sh_builtin_printf, 0 },
	{ "pwd", sh_builtin_pwd, 0 },
	{ "read", sh_builtin_read, 0 },
	{ "readonly", sh_builtin_export,
	  SH_BUILTIN_SPECIAL | SH_BUILTIN_DECLARATION },
	{ "return", sh_builtin_return, SH_BUILTIN_SPECIAL },
	{ "set", sh_set_builtin, SH_BUILTIN_SPECIAL },
	{ "shift", sh_builtin_shift, SH_BUILTIN_SPECIAL },
	{ "test", sh_builtin_test, 0 },
	{ "times", builtin_times, SH_BUILTIN_SPECIAL },
	{ "trap", sh_trap_builtin, SH_BUILTIN_SPECIAL },
	{ "true", builtin_colon, 0 },
	{ "type", sh_builtin_type, 0 },
	{ "ulimit", sh_builtin_ulimit, 0 },
	{ "umask", sh_builtin_umask, 0 },
	{ "unalias", builtin_unalias, 0 },
	{ "unset", sh_builtin_unset, SH_BUILTIN_SPECIAL },
	{ "wait", sh_wait_builtin, 0 },
	{ NULL, NULL, 0 }
};

/*
 * Finds a builtin by name.
 */
const struct sh_builtin *
sh_builtin_find(
	const char *name)
{
	int compare;
	int index;

	/* A short table, searched in order. */
	for (index = 0; builtins[index].name != NULL; index++) {
		compare = strcmp(builtins[index].name, name);
		if (compare == 0)
			return &builtins[index];
	}

	/* Not a builtin. */
	return NULL;
}

/*
 * Runs a builtin.  Output it could not write makes it fail.
 */
int
sh_builtin_run(
	const struct sh_builtin *builtin,
	int argc,
	char **argv)
{
	int status;
	int flushed;
	int failed;

	/* The builtin writes through stdio, which is flushed after it. */
	clearerr(stdout);
	status = builtin->run(argc, argv);
	flushed = fflush(stdout);
	failed = ferror(stdout);

	/*
	 * A write error fails a builtin that had succeeded, and what could not
	 * be written is dropped, so that it does not come out later on the
	 * standard output the redirection is undone to.
	 */
	if (flushed != 0 || failed) {
		fpurge(stdout);
		if (status == 0) {
			sh_warn("%s: write error: %s", argv[0],
				strerror(errno));
			status = 1;
		}

		/* The error is not left for the next builtin to see. */
		clearerr(stdout);
	}

	/* Succeeded: the builtin's status. */
	return status;
}

/* Implements : and true. */
static int
builtin_colon(
	int argc,
	char **argv)
{
	/* Does nothing, successfully. */
	(void)argc;
	(void)argv;
	return 0;
}

/* Implements false. */
static int
builtin_false(
	int argc,
	char **argv)
{
	/* Does nothing, unsuccessfully. */
	(void)argc;
	(void)argv;
	return 1;
}

/*
 * Implements alias: lists, prints, or defines aliases.
 */
static int
builtin_alias(
	int argc,
	char **argv)
{
	const char *equals;
	int index;
	int status;

	/* With no operands, every alias. */
	if (argc < 2) {
		sh_alias_print(NULL);
		return 0;
	}

	/* name=value defines; name alone prints. */
	status = 0;
	for (index = 1; index < argc; index++) {
		equals = strchr(argv[index], '=');
		if (equals == NULL)
			alias_show(argv[index], &status);
		else
			alias_define(argv[index], equals, &status);
	}

	/* Succeeded: 0 unless a name was missing or bad. */
	return status;
}

/* Prints an alias, or reports that there is none. */
static void
alias_show(
	const char *name,
	int *status)
{
	const char *value;

	/* A missing alias fails the builtin. */
	value = sh_alias_get(name);
	if (value == NULL) {
		fprintf(stderr, "alias: %s not found\n", name);
		*status = 1;
		return;
	}

	/* The alias, as it can be read back. */
	sh_alias_print(name);
}

/* Defines an alias from name=value. */
static void
alias_define(
	const char *word,
	const char *equals,
	int *status)
{
	char *name;
	int error;

	/* The name before the =, and the value after it. */
	name = sh_temp_own(sh_strndup(word, (size_t)(equals - word)));
	error = sh_alias_set(name, equals + 1);
	if (error != 0) {
		fprintf(stderr, "alias: %s: invalid alias name\n", name);
		*status = 1;
	}
}

/*
 * Implements unalias: removes aliases, or all of them with -a.
 */
static int
builtin_unalias(
	int argc,
	char **argv)
{
	int index;
	int status;
	int error;

	/* With no names nothing is removed (as dash). */
	if (argc < 2)
		return 0;

	/* -a removes all. */
	if (argv[1][0] == '-' && argv[1][1] == 'a' && argv[1][2] == '\0') {
		sh_alias_clear();
		return 0;
	}

	/* Each name. */
	status = 0;
	for (index = 1; index < argc; index++) {
		error = sh_alias_unset(argv[index]);
		if (error != 0) {
			fprintf(stderr, "unalias: %s not found\n", argv[index]);
			status = 1;
		}
	}

	/* Succeeded: 0 when every alias was there. */
	return status;
}

/*
 * Implements times: the user and system times of the shell, then of its
 * children, as minutes and seconds.
 */
static int
builtin_times(
	int argc,
	char **argv)
{
	struct tms usage;
	long ticks;

	/* The clock ticks of the four times. */
	(void)argc;
	(void)argv;
	(void)times(&usage);
	ticks = sysconf(_SC_CLK_TCK);
	if (ticks <= 0)
		ticks = 100;

	/* Two lines: the shell, then its children. */
	print_time(usage.tms_utime, ticks, ' ');
	print_time(usage.tms_stime, ticks, '\n');
	print_time(usage.tms_cutime, ticks, ' ');
	print_time(usage.tms_cstime, ticks, '\n');

	/* Succeeded. */
	return 0;
}

/* Prints a time in clock ticks as 0m0.000s, and a character after it. */
static void
print_time(
	clock_t value,
	long ticks,
	char after)
{
	long minutes;
	long seconds;
	long milliseconds;

	/* Minutes, seconds, and thousandths. */
	minutes = (long)(value / ticks / 60);
	seconds = (long)(value / ticks % 60);
	milliseconds = (long)(value % ticks * 1000 / ticks);
	printf("%ldm%ld.%03lds%c", minutes, seconds, milliseconds, after);
}

/* Implements clear: clears the console. */
static int
builtin_clear(
	int argc,
	char **argv)
{
	int cleared;

	/* The console clears itself; a terminal gets the ANSI sequence. */
	(void)argc;
	(void)argv;
	fflush(stdout);
	cleared = ioctl(1, KERN_CONSOLE_CLEAR);
	if (cleared != 0)
		printf("\033[H\033[2J");

	/* Succeeded. */
	return 0;
}

/*
 * Implements env: env [-i] [name=value...] [command [argument...]].  With no
 * command, prints the environment.
 */
static int
builtin_env(
	int argc,
	char **argv)
{
	pid_t child;
	void *job;
	int index;
	int clean;
	int status;

	/* -i (or -) starts from an empty environment. */
	clean = 0;
	index = 1;
	if (index < argc && argv[index][0] == '-') {
		if (argv[index][1] == '\0')
			clean = 1;
		else if (argv[index][1] == 'i' && argv[index][2] == '\0')
			clean = 1;
		if (clean)
			index++;
	}

	/* The rest runs in a child, which changes its own variables. */
	job = sh_job_new(0);
	child = sh_fork(SH_FORK_FOREGROUND, job);
	if (child == 0)
		env_child(argc, argv, index, clean);
	status = sh_job_wait_foreground(job);

	/* Succeeded: the command's status. */
	return status;
}

/*
 * The child of env: sets the variables, then prints the environment or runs
 * the command.  It does not return.
 */
static void
env_child(
	int argc,
	char **argv,
	int index,
	int clean)
{
	char path[PATH_MAX];
	char **environment;
	const char *equals;
	int found;

	/* -i clears what the shell exports. */
	if (clean)
		sh_var_clear_exports();

	/* Each name=value, exported. */
	for (; index < argc; index++) {
		equals = strchr(argv[index], '=');
		if (equals == NULL)
			break;
		(void)sh_var_set_assignment(argv[index], SH_VAR_EXPORT);
	}

	/* The environment the command runs with. */
	environment = sh_var_environment();

	/* With no command, the environment is printed. */
	if (index >= argc) {
		for (; *environment != NULL; environment++)
			printf("%s\n", *environment);
		fflush(stdout);
		_exit(0);
	}

	/* The command, found on PATH. */
	found = sh_find_command_path(argv[index], path, sizeof(path), 0);
	if (!found) {
		fprintf(stderr, "env: %s: not found\n", argv[index]);
		_exit(127);
	}

	/* Run; failing that, the reason. */
	sh_signals_for_exec();
	execve(path, argv + index, environment);
	fprintf(stderr, "env: %s: %s\n", argv[index], strerror(errno));
	_exit(126);
}

/* Implements help: lists the builtins. */
static int
builtin_help(
	int argc,
	char **argv)
{
	int index;

	/* One name per line. */
	(void)argc;
	(void)argv;
	printf("Builtin commands:\n");
	for (index = 0; builtins[index].name != NULL; index++)
		printf("  %s\n", builtins[index].name);

	/* Succeeded. */
	return 0;
}
