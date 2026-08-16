#include "kern/kmem.h"
#include "kern/swap.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRESS_SLOTS 32U
#define STRESS_WORKERS 8U
#define STRESS_ITERATIONS 5000U

struct fake_swap {
	uint8_t pages[3][SWAP_PAGE_SIZE];
	int fail;
	pthread_mutex_t lock;
	pthread_cond_t condition;
	int block_write;
	int write_entered;
	int allow_write;
};

struct writer_argument {
	struct swap_backend *backend;
	uint32_t slot;
	const void *page;
	int error;
};

struct stress_context {
	struct swap_backend backend;
	pthread_mutex_t lock;
	uint8_t active[STRESS_SLOTS];
	int failed;
};

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

static void make_header(uint8_t *header, uint32_t bytes)
{
	memset(header, 0, ZEDBSD_SWAP_HEADER_SIZE);
	memcpy(header, "ZEDSWAP1", 8);
	put32(header + 8, 1);
	put32(header + 12, ZEDBSD_SWAP_HEADER_SIZE);
	put32(header + 16, SWAP_PAGE_SIZE);
	put32(header + 20, bytes);
	put32(header + 24, bytes / SWAP_PAGE_SIZE - 1U);
	put32(header + 28, swap_header_checksum(header));
}

void *kern_calloc(size_t n, size_t s) { return calloc(n, s); }
void kern_free(void *p) { free(p); }

static int read_page(void *data, uint32_t slot, void *page)
{
	struct fake_swap *fake = data;
	if (fake->fail) return EIO;
	memcpy(page, fake->pages[slot], SWAP_PAGE_SIZE);
	return 0;
}

static int write_page(void *data, uint32_t slot, const void *page)
{
	struct fake_swap *fake = data;
	if (fake->fail) return EIO;
	if (fake->block_write) {
		assert(pthread_mutex_lock(&fake->lock) == 0);
		fake->write_entered = 1;
		assert(pthread_cond_broadcast(&fake->condition) == 0);
		while (!fake->allow_write)
			assert(pthread_cond_wait(&fake->condition,
			    &fake->lock) == 0);
		assert(pthread_mutex_unlock(&fake->lock) == 0);
	}
	memcpy(fake->pages[slot], page, SWAP_PAGE_SIZE);
	return 0;
}

static void *writer_thread(void *argument)
{
	struct writer_argument *writer = argument;
	writer->error = swap_write_page(writer->backend, writer->slot,
	    writer->page);
	return NULL;
}

static void block_next_write(struct fake_swap *fake)
{
	assert(pthread_mutex_lock(&fake->lock) == 0);
	fake->block_write = 1;
	fake->write_entered = 0;
	fake->allow_write = 0;
	assert(pthread_mutex_unlock(&fake->lock) == 0);
}

static void wait_for_write(struct fake_swap *fake)
{
	assert(pthread_mutex_lock(&fake->lock) == 0);
	while (!fake->write_entered)
		assert(pthread_cond_wait(&fake->condition, &fake->lock) == 0);
	assert(pthread_mutex_unlock(&fake->lock) == 0);
}

static void release_write(struct fake_swap *fake)
{
	assert(pthread_mutex_lock(&fake->lock) == 0);
	fake->allow_write = 1;
	fake->block_write = 0;
	assert(pthread_cond_broadcast(&fake->condition) == 0);
	assert(pthread_mutex_unlock(&fake->lock) == 0);
}

static void *stress_worker(void *argument)
{
	struct stress_context *stress = argument;
	unsigned iteration;

	for (iteration = 0; iteration < STRESS_ITERATIONS; iteration++) {
		uint32_t slot;
		int error;
		do {
			error = swap_alloc_slot(&stress->backend, &slot);
			if (error == ENOSPC)
				sched_yield();
		} while (error == ENOSPC);
		assert(error == 0 && slot < STRESS_SLOTS);
		assert(pthread_mutex_lock(&stress->lock) == 0);
		if (stress->active[slot] != 0)
			stress->failed = 1;
		stress->active[slot] = 1;
		assert(pthread_mutex_unlock(&stress->lock) == 0);
		sched_yield();
		assert(pthread_mutex_lock(&stress->lock) == 0);
		stress->active[slot] = 0;
		assert(pthread_mutex_unlock(&stress->lock) == 0);
		swap_free_slot(&stress->backend, slot);
	}
	return NULL;
}

static void test_concurrent_slot_allocation(
	const struct swap_backend_ops *ops)
{
	struct stress_context stress;
	pthread_t workers[STRESS_WORKERS];
	uint32_t total, free_slots;
	unsigned i;

	memset(&stress, 0, sizeof(stress));
	assert(pthread_mutex_init(&stress.lock, NULL) == 0);
	swap_init(&stress.backend);
	assert(swap_activate(&stress.backend, ops, NULL, SWAP_PAGE_SIZE,
	    STRESS_SLOTS) == 0);
	for (i = 0; i < STRESS_WORKERS; i++)
		assert(pthread_create(&workers[i], NULL, stress_worker,
		    &stress) == 0);
	for (i = 0; i < STRESS_WORKERS; i++)
		assert(pthread_join(workers[i], NULL) == 0);
	assert(!stress.failed);
	assert(swap_get_stats(&stress.backend, &total, &free_slots) == 0);
	assert(total == STRESS_SLOTS && free_slots == STRESS_SLOTS);
	assert(swap_shutdown(&stress.backend) == 0);
	assert(pthread_mutex_destroy(&stress.lock) == 0);
}

