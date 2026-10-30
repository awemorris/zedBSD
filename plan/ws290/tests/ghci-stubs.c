/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel's services for the GHCI driver's host tests (ghci-stubs.h).
 *
 * Everything runs on the test's one thread.  A sleep advances the fake
 * clock by one tick, lets the model run, and calls the test's hook; a
 * lock taken twice would never come back on one thread, so it is counted
 * as an error instead.  drv_gpu_register checks the operations with the
 * checks of gpu.c (stub_gpu_ops_validate, which the test script takes out
 * of src/drivers/gpu/gpu.c when it builds the test).
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/gpu/gpu.h"
#include "kern/clock.h"
#include "kern/cred.h"
#include "kern/device-io.h"
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/pmem.h"
#include "kern/poll.h"
#include "kern/sched.h"
#include "kern/waitq.h"
#include "uapi/errno.h"
#include "ghci-model.h"
#include "ghci-stubs.h"

/* The bytes of the stand-in of BAR0: a 4 KiB page, of which the model answers the first 1 KiB. */
#define STUB_BAR0_BYTES			4096U

/* The highest address the stand-in of RAM hands out. */
#define STUB_RAM_LIMIT			0xFFFFFFFFFFFFFFFFULL

/* The stubs' state; stub_reset clears it before each scene of a test. */
struct stub_state stub;

/*
 * The stand-in of BAR0.  Its bytes are never read: the MMIO stubs turn an
 * address inside it into an offset of the model's register window.
 */
static uint32_t stub_bar0_words[STUB_BAR0_BYTES / 4U];

static void held_add(const char *name);
static void held_remove(const char *name);
static void check_order(const char *name);
static void check_wait(const char *queue);
static void sleep_tick(void);

/*
 * Clears what the stubs saw and set, and the fake clock.
 */
void
stub_reset(void)
{
	int quiet;

	/* Keeps only whether the log is quiet. */
	quiet = stub.quiet;
	memset(&stub, 0, sizeof(stub));
	stub.quiet = quiet;
	stub.text_end_result = ENOTSUP;
}

/*
 * Gives the address the driver maps as BAR0.
 */
volatile void *
stub_bar0(void)
{
	/* The stand-in's first byte. */
	return stub_bar0_words;
}

/*
 * Sets the order in which the driver must take its locks, by name.
 */
void
stub_lock_order(
	const char *const *names,
	unsigned count)
{
	unsigned index;

	/* Keeps up to STUB_ORDER_MAX names, first taken first. */
	stub.order_count = 0U;
	for (index = 0U; index < count && index < STUB_ORDER_MAX; index++)
		stub.order[index] = names[index];
	stub.order_count = index;
}

/*
 * Forbids holding a lock while waiting on a named queue.
 */
void
stub_forbid_wait(
	const char *queue,
	const char *lock)
{
	/* Keeps the rule while there is room. */
	if (stub.forbid_count >= STUB_FORBID_MAX)
		return;
	stub.forbid_queue[stub.forbid_count] = queue;
	stub.forbid_lock[stub.forbid_count] = lock;
	stub.forbid_count++;
}

/*
 * Sets the hook that runs at each sleep of the driver.
 */
void
stub_set_sleep_hook(
	stub_sleep_hook hook,
	void *argument)
{
	/* Remembers the hook and its argument. */
	stub.hook = hook;
	stub.hook_argument = argument;
}

/*
 * Reports whether the driver holds a lock now.
 */
int
stub_holds(
	const char *name)
{
	unsigned index;

	/* Searches the held locks for the name. */
	for (index = 0U; index < stub.held_count; index++) {
		/* The same name is the same lock. */
		if (strcmp(stub.held[index], name) == 0)
			return 1;
	}

	/* Not held. */
	return 0;
}

/* The kernel's MMIO read: an address in BAR0 reads the model's register. */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	uintptr_t offset;

	/* Turns the address into an offset of BAR0. */
	offset = (uintptr_t)address - (uintptr_t)stub_bar0_words;
	if (offset >= STUB_BAR0_BYTES) {
		stub.mmio_errors++;
		if (!stub.quiet)
			printf("FAIL MMIO read outside BAR0\n");
		return 0xFFFFFFFFU;
	}

	/* Succeeded: the model's register. */
	return model_read((uint32_t)offset);
}

