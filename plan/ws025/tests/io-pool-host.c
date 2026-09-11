/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include <kern/io-pool.h>
#include <kern/io-stats.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "io-pool-hal-host.h"

static pthread_mutex_t oracle = PTHREAD_MUTEX_INITIALIZER;
static void *borrowed[8];

static void *worker(void *argument)
{
	unsigned id = (uintptr_t)argument;
	unsigned round, index;
	size_t capacity, offset;
	unsigned char *buffer;

	for (round = 0; round < 1000; round++) {
		capacity = 512;
		buffer = io_pool_borrow(KERN_IO_BATCH_MAX, &capacity);
		assert(buffer && capacity == KERN_IO_BATCH_MAX);
		pthread_mutex_lock(&oracle);
		for (index = 0; index < 8; index++) assert(borrowed[index] != buffer);
		borrowed[id] = buffer;
		pthread_mutex_unlock(&oracle);
		memset(buffer, id + 1, capacity);
		sched_yield();
		for (offset = 0; offset < capacity; offset += 4096) assert(buffer[offset] == id + 1);
		assert(buffer[capacity - 1] == id + 1);
		pthread_mutex_lock(&oracle);
		borrowed[id] = NULL;
		pthread_mutex_unlock(&oracle);
		io_pool_release(buffer);
	}
	return NULL;
}

static void run_case(unsigned failure, size_t memory, size_t page, unsigned cpu_count)
{
	struct io_pool_stats stats;
	struct io_stats before, after;
	void *held[320];
	size_t capacity;
	unsigned count, saved, index;
	pthread_t threads[8];

	fail_at = failure; ram = memory; page_size = page; cpus = cpu_count;
	io_pool_init();
	io_pool_get_stats(&stats);
	assert(stats.resident_bytes == live_bytes && live_bytes <= stats.budget_bytes);
	assert(stats.budget_bytes <= ram / 64 && stats.budget_bytes <= 4U * 1024U * 1024U);
	saved = allocation_calls;
	io_stats_snapshot(&before);
	count = 0;
	while (count < 320) {
		capacity = 512;
		held[count] = io_pool_borrow(KERN_IO_BATCH_MAX, &capacity);
		if (!held[count]) { assert(capacity == 512); break; }
		assert(capacity == (count < stats.large_count ? KERN_IO_BATCH_MAX : KERN_IO_SMALL_SIZE));
		for (index = 0; index < count; index++) assert(held[index] != held[count]);
		count++;
	}
	assert(count == stats.large_count + stats.small_count);
	io_pool_get_stats(&stats);
	assert(stats.in_use == count);
	for (index = 0; index < count; index++) io_pool_release(held[index]);
	io_pool_get_stats(&stats);
	assert(stats.in_use == 0 && allocation_calls == saved);
	io_stats_snapshot(&after);
	assert(after.events[IO_POOL_BACKING_ALLOC].calls == before.events[IO_POOL_BACKING_ALLOC].calls);
	assert(after.events[IO_POOL_RETURN].calls - before.events[IO_POOL_RETURN].calls == count);

	if (failure == 0 && memory == 256U * 1024U * 1024U && page == 4096 && cpus == 4) {
		assert(stats.large_count == 16 && stats.small_count == 16);
		for (index = 0; index < 8; index++) assert(pthread_create(&threads[index], NULL, worker, (void *)(uintptr_t)index) == 0);
		for (index = 0; index < 8; index++) assert(pthread_join(threads[index], NULL) == 0);
		io_pool_get_stats(&stats);
		assert(stats.in_use == 0 && allocation_calls == saved);
	}
}

int main(void)
{
	unsigned index;
	int status;
	pid_t child;

	/* Each fresh child owns one permanent pool, matching one kernel boot. */
	for (index = 0; index < 40; index++) {
		child = fork();
		assert(child >= 0);
		if (child == 0) {
			if (index < 35) run_case(index, 256U * 1024U * 1024U, 4096, 4);
			else if (index == 35) run_case(0, 8U * 1024U * 1024U, 4096, 4);
			else if (index == 36) run_case(0, 1024U * 1024U, 8192, 4);
			else if (index == 37) run_case(0, 256U * 1024U * 1024U, 8192, 4);
			else if (index == 38) run_case(0, 256U * 1024U * 1024U, 4096, 256);
			else run_case(0, 0, 4096, 1);
			exit(0);
		}
		assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	puts("PASS: I/O pool 40 capacity/failure boots, complete exhaustion and 8-thread ownership; warm borrow performs zero allocations");
	return 0;
}
