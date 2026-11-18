/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Terminates or signals processes (POSIX XCU kill).
 *
 *	kill -s signal_name pid...
 *	kill -l [exit_status]
 *	kill [-signal_name] pid...
 *	kill [-signal_number] pid...
 *
 * The signal (TERM by default) is sent to each pid; a negative pid is a
 * process group, and -- is needed before it when no -s comes first.
 * Signal names are read without regard to case, with or without the SIG
 * prefix, and 0 is the null signal that only checks that the process
 * exists.  -l lists the signal names, or names the signal of an exit
 * status: a status above 128, as the shell reports a process killed by a
 * signal, stands for the status less 128.  The shell has its own kill,
 * which also knows jobs; this is the command.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* The status of a process killed by signal n, as the shell reports it. */
#define KILL_SIGNAL_STATUS 128

/*
 * One signal name.
 *
 * The table below is constant for the life of the program.
 */
struct signal_name {
	const char *name;
	int number;
};

/* The names of the signals, in the order -l lists them. */
static const struct signal_name signal_names[] = {
	{ "HUP", SIGHUP },
	{ "INT", SIGINT },
	{ "QUIT", SIGQUIT },
	{ "ILL", SIGILL },
	{ "TRAP", SIGTRAP },
	{ "ABRT", SIGABRT },
	{ "BUS", SIGBUS },
	{ "FPE", SIGFPE },
	{ "KILL", SIGKILL },
	{ "USR1", SIGUSR1 },
	{ "SEGV", SIGSEGV },
	{ "USR2", SIGUSR2 },
	{ "PIPE", SIGPIPE },
	{ "ALRM", SIGALRM },
	{ "TERM", SIGTERM },
	{ "CHLD", SIGCHLD },
	{ "CONT", SIGCONT },
	{ "STOP", SIGSTOP },
	{ "TSTP", SIGTSTP },
	{ "TTIN", SIGTTIN },
	{ "TTOU", SIGTTOU },
	{ "URG", SIGURG },
	{ "XCPU", SIGXCPU },
	{ "XFSZ", SIGXFSZ },
	{ "VTALRM", SIGVTALRM },
	{ "PROF", SIGPROF },
	{ "WINCH", SIGWINCH },
	{ "SYS", SIGSYS },
	{ "IO", SIGIO },
	{ "POLL", SIGPOLL },
};

static int list_signals(int argc, char **argv);
static int read_signal(const char *text, int *number);
static const char *signal_name(int number);
static int send_signal(int number, const char *text);
static void usage(void);

/*
 * Runs kill.
 */
int
main(
	int argc,
	char **argv)
{
	const char *option;
	int number;
	int first;
	int index;
	int failed;
	int status;
	int compare;

	/* Something must follow. */
	if (argc < 2)
		usage();

	/* -l lists or names signals. */
	option = argv[1];
	compare = strcmp(option, "-l");
	if (compare == 0) {
		status = list_signals(argc - 2, argv + 2);
		return status;
	}

	/* Reads the signal: -s name, -name, -number, or TERM. */
	number = SIGTERM;
	first = 1;
	compare = strcmp(option, "-s");
	if (compare == 0) {
		if (argc < 3)
			usage();
		status = read_signal(argv[2], &number);
		if (status != 0) {
			fprintf(stderr, "kill: unknown signal: '%s'\n", argv[2]);
			return 1;
		}

		/* The process IDs follow the name. */
		first = 3;
	} else if (option[0] == '-' && option[1] != '-' && option[1] != '\0') {
		status = read_signal(option + 1, &number);
		if (status != 0) {
			fprintf(stderr, "kill: unknown signal: '%s'\n", option + 1);
			return 1;
		}

		/* The process IDs follow the signal. */
		first = 2;
	}

	/* -- may end the options before a negative process group. */
	if (first < argc) {
		compare = strcmp(argv[first], "--");
		if (compare == 0)
			first++;
	}

	/* At least one process. */
	if (first >= argc)
		usage();

	/* Signals each; a failure is remembered and the rest go on. */
	failed = 0;
	for (index = first; index < argc; index++) {
		status = send_signal(number, argv[index]);
		if (status != 0)
			failed = 1;
	}

	/* Reports whether any process could not be signalled. */
	if (failed)
		return 1;

	/* Succeeded: every process was signalled. */
	return 0;
}

