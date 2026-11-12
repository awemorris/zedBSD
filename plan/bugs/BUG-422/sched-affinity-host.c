/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the scheduler's CPU affinity: a thread bound to its CPU is
 * never taken by an idle CPU, a free thread is, a bound thread does not
 * keep the free threads queued behind it from being taken, and
 * sched_set_cpu() moves a thread and sets its affinity.
 *
 * The scheduler source is compiled into this file freestanding against the
 * kernel's headers, so its static functions are reachable; the HAL and the
 * rest of the kernel are stubs that do nothing.  One CPU of the test runs
 * everything, so the locks are empty.
 */

#include "../../../src/kern/sched.c"

int printf(const char *format, ...);
void exit(int status);
void *calloc(unsigned long count, unsigned long size);

#define TEST_CPUS	4U

/* The checks made and the ones that failed. */
static unsigned test_checks;
static unsigned test_failures;

/* The per-CPU states the scheduler under test works on. */
static struct sched_cpu test_cpus[TEST_CPUS];

/* The threads the scenarios queue, and a stand-in task for each. */
static struct thread test_threads[4];
static char test_tasks[4];

/* The CPU the test claims to run on. */
static hal_cpu_id_t test_current;

/* The task transfers the scheduler asked for. */
static unsigned test_transfers;

#define CHECK(condition) test_check((condition), #condition, __LINE__)

static void
test_check(
	int passed,
	const char *text,
	int line)
{
	test_checks++;
	if (passed)
		return;
	test_failures++;
	printf("FAIL line %d: %s\n", line, text);
}

/* The HAL and kernel stand-ins the scheduler calls. */

void
hal_fatal(
	const char *file,
	int line,
	const char *s)
{
	printf("fatal %s:%d: %s\n", file, line, s);
	exit(2);
	for (;;)
		;
}

unsigned
hal_cpu_count(void)
{
	return TEST_CPUS;
}

hal_cpu_id_t
hal_cpu_current(void)
{
	return test_current;
}

int
hal_cpu_notify(
	hal_cpu_id_t cpu)
{
	return HAL_OK;
}

void
hal_cpu_ready_mask(
	struct hal_cpu_mask *result)
{
	kern_memset(result, 0, sizeof(*result));
}

bool
hal_irq_disable(void)
{
	return false;
}

void
hal_irq_enable(void)
{
}

void
hal_task_context_switch(
	hal_task_t t)
{
	HAL_FATAL("context switch in the host test");
}

void *
hal_task_get_private(
	hal_task_t t)
{
	return NULL;
}

int
hal_task_transfer(
	hal_task_t task,
	hal_cpu_id_t target_cpu)
{
	test_transfers++;
	return HAL_OK;
}

void *
kern_boot_handoff(
	const char *name)
{
	return NULL;
}

int
kern_boot_parameters_token_present(
	const char *text,
	const char *token)
{
	return 0;
}

void *
kern_calloc(
	size_t count,
	size_t size)
{
	return calloc(count, size);
}

void
kern_logf(
	const char *format,
	...)
{
}

void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t count)
{
	unsigned char *to;
	const unsigned char *from;

	to = destination;
	from = source;
	while (count-- > 0)
		*to++ = *from++;
	return destination;
}

void *
kern_memset(
	void *destination,
	int value,
	size_t count)
{
	unsigned char *to;

	to = destination;
	while (count-- > 0)
		*to++ = (unsigned char)value;
	return destination;
}

int
kern_snprintf(
	char *buffer,
	size_t capacity,
	const char *format,
	...)
{
	if (capacity > 0)
		buffer[0] = '\0';
	return 0;
}

uint64_t
clock_ticks(void)
{
	return scheduler_ticks;
}

uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	return milliseconds;
}

uint64_t
kern_ticks_to_ms(
	uint64_t ticks)
{
	return ticks;
}

bool
kern_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	return false;
}

void
kern_sleep_idle(
	kern_cpu_id_t cpu)
{
}

struct process process0;
struct thread thread0;

int
process_itimer_tick(
	struct process *process,
	int which)
{
	return 0;
}

