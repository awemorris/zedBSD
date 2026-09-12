/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

/* Exercises the actual HAL mapping functions against controlled page tables. */
#define _POSIX_C_SOURCE 200809L
#include <hal/hal.h>
#include "src/hal/amd64/defs.h"
#include "src/hal/amd64/space.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device-map-definitions.h"

/* Test page tables and immutable RAM geometry replace hardware discovery. */
static uint64_t system_mmio_pd[512];
static hal_physaddr_t device_leaf_tables[AMD64_DEVICE_PD_COUNT];
static struct amd64_device_mapping *device_mappings;
static uint32_t page_table_count;
static uint64_t acpi_physical_max = UINT64_C(0xfffffffff);
static int ram_builder;
static uint64_t user_leaves[64];
static unsigned table_allocations;
static unsigned fail_table;
static unsigned fail_user_leaf;
static unsigned leaf_allocations;
static unsigned fail_metadata;
static unsigned live_metadata;
static unsigned flushes;
static unsigned user_flushes;

/* Independent mutexes model registry serialization and the shootdown boundary. */
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t user_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t flush_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flush_condition = PTHREAD_COND_INITIALIZER;
static unsigned stall_flush;
static unsigned flush_arrived;
static unsigned flush_release;

static int user_page_allowed(hal_physaddr_t physical, uint32_t attr);
static int device_fixed_range(hal_physaddr_t physical, size_t size, void **address);
static int device_window_map(hal_physaddr_t physical, size_t size, uint32_t attr, void **address);
static int device_window_unmap(void *address, size_t size);
static int device_window_populate(struct amd64_device_mapping *mapping);
static void device_window_clear(struct amd64_device_mapping *mapping);
static void device_window_retire(struct amd64_device_mapping *mapping);
static int valid_user_range(uintptr_t address, size_t size);
static uint64_t leaf_flags(uint32_t attr);
static void test_fixed_and_ranges(void);
static void test_ownership_and_failures(void);
static void test_retirement(void);
static void test_user_permissions(void);
static void reset_tables(void);
static uint64_t device_entry(void *address);
static void *release_thread(void *address);

/* RAM aliases cover exactly this ordinary allocator interval. */
static int
amd64_ram_lookup(
	const void *builder,
	uint64_t physical,
	uint64_t *entry)
{
	(void)builder;
	if (physical < 0x100000U || physical >= 0x200000U)
		return 0;
	*entry = AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	return 1;
}

/* Failure injection distinguishes metadata allocation from page-table pages. */
void *
kernel_alloc(
	size_t size)
{
	void *allocation;

	if (fail_metadata)
		return NULL;
	allocation = calloc(1U, size);
	if (allocation != NULL)
		(void)__atomic_add_fetch(&live_metadata, 1U, __ATOMIC_RELAXED);
	return allocation;
}

void
kernel_free(
	void *allocation)
{
	assert(allocation != NULL);
	assert(__atomic_load_n(&live_metadata, __ATOMIC_RELAXED) != 0U);
	(void)__atomic_sub_fetch(&live_metadata, 1U, __ATOMIC_RELAXED);
	free(allocation);
}

void *
hal_memset(
	void *destination,
	int byte,
	size_t size)
{
	return memset(destination, byte, size);
}

void *
hal_pmem_to_kernel(
	hal_physaddr_t address)
{
	return (void *)(uintptr_t)address;
}

void
hal_fatal(
	const char *file,
	int line,
	const char *message)
{
	fprintf(stderr, "%s:%d: %s\n", file, line, message);
	abort();
}

/* Page allocation is real aligned host RAM; physical identities remain stable. */
static int
alloc_page(
	hal_physaddr_t *physical)
{
	void *allocation;
	int error;

	table_allocations++;
	if (table_allocations == fail_table)
		return HAL_ERR_NOMEM;
	error = posix_memalign(&allocation, PAGE_SIZE, PAGE_SIZE);
	if (error != 0)
		return HAL_ERR_NOMEM;
	*physical = (uintptr_t)allocation;
	return HAL_OK;
}

static bool
registry_lock_enter(void)
{
	assert(pthread_mutex_lock(&registry_mutex) == 0);
	return true;
}

static void
registry_lock_leave(bool enabled)
{
	assert(enabled);
	assert(pthread_mutex_unlock(&registry_mutex) == 0);
}

static int
space_op_enter(struct amd64_space *space)
{
	return space != NULL;
}

static void
space_op_leave(struct amd64_space *space)
{
	assert(space != NULL);
}

