/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/test-fault.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

struct worker_argument { uint32_t tid; unsigned hits; };

static int
worker(void *opaque)
{
	struct worker_argument *argument = opaque;
	unsigned attempt;
	for (attempt = 0; attempt < 32U; attempt++) {
		struct kern_test_fault_result result;
		if (KERN_TEST_FAULT(KERN_TEST_FAULT_NET_PACKET_ALLOC, 3,
		    argument->tid, &result)) {
			assert(result.error == ENOMEM && result.short_count == 7U);
			argument->hits++;
		}
	}
	return 0;
}

int
main(void)
{
	struct kern_test_fault_config config;
	struct kern_test_fault_log_entry log[2];
	struct worker_argument arguments[2] = {{11, 0}, {12, 0}};
	thrd_t threads[2];

	kern_test_fault_reset();
	memset(&config, 0, sizeof(config));
	config.id = KERN_TEST_FAULT_NET_PACKET_ALLOC;
	config.fail_at = 17;
	config.cpu = 3;
	config.tid = KERN_TEST_FAULT_ANY_CONTEXT;
	config.error = ENOMEM;
	config.short_count = 7;
	assert(kern_test_fault_configure(&config) == 0);
	assert(thrd_create(&threads[0], worker, &arguments[0]) == thrd_success);
	assert(thrd_create(&threads[1], worker, &arguments[1]) == thrd_success);
	assert(thrd_join(threads[0], NULL) == thrd_success);
	assert(thrd_join(threads[1], NULL) == thrd_success);
	assert(arguments[0].hits + arguments[1].hits == 1U);
	assert(kern_test_fault_log(log, 2) == 1U);
	assert(log[0].sequence == 1 && log[0].ordinal == 17 &&
	    log[0].id == KERN_TEST_FAULT_NET_PACKET_ALLOC &&
	    log[0].error == ENOMEM && log[0].short_count == 7U);
	puts("zedBSD deterministic fault injection test: PASS");
	return 0;
}
