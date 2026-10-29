/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of libkeiland's threads (<keiland/keiland_thread.h>):
 * mutexes under contention, conditions between a producer and consumers, a
 * timed wait, a call made once, the threads' values and their destructors,
 * join and detach, the static initializers, the name and the limits of the
 * arguments.  It prints PASS or FAIL for each check and a SUMMARY line, and
 * exits 0 only when every check passed.  Build it against libkeiland and,
 * for the races, with -fsanitize=thread (thread-test.sh).
 */

#include <keiland/keiland_thread.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The threads that add under one mutex, and how many times each adds. */
#define TEST_ADDERS		8U
#define TEST_ADDS		100000U

/* The items the producer makes, the consumers that take them, and the room between them. */
#define TEST_ITEMS		20000U
#define TEST_CONSUMERS		4U
#define TEST_RING		16U

/* The threads that pass one once and set one key. */
#define TEST_RACERS		8U

/* The timed wait and the sleep (ms), and how late either may end (ms). */
#define TEST_WAIT_MS		50U
#define TEST_SLEEP_MS		20U
#define TEST_LATE_MS		2000U

/* What the checks counted. */
struct test_counts {
	unsigned passed;
	unsigned failed;
};

/* The sum the adders build under one mutex. */
struct test_sum {
	struct kl_mutex lock;
	unsigned long total;
};

/*
 * The ring between the producer and the consumers: the items waiting, the
 * conditions for room and for an item, how many were made and taken, the
 * sum of the values taken, and whether the producer is done.
 */
struct test_ring {
	struct kl_mutex lock;
	struct kl_cond room;
	struct kl_cond item;
	unsigned values[TEST_RING];
	unsigned head;
	unsigned count;
	unsigned made;
	unsigned taken;
	unsigned long long sum;
	int done;
};

/*
 * What a thread that joins itself is handed: its own record (set by the
 * starter once kl_thread_start returned), and what kl_thread_join told it.
 */
struct test_self {
	struct kl_mutex lock;
	struct kl_cond ready;
	struct kl_thread *thread;
	int answer;
};

/* The results of the checks. */
static struct test_counts test_counts;

/* A mutex of static storage: KL_MUTEX_INIT makes it ready without kl_mutex_init. */
static struct kl_mutex test_static_lock = KL_MUTEX_INIT;

/* The once the racers pass, and how many times its call ran (once, if kl_once holds). */
static struct kl_once test_once = KL_ONCE_INIT;
static atomic_uint test_once_runs;

/* The key the racers set, and how many values its destructor was given. */
static struct kl_thread_key test_key;
static atomic_uint test_key_destroyed;

/* Whether the detached thread ran to its end. */
static atomic_int test_detached_ran;

/* The name the named thread read back for itself (Linux). */
static char test_name_read[32];

static void test_check(int passed, const char *name);
static uint64_t test_now_ms(void);
static void test_add(void *argument);
static void test_produce(void *argument);
static void test_consume(void *argument);
static void test_once_call(void);
static void test_once_race(void *argument);
static void test_key_destroy(void *value);
static void test_key_race(void *argument);
static void test_detached(void *argument);
static void test_join_self(void *argument);
static void test_hold(void *argument);
static void test_name(void *argument);
static void test_nothing(void *argument);
static void test_mutexes(void);
static void test_conditions(void);
static void test_timed_wait(void);
static void test_once_and_keys(void);
static void test_join_and_detach(void);
static void test_static_and_limits(void);

/*
 * Runs every check and reports.
 */
