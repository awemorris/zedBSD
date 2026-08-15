/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/atomic.h>
#include <kern/lock.h>
#include <hal/hal.h>

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <threads.h>

struct thread;
static struct spinlock test_lock;
static unsigned protected_counter;
static _Thread_local hal_cpu_id_t test_cpu;

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
hal_cpu_id_t hal_cpu_current(void) { return test_cpu; }
struct thread *thread_current(void) { return (struct thread *)(uintptr_t)1; }
int signal_pending_unblocked(const struct thread *thread)
{ (void)thread; return 0; }
void sched_yield(void) { thrd_yield(); }
void waitq_init(struct wait_queue *queue, const char *name)
{ queue->head = queue->tail = NULL; queue->sequence = 1; queue->name = name; }
uint64_t waitq_sequence(const struct wait_queue *queue)
{ return queue->sequence; }
int waitq_sleep(struct wait_queue *queue, struct spinlock *lock,
    uint64_t sequence, uint64_t deadline, unsigned flags)
{ (void)queue; (void)lock; (void)sequence; (void)deadline; (void)flags; return 0; }
void waitq_wake_one(struct wait_queue *queue) { (void)queue; }
void waitq_wake_all(struct wait_queue *queue) { (void)queue; }

static int increment(void *argument)
{
	unsigned i;
	test_cpu = (hal_cpu_id_t)(uintptr_t)argument;
	for (i = 0; i < 100000U; i++) {
		spin_lock(&test_lock);
		protected_counter++;
		spin_unlock(&test_lock);
	}
	return 0;
}

int main(void)
{
	atomic_uint_t atomic = { 0 };
	refcount_t references;
	struct hal_cpu_mask mask;
	thrd_t first, second;
	unsigned expected = 1;

	assert(atomic_load_acquire(&atomic) == 0);
	atomic_store_release(&atomic, 1);
	assert(atomic_fetch_add_relaxed(&atomic, 2) == 1);
	assert(atomic_compare_exchange(&atomic, &expected, 4) == 0);
	expected = 3;
	assert(atomic_compare_exchange(&atomic, &expected, 4) != 0);
	refcount_init(&references, 1);
	assert(refcount_tryget(&references) != 0);
	assert(refcount_put(&references) == 0);
	assert(refcount_put(&references) != 0);
	assert(refcount_tryget(&references) == 0);

	hal_cpu_mask_zero(&mask);
	assert(!hal_cpu_mask_test(&mask, 0));
	hal_cpu_mask_set(&mask, 0);
	hal_cpu_mask_set(&mask, 63);
	hal_cpu_mask_set(&mask, 64);
	hal_cpu_mask_set(&mask, HAL_CPU_MAX - 1U);
	hal_cpu_mask_set(&mask, HAL_CPU_MAX);
	assert(hal_cpu_mask_test(&mask, 0));
	assert(hal_cpu_mask_test(&mask, 63));
	assert(hal_cpu_mask_test(&mask, 64));
	assert(hal_cpu_mask_test(&mask, HAL_CPU_MAX - 1U));
	assert(!hal_cpu_mask_test(&mask, HAL_CPU_MAX));
	hal_cpu_mask_clear(&mask, 64);
	assert(!hal_cpu_mask_test(&mask, 64));
	hal_cpu_mask_fill(&mask);
	assert(hal_cpu_mask_test(&mask, 0));
	assert(hal_cpu_mask_test(&mask, HAL_CPU_MAX - 1U));

	spin_init(&test_lock, LOCK_RANK_FILE, "host-test");
	assert(thrd_create(&first, increment, (void *)(uintptr_t)1) == thrd_success);
	assert(thrd_create(&second, increment, (void *)(uintptr_t)2) == thrd_success);
	assert(thrd_join(first, NULL) == thrd_success);
	assert(thrd_join(second, NULL) == thrd_success);
	assert(protected_counter == 200000U);
	puts("zedBSD atomic/refcount/spinlock host tests: PASS");
	return 0;
}
