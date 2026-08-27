/* SWAP-T006: production VM commitment resize fixture. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <kern/swap.h>
#include <kern/lock.h>
#include <kern/vm-commit.h>

#include <assert.h>
#include <errno.h>
#include <hal/hal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct swap_backend test_backend;
static uint32_t test_swap_pages;

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)lock;
	(void)enabled;
}

void
hal_memory_get_stats(struct hal_memory_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->physical_free =
	    (VM_COMMIT_SYSTEM_RESERVE_PAGES + 10U) * VM_COMMIT_PAGE_SIZE;
}

void
hal_fatal(const char *file, int line, const char *message)
{
	(void)file;
	(void)line;
	(void)message;
	assert(!"unexpected HAL_FATAL");
}

struct swap_backend *
swap_system_backend(void)
{
	return &test_backend;
}

int
swap_get_stats(struct swap_backend *backend, uint32_t *total,
	       uint32_t *free_slots)
{
	assert(backend == &test_backend);
	*total = test_swap_pages;
	*free_slots = test_swap_pages;
	return 0;
}

int
main(void)
{
	struct vm_commit_stats stats;

	/* Boot source preparation may resize capacity before physical-memory
	 * commitment accounting is initialized. */
	assert(vm_commit_resize_swap(0, 100) == 0);
	assert(vm_commit_resize_swap(0, 101) == EAGAIN);
	test_swap_pages = 100;
	assert(vm_commit_init() == 0);
	vm_commit_get_stats(&stats);
	assert(stats.physical_pages == 10);
	assert(stats.swap_pages == 100);
	assert(stats.limit_pages == 110);

	assert(vm_commit_reserve(80U * VM_COMMIT_PAGE_SIZE) == 0);
	/* A rejected shrink is failure-atomic. */
	assert(vm_commit_resize_swap(100, 60) == ENOMEM);
	vm_commit_get_stats(&stats);
	assert(stats.swap_pages == 100 && stats.limit_pages == 110 &&
	       stats.used_pages == 80);

	/* Exact-limit removal succeeds and immediately constrains reservations.
	 */
	assert(vm_commit_resize_swap(100, 70) == 0);
	assert(vm_commit_reserve(VM_COMMIT_PAGE_SIZE) == ENOMEM);
	assert(vm_commit_resize_swap(100, 90) == EAGAIN);
	assert(vm_commit_resize_swap(70, 90) == 0);
	assert(vm_commit_reserve(20U * VM_COMMIT_PAGE_SIZE) == 0);
	assert(vm_commit_reserve(VM_COMMIT_PAGE_SIZE) == ENOMEM);

	vm_commit_release(100U * VM_COMMIT_PAGE_SIZE);
	vm_commit_get_stats(&stats);
	assert(stats.swap_pages == 90 && stats.limit_pages == 100 &&
	       stats.used_pages == 0);
	puts("SWAP-T006: VM commitment resize: PASS");
	return 0;
}
