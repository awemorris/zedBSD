/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/atomic.h>
#include <kern/lock.h>

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <threads.h>

struct thread;
static struct spinlock test_lock;
static unsigned protected_counter;

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
struct thread *thread_current(void) { return (struct thread *)(uintptr_t)1; }
int signal_pending_unblocked(const struct thread *thread)
{ (void)thread; return 0; }
void sched_yield(void) { thrd_yield(); }

static int increment(void *argument)
{
	unsigned i;
	(void)argument;
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

	spin_init(&test_lock, LOCK_RANK_FILE, "host-test");
	assert(thrd_create(&first, increment, NULL) == thrd_success);
	assert(thrd_create(&second, increment, NULL) == thrd_success);
	assert(thrd_join(first, NULL) == thrd_success);
	assert(thrd_join(second, NULL) == thrd_success);
	assert(protected_counter == 200000U);
	puts("zedBSD atomic/refcount/spinlock host tests: PASS");
	return 0;
}
