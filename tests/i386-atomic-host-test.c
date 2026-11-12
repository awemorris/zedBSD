/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the C library's i386 atomic runtime
 * (src/libc/atomic-runtime.c), which serializes every operation it is asked
 * for under lock words taken with XCHG.  Threads count on objects of 1, 2, 4,
 * 8 and 16 bytes through the runtime's entries, a reader watches an 8-byte
 * object that a writer stores for a torn value, and the process forks while
 * other threads work, so that a lock word copied held would hang the child.
 *
 * The runtime is compiled for the 386 by the tree's compiler and linked into
 * a 32-bit host program; the 8-byte and 16-byte operations below are the
 * compiler's own calls into it, and the narrower ones are called by name, as
 * the compiler inlines them:
 *
 * build/llvm/bin/clang --target=i386-unknown-linux-gnu -march=i386 -Os \
 *    -ffreestanding -fno-builtin -fno-pic -DHAL_ARCH_I386 -Iinclude \
 *    -Wall -Wextra -Werror -c src/libc/atomic-runtime.c -o <runtime.o>
 * build/llvm/bin/clang --target=i386-unknown-linux-gnu -march=i386 -O2 \
 *    -fno-pic -Iinclude -Wall -Wextra -Werror -Wno-atomic-alignment \
 *    -c tests/i386-atomic-host-test.c -o <test.o>
 * cc -m32 -no-pie <test.o> <runtime.o> -lpthread -o <out> && <out>
 */

#include <uapi/syscall.h>

#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <pthread.h>
#include <unistd.h>

/* The threads that count at once, and the additions each makes per object. */
#define COUNT_THREADS	4U
#define COUNT_ROUNDS	200000U

/* The stores the torn-value writer makes. */
#define TEAR_ROUNDS	400000U

/* The forks made while counting threads run, and how long a child may take (s). */
#define FORK_ROUNDS	200U
#define FORK_SECONDS	5U

/*
 * An object wider than any lock-free one, so that the compiler calls the
 * runtime's generic entries for it.
 */
struct wide_counter {
	uint32_t word[4];
};

/* The runtime's entries for the widths the compiler inlines on the host. */
uint8_t runtime_fetch_add_1(volatile void *object, uint8_t operand, int order) __asm__("__atomic_fetch_add_1");
uint16_t runtime_fetch_add_2(volatile void *object, uint16_t operand, int order) __asm__("__atomic_fetch_add_2");
uint32_t runtime_fetch_add_4(volatile void *object, uint32_t operand, int order) __asm__("__atomic_fetch_add_4");
bool runtime_compare_exchange_4(volatile void *object, uint32_t *expected, uint32_t desired, int success, int failure) __asm__("__atomic_compare_exchange_4");
uint32_t runtime_load_4(const volatile void *object, int order) __asm__("__atomic_load_4");

/* The runtime's fork hooks. */
void __libc_atomic_fork_prepare(void);
void __libc_atomic_fork_parent(void);
void __libc_atomic_fork_child(void);

/* The system-call entry the runtime yields through. */
intptr_t __syscall6(uint32_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);

/*
 * The objects the counting threads add to.  The 8-byte ones are aligned as
 * the library's are; the i386 ABI would align them to 4 only.
 */
static uint8_t count_1;
static uint16_t count_2;
static uint32_t count_4;
static uint32_t count_4_compare;
static _Alignas(8) uint64_t count_8;
static _Alignas(8) uint64_t count_8_compare;
static struct wide_counter count_16;

/* The 8-byte object the tear check stores and loads, and the torn values the reader saw. */
static _Alignas(8) uint64_t tear_value;
static unsigned tear_seen;

/* Whether the fork check's busy threads should stop. */
static volatile int busy_stop;

/* The checks that failed. */
static int failures;

static void check(int condition, const char *what);
static void *count_thread(void *argument);
static void *tear_writer(void *argument);
static void *tear_reader(void *argument);
static void *busy_thread(void *argument);
static void check_counts(void);
static void check_tear(void);
static void check_fork(void);

/*
 * Runs the checks and reports the result.
 */
