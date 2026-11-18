/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD top userland command.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <uapi/process.h>
#include <uapi/system.h>
#include <uapi/sysctl.h>

#define TOP_MAX_PROCESSES 256U
#define TOP_CLEAR_SCREEN "\033[H\033[2J"
#define fputs(text, stream)                                                    \
	((void)(stream),                                                       \
	 command_write_all(                                                    \
	     STDOUT_FILENO,                                                    \
	     strcmp((text), "\033[H\033[J") == 0 ? TOP_CLEAR_SCREEN : (text),  \
	     strlen(strcmp((text), "\033[H\033[J") == 0 ? TOP_CLEAR_SCREEN     \
							: (text))))
#define puts(text) printf("%s\n", (text))

/* How long the first draw waits between its two samples of the processes' CPU time, in nanoseconds. */
#define TOP_FIRST_SAMPLE_NANOSECONDS 250000000L

/*
 * One process's CPU time when the processes were last sampled.
 */
struct top_cpu_sample {
	/* The process. */
	int32_t pid;

	/* Its CPU time then, in 1/KERN_PROCESS_TIMES_HZ of a second. */
	uint64_t cpu_ticks;
};

/*
 * The processes' CPU times at the last sample, which each draw replaces.
 *
 * A draw turns the CPU time each process used since this sample into its
 * %CPU.  top_previous_count is 0 until the first sample.
 */
static struct top_cpu_sample top_previous[TOP_MAX_PROCESSES];

/* How many entries of top_previous the last sample filled. */
static size_t top_previous_count;

/* When the last sample was taken, on the monotonic clock. */
static struct timespec top_previous_time;

static void draw(int fd, int batch);
static int snapshot(int fd, struct process_info *p, size_t *n);
static void human(uint64_t bytes, char out[16]);
static const char *user_name(uid_t uid, char b[16]);
static char state_letter(unsigned state);
static const char *leaf(const char *s);
static void cpu_line(void);
static int cpu_totals(uint64_t totals[4]);
static void cpu_prime(int fd);
static void cpu_remember(const struct process_info *processes, size_t count, const struct timespec *when);
static double cpu_elapsed(const struct timespec *now);
static double cpu_percent(const struct process_info *process, double elapsed);

/*
 * Runs the top command.
 */
int
main(
	int argc,
	char **argv)
{
	char key;
	int fd, batch, iterations, delay, i;
	struct termios saved, raw;
	struct timespec pause;
	int tty;
	struct pollfd input;

	/* Process each remaining command-line operand. */
	batch = 0;
	iterations = -1;
	delay = 1000;
	tty = 0;
	for (i = 1; i < argc; i++) {
		/* Handles the selected command-line operation. */
		if (!strcmp(argv[i], "-b"))
			batch = 1;
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			iterations = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			delay = atoi(argv[++i]) * 1000;
		} else {
			fprintf(stderr,
				"usage: top [-b] [-n count] [-d seconds]\n");

			/* Reports operation failure. */
			return 2;
		}
	}
	fd = open("/dev/system", O_RDONLY);

	/* Checks the file descriptor. */
	if (fd < 0) {
		command_error("top", "/dev/system");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed isatty operation. */
	if (!batch && isatty(STDIN_FILENO) &&
	    tcgetattr(STDIN_FILENO, &saved) == 0) {
		raw = saved;
		raw.c_lflag &= ~(ECHO | ICANON);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;

		/* Handles a failed tcsetattr operation. */
		if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
			tty = 1;
		fputs("\033[?25l", stdout);
	}

	/* Samples the processes' CPU time once before the first draw, so that its %CPU covers a moment. */
	cpu_prime(fd);

	/* Process each element required by the operation. */
	for (i = 0; iterations < 0 || i < iterations; i++) {
		input.fd = STDIN_FILENO;
		input.events = POLLIN;
		input.revents = 0;

		draw(fd, batch);

		/* Handles the iterations condition. */
		if (iterations >= 0 && i + 1 >= iterations)
			break;

		/*
		 * Batch mode reads no keys: it waits the whole delay, since a
		 * standard input at its end (a pipe, a closed SSH input) would
		 * otherwise end every wait at once and the draws would come
		 * with no time between them.
		 */
		if (batch) {
			pause.tv_sec = delay / 1000;
			pause.tv_nsec = (long)(delay % 1000) * 1000000L;
			(void)nanosleep(&pause, NULL);
			continue;
		}

		/* Handles a failed poll operation. */
		if (poll(&input, 1, delay) > 0 &&
		    read(STDIN_FILENO, &key, 1) == 1 &&
		    (key == 'q' || key == 'Q'))
			break;
	}

	/* Handles the tty condition. */
	if (tty) {
		tcsetattr(STDIN_FILENO, TCSANOW, &saved);
		fputs("\033[?25h\n", stdout);
	}
	close(fd);

	/* Reports successful completion. */
	return 0;
}

