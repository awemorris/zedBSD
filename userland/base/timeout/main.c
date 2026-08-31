/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD timeout userland command.
 */

#include "userland/base/common/command.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SIG2STR_MAX
#define SIG2STR_MAX 32
#endif

struct options {
	struct timespec duration;
	struct timespec kill_delay;
	int foreground;
	int has_kill_delay;
	int preserve_status;
	int signal_number;
};

static volatile int timeout_child = -1;
static volatile int timeout_foreground;
static volatile int timeout_reached;
static volatile int forwarded_signal;
static volatile int timeout_signal = SIGTERM;

#if defined(HAL_ARCH_I386) || defined(HAL_ARCH_AMD64) ||                       \
    defined(HAL_ARCH_ARM64) || defined(HAL_ARCH_SPARCV9) ||                    \
    defined(HAL_ARCH_M68K)
#define ACTION_HANDLER(action, handler)                                        \
	((action).sa_handler = (uint64_t)(uintptr_t)(handler))
#else
#define ACTION_HANDLER(action, handler) ((action).sa_handler = (handler))
#endif

static int parse_options(int argc, char **argv, struct options *options, int *first);
static int duration_parse(const char *text, struct timespec *result);
static int signal_parse(const char *text, int *result);
static int timer_arm(timer_t timer, const struct timespec *duration);
static void send_to_child(int signal_number);
static int status_result(int status);
static void kill_and_reap(pid_t child);
static void relay_signal(int signal_number);

/*
 * Runs the timeout command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	pid_t waited;
	static const int relayed[] = {SIGHUP,  SIGINT,	SIGQUIT, SIGABRT,
				      SIGPIPE, SIGTERM, SIGUSR1, SIGUSR2,
				      SIGXCPU, SIGXFSZ};
	struct sigaction action;
	struct sigevent event;
	struct options options;
	timer_t timer;
	pid_t child;
	int first, status;
	int kill_timer_armed;
	int timer_created;
	unsigned index;

	status = 0;
	kill_timer_armed = 0;
	timer_created = 0;

	/* Validates the command-line arguments. */
	if (parse_options(argc, argv, &options, &first) != 0) {
		fprintf(stderr,
			"usage: timeout [-fp] [-k time] [-s signal_name] "
			"duration utility [argument ...]\n");

		/* Returns the computed result. */
		return 125;
	}
	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
		command_error("timeout", NULL);

		/* Returns the computed result. */
		return 125;
	}

	/* Checks the child process state. */
	if (child == 0) {
		/* Checks the selected options. */
		if (!options.foreground)
			(void)setpgid(0, 0);
		command_exec(argv[first], &argv[first]);
		_exit(errno == ENOENT ? 127 : 126);
	}
	timeout_child = child;
	timeout_foreground = options.foreground;
	timeout_signal = options.signal_number;

	/* Checks the selected options. */
	if (!options.foreground)
		(void)setpgid(child, child);
	memset(&action, 0, sizeof(action));
	ACTION_HANDLER(action, relay_signal);
	(void)sigemptyset(&action.sa_mask);

	/* Process each remaining element. */
	for (index = 0; index < sizeof(relayed) / sizeof(relayed[0]); index++) {
		/* Handles a failed sigaction operation. */
		if (sigaction(relayed[index], &action, NULL) != 0)
			goto internal_failure;
	}

	/* Handles a failed sigaction operation. */
	if (sigaction(SIGALRM, &action, NULL) != 0)
		goto internal_failure;
	memset(&action, 0, sizeof(action));
	ACTION_HANDLER(action, SIG_IGN);
	(void)sigemptyset(&action.sa_mask);

	/* Handles a failed sigaction operation. */
	if (sigaction(SIGTTIN, &action, NULL) != 0 ||
	    sigaction(SIGTTOU, &action, NULL) != 0)
		goto internal_failure;

	/* Checks the selected options. */
	if (options.duration.tv_sec != 0 || options.duration.tv_nsec != 0) {
		memset(&event, 0, sizeof(event));
		event.sigev_notify = SIGEV_SIGNAL;
		event.sigev_signo = SIGALRM;

		/* Handles a failed timer create operation. */
		if (timer_create(CLOCK_MONOTONIC, &event, &timer) != 0)
			goto internal_failure;
		timer_created = 1;

		/* Handles a failed timer arm operation. */
		if (timer_arm(timer, &options.duration) != 0)
			goto internal_failure;
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		waited = waitpid(child, &status, 0);

		/* Handles the waited condition. */
		if (waited == child)
			break;

		/* Handles the reported system error. */
		if (waited < 0 && errno == EINTR) {
			/* Handles the timeout reached condition. */
			if ((timeout_reached || forwarded_signal != 0) &&
			    options.has_kill_delay && !kill_timer_armed) {
				/* Handles the timer created condition. */
				if (!timer_created) {
					memset(&event, 0, sizeof(event));
					event.sigev_notify = SIGEV_SIGNAL;
					event.sigev_signo = SIGALRM;

					/* Handles a failed timer create operation. */
					if (timer_create(CLOCK_MONOTONIC,
							 &event, &timer) != 0)
						goto internal_failure;
					timer_created = 1;
				}

				/* Checks the selected options. */
				if (options.kill_delay.tv_sec == 0 &&
				    options.kill_delay.tv_nsec == 0) {
					send_to_child(SIGKILL);
				} else {
					timeout_signal = SIGKILL;

					/* Handles a failed timer arm operation. */
					if (timer_arm(timer,
						      &options.kill_delay) != 0)
						goto internal_failure;
				}
				kill_timer_armed = 1;
				forwarded_signal = 0;
			}
			continue;
		}
		goto internal_failure;
	}
	timeout_child = -1;

	/* Handles the timer created condition. */
	if (timer_created)
		(void)timer_delete(timer);

	/* Handles the timeout reached condition. */
	if (timeout_reached && !options.preserve_status)
		return 124;

	/* Obtains the status result result. */
	function_result = status_result(status);

	/* Returns the computed result. */
	return function_result;

