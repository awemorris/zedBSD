/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Processes and jobs: forking, waiting, and the job table (POSIX XCU 2.9.3,
 * and the jobs, fg, bg, wait and kill builtins).
 *
 * Every command the shell forks for belongs to a job: a pipeline is one job
 * of several processes.  A job run in the foreground is waited for and
 * forgotten; one run in the background is kept in the table until it has
 * been waited for (or, in an interactive shell, reported done).  With job
 * control each job has a process group of its own and the terminal is given
 * to the job in the foreground.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/vars.h"

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* How a process of a job stands. */
#define PROCESS_RUNNING	0
#define PROCESS_STOPPED	1
#define PROCESS_DONE	2

/* How many ended jobs a shell that is not interactive keeps for wait. */
#define DONE_JOBS_MAX 1024

/* One process of a job, with what the system last reported of it. */
struct process {
	pid_t pid;
	int state;
	int status;
};

/*
 * One job: the processes of one pipeline or background command.
 *
 * A job in the table has a number from 1; a foreground job that has not
 * stopped is not in the table and has none.  The job owns its process array
 * and its text.
 */
struct job {
	struct job *next;
	int number;
	pid_t group;
	struct process *processes;
	int count;
	int capacity;
	int background;
	unsigned long order;
	char *text;
};

/* The table of jobs, in the order they were added. */
static struct job *job_table;

/*
 * The clock that orders jobs: a job gets the next tick when it is started
 * in the background or stops, which makes it the current job (%+).
 */
static unsigned long job_clock;

/* Set when the shell does job control: interactive, on its terminal. */
static int job_control;

/* The shell's own process group, given the terminal back after each job. */
static pid_t shell_group;

static struct job *job_find_spec(const char *spec, const char *command);
static struct job *job_find_numbered(const char *spec);
static struct job *job_find_text(const char *spec, int anywhere);
static void job_ranked(struct job **current, struct job **previous);
static struct job *job_of_pid(pid_t pid);
static void job_add_to_table(struct job *job);
static void job_limit_done(void);
static void job_remove(struct job *job);
static void job_free(struct job *job);
static int job_state(const struct job *job);
static int job_status(const struct job *job);
static void job_update(pid_t pid, int status);
static void process_record(struct process *process, int status);
static int job_wait(struct job *job, int interruptible, int *signalled);
static void job_continue(struct job *job);
static void job_print(struct job *job, int long_form, int groups_only);
static void job_state_text(const struct job *job, char *text, size_t size);
static char job_mark(const struct job *job);
static void set_terminal_group(pid_t group);
static void report_signal(int status);
static int kill_list(int argc, char **argv);
static int kill_name(const char *operand);
static int kill_signal(int argc, char **argv, int *index);
static int kill_send(const char *operand, int number);

/* Makes the text of a command, for the jobs listing (show.c). */
char *sh_node_text(struct sh_node *node);

/*
 * Starts job control when the shell is interactive and on its terminal.
 */
void
sh_job_control_init(
	void)
{
	int terminal;
	pid_t foreground;
	pid_t group;

	/* Only an interactive shell with monitor on does job control. */
	job_control = 0;
	if (!sh_option[SH_OPT_INTERACTIVE] || !sh_option[SH_OPT_MONITOR])
		return;

	/* The shell must already be the terminal's foreground group. */
	terminal = isatty(0);
	if (!terminal)
		return;
	foreground = tcgetpgrp(0);
	group = getpgrp();
	if (foreground != group)
		return;
	shell_group = group;
	job_control = 1;
}

/*
 * Reports whether the shell is doing job control.
 */
int
sh_job_control_active(
	void)
{
	/* Succeeded: whether it is. */
	return job_control;
}

/*
 * Makes a job, not yet in the table.
 */
void *
sh_job_new(
	int background)
{
	struct job *job;

	/* An empty job; processes are added as they are forked. */
	job = sh_malloc(sizeof(*job));
	memset(job, 0, sizeof(*job));
	job->background = background;

	/* Succeeded: the job. */
	return job;
}

/*
 * Records the command a job runs, for the jobs listing.
 */
void
sh_job_set_text(
	void *memory,
	struct sh_node *node)
{
	struct job *job;

	/* The first text given stands. */
	job = memory;
	if (job == NULL || job->text != NULL)
		return;
	job->text = sh_node_text(node);
}

/*
 * Forks a process for a job (or for no job).  In the child, the shell
 * becomes a subshell: its traps and job control are reset.  Returns the
 * child's pid in the shell and 0 in the child.
 */
