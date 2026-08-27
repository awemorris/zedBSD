/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/vm-commit.h"
#include "kern/lock.h"
#include "kern/swap.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

static struct vm_commit_stats commit_stats;
static int commit_initialized;
static int commit_swap_seeded;
static struct spinlock commit_lock = {
	{ 0 }, LOCK_RANK_VM_OBJECT, "VM commit accounting", 0, 0
};

int
vm_commit_init(void)
{
	struct hal_memory_stats memory;
	struct swap_backend *swap;
	uint32_t swap_pages = 0, swap_free = 0;
	uint64_t physical_pages;
	unsigned long irq;

	irq = spin_lock_irqsave(&commit_lock);
	if (commit_initialized) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return EBUSY;
	}
	hal_memory_get_stats(&memory);
	physical_pages = memory.physical_free / VM_COMMIT_PAGE_SIZE;
	if (physical_pages > VM_COMMIT_SYSTEM_RESERVE_PAGES)
		physical_pages -= VM_COMMIT_SYSTEM_RESERVE_PAGES;
	else
		physical_pages = 0;
	swap = swap_system_backend();
	if (swap != NULL)
		(void)swap_get_stats(swap, &swap_pages, &swap_free);
	if (!commit_swap_seeded) {
		memset(&commit_stats, 0, sizeof(commit_stats));
		commit_stats.swap_pages = swap_pages;
	} else if (commit_stats.swap_pages != swap_pages) {
		/* The prepared manager and published backend must describe the same
		 * capacity before user commitment can begin. */
		spin_unlock_irqrestore(&commit_lock, irq);
		return EAGAIN;
	}
	commit_stats.physical_pages = physical_pages;
	commit_stats.limit_pages = commit_stats.physical_pages +
		commit_stats.swap_pages;
	if (commit_stats.limit_pages == 0) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return ENOMEM;
	}
	commit_initialized = 1;
	spin_unlock_irqrestore(&commit_lock, irq);
	return 0;
}

int
vm_commit_resize_swap(uint64_t expected_pages, uint64_t replacement_pages)
{
	uint64_t limit;
	unsigned long irq;

	irq = spin_lock_irqsave(&commit_lock);
	if (commit_stats.swap_pages != expected_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return EAGAIN;
	}
	if (replacement_pages > UINT64_MAX - commit_stats.physical_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return EOVERFLOW;
	}
	limit = commit_stats.physical_pages + replacement_pages;
	if (commit_initialized && commit_stats.used_pages > limit) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return ENOMEM;
	}
	commit_stats.swap_pages = replacement_pages;
	commit_stats.limit_pages = limit;
	if (!commit_initialized)
		commit_swap_seeded = 1;
	spin_unlock_irqrestore(&commit_lock, irq);
	return 0;
}

int
vm_commit_reserve(size_t bytes)
{
	uint64_t pages;
	unsigned long irq;

	if (bytes == 0 || (bytes & (VM_COMMIT_PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	pages = bytes / VM_COMMIT_PAGE_SIZE;
	irq = spin_lock_irqsave(&commit_lock);
	if (!commit_initialized) {
		spin_unlock_irqrestore(&commit_lock, irq);
		HAL_FATAL("VM commit before initialization");
	}
	if (pages > commit_stats.limit_pages - commit_stats.used_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return ENOMEM;
	}
	commit_stats.used_pages += pages;
	spin_unlock_irqrestore(&commit_lock, irq);
	return 0;
}

void
vm_commit_release(size_t bytes)
{
	uint64_t pages;
	unsigned long irq;

	if (bytes == 0 || (bytes & (VM_COMMIT_PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid VM commit release");
	pages = bytes / VM_COMMIT_PAGE_SIZE;
	irq = spin_lock_irqsave(&commit_lock);
	if (!commit_initialized) {
		spin_unlock_irqrestore(&commit_lock, irq);
		HAL_FATAL("VM commit release before initialization");
	}
	if (pages > commit_stats.used_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		HAL_FATAL("VM commit accounting underflow");
	}
	commit_stats.used_pages -= pages;
	spin_unlock_irqrestore(&commit_lock, irq);
}

void
vm_commit_get_stats(struct vm_commit_stats *output)
{
	unsigned long irq;

	if (output == NULL)
		return;
	irq = spin_lock_irqsave(&commit_lock);
	memcpy(output, &commit_stats, sizeof(*output));
	spin_unlock_irqrestore(&commit_lock, irq);
}

int
vm_commit_can_shutdown_swap(void)
{
	unsigned long irq;
	int safe;

	irq = spin_lock_irqsave(&commit_lock);
	safe = !commit_initialized || commit_stats.swap_pages == 0 ||
		commit_stats.used_pages == 0;
	spin_unlock_irqrestore(&commit_lock, irq);
	return safe;
}
