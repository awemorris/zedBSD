/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * monitor-probe: the System Monitor's backend area on zedBSD without the
 * compositor (WS134 p008), for the guest test
 * plan/ws134/tests/monitor-backend-p008.sh.
 *
 *	monitor-probe [COUNT [INTERVAL_MS]]
 *
 * Prints the info (MPROBE INFO, and a DISK, LINK or GPU line a device), then
 * COUNT samples INTERVAL_MS apart (default 2 and 2000), each as an MPROBE
 * SAMPLE line of its sums: the CPUs' ticks, the memory, the links' and the
 * disks' bytes, the GPUs' busy time.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_info(const struct kl_backend_monitor_info *info);
static void print_sample(unsigned number, const struct kl_backend_monitor_sample *sample);
static void wait_ms(unsigned milliseconds);

/*
 * Opens the monitor and prints its info and samples; returns 0, or 1 when
 * the monitor or a sample fails.
 */
int
main(
	int argc,
	char **argv)
{
	struct kl_backend_monitor *monitor;
	struct kl_backend_monitor_info *info;
	struct kl_backend_monitor_sample *sample;
	unsigned count;
	unsigned interval;
	unsigned number;
	int error;

	/* How many samples, how far apart. */
	count = 2;
	interval = 2000;
	if (argc > 1)
		count = (unsigned)strtoul(argv[1], NULL, 10);
	if (argc > 2)
		interval = (unsigned)strtoul(argv[2], NULL, 10);

	/* The info is large: on the heap. */
	info = calloc(1, sizeof(*info));
	if (info == NULL) {
		printf("MPROBE FAILED memory\n");
		return 1;
	}

	/* So is the sample. */
	sample = calloc(1, sizeof(*sample));
	if (sample == NULL) {
		printf("MPROBE FAILED memory\n");
		return 1;
	}

	/* The monitor. */
	monitor = kl_backend_monitor_open();
	if (monitor == NULL) {
		printf("MPROBE FAILED open\n");
		return 1;
	}

	/* The info. */
	error = kl_backend_monitor_info(monitor, info);
	if (error != 0) {
		printf("MPROBE FAILED info error=%d\n", error);
		return 1;
	}

	/* What it found. */
	print_info(info);

	/* Each sample. */
	for (number = 0; number < count; number++) {
		/* A wait between two. */
		if (number != 0U)
			wait_ms(interval);

		/* The sample. */
		error = kl_backend_monitor_sample(monitor, sample);
		if (error != 0) {
			printf("MPROBE FAILED sample error=%d\n", error);
			return 1;
		}

		/* Its line, out at once. */
		print_sample(number, sample);
		fflush(stdout);
	}

	/* The monitor and the memory go; the tests read the last line. */
	kl_backend_monitor_close(monitor);
	free(info);
	free(sample);
	printf("MPROBE DONE\n");

	/* Succeeded: everything is printed. */
	return 0;
}

/* Prints the info and its devices. */
static void
print_info(
	const struct kl_backend_monitor_info *info)
{
	unsigned index;

	/* The machine. */
	printf("MPROBE INFO cpus=%u host=%s generation=%llu gpus=%u disks=%u links=%u\n", info->cpu_count, info->host,
	       (unsigned long long)info->generation, info->gpu_count, info->disk_count, info->link_count);

	/* The disks. */
	for (index = 0; index < info->disk_count; index++) {
		printf("MPROBE DISK id=%llu name=%s kind=%u generation=%llu\n", (unsigned long long)info->disk[index].id,
		       info->disk[index].name, info->disk[index].kind, (unsigned long long)info->disk[index].generation);
	}

	/* The links. */
	for (index = 0; index < info->link_count; index++)
		printf("MPROBE LINK id=%llu name=%s\n", (unsigned long long)info->link[index].id, info->link[index].name);

	/* The GPUs. */
	for (index = 0; index < info->gpu_count; index++) {
		printf("MPROBE GPU id=%llu name=%s driver=%s\n", (unsigned long long)info->gpu[index].id, info->gpu[index].name,
		       info->gpu[index].driver);
	}
}

/* Prints a sample's sums. */
static void
print_sample(
	unsigned number,
	const struct kl_backend_monitor_sample *sample)
{
	unsigned long long user;
	unsigned long long system;
	unsigned long long idle;
	unsigned long long other;
	unsigned long long rx;
	unsigned long long tx;
	unsigned long long read_bytes;
	unsigned long long write_bytes;
	unsigned long long busy;
	unsigned index;

	/* The CPUs' ticks, added up. */
	user = 0;
	system = 0;
	idle = 0;
	other = 0;
	for (index = 0; index < sample->cpu_count; index++) {
		user += sample->cpu[index].user;
		system += sample->cpu[index].system;
		idle += sample->cpu[index].idle;
		other += sample->cpu[index].other;
	}

	/* The links' bytes. */
	rx = 0;
	tx = 0;
	for (index = 0; index < sample->link_count; index++) {
		rx += sample->link[index].rx_bytes;
		tx += sample->link[index].tx_bytes;
	}

	/* The disks' bytes. */
	read_bytes = 0;
	write_bytes = 0;
	for (index = 0; index < sample->disk_count; index++) {
		read_bytes += sample->disk[index].read_bytes;
		write_bytes += sample->disk[index].write_bytes;
	}

	/* The GPUs' busy time. */
	busy = 0;
	for (index = 0; index < sample->gpu_count; index++)
		busy += sample->gpu[index].busy_ns;

	/* The line. */
	printf("MPROBE SAMPLE n=%u time_ns=%llu valid=0x%llx cpu_hz=%llu cpus=%u user=%llu system=%llu idle=%llu other=%llu "
	       "mem_total=%llu mem_free=%llu cache=%llu reclaimable=%llu swap_total=%llu swap_used=%llu "
	       "links=%u rx=%llu tx=%llu disks=%u read_bytes=%llu write_bytes=%llu gpus=%u gpu_busy_ns=%llu cpu_mc=%d\n",
	       number, (unsigned long long)sample->time_ns, (unsigned long long)sample->valid, (unsigned long long)sample->cpu_hz,
	       sample->cpu_count, user, system, idle, other,
	       (unsigned long long)sample->memory_total, (unsigned long long)sample->memory_free,
	       (unsigned long long)sample->memory_cache, (unsigned long long)sample->memory_reclaimable,
	       (unsigned long long)sample->swap_total, (unsigned long long)sample->swap_used,
	       sample->link_count, rx, tx, sample->disk_count, read_bytes, write_bytes, sample->gpu_count, busy,
	       sample->cpu_milli_celsius);
}

/* Waits a number of milliseconds. */
static void
wait_ms(
	unsigned milliseconds)
{
	struct timespec wait;

	/* The wait, whole seconds and the rest. */
	wait.tv_sec = milliseconds / 1000U;
	wait.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&wait, NULL);
}