pid_t
sh_fork(
	int mode,
	void *memory)
{
	struct job *job;
	struct process *process;
	pid_t child;

	/* Output buffered now must not be written twice. */
	job = memory;
	fflush(NULL);
	child = fork();
	if (child < 0)
		sh_error("cannot fork: %s", strerror(errno));

	/* The child is a subshell. */
	if (child == 0) {
		sh_fork_child(mode, job);
		return 0;
	}

	/* A process of no job is the caller's to wait for. */
	if (job == NULL)
		return child;

	/* Grows the job's process array when it is full. */
	if (job->count == job->capacity) {
		if (job->capacity == 0)
			job->capacity = 4;
		else
			job->capacity *= 2;
		job->processes = sh_realloc(job->processes,
					    (size_t)job->capacity *
					    sizeof(*job->processes));
	}

	/* Records the process. */
	process = &job->processes[job->count++];
	process->pid = child;
	process->state = PROCESS_RUNNING;
	process->status = 0;

	/* With job control, the job's first process leads its group. */
	if (job_control && mode != SH_FORK_NO_JOB) {
		if (job->group == 0)
			job->group = child;
		(void)setpgid(child, job->group);
	}

	/* Succeeded: the child's pid. */
	return child;
}

/*
 * Starts a program in the foreground with posix_spawn, which does not
 * copy the shell, for a job run without job control; actions (or NULL)
 * give it its descriptors, as for a command of a pipeline.  Returns the
 * child's pid, or -1 when the program could not be started this way
 * (the caller forks instead, and the child reports why).
 */
pid_t
sh_spawn(
	const char *path,
	char **argv,
	void *memory,
	const posix_spawn_file_actions_t *actions)
{
	posix_spawnattr_t attributes;
	struct job *job;
	struct process *process;
	sigset_t defaults;
	char **environment;
	pid_t child;
	int error;

	/* Job control puts the child in a group of its own and gives it the terminal: that is fork's. */
	job = memory;
	if (job_control || job == NULL)
		return -1;

	/* Output buffered now goes before the program's. */
	fflush(NULL);

	/* The signals the shell handles for itself go back to their default. */
	sh_signals_shell_set(&defaults);
	error = posix_spawnattr_init(&attributes);
	if (error != 0)
		return -1;
	error = posix_spawnattr_setsigdefault(&attributes, &defaults);
	if (error == 0)
		error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF);

	/* The program, with the exported variables. */
	environment = sh_var_environment();
	if (error == 0)
		error = posix_spawn(&child, path, actions, &attributes, argv, environment);
	sh_var_environment_free(environment);
	(void)posix_spawnattr_destroy(&attributes);
	if (error != 0)
		return -1;

	/* Grows the job's process array when it is full. */
	if (job->count == job->capacity) {
		if (job->capacity == 0)
			job->capacity = 4;
		else
			job->capacity *= 2;
		job->processes = sh_realloc(job->processes,
					    (size_t)job->capacity *
					    sizeof(*job->processes));
	}

	/* Succeeded: the process of the job. */
	process = &job->processes[job->count++];
	process->pid = child;
	process->state = PROCESS_RUNNING;
	process->status = 0;
	return child;
}

/*
 * Makes a newly forked child a subshell of the shell.
 */
void
sh_fork_child(
	int mode,
	void *memory)
{
	struct job *job;
	pid_t group;
	int controlled;

	/* A job of its own process group; the foreground one gets the terminal. */
	job = memory;
	controlled = job_control && mode != SH_FORK_NO_JOB;
	if (controlled) {
		group = getpid();
		if (job != NULL && job->group != 0)
			group = job->group;
		(void)setpgid(0, group);
		if (mode == SH_FORK_FOREGROUND)
			set_terminal_group(group);
	}

	/* The subshell resets traps, and does no job control of its own. */
	sh_subshell++;
	sh_trap_reset_subshell();
	sh_signals_for_child(mode == SH_FORK_BACKGROUND, controlled);
	job_control = 0;
	sh_option[SH_OPT_MONITOR] = 0;
	sh_job_forget_all();
	sh_option[SH_OPT_INTERACTIVE] = 0;
}

/*
 * Waits for a job run in the foreground, and returns its status: the last
 * command's, or with pipefail the last that failed.  A job that stops (job
 * control) goes into the table.
 */
