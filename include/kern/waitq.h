/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_KERN_WAITQ_H
#define ZEDBSD_KERN_WAITQ_H

#include <stdint.h>

struct spinlock;
struct thread;
struct wait_queue;

struct wait_token {
	struct thread *thread;
	struct wait_token *next;
	struct wait_queue *queue;
};

struct wait_queue {
	struct wait_token *head;
	struct wait_token *tail;
	uint64_t sequence;
	const char *name;
};

#define WAITQ_INTERRUPTIBLE	0x0001U

/*
 * Cancellation uses the thread's atomic sticky request plus the scheduler's
 * interrupt-generation handoff.  The condition lock need not be proc->lock.
 */
#define WAITQ_CANCELABLE	0x0002U

void
waitq_init(
	struct wait_queue *,
	const char *);

uint64_t
waitq_sequence(
	const struct wait_queue *);

int
waitq_sleep(
	struct wait_queue *,
	struct spinlock *,
	uint64_t,
	uint64_t,
	unsigned);

void
waitq_wake_one(
	struct wait_queue *);

void
waitq_wake_all(
	struct wait_queue *);

#endif
