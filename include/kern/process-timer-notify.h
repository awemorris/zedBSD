/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_PROCESS_TIMER_NOTIFY_H
#define ZEDBSD_KERN_PROCESS_TIMER_NOTIFY_H

#include <stdint.h>

struct process;

/* Complete/fail a notification captured by slot+generation.  Both operations
 * tolerate timer deletion and slot reuse; callers hold a process reference but
 * no process/timer lock. */
void process_timer_notification_complete(struct process *, unsigned,
	uint32_t);
void process_timer_notification_failed(struct process *, unsigned, uint32_t);

#endif
