/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A fork and posix_spawn race test: forks and spawns while other threads
 * of the process keep faulting.  Writer threads dirty a large area that
 * every fork makes copy-on-write, a toucher thread writes fresh pages in
 * order, and a third kind of thread makes system calls that copy into
 * and out of that memory (a copy-on-write fault taken in the kernel).
 *
 *   forkrace [MEGABYTES [ROUNDS [LIMIT_MS]]]
 *
 * Each fork must return within LIMIT_MS, and its child must see every
 * page the parent had before the fork (none zero) and the fresh pages as
 * one unbroken run from the first.  Each posix_spawn must start its
 * program within LIMIT_MS and report its exit status.  Prints one line
 * per round and "FORKRACE PASS", or "FORKRACE FAIL" with the reason; a
 * watchdog prints "FORKRACE FAIL timeout" when the whole run hangs.  It
 * exits 0 when every check passed, else 1.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The size of a page the checks assume. */
#define RACE_PAGE 4096U

/* The writer threads that dirty the large area. */
#define RACE_WRITERS 2U

/* The threads that copy in and out through system calls. */
#define RACE_COPIERS 1U

/* The fresh pages the toucher writes in order. */
#define RACE_FRESH_PAGES 4096U

/* The default area, rounds and time limit. */
#define RACE_DEFAULT_MEGABYTES 64UL
#define RACE_DEFAULT_ROUNDS 5UL
#define RACE_DEFAULT_LIMIT_MS 5000UL

/* The exit status the spawned shell reports. */
#define RACE_SPAWN_STATUS 7

/* The seconds after which the watchdog ends a run that hangs. */
#define RACE_WATCHDOG_SECONDS 240U

extern char **environ;

/* The area the writers dirty; every page holds a byte that is never zero. */
static unsigned char *race_area;
static size_t race_area_pages;

/* The pages the toucher writes in order; race_fresh_done of them so far. */
static unsigned char *race_fresh;
static volatile size_t race_fresh_done;

/* Set to stop the threads. */
static volatile int race_stop;

/* What the copier threads move through their pipe. */
static int race_pipe[2];

static int race_number(const char *text, unsigned long minimum, unsigned long maximum, unsigned long *value);
static long long race_now_ms(void);
static void race_watchdog(int signo);
static void *race_writer(void *argument);
static void *race_toucher(void *argument);
static void *race_copier(void *argument);
static int race_child_check(void);
static int race_fork_round(unsigned long round, unsigned long limit_ms);
static int race_spawn_round(unsigned long round, unsigned long limit_ms);

/*
 * Starts the threads, forks and spawns the rounds, and reports.
 */