/*
 * Handles -l: without an operand, writes every signal name, one to a
 * line; with an exit status or a signal number, writes the name of its
 * signal.
 */
static int
list_signals(
	int argc,
	char **argv)
{
	const char *name;
	char *end;
	long value;
	size_t index;

	/* Without an operand, every name. */
	if (argc == 0) {
		for (index = 0; index < sizeof(signal_names) / sizeof(signal_names[0]); index++)
			printf("%s\n", signal_names[index].name);
		return 0;
	}

	/* At most one operand. */
	if (argc > 1)
		usage();

	/* The operand is a number: a status above 128 means its signal. */
	errno = 0;
	value = strtol(argv[0], &end, 10);
	if (end == argv[0] || *end != '\0' || value < 0 || errno != 0) {
		fprintf(stderr, "kill: invalid exit status: '%s'\n", argv[0]);
		return 1;
	}

	/* A status above 128 stands for the signal that killed the process. */
	if (value > KILL_SIGNAL_STATUS)
		value -= KILL_SIGNAL_STATUS;

	/* Writes the name of the signal. */
	name = signal_name((int)value);
	if (name == NULL) {
		fprintf(stderr, "kill: unknown signal: %ld\n", value);
		return 1;
	}

	/* Writes the name. */
	printf("%s\n", name);

	/* Succeeded: the name was written. */
	return 0;
}

/*
 * Reads a signal: a name in any case, with or without SIG, or a number;
 * 0 is the null signal.
 */
static int
read_signal(
	const char *text,
	int *number)
{
	char upper[32];
	const char *name;
	char *end;
	long value;
	size_t length;
	size_t index;
	int compare;

	/* A number is taken as it is. */
	if (text[0] >= '0' && text[0] <= '9') {
		errno = 0;
		value = strtol(text, &end, 10);
		if (*end != '\0' || errno != 0 || value < 0 || value >= NSIG)
			return -1;
		*number = (int)value;
		return 0;
	}

	/* A name is compared in upper case. */
	length = strlen(text);
	if (length == 0 || length >= sizeof(upper))
		return -1;
	for (index = 0; index <= length; index++) {
		upper[index] = text[index];
		if (upper[index] >= 'a' && upper[index] <= 'z')
			upper[index] = (char)(upper[index] - 'a' + 'A');
	}

	/* The SIG prefix may be there or not. */
	name = upper;
	compare = strncmp(name, "SIG", 3);
	if (compare == 0 && name[3] != '\0')
		name += 3;

	/* Looks the name up. */
	for (index = 0; index < sizeof(signal_names) / sizeof(signal_names[0]); index++) {
		compare = strcmp(name, signal_names[index].name);
		if (compare == 0) {
			*number = signal_names[index].number;
			return 0;
		}
	}

	/* Not a signal name. */
	return -1;
}

/* Gives the name of a signal number, or NULL. */
static const char *
signal_name(
	int number)
{
	size_t index;

	/* The first name of the number. */
	for (index = 0; index < sizeof(signal_names) / sizeof(signal_names[0]); index++) {
		if (signal_names[index].number == number)
			return signal_names[index].name;
	}

	/* No name. */
	return NULL;
}

/* Sends a signal to one pid operand. */
static int
send_signal(
	int number,
	const char *text)
{
	char *end;
	long value;
	int status;

	/* The operand is a decimal process ID, negative for a group. */
	errno = 0;
	value = strtol(text, &end, 10);
	if (end == text || *end != '\0' || errno != 0) {
		fprintf(stderr, "kill: invalid process ID: '%s'\n", text);
		return -1;
	}

	/* Sends the signal. */
	status = kill((pid_t)value, number);
	if (status != 0) {
		fprintf(stderr, "kill: %s: %s\n", text, strerror(errno));
		return -1;
	}

	/* Succeeded: the signal was sent. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX forms. */
	fprintf(stderr,
		"usage: kill -s signal_name pid...\n"
		"       kill -l [exit_status]\n"
		"       kill [-signal_name|-signal_number] pid...\n");
	exit(1);
}
