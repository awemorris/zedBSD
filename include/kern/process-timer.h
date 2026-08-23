/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_PROCESS_TIMER_H
#define ZEDBSD_KERN_PROCESS_TIMER_H

#include <uapi/zedbsd/signal.h>
#include <stdint.h>
#include <time.h>

struct process;
struct itimerspec;

void
process_timer_init(void);

int
process_timer_create(
	struct process *,
	clockid_t,
	const struct sigevent *,
	timer_t *);

int
process_timer_delete(
	struct process *,
	timer_t);

int
process_timer_settime(
	struct process *,
	timer_t,
	int,
	const struct itimerspec *,
	struct itimerspec *);

int
process_timer_gettime(
	struct process *,
	timer_t,
	struct itimerspec *);

int
process_timer_getoverrun(
	struct process *,
	timer_t,
	int *);

void
process_timer_tick(
	uint64_t);

void
process_timer_cleanup(
	struct process *);

/*
 * Complete/fail a notification captured by slot+generation.  Both
 * operations tolerate timer deletion and slot reuse; callers hold a
 * process reference but no process/timer lock.
 */
void
process_timer_notification_complete(
	struct process *,
	unsigned,
	uint32_t);

void
process_timer_notification_failed(
	struct process *,
	unsigned,
	uint32_t);

#endif
