/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/io-error.h>
#include <hal/hal.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static struct io_error_state state;
static unsigned observations;
static unsigned done;
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
static void *writer(void *argument)
{
	unsigned i;
	(void)argument;
	for (i = 1; i <= 100000; i++)
		io_error_record(&state, (i & 1U) ? EIO : ENOSPC);
	__atomic_store_n(&done, 1, __ATOMIC_RELEASE);
	return NULL;
}
static void *reader(void *argument)
{
	struct io_error_snapshot snapshot;
	uint64_t cursor = 0;
	unsigned count = 0;
	(void)argument;
	do {
		io_error_snapshot(&state, &snapshot);
		if (snapshot.sequence != 0)
			assert(snapshot.error == ((snapshot.sequence & 1U) ? EIO : ENOSPC));
		if (io_error_observe(&snapshot, &cursor) != 0) count++;
	} while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE));
	io_error_snapshot(&state, &snapshot);
	(void)io_error_observe(&snapshot, &cursor);
	assert(cursor == 100000);
	__atomic_add_fetch(&observations, count, __ATOMIC_RELAXED);
	return NULL;
}
int main(void)
{
	struct io_error_snapshot old, newer;
	uint64_t first = 0, second = 0;
	pthread_t producer, consumers[4];
	unsigned i;

	io_error_snapshot(&state, &old);
	assert(io_error_observe(&old, &first) == 0);
	io_error_record(&state, EIO);
	io_error_snapshot(&state, &old);
	io_error_record(&state, ENOSPC);
	assert(io_error_observe(&old, &first) == EIO && first == 1);
	assert(io_error_observe(&old, &first) == 0);
	assert(io_error_observe(&old, &second) == EIO && second == 1);
	io_error_snapshot(&state, &newer);
	assert(io_error_observe(&newer, &first) == ENOSPC && first == 2);
	assert(io_error_observe(&old, &first) == 0 && first == 2);
	io_error_record(&state, 0);
	assert(io_error_observe(&newer, &second) == ENOSPC);
	state.sequence = UINT64_MAX - 1U;
	io_error_record(&state, ENOMEM);
	io_error_snapshot(&state, &old);
	assert(io_error_observe(&old, &first) == ENOMEM && first == UINT64_MAX);
	io_error_record(&state, EIO);
	io_error_snapshot(&state, &newer);
	assert(newer.sequence == UINT64_MAX && newer.error == EIO);
	assert(io_error_observe(&newer, &first) == EIO);
	assert(io_error_observe(&old, &first) == ENOMEM);

	memset(&state, 0, sizeof(state));
	for (i = 0; i < 4; i++) assert(pthread_create(&consumers[i], NULL, reader, NULL) == 0);
	assert(pthread_create(&producer, NULL, writer, NULL) == 0);
	assert(pthread_join(producer, NULL) == 0);
	for (i = 0; i < 4; i++) assert(pthread_join(consumers[i], NULL) == 0);
	printf("I/O error ledger PASS: independent observers, captured cursor, saturation, paired concurrent snapshots (%u observations)\n", observations);
	return 0;
}
