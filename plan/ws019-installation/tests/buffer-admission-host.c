/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define main buffer_regression_main
#include "plan/ws025-io-memory-cache/tests/buffer-run-host.c"
#undef main

static unsigned admission_errors[4];

static void
admission_reader(void *argument)
{
	struct buf *buffer;
	unsigned worker;
	unsigned iteration;
	unsigned block;
	int error;

	worker = (unsigned)(uintptr_t)argument;
	for (iteration = 0; iteration < 5000; iteration++) {
		block = ((iteration * 13U + worker * 17U) % 64U) * 8U;
		error = reference_line(&device, block, &buffer);
		if (error != 0) {
			assert(error == ENOMEM);
			admission_errors[worker]++;
			continue;
		}
		host_thread_yield();
		drop_caller_reference(buffer);
	}
}

int
main(void)
{
	void *workers[4];
	struct bufcache_stats stats;
	unsigned worker;
	unsigned failures;

	assert(buffer_regression_main() == 0);
	check_preparation = 0;
	assert(buf_set_max_bytes(65536) == 0);
	for (worker = 0; worker < 4; worker++)
		workers[worker] = host_thread_start(admission_reader, (void *)(uintptr_t)worker);
	for (worker = 0; worker < 4; worker++)
		host_thread_join(workers[worker]);
	buf_get_stats(&stats);
	assert(stats.current_bytes <= stats.max_bytes);
	assert(cache_reserved_bytes == 0);
	failures = 0;
	for (worker = 0; worker < 4; worker++)
		failures += admission_errors[worker];
	printf("concurrent clean admission: %u failures across 20000 reads\n", failures);
	buf_reset();
	assert(live_pages == 1);
	return failures != 0;
}