int
main(
	void)
{
	/* Counts through every width from several threads. */
	check_counts();

	/* Watches an 8-byte object for a value half old and half new. */
	check_tear();

	/* Forks while other threads hold and give back lock words. */
	check_fork();

	/* Reports any failed check. */
	if (failures != 0) {
		printf("i386-atomic-host-test: FAIL (%d)\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("i386-atomic-host-test: PASS\n");
	return 0;
}

/*
 * Stands in for the system call the runtime makes to yield.
 */
intptr_t
__syscall6(
	uint32_t number,
	uintptr_t a0,
	uintptr_t a1,
	uintptr_t a2,
	uintptr_t a3,
	uintptr_t a4,
	uintptr_t a5)
{
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	(void)a5;

	/* Refuses any call but the yield: the runtime makes no other on i386. */
	if (number != KERN_SYS_sched_yield)
		abort();

	/* Gives the processor away. */
	(void)sched_yield();

	/* Succeeded: the yield. */
	return 0;
}

/* Counts a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* Reports the check that failed. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/* Adds one to every counted object, COUNT_ROUNDS times. */
static void *
count_thread(
	void *argument)
{
	struct wide_counter expected;
	struct wide_counter desired;
	uint64_t observed;
	uint32_t observed_4;
	unsigned round;

	(void)argument;

	/* Adds through each width's entry, and through comparison loops. */
	for (round = 0; round < COUNT_ROUNDS; round++) {
		(void)runtime_fetch_add_1(&count_1, 1U, __ATOMIC_SEQ_CST);
		(void)runtime_fetch_add_2(&count_2, 1U, __ATOMIC_SEQ_CST);
		(void)runtime_fetch_add_4(&count_4, 1U, __ATOMIC_SEQ_CST);
		(void)__atomic_fetch_add(&count_8, 1U, __ATOMIC_SEQ_CST);

		/* The 4-byte comparison loop. */
		observed_4 = runtime_load_4(&count_4_compare, __ATOMIC_ACQUIRE);
		while (!runtime_compare_exchange_4(&count_4_compare, &observed_4, observed_4 + 1U, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE))
			;

		/* The 8-byte comparison loop. */
		observed = __atomic_load_n(&count_8_compare, __ATOMIC_ACQUIRE);
		while (!__atomic_compare_exchange_n(&count_8_compare, &observed, observed + 1U, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE))
			;

		/* The 16-byte comparison loop, every word one more. */
		__atomic_load(&count_16, &expected, __ATOMIC_ACQUIRE);
		do {
			desired.word[0] = expected.word[0] + 1U;
			desired.word[1] = expected.word[1] + 1U;
			desired.word[2] = expected.word[2] + 1U;
			desired.word[3] = expected.word[3] + 1U;
		} while (!__atomic_compare_exchange(&count_16, &expected, &desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));
	}

	/* Succeeded: the thread's additions are made. */
	return NULL;
}

/* Stores values whose two halves are equal into the watched object. */
static void *
tear_writer(
	void *argument)
{
	uint64_t value;
	unsigned round;

	(void)argument;

	/* Stores a run of values that change both halves at once. */
	for (round = 1; round <= TEAR_ROUNDS; round++) {
		value = ((uint64_t)round << 32) | round;
		__atomic_store_n(&tear_value, value, __ATOMIC_RELEASE);
	}

	/* Succeeded: the stores are made. */
	return NULL;
}

/* Loads the watched object until the writer's last value and counts torn ones. */
static void *
tear_reader(
	void *argument)
{
	uint64_t value;
	uint32_t high;
	uint32_t low;

	(void)argument;

	/* Reads until the last value shows up. */
	do {
		value = __atomic_load_n(&tear_value, __ATOMIC_ACQUIRE);
		high = (uint32_t)(value >> 32);
		low = (uint32_t)value;
		if (high != low)
			tear_seen++;
	} while (low != TEAR_ROUNDS);

	/* Succeeded: the reader saw the last value. */
	return NULL;
}

/* Exchanges and compares on a few objects until told to stop. */
static void *
busy_thread(
	void *argument)
{
	uint64_t observed;
	uint64_t *object;

	object = argument;

	/* Keeps a lock word taken and given back as often as it can. */
	while (!busy_stop) {
		observed = __atomic_load_n(object, __ATOMIC_ACQUIRE);
		(void)__atomic_compare_exchange_n(object, &observed, observed + 1U, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE);
	}

	/* Succeeded: the thread stopped. */
	return NULL;
}

/* Counts through every width from several threads and checks the sums. */
static void
check_counts(
	void)
{
	pthread_t threads[COUNT_THREADS];
	uint64_t total;
	unsigned index;

	/* Starts the counting threads and waits for them. */
	for (index = 0; index < COUNT_THREADS; index++)
		check(pthread_create(&threads[index], NULL, count_thread, NULL) == 0, "pthread_create (count)");
	for (index = 0; index < COUNT_THREADS; index++)
		check(pthread_join(threads[index], NULL) == 0, "pthread_join (count)");

	/* Checks that no addition was lost, modulo each width. */
	total = (uint64_t)COUNT_THREADS * COUNT_ROUNDS;
	check(count_1 == (uint8_t)total, "1-byte fetch_add total");
	check(count_2 == (uint16_t)total, "2-byte fetch_add total");
	check(count_4 == (uint32_t)total, "4-byte fetch_add total");
	check(count_4_compare == (uint32_t)total, "4-byte compare_exchange total");
	check(count_8 == total, "8-byte fetch_add total");
	check(count_8_compare == total, "8-byte compare_exchange total");
	check(count_16.word[0] == (uint32_t)total, "16-byte compare_exchange word 0");
	check(count_16.word[3] == (uint32_t)total, "16-byte compare_exchange word 3");
	printf("counts: 1=%u 2=%u 4=%u 4c=%u 8=%llu 8c=%llu 16=%u (expect %llu)\n",
	    (unsigned)count_1,
	    (unsigned)count_2,
	    (unsigned)count_4,
	    (unsigned)count_4_compare,
	    (unsigned long long)count_8,
	    (unsigned long long)count_8_compare,
	    (unsigned)count_16.word[0],
	    (unsigned long long)total);
}

/* Watches an 8-byte object a writer stores for a value half old and half new. */
static void
check_tear(
	void)
{
	pthread_t writer;
	pthread_t reader;

	/* Runs the writer and the reader together. */
	check(pthread_create(&reader, NULL, tear_reader, NULL) == 0, "pthread_create (reader)");
	check(pthread_create(&writer, NULL, tear_writer, NULL) == 0, "pthread_create (writer)");
	check(pthread_join(writer, NULL) == 0, "pthread_join (writer)");
	check(pthread_join(reader, NULL) == 0, "pthread_join (reader)");

	/* Checks that every load saw a whole value. */
	check(tear_seen == 0U, "8-byte load saw no torn value");
	printf("tear: %u torn of %u stores\n", tear_seen, TEAR_ROUNDS);
}

/* Forks while other threads take and give back lock words, and checks each child finishes. */
static void
check_fork(
	void)
{
	static _Alignas(8) uint64_t busy_objects[COUNT_THREADS * 4U];
	pthread_t threads[COUNT_THREADS];
	uint64_t observed;
	unsigned index;
	unsigned round;
	unsigned slot;
	int status;
	pid_t child;
	unsigned finished;

	/* Starts threads that keep lock words busy on objects spread over the words. */
	busy_stop = 0;
	for (index = 0; index < COUNT_THREADS; index++)
		check(pthread_create(&threads[index], NULL, busy_thread, &busy_objects[index * 4U]) == 0, "pthread_create (busy)");

	/* Forks as fork() does, and has each child use every busy object once. */
	finished = 0;
	for (round = 0; round < FORK_ROUNDS; round++) {
		__libc_atomic_fork_prepare();
		child = fork();
		if (child == 0) {
			__libc_atomic_fork_child();
			(void)alarm(FORK_SECONDS);
			for (slot = 0; slot < COUNT_THREADS; slot++) {
				observed = __atomic_load_n(&busy_objects[slot * 4U], __ATOMIC_ACQUIRE);
				(void)__atomic_compare_exchange_n(&busy_objects[slot * 4U], &observed, observed + 1U, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE);
			}
			_exit(0);
		}
		__libc_atomic_fork_parent();
		check(child > 0, "fork");
		if (child <= 0)
			break;

		/* Checks that the child finished rather than hung on a word copied held. */
		check(waitpid(child, &status, 0) == child, "waitpid");
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			finished++;
	}

	/* Stops the busy threads. */
	busy_stop = 1;
	for (index = 0; index < COUNT_THREADS; index++)
		check(pthread_join(threads[index], NULL) == 0, "pthread_join (busy)");

	/* Checks that every child finished. */
	check(finished == FORK_ROUNDS, "every forked child finished");
	printf("fork: %u of %u children finished\n", finished, FORK_ROUNDS);
}