internal_failure:
	command_error("timeout", NULL);
	kill_and_reap(child);

	/* Handles the timer created condition. */
	if (timer_created)
		(void)timer_delete(timer);

	/* Returns the computed result. */
	return 125;
}

/* Supports the parse options operation. */
static int
parse_options(
	int argc,
	char **argv,
	struct options *options,
	int *first)
{
	char option;
	const char *value;
	const char *argument;
	unsigned position;
	int index;

	memset(options, 0, sizeof(*options));

	/* Process each remaining command-line operand. */
	options->signal_number = SIGTERM;
	for (index = 1; index < argc; index++) {
				argument = argv[index];

		/* Handles the argument condition. */
		if (argument[0] != '-' || argument[1] == '\0')
			break;

		/* Selects the matching value. */
		if (!strcmp(argument, "--")) {
			index++;
			break;
		}

		/* Process each element required by the operation. */
		for (position = 1; argument[position] != '\0'; position++) {
			/* Dispatch the selected operation case. */
			switch (argument[position]) {
			case 'f':
				options->foreground = 1;
				break;
			case 'p':
				options->preserve_status = 1;
				break;
			case 'k':
			case 's':
							option = argument[position];

			/* Handles the argument condition. */
			if (argument[position + 1U] != '\0') {
				value = argument + position + 1U;
				position =
				    (unsigned)strlen(argument) - 1U;
			} else if (++index < argc) {
				value = argv[index];
			} else {
				/* Reports operation failure. */
				return -1;
			}

			/* Handles the option condition. */
			if (option == 'k') {
				/* Handles a failed duration parse operation. */
				if (duration_parse(
					value, &options->kill_delay) !=
				    0)

					/* Reports operation failure. */
					return -1;
				options->has_kill_delay = 1;
			} else if (signal_parse(
				       value,
				       &options->signal_number) != 0) {
				/* Reports operation failure. */
				return -1;
			}
			break;
			default:
				/* Reports operation failure. */
				return -1;
			}
		}
	}

	/* Validates the command-line arguments. */
	if (argc - index < 2 ||
	    duration_parse(argv[index], &options->duration) != 0)

		/* Reports operation failure. */
		return -1;
	*first = index + 1;
	/* Reports successful completion. */
	return 0;
}