static bool
space_lock_enter(struct amd64_space *space)
{
	assert(space != NULL);
	assert(pthread_mutex_lock(&user_mutex) == 0);
	return true;
}

static void
space_lock_leave(struct amd64_space *space, bool enabled)
{
	assert(space != NULL && enabled);
	assert(pthread_mutex_unlock(&user_mutex) == 0);
}

static uint64_t *
walk_leaf(struct amd64_space *space, uintptr_t address, int create)
{
	size_t index;

	assert(space != NULL);
	index = address / PAGE_SIZE - 1U;
	assert(index < 64U);
	if (create) {
		leaf_allocations++;
		if (leaf_allocations == fail_user_leaf)
			return NULL;
	}
	return &user_leaves[index];
}

static struct amd64_table_page *
detach_empty_tables(struct amd64_space *space)
{
	assert(space != NULL);
	return NULL;
}

static void
free_detached_tables(struct amd64_table_page *pages)
{
	assert(pages == NULL);
}

/* A paused acknowledgement lets another thread probe retiring-slot ownership. */
static void
shootdown(hal_space_t space, void *address, size_t size)
{
	(void)address;
	(void)size;
	if (space != HAL_SPACE_SYS) {
		user_flushes++;
		return;
	}
	assert(pthread_mutex_lock(&flush_mutex) == 0);
	flushes++;
	if (stall_flush) {
		stall_flush = 0U;
		flush_arrived = 1U;
		assert(pthread_cond_broadcast(&flush_condition) == 0);
		while (!flush_release)
			assert(pthread_cond_wait(&flush_condition, &flush_mutex) == 0);
	}
	assert(pthread_mutex_unlock(&flush_mutex) == 0);
}

#include "device-map-functions.h"

int
main(void)
{
	/* Each case uses real production functions, with fresh owned table storage. */
	test_fixed_and_ranges();
	reset_tables();
	test_ownership_and_failures();
	reset_tables();
	test_retirement();
	reset_tables();
	test_user_permissions();
	reset_tables();
	puts("PASS: device extent/ownership/rollback/retirement and user protection");
	return 0;
}

/* Inspects a mapped virtual byte without dereferencing its privileged alias. */
static uint64_t
device_entry(void *address)
{
	uintptr_t offset;
	unsigned slot;
	uint64_t *table;

	offset = (uintptr_t)address - AMD64_DEVICE_WINDOW_BASE;
	slot = (unsigned)(offset / PAGE_SIZE);
	assert(slot < AMD64_DEVICE_PAGE_COUNT);
	table = (uint64_t *)(uintptr_t)device_leaf_tables[slot / 512U];
	if (table == NULL)
		return 0U;
	return table[slot % 512U];
}

static void
test_fixed_and_ranges(void)
{
	void *address;
	uint64_t entry;
	uint32_t flags;

	flags = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_NOCACHE;
	assert(hal_space_map_device(0xf0000100U, 0x100U, flags, &address) == HAL_OK);
	assert((uintptr_t)address == UINT64_C(0xffffffffc0000100));
	assert(hal_space_unmap_device(address, 0x100U) == HAL_OK);
	assert(device_mappings == NULL && live_metadata == 0U);

	/* A start inside the old PCI window cannot authorize bytes beyond its end. */
	assert(hal_space_map_device(0xf0ffff00U, 0x200U, flags, &address) == HAL_OK);
	assert((uintptr_t)address >= AMD64_DEVICE_WINDOW_BASE);
	entry = device_entry(address);
	assert((entry & AMD64_PTE_ADDR_MASK) == 0xf0fff000U);
	assert((entry & (AMD64_PTE_NOCACHE | AMD64_PTE_NX | AMD64_PTE_WRITE)) ==
	    (AMD64_PTE_NOCACHE | AMD64_PTE_NX | AMD64_PTE_WRITE));
	assert((entry & (AMD64_PTE_USER | AMD64_PTE_GLOBAL)) == 0U);
	assert(hal_space_unmap_device(address, 0x200U) == HAL_OK);

	/* Validate physical width, RAM overlap, empty/overflowed ranges and policy. */
	assert(hal_space_map_device(0x100000U, PAGE_SIZE, flags, &address) == HAL_ERR_INVALID);
	assert(hal_space_map_device(0xfffffU, 2U, flags, &address) == HAL_ERR_INVALID);
	assert(hal_space_map_device(acpi_physical_max, 2U, flags, &address) == HAL_ERR_INVALID);
	assert(hal_space_map_device(UINT64_C(1) << 40, PAGE_SIZE, flags, &address) == HAL_ERR_INVALID);
	assert(hal_space_map_device(UINT64_MAX, SIZE_MAX, flags, &address) == HAL_ERR_INVALID);
	assert(hal_space_map_device(0x300000U, 0U, flags, &address) == HAL_ERR_INVALID);
	assert(hal_space_map_device(0x300000U, PAGE_SIZE, flags | HAL_SPACE_EXEC, &address) == HAL_ERR_INVALID);
	assert(hal_space_map_device(0x300000U, PAGE_SIZE, flags | HAL_SPACE_WRITETHRU, &address) == HAL_ERR_UNSUPPORTED);
	assert(device_mappings == NULL && live_metadata == 0U);
}

