#include "kern/process.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/lock.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CPUS 4U

struct thread thread0;
struct process process0;
static struct process accounting_process;
static struct thread *current[TEST_CPUS];
static hal_cpu_id_t current_cpu;
static int irq_enabled = 1;
static unsigned notifications[TEST_CPUS];
static hal_cpu_id_t task_targets[32];

struct thread *thread_current(void) { return current[current_cpu]; }
void thread_sched_retired(struct thread *thread)
{ thread->state = THREAD_ZOMBIE; }
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
struct process *process_find_next_ref(pid_t after)
{ (void)after; return NULL; }
void process_release(struct process *process) { (void)process; }
int signal_send_process(struct process *process, int signal)
{ (void)process; (void)signal; return 0; }

hal_cpu_id_t hal_cpu_current(void) { return current_cpu; }
unsigned hal_cpu_count(void) { return TEST_CPUS; }
void hal_cpu_ready_mask(struct hal_cpu_mask *mask)
{
	hal_cpu_id_t cpu;
	hal_cpu_mask_zero(mask);
	for (cpu = 0; cpu < TEST_CPUS; cpu++) hal_cpu_mask_set(mask, cpu);
}
int hal_cpu_notify(hal_cpu_id_t cpu)
{
	if (cpu >= TEST_CPUS) return HAL_ERR_INVALID;
	notifications[cpu]++;
	return HAL_OK;
}
int hal_task_transfer(hal_task_t task, hal_cpu_id_t cpu)
{
	struct thread *thread = task;
	if (thread == NULL || cpu >= TEST_CPUS) return HAL_ERR_INVALID;
	task_targets[(unsigned)(thread - &thread0) & 31U] = cpu;
	return HAL_OK;
}
void *hal_task_get_private(hal_task_t task) { return task; }

bool
hal_irq_disable(void)
{
	bool was_enabled = irq_enabled != 0;
	irq_enabled = 0;
	return was_enabled;
}
void hal_irq_enable(void) { irq_enabled = 1; }
void hal_cpu_idle(void) { irq_enabled = 0; }
void hal_compiler_barrier_test(void) { }
void
hal_task_context_switch(hal_task_t task)
{
	current[current_cpu] = task;
}

void spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{ lock->held.value = 0; lock->rank = rank; lock->name = name; }
void spin_lock(struct spinlock *lock) { assert(lock->held.value == 0); lock->held.value = 1; }
void spin_unlock(struct spinlock *lock) { assert(lock->held.value == 1); lock->held.value = 0; }
unsigned long spin_lock_irqsave(struct spinlock *lock)
{ unsigned long enabled = hal_irq_disable(); spin_lock(lock); return enabled; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{ spin_unlock(lock); if (enabled) hal_irq_enable(); }

void
hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "unexpected HAL fatal at %s:%d: %s\n", file, line,
	    message);
	abort();
}

static void
init_thread(struct thread *thread)
{
	memset(thread, 0, sizeof(*thread));
	thread->task = thread;
	thread->state = THREAD_NEW;
	thread->sched.priority = SCHED_PRIORITY_DEFAULT;
}

int
main(void)
{
	struct thread idle[TEST_CPUS], tasks[TEST_CPUS], sleeper;
	hal_cpu_id_t cpu;

	memset(&thread0, 0, sizeof(thread0));
	thread0.task = &thread0;
	thread0.flags = THREAD_FLAG_IDLE;
	thread0.sched.priority = SCHED_PRIORITY_DEFAULT;
	current[0] = &thread0;
	sched_init();
	for (cpu = 1; cpu < TEST_CPUS; cpu++) {
		init_thread(&idle[cpu]);
		idle[cpu].task = &idle[cpu];
		current[cpu] = &idle[cpu];
		assert(sched_test_cpu_online(cpu, &idle[cpu]) == 0);
	}
	assert(sched_wait_others_online() == 0);

	for (cpu = 0; cpu < TEST_CPUS; cpu++) {
		init_thread(&tasks[cpu]);
		if (cpu == 0)
			tasks[cpu].proc = &accounting_process;
		assert(sched_prepare_thread(&tasks[cpu]) == 0);
		assert(tasks[cpu].sched.cpu == cpu);
		sched_add(&tasks[cpu]);
	}
	assert(notifications[1] == 1 && notifications[2] == 1 &&
	    notifications[3] == 1);

	for (cpu = 0; cpu < TEST_CPUS; cpu++) {
		current_cpu = cpu;
		sched_yield();
		assert(current[cpu] == &tasks[cpu]);
		assert(tasks[cpu].state == THREAD_RUNNING);
	}

	current_cpu = 2;
	init_thread(&sleeper);
	sleeper.sched.cpu = 2;
	sleeper.sched.last_cpu = 2;
	sleeper.state = THREAD_SLEEPING;
	sched_add(&sleeper);
	assert(sleeper.state == THREAD_RUNNABLE);
	sched_unlink(&sleeper);
	assert(sleeper.sched.queue_kind == SCHED_QUEUE_NONE);
	assert(sched_set_cpu(&sleeper, 3) == 0);
	assert(sleeper.sched.cpu == 3);

	current_cpu = 0;
	for (cpu = 1; cpu <= SCHED_QUANTUM_TICKS; cpu++)
		sched_clock_cpu(0, cpu);
	assert(sched_ticks() == SCHED_QUANTUM_TICKS);
	assert(accounting_process.cpu_ticks == SCHED_QUANTUM_TICKS);

	puts("zedBSD per-CPU scheduler host tests: PASS");
	return 0;
}