/* The kernel's MMIO write: an address in BAR0 writes the model's register. */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	uintptr_t offset;

	/* Turns the address into an offset of BAR0. */
	offset = (uintptr_t)address - (uintptr_t)stub_bar0_words;
	if (offset >= STUB_BAR0_BYTES) {
		stub.mmio_errors++;
		if (!stub.quiet)
			printf("FAIL MMIO write outside BAR0\n");
		return;
	}

	/* Writes the model's register. */
	model_write((uint32_t)offset, value);
}

/* The barriers do nothing on the host. */
void
kern_io_write_barrier(void)
{
}

void
kern_io_read_barrier(void)
{
}

/* A mutex: its name is kept for the order and wait checks. */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	/* Clears the mutex and keeps its name in the guard. */
	memset(mutex, 0, sizeof(*mutex));
	mutex->guard.rank = rank;
	mutex->guard.name = name;

	/* Succeeded. */
	return 0;
}

void
mutex_lock(
	struct mutex *mutex)
{
	/* A second lock of the same mutex on one thread would never come back. */
	if (mutex->locked != 0U) {
		stub.double_locks++;
		if (!stub.quiet)
			printf("FAIL mutex %s locked twice\n", mutex->guard.name);
	}

	/* Checks the order, then holds it. */
	check_order(mutex->guard.name);
	mutex->locked = 1U;
	held_add(mutex->guard.name);
}

int
mutex_trylock(
	struct mutex *mutex)
{
	/* A mutex held (on the one thread there is, by a caller further up) is not taken. */
	if (mutex->locked != 0U)
		return 0;

	/* Checks the order, then holds it. */
	check_order(mutex->guard.name);
	mutex->locked = 1U;
	held_add(mutex->guard.name);

	/* Succeeded: taken. */
	return 1;
}

void
mutex_unlock(
	struct mutex *mutex)
{
	/* Unlocking a mutex that is not locked is an error. */
	if (mutex->locked == 0U) {
		stub.unlock_errors++;
		if (!stub.quiet)
			printf("FAIL mutex %s unlocked while not locked\n", mutex->guard.name);
	}

	/* Releases it. */
	mutex->locked = 0U;
	held_remove(mutex->guard.name);
}

/* A spinlock: followed like a mutex, by name. */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* Clears the lock and keeps its name. */
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

void
spin_lock(
	struct spinlock *lock)
{
	/* A second lock of the same spinlock on one thread would spin for ever. */
	if (lock->held.value != 0U) {
		stub.double_locks++;
		if (!stub.quiet)
			printf("FAIL spinlock %s locked twice\n", lock->name);
	}

	/* Checks the order, then holds it. */
	check_order(lock->name);
	lock->held.value = 1U;
	held_add(lock->name);
}

void
spin_unlock(
	struct spinlock *lock)
{
	/* Releases it. */
	lock->held.value = 0U;
	held_remove(lock->name);
}

unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* The same as spin_lock; there are no interrupts to save. */
	spin_lock(lock);
	return 0;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* The same as spin_unlock. */
	(void)enabled;
	spin_unlock(lock);
}

/* A wait queue: a sleep is one tick of the fake clock in which the model runs. */
void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	/* Clears the queue and keeps its name for the wait checks. */
	memset(queue, 0, sizeof(*queue));
	queue->name = name;
}

uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	/* The sequence the queue's wakes advanced. */
	return queue->sequence;
}

int
waitq_sleep(
	struct wait_queue *queue,
	struct spinlock *condition_lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	/* The condition lock is released while asleep, as the kernel does. */
	(void)observed;
	(void)deadline;
	(void)flags;
	if (condition_lock != NULL) {
		held_remove(condition_lock->name);
		condition_lock->held.value = 0U;
	}

	/* Checks the locks held across the wait, then sleeps one tick. */
	check_wait(queue->name);
	sleep_tick();

	/* Takes the condition lock back. */
	if (condition_lock != NULL) {
		condition_lock->held.value = 1U;
		held_add(condition_lock->name);
	}

	/* Succeeded: woken. */
	return 0;
}

void
waitq_wake_one(
	struct wait_queue *queue)
{
	/* Advances the sequence a sleeper compares. */
	queue->sequence++;
}

void
waitq_wake_all(
	struct wait_queue *queue)
{
	/* Advances the sequence a sleeper compares. */
	queue->sequence++;
}

/* The clock: one tick per sleep. */
uint64_t
clock_ticks(void)
{
	/* The fake clock. */
	return stub.ticks;
}

/* A tick of the fake clock is one millisecond. */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	/* The same count in the fake clock's unit. */
	return milliseconds;
}

