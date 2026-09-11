/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Fault injection for test builds.
 *
 * A test configures one fault point to fail on its n-th hit, optionally
 * only on a given CPU or thread.  Every injected fault is recorded in a
 * bounded log that the test reads back.  Production builds compile the
 * whole facility out.
 */

#include "kern/test-fault.h"

#ifdef KERN_TEST_FAULTS
#include <errno.h>
#include <kern/atomic.h>
#include <string.h>

static struct kern_test_fault_config configured;
static struct kern_test_fault_log_entry log_entries[KERN_TEST_FAULT_LOG_CAPACITY];
static uint64_t point_ordinals[KERN_TEST_FAULT_COUNT];
static uint64_t sequence;
static uint32_t log_head;
static uint32_t log_count;
static atomic_uint_t guard;

static void fault_lock(void);
static void fault_unlock(void);

/*
 * Clears the configured fault, the hit counters, and the log.
 */
void
kern_test_fault_reset(
	void)
{
	/* Forgets every configured fault and every recorded hit. */
	fault_lock();
	memset(&configured, 0, sizeof(configured));
	memset(point_ordinals, 0, sizeof(point_ordinals));
	memset(log_entries, 0, sizeof(log_entries));
	sequence = 0;
	log_head = 0;
	log_count = 0;
	fault_unlock();
}

/*
 * Configures the one fault to inject and restarts the hit counters.
 */
int
kern_test_fault_configure(
	const struct kern_test_fault_config *config)
{
	/* Rejects an unknown point, an impossible hit count, or a bad error. */
	if (config == NULL ||
	    config->id <= KERN_TEST_FAULT_NONE ||
	    config->id >= KERN_TEST_FAULT_COUNT ||
	    config->fail_at == 0 ||
	    config->error < 0)
		return EINVAL;

	/* Installs the configuration with fresh hit counters. */
	fault_lock();
	configured = *config;
	memset(point_ordinals, 0, sizeof(point_ordinals));
	fault_unlock();

	/* Reports the installed configuration. */
	return 0;
}

/*
 * Counts one hit of a fault point and reports whether to inject the fault.
 *
 * The injected error and short count are returned through result when it
 * is given, and every injection is appended to the log.
 */
int
kern_test_fault_hit(
	enum kern_test_fault_id id,
	uint32_t cpu,
	uint32_t tid,
	struct kern_test_fault_result *result)
{
	struct kern_test_fault_log_entry *entry;
	uint64_t ordinal;
	int inject;

	/* Ignores an unknown point. */
	if (id <= KERN_TEST_FAULT_NONE || id >= KERN_TEST_FAULT_COUNT)
		return 0;

	fault_lock();

	/* Injects only the configured hit of the configured point. */
	ordinal = ++point_ordinals[id];
	inject = 0;
	if (configured.id == id &&
	    configured.fail_at == ordinal &&
	    (configured.cpu == KERN_TEST_FAULT_ANY_CONTEXT ||
	     configured.cpu == cpu) &&
	    (configured.tid == KERN_TEST_FAULT_ANY_CONTEXT ||
	     configured.tid == tid))
		inject = 1;

	/* Records the injection and reports its effect. */
	if (inject) {
		entry = &log_entries[log_head];
		memset(entry, 0, sizeof(*entry));
		entry->sequence = ++sequence;
		entry->ordinal = ordinal;
		entry->id = (uint32_t)id;
		entry->cpu = cpu;
		entry->tid = tid;
		entry->error = configured.error;
		entry->short_count = configured.short_count;
		log_head = (log_head + 1U) % KERN_TEST_FAULT_LOG_CAPACITY;
		if (log_count < KERN_TEST_FAULT_LOG_CAPACITY)
			log_count++;
		if (result != NULL) {
			result->error = configured.error;
			result->short_count = configured.short_count;
		}
	}

	fault_unlock();

	/* Reports whether the fault was injected. */
	return inject;
}

/*
 * Copies the oldest recorded injections into a caller buffer.
 */
size_t
kern_test_fault_log(
	struct kern_test_fault_log_entry *output,
	size_t capacity)
{
	uint32_t first;
	uint32_t index;
	size_t count;

	/* Rejects a capacity without a buffer. */
	if (output == NULL && capacity != 0)
		return 0;

	fault_lock();

	/* Copies as many entries as fit, oldest first. */
	count = capacity;
	if (log_count < capacity)
		count = log_count;
	first = (log_head + KERN_TEST_FAULT_LOG_CAPACITY - log_count) %
	    KERN_TEST_FAULT_LOG_CAPACITY;
	for (index = 0; index < count; index++)
		output[index] = log_entries[(first + index) % KERN_TEST_FAULT_LOG_CAPACITY];

	fault_unlock();

	/* Reports the number of copied entries. */
	return count;
}

/* Acquires the fault state spin guard. */
static void
fault_lock(
	void)
{
	/* Spins until the guard is free. */
	while (!atomic_try_acquire_zero(&guard))
		;
}

/* Releases the fault state spin guard. */
static void
fault_unlock(
	void)
{
	atomic_store_release(&guard, 0U);
}
#endif
