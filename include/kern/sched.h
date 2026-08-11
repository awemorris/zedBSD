/*
 * Scheduler interface (priority round-robin; sched.c arrives with the
 * kernel scheduling phase — until then the kernel links stubs).
 */

#ifndef _SYS_SCHED_H_
#define _SYS_SCHED_H_

#include <hal/types.h>
#include <hal/task.h>

/* Which list a task is linked on. */
enum SchedListType {
	SCHED_LIST_UNLINKED = 0,	/* not linked (sleeping) */
	SCHED_LIST_ACTIVE = 1,
	SCHED_LIST_TIMEWAIT = 2,
};

/* Priorities. */
#define SCHED_PRIOR_LEVELS	(16)
#define SCHED_PRIOR_HIGH	(0)
#define SCHED_PRIOR_LOW		(15)

/*
 * Schedulable header: placed first in the architecture task_info so
 * task_t, struct task_info *, and struct schedulable * cast freely.
 */
struct schedulable {
	int cpu;
	int status;		/* SchedListType */
	int priority;
	uint32 timeout;		/* tick for SCHED_LIST_TIMEWAIT */
	struct schedulable *next;	/* circular per-list link */
};

/* Per-CPU scheduling lists. */
struct cpu_sched_list {
	struct schedulable *active_head[SCHED_PRIOR_LEVELS];
	struct schedulable *timewait_head;
	struct schedulable *idle_task;
};

/* sched.c — callable only with interrupts disabled. */
void sched_init(void);
void sched_link(task_t t, int list, int priority, int opt);
void sched_yield(void);
void sched_clock_handler(void);

#endif