static void
test_ownership_and_failures(void)
{
	void *first;
	void *same;
	void *other;
	unsigned before;
	uint32_t flags;

	flags = HAL_SPACE_READ | HAL_SPACE_WRITE;
	assert(hal_space_map_device(UINT64_C(0x100000000), 32U * 1024U * 1024U, flags, &first) == HAL_OK);
	assert(page_table_count == 16U);
	assert(hal_space_map_device(UINT64_C(0x100000000), 32U * 1024U * 1024U, flags, &same) == HAL_OK);
	assert(first == same && live_metadata == 1U);
	before = flushes;
	assert(hal_space_unmap_device(first, 32U * 1024U * 1024U) == HAL_OK);
	assert(flushes == before && device_entry(same) != 0U);
	assert(hal_space_unmap_device(same, 32U * 1024U * 1024U) == HAL_OK);
	assert(flushes == before + 1U && device_entry(same) == 0U);
	assert(hal_space_unmap_device(same, 32U * 1024U * 1024U) == HAL_ERR_INVALID);
	assert(live_metadata == 0U);

	/* Exhaustion and metadata failure leave both caller output and ownership intact. */
	other = (void *)(uintptr_t)123U;
	fail_metadata = 1U;
	assert(hal_space_map_device(0x300000U, PAGE_SIZE, flags, &other) == HAL_ERR_NOMEM);
	assert((uintptr_t)other == 123U);
	fail_metadata = 0U;
	assert(hal_space_map_device(0x300000U, 513U * 1024U * 1024U, flags, &other) == HAL_ERR_NOMEM);
	assert((uintptr_t)other == 123U);

	/* A failure after existing and new tables were populated clears the entire view. */
	before = flushes;
	fail_table = table_allocations + 2U;
	assert(hal_space_map_device(UINT64_C(0x200000000), 40U * 1024U * 1024U, flags, &other) == HAL_ERR_NOMEM);
	assert(flushes == before + 1U && live_metadata == 0U);
	assert((uintptr_t)other == 123U && device_mappings == NULL);
	assert(device_entry(first) == 0U);
	fail_table = 0U;
	assert(hal_space_map_device(0x300000U, PAGE_SIZE, flags, &other) == HAL_OK);
	assert(other == first && device_entry(other) != 0U);
	assert(hal_space_unmap_device(other, PAGE_SIZE) == HAL_OK);
}

static void *
release_thread(void *address)
{
	assert(hal_space_unmap_device(address, PAGE_SIZE) == HAL_OK);
	return NULL;
}

