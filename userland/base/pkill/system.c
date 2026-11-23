/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system's side of pgrep and pkill: the list of processes, taken from
 * /dev/system as ps takes it (KERN_SYSTEM_GET_PROCESS for each process
 * after the one before, and KERN_SYSTEM_GET_PROCESS_ARGUMENTS for each
 * one's command line), and the signal sent to one of them.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/system.h>

#include "match.h"

/* How many processes the first array of a snapshot holds; it doubles when full. */
#define PKILL_SNAPSHOT_FIRST	64

static int pkill_system_grow(struct pkill_process **processes, size_t *capacity);
static void pkill_system_arguments(int descriptor, struct pkill_process *process);

/*
 * Takes a snapshot of every process.
 *
 * The array has no fixed limit, so a busy system's last processes are not
 * left out of a selection.  On failure nothing is left for the caller to
 * free.
 */
int
pkill_system_snapshot(
	struct pkill_process **processes,
	size_t *count)
{
	struct pkill_process *array;
	struct pkill_process *process;
	size_t capacity;
	size_t used;
	int32_t cursor;
	int descriptor;
	int status;
	int error;

	/* Starts with nothing, so the results are set on every return. */
	*processes = NULL;
	*count = 0;

	/* Opens the system's device. */
	descriptor = open("/dev/system", O_RDONLY);
	if (descriptor < 0)
		return errno;

	/* Asks for each process after the one before, until there is none. */
	array = NULL;
	capacity = 0;
	used = 0;
	error = 0;
	cursor = -1;
	for (;;) {
		/* Makes room for one more process. */
		if (used == capacity) {
			error = pkill_system_grow(&array, &capacity);
			if (error != 0)
				break;
		}

		/* Asks for the process after the cursor; ENOENT means there is none. */
		process = &array[used];
		memset(process, 0, sizeof(*process));
		process->info.pid = cursor;
		status = ioctl(descriptor, KERN_SYSTEM_GET_PROCESS, &process->info);
		if (status != 0) {
			if (errno != ENOENT)
				error = errno;
			break;
		}

		/* Refuses an answer of another version or size. */
		if (process->info.version != KERN_SYSTEM_PROCESS_INFO_VERSION ||
		    process->info.struct_size != sizeof(process->info)) {
			error = EINVAL;
			break;
		}

		/* Keeps the process with its command line. */
		process->info.command[sizeof(process->info.command) - 1] = '\0';
		pkill_system_arguments(descriptor, process);
		cursor = process->info.pid;
		used++;
	}

	/* Closes the device. */
	(void)close(descriptor);

	/* Reports why the snapshot could not be taken. */
	if (error != 0) {
		free(array);
		return error;
	}

	/* Succeeded: the caller owns the snapshot. */
	*processes = array;
	*count = used;
	return 0;
}

/*
 * Sends a signal to one process.
 */
int
pkill_system_signal(
	pid_t pid,
	int signal_number)
{
	int status;

	/* Sends the signal. */
	status = kill(pid, signal_number);
	if (status != 0)
		return errno;

	/* Succeeded: the process was signalled. */
	return 0;
}

/* Doubles a snapshot's array, or makes its first one. */
static int
pkill_system_grow(
	struct pkill_process **processes,
	size_t *capacity)
{
	struct pkill_process *array;
	size_t larger;

	/* The first array, or one twice as large. */
	larger = PKILL_SNAPSHOT_FIRST;
	if (*capacity != 0)
		larger = *capacity * 2;

	/* Refuses a size the multiplication cannot express. */
	if (larger > (size_t)-1 / sizeof(**processes))
		return ENOMEM;

	/* Moves the processes taken so far into the larger array. */
	array = realloc(*processes, larger * sizeof(**processes));
	if (array == NULL)
		return ENOMEM;

	/* Succeeded: the array has room for more processes. */
	*processes = array;
	*capacity = larger;
	return 0;
}

/*
 * Takes a process's command line, or leaves it empty when the system
 * gives none (another user's process, or one that has gone).
 */
static void
pkill_system_arguments(
	int descriptor,
	struct pkill_process *process)
{
	struct system_process_arguments line;
	int status;

	/* Asks the system for the line. */
	memset(&line, 0, sizeof(line));
	line.pid = process->info.pid;
	status = ioctl(descriptor, KERN_SYSTEM_GET_PROCESS_ARGUMENTS, &line);
	if (status != 0)
		return;

	/* Takes only an answer of this version and size. */
	if (line.version != KERN_SYSTEM_PROCESS_ARGUMENTS_VERSION ||
	    line.struct_size != sizeof(line))
		return;

	/* Keeps the line, ended within its field. */
	line.arguments[sizeof(line.arguments) - 1] = '\0';
	memcpy(process->arguments, line.arguments, sizeof(process->arguments));
}
