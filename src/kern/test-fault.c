/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/test-fault.h"

#ifdef ZEDBSD_TEST_FAULTS
#include <errno.h>
#include <string.h>

static struct kern_test_fault_config configured;
static struct kern_test_fault_log_entry log_entries[KERN_TEST_FAULT_LOG_CAPACITY];
static uint64_t point_ordinals[KERN_TEST_FAULT_COUNT];
static uint64_t sequence;
static uint32_t log_head;
static uint32_t log_count;
static uint32_t guard;

static void
fault_lock(void)
{
	while (__atomic_exchange_n(&guard, 1U, __ATOMIC_ACQUIRE) != 0U)
		;
}

static void
fault_unlock(void)
{
	__atomic_store_n(&guard, 0U, __ATOMIC_RELEASE);
}

void
kern_test_fault_reset(void)
{
	fault_lock();
	memset(&configured, 0, sizeof(configured));
	memset(point_ordinals, 0, sizeof(point_ordinals));
	memset(log_entries, 0, sizeof(log_entries));
	sequence = 0;
	log_head = 0;
	log_count = 0;
	fault_unlock();
}

int
kern_test_fault_configure(const struct kern_test_fault_config *config)
{
	if (config == NULL || config->id <= KERN_TEST_FAULT_NONE ||
	    config->id >= KERN_TEST_FAULT_COUNT || config->fail_at == 0 ||
	    config->error < 0)
		return EINVAL;
	fault_lock();
	configured = *config;
	memset(point_ordinals, 0, sizeof(point_ordinals));
	fault_unlock();
	return 0;
}

int
kern_test_fault_hit(enum kern_test_fault_id id, uint32_t cpu, uint32_t tid,
	struct kern_test_fault_result *result)
{
	struct kern_test_fault_log_entry *entry;
	uint64_t ordinal;
	int inject;

	if (id <= KERN_TEST_FAULT_NONE || id >= KERN_TEST_FAULT_COUNT)
		return 0;
	fault_lock();
	ordinal = ++point_ordinals[id];
	inject = configured.id == id && configured.fail_at == ordinal &&
	    (configured.cpu == KERN_TEST_FAULT_ANY_CONTEXT ||
	    configured.cpu == cpu) &&
	    (configured.tid == KERN_TEST_FAULT_ANY_CONTEXT ||
	    configured.tid == tid);
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
	return inject;
}

size_t
kern_test_fault_log(struct kern_test_fault_log_entry *output, size_t capacity)
{
	uint32_t first, index;
	size_t count;

	if (output == NULL && capacity != 0)
		return 0;
	fault_lock();
	count = log_count < capacity ? log_count : capacity;
	first = (log_head + KERN_TEST_FAULT_LOG_CAPACITY - log_count) %
	    KERN_TEST_FAULT_LOG_CAPACITY;
	for (index = 0; index < count; index++)
		output[index] = log_entries[(first + index) %
		    KERN_TEST_FAULT_LOG_CAPACITY];
	fault_unlock();
	return count;
}
#endif