/* Supports the duration parse operation. */
static int
duration_parse(
	const char *text,
	struct timespec *result)
{
	unsigned digit_local;
	unsigned digit_local1;
	uint64_t multiplied;
	const char *cursor;
	uint64_t seconds;
	uint64_t nanoseconds;
	uint64_t scale;
	uint64_t multiplier;
	int fractional;
	int rounded;

	cursor = text;
	seconds = 0;
	nanoseconds = 0;
	scale = 100000000U;
	fractional = 0;
	rounded = 0;

	/* Handles a failed isdigit operation. */
	if (text == NULL || !isdigit((unsigned char)*cursor))
		return -1;

	/* Continue while the operation condition remains true. */
	while (isdigit((unsigned char)*cursor)) {

		digit_local = (unsigned)(*cursor++ - '0');

		/* Handles the seconds condition. */
		if (seconds > (UINT64_MAX - digit_local) / 10U)
			return -1;
		seconds = seconds * 10U + digit_local;
	}

	/* Checks the current cursor position. */
	if (*cursor == '.') {
		fractional = 1;
		cursor++;

		/* Handles a failed isdigit operation. */
		if (!isdigit((unsigned char)*cursor))
			return -1;

		/* Continue while the operation condition remains true. */
		while (isdigit((unsigned char)*cursor)) {

			digit_local1 = (unsigned)(*cursor++ - '0');

			/* Handles the scale condition. */
			if (scale != 0) {
				nanoseconds += digit_local1 * scale;
				scale /= 10U;
			} else if (digit_local1 != 0) {
				rounded = 1;
			}
		}
	}

	/* Handles the fractional condition. */
	if (!fractional)
		nanoseconds = 0;

	/* Handles the rounded condition. */
	if (rounded)
		nanoseconds++;

	/* Dispatch the selected operation case. */
	switch (*cursor) {
	case '\0':
	case 's':
		multiplier = 1U;
		break;
	case 'm':
		multiplier = 60U;
		break;
	case 'h':
		multiplier = 60U * 60U;
		break;
	case 'd':
		multiplier = 24U * 60U * 60U;
		break;
	default:
		/* Reports operation failure. */
		return -1;
	}

	/* Checks the current cursor position. */
	if (*cursor != '\0' && cursor[1] != '\0')
		return -1;

	/* Handles the seconds condition. */
	if (seconds > (uint64_t)INT64_MAX / multiplier)
		return -1;
	seconds *= multiplier;

	/* Handles the nanoseconds condition. */
	if (nanoseconds != 0) {
				multiplied = nanoseconds * multiplier;

		seconds += multiplied / 1000000000U;
		nanoseconds = multiplied % 1000000000U;

		/* Handles the seconds condition. */
		if (seconds > (uint64_t)INT64_MAX)
			return -1;
	}
	result->tv_sec = (time_t)seconds;
	result->tv_nsec = (long)nanoseconds;

	/* Reports successful completion. */
	return 0;
}