/* A tick of the fake clock is one millisecond. */
uint64_t
kern_ticks_to_ns(
	uint64_t ticks)
{
	/* The same time in nanoseconds. */
	return ticks * 1000000U;
}

void
sched_sleep(
	uint64_t timeout_tick)
{
	/* A timed sleep checks the held locks like a wait on the queue "sleep". */
	check_wait("sleep");

	/* The clock jumps to the deadline, at least one tick, and the model runs. */
	if (timeout_tick > stub.ticks + 1U)
		stub.ticks = timeout_tick - 1U;
	sleep_tick();
}

/* Nothing waits on poll in the tests. */
void
poll_notify(void)
{
}

/* The log goes to standard output, unless the test made it quiet. */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	/* Counts the line, and prints it when the log is not quiet. */
	stub.log_lines++;
	if (stub.quiet)
		return;
	va_start(arguments, format);
	printf("  log: ");
	vprintf(format, arguments);
	va_end(arguments);
}

/*
 * The console's leaving of the display: counts the call, the locks held at
 * it, keeps the ranges, runs the test's hook and gives the test's result.
 */
int
kern_text_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	unsigned index;

	/* The call, made with no lock held. */
	stub.text_end_calls++;
	if (stub.held_count != 0U) {
		stub.text_end_held++;
		if (!stub.quiet)
			printf("FAIL kern_text_end called holding %s\n", stub.held[0]);
	}

	/* The ranges, as many as fit. */
	stub.text_end_count = count;
	for (index = 0U; index < count && index < KERN_TEXT_END_RANGES; index++)
		stub.text_end_ranges[index] = ranges[index];

	/* What the test does meanwhile. */
	if (stub.text_end_hook != NULL)
		stub.text_end_hook(stub.text_end_argument);

	/* The test's result. */
	return stub.text_end_result;
}

/* The kernel's heap is the host's. */
void *
kern_malloc(
	size_t size)
{
	/* The host's allocator. */
	return malloc(size);
}

void *
kern_calloc(
	size_t count,
	size_t size)
{
	/* The host's allocator, cleared. */
	return calloc(count, size);
}

void
kern_free(
	void *pointer)
{
	/* The host's allocator. */
	free(pointer);
}

/* RAM: the model's host memory, which never reuses a run. */
int
kern_pmem_alloc_limited(
	size_t size,
	size_t alignment,
	uint64_t max_address,
	size_t boundary,
	struct kern_pmem *run)
{
	uint64_t bus;
	int error;

	/* Takes a run of the model's host memory the device can name. */
	(void)boundary;
	error = model_host_alloc(size, alignment, max_address, &bus);
	if (error != 0)
		return 4;

	/* Succeeded: the run. */
	run->paddr = (kern_physaddr_t)bus;
	run->size = size;
	return 0;
}

int
kern_pmem_alloc(
	size_t size,
	size_t alignment,
	struct kern_pmem *run)
{
	/* Any address. */
	return kern_pmem_alloc_limited(size, alignment, STUB_RAM_LIMIT, 0U, run);
}

int
kern_pmem_free(
	struct kern_pmem *run)
{
	/* The model never reuses a run. */
	(void)run;
	return 0;
}

void *
kern_pmem_to_kernel(
	kern_physaddr_t address)
{
	/* The run's bytes in the model. */
	return model_dma(address, 1U);
}

int
kern_pmem_map_uncached(
	const struct kern_pmem *run,
	void **mapped)
{
	/* The run's own bytes; the model has no cache. */
	stub.uncached_maps++;
	*mapped = model_dma(run->paddr, run->size);
	if (*mapped == NULL)
		return 4;

	/* Succeeded: the view. */
	return 0;
}

int
kern_pmem_unmap_uncached(
	void *mapped,
	size_t size)
{
	/* Counted only. */
	(void)mapped;
	(void)size;
	stub.uncached_unmaps++;
	return 0;
}

/* The GPU core: a registration is checked as gpu.c checks it and remembered. */
int
drv_gpu_register(
	const struct drv_gpu_ops *ops,
	void *private_data,
	struct drv_gpu_device **result)
{
	int error;

	/* Refuses what gpu.c refuses. */
	*result = NULL;
	error = stub_gpu_ops_validate(ops);
	if (error != 0) {
		stub.register_errors++;
		if (!stub.quiet)
			printf("FAIL drv_gpu_register refused the operations (%d)\n", error);
		return error;
	}

	/* Remembers the operations; the handle is the stubs' state. */
	stub.registrations++;
	stub.core_ops = ops;
	stub.core_private = private_data;
	*result = (struct drv_gpu_device *)(void *)&stub;

