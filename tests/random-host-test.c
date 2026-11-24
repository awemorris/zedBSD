/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test for kernel random seeding, KERN_RANDOM_WAIT, and
 * KERN_RANDOM_NONBLOCK behavior in src/kern/random.c.
 *
 * cc -std=c99 -O1 -Wall -Wextra -Werror -DHAL_ARCH_X86_64 -Iinclude -Isrc \
 *    -fsanitize=address,undefined -fno-sanitize-recover=all \
 *    tests/random-host-test.c src/kern/random-crypto.c -o <out> && <out>
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <kern/random.h>
#include <uapi/errno.h>

/* Host stubs for kernel primitives used by src/kern/random.c. */
#define KERN_KERN_KCRT_H
#define KERN_KERN_LOCK_H
#define KERN_KERN_SCHED_H
#define KERN_KERN_CLOCK_H
#define KERN_KERN_SIGNAL_H
#define KERN_KERN_THREAD_H
#define KERN_KERN_WAITQ_H
#define HAL_HAL_H

#define LOCK_RANK_RANDOM 0
#define WAITQ_INTERRUPTIBLE 1

struct spinlock {
	int locked;
};

struct wait_queue {
	int waiters;
};

typedef unsigned hal_cpu_id_t;

static unsigned stub_hal_entropy_ok;
static uint64_t stub_ticks;
static unsigned stub_wait_sleeps;
static unsigned stub_wake_all_calls;
static void (*stub_on_sleep)(void);

static void
kern_memset_explicit(void *ptr, int value, size_t size)
{
	volatile unsigned char *p = ptr;
	while (size-- != 0U)
		*p++ = (unsigned char)value;
}

static void *
kern_memcpy(void *dst, const void *src, size_t size)
{
	return memcpy(dst, src, size);
}

static void
spin_init(struct spinlock *lock, int rank, const char *name)
{
	(void)rank;
	(void)name;
	lock->locked = 0;
}

static unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	lock->locked = 1;
	return 0UL;
}

static void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{
	(void)irq;
	lock->locked = 0;
}

static void
waitq_init(struct wait_queue *wq, const char *name)
{
	(void)name;
	wq->waiters = 0;
}

static uint64_t
waitq_sequence(struct wait_queue *wq)
{
	(void)wq;
	return 0ULL;
}

static int
waitq_sleep(struct wait_queue *wq, struct spinlock *lock, uint64_t seq,
	    uint64_t timeout, unsigned flags)
{
	(void)wq;
	(void)seq;
	(void)timeout;
	(void)flags;
	stub_wait_sleeps++;
	lock->locked = 0;
	if (stub_on_sleep != NULL)
		stub_on_sleep();
	lock->locked = 1;
	return 0;
}

static void
waitq_wake_all(struct wait_queue *wq)
{
	(void)wq;
	stub_wake_all_calls++;
}

static uint64_t
sched_ticks(void)
{
	return stub_ticks;
}

static uint64_t
kern_ms_to_ticks(uint64_t ms)
{
	return ms;
}

static unsigned
atomic_raw_load_acquire(const volatile unsigned *ptr)
{
	return *ptr;
}

static void
atomic_raw_store_release(volatile unsigned *ptr, unsigned value)
{
	*ptr = value;
}

static bool
hal_entropy_fill(void *buffer, size_t size)
{
	unsigned char *bytes;
	size_t i;

	if (stub_hal_entropy_ok == 0U)
		return false;
	bytes = buffer;
	for (i = 0U; i < size; i++)
		bytes[i] = (unsigned char)(0xa5U ^ (unsigned)i);
	return true;
}

static bool
hal_rtc_read_epoch_time(uint64_t *seconds)
{
	*seconds = 1700000000ULL;
	return true;
}

static bool
hal_rtc_read_counter(uint64_t *counter, uint64_t *frequency)
{
	stub_ticks++;
	*counter = stub_ticks * 1000ULL + (stub_ticks & 0xfULL);
	*frequency = 1000000ULL;
	return true;
}

#include "../src/kern/random.c"

static unsigned failures;
static unsigned checks;

static void
check(int cond, const char *msg)
{
	checks++;
	if (!cond) {
		fprintf(stderr, "random-host-test: FAIL: %s\n", msg);
		failures++;
	}
}

static void
feed_256_ticks(void)
{
	unsigned i;

	for (i = 0U; i < RANDOM_SEED_SAMPLES; i++)
		kern_random_tick(0U);
}

int
main(void)
{
	uint8_t buf1[32];
	uint8_t buf2[32];
	int error;

	/* Before init: unseeded, read returns EAGAIN. */
	check(!kern_random_seeded(), "not seeded before init");
	error = kern_random_read(buf1, sizeof(buf1), KERN_RANDOM_NONBLOCK);
	check(error == EAGAIN, "nonblock before init returns EAGAIN");

	/* Init without platform entropy: remains unseeded until 256 ticks. */
	stub_hal_entropy_ok = 0U;
	stub_wait_sleeps = 0U;
	stub_wake_all_calls = 0U;
	stub_on_sleep = NULL;
	kern_random_init();
	check(!kern_random_seeded(), "unseeded when hal_entropy_fill returns false");

	/* Non-blocking read fails with EAGAIN while unseeded. */
	error = kern_random_read(buf1, sizeof(buf1), KERN_RANDOM_NONBLOCK);
	check(error == EAGAIN, "nonblock while unseeded returns EAGAIN");

	/* Flags == 0 (urandom path) succeeds without blocking. */
	memset(buf1, 0, sizeof(buf1));
	error = kern_random_read(buf1, sizeof(buf1), 0U);
	check(error == 0, "unflagged read succeeds while unseeded");

	/* KERN_RANDOM_WAIT sleeps until 256 ticks seed the pool. */
	stub_on_sleep = feed_256_ticks;
	memset(buf2, 0, sizeof(buf2));
	error = kern_random_read(buf2, sizeof(buf2), KERN_RANDOM_WAIT);
	check(error == 0, "waiting read succeeds after tick seeding");
	check(stub_wait_sleeps == 1U, "waiting read slept once");
	check(stub_wake_all_calls == 1U, "256th tick woke waiters");
	check(kern_random_seeded(), "seeded after 256 ticks");

	/* Non-blocking read now succeeds. */
	error = kern_random_read(buf1, sizeof(buf1), KERN_RANDOM_NONBLOCK);
	check(error == 0, "nonblock succeeds once seeded");
	check(memcmp(buf1, buf2, sizeof(buf1)) != 0, "successive draws differ");

	/* Reset state and test init with platform entropy available. */
	random_ready = 0U;
	random_is_seeded = 0U;
	random_samples = 0U;
	stub_hal_entropy_ok = 1U;
	stub_wait_sleeps = 0U;
	stub_on_sleep = NULL;
	kern_random_init();
	check(kern_random_seeded(), "seeded immediately when hal_entropy_fill succeeds");

	error = kern_random_read(buf1, sizeof(buf1), KERN_RANDOM_NONBLOCK);
	check(error == 0, "nonblock succeeds immediately with platform entropy");
	error = kern_random_read(buf2, sizeof(buf2), KERN_RANDOM_WAIT);
	check(error == 0 && stub_wait_sleeps == 0U, "wait does not sleep with platform entropy");

	if (failures != 0U) {
		printf("random-host-test: FAIL (%u of %u checks)\n", failures, checks);
		return 1;
	}
	printf("random-host-test: PASS (%u checks)\n", checks);
	return 0;
}