int
sh_job_wait_foreground(
	void *memory)
{
	struct job *job;
	int status;
	int state;
	int signalled;

	/* Waits for every process of the job. */
	job = memory;
	(void)job_wait(job, 0, &signalled);
	state = job_state(job);
	status = job_status(job);

	/* The terminal comes back to the shell. */
	if (job_control)
		set_terminal_group(shell_group);

	/* A job stopped from the terminal is kept, and reported. */
	if (state == PROCESS_STOPPED) {
		job->background = 1;
		job_add_to_table(job);
		job->order = ++job_clock;
		fputc('\n', stderr);
		job_print(job, 0, 0);
		return status;
	}

	/* A command killed by a signal other than an interrupt says so. */
	if (job->count > 0)
		report_signal(job->processes[job->count - 1].status);
	job_free(job);

	/* Succeeded: the job's status. */
	return status;
}

/*
 * Puts a job started in the background into the table; $! is its last
 * process.
 */
void
sh_job_background(
	void *memory)
{
	struct job *job;

	/* $! names the last process. */
	job = memory;
	if (job->count > 0)
		sh_last_background = job->processes[job->count - 1].pid;

	/* The job joins the table; an interactive shell says its number. */
	job_add_to_table(job);
	job->order = ++job_clock;
	if (sh_option[SH_OPT_INTERACTIVE] && sh_subshell == 0 &&
	    job->count > 0)
		fprintf(stderr, "[%d] %ld\n", job->number,
			(long)job->processes[job->count - 1].pid);

	/* Collects what has already ended, so the table does not grow. */
	sh_job_poll();
}

/*
 * Collects, without waiting, every process of the table that has ended or
 * stopped.
 */
void
sh_job_poll(
	void)
{
	pid_t pid;
	int status;
	int flags;

	/* Stops are reported only with job control. */
	flags = WNOHANG;
	if (job_control)
		flags |= WUNTRACED;

	/* Reaps whatever the system has to report. */
	for (;;) {
		pid = waitpid(-1, &status, flags);
		if (pid < 0 && errno == EINTR)
			continue;
		if (pid <= 0)
			break;
		job_update(pid, status);
	}
}

/*
 * Reports background jobs that have ended since the last prompt, and drops
 * them (interactive shell).
 */
void
sh_job_notify(
	void)
{
	struct job *job;
	struct job *next;
	int state;

	/* Collects, then reports each ended job once. */
	sh_job_poll();
	for (job = job_table; job != NULL; job = next) {
		next = job->next;
		state = job_state(job);
		if (state != PROCESS_DONE)
			continue;
		job_print(job, 0, 0);
		job_remove(job);
	}
}

/*
 * Waits for one process that belongs to no job (a command substitution, a
 * here-document writer), and returns its status.
 */
int
sh_wait_process(
	pid_t pid,
	int flags)
{
	pid_t result;
	int status;
	int converted;

	/* Waits through interruptions. */
	do
		result = waitpid(pid, &status, flags);
	while (result < 0 && errno == EINTR);
	if (result < 0)
		return 127;

	/* Succeeded: its status as the shell counts it. */
	converted = sh_status_of(status);
	return converted;
}

/*
 * Converts a wait status to a shell status: the exit status, or 128 and
 * the signal.
 */
int
sh_status_of(
	int status)
{
	int exited;
	int signalled;
	int stopped;

	/* An exit gives its status. */
	exited = WIFEXITED(status);
	if (exited)
		return WEXITSTATUS(status);

	/* A signal that ended the process gives 128 and its number. */
	signalled = WIFSIGNALED(status);
	if (signalled)
		return 128 + WTERMSIG(status);

	/* So does a signal that stopped it. */
	stopped = WIFSTOPPED(status);
	if (stopped)
		return 128 + WSTOPSIG(status);

	/* Anything else is a failure. */
	return 1;
}

/*
 * Forgets every job (in a subshell, which has none of its parent's).
 */
void
sh_job_forget_all(
	void)
{
	struct job *job;

	/* Frees the table. */
	while (job_table != NULL) {
		job = job_table;
		job_table = job->next;
		job_free(job);
	}
}

/*
 * Reports whether a job is stopped, which an interactive shell warns about
 * before it exits.
 */
int
sh_job_has_stopped(
	void)
{
	struct job *job;
	int state;

	/* Looks for one. */
	for (job = job_table; job != NULL; job = job->next) {
		state = job_state(job);
		if (state == PROCESS_STOPPED)
			return 1;
	}

	/* None is stopped. */
	return 0;
}

/*
 * Implements the jobs builtin: jobs [-l | -p] [job ...].
 */
