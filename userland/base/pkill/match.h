/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What pgrep and pkill share: the selection of processes (match.c) and the
 * system's list of processes and its signals (system.c).
 */

#ifndef PKILL_MATCH_H
#define PKILL_MATCH_H

#include <stddef.h>
#include <sys/types.h>
#include <uapi/system.h>

/* The exit statuses: some process selected, none, a wrong argument, an internal error. */
#define PKILL_STATUS_MATCH	0
#define PKILL_STATUS_NO_MATCH	1
#define PKILL_STATUS_USAGE	2
#define PKILL_STATUS_ERROR	3

/*
 * Which of the two programs runs: pgrep writes the selected processes,
 * pkill signals them.
 */
enum pkill_program {
	PKILL_PROGRAM_PGREP,
	PKILL_PROGRAM_PKILL
};

/*
 * One process of a snapshot: what the system says of it, and its command
 * line (empty when the system gives none, as for another user's process).
 */
struct pkill_process {
	struct process_info info;
	char arguments[KERN_SYSTEM_PROCESS_ARGUMENTS_MAX];
};

/*
 * Runs pgrep or pkill with its arguments and returns its exit status.
 */
int pkill_run(int argc, char **argv, enum pkill_program program);

/*
 * Takes a snapshot of every process, in the order of their IDs, into an
 * array the caller frees.  Returns 0, or the error that kept the snapshot
 * from being taken.
 */
int pkill_system_snapshot(struct pkill_process **processes, size_t *count);

/*
 * Sends a signal to one process.  Returns 0, or the error the system
 * reported.
 */
int pkill_system_signal(pid_t pid, int signal_number);

#endif