int
main(
	int argc,
	char **argv)
{
	pthread_t threads[RACE_WRITERS + RACE_COPIERS + 1U];
	unsigned long megabytes;
	unsigned long rounds;
	unsigned long limit_ms;
	unsigned long round;
	unsigned count;
	unsigned index;
	size_t page;
	int failed;
	int status;

	/* Reads the command line. */
	megabytes = RACE_DEFAULT_MEGABYTES;
	rounds = RACE_DEFAULT_ROUNDS;
	limit_ms = RACE_DEFAULT_LIMIT_MS;
	if (argc > 1 && race_number(argv[1], 1UL, 1024UL, &megabytes) != 0)
		return 2;
	if (argc > 2 && race_number(argv[2], 1UL, 1000UL, &rounds) != 0)
		return 2;
	if (argc > 3 && race_number(argv[3], 1UL, 600000UL, &limit_ms) != 0)
		return 2;

	/* A run that hangs is reported rather than left hanging. */
	(void)signal(SIGALRM, race_watchdog);
	(void)alarm(RACE_WATCHDOG_SECONDS);

	/* Every page of the area holds a byte that is never zero. */
	race_area_pages = (size_t)megabytes * 1024U * 1024U / RACE_PAGE;
	race_area = malloc(race_area_pages * RACE_PAGE);
	race_fresh = malloc((size_t)RACE_FRESH_PAGES * RACE_PAGE);
	if (race_area == NULL || race_fresh == NULL) {
		printf("FORKRACE FAIL no memory\n");
		return 1;
	}
	for (page = 0; page < race_area_pages; page++)
		race_area[page * RACE_PAGE] = 1;

	/* The copiers' pipe. */
	if (pipe(race_pipe) != 0) {
		printf("FORKRACE FAIL pipe errno=%d\n", errno);
		return 1;
	}

	/* Starts the writers, the copiers and the toucher. */
	count = 0;
	for (index = 0; index < RACE_WRITERS; index++) {
		status = pthread_create(&threads[count], NULL, race_writer, (void *)(unsigned long)index);
		if (status != 0) {
			printf("FORKRACE FAIL pthread_create %d\n", status);
			return 1;
		}
		count++;
	}
	for (index = 0; index < RACE_COPIERS; index++) {
		status = pthread_create(&threads[count], NULL, race_copier, (void *)(unsigned long)index);
		if (status != 0) {
			printf("FORKRACE FAIL pthread_create %d\n", status);
			return 1;
		}
		count++;
	}
	status = pthread_create(&threads[count], NULL, race_toucher, NULL);
	if (status != 0) {
		printf("FORKRACE FAIL pthread_create %d\n", status);
		return 1;
	}
	count++;

	/* Lets the threads get going. */
	(void)usleep(200000);

	/* Forks, then spawns, each round while the threads run. */
	failed = 0;
	for (round = 0; round < rounds; round++) {
		failed += race_fork_round(round, limit_ms);
		failed += race_spawn_round(round, limit_ms);
	}

	/* Stops the threads. */
	race_stop = 1;
	for (index = 0; index < count; index++)
		(void)pthread_join(threads[index], NULL);

	/* Reports the run. */
	if (failed != 0) {
		printf("FORKRACE FAIL %d checks\n", failed);
		return 1;
	}
	printf("FORKRACE PASS\n");
	return 0;
}

/* Reads a decimal number in [minimum, maximum] into *value. */
static int
race_number(
	const char *text,
	unsigned long minimum,
	unsigned long maximum,
	unsigned long *value)
{
	char *end;

	/* Reads it whole. */
	errno = 0;
	*value = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return EINVAL;

	/* Refuses one outside the range. */
	if (*value < minimum || *value > maximum)
		return EINVAL;

	/* Succeeded: the value is in range. */
	return 0;
}

/* Reports the monotonic clock in milliseconds. */
static long long
race_now_ms(
	void)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000L;
}

/* Ends a run that hangs, saying so. */
static void
race_watchdog(
	int signo)
{
	static const char message[] = "FORKRACE FAIL timeout\n";

	(void)signo;
	(void)write(STDOUT_FILENO, message, sizeof(message) - 1U);
	_exit(1);
}

/* Dirties the area's pages in a loop, never storing zero. */
static void *
race_writer(
	void *argument)
{
	size_t page;
	unsigned char value;

	/* Each writer starts at its own place. */
	page = (size_t)(unsigned long)argument * 7919U;
	while (!race_stop) {
		page = (page + 1U) % race_area_pages;

		/* The next value after 255 is 1, so a page never reads zero. */
		value = (unsigned char)(race_area[page * RACE_PAGE] + 1U);
		if (value == 0)
			value = 1;
		race_area[page * RACE_PAGE] = value;
	}
	return NULL;
}

/* Writes the fresh pages once each, in order, slowly enough to last the run. */
static void *
race_toucher(
	void *argument)
{
	size_t page;

	(void)argument;
	for (page = 0; page < RACE_FRESH_PAGES && !race_stop; page++) {
		race_fresh[page * RACE_PAGE] = 0xa5;
		race_fresh_done = page + 1U;
		if ((page & 15U) == 15U)
			(void)usleep(1000);
	}
	return NULL;
}

/*
 * Copies through a pipe in a loop: write() reads a page of the area in
 * the kernel and read() stores into another, a copy-on-write fault taken
 * inside the system call once a fork has shared the page.
 */
