/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD kill userland command.
 */

#include "userland/base/common/command.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
struct signal_name {
	const char *name;
	int number;
};
static const struct signal_name names[] = {
    {"HUP", SIGHUP},   {"INT", SIGINT},	    {"QUIT", SIGQUIT},
    {"ILL", SIGILL},   {"TRAP", SIGTRAP},   {"ABRT", SIGABRT},
    {"FPE", SIGFPE},   {"KILL", SIGKILL},   {"BUS", SIGBUS},
    {"SEGV", SIGSEGV}, {"PIPE", SIGPIPE},   {"ALRM", SIGALRM},
    {"TERM", SIGTERM}, {"USR1", SIGUSR1},   {"USR2", SIGUSR2},
    {"CHLD", SIGCHLD}, {"CONT", SIGCONT},   {"STOP", SIGSTOP},
    {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN},   {"TTOU", SIGTTOU},
    {"URG", SIGURG},   {"WINCH", SIGWINCH}, {"IO", SIGIO},
    {"POLL", SIGPOLL}, {"XCPU", SIGXCPU},   {"XFSZ", SIGXFSZ}};

static int parse_signal(const char *text);

/*
 * Runs the kill command.
 */
int
main(
	int argc,
	char **argv)
{
	char *end;
	long pid;
	size_t n_index_for;
	int signal_number, i, failed;

	signal_number = SIGTERM;
	i = 1;
	failed = 0;

	/* Handles the selected command-line operation. */
	if (i < argc && !strcmp(argv[i], "-l")) {
		/* Process each remaining element. */
		for (n_index_for = 0; n_index_for < sizeof(names) / sizeof(names[0]); n_index_for++)
			printf("%d %s\n", names[n_index_for].number, names[n_index_for].name);

		/* Reports successful completion. */
		return 0;
	}

	/* Validates the command-line arguments. */
	if (i < argc && argv[i][0] == '-') {
		signal_number = parse_signal(argv[i] + 1);
		i++;
	}

	/* Validates the command-line arguments. */
	if (signal_number < 0 || i == argc) {
		fprintf(stderr,
			"usage: kill [-signal] pid...\n       kill -l\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (; i < argc; i++) {
		pid = strtol(argv[i], &end, 10);

		/* Validates the command-line arguments. */
		if (!*argv[i] || *end || kill((pid_t)pid, signal_number)) {
			command_error("kill", argv[i]);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
}

/* Supports the parse signal operation. */
static int
parse_signal(
	const char *text)
{
	char *end;
	long n;
	size_t i;

	/* Selects the matching prefix. */
	if (!strncmp(text, "SIG", 3))
		text += 3;

	/* Process each remaining element. */
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		/* Selects the matching value. */
		if (!strcmp(text, names[i].name))
			return names[i].number;
	}
	n = strtol(text, &end, 10);

	/* Returns the computed result. */
	return *text && !*end && n >= 0 && n <= SIGRTMAX ? (int)n : -1;
}
