/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
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

static void
send_to_child(int signal_number)
{
	pid_t child = (pid_t)timeout_child;

	if (child <= 0)
		return;
	(void)kill(timeout_foreground ? child : -child, signal_number);
	if (signal_number != SIGKILL && signal_number != SIGCONT)
		(void)kill(timeout_foreground ? child : -child, SIGCONT);
}

static void
relay_signal(int signal_number)
{
	if (signal_number == SIGALRM) {
		timeout_reached = 1;
		send_to_child(timeout_signal);
		return;
	}
	forwarded_signal = signal_number;
	send_to_child(signal_number);
}

static int
duration_parse(const char *text, struct timespec *result)
{
	const char *cursor = text;
	uint64_t seconds = 0;
	uint64_t nanoseconds = 0;
	uint64_t scale = 100000000U;
	uint64_t multiplier;
	int fractional = 0;
	int rounded = 0;

	if (text == NULL || !isdigit((unsigned char)*cursor))
		return -1;
	while (isdigit((unsigned char)*cursor)) {
		unsigned digit = (unsigned)(*cursor++ - '0');

		if (seconds > (UINT64_MAX - digit) / 10U)
			return -1;
		seconds = seconds * 10U + digit;
	}
	if (*cursor == '.') {
		fractional = 1;
		cursor++;
		if (!isdigit((unsigned char)*cursor))
			return -1;
		while (isdigit((unsigned char)*cursor)) {
			unsigned digit = (unsigned)(*cursor++ - '0');

			if (scale != 0) {
				nanoseconds += digit * scale;
				scale /= 10U;
			} else if (digit != 0) {
				rounded = 1;
			}
		}
	}
	if (!fractional)
		nanoseconds = 0;
	if (rounded)
		nanoseconds++;
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
		return -1;
	}
	if (*cursor != '\0' && cursor[1] != '\0')
		return -1;
	if (seconds > (uint64_t)INT64_MAX / multiplier)
		return -1;
	seconds *= multiplier;
	if (nanoseconds != 0) {
		uint64_t multiplied = nanoseconds * multiplier;

		seconds += multiplied / 1000000000U;
		nanoseconds = multiplied % 1000000000U;
		if (seconds > (uint64_t)INT64_MAX)
			return -1;
	}
	result->tv_sec = (time_t)seconds;
	result->tv_nsec = (long)nanoseconds;
	return 0;
}

static int
signal_parse(const char *text, int *result)
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

	if (strlen(text) >= sizeof(name))
		return -1;
	for (index = 0; text[index] != '\0'; index++)
		name[index] = (char)toupper((unsigned char)text[index]);
	name[index] = '\0';
	text = !strncmp(name, "SIG", 3) ? name + 3 : name;
	errno = 0;
	number = strtol(text, &end, 10);
	if (errno == 0 && *text != '\0' && *end == '\0' && number >= 0 &&
	    number < NSIG) {
		*result = (int)number;
		return 0;
	}
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		if (!strcmp(text, names[index].name)) {
			*result = names[index].number;
			return 0;
		}
	}
	return -1;
}

static int
parse_options(int argc, char **argv, struct options *options, int *first)
{
	int index;

	memset(options, 0, sizeof(*options));
	options->signal_number = SIGTERM;
	for (index = 1; index < argc; index++) {
		const char *argument = argv[index];
		unsigned position;

		if (argument[0] != '-' || argument[1] == '\0')
			break;
		if (!strcmp(argument, "--")) {
			index++;
			break;
		}
		for (position = 1; argument[position] != '\0'; position++) {
			switch (argument[position]) {
			case 'f':
				options->foreground = 1;
				break;
			case 'p':
				options->preserve_status = 1;
				break;
			case 'k':
			case 's': {
				char option = argument[position];
				const char *value;

				if (argument[position + 1U] != '\0') {
					value = argument + position + 1U;
					position =
					    (unsigned)strlen(argument) - 1U;
				} else if (++index < argc) {
					value = argv[index];
				} else {
					return -1;
				}
				if (option == 'k') {
					if (duration_parse(
						value, &options->kill_delay) !=
					    0)
						return -1;
					options->has_kill_delay = 1;
				} else if (signal_parse(
					       value,
					       &options->signal_number) != 0) {
					return -1;
				}
				break;
			}
			default:
				return -1;
			}
		}
	}
	if (argc - index < 2 ||
	    duration_parse(argv[index], &options->duration) != 0)
		return -1;
	*first = index + 1;
	return 0;
}

