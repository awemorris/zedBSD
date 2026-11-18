/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Measures how soon a woken thread runs next to one that keeps the CPU
 * busy (WS041).  Run it on one CPU so that every process shares it.
 *
 *   wakebench pingpong N    N pipe round trips beside a busy process
 *   wakebench quiet N       the same with nothing else running
 *   wakebench sleep N       N sleeps of 1 ms beside a busy process
 *   wakebench fair SECONDS  two busy processes; their shares of the CPU
 *   wakebench starve SECONDS a busy process beside a round-trip pair
 *
 * Each mode prints one "WAKEBENCH ..." line.  Times are microseconds.
 */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

_Noreturn static void fail(const char *what);
static uint64_t now_us(void);
static pid_t start_busy(void);
static pid_t start_measured_busy(uint64_t deadline, int report);
static void stop_child(pid_t child);
static int round_trips(unsigned long count, int busy);
static int sleeps(unsigned long count);
static int fair(unsigned seconds);
static int starve(unsigned seconds);

/* Reports a failed step and ends the run. */
_Noreturn static void
fail(const char *what)
{
	printf("WAKEBENCH FAIL %s\n", what);
	exit(1);
}

/* Reads the monotonic clock in microseconds. */
static uint64_t
now_us(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/* Starts a process that only spins. */
static pid_t
start_busy(void)
{
	volatile unsigned long spin;
	pid_t child;

	child = fork();
	if (child < 0)
		fail("fork");
	if (child == 0) {
		for (spin = 0;; spin++)
			;
	}
	return child;
}

/* Ends a child and collects it. */
static void
stop_child(pid_t child)
{
	int status;

	kill(child, SIGKILL);
	(void)waitpid(child, &status, 0);
}

/*
 * Sends one byte to a partner that sends it straight back, count times,
 * and reports the average, the largest and the 99th percentile.
 */
static int
round_trips(unsigned long count, int busy)
{
	static uint32_t samples[100000];
	uint64_t start;
	uint64_t total;
	uint32_t largest;
	uint32_t sample;
	unsigned long index;
	unsigned long slot;
	int to_partner[2];
	int from_partner[2];
	pid_t partner;
	pid_t spinner;
	char byte;

	if (count == 0 || count > sizeof(samples) / sizeof(samples[0]))
		fail("count");
	if (pipe(to_partner) != 0 || pipe(from_partner) != 0)
		fail("pipe");

	/* The partner echoes every byte until the pipe closes. */
	partner = fork();
	if (partner < 0)
		fail("fork");
	if (partner == 0) {
		close(to_partner[1]);
		close(from_partner[0]);
		while (read(to_partner[0], &byte, 1) == 1)
			if (write(from_partner[1], &byte, 1) != 1)
				_exit(1);
		_exit(0);
	}
	close(to_partner[0]);
	close(from_partner[1]);

	spinner = busy ? start_busy() : 0;
	usleep(100000);

	/* Times each round trip. */
	total = 0;
	largest = 0;
	for (index = 0; index < count; index++) {
		start = now_us();
		byte = (char)index;
		if (write(to_partner[1], &byte, 1) != 1 ||
		    read(from_partner[0], &byte, 1) != 1)
			fail("round trip");
		sample = (uint32_t)(now_us() - start);
		samples[index] = sample;
		total += sample;
		if (sample > largest)
			largest = sample;
	}

	if (spinner != 0)
		stop_child(spinner);
	close(to_partner[1]);
	(void)waitpid(partner, NULL, 0);

	/* The 99th percentile: the largest after dropping the top 1 %. */
	for (slot = 0; slot < count / 100; slot++) {
		sample = 0;
		for (index = 0; index < count; index++)
			if (samples[index] > samples[sample])
				sample = (uint32_t)index;
		samples[sample] = 0;
	}
	sample = 0;
	for (index = 0; index < count; index++)
		if (samples[index] > sample)
			sample = samples[index];

	printf("WAKEBENCH %s n=%lu avg=%llu max=%u p99=%u\n",
	    busy ? "PINGPONG" : "QUIET", count,
	    (unsigned long long)(total / count), largest, sample);
	return 0;
}

/* Sleeps 1 ms count times beside a busy process and reports the overshoot. */
static int
sleeps(unsigned long count)
{
	struct timespec request;
	uint64_t start;
	uint64_t total;
	uint64_t over;
	uint64_t largest;
	unsigned long index;
	pid_t spinner;

	spinner = start_busy();
	usleep(100000);

	request.tv_sec = 0;
	request.tv_nsec = 1000000L;
	total = 0;
	largest = 0;
	for (index = 0; index < count; index++) {
		start = now_us();
		nanosleep(&request, NULL);
		over = now_us() - start;
		over = over > 1000U ? over - 1000U : 0U;
		total += over;
		if (over > largest)
			largest = over;
	}

	stop_child(spinner);
	printf("WAKEBENCH SLEEP n=%lu over_avg=%llu over_max=%llu\n",
	    count, (unsigned long long)(total / count),
	    (unsigned long long)largest);
	return 0;
}

/*
 * Runs a process that spins until the deadline and then reports the CPU
 * time it was given, in clock ticks of times().
 */
static pid_t
start_measured_busy(uint64_t deadline, int report)
{
	struct tms used;
	volatile unsigned long spin;
	long ticks;
	pid_t child;

	child = fork();
	if (child < 0)
		fail("fork");
	if (child == 0) {
		spin = 0;
		while (now_us() < deadline)
			spin++;
		times(&used);
		ticks = (long)(used.tms_utime + used.tms_stime);
		if (write(report, &ticks, sizeof(ticks)) != (ssize_t)sizeof(ticks))
			_exit(1);
		_exit(0);
	}
	return child;
}

/* Two busy processes for a while; each should get about half the CPU. */
static int
fair(unsigned seconds)
{
	uint64_t deadline;
	long first;
	long second;
	int report[2];
	pid_t a;
	pid_t b;

	if (pipe(report) != 0)
		fail("pipe");
	deadline = now_us() + (uint64_t)seconds * 1000000U;
	a = start_measured_busy(deadline, report[1]);
	b = start_measured_busy(deadline, report[1]);
	if (read(report[0], &first, sizeof(first)) != (ssize_t)sizeof(first) ||
	    read(report[0], &second, sizeof(second)) != (ssize_t)sizeof(second))
		fail("report");
	(void)waitpid(a, NULL, 0);
	(void)waitpid(b, NULL, 0);
	printf("WAKEBENCH FAIR seconds=%u ticks=%ld,%ld hz=%ld\n",
	    seconds, first, second, sysconf(_SC_CLK_TCK));
	return 0;
}

/*
 * A pair that sends one byte back and forth without pause, beside a busy
 * process: the busy one must still get its share.
 */
static int
starve(unsigned seconds)
{
	uint64_t deadline;
	unsigned long trips;
	long busy_ticks;
	int to_partner[2];
	int from_partner[2];
	int report[2];
	pid_t partner;
	pid_t spinner;
	char byte;

	if (pipe(to_partner) != 0 || pipe(from_partner) != 0 || pipe(report) != 0)
		fail("pipe");
	partner = fork();
	if (partner < 0)
		fail("fork");
	if (partner == 0) {
		close(to_partner[1]);
		close(from_partner[0]);
		while (read(to_partner[0], &byte, 1) == 1)
			if (write(from_partner[1], &byte, 1) != 1)
				_exit(1);
		_exit(0);
	}
	close(to_partner[0]);
	close(from_partner[1]);

	deadline = now_us() + (uint64_t)seconds * 1000000U;
	spinner = start_measured_busy(deadline, report[1]);
	trips = 0;
	while (now_us() < deadline) {
		byte = 0;
		if (write(to_partner[1], &byte, 1) != 1 ||
		    read(from_partner[0], &byte, 1) != 1)
			fail("round trip");
		trips++;
	}
	if (read(report[0], &busy_ticks, sizeof(busy_ticks)) !=
	    (ssize_t)sizeof(busy_ticks))
		fail("report");
	(void)waitpid(spinner, NULL, 0);
	close(to_partner[1]);
	(void)waitpid(partner, NULL, 0);
	printf("WAKEBENCH STARVE seconds=%u busy_ticks=%ld hz=%ld trips=%lu\n",
	    seconds, busy_ticks, sysconf(_SC_CLK_TCK), trips);
	return 0;
}

int
main(int argc, char **argv)
{
	unsigned long value;

	if (argc != 3) {
		fprintf(stderr, "usage: wakebench pingpong|quiet|sleep|fair|starve N\n");
		return 2;
	}
	value = strtoul(argv[2], NULL, 10);
	if (strcmp(argv[1], "pingpong") == 0)
		return round_trips(value, 1);
	if (strcmp(argv[1], "quiet") == 0)
		return round_trips(value, 0);
	if (strcmp(argv[1], "sleep") == 0)
		return sleeps(value);
	if (strcmp(argv[1], "fair") == 0)
		return fair((unsigned)value);
	if (strcmp(argv[1], "starve") == 0)
		return starve((unsigned)value);
	fprintf(stderr, "wakebench: unknown mode %s\n", argv[1]);
	return 2;
}
