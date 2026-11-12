/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of /sbin/thermal's CPU list: "thermal cpu" samples every CPU
 * when /dev/thermal opens for writing, and shows the last samples when
 * the kernel refuses a sample on a descriptor opened for reading.  The
 * program is compiled in with open and ioctl replaced by a model of the
 * device: three CPUs, whose last sample reads 100 + n MHz and whose fresh
 * sample reads 3000 + n MHz.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uapi/thermal.h>

int test_open(const char *path, int flags, ...);
int test_ioctl(int descriptor, unsigned long request, ...);
int thermal_main(int argc, char **argv);

/* The number of CPUs the model has. */
#define MODEL_CPUS		3U

/* The descriptor the model hands out. */
#define MODEL_DESCRIPTOR	42

/* Whether the model lets the device open for writing. */
static bool model_writable;

/* The descriptor's mode: true when it was opened for writing. */
static bool model_opened_writable;

/* The number of samples the model took. */
static unsigned model_samples;

/* The number of failed checks. */
static unsigned failures;

/* Opens the device of the model, for writing only when the model lets. */
int
test_open(
	const char *path,
	int flags,
	...)
{
	/* Opens only the device. */
	if (strcmp(path, "/dev/thermal") != 0) {
		errno = ENOENT;
		return -1;
	}

	/* Refuses writing outside the group. */
	if ((flags & O_ACCMODE) != O_RDONLY && !model_writable) {
		errno = EACCES;
		return -1;
	}

	/* Hands out the descriptor in its mode. */
	model_opened_writable = (flags & O_ACCMODE) != O_RDONLY;
	return MODEL_DESCRIPTOR;
}

/* Answers GET_CPU as the kernel does for a sample and for the last sample. */
int
test_ioctl(
	int descriptor,
	unsigned long request,
	...)
{
	struct thermal_cpu_info *cpu;
	va_list arguments;

	/* Takes the record. */
	va_start(arguments, request);
	cpu = va_arg(arguments, struct thermal_cpu_info *);
	va_end(arguments);

	/* Knows only the CPU request of its one descriptor. */
	if (descriptor != MODEL_DESCRIPTOR || request != KERN_THERMAL_GET_CPU) {
		errno = ENOTTY;
		return -1;
	}

	/* Refuses a sample on a descriptor opened for reading, before the CPU number. */
	if ((cpu->request & KERN_THERMAL_CPU_READ_SAMPLE) != 0U && !model_opened_writable) {
		errno = EPERM;
		return -1;
	}

	/* Refuses a CPU past the last. */
	if (cpu->cpu >= MODEL_CPUS) {
		errno = ENOENT;
		return -1;
	}

	/* Reports the fresh rate of a sample, else the last one. */
	cpu->count = MODEL_CPUS;
	cpu->core_class = KERN_THERMAL_CPU_CORE_PERFORMANCE;
	cpu->relative = KERN_THERMAL_UNKNOWN;
	cpu->delivered_mhz = 100U + cpu->cpu;
	if ((cpu->request & KERN_THERMAL_CPU_READ_SAMPLE) != 0U) {
		cpu->delivered_mhz = 3000U + cpu->cpu;
		model_samples++;
	}
	return 0;
}

/* Runs "thermal cpu" and reads what it printed into text. */
static int
run_list(
	const char *output,
	char *text,
	size_t size)
{
	char *argv[] = { "thermal", "cpu", NULL };
	FILE *file;
	size_t length;
	int saved;
	int status;

	/* Points stdout at the file for the run. */
	fflush(stdout);
	saved = dup(1);
	file = freopen(output, "w", stdout);
	if (file == NULL)
		return -1;
	status = thermal_main(2, argv);
	fflush(stdout);
	dup2(saved, 1);
	close(saved);
	stdout = fdopen(1, "w");

	/* Reads what the run printed. */
	file = fopen(output, "r");
	if (file == NULL)
		return -1;
	length = fread(text, 1U, size - 1U, file);
	text[length] = '\0';
	fclose(file);
	return status;
}

/* Checks one condition and reports it. */
static void
check(
	bool condition,
	const char *what)
{
	/* Counts and names a failure. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

int
main(
	int argc,
	char **argv)
{
	char text[4096];
	char output[4096];
	int status;

	/* Writes the run's output next to the program. */
	if (argc < 2)
		return 2;
	(void)snprintf(output, sizeof(output), "%s/list.txt", argv[1]);

	/* A member of the group: every CPU is sampled. */
	model_writable = true;
	model_samples = 0U;
	status = run_list(output, text, sizeof(text));
	check(status == 0, "writable: exit 0");
	check(model_samples == MODEL_CPUS, "writable: one sample per CPU");
	check(strstr(text, "cpu 0 ") != NULL && strstr(text, "mhz=3000\n") != NULL, "writable: cpu 0 fresh");
	check(strstr(text, "cpu 2 ") != NULL && strstr(text, "mhz=3002\n") != NULL, "writable: cpu 2 fresh");
	check(strstr(text, "mhz=10") == NULL, "writable: no last sample");
	check(strstr(text, "cpu 3 ") == NULL, "writable: three CPUs");

	/* Outside the group: the last samples, without a failure. */
	model_writable = false;
	model_samples = 0U;
	status = run_list(output, text, sizeof(text));
	check(status == 0, "read-only: exit 0");
	check(model_samples == 0U, "read-only: no sample");
	check(strstr(text, "mhz=100\n") != NULL && strstr(text, "mhz=102\n") != NULL, "read-only: last samples");
	check(strstr(text, "cpu 3 ") == NULL, "read-only: three CPUs");

	/* Reports the result. */
	if (failures != 0U) {
		printf("thermal-tool-host: FAIL (%u)\n", failures);
		return 1;
	}
	printf("thermal-tool-host: PASS\n");
	return 0;
}
