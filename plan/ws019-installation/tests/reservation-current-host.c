/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Retains the original cases while linking the complete current VM owners. */
#define ZEDBSD_RESERVATION_CURRENT_VM
#define main reservation_cases
#define vm_reclaim_private_one legacy_reclaim_private_one
#define vm_reclaim_one legacy_reclaim_one
#define vm_commit_release legacy_commit_release
#define vm_commit_reserve legacy_commit_reserve
#define vm_metadata_enter legacy_metadata_enter
#define vm_metadata_leave legacy_metadata_leave
#define vm_metadata_init legacy_metadata_init
#define vm_page_untrack legacy_page_untrack
#include "format-reservation-test.c"
#undef main
#undef vm_reclaim_private_one
#undef vm_reclaim_one
#undef vm_commit_release
#undef vm_commit_reserve
#undef vm_metadata_enter
#undef vm_metadata_leave
#undef vm_metadata_init
#undef vm_page_untrack
#include <kern/cache-memory.h>
#include <kern/swap.h>

/* IRQ exclusion is supplied by the fixture's real atomic host locks. */
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }

/* Test recursive metadata ownership against the actual owner field. */
int mutex_owned(struct mutex *mutex)
{
	return mutex->locked != 0 && mutex->owner == thread_current();
}

/* Supply ample deterministic memory to the production cache accountant. */
void hal_memory_get_stats(struct hal_memory_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->physical_total = 512U * 1024U * 1024U;
	stats->physical_free = 256U * 1024U * 1024U;
}

/* No test publishes private anonymous pages or performs swap I/O. */
struct swap_backend *swap_system_backend(void) { return NULL; }
int swap_alloc_slot(struct swap_backend *backend, uint32_t *slot)
{
	(void)backend; (void)slot;
	CHECK(0);
	return ENOSPC;
}
void swap_free_slot(struct swap_backend *backend, uint32_t slot)
{
	(void)backend; (void)slot;
	CHECK(0);
}
int swap_write_page(struct swap_backend *backend, uint32_t slot, const void *page)
{
	(void)backend; (void)slot; (void)page;
	CHECK(0);
	return EIO;
}

/* Region-admission tests must never reach hardware translation operations. */
int hal_page_query(hal_space_t space, void *address, uint32_t *flags)
{
	(void)space; (void)address; (void)flags;
	CHECK(0);
	return HAL_ERR_UNSUPPORTED;
}
int hal_page_map(hal_space_t space, void *address, hal_physaddr_t physical, size_t size, uint32_t flags)
{
	(void)space; (void)address; (void)physical; (void)size; (void)flags;
	CHECK(0);
	return HAL_ERR_UNSUPPORTED;
}
int hal_page_prot(hal_space_t space, void *address, size_t size, uint32_t flags)
{
	(void)space; (void)address; (void)size; (void)flags;
	CHECK(0);
	return HAL_ERR_UNSUPPORTED;
}

int main(void)
{
	int error;
	cache_memory_init();
	error = reservation_cases();
	return error;
}