int main(void)
{
	static const struct swap_backend_ops ops = {
		.read_page = read_page, .write_page = write_page,
	};
	struct swap_backend backend;
	struct fake_swap fake = { 0 };
	uint8_t input[SWAP_PAGE_SIZE], output[SWAP_PAGE_SIZE];
	uint8_t header[ZEDBSD_SWAP_HEADER_SIZE];
	uint32_t slots[4], total, free_slots;
	struct writer_argument writer;
	pthread_t writer_id;
	unsigned i;

	assert(pthread_mutex_init(&fake.lock, NULL) == 0);
	assert(pthread_cond_init(&fake.condition, NULL) == 0);

	make_header(header, ZEDBSD_SWAP_FILE_MIN_BYTES);
	assert(swap_header_validate(header, ZEDBSD_SWAP_FILE_MIN_BYTES) == 0);
	make_header(header, ZEDBSD_SWAP_FILE_MAX_BYTES);
	assert(swap_header_validate(header, ZEDBSD_SWAP_FILE_MAX_BYTES) == 0);
	assert(swap_header_validate(header, ZEDBSD_SWAP_FILE_MIN_BYTES) == EINVAL);
	header[24] ^= 1;
	assert(swap_header_validate(header, ZEDBSD_SWAP_FILE_MAX_BYTES) == EINVAL);

	for (i = 0; i < sizeof(input); i++) input[i] = (uint8_t)(i * 17U);
	swap_init(&backend);
	assert(swap_activate(&backend, &ops, &fake, SWAP_PAGE_SIZE, 3) == 0);
	swap_set_system_backend(&backend);
	assert(swap_system_backend() == &backend && backend.free_slots == 3);
	assert(swap_alloc_slot(&backend, &slots[0]) == 0 && slots[0] == 0);
	assert(swap_alloc_slot(&backend, &slots[1]) == 0 && slots[1] == 1);
	assert(swap_alloc_slot(&backend, &slots[2]) == 0 && slots[2] == 2);
	assert(swap_alloc_slot(&backend, &slots[3]) == ENOSPC);
	assert(swap_write_page(&backend, slots[1], input) == 0);
	memset(output, 0, sizeof(output));
	assert(swap_read_page(&backend, slots[1], output) == 0);
	assert(!memcmp(input, output, sizeof(input)));
	fake.fail = 1;
	assert(swap_read_page(&backend, slots[1], output) == EIO);
	fake.fail = 0;
	swap_free_slot(&backend, slots[1]);
	assert(swap_alloc_slot(&backend, &slots[3]) == 0 && slots[3] == 1);

	/* A slot cannot be recycled until the callback using it has returned. */
	swap_free_slot(&backend, slots[0]);
	swap_free_slot(&backend, slots[2]);
	block_next_write(&fake);
	writer = (struct writer_argument) { &backend, slots[3], input, -1 };
	assert(pthread_create(&writer_id, NULL, writer_thread, &writer) == 0);
	wait_for_write(&fake);
	swap_free_slot(&backend, slots[3]);
	assert(swap_get_stats(&backend, &total, &free_slots) == 0);
	assert(total == 3 && free_slots == 2);
	assert(swap_alloc_slot(&backend, &slots[0]) == 0 && slots[0] == 0);
	assert(swap_alloc_slot(&backend, &slots[2]) == 0 && slots[2] == 2);
	assert(swap_alloc_slot(&backend, &slots[3]) == ENOSPC);
	release_write(&fake);
	assert(pthread_join(writer_id, NULL) == 0 && writer.error == 0);
	assert(swap_alloc_slot(&backend, &slots[3]) == 0 && slots[3] == 1);
	swap_free_slot(&backend, slots[0]);
	swap_free_slot(&backend, slots[2]);
	swap_free_slot(&backend, slots[3]);

	/* Shutdown publishes quiescing and is retried after in-flight I/O. */
	assert(swap_alloc_slot(&backend, &slots[0]) == 0);
	block_next_write(&fake);
	writer = (struct writer_argument) { &backend, slots[0], input, -1 };
	assert(pthread_create(&writer_id, NULL, writer_thread, &writer) == 0);
	wait_for_write(&fake);
	assert(swap_shutdown(&backend) == EBUSY);
	release_write(&fake);
	assert(pthread_join(writer_id, NULL) == 0 && writer.error == 0);
	assert(swap_shutdown(&backend) == 0);
	assert(swap_system_backend() == NULL && backend.bitmap == NULL);
	test_concurrent_slot_allocation(&ops);
	assert(pthread_cond_destroy(&fake.condition) == 0);
	assert(pthread_mutex_destroy(&fake.lock) == 0);
	puts("zedBSD swap backend host tests: PASS");
	return 0;
}
