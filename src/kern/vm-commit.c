/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/vm-commit.h"
#include "kern/swap.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

static struct vm_commit_stats commit_stats;
static int commit_initialized;

int
vm_commit_init(void)
{
	struct hal_memory_stats memory;
	struct swap_backend *swap;
	uint64_t physical_pages;
	bool enabled;

	enabled = hal_irq_disable();
	if (commit_initialized) {
		if (enabled)
			hal_irq_enable();
		return EBUSY;
	}
	hal_memory_get_stats(&memory);
	physical_pages = memory.physical_free / VM_COMMIT_PAGE_SIZE;
	if (physical_pages > VM_COMMIT_SYSTEM_RESERVE_PAGES)
		physical_pages -= VM_COMMIT_SYSTEM_RESERVE_PAGES;
	else
		physical_pages = 0;
	swap = swap_system_backend();
	memset(&commit_stats, 0, sizeof(commit_stats));
	commit_stats.physical_pages = physical_pages;
	commit_stats.swap_pages = swap != NULL ? swap->slot_count : 0;
	commit_stats.limit_pages = commit_stats.physical_pages +
		commit_stats.swap_pages;
	if (commit_stats.limit_pages == 0) {
		if (enabled)
			hal_irq_enable();
		return ENOMEM;
	}
	commit_initialized = 1;
	if (enabled)
		hal_irq_enable();
	return 0;
}

int
vm_commit_reserve(size_t bytes)
{
	uint64_t pages;
	bool enabled;

	if (!commit_initialized)
		HAL_FATAL("VM commit before initialization");
	if (bytes == 0 || (bytes & (VM_COMMIT_PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	pages = bytes / VM_COMMIT_PAGE_SIZE;
	enabled = hal_irq_disable();
	if (pages > commit_stats.limit_pages - commit_stats.used_pages) {
		if (enabled)
			hal_irq_enable();
		return ENOMEM;
	}
	commit_stats.used_pages += pages;
	if (enabled)
		hal_irq_enable();
	return 0;
}

void
vm_commit_release(size_t bytes)
{
	uint64_t pages;
	bool enabled;

	if (!commit_initialized)
		HAL_FATAL("VM commit release before initialization");
	if (bytes == 0 || (bytes & (VM_COMMIT_PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid VM commit release");
	pages = bytes / VM_COMMIT_PAGE_SIZE;
	enabled = hal_irq_disable();
	if (pages > commit_stats.used_pages)
		HAL_FATAL("VM commit accounting underflow");
	commit_stats.used_pages -= pages;
	if (enabled)
		hal_irq_enable();
}

void
vm_commit_get_stats(struct vm_commit_stats *output)
{
	bool enabled;

	if (output == NULL)
		return;
	enabled = hal_irq_disable();
	memcpy(output, &commit_stats, sizeof(*output));
	if (enabled)
		hal_irq_enable();
}

int
vm_commit_can_shutdown_swap(void)
{
	bool enabled;
	int safe;

	enabled = hal_irq_disable();
	safe = !commit_initialized || commit_stats.swap_pages == 0 ||
		commit_stats.used_pages == 0;
	if (enabled)
		hal_irq_enable();
	return safe;
}
