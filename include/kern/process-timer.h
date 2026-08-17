/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_PROCESS_TIMER_H
#define ZEDBSD_KERN_PROCESS_TIMER_H

#include <signal.h>
#include <stdint.h>
#include <time.h>

struct process;

void process_timer_init(void);
int process_timer_create(struct process *, clockid_t,
	const struct sigevent *, timer_t *);
int process_timer_delete(struct process *, timer_t);
int process_timer_settime(struct process *, timer_t, int,
	const struct itimerspec *, struct itimerspec *);
int process_timer_gettime(struct process *, timer_t, struct itimerspec *);
int process_timer_getoverrun(struct process *, timer_t, int *);
void process_timer_tick(uint64_t);
void process_timer_cleanup(struct process *);

#endif
