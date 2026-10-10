/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * WS048 p004: host test of coherent DMA memory for non-coherent devices.
 *
 * The production src/drivers/generic/dma.c is compiled unchanged.  Built
 * with WS048_UNCACHED, the test supplies kern_pmem_map_uncached() and
 * kern_pmem_unmap_uncached() the way arm64 would once the HAL provides an
 * uncached mapping; built without, the weak references stay null, as on a
 * kernel without them.  Physical memory is a host array; the "direct map" is
 * that array and the uncached view is a separate host address, so the test
 * can tell which one the DMA layer handed out.
 */

#include <drivers/generic/dma.h>
#include <kern/io-stats.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/panic.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr, "ws048-dma: failed at %s:%d: %s\n",   \
			    __FILE__, __LINE__, #expression);                  \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

#define PHYSICAL_BASE	0x100000ULL
#define PHYSICAL_PAGES	64U
#define PAGE		4096U

static uint8_t ram[PHYSICAL_PAGES * PAGE] __attribute__((aligned(4096)));
static uint8_t uncached_window[PHYSICAL_PAGES * PAGE] __attribute__((aligned(4096), unused));
static bool page_used[PHYSICAL_PAGES];
static int pmem_outstanding;
static int uncached_outstanding;
static int refuse_unmap __attribute__((unused));

static struct drv_dma_device *make_device(int coherent, unsigned address_bits);
static void test_coherent_device(void);
static void test_noncoherent_device(void);

int
main(void)
{
	test_coherent_device();
	test_noncoherent_device();
	CHECK(pmem_outstanding == 0);
	CHECK(uncached_outstanding == 0);
	printf("ws048-dma%s: %u checks passed\n",
#ifdef WS048_UNCACHED
	    " (uncached provided)",
#else
	    " (no uncached mapping)",
#endif
	    checks);
	return EXIT_SUCCESS;
}

static struct drv_dma_device *
make_device(
	int coherent,
	unsigned address_bits)
{
	struct drv_dma_constraints constraints;
	struct drv_dma_device *device;

	memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = address_bits;
	constraints.max_segment_size = 16U * 1024U * 1024U;
	constraints.coherent = coherent;
	CHECK(drv_dma_device_create(&constraints, &device) == 0);
	return device;
}

/* A device that snoops the caches keeps using the direct map, as before. */
static void
test_coherent_device(void)
{
	struct drv_dma_device *device;
	struct drv_dma_buffer buffer;
	struct drv_dma_mapping *mapping;
	struct drv_dma_segment segment;
	struct drv_dma_vector *vector;

	device = make_device(1, 32U);
	CHECK(drv_dma_alloc_coherent(device, 100U, 64U, &buffer) == 0);
	CHECK(buffer.address == ram + (buffer.device_address - PHYSICAL_BASE));
	CHECK(uncached_outstanding == 0);
	CHECK(drv_dma_map(device, (uint8_t *)buffer.address + 8U, 16U, DRV_DMA_TO_DEVICE, &mapping) == 0);
	CHECK(drv_dma_mapping_segment(mapping, 0, &segment) == 0);
	CHECK(segment.address == buffer.device_address + 8U);
	drv_dma_unmap(device, mapping);
	drv_dma_free_coherent(device, &buffer);
	CHECK(buffer.address == NULL);

	CHECK(drv_dma_vector_create(device, 8192U, &vector) == 0);
	CHECK(drv_dma_vector_address(vector) >= (void *)ram);
	CHECK(drv_dma_vector_free(vector) == 0);
	CHECK(drv_dma_device_destroy(device) == 0);
}

