/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The user side of the scheduler hint stress test: work that keeps the
 * scheduler's hint paths busy while a kernel test thread sets random hints
 * on random threads and moves them.
 *
 *   hintstress threads SECONDS   threads that compute briefly and exit, and children forked and reaped
 *   hintstress sleep SECONDS     threads in timed sleeps, each checked for waking before its time
 *   hintstress busy SECONDS N    N threads computing without a system call, the running threads to hand off
 *
 * Each mode prints one line ending in result=PASS or result=FAIL.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The threads of one round of the threads mode, and of the sleep mode. */
#define HINTSTRESS_ROUND_THREADS	8
#define HINTSTRESS_SLEEPERS		4
#define HINTSTRESS_BUSY_MAX		16

/* How much earlier than asked a sleep may end before it counts as early: the clock's grain. */
#define HINTSTRESS_EARLY_SLACK_NS	1000000LL

/*
 * What the sleepers found, written by each sleeper for itself and read by
 * the main thread after it joined them.
 */
struct sleeper {
	pthread_t thread;
	unsigned seed;
	long long sleeps;
	long long early;
	long long worst_early_ns;
};

/*
 * Whether the run is over, set once by the main thread and polled by the
 * workers, which do not call the kernel to read it.
 */
static volatile int stop_flag;

/* A value the computing threads write so that their loops are not removed. */
static volatile unsigned long sink;

static long long now_ns(void);
static void compute(unsigned long rounds);
static void *round_thread(void *argument);
static void *sleeper_thread(void *argument);
static void *busy_thread(void *argument);
static int run_threads(int seconds);
static int run_sleep(int seconds);
static int run_busy(int seconds, int count);

/*
 * Runs the mode named on the command line.
 */
int
main(
	int argc,
	char **argv)
{
	int seconds;
	int count;
	int status;

	/* Needs a mode and a time. */
	if (argc < 3) {
		fprintf(stderr, "usage: hintstress threads|sleep SECONDS | busy SECONDS N\n");
		return 2;
	}
	seconds = atoi(argv[2]);
	if (seconds <= 0)
		seconds = 1;

	/* Runs the mode. */
	if (strcmp(argv[1], "threads") == 0) {
		status = run_threads(seconds);
	} else if (strcmp(argv[1], "sleep") == 0) {
		status = run_sleep(seconds);
	} else if (strcmp(argv[1], "busy") == 0 && argc >= 4) {
		count = atoi(argv[3]);
		status = run_busy(seconds, count);
	} else {
		fprintf(stderr, "hintstress: unknown mode %s\n", argv[1]);
		status = 2;
	}

	/* Reports the mode's verdict as the exit status. */
	return status;
}

/* Reads the monotonic clock in nanoseconds. */
static long long
now_ns(
	void)
{
	struct timespec time;

	/* The clock the timed sleeps are measured against. */
	clock_gettime(CLOCK_MONOTONIC, &time);

	/* Reports the reading. */
	return (long long)time.tv_sec * 1000000000LL + (long long)time.tv_nsec;
}

/* Computes for a while without calling the kernel. */
static void
compute(
	unsigned long rounds)
{
	unsigned long index;
	unsigned long value;

	/* A loop the compiler cannot fold away. */
	value = sink;
	for (index = 0; index < rounds; index++)
		value = value * 1103515245UL + 12345UL;
	sink = value;
}

/* Computes a little and exits: one thread of a round. */
static void *
round_thread(
	void *argument)
{
	unsigned long rounds;

	/* A different length of work for each thread. */
	rounds = (unsigned long)(size_t)argument;
	compute(rounds);

	/* Done. */
	return NULL;
}

/* Sleeps for random times and counts the sleeps that ended early. */
static void *
sleeper_thread(
	void *argument)
{
	struct sleeper *sleeper;
	struct timespec request;
	long long asked;
	long long start;
	long long slept;
	long long early;

	/* Sleeps until the run ends. */
	sleeper = argument;
	while (!stop_flag) {
		/* Five to forty milliseconds, from the sleeper's own generator. */
		sleeper->seed = sleeper->seed * 1103515245U + 12345U;
		asked = (long long)(5 + (sleeper->seed >> 16) % 36) * 1000000LL;
		request.tv_sec = 0;
		request.tv_nsec = (long)asked;

		/* The sleep, measured on the clock. */
		start = now_ns();
		nanosleep(&request, NULL);
		slept = now_ns() - start;
		sleeper->sleeps++;

		/* A sleep shorter than asked, beyond the clock's grain. */
		early = asked - slept;
		if (early > HINTSTRESS_EARLY_SLACK_NS) {
			sleeper->early++;
			if (early > sleeper->worst_early_ns)
				sleeper->worst_early_ns = early;
		}
	}

	/* Done. */
	return NULL;
}

