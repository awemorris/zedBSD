/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_SYSCALL_H
#define ZEDBSD_KERN_SYSCALL_H

#include <stdint.h>

struct thread;

/*
 * Relative wait deadlines are computed once per syscall attempt and retained
 * only while STOP/CONT transparently redispatches that same attempt.
 */
int
syscall_restart_deadline_after(
	uint64_t,
	uint64_t *);

int
syscall_restart_deadline_rearm(
	uint64_t,
	uint64_t *);

void
syscall_restart_state_begin(
	struct thread *);

void
syscall_restart_prepare_stop(
	struct thread *);

void
syscall_restart_state_finish(
	struct thread *);

void
syscall_init(void);

#endif