/* Supports the draw operation. */
static void
draw(
	int fd,
	int batch)
{
	char ub[16], virt[16];
	const char *u;
	struct vm_statistics vm;
	struct process_info p[TOP_MAX_PROCESSES];
	struct timespec now;
	double elapsed;
	uint64_t ticks;
	size_t count, i;
	unsigned running, sleeping, stopped, zombie;
	char total[16], freeb[16], used[16], swap[16], swapfree[16];

	count = 0;
	running = 0;
	sleeping = 0;
	stopped = 0;
	zombie = 0;

	/* Handles a failed ioctl operation. */
	if (ioctl(fd, KERN_SYSTEM_GET_VMSTAT, &vm) != 0 ||
	    snapshot(fd, p, &count) != 0)

		/* Returns the computed result. */
		return;

	/* Process each remaining element. */
	for (i = 0; i < count; i++) {
		/* Checks the current pointer. */
		if (p[i].state == 1)
			running++;
		else if (p[i].state == 2)
			stopped++;
		else if (p[i].state == 4)
			zombie++;
		else
			sleeping++;
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed = cpu_elapsed(&now);
	human(vm.physical_total, total);
	human(vm.physical_free, freeb);
	human(vm.physical_total - vm.physical_free, used);
	human(vm.swap_total * KERN_SYSTEM_SWAP_PAGE_SIZE, swap);
	human(vm.swap_free * KERN_SYSTEM_SWAP_PAGE_SIZE, swapfree);

	/* Handles the batch condition. */
	if (!batch)
		fputs("\033[H\033[J", stdout);
	printf("top - up %lld days, %02lld:%02lld,  %zu tasks\n",
	       (long long)(now.tv_sec / 86400),
	       (long long)(now.tv_sec / 3600 % 24),
	       (long long)(now.tv_sec / 60 % 60), count);
	printf("Tasks: %3zu total, %3u running, %3u sleeping, %3u stopped, %3u "
	       "zombie\n",
	       count, running, sleeping, stopped, zombie);
	cpu_line();
	printf("MiB Mem : %8s total, %8s free, %8s used\n", total, freeb, used);
	printf("MiB Swap: %8s total, %8s free\n\n", swap, swapfree);
	printf("VM I/O: %llu page-in, %llu page-out, %llu swapped\n\n",
	       (unsigned long long)vm.vm_page_in,
	       (unsigned long long)vm.vm_page_out,
	       (unsigned long long)vm.vm_swapped);
	puts("    PID USER      PR  NI    VIRT    RES    SHR S  %CPU %MEM     "
	     "TIME+ COMMAND");

	/* Process each remaining element. */
	for (i = 0; i < count; i++) {
		u = user_name(p[i].uid, ub);
		human(p[i].virtual_bytes, virt);
		ticks = p[i].cpu_ticks;
		printf("%7d %-8.8s  20   0 %7s      0      0 %c %5.1f  0.0 "
		       "%3llu:%02llu.%02llu %s\n",
		       p[i].pid, u, virt, state_letter(p[i].state),
		       cpu_percent(&p[i], elapsed),
		       (unsigned long long)(ticks / KERN_PROCESS_TIMES_HZ / 60U),
		       (unsigned long long)(ticks / KERN_PROCESS_TIMES_HZ % 60U),
		       (unsigned long long)(ticks % KERN_PROCESS_TIMES_HZ * 100U / KERN_PROCESS_TIMES_HZ),
		       p[i].command[0] ? leaf(p[i].command) : "kernel");
	}
	fflush(stdout);

	/* This draw's CPU times are what the next one measures from. */
	cpu_remember(p, count, &now);
}

/* Supports the snapshot operation. */
static int
snapshot(
	int fd,
	struct process_info *p,
	size_t *n)
{
	int32_t cursor;
	size_t used;

	/* Continue while the operation condition remains true. */
	cursor = -1;
	used = 0;
	while (used < TOP_MAX_PROCESSES) {
		memset(&p[used], 0, sizeof(p[used]));
		p[used].pid = cursor;

		/* Handles a failed ioctl operation. */
		if (ioctl(fd, KERN_SYSTEM_GET_PROCESS, &p[used]) != 0) {
			/* Handles the reported system error. */
			if (errno == ENOENT)
				break;

			/* Reports operation failure. */
			return -1;
		}
		p[used].command[KERN_SYSTEM_PROCESS_COMMAND_MAX - 1U] = '\0';
		cursor = p[used].pid;
		used++;
	}
	*n = used;
	/* Reports successful completion. */
	return 0;
}

/* Supports the human operation. */
static void
human(
	uint64_t bytes,
	char out[16])
{
	static const char units[] = "BKMGT";
	unsigned u;
	uint64_t scale;

	/* Process each remaining element. */
	u = 0;
	scale = 1;
	while (u + 1U < sizeof(units) - 1U && bytes >= scale * 1024U) {
		scale *= 1024U;
		u++;
	}

	/* Handles the u condition. */
	if (u == 0)
		snprintf(out, 16, "%lluB", (unsigned long long)bytes);
	else
		snprintf(out, 16, "%llu%c",
			 (unsigned long long)((bytes + scale / 2U) / scale),
			 units[u]);
}

/* Supports the user name operation. */
static const char *
user_name(
	uid_t uid,
	char b[16])
{
	/* Handles the uid condition. */
	if (uid == 0)
		return "root";
	snprintf(b, 16, "%u", (unsigned)uid);

	/* Returns the computed result. */
	return b;
}

/* Supports the state letter operation. */
static char
state_letter(
	unsigned state)
{
	char function_result;
	static const char map[] = "?RTXZX";

	/* Computes the function result. */
	function_result = state < sizeof(map) - 1U ? map[state] : '?';

	/* Returns the computed result. */
	return function_result;
}

/* Supports the leaf operation. */
static const char *
leaf(
	const char *s)
{
	const char *p;

	p = strrchr(s, '/');

	/* Returns the computed result. */
	return p ? p + 1 : s;
}

/*
 * Prints the CPUs' row (ws134-p005): the shares of user, system, idle and
 * other time over all CPUs since the last draw (since boot the first
 * time), from hw.cputimes.  Without hw.cputimes the row says so.
 */
static void
cpu_line(
	void)
{
	static uint64_t last[4];
	uint64_t totals[4];
	uint64_t delta[4];
	uint64_t sum;
	double share[4];
	unsigned kind;
	int error;

	/* The ticks of every CPU, added up. */
	error = cpu_totals(totals);
	if (error != 0) {
		printf("%%Cpu(s): unavailable\n");
		return;
	}

	/* The ticks since the last draw, and the last for the next. */
	sum = 0;
	for (kind = 0; kind < 4U; kind++) {
		delta[kind] = totals[kind] - last[kind];
		sum += delta[kind];
		last[kind] = totals[kind];
	}

	/* Each kind's share (all idle when no tick came). */
	for (kind = 0; kind < 4U; kind++) {
		share[kind] = 0.0;
		if (sum != 0U)
			share[kind] = 100.0 * (double)delta[kind] / (double)sum;
	}

	/* No tick between two draws: idle. */
	if (sum == 0U)
		share[2] = 100.0;

	/* The row: user, system, idle, other (between threads). */
	printf("%%Cpu(s): %5.1f us, %5.1f sy, %5.1f id, %5.1f ot\n", share[0], share[1], share[2], share[3]);
}

/*
 * Adds up every CPU's user, system, idle and other ticks from hw.cputimes
 * into totals.  Returns 0, or -1 when the value cannot be read.
 */
static int
cpu_totals(
	uint64_t totals[4])
{
	const struct cpu_times_header *header;
	const struct cpu_times_entry *entry;
	unsigned char *buffer;
	size_t length;
	size_t needed;
	uint32_t cpu;
	int status;

	/* The length the kernel needs. */
	length = 0;
	status = sysctlbyname("hw.cputimes", NULL, &length, NULL, 0);
	if (status != 0)
		return -1;

	/* A buffer of that length. */
	buffer = malloc(length);
	if (buffer == NULL)
		return -1;

	/* The value, whole and of the known layout. */
	needed = length;
	status = sysctlbyname("hw.cputimes", buffer, &length, NULL, 0);
	header = (const struct cpu_times_header *)(void *)buffer;
	if (status != 0) {
		free(buffer);
		return -1;
	}

	/* A short value or another layout. */
	if (length != needed || length < sizeof(*header)) {
		free(buffer);
		return -1;
	}

	/* Another version or element. */
	if (header->version != CPU_TIMES_VERSION || header->element_size != sizeof(*entry)) {
		free(buffer);
		return -1;
	}

	/* Entries beyond the value. */
	if (header->struct_size + (size_t)header->count * header->element_size > length) {
		free(buffer);
		return -1;
	}

	/* Every CPU's ticks, added up by kind. */
	memset(totals, 0, 4U * sizeof(totals[0]));
	for (cpu = 0; cpu < header->count; cpu++) {
		entry = (const struct cpu_times_entry *)(void *)(buffer + header->struct_size + (size_t)cpu * header->element_size);
		totals[0] += entry->user;
		totals[1] += entry->system;
		totals[2] += entry->idle;
		totals[3] += entry->other;
	}

	/* The value is not needed any more. */
	free(buffer);

	/* Succeeded: the totals. */
	return 0;
}

/*
 * Takes the first sample of the processes' CPU time and waits a moment, so
 * that the first draw's %CPU is the share of that moment.
 */
static void
cpu_prime(
	int fd)
{
	struct process_info processes[TOP_MAX_PROCESSES];
	struct timespec now;
	struct timespec pause;
	size_t count;
	int status;

	/* The processes now; without them the first draw shows no share. */
	status = snapshot(fd, processes, &count);
	if (status != 0)
		return;

	/* Remembers their CPU time and when it was read. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	cpu_remember(processes, count, &now);

	/* The moment the first draw's %CPU covers. */
	pause.tv_sec = 0;
	pause.tv_nsec = TOP_FIRST_SAMPLE_NANOSECONDS;
	(void)nanosleep(&pause, NULL);
}

/* Keeps the processes' CPU times and the time they were read, for the next draw. */
static void
cpu_remember(
	const struct process_info *processes,
	size_t count,
	const struct timespec *when)
{
	size_t index;

	/* Each process's CPU time. */
	for (index = 0; index < count && index < TOP_MAX_PROCESSES; index++) {
		top_previous[index].pid = processes[index].pid;
		top_previous[index].cpu_ticks = processes[index].cpu_ticks;
	}

	/* How many there are, and when they were read. */
	top_previous_count = index;
	top_previous_time = *when;
}

/* Gives the seconds since the last sample, or 0 when there was none. */
static double
cpu_elapsed(
	const struct timespec *now)
{
	double seconds;

	/* No sample yet: nothing to measure from. */
	if (top_previous_count == 0U)
		return 0.0;

	/* The monotonic time between the sample and now. */
	seconds = (double)(now->tv_sec - top_previous_time.tv_sec);
	seconds += (double)(now->tv_nsec - top_previous_time.tv_nsec) / 1000000000.0;

	/* The interval, never negative. */
	if (seconds < 0.0)
		return 0.0;
	return seconds;
}

/*
 * Gives a process's %CPU: the CPU time it used since the last sample, as a
 * share of one CPU over the time elapsed.  A process the sample lacks
 * started since, so all its time counts.  A process busy on several CPUs
 * passes 100.
 */
static double
cpu_percent(
	const struct process_info *process,
	double elapsed)
{
	uint64_t previous_ticks;
	uint64_t ticks;
	size_t index;

	/* No interval: no share can be told. */
	if (elapsed <= 0.0)
		return 0.0;

	/* The time the process had used at the sample. */
	previous_ticks = 0;
	for (index = 0; index < top_previous_count; index++) {
		if (top_previous[index].pid == process->pid) {
			previous_ticks = top_previous[index].cpu_ticks;
			break;
		}
	}

	/* The time used since; a reused pid whose new process has less counts that one's alone. */
	ticks = process->cpu_ticks;
	if (ticks >= previous_ticks)
		ticks -= previous_ticks;

	/* The share of one CPU over the interval. */
	return 100.0 * (double)ticks / (elapsed * (double)KERN_PROCESS_TIMES_HZ);
}