int
sh_jobs_builtin(
	int argc,
	char **argv)
{
	struct job *job;
	struct job *next;
	const char *word;
	int long_form;
	int groups_only;
	int index;
	int status;
	int state;

	/* Reads -l and -p. */
	long_form = 0;
	groups_only = 0;
	for (index = 1; index < argc; index++) {
		word = argv[index];
		if (word[0] != '-')
			break;
		if (word[1] == '-' && word[2] == '\0') {
			index++;
			break;
		}

		/* -l: the long form. */
		if (word[1] == 'l' && word[2] == '\0') {
			long_form = 1;
		} else if (word[1] == 'p' && word[2] == '\0') {
			groups_only = 1;
		} else {
			fprintf(stderr, "jobs: illegal option %s\n", word);
			return 2;
		}
	}

	/* Every job, reporting and dropping the ended ones. */
	sh_job_poll();
	if (index >= argc) {
		for (job = job_table; job != NULL; job = next) {
			next = job->next;
			job_print(job, long_form, groups_only);
			state = job_state(job);
			if (state == PROCESS_DONE && !groups_only)
				job_remove(job);
		}

		/* Succeeded: every job was listed. */
		return 0;
	}

	/* The named jobs. */
	status = 0;
	for (; index < argc; index++) {
		job = job_find_spec(argv[index], "jobs");
		if (job == NULL) {
			status = 1;
			continue;
		}

		/* The job. */
		job_print(job, long_form, groups_only);
	}

	/* Succeeded: 0 when every job was found. */
	return status;
}

/*
 * Implements the fg builtin: continues a job in the foreground.
 */
int
sh_fg_builtin(
	int argc,
	char **argv)
{
	struct job *job;
	const char *spec;
	int status;
	int state;
	int signalled;

	/* Only with job control. */
	if (!job_control) {
		fprintf(stderr, "fg: no job control\n");
		return 1;
	}

	/* The job named, or the current one. */
	spec = "%%";
	if (argc > 1)
		spec = argv[1];
	job = job_find_spec(spec, "fg");
	if (job == NULL)
		return 1;

	/* Gives it the terminal and continues it. */
	if (job->text != NULL)
		printf("%s\n", job->text);
	fflush(stdout);
	set_terminal_group(job->group);
	job_continue(job);
	job->background = 0;
	(void)job_wait(job, 0, &signalled);
	set_terminal_group(shell_group);
	status = job_status(job);

	/* A job that stopped again stays; one that ended goes. */
	state = job_state(job);
	if (state == PROCESS_STOPPED) {
		job->background = 1;
		job->order = ++job_clock;
		fputc('\n', stderr);
		job_print(job, 0, 0);
		return status;
	}

	/* An ended job: how it ended, and it is forgotten. */
	report_signal(job->processes[job->count - 1].status);
	job_remove(job);

	/* Succeeded: the job's status. */
	return status;
}

/*
 * Implements the bg builtin: continues stopped jobs in the background.
 */
int
sh_bg_builtin(
	int argc,
	char **argv)
{
	struct job *job;
	const char *spec;
	int index;
	int status;

	/* Only with job control. */
	if (!job_control) {
		fprintf(stderr, "bg: no job control\n");
		return 1;
	}

	/* Each named job, or the current one. */
	status = 0;
	index = 1;
	do {
		spec = "%%";
		if (index < argc)
			spec = argv[index];
		job = job_find_spec(spec, "bg");
		if (job == NULL) {
			status = 1;
			continue;
		}

		/* The job goes on in the background. */
		job_continue(job);
		job->background = 1;
		if (job->text != NULL)
			printf("[%d] %s &\n", job->number, job->text);
		else
			printf("[%d] &\n", job->number);
	} while (++index < argc);

	/* Succeeded: 0 when every job was found. */
	return status;
}

/*
 * Implements the wait builtin: waits for the named processes or jobs, or for
 * every background job.  A trapped signal ends the wait with 128 and the
 * signal.
 */