static int
timer_arm(timer_t timer, const struct timespec *duration)
{
	struct itimerspec value;

	memset(&value, 0, sizeof(value));
	value.it_value = *duration;
	return timer_settime(timer, 0, &value, NULL);
}

static int
status_result(int status)
{
	int signal_number;
	struct sigaction action;
	sigset_t set;

	if (WIFEXITED(status))
		return WEXITSTATUS(status);
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
	return 128 + signal_number;
}

static void
kill_and_reap(pid_t child)
{
	int status;

	send_to_child(SIGKILL);
	while (waitpid(child, &status, 0) < 0 && errno == EINTR)
		;
	timeout_child = -1;
}

int
main(int argc, char **argv)
{
	static const int relayed[] = {SIGHUP,  SIGINT,	SIGQUIT, SIGABRT,
				      SIGPIPE, SIGTERM, SIGUSR1, SIGUSR2,
				      SIGXCPU, SIGXFSZ};
	struct sigaction action;
	struct sigevent event;
	struct options options;
	timer_t timer;
	pid_t child;
	int first, status = 0;
	int kill_timer_armed = 0;
	int timer_created = 0;
	unsigned index;

	if (parse_options(argc, argv, &options, &first) != 0) {
		fprintf(stderr,
			"usage: timeout [-fp] [-k time] [-s signal_name] "
			"duration utility [argument ...]\n");
		return 125;
	}
	child = fork();
	if (child < 0) {
		command_error("timeout", NULL);
		return 125;
	}
	if (child == 0) {
		if (!options.foreground)
			(void)setpgid(0, 0);
		command_exec(argv[first], &argv[first]);
		_exit(errno == ENOENT ? 127 : 126);
	}
	timeout_child = child;
	timeout_foreground = options.foreground;
	timeout_signal = options.signal_number;
	if (!options.foreground)
		(void)setpgid(child, child);
	memset(&action, 0, sizeof(action));
	ACTION_HANDLER(action, relay_signal);
	(void)sigemptyset(&action.sa_mask);
	for (index = 0; index < sizeof(relayed) / sizeof(relayed[0]); index++) {
		if (sigaction(relayed[index], &action, NULL) != 0)
			goto internal_failure;
	}
	if (sigaction(SIGALRM, &action, NULL) != 0)
		goto internal_failure;
	memset(&action, 0, sizeof(action));
	ACTION_HANDLER(action, SIG_IGN);
	(void)sigemptyset(&action.sa_mask);
	if (sigaction(SIGTTIN, &action, NULL) != 0 ||
	    sigaction(SIGTTOU, &action, NULL) != 0)
		goto internal_failure;
	if (options.duration.tv_sec != 0 || options.duration.tv_nsec != 0) {
		memset(&event, 0, sizeof(event));
		event.sigev_notify = SIGEV_SIGNAL;
		event.sigev_signo = SIGALRM;
		if (timer_create(CLOCK_MONOTONIC, &event, &timer) != 0)
			goto internal_failure;
		timer_created = 1;
		if (timer_arm(timer, &options.duration) != 0)
			goto internal_failure;
	}
	for (;;) {
		pid_t waited = waitpid(child, &status, 0);

		if (waited == child)
			break;
		if (waited < 0 && errno == EINTR) {
			if ((timeout_reached || forwarded_signal != 0) &&
			    options.has_kill_delay && !kill_timer_armed) {
				if (!timer_created) {
					memset(&event, 0, sizeof(event));
					event.sigev_notify = SIGEV_SIGNAL;
					event.sigev_signo = SIGALRM;
					if (timer_create(CLOCK_MONOTONIC,
							 &event, &timer) != 0)
						goto internal_failure;
					timer_created = 1;
				}
				if (options.kill_delay.tv_sec == 0 &&
				    options.kill_delay.tv_nsec == 0) {
					send_to_child(SIGKILL);
				} else {
					timeout_signal = SIGKILL;
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
	if (timer_created)
		(void)timer_delete(timer);
	if (timeout_reached && !options.preserve_status)
		return 124;
	return status_result(status);

internal_failure:
	command_error("timeout", NULL);
	kill_and_reap(child);
	if (timer_created)
		(void)timer_delete(timer);
	return 125;
}
