#include "kern/process.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/lock.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct thread thread0;
struct process process0;
static struct thread *current;
static int irq_enabled = 1;
static unsigned reschedule_requests;

struct thread *thread_current(void) { return current; }

bool
hal_irq_disable(void)
{
	bool was_enabled = irq_enabled != 0;
	irq_enabled = 0;
	return was_enabled;
}

void hal_irq_enable(void) { irq_enabled = 1; }
void hal_reschedule_on_interrupt_return(void) { reschedule_requests++; }
void hal_cpu_idle(void) { irq_enabled = 0; }
void spin_lock(struct spinlock *lock) { (void)lock; }
void spin_unlock(struct spinlock *lock) { (void)lock; }
int kern_boot_pending(void) { return 0; }
void kern_boot_execute_pending(void) { abort(); }

void
hal_task_context_switch(hal_task_t task)
{
	current = task;
}

void
hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "unexpected HAL fatal at %s:%d: %s\n", file, line,
		message);
	abort();
}

int
main(void)
{
	struct thread first, second, sleeper;
	unsigned i;

	memset(&thread0, 0, sizeof(thread0));
	thread0.task = &thread0;
	thread0.sched.priority = SCHED_PRIORITY_DEFAULT;
	current = &thread0;
	sched_init();

	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	first.task = &first;
	second.task = &second;
	first.state = second.state = THREAD_NEW;
	first.sched.priority = second.sched.priority = SCHED_PRIORITY_DEFAULT;
	sched_add(&first);
	sched_add(&second);
	sched_yield();
	assert(current == &first && first.state == THREAD_RUNNING);
	assert(thread0.state == THREAD_RUNNABLE);
	sched_yield();
	assert(current == &second && second.state == THREAD_RUNNING);

	for (i = 0; i < SCHED_QUANTUM_TICKS; i++)
		sched_clock();
	assert(reschedule_requests == 1);
	assert(sched_ticks() == SCHED_QUANTUM_TICKS);

	memset(&sleeper, 0, sizeof(sleeper));
	sleeper.task = &sleeper;
	sleeper.state = THREAD_SLEEPING;
	sleeper.sched.priority = SCHED_PRIORITY_DEFAULT;
	sched_add(&sleeper);
	assert(sleeper.state == THREAD_RUNNABLE);
	sched_unlink(&sleeper);
	assert(sleeper.sched.queue_kind == SCHED_QUEUE_NONE);

	puts("zedBSD scheduler host tests: PASS");
	return 0;
}
