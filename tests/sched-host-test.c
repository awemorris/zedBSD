#include "kern/process.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/lock.h"
#include "kern/test-checkpoint.h"
#include "kern/waitq.h"

#include <assert.h>
#include <errno.h>
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
static unsigned itimer_signals[NSIG];
static unsigned cpu_limit_ticks;
static uint64_t cpu_limit_total;
static enum kern_test_checkpoint_id interrupt_checkpoint;
static struct wait_queue *interrupt_queue;
static int pending_signal;

struct thread *thread_current(void) { return current[current_cpu]; }
void thread_sched_retired(struct thread *thread)
{ thread->state = THREAD_ZOMBIE; }
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
struct process *process_find_next_ref(pid_t after)
{ (void)after; return NULL; }
void process_release(struct process *process) { (void)process; }
int signal_send_process(struct process *process, int signal)
{ (void)process; (void)signal; return 0; }
int signal_send_process_info(struct process *process, int signal,
	const struct signal_info *info)
{
	(void)process;
	(void)info;
	if (signal > 0 && signal < NSIG)
		itimer_signals[signal]++;
	return 0;
}
void resource_limit_cpu_tick(struct process *process, uint64_t total)
{
	assert(process == &accounting_process);
	cpu_limit_ticks++;
	cpu_limit_total = total;
}
int process_itimer_tick(struct process *process, int which)
{
	uint64_t remaining = process->itimer_remaining[which];
	if (remaining == 0)
		return 0;
	process->itimer_remaining[which] = remaining == 1U ?
	    process->itimer_interval[which] : remaining - 1U;
	return remaining == 1U;
}

void process_itimer_real_tick_all(void) { }
int process_stop_requested(const struct thread *thread)
{ (void)thread; return 0; }
int signal_pending_unblocked(const struct thread *thread)
{ (void)thread; return pending_signal; }

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

static void
interrupt_wait_checkpoint(enum kern_test_checkpoint_id id, void *object,
	void *argument)
{
	(void)argument;
	if (id != interrupt_checkpoint || object != interrupt_queue)
		return;
	pending_signal = 1;
	sched_interrupt(current[current_cpu]);
}

static void
test_interruptible_wait_handoff(struct thread *thread)
{
	static const enum kern_test_checkpoint_id checkpoints[] = {
		KERN_TEST_WAIT_BEFORE_REGISTER,
		KERN_TEST_WAIT_AFTER_REGISTER,
	};
	struct wait_queue queue;
	struct spinlock condition;
	unsigned index;

	waitq_init(&queue, "interrupt race");
	spin_init(&condition, LOCK_RANK_PROCESS, "interrupt condition");
	interrupt_queue = &queue;
	for (index = 0; index < sizeof(checkpoints) / sizeof(checkpoints[0]);
	    index++) {
		unsigned long irq;
		uint64_t sequence = waitq_sequence(&queue);

		pending_signal = 0;
		interrupt_checkpoint = checkpoints[index];
		kern_test_checkpoint_set(interrupt_wait_checkpoint, NULL);
		irq = spin_lock_irqsave(&condition);
		assert(waitq_sleep(&queue, &condition, sequence, 0,
		    WAITQ_INTERRUPTIBLE) == EINTR);
		spin_unlock_irqrestore(&condition, irq);
		assert(thread->state == THREAD_RUNNING);
		assert(thread->wait_token.queue == NULL);
		assert(queue.head == NULL && queue.tail == NULL);
	}
	kern_test_checkpoint_set(NULL, NULL);
	pending_signal = 0;
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
	tasks[0].sched.quantum = 32U;
	accounting_process.itimer_remaining[1] = 3U;
	accounting_process.itimer_remaining[2] = 4U;
	sched_clock_cpu(0, 1U);
	sched_clock_cpu(0, 2U);
	assert(accounting_process.user_ticks == 2U);
	assert(accounting_process.system_ticks == 0U);
	assert(accounting_process.itimer_remaining[1] == 1U);
	assert(accounting_process.itimer_remaining[2] == 2U);
	sched_accounting_kernel_enter();
	sched_clock_cpu(0, 3U);
	sched_accounting_kernel_enter();
	sched_clock_cpu(0, 4U);
	sched_accounting_kernel_leave();
	sched_accounting_kernel_leave();
	assert(accounting_process.user_ticks == 2U);
	assert(accounting_process.system_ticks == 2U);
	assert(accounting_process.itimer_remaining[1] == 1U);
	assert(accounting_process.itimer_remaining[2] == 0U);
	assert(itimer_signals[SIGPROF] == 1U);
	sched_clock_cpu(0, 5U);
	assert(sched_ticks() == 5U);
	assert(accounting_process.cpu_ticks == 5U);
	assert(accounting_process.user_ticks == 3U);
	assert(accounting_process.system_ticks == 2U);
	assert(accounting_process.itimer_remaining[1] == 0U);
	assert(itimer_signals[SIGVTALRM] == 1U);
	assert(cpu_limit_ticks == 5U && cpu_limit_total == 5U);

	/* Kernel threads and the idle/process0 domain never contribute CPU time
	 * or consume process interval timers. */
	tasks[0].proc = &process0;
	process0.itimer_remaining[2] = 1U;
	sched_clock_cpu(0, 6U);
	assert(process0.cpu_ticks == 0U && process0.user_ticks == 0U &&
	    process0.system_ticks == 0U);
	assert(process0.itimer_remaining[2] == 1U);
	assert(cpu_limit_ticks == 5U);

	/* A signal delivered on either side of wait-queue registration must not
	 * vanish merely because sched_wakeup() saw the task as RUNNING. */
	test_interruptible_wait_handoff(&tasks[0]);

	puts("zedBSD per-CPU scheduler host tests: PASS");
	return 0;
}
