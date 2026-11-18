/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The running of recipes.
 *
 * Each line of a recipe is expanded, then split at the newlines a
 * variable may bring (a backslash-newline stays and goes to the shell).
 * Each command loses its leading @, - and + (which may also come from a
 * variable, as automake's silent rules do), is echoed unless silent, and
 * runs as $(SHELL) -c command, one shell per command, as POSIX says.
 * With -n a command is only echoed, unless it has + or the line names
 * $(MAKE).  A failure stops the recipe unless it is ignored.  An
 * interrupt removes the targets that were being made, unless they are
 * precious.
 *
 * A recipe runs as a job: its commands one after another, while other
 * jobs run theirs (-j).  A make has one job of its own; each further job
 * takes a token from the jobserver, a pipe that the top make fills with
 * one token for each job beyond the first and that recursive makes
 * share through MAKEFLAGS (--jobserver-auth=R,W, as GNU make writes it).
 * A token goes back into the pipe when its job ends.
 */

#include "make.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The byte that stands for one job in the jobserver's pipe. */
#define JOB_TOKEN '+'

/*
 * A recipe that is running: its target, its lines and the variables they
 * expand with, how far it has got, and the shell running a command now.
 *
 * A job lives from job_start until job_end, which frees it and the
 * recipe's variables it owns.
 */
struct job {
	struct job *next;
	struct target *target;
	const struct recipe *recipe;
	struct recipe_variables *variables;
	struct expansion context;
	char **environment;
	size_t line_index;		/* the line being run */
	char *expanded;			/* that line expanded, or NULL before the first */
	char *command;			/* the next command of the expansion, or NULL when none is left */
	int force;			/* the line names $(MAKE), so -n runs it */
	int ignore_errors;		/* the target's failures are ignored (-i, .IGNORE) */
	int silent;			/* the target's commands are not echoed (-s, .SILENT) */
	int command_ignored;		/* the running command had - */
	pid_t child;			/* the shell running a command */
	int holds_token;		/* the job took a token from the jobserver */
	int removable;			/* an interrupt may remove its target */
	int existed;			/* its target's file before the recipe */
	long long seconds;		/* that file's time before the recipe */
	long nanoseconds;
};

/*
 * The jobs that are running, newest first.  job_start adds a job and
 * job_end takes it out; the interrupt handler walks the list.
 */
static struct job *jobs;

/* The number of jobs on the list. */
static size_t job_count;

/*
 * Whether the job this make has without a token is running.  The first
 * job to start takes it; any other takes a token.
 */
static int own_job_busy;

/*
 * The reading end of the jobserver's pipe, or -1 when there is none: a
 * make runs one job at a time then, unless -j gave no limit.
 */
static int server_read = -1;

/* The writing end of the jobserver's pipe, or -1. */
static int server_write = -1;

/*
 * The tokens this make has taken from the pipe but not yet given to a
 * job.  A token no recipe wants goes back before make waits.
 */
static size_t spare_tokens;

/*
 * Whether the last walk found a recipe with no free job, so that the next
 * wait also waits for a token.
 */
static int token_wanted;

/*
 * The flags a recursive make inherits for the jobs in MAKEFLAGS: -jN and
 * the jobserver, set once by job_server_start.
 */
static char server_flags[64];

/*
 * The descriptor a wait for a token reads, or -1.  It is a copy of the
 * pipe's reading end that the end of a child closes (in the SIGCHLD
 * handler), so that the read returns rather than waiting on while a job
 * has ended.
 */
static volatile sig_atomic_t token_descriptor = -1;

static int job_advance(struct job *job);
static int job_next_line(struct job *job);
static int start_command(struct job *job, char *command);
static void job_end(struct job *job);
static void inherit_server(const char *inherited);
static void create_server(void);
static void wait_for_token(void);
static void reap(int block);
static void child_ended(pid_t child, int raw_status);
static void remember_target(struct job *job);
static int target_changed(const struct job *job);
static void give_token(void);
static void child_signal(int signal_number);
static int names_make(const char *line);
static char *shell_of(const struct expansion *context);
static int spawn(const char *shell, const char *command, char **environment, int output);
static int wait_for(int child);
static int shell_status(int raw_status);
static void report_failure(const struct target *target, const struct recipe *recipe, size_t line_index, int status, int ignored);
static void interrupted(int signal_number);

/*
 * Starts a target's recipe as a job, with its automatic variables in a
 * scope; the job owns the variables.  Returns UPDATE_PENDING while a
 * command runs; a recipe that ends without running one (-n, or empty
 * commands) returns 0, or 1 when it failed.
 */
int
job_start(
	struct target *target,
	const struct recipe *recipe,
	struct recipe_variables *variables,
	const struct variable_scope *scope,
	int ignore_errors,
	int silent)
{
	struct job *job;
	int outcome;

	/* Makes the job, before its first line. */
	job = make_malloc(sizeof(*job));
	memset(job, 0, sizeof(*job));
	job->target = target;
	job->recipe = recipe;
	job->variables = variables;
	job->ignore_errors = ignore_errors;
	job->silent = silent;
	job->child = -1;

	/* The lines expand in the recipe's scope, with the target's automatic variables. */
	job->context.scope = scope;
	job->context.automatic = &variables->automatic;
	job->context.file = recipe->file;
	job->context.line = recipe->line;
	job->environment = variable_environment(scope, &variables->automatic);
	remember_target(job);

	/* Takes the job this make has of its own, or a token taken for it. */
	if (!own_job_busy) {
		own_job_busy = 1;
	} else if (server_read >= 0) {
		spare_tokens--;
		job->holds_token = 1;
	}

	/* Puts the job on the list of running jobs. */
	job->next = jobs;
	jobs = job;
	job_count++;

	/* Runs the first command; a recipe that runs nothing ends at once. */
	outcome = job_advance(job);
	if (outcome == UPDATE_PENDING)
		return UPDATE_PENDING;

	/* Ends the recipe that ran nothing. */
	job_end(job);

	/* Succeeded: 0, or 1 when the recipe failed. */
	return outcome;
}

/*
 * Reports whether a recipe may start now: this make's own job is free,
 * or there is no limit, or a token is at hand.
 */
int
job_slot_free(
	void)
{
	/* .NOTPARALLEL: one recipe at a time. */
	if (make_not_parallel) {
		if (job_count == 0)
			return 1;
		return 0;
	}

	/* This make's own job. */
	if (!own_job_busy)
		return 1;

	/* -j without a number, and no jobserver above. */
	if (make_options.jobs == 0 && server_read < 0)
		return 1;

	/* A serial make. */
	if (server_read < 0)
		return 0;

	/* A token that was taken already. */
	if (spare_tokens > 0)
		return 1;

	/* No job is free. */
	return 0;
}

/*
 * Notes that the walk has a recipe, or may have one, that found no free
 * job: the next wait also waits for a token.
 */
void
job_want_slot(
	void)
{
	/* Only a jobserver has tokens to wait for. */
	if (server_read >= 0 && !make_not_parallel)
		token_wanted = 1;
}

/*
 * Waits until something changes for the walk: a command ends (its job
 * goes on to the next, or ends), or a token comes when a recipe wanted
 * one.
 */
void
job_wait(
	void)
{
	/* Nothing runs: the walk would wait for ever. */
	if (job_count == 0)
		make_fatal("internal error: waiting with no recipe running");

	/* Tokens that no recipe wanted in the last walk go back for other makes. */
	if (!token_wanted)
		job_server_finish();

	/* A token or the end of a child, whichever comes first. */
	if (token_wanted && server_read >= 0) {
		token_wanted = 0;
		wait_for_token();
		return;
	}

	/* The end of a child, then any others that ended with it. */
	reap(1);
	reap(0);
}

/* Returns the number of recipes that are running. */
size_t
job_running(
	void)
{
	/* Reports the length of the list of running jobs. */
	return job_count;
}

/*
 * Sets up the jobs of this make from -j and from the jobserver named in
 * MAKEFLAGS (inherited, or NULL).  A make given -jN on its own command
 * line starts a jobserver of its own, as GNU make does.
 */
void
job_server_start(
	const char *inherited,
	int jobs_on_command_line)
{
	/* A jobserver from the make above, unless -j was given here. */
	if (inherited != NULL && !jobs_on_command_line) {
		inherit_server(inherited);
	} else if (inherited != NULL && make_options.jobs != 1) {
		make_message("warning: -j%d forced in submake: resetting jobserver mode.",
			     make_options.jobs);
	}

	/* -jN with more than one job and no jobserver yet starts one. */
	if (server_read < 0 && make_options.jobs > 1)
		create_server();

	/* Writes what a recursive make inherits. */
	server_flags[0] = '\0';
	if (server_read >= 0) {
		snprintf(server_flags,
			 sizeof(server_flags),
			 "-j%d --jobserver-auth=%d,%d",
			 make_options.jobs,
			 server_read,
			 server_write);
	} else if (make_options.jobs == 0) {
		snprintf(server_flags, sizeof(server_flags), "-j");
	}
}

/* Returns the -j flags a recursive make inherits in MAKEFLAGS, or "". */
const char *
job_server_flags(
	void)
{
	/* Reports the flags job_server_start wrote. */
	return server_flags;
}

/* Gives the tokens this make holds back to the jobserver before it ends. */
void
job_server_finish(
	void)
{
	/* Gives back each spare token. */
	while (spare_tokens > 0) {
		give_token();
		spare_tokens--;
	}
}

/*
 * Runs a command through the shell and returns what it wrote to standard
 * output ($(shell) and !=); its exit status goes to *status.
 */
char *
job_shell_output(
	const char *command,
	int *status)
{
	struct expansion context;
	struct buffer output;
	char **environment;
	char *shell;
	char *text;
	char chunk[4096];
	ssize_t count;
	int pipe_ends[2];
	int child;
	int error;

	/* Finds the shell and the environment of the global scope. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = NULL;
	context.line = 0;
	shell = shell_of(&context);
	environment = variable_environment(&make_global_scope, NULL);

	/* Creates the pipe the command writes into. */
	memset(&output, 0, sizeof(output));
	error = pipe(pipe_ends);
	if (error != 0)
		make_fatal("pipe: %s", strerror(errno));

	/* Starts the command, after what make wrote so far. */
	fflush(stdout);
	child = spawn(shell, command, environment, pipe_ends[1]);

	/* Reads everything the command writes, until it closes the pipe. */
	close(pipe_ends[1]);
	for (;;) {
		count = read(pipe_ends[0], chunk, sizeof(chunk));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		buffer_add(&output, chunk, (size_t)count);
	}

	/* The command has closed its output. */
	close(pipe_ends[0]);

	/* Waits for the command and frees what running it needed. */
	*status = wait_for(child);
	free(shell);
	variable_free_environment(environment);
	text = buffer_finish(&output);

	/* Succeeded: the output; the status is stored. */
	return text;
}

/*
 * Makes an interrupt, a hang-up or a termination remove the target whose
 * recipe was running (when it changed and is not precious) before make
 * ends.
 */
void
job_install_signals(
	void)
{
	struct sigaction action;

	/* The same handler for each; the signal ends make afterwards. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = interrupted;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
}

/*
 * Runs a job's commands until one is running in a child: each line is
 * expanded when it is reached, then each command of the expansion (one
 * per line of it, a backslash-newline staying inside) in turn.  Returns
 * UPDATE_PENDING while a command runs, or 0 when the recipe is over.
 */
static int
job_advance(
	struct job *job)
{
	char *command;
	char *end;
	int running;
	int more;

	/* Starts commands until one runs in a child. */
	for (;;) {
		/* Goes to the next line when the commands of this one are done. */
		if (job->command == NULL) {
			more = job_next_line(job);
			if (!more)
				return 0;
		}

		/* Cuts the next command at a newline that no backslash escapes. */
		command = job->command;
		end = strchr(command, '\n');
		while (end != NULL && end > command && end[-1] == '\\')
			end = strchr(end + 1, '\n');
		job->command = NULL;
		if (end != NULL) {
			*end = '\0';
			job->command = end + 1;
		}

		/* Starts the command, which may run in a child. */
		running = start_command(job, command);
		if (running)
			return UPDATE_PENDING;
	}
}

/*
 * Expands the next line of a job's recipe into its commands.  Returns 0
 * when the recipe has no more lines.
 */
static int
job_next_line(
	struct job *job)
{
	size_t index;

	/* Leaves the line whose commands are done. */
	if (job->expanded != NULL) {
		free(job->expanded);
		job->expanded = NULL;
		job->line_index++;
	}

	/* The recipe is over after its last line. */
	index = job->line_index;
	if (index >= job->recipe->count)
		return 0;

	/* A line that names $(MAKE) runs even with -n (GNU make and POSIX). */
	job->force = names_make(job->recipe->lines[index]);

	/* Expands the line; its commands are taken from the expansion. */
	job->context.line = job->recipe->line_numbers[index];
	job->expanded = expand(&job->context, job->recipe->lines[index]);
	job->command = job->expanded;

	/* Succeeded: the line's commands are next. */
	return 1;
}

/*
 * Starts one command: its prefixes decide whether it is echoed, whether
 * its failure counts, and whether -n runs it.  Returns 1 when a child
 * runs it, and 0 when there is nothing to run.
 */
static int
start_command(
	struct job *job,
	char *command)
{
	struct expansion context;
	char *shell;
	int quiet;
	int ignore;
	int force;

	/* Reads the prefixes @ (silent), - (ignore) and +, with blanks among them. */
	quiet = job->silent;
	ignore = job->ignore_errors;
	force = job->force;
	for (;;) {
		if (*command == '@') {
			quiet = 1;
		} else if (*command == '-') {
			ignore = 1;
		} else if (*command == '+') {
			force = 1;
		} else if (*command != ' ' && *command != '\t') {
			break;
		}

		/* Steps past the prefix. */
		command++;
	}

	/* With -n the command is shown; only a forced one also runs. */
	if (make_options.dry_run) {
		printf("%s\n", command);
		if (!force)
			return 0;
	} else if (!quiet) {
		printf("%s\n", command);
	}

	/* An empty command does nothing. */
	if (command[0] == '\0')
		return 0;

	/* Finds the shell of the global scope. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = NULL;
	context.line = 0;
	shell = shell_of(&context);

	/* Starts the shell on the command, after what make wrote so far. */
	fflush(stdout);
	job->command_ignored = ignore;
	job->child = spawn(shell, command, job->environment, -1);
	free(shell);

	/* Succeeded: the child runs it. */
	return 1;
}

/*
 * Ends a job: its token goes back and what it held is freed.  The caller
 * has told the walk how the recipe ended.
 */
static void
job_end(
	struct job *job)
{
	struct job **link;

	/* Takes the job out of the list of running jobs. */
	for (link = &jobs; *link != NULL; link = &(*link)->next) {
		if (*link == job) {
			*link = job->next;
			break;
		}
	}

	/* One job fewer runs. */
	job_count--;

	/* Gives back its token, or frees this make's own job. */
	if (job->holds_token) {
		give_token();
	} else {
		own_job_busy = 0;
	}

	/* Frees what the recipe held. */
	free(job->expanded);
	variable_free_environment(job->environment);
	free(job->variables->newer.text);
	free(job->variables->unique.text);
	free(job->variables->all.text);
	free(job->variables->order_only.text);
	free(job->variables);
	free(job);
}

/*
 * Takes the jobserver from the make above, named in MAKEFLAGS as "R,W".
 * Without both descriptors open, this make runs one job at a time.
 */
static void
inherit_server(
	const char *inherited)
{
	const char *cursor;
	char *end;
	int read_end;
	int write_end;
	int matched;
	int read_open;
	int write_open;

	/* Reads the two descriptors. */
	read_end = (int)strtol(inherited, &end, 10);
	write_end = -1;
	matched = 0;
	if (end != inherited && *end == ',') {
		cursor = end + 1;
		write_end = (int)strtol(cursor, &end, 10);
		if (end != cursor && *end == '\0')
			matched = 1;
	}

	/* The two descriptors must be open in this make. */
	read_open = -1;
	write_open = -1;
	if (matched) {
		read_open = fcntl(read_end, F_GETFD);
		if (read_open >= 0)
			write_open = fcntl(write_end, F_GETFD);
	}

	/* Uses the jobserver, or falls back to one job at a time. */
	if (read_open >= 0 && write_open >= 0) {
		server_read = read_end;
		server_write = write_end;
		make_options.jobs = 2;
	} else {
		make_message("warning: jobserver unavailable: using -j1.  Add '+' to parent make rule.");
		make_options.jobs = 1;
	}
}

/* Starts a jobserver: a pipe with a token for each job beyond the first. */
static void
create_server(
	void)
{
	ssize_t written;
	char token;
	int descriptors[2];
	int error;
	int index;

	/* Creates the pipe. */
	error = pipe(descriptors);
	if (error != 0)
		make_fatal("pipe: %s", strerror(errno));
	server_read = descriptors[0];
	server_write = descriptors[1];

	/* Fills it with the tokens. */
	token = JOB_TOKEN;
	for (index = 1; index < make_options.jobs; index++) {
		written = write(server_write, &token, 1);
		if (written != 1)
			make_fatal("jobserver: %s", strerror(errno));
	}
}

/*
 * Waits for a token from the jobserver or for the end of a child.  The
 * read is on a copy of the pipe's descriptor that the end of a child
 * closes, so that a child that ends before or during the read cannot
 * leave make waiting on a pipe that no one fills.
 */
static void
wait_for_token(
	void)
{
	struct sigaction action;
	struct sigaction previous;
	sigset_t block;
	sigset_t saved;
	ssize_t count;
	char token;
	int descriptor;

	/* The end of a child closes the copy; it interrupts the read too. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = child_signal;
	sigemptyset(&action.sa_mask);
	sigaction(SIGCHLD, &action, &previous);
	sigemptyset(&block);
	sigaddset(&block, SIGCHLD);

	/* Makes the copy while a child's end cannot close it half made. */
	sigprocmask(SIG_BLOCK, &block, &saved);
	token_descriptor = dup(server_read);
	sigprocmask(SIG_SETMASK, &saved, NULL);

	/* A child that ended before the copy was made is taken now. */
	reap(0);

	/* Reads a token, or returns at the end of a child. */
	count = -1;
	descriptor = token_descriptor;
	if (descriptor >= 0)
		count = read(descriptor, &token, 1);

	/* Closes the copy, unless the signal has closed it. */
	sigprocmask(SIG_BLOCK, &block, &saved);
	if (token_descriptor >= 0) {
		close(token_descriptor);
		token_descriptor = -1;
	}

	/* Puts back the mask and the handler from before the wait. */
	sigprocmask(SIG_SETMASK, &saved, NULL);
	sigaction(SIGCHLD, &previous, NULL);

	/* A token is kept for the next recipe. */
	if (count == 1)
		spare_tokens++;

	/* Takes any child that ended. */
	reap(0);
}

/* Takes the ends of children: one, waiting for it (block), or all that ended. */
static void
reap(
	int block)
{
	pid_t child;
	int raw_status;
	int options;

	/* Takes each child that ended; only one when waiting. */
	options = WNOHANG;
	if (block)
		options = 0;
	for (;;) {
		child = waitpid((pid_t)-1, &raw_status, options);
		if (child < 0 && errno == EINTR)
			continue;
		if (child <= 0)
			return;

		/* Moves the child's job on. */
		child_ended(child, raw_status);
		if (block)
			return;
	}
}

/*
 * Takes the end of a child: the job goes on to its next command, or ends
 * when the recipe is over or the command failed and was not ignored.
 */
static void
child_ended(
	pid_t child,
	int raw_status)
{
	struct job *job;
	int status;
	int outcome;

	/* Finds the job the child belongs to. */
	for (job = jobs; job != NULL; job = job->next) {
		if (job->child == child)
			break;
	}

	/* A child that belongs to no job is not the walk's. */
	if (job == NULL)
		return;
	job->child = -1;

	/* A failure is reported; unless it is ignored, it ends the recipe. */
	status = shell_status(raw_status);
	if (status != 0) {
		report_failure(job->target,
			       job->recipe,
			       job->line_index,
			       status,
			       job->command_ignored);
		if (!job->command_ignored) {
			update_recipe_done(job->target, 1);
			job_end(job);
			return;
		}
	}

	/* Starts the next command, unless the recipe is over. */
	outcome = job_advance(job);
	if (outcome == UPDATE_PENDING)
		return;

	/* Tells the walk that the recipe succeeded, and ends the job. */
	update_recipe_done(job->target, 0);
	job_end(job);
}

/* Puts a token back into the jobserver's pipe. */
static void
give_token(
	void)
{
	char token;
	ssize_t count;

	/* Writes the byte, through interruptions. */
	token = JOB_TOKEN;
	for (;;) {
		count = write(server_write, &token, 1);
		if (count == 1)
			return;
		if (count < 0 && errno == EINTR)
			continue;
		make_fatal("jobserver: %s", strerror(errno));
	}
}

/* Closes the descriptor a wait for a token reads, when a child ends. */
static void
child_signal(
	int signal_number)
{
	int descriptor;

	/* Closes the copy, once. */
	(void)signal_number;
	descriptor = token_descriptor;
	if (descriptor >= 0) {
		token_descriptor = -1;
		close(descriptor);
	}
}

/* Reports whether a recipe line, unexpanded, names $(MAKE) or ${MAKE}. */
static int
names_make(
	const char *line)
{
	const char *found;

	/* Looks for either spelling. */
	found = strstr(line, "$(MAKE)");
	if (found != NULL)
		return 1;
	found = strstr(line, "${MAKE}");
	if (found != NULL)
		return 1;

	/* The line names neither. */
	return 0;
}

/* Returns the shell that runs commands: $(SHELL), or /bin/sh. */
static char *
shell_of(
	const struct expansion *context)
{
	struct variable *variable;
	char *shell;

	/* Finds SHELL as the makefiles set it; without it, /bin/sh. */
	variable = variable_lookup(context->scope, "SHELL", 5);
	if (variable == NULL) {
		shell = make_strdup("/bin/sh");
		return shell;
	}

	/* Expands the value, which may name other variables. */
	shell = expand(context, variable->value);

	/* An empty SHELL is /bin/sh too. */
	if (shell[0] == '\0') {
		free(shell);
		shell = make_strdup("/bin/sh");
		return shell;
	}

	/* Succeeded: the shell, which the caller frees. */
	return shell;
}

/*
 * Starts the shell on a command in a child process, with standard output
 * sent to a descriptor (or left as it is for -1).  Returns the child.
 * posix_spawn starts it without copying make's memory; a shell that
 * cannot be started is left to a forked child to report, as before.
 */
static int
spawn(
	const char *shell,
	const char *command,
	char **environment,
	int output)
{
	posix_spawn_file_actions_t actions;
	char *arguments[4];
	pid_t spawned;
	int child;
	int error;

	/* The shell's arguments: -c, or -ec under .POSIX. */
	arguments[0] = "sh";
	arguments[1] = "-c";
	if (make_posix)
		arguments[1] = "-ec";
	arguments[2] = (char *)command;
	arguments[3] = NULL;

	/* Starts the shell with posix_spawn, its output where it was asked for. */
	error = posix_spawn_file_actions_init(&actions);
	if (error == 0 && output >= 0)
		error = posix_spawn_file_actions_adddup2(&actions, output, 1);
	if (error == 0)
		error = posix_spawn(&spawned, shell, &actions, NULL, arguments, environment);
	(void)posix_spawn_file_actions_destroy(&actions);

	/* The spawned shell is the child. */
	if (error == 0)
		return (int)spawned;

	/* Otherwise a forked child becomes the shell, or says why it cannot. */
	child = fork();
	if (child < 0)
		make_fatal("fork: %s", strerror(errno));
	if (child == 0) {
		if (output >= 0)
			dup2(output, 1);
		execve(shell, arguments, environment);
		fprintf(stderr, "%s: %s: %s\n", make_program(), shell, strerror(errno));
		_exit(127);
	}

	/* Succeeded: the child. */
	return child;
}

/*
 * Waits for a child and returns its status as the shell would: the exit
 * status, or 128 and the signal that ended it.
 */
static int
wait_for(
	int child)
{
	pid_t done;
	int raw_status;
	int status;

	/* Waits for the child's end, through interruptions. */
	for (;;) {
		done = waitpid((pid_t)child, &raw_status, 0);
		if (done >= 0)
			break;
		if (errno != EINTR)
			return 127;
	}

	/* Turns the raw status into the shell's. */
	status = shell_status(raw_status);

	/* Succeeded: the child's status. */
	return status;
}

/*
 * Returns a child's status as the shell would: the exit status, or 128
 * and the signal that ended it.
 */
static int
shell_status(
	int raw_status)
{
	int exited;
	int signalled;

	/* An exit gives its status. */
	exited = WIFEXITED(raw_status);
	if (exited)
		return WEXITSTATUS(raw_status);

	/* A signal gives 128 and its number. */
	signalled = WIFSIGNALED(raw_status);
	if (signalled)
		return 128 + WTERMSIG(raw_status);

	/* Anything else is a failure. */
	return 1;
}

/*
 * Reports a failed command as GNU make does: "make: *** [file:line:
 * target] Error N", or with "(ignored)" and no stars when it is ignored.
 */
static void
report_failure(
	const struct target *target,
	const struct recipe *recipe,
	size_t line_index,
	int status,
	int ignored)
{
	/* What make wrote so far goes first. */
	fflush(stdout);

	/* An ignored failure. */
	if (ignored) {
		fprintf(stderr,
			"%s: [%s:%ld: %s] Error %d (ignored)\n",
			make_program(),
			recipe->file,
			recipe->line_numbers[line_index],
			target->name,
			status);
		return;
	}

	/* A failure that stops the recipe. */
	fprintf(stderr,
		"%s: *** [%s:%ld: %s] Error %d\n",
		make_program(),
		recipe->file,
		recipe->line_numbers[line_index],
		target->name,
		status);
}

/* Records a job's target as the recipe starts, for the interrupt handler. */
static void
remember_target(
	struct job *job)
{
	struct stat status;
	int error;

	/* A phony or precious target is never removed. */
	job->removable = 1;
	if (job->target->phony || job->target->precious)
		job->removable = 0;

	/* Keeps its time now, to tell whether the recipe changed it. */
	error = stat(job->target->name, &status);
	job->existed = 0;
	if (error == 0) {
		job->existed = 1;
		job->seconds = (long long)status.st_mtim.tv_sec;
		job->nanoseconds = (long)status.st_mtim.tv_nsec;
	}
}

/*
 * Reports whether a running recipe has changed its target's file: made it,
 * or given it another time.  Called from the interrupt handler.
 */
static int
target_changed(
	const struct job *job)
{
	struct stat status;
	int error;

	/* A file that is not there was not made. */
	error = stat(job->target->name, &status);
	if (error != 0)
		return 0;

	/* A file that was not there before was made. */
	if (!job->existed)
		return 1;

	/* A file with another time was changed. */
	if ((long long)status.st_mtim.tv_sec != job->seconds)
		return 1;
	if ((long)status.st_mtim.tv_nsec != job->nanoseconds)
		return 1;

	/* The file is as it was. */
	return 0;
}

/*
 * Handles an interrupt: removes the target of each running recipe when
 * the recipe changed it, then ends make by the same signal.
 */
static void
interrupted(
	int signal_number)
{
	struct job *job;
	const char *name;
	int changed;

	/* Removes each running recipe's target, when it is removable and the recipe touched it. */
	for (job = jobs; job != NULL; job = job->next) {
		if (!job->removable)
			continue;

		/* Leaves a target the recipe did not change. */
		changed = target_changed(job);
		if (!changed)
			continue;

		/* Says so, with write since this is a signal handler, and removes it. */
		name = job->target->name;
		(void)write(2, "make: *** Deleting file '", 25);
		(void)write(2, name, strlen(name));
		(void)write(2, "'\n", 2);
		unlink(name);
	}

	/* Raises the signal again, with its default action. */
	signal(signal_number, SIG_DFL);
	kill(getpid(), signal_number);
}
