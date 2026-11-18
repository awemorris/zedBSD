/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Times a simple command (POSIX XCU time).
 *
 *	time [-p] utility [argument...]
 *
 * The utility runs as a child and, when it has finished, the elapsed real
 * time and the user and system CPU time it used are written to standard
 * error in the form -p names:
 *
 *	real %f
 *	user %f
 *	sys %f
 *
 * The same form is written without -p.  time exits with the utility's
 * status, 126 when the utility was found but could not be run, and 127
 * when it could not be found.  A utility killed by a signal gives 128 plus
 * the signal number, as the shell reports it.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The status when the utility was found but could not be run. */
#define TIME_STATUS_NOT_RUNNABLE 126

/* The status when the utility could not be found. */
#define TIME_STATUS_NOT_FOUND 127

static int read_options(int argc, char **argv);
static int run_utility(char **argv, int *wait_status, struct rusage *usage);
static void write_times(const struct timespec *start, const struct timespec *end, const struct rusage *usage);
static void usage(void);

/*
 * Runs time.
 */
int
main(
	int argc,
	char **argv)
{
	struct timespec start;
	struct timespec end;
	struct rusage usage_of_child;
	int first;
	int wait_status;
	int status;
	int signaled;

	/* Reads the options; the utility follows them. */
	first = read_options(argc, argv);
	if (first >= argc)
		usage();

	/*
	 * An interrupt or quit from the terminal goes to the utility; time
	 * stays to report what the utility used until then.
	 */
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);

	/* Runs the utility between two readings of the clock. */
	clock_gettime(CLOCK_MONOTONIC, &start);
	status = run_utility(argv + first, &wait_status, &usage_of_child);
	clock_gettime(CLOCK_MONOTONIC, &end);

	/* A utility that could not be started is not timed. */
	if (status != 0)
		return status;

	/* Writes the times. */
	write_times(&start, &end, &usage_of_child);

	/* A utility killed by a signal is reported as the shell does. */
	signaled = WIFSIGNALED(wait_status);
	if (signaled)
		return 128 + WTERMSIG(wait_status);

	/* Succeeded: the utility's own exit status. */
	return WEXITSTATUS(wait_status);
}

/* Reads the options and returns the index of the utility operand. */
static int
read_options(
	int argc,
	char **argv)
{
	int option;

	/* -p is the only option; its form is also the default one. */
	for (;;) {
		option = getopt(argc, argv, "p");
		if (option == -1)
			break;

		/* Refuses anything but -p. */
		if (option != 'p')
			usage();
	}

	/* Reports where the utility operand starts. */
	return optind;
}

/*
 * Runs the utility and waits for it, storing its wait status and the
 * resources it used.  Returns 0 when it ran, or 126 or 127 when it could
 * not be run, and 1 when time itself failed.
 */
static int
run_utility(
	char **argv,
	int *wait_status,
	struct rusage *usage)
{
	int report[2];
	int exec_error;
	int error;
	ssize_t got;
	pid_t child;
	pid_t waited;

	/*
	 * A pipe closed on exec tells whether the utility started: it carries
	 * the error of an exec that failed and nothing otherwise.
	 */
	error = pipe(report);
	if (error != 0) {
		fprintf(stderr, "time: pipe: %s\n", strerror(errno));
		return 1;
	}

	/* Closes the writing end in the utility once it starts. */
	fcntl(report[1], F_SETFD, FD_CLOEXEC);

	/* Starts the utility in a child. */
	child = fork();
	if (child < 0) {
		fprintf(stderr, "time: fork: %s\n", strerror(errno));
		close(report[0]);
		close(report[1]);
		return 1;
	}

	/*
	 * The child restores the signals time ignores, runs the utility, and
	 * reports a failed exec through the pipe.
	 */
	if (child == 0) {
		close(report[0]);
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		execvp(argv[0], argv);
		exec_error = errno;
		got = write(report[1], &exec_error, sizeof(exec_error));
		(void)got;
		_exit(TIME_STATUS_NOT_FOUND);
	}

	/* Learns whether the exec failed; the pipe closes when it succeeds. */
	close(report[1]);
	exec_error = 0;
	do {
		got = read(report[0], &exec_error, sizeof(exec_error));
	} while (got < 0 && errno == EINTR);
	close(report[0]);

	/* Waits for the child, collecting what it used. */
	memset(usage, 0, sizeof(*usage));
	do {
		waited = wait4(child, wait_status, 0, usage);
	} while (waited < 0 && errno == EINTR);
	if (waited < 0) {
		fprintf(stderr, "time: wait: %s\n", strerror(errno));
		return 1;
	}

	/* A utility that could not be run is reported by its error. */
	if (got == (ssize_t)sizeof(exec_error)) {
		fprintf(stderr, "time: %s: %s\n", argv[0], strerror(exec_error));
		if (exec_error == ENOENT)
			return TIME_STATUS_NOT_FOUND;
		return TIME_STATUS_NOT_RUNNABLE;
	}

	/* Succeeded: the utility ran and has finished. */
	return 0;
}

/* Writes the real, user and system times in seconds to standard error. */
static void
write_times(
	const struct timespec *start,
	const struct timespec *end,
	const struct rusage *usage)
{
	long long seconds;
	long nanoseconds;
	double real;
	double user;
	double system;

	/* The elapsed time, borrowing a second for the fraction if needed. */
	seconds = (long long)end->tv_sec - (long long)start->tv_sec;
	nanoseconds = end->tv_nsec - start->tv_nsec;
	if (nanoseconds < 0) {
		seconds--;
		nanoseconds += 1000000000L;
	}

	/* Converts the elapsed time to seconds. */
	real = (double)seconds + (double)nanoseconds / 1e9;

	/* The CPU times the utility and its waited-for children used. */
	user = (double)usage->ru_utime.tv_sec + (double)usage->ru_utime.tv_usec / 1e6;
	system = (double)usage->ru_stime.tv_sec + (double)usage->ru_stime.tv_usec / 1e6;

	/* Writes the three lines in the form -p specifies. */
	fprintf(stderr, "real %.2f\n", real);
	fprintf(stderr, "user %.2f\n", user);
	fprintf(stderr, "sys %.2f\n", system);
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the one option. */
	fprintf(stderr, "usage: time [-p] utility [argument...]\n");
	exit(1);
}