static void
test_retirement(void)
{
	pthread_t thread;
	void *first;
	void *next;
	void *reused;
	uint32_t flags;

	flags = HAL_SPACE_READ | HAL_SPACE_WRITE;
	assert(hal_space_map_device(0x300000U, PAGE_SIZE, flags, &first) == HAL_OK);
	stall_flush = 1U;
	flush_arrived = 0U;
	flush_release = 0U;
	assert(pthread_create(&thread, NULL, release_thread, first) == 0);
	assert(pthread_mutex_lock(&flush_mutex) == 0);
	while (!flush_arrived)
		assert(pthread_cond_wait(&flush_condition, &flush_mutex) == 0);
	assert(pthread_mutex_unlock(&flush_mutex) == 0);

	/* PTEs are gone, but pending acknowledgement still reserves the old slot. */
	assert(hal_space_map_device(0x400000U, PAGE_SIZE, flags, &next) == HAL_OK);
	assert(next != first && device_entry(first) == 0U);
	assert(pthread_mutex_lock(&flush_mutex) == 0);
	flush_release = 1U;
	assert(pthread_cond_broadcast(&flush_condition) == 0);
	assert(pthread_mutex_unlock(&flush_mutex) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(hal_space_map_device(0x500000U, PAGE_SIZE, flags, &reused) == HAL_OK);
	assert(reused == first);
	assert(hal_space_unmap_device(next, PAGE_SIZE) == HAL_OK);
	assert(hal_space_unmap_device(reused, PAGE_SIZE) == HAL_OK);
}

static void
test_user_permissions(void)
{
	struct amd64_space user;
	uint32_t flags;
	uint32_t observed;
	uint64_t old;
	unsigned before;

	memset(&user, 0, sizeof(user));
	flags = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_DEVICE;
	assert(hal_space_map(&user, (void *)PAGE_SIZE, 0x300000U, PAGE_SIZE, flags) == HAL_OK);
	assert((user_leaves[0] & (AMD64_PTE_USER | AMD64_PTE_NX | AMD64_PTE_NOCACHE)) ==
	    (AMD64_PTE_USER | AMD64_PTE_NX | AMD64_PTE_NOCACHE));
	user_leaves[0] |= AMD64_PTE_DIRTY | AMD64_PTE_ACCESSED;
	old = user_leaves[0];
	before = user_flushes;
	assert(hal_space_prot(&user, (void *)PAGE_SIZE, PAGE_SIZE, HAL_SPACE_READ) == HAL_ERR_INVALID);
	assert(hal_space_prot(&user, (void *)PAGE_SIZE, PAGE_SIZE, flags | HAL_SPACE_EXEC) == HAL_ERR_INVALID);
	assert(hal_space_prot(&user, (void *)PAGE_SIZE, PAGE_SIZE, flags | HAL_SPACE_WRITETHRU) == HAL_ERR_UNSUPPORTED);
	assert(old == user_leaves[0] && before == user_flushes);
	assert(hal_space_prot_query(&user, (void *)PAGE_SIZE, PAGE_SIZE,
	    HAL_SPACE_READ | HAL_SPACE_DEVICE, &observed) == HAL_OK);
	assert((user_leaves[0] & AMD64_PTE_WRITE) == 0U);
	assert((observed & (HAL_SPACE_PAGE_DIRTY | HAL_SPACE_PAGE_ACCESSED)) ==
	    (HAL_SPACE_PAGE_DIRTY | HAL_SPACE_PAGE_ACCESSED));
	assert(hal_space_unmap(&user, (void *)PAGE_SIZE, PAGE_SIZE) == HAL_OK);

	/* Ordinary RAM retains its existing permissions and cannot acquire a device alias. */
	assert(hal_space_map(&user, (void *)PAGE_SIZE, 0x100000U, PAGE_SIZE, flags) == HAL_ERR_INVALID);
	assert(hal_space_map(&user, (void *)PAGE_SIZE, 0x300000U, PAGE_SIZE, HAL_SPACE_READ) == HAL_ERR_INVALID);
	assert(hal_space_map(&user, (void *)PAGE_SIZE, UINT64_C(1) << 40, PAGE_SIZE, flags) == HAL_ERR_INVALID);
	assert(hal_space_map(&user, (void *)PAGE_SIZE, 0x100000U, PAGE_SIZE, HAL_SPACE_READ) == HAL_OK);
	assert(hal_space_prot(&user, (void *)PAGE_SIZE, PAGE_SIZE, flags) == HAL_ERR_INVALID);
	assert(hal_space_prot(&user, (void *)PAGE_SIZE, PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_EXEC) == HAL_OK);
	assert((user_leaves[0] & (AMD64_PTE_NX | AMD64_PTE_NOCACHE)) == 0U);
	assert(hal_space_unmap(&user, (void *)PAGE_SIZE, PAGE_SIZE) == HAL_OK);

	/* Failure on the second leaf rolls back the first leaf and its translation. */
	fail_user_leaf = leaf_allocations + 2U;
	assert(hal_space_map(&user, (void *)PAGE_SIZE, 0x300000U, 2U * PAGE_SIZE, flags) == HAL_ERR_NOMEM);
	assert(user_leaves[0] == 0U && user_leaves[1] == 0U);
	fail_user_leaf = 0U;
}

static void
reset_tables(void)
{
	unsigned index;

	assert(device_mappings == NULL && live_metadata == 0U);
	for (index = 0U; index < AMD64_DEVICE_PD_COUNT; index++) {
		free((void *)(uintptr_t)device_leaf_tables[index]);
		device_leaf_tables[index] = 0U;
	}
	memset(system_mmio_pd, 0, sizeof(system_mmio_pd));
	memset(user_leaves, 0, sizeof(user_leaves));
	page_table_count = 0U;
	table_allocations = 0U;
	fail_table = 0U;
}