void
process_itimer_real_tick_all(void)
{
}

void
resource_limit_cpu_tick(
	struct process *process,
	uint64_t total_ticks)
{
}

int
signal_send_process_info(
	struct process *process,
	int signo,
	const struct signal_info *info)
{
	return 0;
}

void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
}

void
spin_lock(
	struct spinlock *lock)
{
}

void
spin_unlock(
	struct spinlock *lock)
{
}

unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	return 0;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
}

struct thread *
thread_current(void)
{
	return NULL;
}

void
thread_sched_retired(
	struct thread *thread)
{
}

int
vm_metadata_perf_take(
	struct vm_metadata_perf *window)
{
	return 0;
}

/* The test's own steps. */

/* Brings up the test's CPUs, all online and empty, at tick 10. */
static void
test_reset(void)
{
	unsigned i;

	kern_memset(test_cpus, 0, sizeof(test_cpus));
	kern_memset(test_threads, 0, sizeof(test_threads));
	for (i = 0; i < TEST_CPUS; i++)
		test_cpus[i].online = 1;
	scheduler_cpus = test_cpus;
	scheduler_cpu_count = TEST_CPUS;
	scheduler_ticks = 10;
	test_current = 3;
	test_transfers = 0;
}

/* Queues test thread `index` on `cpu` as runnable, with an affinity. */
static struct thread *
test_queue(
	unsigned index,
	hal_cpu_id_t cpu,
	enum sched_cpu_affinity affinity)
{
	struct thread *thread;

	thread = &test_threads[index];
	thread->task = &test_tasks[index];
	thread->state = THREAD_RUNNABLE;
	thread->sched.priority = SCHED_PRIORITY_DEFAULT;
	thread->sched.cpu = cpu;
	thread->sched.last_cpu = cpu;
	thread->sched.affinity = affinity;
	queue_append(&test_cpus[cpu].run[SCHED_PRIORITY_DEFAULT], thread,
	    SCHED_QUEUE_RUN);
	return thread;
}

/* Tests whether a thread sits in a CPU's run queue of its priority. */
static int
test_queued_on(
	const struct thread *thread,
	hal_cpu_id_t cpu)
{
	const struct thread *walk;

	walk = test_cpus[cpu].run[thread->sched.priority].head;
	while (walk != NULL) {
		if (walk == thread)
			return 1;
		walk = walk->sched.next;
	}
	return 0;
}

/* A free thread waiting since an earlier tick is taken by an idle CPU. */
static void
scenario_free_is_stolen(void)
{
	struct thread *thread;
	int taken;

	test_reset();
	thread = test_queue(0, 0, SCHED_CPU_FREE);
	scheduler_ticks++;
	taken = steal_runnable(1);
	CHECK(taken == 1);
	CHECK(thread->sched.cpu == 1);
	CHECK(test_queued_on(thread, 1));
	CHECK(!test_queued_on(thread, 0));
	CHECK(thread->sched.need_migrate == 0);
	CHECK(test_transfers == 1);
}

/* A bound thread stays on its CPU however long it waits. */
static void
scenario_bound_is_not_stolen(void)
{
	struct thread *thread;
	int taken;
	hal_cpu_id_t idle;

	test_reset();
	thread = test_queue(0, 0, SCHED_CPU_BOUND);
	scheduler_ticks += 100;
	for (idle = 1; idle < TEST_CPUS; idle++) {
		taken = steal_runnable(idle);
		CHECK(taken == 0);
	}
	CHECK(thread->sched.cpu == 0);
	CHECK(test_queued_on(thread, 0));
	CHECK(test_transfers == 0);
}

/* A bound thread at the head of the queue does not hold back a free one behind it. */
static void
scenario_free_behind_bound(void)
{
	struct thread *bound;
	struct thread *free_thread;
	int taken;

	test_reset();
	bound = test_queue(0, 0, SCHED_CPU_BOUND);
	free_thread = test_queue(1, 0, SCHED_CPU_FREE);
	scheduler_ticks++;
	taken = steal_runnable(2);
	CHECK(taken == 1);
	CHECK(free_thread->sched.cpu == 2);
	CHECK(test_queued_on(free_thread, 2));
	CHECK(bound->sched.cpu == 0);
	CHECK(test_queued_on(bound, 0));

	/* Nothing more can leave CPU 0. */
	taken = steal_runnable(1);
	CHECK(taken == 0);
	CHECK(test_queued_on(bound, 0));
}