/* A device that does not snoop gets an uncached view, or nothing. */
static void
test_noncoherent_device(void)
{
	struct drv_dma_device *device;
	struct drv_dma_buffer buffer;
	struct drv_dma_buffer second;
	struct drv_dma_mapping *mapping;
	struct drv_dma_segment segment;
	struct drv_dma_vector *vector;

	device = make_device(0, 31U);
	CHECK(drv_dma_device_is_coherent(device) == 0);

#ifdef WS048_UNCACHED
	/* The CPU's view is the uncached window, the device's address the physical one. */
	CHECK(drv_dma_alloc_coherent(device, 100U, 64U, &buffer) == 0);
	CHECK(buffer.address == uncached_window + (buffer.device_address - PHYSICAL_BASE));
	CHECK(uncached_outstanding == 1);

	/* Streaming maps are found through the uncached view. */
	CHECK(drv_dma_map(device, (uint8_t *)buffer.address + 32U, 8U, DRV_DMA_FROM_DEVICE, &mapping) == 0);
	CHECK(drv_dma_mapping_segment(mapping, 0, &segment) == 0);
	CHECK(segment.address == buffer.device_address + 32U);
	drv_dma_unmap(device, mapping);

	/* An address in the direct map is not one of this device's buffers. */
	CHECK(drv_dma_map(device, ram + (buffer.device_address - PHYSICAL_BASE), 8U, DRV_DMA_TO_DEVICE, &mapping) == ENOTSUP);

	/* A refused unmap keeps the buffer so the owner can retry. */
	refuse_unmap = 1;
	drv_dma_free_coherent(device, &buffer);
	CHECK(buffer.address != NULL);
	CHECK(uncached_outstanding == 1);
	refuse_unmap = 0;
	drv_dma_free_coherent(device, &buffer);
	CHECK(buffer.address == NULL);
	CHECK(uncached_outstanding == 0);

	/* Vectors are now allowed, backed by the same uncached memory. */
	CHECK(drv_dma_vector_create(device, 8192U, &vector) == 0);
	CHECK(drv_dma_vector_address(vector) >= (void *)uncached_window);
	CHECK(drv_dma_vector_count(vector) == 1U);
	CHECK(drv_dma_vector_free(vector) == 0);
	CHECK(uncached_outstanding == 0);

	/* A device being destroyed refuses new memory and returns what it took. */
	CHECK(drv_dma_alloc_coherent(device, 64U, 64U, &second) == 0);
	CHECK(drv_dma_device_destroy(device) == EBUSY);
	CHECK(drv_dma_alloc_coherent(device, 64U, 64U, &buffer) == EBUSY);
	drv_dma_free_coherent(device, &second);
	CHECK(drv_dma_device_destroy(device) == 0);
#else
	/* Without an uncached mapping there is no coherent memory for this device. */
	CHECK(drv_dma_alloc_coherent(device, 100U, 64U, &buffer) == ENOTSUP);
	CHECK(drv_dma_vector_create(device, 8192U, &vector) == ENOTSUP);
	CHECK(pmem_outstanding == 0);
	(void)second;
	(void)mapping;
	(void)segment;
	CHECK(drv_dma_device_destroy(device) == 0);
#endif
}

/*
 * The kernel services the DMA layer calls.
 */

int
kern_pmem_alloc_limited(
	size_t size,
	size_t alignment,
	uint64_t max_address,
	size_t boundary,
	struct kern_pmem *run)
{
	unsigned pages;
	unsigned first;
	unsigned i;
	bool free_run;

	(void)boundary;
	CHECK(alignment <= PAGE);
	pages = (unsigned)((size + PAGE - 1U) / PAGE);
	for (first = 0; first + pages <= PHYSICAL_PAGES; first++) {
		free_run = true;
		for (i = 0; i < pages; i++) {
			if (page_used[first + i])
				free_run = false;
		}
		if (!free_run)
			continue;
		if (PHYSICAL_BASE + (uint64_t)(first + pages) * PAGE - 1U > max_address)
			return ENOMEM;
		for (i = 0; i < pages; i++)
			page_used[first + i] = true;
		run->paddr = PHYSICAL_BASE + (uint64_t)first * PAGE;
		/* Matches kern_pmem: the run records the size its caller requested. */
		run->size = size;
		pmem_outstanding++;
		return 0;
	}
	return ENOMEM;
}

int
kern_pmem_free(
	struct kern_pmem *run)
{
	unsigned first;
	unsigned i;

	first = (unsigned)((run->paddr - PHYSICAL_BASE) / PAGE);
	for (i = 0; i < (run->size + PAGE - 1U) / PAGE; i++) {
		CHECK(page_used[first + i]);
		page_used[first + i] = false;
	}
	pmem_outstanding--;
	return 0;
}

void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	CHECK(address >= PHYSICAL_BASE);
	CHECK(address < PHYSICAL_BASE + sizeof(ram));
	return ram + (address - PHYSICAL_BASE);
}

size_t
kern_page_size(
	int level)
{
	CHECK(level == 1);
	return PAGE;
}

#ifdef WS048_UNCACHED
int
kern_pmem_map_uncached(
	const struct kern_pmem *run,
	void **mapped)
{
	CHECK((run->paddr % PAGE) == 0);
	CHECK((run->size % PAGE) == 0);
	*mapped = uncached_window + (run->paddr - PHYSICAL_BASE);
	uncached_outstanding++;
	return 0;
}

int
kern_pmem_unmap_uncached(
	void *mapped,
	size_t size)
{
	CHECK((uint8_t *)mapped >= uncached_window);
	CHECK((size % PAGE) == 0);
	if (refuse_unmap)
		return EBUSY;
	uncached_outstanding--;
	return 0;
}
#endif

void
io_stats_record(
	enum io_stat_event event,
	uint64_t bytes)
{
	(void)event;
	(void)bytes;
}

void *
kern_malloc(
	size_t size)
{
	return malloc(size);
}

void
kern_free(
	void *pointer)
{
	free(pointer);
}

void
kern_fatal(
	const char *file,
	int line,
	const char *message)
{
	fprintf(stderr, "ws048-dma: fatal at %s:%d: %s\n", file, line, message);
	exit(EXIT_FAILURE);
}

void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	(void)lock;
	return 0;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)lock;
	(void)enabled;
}