int
sh_wait_builtin(
	int argc,
	char **argv)
{
	struct job *job;
	struct job *next;
	const char *word;
	char *end;
	long pid;
	int index;
	int status;
	int signalled;
	int waited;
	int state;

	/* -- ends the options. */
	index = 1;
	if (index < argc) {
		word = argv[index];
		if (word[0] == '-' && word[1] == '-' && word[2] == '\0')
			index++;
	}

	/* Without operands, every background job. */
	if (index >= argc) {
		for (job = job_table; job != NULL; job = next) {
			waited = job_wait(job, 1, &signalled);
			if (waited != 0 && signalled)
				return 128 + sh_trap_last_signal;
			next = job->next;
			state = job_state(job);
			if (state == PROCESS_DONE)
				job_remove(job);
		}

		/* Succeeded: every job has ended. */
		return 0;
	}

	/* Each operand: a job (an unknown one is an error) or a pid. */
	status = 0;
	for (; index < argc; index++) {
		word = argv[index];
		if (word[0] == '%') {
			job = job_find_spec(word, "wait");
			if (job == NULL) {
				status = 2;
				continue;
			}
		} else {
			pid = strtol(word, &end, 10);
			if (*word == '\0' || *end != '\0' || pid <= 0) {
				fprintf(stderr, "wait: Illegal number: %s\n",
					word);
				return 2;
			}

			/* The job the process belongs to. */
			job = job_of_pid((pid_t)pid);
			if (job == NULL) {
				status = 127;
				continue;
			}
		}

		/* Waits for it; a trapped signal ends the wait. */
		waited = job_wait(job, 1, &signalled);
		if (waited != 0 && signalled)
			return 128 + sh_trap_last_signal;
		status = job_status(job);
		state = job_state(job);
		if (state == PROCESS_DONE) {
			report_signal(job->processes[job->count - 1].status);
			job_remove(job);
		}
	}

	/* Succeeded: the status of the last operand. */
	return status;
}

/*
 * Implements the kill builtin: kill [-s name | -name | -number] pid|job...,
 * and kill -l [status].
 */
int
sh_kill_builtin(
	int argc,
	char **argv)
{
	const char *word;
	int number;
	int index;
	int status;
	int sent;

	/* -l lists the signal names, or names the signal of a status. */
	if (argc > 1) {
		word = argv[1];
		if (word[0] == '-' && (word[1] == 'l' || word[1] == 'L') &&
		    word[2] == '\0') {
			status = kill_list(argc, argv);
			return status;
		}
	}

	/* The signal. */
	index = 1;
	number = kill_signal(argc, argv, &index);
	if (number < 0)
		return 2;
	if (index >= argc) {
		fprintf(stderr, "usage: kill [-s sigspec | -signum | "
			"-sigspec] [pid | job]...\n");
		return 2;
	}

	/* Sends it to each pid or job. */
	status = 0;
	for (; index < argc; index++) {
		sent = kill_send(argv[index], number);
		if (sent != 0)
			status = 1;
	}

	/* Succeeded: 0 when every signal was sent. */
	return status;
}

/* Finds the job a specification names (%n, %%, %+, %-, %name, %?text, pid). */
static struct job *
job_find_spec(
	const char *spec,
	const char *command)
{
	struct job *job;
	struct job *current;
	struct job *previous;
	char *end;
	long number;

	/* A pid names the job it belongs to. */
	if (spec[0] != '%') {
		job = NULL;
		number = strtol(spec, &end, 10);
		if (*spec != '\0' && *end == '\0')
			job = job_of_pid((pid_t)number);
		if (job == NULL)
			fprintf(stderr, "%s: %s: no such job\n", command, spec);
		return job;
	}

	/* The forms of a job specification. */
	job_ranked(&current, &previous);
	spec++;
	if (spec[0] == '\0' || ((spec[0] == '%' || spec[0] == '+') &&
				spec[1] == '\0'))
		job = current;
	else if (spec[0] == '-' && spec[1] == '\0')
		job = previous;
	else if (spec[0] >= '0' && spec[0] <= '9')
		job = job_find_numbered(spec);
	else if (spec[0] == '?')
		job = job_find_text(spec + 1, 1);
	else
		job = job_find_text(spec, 0);
	if (job == NULL)
		fprintf(stderr, "%s: %%%s: no such job\n", command, spec);

	/* Succeeded: the job, or NULL. */
	return job;
}

/* Finds the job of a number, as %n names it. */
static struct job *
job_find_numbered(
	const char *spec)
{
	struct job *job;
	char *end;
	long number;

	/* The number, all of it. */
	number = strtol(spec, &end, 10);
	if (*end != '\0')
		return NULL;

	/* The job with that number. */
	for (job = job_table; job != NULL; job = job->next) {
		if (job->number == number)
			return job;
	}

	/* No job has it. */
	return NULL;
}

/* Finds the job whose text starts with (or holds, anywhere) a string. */
static struct job *
job_find_text(
	const char *spec,
	int anywhere)
{
	struct job *job;
	const char *found;
	int compare;

	/* Looks through the jobs that have text. */
	for (job = job_table; job != NULL; job = job->next) {
		if (job->text == NULL)
			continue;
		if (anywhere) {
			found = strstr(job->text, spec);
			if (found != NULL)
				return job;
			continue;
		}

		/* Otherwise the command must start with the text. */
		compare = strncmp(job->text, spec, strlen(spec));
		if (compare == 0)
			return job;
	}

	/* No job matches. */
	return NULL;
}

