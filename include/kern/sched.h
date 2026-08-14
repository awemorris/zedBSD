/*
 * Round-robin kernel scheduler
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_SCHED_H
#define ZEDBSD_KERN_SCHED_H

#include <stdint.h>

struct thread;

#define SCHED_PRIOR_LEVELS 16
#define SCHED_PRIOR_HIGH 0
#define SCHED_PRIOR_LOW 15
#define SCHED_PRIORITY_DEFAULT 8
#define SCHED_QUANTUM_TICKS 5U

enum sched_queue_kind {
	SCHED_QUEUE_NONE = 0,
	SCHED_QUEUE_RUN,
	SCHED_QUEUE_SLEEP,
};

struct sched {
	int priority;
	uint32_t quantum;
	uint64_t wakeup_tick;
	unsigned queue_kind;
	struct thread *next;
	struct thread *prev;
};

struct sched_queue {
	struct thread *head;
	struct thread *tail;
	unsigned count;
};

void sched_init(void);
void sched_add(struct thread *thread);
void sched_unlink(struct thread *thread);
void sched_wakeup(struct thread *thread);
void sched_switch(void);
void sched_yield(void);
void sched_clock(void);
void sched_sleep(uint64_t timeout_tick);
void sched_awake_from_sleep(struct thread *thread);
uint64_t sched_ticks(void);
int sched_has_runnable(void);
void sched_idle(void) __attribute__((noreturn));

#endif
