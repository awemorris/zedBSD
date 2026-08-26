/*
 * WS003 shared-bus DMA allocation-list concurrency fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/dma.h>
#include <hal/hal.h>
#include <kern/lock.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKERS 8U
#define ITERATIONS 2000U

static _Thread_local unsigned lock_depth;
static unsigned long next_paddr = 0x100000UL;
static unsigned live_pmem;
static pthread_mutex_t allocation_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t allocation_condition = PTHREAD_COND_INITIALIZER;
static int block_allocation;
static int allocation_entered;

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	while (__atomic_exchange_n(&lock->held.value, 1U, __ATOMIC_ACQUIRE) != 0)
		;
	assert(lock_depth == 0);
	lock_depth = 1;
	return 1;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)enabled;
	assert(lock_depth == 1);
	lock_depth = 0;
	__atomic_store_n(&lock->held.value, 0U, __ATOMIC_RELEASE);
}

void *
hal_malloc(size_t size)
{
	assert(lock_depth == 0);
	return malloc(size);
}

void
hal_free(void *pointer)
{
	assert(lock_depth == 0);
	free(pointer);
}

size_t
hal_page_get_page_size(int level)
{
	(void)level;
	return 4096U;
}

int
hal_pmem_alloc(const struct hal_pmem_request *request, struct hal_pmem *memory)
{
	void *address;

	assert(lock_depth == 0);
	pthread_mutex_lock(&allocation_gate);
	if (block_allocation) {
		allocation_entered = 1;
		pthread_cond_broadcast(&allocation_condition);
		while (block_allocation)
			pthread_cond_wait(&allocation_condition, &allocation_gate);
	}
	pthread_mutex_unlock(&allocation_gate);
	address = malloc(request->size);
	if (address == NULL)
		return HAL_ERR_NOMEM;
	memory->vaddr = address;
	memory->paddr = __atomic_fetch_add(&next_paddr, 0x10000UL,
	    __ATOMIC_RELAXED);
	memory->size = request->size;
	memory->type = request->type;
	memory->attr = request->attr;
	(void)__atomic_fetch_add(&live_pmem, 1U, __ATOMIC_RELAXED);
	return HAL_OK;
}

int
hal_pmem_free(struct hal_pmem *memory)
{
	assert(lock_depth == 0);
	free(memory->vaddr);
	assert(__atomic_fetch_sub(&live_pmem, 1U, __ATOMIC_RELAXED) != 0);
	return HAL_OK;
}

struct worker_context {
	struct drv_dma_device *device;
	struct drv_dma_buffer *shared;
};

static void *
allocation_worker(void *argument)
{
	struct worker_context *context = argument;
	unsigned i;

	for (i = 0; i < ITERATIONS; i++) {
		struct drv_dma_buffer buffer = { 0 };
		struct drv_dma_mapping *mapping;
		struct drv_dma_segment segment;

		assert(drv_dma_alloc_coherent(context->device, 512U, 64U,
		    &buffer) == 0);
		assert(drv_dma_map(context->device, buffer.address, buffer.size,
		    DRV_DMA_BIDIRECTIONAL, &mapping) == 0);
		assert(drv_dma_mapping_segment(mapping, 0, &segment) == 0);
		assert(segment.address == buffer.device_address);
		assert(segment.length == buffer.size);
		drv_dma_unmap(context->device, mapping);
		assert(drv_dma_map(context->device, context->shared->address, 64U,
		    DRV_DMA_FROM_DEVICE, &mapping) == 0);
		drv_dma_unmap(context->device, mapping);
		drv_dma_free_coherent(context->device, &buffer);
		assert(buffer.address == NULL);
	}
	return NULL;
}

struct blocked_context {
	struct drv_dma_device *device;
	struct drv_dma_buffer buffer;
	int result;
};

static void *
blocked_worker(void *argument)
{
	struct blocked_context *context = argument;

	context->result = drv_dma_alloc_coherent(context->device, 512U, 64U,
	    &context->buffer);
	return NULL;
}

static struct drv_dma_device *
create_device(void)
{
	const struct drv_dma_constraints constraints = {
		.address_bits = 64,
		.max_segment_size = 65536U,
		.segment_boundary = 0,
		.coherent = 1,
	};
	struct drv_dma_device *device;

	assert(drv_dma_device_create(&constraints, &device) == 0);
	return device;
}

int
main(void)
{
	struct drv_dma_device *device = create_device();
	struct drv_dma_buffer shared = { 0 };
	struct worker_context context = { device, &shared };
	pthread_t workers[WORKERS];
	struct drv_dma_mapping *mapping;
	struct blocked_context blocked = { 0 };
	pthread_t blocker;
	unsigned i;

	assert(drv_dma_alloc_coherent(device, 4096U, 4096U, &shared) == 0);
	for (i = 0; i < WORKERS; i++)
		assert(pthread_create(&workers[i], NULL, allocation_worker,
		    &context) == 0);
	for (i = 0; i < WORKERS; i++)
		assert(pthread_join(workers[i], NULL) == 0);
	assert(__atomic_load_n(&live_pmem, __ATOMIC_RELAXED) == 1U);

	/* The first destroy closes new work but permits coherent frees to drain. */
	assert(drv_dma_device_destroy(device) == EBUSY);
	assert(drv_dma_alloc_coherent(device, 512U, 64U,
	    &(struct drv_dma_buffer){ 0 }) == EBUSY);
	assert(drv_dma_map(device, shared.address, 64U, DRV_DMA_FROM_DEVICE,
	    &mapping) == EBUSY);
	drv_dma_free_coherent(device, &shared);
	assert(__atomic_load_n(&live_pmem, __ATOMIC_RELAXED) == 0U);
	assert(drv_dma_device_destroy(device) == 0);

	/* A pmem allocation in progress keeps an otherwise-empty device alive.
	 * Once destroy closes it, the allocation must be discarded, not linked. */
	device = create_device();
	blocked.device = device;
	pthread_mutex_lock(&allocation_gate);
	block_allocation = 1;
	allocation_entered = 0;
	pthread_mutex_unlock(&allocation_gate);
	assert(pthread_create(&blocker, NULL, blocked_worker, &blocked) == 0);
	pthread_mutex_lock(&allocation_gate);
	while (!allocation_entered)
		pthread_cond_wait(&allocation_condition, &allocation_gate);
	pthread_mutex_unlock(&allocation_gate);
	assert(drv_dma_device_destroy(device) == EBUSY);
	pthread_mutex_lock(&allocation_gate);
	block_allocation = 0;
	pthread_cond_broadcast(&allocation_condition);
	pthread_mutex_unlock(&allocation_gate);
	assert(pthread_join(blocker, NULL) == 0);
	assert(blocked.result == EBUSY);
	assert(blocked.buffer.address == NULL);
	assert(__atomic_load_n(&live_pmem, __ATOMIC_RELAXED) == 0U);
	assert(drv_dma_device_destroy(device) == 0);

	puts("DMA allocation lock test: PASS");
	return 0;
}