/* Computes without a system call until the run ends. */
static void *
busy_thread(
	void *argument)
{
	/* Loops on the flag only. */
	(void)argument;
	while (!stop_flag)
		compute(100000UL);

	/* Done. */
	return NULL;
}

/* Makes and reaps threads and children until the time is up. */
static int
run_threads(
	int seconds)
{
	pthread_t threads[HINTSTRESS_ROUND_THREADS];
	long long end;
	long long rounds;
	long long failures;
	pid_t child;
	int status;
	int index;
	int error;

	/* Rounds of threads and one child each. */
	rounds = 0;
	failures = 0;
	end = now_ns() + (long long)seconds * 1000000000LL;
	while (now_ns() < end) {
		/* Threads that compute a little and exit. */
		for (index = 0; index < HINTSTRESS_ROUND_THREADS; index++) {
			error = pthread_create(&threads[index], NULL, round_thread,
			    (void *)(size_t)(20000UL * (unsigned long)(index + 1)));
			if (error != 0) {
				failures++;
				threads[index] = 0;
			}
		}

		/* A child that computes a little and exits, reaped. */
		child = fork();
		if (child == 0) {
			compute(50000UL);
			_exit(7);
		}
		if (child < 0) {
			failures++;
		} else {
			if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 7)
				failures++;
		}

		/* The threads, joined. */
		for (index = 0; index < HINTSTRESS_ROUND_THREADS; index++) {
			if (threads[index] != 0)
				pthread_join(threads[index], NULL);
		}
		rounds++;
	}

	/* Reports the rounds. */
	printf("hintstress threads rounds=%lld failures=%lld result=%s\n", rounds, failures,
	    failures == 0 && rounds > 0 ? "PASS" : "FAIL");
	if (failures != 0 || rounds == 0)
		return 1;

	/* Succeeded: every round made and reaped its threads and child. */
	return 0;
}

/* Runs the sleepers for the time and checks that no sleep ended early. */
static int
run_sleep(
	int seconds)
{
	struct sleeper sleepers[HINTSTRESS_SLEEPERS];
	long long sleeps;
	long long early;
	long long worst;
	int index;

	/* Starts the sleepers. */
	memset(sleepers, 0, sizeof(sleepers));
	stop_flag = 0;
	for (index = 0; index < HINTSTRESS_SLEEPERS; index++) {
		sleepers[index].seed = (unsigned)(index * 7919 + 1);
		pthread_create(&sleepers[index].thread, NULL, sleeper_thread, &sleepers[index]);
	}

	/* Lets them sleep, then stops and gathers them. */
	sleep((unsigned)seconds);
	stop_flag = 1;
	sleeps = 0;
	early = 0;
	worst = 0;
	for (index = 0; index < HINTSTRESS_SLEEPERS; index++) {
		pthread_join(sleepers[index].thread, NULL);
		sleeps += sleepers[index].sleeps;
		early += sleepers[index].early;
		if (sleepers[index].worst_early_ns > worst)
			worst = sleepers[index].worst_early_ns;
	}

	/* Reports the sleeps. */
	printf("hintstress sleep sleeps=%lld early=%lld worst_early_us=%lld result=%s\n", sleeps, early, worst / 1000LL,
	    early == 0 && sleeps > 0 ? "PASS" : "FAIL");
	if (early != 0 || sleeps == 0)
		return 1;

	/* Succeeded: no sleep ended before its time. */
	return 0;
}

/* Runs the computing threads for the time. */
static int
run_busy(
	int seconds,
	int count)
{
	pthread_t threads[HINTSTRESS_BUSY_MAX];
	int index;

	/* At least one thread, at most the table. */
	if (count < 1)
		count = 1;
	if (count > HINTSTRESS_BUSY_MAX)
		count = HINTSTRESS_BUSY_MAX;

	/* Starts them, lets them run, stops them. */
	stop_flag = 0;
	for (index = 0; index < count; index++)
		pthread_create(&threads[index], NULL, busy_thread, NULL);
	sleep((unsigned)seconds);
	stop_flag = 1;
	for (index = 0; index < count; index++)
		pthread_join(threads[index], NULL);

	/* Reports the run. */
	printf("hintstress busy threads=%d result=PASS\n", count);

	/* Succeeded: the threads ran and ended. */
	return 0;
}