/* sched_set_cpu() moves a thread and sets its affinity, or only sets it in place. */
static void
scenario_set_cpu(void)
{
	struct thread *thread;
	int error;
	int taken;

	test_reset();
	thread = test_queue(0, 0, SCHED_CPU_FREE);

	/* An unknown affinity is refused and changes nothing. */
	error = sched_set_cpu(thread, 2, (enum sched_cpu_affinity)7);
	CHECK(error == EINVAL);
	CHECK(thread->sched.cpu == 0);
	CHECK(thread->sched.affinity == SCHED_CPU_FREE);

	/* A move with BOUND leaves the thread bound on the new CPU. */
	error = sched_set_cpu(thread, 2, SCHED_CPU_BOUND);
	CHECK(error == 0);
	CHECK(thread->sched.cpu == 2);
	CHECK(thread->sched.affinity == SCHED_CPU_BOUND);
	CHECK(test_queued_on(thread, 2));
	scheduler_ticks++;
	taken = steal_runnable(1);
	CHECK(taken == 0);
	CHECK(test_queued_on(thread, 2));

	/* FREE on the same CPU only clears the binding; the thread may then go. */
	error = sched_set_cpu(thread, 2, SCHED_CPU_FREE);
	CHECK(error == 0);
	CHECK(thread->sched.cpu == 2);
	CHECK(thread->sched.affinity == SCHED_CPU_FREE);
	taken = steal_runnable(1);
	CHECK(taken == 1);
	CHECK(thread->sched.cpu == 1);

	/* BOUND on the same CPU binds without a move. */
	test_transfers = 0;
	error = sched_set_cpu(thread, 1, SCHED_CPU_BOUND);
	CHECK(error == 0);
	CHECK(test_transfers == 0);
	CHECK(thread->sched.affinity == SCHED_CPU_BOUND);
	scheduler_ticks++;
	taken = steal_runnable(0);
	CHECK(taken == 0);

	/* A thread in transit is not rebound. */
	thread->sched.need_migrate = SCHED_MIGRATING;
	error = sched_set_cpu(thread, 1, SCHED_CPU_FREE);
	CHECK(error == EBUSY);
	CHECK(thread->sched.affinity == SCHED_CPU_BOUND);
	thread->sched.need_migrate = 0;
}

/* A new thread, not yet queued, is bound where it was placed, as cpuperf binds one. */
static void
scenario_new_thread(void)
{
	struct thread *thread;
	int error;

	test_reset();
	thread = &test_threads[0];
	thread->task = &test_tasks[0];
	thread->state = THREAD_NEW;
	thread->sched.priority = SCHED_PRIORITY_DEFAULT;
	thread->sched.cpu = 0;
	error = sched_set_cpu(thread, 3, SCHED_CPU_BOUND);
	CHECK(error == 0);
	CHECK(thread->sched.cpu == 3);
	CHECK(thread->sched.affinity == SCHED_CPU_BOUND);
	CHECK(thread->sched.queue_kind == SCHED_QUEUE_NONE);

	/* Started on its CPU, it is not taken from there. */
	sched_add(thread);
	CHECK(test_queued_on(thread, 3));
	scheduler_ticks++;
	CHECK(steal_runnable(0) == 0);
	CHECK(thread->sched.cpu == 3);
}

int
main(void)
{
	scenario_free_is_stolen();
	scenario_bound_is_not_stolen();
	scenario_free_behind_bound();
	scenario_set_cpu();
	scenario_new_thread();
	printf("sched affinity host test: %u checks, %u failures\n",
	    test_checks, test_failures);
	if (test_failures != 0) {
		printf("sched affinity host test: FAIL\n");
		return 1;
	}
	printf("sched affinity host test: PASS\n");
	return 0;
}