static void *
race_copier(
	void *argument)
{
	struct timespec now;
	size_t page;
	ssize_t count;

	/* Each copier starts at its own place. */
	page = (size_t)(unsigned long)argument * 104729U;
	while (!race_stop) {
		page = (page + 2U) % (race_area_pages - 1U);

		/* The kernel reads one page and writes the next; neither is ever zero. */
		count = write(race_pipe[1], &race_area[page * RACE_PAGE], 64U);
		if (count == 64)
			(void)read(race_pipe[0], &race_area[(page + 1U) * RACE_PAGE], 64U);

		/* The kernel also writes this thread's stack. */
		(void)clock_gettime(CLOCK_MONOTONIC, &now);
	}
	return NULL;
}

/*
 * In a fork child: checks that no page of the area went missing (read
 * zero) and that the fresh pages are one run from the first.  Reports 0
 * when both hold.
 */
static int
race_child_check(
	void)
{
	size_t page;
	size_t seen;
	size_t done;

	/* Every page of the area held a byte that is never zero. */
	for (page = 0; page < race_area_pages; page++) {
		if (race_area[page * RACE_PAGE] == 0)
			return 1;
	}

	/* The fresh pages were written in order: no hole before the last one written. */
	done = race_fresh_done;
	seen = 0;
	while (seen < RACE_FRESH_PAGES && race_fresh[seen * RACE_PAGE] == 0xa5)
		seen++;
	if (seen < done)
		return 2;
	for (page = seen; page < RACE_FRESH_PAGES; page++) {
		if (race_fresh[page * RACE_PAGE] != 0)
			return 3;
	}

	/* Succeeded: the child's memory is a snapshot of the parent's. */
	return 0;
}

/* Forks once and checks the time and the child's memory: 0 or 1. */
static int
race_fork_round(
	unsigned long round,
	unsigned long limit_ms)
{
	long long started;
	long long elapsed;
	pid_t child;
	int status;

	/* Forks; the child checks its memory and reports by its status. */
	started = race_now_ms();
	child = fork();
	if (child == 0)
		_exit(race_child_check());
	elapsed = race_now_ms() - started;
	if (child < 0) {
		printf("FORKRACE FAIL fork round=%lu errno=%d\n", round, errno);
		return 1;
	}

	/* Collects the child's verdict. */
	if (waitpid(child, &status, 0) != child) {
		printf("FORKRACE FAIL waitpid round=%lu errno=%d\n", round, errno);
		return 1;
	}
	printf("forkrace fork round=%lu ms=%lld status=%d\n", round, elapsed, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	(void)fflush(stdout);

	/* The fork must have been quick and the child's memory whole. */
	if ((unsigned long long)elapsed > limit_ms) {
		printf("FORKRACE FAIL fork round=%lu took %lld ms\n", round, elapsed);
		return 1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("FORKRACE FAIL fork child round=%lu\n", round);
		return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Spawns a shell that exits with a known status: 0 or 1. */
static int
race_spawn_round(
	unsigned long round,
	unsigned long limit_ms)
{
	char *arguments[4];
	long long started;
	long long elapsed;
	pid_t child;
	int status;
	int error;

	/* The shell only exits. */
	arguments[0] = "sh";
	arguments[1] = "-c";
	arguments[2] = "exit 7";
	arguments[3] = NULL;

	/* Spawns it while the threads run. */
	started = race_now_ms();
	error = posix_spawn(&child, "/bin/sh", NULL, NULL, arguments, environ);
	elapsed = race_now_ms() - started;
	if (error != 0) {
		printf("FORKRACE FAIL posix_spawn round=%lu error=%d\n", round, error);
		return 1;
	}

	/* Collects its status. */
	if (waitpid(child, &status, 0) != child) {
		printf("FORKRACE FAIL waitpid spawn round=%lu errno=%d\n", round, errno);
		return 1;
	}
	printf("forkrace spawn round=%lu ms=%lld status=%d\n", round, elapsed, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	(void)fflush(stdout);

	/* The spawn must have been quick and the shell's status its own. */
	if ((unsigned long long)elapsed > limit_ms) {
		printf("FORKRACE FAIL spawn round=%lu took %lld ms\n", round, elapsed);
		return 1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != RACE_SPAWN_STATUS) {
		printf("FORKRACE FAIL spawn status round=%lu\n", round);
		return 1;
	}

	/* Succeeded. */
	return 0;
}