/* Finds the current (%+) and previous (%-) jobs, the two most recent. */
static void
job_ranked(
	struct job **current,
	struct job **previous)
{
	struct job *job;

	/* The two highest ticks of the clock. */
	*current = NULL;
	*previous = NULL;
	for (job = job_table; job != NULL; job = job->next) {
		if (*current == NULL || job->order > (*current)->order) {
			*previous = *current;
			*current = job;
		} else if (*previous == NULL ||
			   job->order > (*previous)->order) {
			*previous = job;
		}
	}
}

/* Finds the job a process belongs to. */
static struct job *
job_of_pid(
	pid_t pid)
{
	struct job *job;
	int member;

	/* Looks through every process of every job. */
	for (job = job_table; job != NULL; job = job->next) {
		for (member = 0; member < job->count; member++) {
			if (job->processes[member].pid == pid)
				return job;
		}
	}

	/* No job has that process. */
	return NULL;
}

/* Puts a job into the table with the least free number. */
static void
job_add_to_table(
	struct job *job)
{
	struct job **link;
	struct job *other;
	int number;
	int taken;

	/* The least number no job has. */
	for (number = 1;; number++) {
		taken = 0;
		for (other = job_table; other != NULL; other = other->next) {
			if (other->number == number)
				taken = 1;
		}

		/* A free number is the job's. */
		if (!taken)
			break;
	}

	/* The job's number. */
	job->number = number;

	/* Appended at the end. */
	link = &job_table;
	while (*link != NULL)
		link = &(*link)->next;
	job->next = NULL;
	*link = job;

	/* A script that never waits must not fill the table with ended jobs. */
	job_limit_done();
}

/* Drops the oldest ended job while too many ended jobs are kept. */
static void
job_limit_done(
	void)
{
	struct job *job;
	struct job *oldest;
	int done;
	int state;

	/* Counts the ended jobs and finds the oldest. */
	done = 0;
	oldest = NULL;
	for (job = job_table; job != NULL; job = job->next) {
		state = job_state(job);
		if (state != PROCESS_DONE)
			continue;
		done++;
		if (oldest == NULL)
			oldest = job;
	}

	/* One over the limit goes. */
	if (done > DONE_JOBS_MAX && oldest != NULL)
		job_remove(oldest);
}

/* Removes a job from the table and frees it. */
static void
job_remove(
	struct job *job)
{
	struct job **link;

	/* Unlinks it. */
	link = &job_table;
	while (*link != NULL && *link != job)
		link = &(*link)->next;
	if (*link != NULL)
		*link = job->next;

	/* Frees it. */
	job_free(job);
}

/* Frees a job. */
static void
job_free(
	struct job *job)
{
	/* The processes, the text, and the job. */
	free(job->processes);
	free(job->text);
	free(job);
}

/* Reports how a job stands: running, stopped, or done. */
static int
job_state(
	const struct job *job)
{
	int member;
	int stopped;

	/* Running while any process runs; stopped while any is stopped. */
	stopped = 0;
	for (member = 0; member < job->count; member++) {
		if (job->processes[member].state == PROCESS_RUNNING)
			return PROCESS_RUNNING;
		if (job->processes[member].state == PROCESS_STOPPED)
			stopped = 1;
	}

	/* Succeeded: stopped or done. */
	if (stopped)
		return PROCESS_STOPPED;
	return PROCESS_DONE;
}

/*
 * Reports a job's status: its last process's, or with pipefail the last
 * process that failed.
 */
static int
job_status(
	const struct job *job)
{
	int member;
	int status;

	/* No processes: nothing ran. */
	if (job->count == 0)
		return 0;

	/* pipefail: the rightmost process that failed. */
	if (sh_option[SH_OPT_PIPEFAIL]) {
		for (member = job->count - 1; member >= 0; member--) {
			status = sh_status_of(job->processes[member].status);
			if (status != 0)
				return status;
		}

		/* Succeeded: every process was 0. */
		return 0;
	}

	/* Succeeded: the last process's status. */
	status = sh_status_of(job->processes[job->count - 1].status);
	return status;
}

/* Records what the system reported of a process, wherever it belongs. */
static void
job_update(
	pid_t pid,
	int status)
{
	struct job *job;
	int member;

	/* Finds the process in the table. */
	for (job = job_table; job != NULL; job = job->next) {
		for (member = 0; member < job->count; member++) {
			if (job->processes[member].pid != pid)
				continue;
			process_record(&job->processes[member], status);
			return;
		}
	}
}