/* Supports the signal parse operation. */
static int
signal_parse(
	const char *text,
	int *result)
{
	static const struct {
		const char *name;
		int number;
	} names[] = {
	    {"HUP", SIGHUP},	 {"INT", SIGINT},   {"QUIT", SIGQUIT},
	    {"ILL", SIGILL},	 {"TRAP", SIGTRAP}, {"ABRT", SIGABRT},
	    {"FPE", SIGFPE},	 {"KILL", SIGKILL}, {"BUS", SIGBUS},
	    {"SEGV", SIGSEGV},	 {"PIPE", SIGPIPE}, {"ALRM", SIGALRM},
	    {"TERM", SIGTERM},	 {"USR1", SIGUSR1}, {"USR2", SIGUSR2},
	    {"CHLD", SIGCHLD},	 {"CONT", SIGCONT}, {"STOP", SIGSTOP},
	    {"TSTP", SIGTSTP},	 {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU},
#ifdef SIGURG
	    {"URG", SIGURG},
#endif
#ifdef SIGWINCH
	    {"WINCH", SIGWINCH},
#endif
#ifdef SIGXCPU
	    {"XCPU", SIGXCPU},
#endif
#ifdef SIGXFSZ
	    {"XFSZ", SIGXFSZ},
#endif
	};
	char name[SIG2STR_MAX];
	size_t index;
	char *end;
	long number;

	/* Handles a failed strlen operation. */
	if (strlen(text) >= sizeof(name))
		return -1;

	/* Process each remaining element. */
	for (index = 0; text[index] != '\0'; index++)
		name[index] = (char)toupper((unsigned char)text[index]);
	name[index] = '\0';
	text = !strncmp(name, "SIG", 3) ? name + 3 : name;
	errno = 0;
	number = strtol(text, &end, 10);

	/* Handles the reported system error. */
	if (errno == 0 && *text != '\0' && *end == '\0' && number >= 0 &&
	    number < NSIG) {
		*result = (int)number;
		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining element. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		/* Selects the matching value. */
		if (!strcmp(text, names[index].name)) {
			*result = names[index].number;
			/* Reports successful completion. */
			return 0;
		}
	}

	/* Reports operation failure. */
	return -1;
}

/* Supports the timer arm operation. */
static int
timer_arm(
	timer_t timer,
	const struct timespec *duration)
{
	int function_result;
	struct itimerspec value;

	memset(&value, 0, sizeof(value));
	value.it_value = *duration;

	/* Obtains the timer settime result. */
	function_result = timer_settime(timer, 0, &value, NULL);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the send to child operation. */
static void
send_to_child(
	int signal_number)
{
	pid_t child;

	child = (pid_t)timeout_child;

	/* Checks the child process state. */
	if (child <= 0)
		return;
	(void)kill(timeout_foreground ? child : -child, signal_number);

	/* Handles the signal number condition. */
	if (signal_number != SIGKILL && signal_number != SIGCONT)
		(void)kill(timeout_foreground ? child : -child, SIGCONT);
}

/* Supports the status result operation. */
static int
status_result(
	int status)
{
	int function_result;
	int signal_number;
	struct sigaction action;
	sigset_t set;

	/* Checks the operation status. */
	if (WIFEXITED(status)) {
		/* Obtains the WEXITSTATUS result. */
		function_result = WEXITSTATUS(status);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed WIFSIGNALED operation. */
	if (!WIFSIGNALED(status))
		return 125;
	signal_number = WTERMSIG(status);
	memset(&action, 0, sizeof(action));
	ACTION_HANDLER(action, SIG_DFL);
	(void)sigemptyset(&action.sa_mask);
	(void)sigaction(signal_number, &action, NULL);
	(void)sigemptyset(&set);
	(void)sigaddset(&set, signal_number);
	(void)sigprocmask(SIG_UNBLOCK, &set, NULL);
	(void)raise(signal_number);

	/* Returns the computed result. */
	return 128 + signal_number;
}

/* Supports the kill and reap operation. */
static void
kill_and_reap(
	pid_t child)
{
	int status;

	send_to_child(SIGKILL);

	/* Continue while the operation condition remains true. */
	while (waitpid(child, &status, 0) < 0 && errno == EINTR)
		;
	timeout_child = -1;
}

/* Supports the relay signal operation. */
static void
relay_signal(
	int signal_number)
{
	/* Handles the signal number condition. */
	if (signal_number == SIGALRM) {
		timeout_reached = 1;
		send_to_child(timeout_signal);

		/* Returns the computed result. */
		return;
	}
	forwarded_signal = signal_number;
	send_to_child(signal_number);
}
