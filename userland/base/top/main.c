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
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <zedbsd/system.h>

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

static void draw(int fd, int batch);
static int snapshot(int fd, struct process_info *p, size_t *n);
static void human(uint64_t bytes, char out[16]);
static const char *user_name(uid_t uid, char b[16]);
static char state_letter(unsigned state);
static const char *leaf(const char *s);

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

	/* Process each element required by the operation. */
	for (i = 0; iterations < 0 || i < iterations; i++) {
		input.fd = STDIN_FILENO;
		input.events = POLLIN;
		input.revents = 0;

		draw(fd, batch);

		/* Handles the iterations condition. */
		if (iterations >= 0 && i + 1 >= iterations)
			break;

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
	size_t count, i;
	unsigned running, sleeping, stopped, zombie;
	char total[16], freeb[16], used[16], swap[16], swapfree[16];

	count = 0;
	running = 0;
	sleeping = 0;
	stopped = 0;
	zombie = 0;

	/* Handles a failed ioctl operation. */
	if (ioctl(fd, ZEDBSD_SYSTEM_GET_VMSTAT, &vm) != 0 ||
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
	human(vm.physical_total, total);
	human(vm.physical_free, freeb);
	human(vm.physical_total - vm.physical_free, used);
	human(vm.swap_total * ZEDBSD_SYSTEM_SWAP_PAGE_SIZE, swap);
	human(vm.swap_free * ZEDBSD_SYSTEM_SWAP_PAGE_SIZE, swapfree);

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
	printf("%%Cpu(s):  0.0 us,  0.0 sy,  0.0 ni, 100.0 id,  0.0 wa\n");
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
		printf("%7d %-8.8s  20   0 %7s      0      0 %c   0.0  0.0   "
		       "0:00.00 %s\n",
		       p[i].pid, u, virt, state_letter(p[i].state),
		       p[i].command[0] ? leaf(p[i].command) : "kernel");
	}
	fflush(stdout);
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
		if (ioctl(fd, ZEDBSD_SYSTEM_GET_PROCESS, &p[used]) != 0) {
			/* Handles the reported system error. */
			if (errno == ENOENT)
				break;

			/* Reports operation failure. */
			return -1;
		}
		p[used].command[ZEDBSD_SYSTEM_PROCESS_COMMAND_MAX - 1U] = '\0';
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