/* Records a wait status on a process: stopped, or done. */
static void
process_record(
	struct process *process,
	int status)
{
	int stopped;

	/* The status, and the state it says. */
	process->status = status;
	stopped = WIFSTOPPED(status);
	if (stopped)
		process->state = PROCESS_STOPPED;
	else
		process->state = PROCESS_DONE;
}

/*
 * Waits until every process of a job has ended (or, with job control,
 * stopped).  An interruptible wait (the wait builtin) gives up when a trapped
 * signal arrives, and sets *signalled.  Returns 0 when the job settled.
 */
static int
job_wait(
	struct job *job,
	int interruptible,
	int *signalled)
{
	pid_t result;
	int member;
	int status;
	int flags;

	/* Stops are waited for only with job control. */
	*signalled = 0;
	flags = 0;
	if (job_control)
		flags |= WUNTRACED;

	/* Waits for each running process in turn. */
	for (member = 0; member < job->count; member++) {
		while (job->processes[member].state == PROCESS_RUNNING) {
			result = waitpid(job->processes[member].pid, &status,
					 flags);

			/* An interrupted wait goes on, unless a trap is due. */
			if (result < 0 && errno == EINTR) {
				if (interruptible && sh_trap_pending_any) {
					*signalled = 1;
					return 1;
				}

				continue;
			}

			/* A process that cannot be waited for counts as gone. */
			if (result < 0) {
				job->processes[member].state = PROCESS_DONE;
				job->processes[member].status = 127 << 8;
				break;
			}

			/* The status the process ended or stopped with. */
			process_record(&job->processes[member], status);
		}
	}

	/* Succeeded: the job has settled. */
	return 0;
}

/* Continues a stopped job: SIGCONT to its group, its processes running. */
static void
job_continue(
	struct job *job)
{
	int member;

	/* The whole group is continued. */
	(void)kill(-job->group, SIGCONT);

	/* Its stopped processes run again. */
	for (member = 0; member < job->count; member++) {
		if (job->processes[member].state == PROCESS_STOPPED)
			job->processes[member].state = PROCESS_RUNNING;
	}
}

/* Prints a job as the jobs builtin lists it: "[%d] %c %s %s". */
static void
job_print(
	struct job *job,
	int long_form,
	int groups_only)
{
	char state[64];
	const char *text;

	/* -p prints only the process group (or the first process). */
	if (groups_only) {
		if (job->group != 0)
			printf("%ld\n", (long)job->group);
		else if (job->count > 0)
			printf("%ld\n", (long)job->processes[0].pid);
		return;
	}

	/* [n] mark [pid] state text. */
	job_state_text(job, state, sizeof(state));
	text = "";
	if (job->text != NULL)
		text = job->text;
	printf("[%d] %c ", job->number, job_mark(job));
	if (long_form && job->count > 0)
		printf("%ld ", (long)job->processes[0].pid);
	printf("%-24s%s\n", state, text);
	fflush(stdout);
}

/* Writes the state of a job in words: Running, Stopped, Done, a signal. */
static void
job_state_text(
	const struct job *job,
	char *text,
	size_t size)
{
	int state;
	int last;
	int status;
	int signalled;

	/* Dispatches on how the job stands. */
	state = job_state(job);
	switch (state) {
	case PROCESS_RUNNING:
		snprintf(text, size, "Running");
		return;
	case PROCESS_STOPPED:
		snprintf(text, size, "Stopped");
		return;
	default:
		break;
	}

	/* An ended job: the signal that killed it, or Done with its status. */
	last = job->processes[job->count - 1].status;
	status = sh_status_of(last);
	signalled = WIFSIGNALED(last);
	if (signalled)
		snprintf(text, size, "%s", strsignal(WTERMSIG(last)));
	else if (status == 0)
		snprintf(text, size, "Done");
	else
		snprintf(text, size, "Done(%d)", status);
}

/* Returns + for the current job, - for the previous one, else a space. */
static char
job_mark(
	const struct job *job)
{
	struct job *current;
	struct job *previous;

	/* The two most recent by the clock. */
	job_ranked(&current, &previous);
	if (job == current)
		return '+';
	if (job == previous)
		return '-';

	/* Neither. */
	return ' ';
}

/* Gives the terminal to a process group. */
static void
set_terminal_group(
	pid_t group)
{
	/* SIGTTOU is ignored by an interactive shell, so this is allowed. */
	(void)tcsetpgrp(0, group);
}

