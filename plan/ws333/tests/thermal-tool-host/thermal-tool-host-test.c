/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of /sbin/thermal's CPU reads: "thermal cpu" and "thermal cpu
 * <n>" show the kernel's fresh rate whether /dev/thermal opens for writing
 * or for reading only, and ask for nothing beyond the CPU.  The program is
 * compiled in with open and ioctl replaced by a model of the device as the
 * kernel answers GET_CPU: three CPUs, one measurement of every CPU shared
 * by the reads of a second, whose rate is 1000 * the measurement's number
 * + n MHz, and a model second that passes between runs of the program.
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

/* The measurements of every CPU the model took, and whether the last one is still fresh. */
static unsigned model_measurements;
static bool model_fresh;

/* The GET_CPU requests the model answered. */
static unsigned model_requests;

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

/* Answers GET_CPU as the kernel does, on any descriptor. */
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

	/* Refuses a reserved field that is not zero. */
	if (cpu->reserved0 != 0U) {
		errno = EINVAL;
		return -1;
	}

	/* Refuses a CPU past the last. */
	if (cpu->cpu >= MODEL_CPUS) {
		errno = ENOENT;
		return -1;
	}

	/* Measures every CPU when the last measurement is old, then reports the CPU's rate. */
	model_requests++;
	if (!model_fresh) {
		model_measurements++;
		model_fresh = true;
	}
	cpu->count = MODEL_CPUS;
	cpu->core_class = KERN_THERMAL_CPU_CORE_PERFORMANCE;
	cpu->relative = KERN_THERMAL_UNKNOWN;
	cpu->delivered_mhz = 1000U * model_measurements + cpu->cpu;
	return 0;
}

/* Runs "thermal cpu" with the arguments given (NULL for none) and reads what it printed into text. */
static int
run_cpu(
	const char *argument,
	const char *second,
	const char *output,
	char *text,
	size_t size)
{
	char *argv[] = { "thermal", "cpu", (char *)argument, (char *)second, NULL };
	int count;
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
	count = 2;
	if (argument != NULL)
		count = second != NULL ? 4 : 3;
	status = thermal_main(count, argv);
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

	/* Outside the group: the open reads only, and the list has the fresh rates of one measurement. */
	model_writable = false;
	model_fresh = false;
	status = run_cpu(NULL, NULL, output, text, sizeof(text));
	check(status == 0, "read-only: exit 0");
	check(!model_opened_writable, "read-only: opened for reading");
	check(model_measurements == 1U && model_requests == MODEL_CPUS, "read-only: one measurement for three CPUs");
	check(strstr(text, "cpu 0 ") != NULL && strstr(text, "mhz=1000\n") != NULL, "read-only: cpu 0 fresh");
	check(strstr(text, "cpu 2 ") != NULL && strstr(text, "mhz=1002\n") != NULL, "read-only: cpu 2 fresh");
	check(strstr(text, "cpu 3 ") == NULL, "read-only: three CPUs");

	/* Within the second, one CPU's fields share that measurement. */
	status = run_cpu("1", NULL, output, text, sizeof(text));
	check(status == 0, "read-only one CPU: exit 0");
	check(model_measurements == 1U, "read-only one CPU: shared");
	check(strstr(text, "delivered_mhz 1001\n") != NULL, "read-only one CPU: fresh");

	/* A second later: measured again. */
	model_fresh = false;
	status = run_cpu("1", NULL, output, text, sizeof(text));
	check(status == 0 && model_measurements == 2U, "a second later: measured again");
	check(strstr(text, "delivered_mhz 2001\n") != NULL, "a second later: the new rate");

	/* A member of the group reads the same way. */
	model_writable = true;
	model_fresh = false;
	model_requests = 0U;
	status = run_cpu(NULL, NULL, output, text, sizeof(text));
	check(status == 0, "writable: exit 0");
	check(model_measurements == 3U && model_requests == MODEL_CPUS, "writable: one measurement");
	check(strstr(text, "mhz=3000\n") != NULL && strstr(text, "mhz=3002\n") != NULL, "writable: fresh");

	/* The sample argument is gone. */
	status = run_cpu("1", "sample", output, text, sizeof(text));
	check(status == 2, "cpu 1 sample: usage");

	/* Reports the result. */
	if (failures != 0U) {
		printf("thermal-tool-host: FAIL (%u)\n", failures);
		return 1;
	}
	printf("thermal-tool-host: PASS\n");
	return 0;
}