int
main(void)
{
	/* The checks, one group at a time. */
	test_mutexes();
	test_conditions();
	test_timed_wait();
	test_once_and_keys();
	test_join_and_detach();
	test_static_and_limits();

	/* The summary. */
	printf("SUMMARY pass=%u fail=%u\n", test_counts.passed, test_counts.failed);

	/* Failed when any check did. */
	if (test_counts.failed != 0U)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Prints a check's result and counts it. */
static void
test_check(
	int passed,
	const char *name)
{
	/* PASS or FAIL with the check's name. */
	if (passed) {
		printf("PASS %s\n", name);
		test_counts.passed++;
	} else {
		printf("FAIL %s\n", name);
		test_counts.failed++;
	}
}

/* Reads the monotonic clock in milliseconds. */
static uint64_t
test_now_ms(void)
{
	struct timespec now;

	/* The clock that only goes forward. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Adds to the shared sum under its mutex, many times. */
static void
test_add(
	void *argument)
{
	struct test_sum *sum;
	unsigned index;

	/* The sum the adders share. */
	sum = argument;

	/* One at a time under the mutex. */
	for (index = 0; index < TEST_ADDS; index++) {
		kl_mutex_lock(&sum->lock);

		sum->total++;

		kl_mutex_unlock(&sum->lock);
	}
}

/* Makes the items 1..TEST_ITEMS, waiting for room in the ring. */
static void
test_produce(
	void *argument)
{
	struct test_ring *ring;
	unsigned value;

	/* The ring the consumers take from. */
	ring = argument;

	/* Each item into the ring, waiting while it is full. */
	for (value = 1U; value <= TEST_ITEMS; value++) {
		kl_mutex_lock(&ring->lock);

		while (ring->count == TEST_RING)
			kl_cond_wait(&ring->room, &ring->lock);
		ring->values[(ring->head + ring->count) % TEST_RING] = value;
		ring->count++;
		ring->made++;
		kl_cond_signal(&ring->item);

		kl_mutex_unlock(&ring->lock);
	}

	/* Done: the consumers that wait for more are woken to see it. */
	kl_mutex_lock(&ring->lock);

	ring->done = 1;
	kl_cond_broadcast(&ring->item);

	kl_mutex_unlock(&ring->lock);
}

/* Takes items from the ring until the producer is done and the ring is empty. */
static void
test_consume(
	void *argument)
{
	struct test_ring *ring;
	unsigned value;

	/* The ring the producer fills. */
	ring = argument;

	/* An item at a time, waiting while the ring is empty and the producer goes on. */
	for (;;) {
		kl_mutex_lock(&ring->lock);

		while (ring->count == 0U && !ring->done)
			kl_cond_wait(&ring->item, &ring->lock);
		if (ring->count == 0U) {
			kl_mutex_unlock(&ring->lock);
			break;
		}
		value = ring->values[ring->head];
		ring->head = (ring->head + 1U) % TEST_RING;
		ring->count--;
		ring->taken++;
		ring->sum += value;
		kl_cond_signal(&ring->room);

		kl_mutex_unlock(&ring->lock);
	}
}

/* The call the once makes: counts its runs. */
static void
test_once_call(void)
{
	/* One run more (one in all when kl_once holds). */
	(void)atomic_fetch_add(&test_once_runs, 1U);
}

/* Passes the once. */
static void
test_once_race(
	void *argument)
{
	(void)argument;

	/* The call, made by whichever racer comes first. */
	kl_once(&test_once, test_once_call);
}

/* The key's destructor: counts the values it is given. */
static void
test_key_destroy(
	void *value)
{
	/* One value of an ended thread (the racers' are their own records). */
	free(value);
	(void)atomic_fetch_add(&test_key_destroyed, 1U);
}

/* Sets the key to a value of the thread's own, and reads it back. */
static void
test_key_race(
	void *argument)
{
	unsigned *value;
	unsigned *read;
	int error;

	/* A value of this thread's own (its index), freed by the destructor as the thread ends. */
	value = malloc(sizeof(*value));
	if (value == NULL)
		return;
	*value = *(unsigned *)argument;

	/* The value set and read back as this thread's. */
	error = kl_thread_key_set(&test_key, value);
	if (error != 0) {
		free(value);
		return;
	}
	read = kl_thread_key_get(&test_key);
	if (read != value || *read != *(unsigned *)argument)
		*(unsigned *)argument = 0U;
}

/* The detached thread: sleeps a little and tells it ran. */
static void
test_detached(
	void *argument)
{
	(void)argument;

	/* Outlives its start, then says so. */
	kl_thread_sleep(10U);
	atomic_store(&test_detached_ran, 1);
}

/* Waits to be handed its own record, then tries to join itself. */
static void
test_join_self(
	void *argument)
{
	struct test_self *self;
	struct kl_thread *thread;
	int answer;

	/* The record, once the starter has it. */
	self = argument;
	kl_mutex_lock(&self->lock);

	while (self->thread == NULL)
		kl_cond_wait(&self->ready, &self->lock);
	thread = self->thread;

	kl_mutex_unlock(&self->lock);

	/* A thread does not join itself (EDEADLK), nor is it current to another. */
	answer = kl_thread_join(thread);
	if (answer == EDEADLK) {
		answer = kl_thread_is_current(thread);
		if (answer != 1)
			answer = -1;
		else
			answer = EDEADLK;
	}

	/* What it was told. */
	kl_mutex_lock(&self->lock);

	self->answer = answer;

	kl_mutex_unlock(&self->lock);
}

/* Holds the static mutex for a while (another thread's kl_mutex_try then fails). */
static void
test_hold(
	void *argument)
{
	atomic_int *holding;

	/* Held, told, and kept until told to let go. */
	holding = argument;
	kl_mutex_lock(&test_static_lock);

	atomic_store(holding, 1);
	while (atomic_load(holding) == 1)
		kl_thread_yield();

	kl_mutex_unlock(&test_static_lock);
}

/* Reads its own name back where the system keeps one. */
static void
test_name(
	void *argument)
{
	(void)argument;

	/* The name the system shows for this thread. */
#if defined(__linux__)
	(void)pthread_getname_np(pthread_self(), test_name_read, sizeof(test_name_read));
#endif
}

/* Runs nothing. */
static void
test_nothing(
	void *argument)
{
	(void)argument;
}

/* Checks that a mutex keeps the adders' sum whole. */
static void
test_mutexes(void)
{
	struct kl_thread_options options;
	struct kl_thread *threads[TEST_ADDERS];
	struct test_sum sum;
	unsigned started;
	unsigned index;
	int error;

	/* The sum's mutex. */
	memset(&sum, 0, sizeof(sum));
	error = kl_mutex_init(&sum.lock);
	test_check(error == 0, "mutex-init");
	if (error != 0)
		return;

	/* The adders. */
	memset(&options, 0, sizeof(options));
	options.name = "test-adder";
	options.run = test_add;
	options.argument = &sum;
	started = 0;
	for (index = 0; index < TEST_ADDERS; index++) {
		error = kl_thread_start(&options, &threads[index]);
		if (error != 0)
			break;
		started++;
	}
	test_check(started == TEST_ADDERS, "thread-start");

	/* Each adder's end. */
	for (index = 0; index < started; index++) {
		error = kl_thread_join(threads[index]);
		if (error != 0)
			started = 0;
	}

	/* Every addition kept. */
	test_check(sum.total == (unsigned long)TEST_ADDERS * TEST_ADDS, "mutex-sum");
	kl_mutex_destroy(&sum.lock);
}

/* Checks that the conditions hand every item from the producer to exactly one consumer. */
static void
test_conditions(void)
{
	struct kl_thread_options options;
	struct kl_thread *producer;
	struct kl_thread *consumers[TEST_CONSUMERS];
	struct test_ring *ring;
	unsigned started;
	unsigned index;
	int error;

	/* The ring and its mutex and conditions. */
	ring = calloc(1U, sizeof(*ring));
	if (ring == NULL) {
		test_check(0, "cond-init");
		return;
	}
	error = kl_mutex_init(&ring->lock);
	if (error == 0)
		error = kl_cond_init(&ring->room);
	if (error == 0)
		error = kl_cond_init(&ring->item);
	test_check(error == 0, "cond-init");
	if (error != 0) {
		free(ring);
		return;
	}

	/* The consumers, then the producer. */
	memset(&options, 0, sizeof(options));
	options.name = "test-consumer";
	options.run = test_consume;
	options.argument = ring;
	started = 0;
	for (index = 0; index < TEST_CONSUMERS; index++) {
		error = kl_thread_start(&options, &consumers[index]);
		if (error != 0)
			break;
		started++;
	}
	options.name = "test-producer";
	options.run = test_produce;
	error = kl_thread_start(&options, &producer);
	test_check(error == 0 && started == TEST_CONSUMERS, "cond-start");
	if (error == 0)
		(void)kl_thread_join(producer);
	else {
		kl_mutex_lock(&ring->lock);

		ring->done = 1;
		kl_cond_broadcast(&ring->item);

		kl_mutex_unlock(&ring->lock);
	}

	/* The consumers' ends. */
	for (index = 0; index < started; index++)
		(void)kl_thread_join(consumers[index]);

	/* Every item made was taken once: the count and the sum 1 + ... + TEST_ITEMS. */
	test_check(ring->made == TEST_ITEMS && ring->taken == TEST_ITEMS, "cond-count");
	test_check(ring->sum == (unsigned long long)TEST_ITEMS * (TEST_ITEMS + 1U) / 2U, "cond-sum");
	kl_cond_destroy(&ring->item);
	kl_cond_destroy(&ring->room);
	kl_mutex_destroy(&ring->lock);
	free(ring);
}

/* Checks that a timed wait nobody signals ends with ETIMEDOUT in time, and that a sleep lasts. */
static void
test_timed_wait(void)
{
	struct kl_mutex lock;
	struct kl_cond cond;
	uint64_t start;
	uint64_t waited;
	int error;

	/* A condition nobody signals. */
	error = kl_mutex_init(&lock);
	if (error == 0)
		error = kl_cond_init(&cond);
	if (error != 0) {
		test_check(0, "wait-for-init");
		return;
	}

	/* A wait that times out; a wait that ended for nothing is waited again. */
	start = test_now_ms();
	kl_mutex_lock(&lock);

	do {
		error = kl_cond_wait_for(&cond, &lock, TEST_WAIT_MS);
		waited = test_now_ms() - start;
	} while (error == 0 && waited < TEST_WAIT_MS);

	kl_mutex_unlock(&lock);

	/* Timed out, not early and not much later. */
	test_check(error == ETIMEDOUT, "wait-for-timedout");
	test_check(waited >= TEST_WAIT_MS && waited < TEST_WAIT_MS + TEST_LATE_MS, "wait-for-time");
	kl_cond_destroy(&cond);
	kl_mutex_destroy(&lock);

	/* A sleep lasts at least its time. */
	start = test_now_ms();
	kl_thread_sleep(TEST_SLEEP_MS);
	waited = test_now_ms() - start;
	test_check(waited >= TEST_SLEEP_MS && waited < TEST_SLEEP_MS + TEST_LATE_MS, "sleep-time");
}

/* Checks that a once runs once among racers, and that each racer's key value is its own and destroyed. */
static void
test_once_and_keys(void)
{
	struct kl_thread_options options;
	struct kl_thread *threads[TEST_RACERS];
	unsigned indexes[TEST_RACERS];
	unsigned started;
	unsigned index;
	unsigned own;
	int error;

	/* The racers on the once. */
	memset(&options, 0, sizeof(options));
	options.run = test_once_race;
	started = 0;
	for (index = 0; index < TEST_RACERS; index++) {
		error = kl_thread_start(&options, &threads[index]);
		if (error != 0)
			break;
		started++;
	}
	for (index = 0; index < started; index++)
		(void)kl_thread_join(threads[index]);
	kl_once(&test_once, test_once_call);
	test_check(started == TEST_RACERS && atomic_load(&test_once_runs) == 1U, "once");

	/* The key, and the racers that set it. */
	error = kl_thread_key_create(&test_key, test_key_destroy);
	test_check(error == 0, "key-create");
	if (error != 0)
		return;
	options.run = test_key_race;
	started = 0;
	for (index = 0; index < TEST_RACERS; index++) {
		indexes[index] = index + 1U;
		options.argument = &indexes[index];
		error = kl_thread_start(&options, &threads[index]);
		if (error != 0)
			break;
		started++;
	}
	for (index = 0; index < started; index++)
		(void)kl_thread_join(threads[index]);

	/* Each racer read its own value, the destructor took every one, and this thread has none. */
	own = 1U;
	for (index = 0; index < started; index++) {
		if (indexes[index] != index + 1U)
			own = 0U;
	}
	test_check(started == TEST_RACERS && own == 1U, "key-own");
	test_check(atomic_load(&test_key_destroyed) == TEST_RACERS, "key-destroy");
	test_check(kl_thread_key_get(&test_key) == NULL, "key-unset");
	kl_thread_key_delete(&test_key);
}

/* Checks a detached thread, a thread that joins itself, and kl_thread_is_current. */
static void
test_join_and_detach(void)
{
	struct kl_thread_options options;
	struct kl_thread *thread;
	struct test_self self;
	uint64_t start;
	int error;

	/* A detached thread runs on and ends by itself (its record goes then; a leak check sees it). */
	memset(&options, 0, sizeof(options));
	options.name = "test-detached";
	options.run = test_detached;
	error = kl_thread_start(&options, &thread);
	if (error == 0)
		error = kl_thread_detach(thread);
	start = test_now_ms();
	while (atomic_load(&test_detached_ran) == 0 && test_now_ms() - start < TEST_LATE_MS)
		kl_thread_sleep(1U);
	test_check(error == 0 && atomic_load(&test_detached_ran) == 1, "detach");

	/* A thread handed its own record cannot join itself and is current to itself. */
	memset(&self, 0, sizeof(self));
	error = kl_mutex_init(&self.lock);
	if (error == 0)
		error = kl_cond_init(&self.ready);
	if (error != 0) {
		test_check(0, "join-self");
		return;
	}
	options.name = "test-self";
	options.run = test_join_self;
	options.argument = &self;
	error = kl_thread_start(&options, &thread);
	if (error != 0) {
		test_check(0, "join-self");
		return;
	}
	test_check(kl_thread_is_current(thread) == 0, "current-other");
	kl_mutex_lock(&self.lock);

	self.thread = thread;
	kl_cond_signal(&self.ready);

	kl_mutex_unlock(&self.lock);
	error = kl_thread_join(thread);
	test_check(error == 0 && self.answer == EDEADLK, "join-self");
	kl_cond_destroy(&self.ready);
	kl_mutex_destroy(&self.lock);
}

/* Checks the static initializers, kl_mutex_try, the name, the priorities and the arguments' limits. */
static void
test_static_and_limits(void)
{
	static const pthread_mutex_t system_mutex = PTHREAD_MUTEX_INITIALIZER;
	static const pthread_once_t system_once = PTHREAD_ONCE_INIT;
	static const unsigned char zero[sizeof(pthread_mutex_t)];
	struct kl_thread_options options;
	struct kl_thread *thread;
	atomic_int holding;
	int error;

	/* The system's static initializers are all zero bytes, which KL_MUTEX_INIT and KL_ONCE_INIT give. */
	test_check(memcmp(&system_mutex, zero, sizeof(system_mutex)) == 0, "static-mutex-zero");
	test_check(memcmp(&system_once, zero, sizeof(system_once)) == 0, "static-once-zero");

	/* The static mutex: free, then held by another thread (kl_mutex_try fails), then free again. */
	error = kl_mutex_try(&test_static_lock);
	if (error == 0)
		kl_mutex_unlock(&test_static_lock);
	test_check(error == 0, "try-free");
	memset(&options, 0, sizeof(options));
	options.run = test_hold;
	options.argument = &holding;
	atomic_init(&holding, 0);
	error = kl_thread_start(&options, &thread);
	if (error == 0) {
		while (atomic_load(&holding) == 0)
			kl_thread_yield();
		error = kl_mutex_try(&test_static_lock);
		atomic_store(&holding, 2);
		(void)kl_thread_join(thread);
	}
	test_check(error == EBUSY, "try-held");

	/* A long name is cut and kept (Linux reads it back), at a low priority; and one at a high priority starts too. */
	options.name = "test-a-name-longer-than-fifteen";
	options.run = test_name;
	options.argument = NULL;
	options.priority = KL_THREAD_PRIORITY_LOW;
	options.stack_size = 256U * 1024U;
	error = kl_thread_start(&options, &thread);
	if (error == 0)
		error = kl_thread_join(thread);
	test_check(error == 0, "start-low");
#if defined(__linux__)
	test_check(strcmp(test_name_read, "test-a-name-lon") == 0, "name");
#endif
	options.name = NULL;
	options.run = test_nothing;
	options.priority = KL_THREAD_PRIORITY_HIGH;
	options.stack_size = 0U;
	error = kl_thread_start(&options, &thread);
	if (error == 0)
		error = kl_thread_join(thread);
	test_check(error == 0, "start-high");

	/* No run, or an unknown priority, is refused. */
	options.run = NULL;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	error = kl_thread_start(&options, &thread);
	test_check(error == EINVAL && thread == NULL, "start-no-run");
	options.run = test_nothing;
	options.priority = KL_THREAD_PRIORITY_HIGH + 1U;
	error = kl_thread_start(&options, &thread);
	test_check(error == EINVAL && thread == NULL, "start-bad-priority");

	/* At least one processor. */
	test_check(kl_thread_cpu_count() >= 1U, "cpu-count");
}