/* Writes the message of a command killed by a signal. */
static void
report_signal(
	int status)
{
	int signalled;
	int number;

	/* Only a command killed by a signal has one. */
	signalled = WIFSIGNALED(status);
	if (!signalled)
		return;

	/* An interrupt is only a new line on a terminal. */
	number = WTERMSIG(status);
	if (number == SIGINT) {
		if (sh_option[SH_OPT_INTERACTIVE])
			fputc('\n', stderr);
		return;
	}

	/* A broken pipe is not worth a message. */
	if (number == SIGPIPE)
		return;

	/* Any other signal is named. */
	fprintf(stderr, "%s\n", strsignal(number));
}

/* Implements kill -l: every signal, or the name of each status or signal. */
static int
kill_list(
	int argc,
	char **argv)
{
	int index;
	int named;

	/* With no operands, every signal. */
	if (argc == 2) {
		sh_trap_list_signals();
		return 0;
	}

	/* Each operand, named. */
	for (index = 2; index < argc; index++) {
		named = kill_name(argv[index]);
		if (named != 0)
			return 2;
	}

	/* Succeeded: every operand was named. */
	return 0;
}

/*
 * Prints what kill -l says of one operand: the number of a name, or the
 * name of a number (a status above 128 names its signal).
 */
static int
kill_name(
	const char *operand)
{
	const char *name;
	char *end;
	long value;
	int number;

	/* A name gives its number. */
	value = strtol(operand, &end, 10);
	if (*operand == '\0' || *end != '\0') {
		number = sh_trap_signal_number(operand);
		if (number <= 0) {
			fprintf(stderr, "kill: invalid signal number or name: "
				"%s\n", operand);
			return 1;
		}

		/* The signal's number. */
		printf("%d\n", number);
		return 0;
	}

	/* A number gives its name; a status names its signal. */
	if (value > 128)
		value -= 128;
	name = sh_trap_signal_name((int)value);
	if (name == NULL) {
		fprintf(stderr, "kill: invalid signal number or name: %s\n",
			operand);
		return 1;
	}

	/* The signal's name. */
	printf("%s\n", name);

	/* Succeeded. */
	return 0;
}

/*
 * Reads the signal of kill: -s name, -name, -number, or TERM when none is
 * given.  Leaves *index at the first operand; returns -1 for a bad signal.
 */
static int
kill_signal(
	int argc,
	char **argv,
	int *index)
{
	const char *word;
	int number;

	/* TERM, unless an option says otherwise. */
	number = SIGTERM;
	if (*index >= argc)
		return number;
	word = argv[*index];

	/* -s name, or -name / -number. */
	if (word[0] == '-' && word[1] == 's' && word[2] == '\0') {
		if (*index + 1 >= argc) {
			fprintf(stderr, "kill: option requires an argument "
				"-- s\n");
			return -1;
		}

		/* The signal named by the next argument. */
		word = argv[*index + 1];
		number = sh_trap_signal_number(word);
		*index += 2;
	} else if (word[0] == '-' && !(word[1] == '-' && word[2] == '\0')) {
		number = sh_trap_signal_number(word + 1);
		(*index)++;
	}

	/* -- before the operands. */
	if (*index < argc) {
		if (argv[*index][0] == '-' && argv[*index][1] == '-' &&
		    argv[*index][2] == '\0')
			(*index)++;
	}

	/* A signal that is no signal. */
	if (number < 0) {
		fprintf(stderr, "kill: invalid signal number or name: %s\n",
			word);
		return -1;
	}

	/* Succeeded: the signal. */
	return number;
}

/* Sends a signal to one kill operand: a job, or a pid. */
static int
kill_send(
	const char *operand,
	int number)
{
	struct job *job;
	char *end;
	long value;
	int member;
	int sent;
	int error;

	/* A job: its group with job control, each of its processes without. */
	if (operand[0] == '%') {
		job = job_find_spec(operand, "kill");
		if (job == NULL)
			return 1;
		if (job_control && job->group != 0) {
			sent = kill(-job->group, number);
			if (sent != 0)
				return 1;
			return 0;
		}

		/* Without a process group, each process gets the signal. */
		for (member = 0; member < job->count; member++)
			(void)kill(job->processes[member].pid, number);
		return 0;
	}

	/* A pid. */
	value = strtol(operand, &end, 10);
	if (*operand == '\0' || *end != '\0') {
		fprintf(stderr, "kill: illegal number %s\n", operand);
		return 1;
	}

	/* The process gets the signal. */
	sent = kill((pid_t)value, number);
	if (sent != 0) {
		error = errno;
		fprintf(stderr, "kill: %s: %s\n", operand, strerror(error));
		return 1;
	}

	/* Succeeded: the signal was sent. */
	return 0;
}