	/* Succeeded. */
	return 0;
}

int
drv_gpu_unregister(
	struct drv_gpu_device *device)
{
	/* Forgets the operations. */
	(void)device;
	stub.unregistrations++;
	stub.core_ops = NULL;
	return 0;
}

void
drv_gpu_report_error(
	struct drv_gpu_device *device,
	int error)
{
	/* Counted. */
	(void)device;
	(void)error;
	stub.errors_reported++;
}

void
drv_gpu_report_session_error(
	struct drv_gpu_device *device,
	void *session,
	int error)
{
	/* Counted. */
	(void)device;
	(void)session;
	(void)error;
	stub.session_errors++;
}

void
drv_gpu_capacity_changed(
	struct drv_gpu_device *device)
{
	/* Counted. */
	(void)device;
	stub.capacity_notes++;
}

void
drv_gpu_complete(
	struct drv_gpu_completion *completion,
	int error)
{
	/* Counted, with the last error. */
	(void)completion;
	stub.completions++;
	stub.last_completion_error = error;
}

/* The credentials: root or not, as the test says. */
const struct ucred *
cred_current(void)
{
	/* No credentials object; cred_is_superuser answers. */
	return NULL;
}

int
cred_is_superuser(
	const struct ucred *cred)
{
	/* What the test set. */
	(void)cred;
	return stub.superuser;
}

/* Adds a lock to the held list. */
static void
held_add(
	const char *name)
{
	/* More locks than the list holds is itself an error of the driver. */
	if (stub.held_count >= STUB_HELD_MAX) {
		stub.order_violations++;
		return;
	}

	/* Holds it. */
	stub.held[stub.held_count] = name;
	stub.held_count++;
}

/* Removes the newest entry of a lock from the held list. */
static void
held_remove(
	const char *name)
{
	unsigned index;

	/* Searches from the newest. */
	for (index = stub.held_count; index > 0U; index--) {
		/* Removes the entry and closes the gap. */
		if (strcmp(stub.held[index - 1U], name) == 0) {
			memmove(&stub.held[index - 1U], &stub.held[index],
			    (stub.held_count - index) * sizeof(stub.held[0]));
			stub.held_count--;
			return;
		}
	}
}

/* Counts a lock taken while a lock later in the order is held. */
static void
check_order(
	const char *name)
{
	unsigned taken;
	unsigned held;
	unsigned rank;

	/* Finds the lock in the order; a lock the order does not name is not checked. */
	for (taken = 0U; taken < stub.order_count; taken++) {
		/* The order names the lock. */
		if (strcmp(stub.order[taken], name) == 0)
			break;
	}
	if (taken == stub.order_count)
		return;

	/* Every held lock of the order must come before it. */
	for (held = 0U; held < stub.held_count; held++) {
		/* Finds the held lock's place in the order. */
		for (rank = 0U; rank < stub.order_count; rank++) {
			/* The order names the held lock. */
			if (strcmp(stub.order[rank], stub.held[held]) == 0)
				break;
		}

		/* A held lock later in the order is a violation. */
		if (rank < stub.order_count && rank > taken) {
			stub.order_violations++;
			if (!stub.quiet)
				printf("FAIL lock %s taken while holding %s\n", name, stub.held[held]);
		}
	}
}

/* Counts each forbidden lock held while waiting on a queue. */
static void
check_wait(
	const char *queue)
{
	unsigned rule;
	int held;

	/* A queue without a name has no rules. */
	if (queue == NULL)
		return;

	/* Checks each rule of the queue. */
	for (rule = 0U; rule < stub.forbid_count; rule++) {
		/* A rule of another queue does not apply. */
		if (strcmp(stub.forbid_queue[rule], queue) != 0)
			continue;

		/* The forbidden lock must not be held. */
		held = stub_holds(stub.forbid_lock[rule]);
		if (held) {
			stub.wait_violations++;
			if (!stub.quiet)
				printf("FAIL waited on %s holding %s\n", queue, stub.forbid_lock[rule]);
		}
	}
}

/* One tick of the fake clock: the model runs, then the test's hook. */
static void
sleep_tick(void)
{
	/* Advances the clock and lets the model go on. */
	stub.ticks++;
	stub.sleeps++;
	model_tick();

	/* Runs the test's hook in the middle of the wait. */
	if (stub.hook != NULL)
		stub.hook(stub.sleeps, stub.hook_argument);
}
