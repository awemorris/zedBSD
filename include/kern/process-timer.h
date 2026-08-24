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
	struct process *owner,
	clockid_t clock,
	const struct sigevent *requested,
	timer_t *result);

int
process_timer_delete(
	struct process *owner,
	timer_t id);

int
process_timer_settime(
	struct process *owner,
	timer_t id,
	int flags,
	const struct itimerspec *requested,
	struct itimerspec *previous);

int
process_timer_gettime(
	struct process *owner,
	timer_t id,
	struct itimerspec *result);

int
process_timer_getoverrun(
	struct process *owner,
	timer_t id,
	int *result);

void
process_timer_tick(
	uint64_t now_ticks);

void
process_timer_cleanup(
	struct process *owner);

/*
 * Complete/fail a notification captured by slot+generation.  Both
 * operations tolerate timer deletion and slot reuse; callers hold a
 * process reference but no process/timer lock.
 */
void
process_timer_notification_complete(
	struct process *owner,
	unsigned slot,
	uint32_t generation);

void
process_timer_notification_failed(
	struct process *owner,
	unsigned slot,
	uint32_t generation);

#endif
