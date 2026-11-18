/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The commitment accounting that bounds anonymous memory.
 */

#include "kern/vm-commit.h"
#include "kern/lock.h"
#include "kern/swap.h"
#include <hal/hal.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <limits.h>
#include <stdint.h>

/*
 * The commitment accounting that bounds anonymous memory.
 *
 * It holds the physical and swap capacity, the limit those two add up to,
 * and how much of that limit is already committed.
 */
static struct vm_commit_stats commit_stats;

/*
 * The spinlock that guards the commitment accounting above.
 */
static struct spinlock commit_lock = {
	{ 0 }, LOCK_RANK_VM_OBJECT, "VM commit accounting", 0, 0
};

/*
 * Whether the commitment limit has been published.
 *
 * Until it is, nothing can commit and a swap resize only seeds the
 * accounting rather than changing a live limit.
 */
static int commit_initialized;

/*
 * Whether a swap capacity has been seeded before initialization.
 *
 * The prepared manager and the published backend must agree on the
 * capacity, so initialization compares against what a resize left here.
 */
static int commit_swap_seeded;

/*
 * Computes the commit limit from the free physical memory and the swap
 * capacity.
 *
 * A swap size seeded earlier by vm_commit_resize_swap() must match the
 * published backend, otherwise the initialization is retried later.
 */
int
vm_commit_init(
	void)
{
	struct hal_memstat memory;
	struct swap_backend *swap;
	uint32_t swap_pages;
	uint32_t swap_free;
	uint64_t physical_pages;
	unsigned long irq;

	swap_pages = 0;
	swap_free = 0;

	/* Rejects a second initialization. */
	irq = spin_lock_irqsave(&commit_lock);
	if (commit_initialized) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return EBUSY;
	}

	/* Counts the physical pages left after the system reserve. */
	hal_get_memstat(&memory);
	physical_pages = memory.physical_free / VM_COMMIT_PAGE_SIZE;
	if (physical_pages > VM_COMMIT_SYSTEM_RESERVE_PAGES)
		physical_pages -= VM_COMMIT_SYSTEM_RESERVE_PAGES;
	else
		physical_pages = 0;

	/* Reads the swap capacity when a backend is published. */
	swap = swap_system_backend();
	if (swap != NULL)
		(void)swap_get_stats(swap, &swap_pages, &swap_free);

	/*
	 * The prepared manager and published backend must describe the same
	 * capacity before user commitment can begin.
	 */
	if (!commit_swap_seeded) {
		kern_memset(&commit_stats, 0, sizeof(commit_stats));
		commit_stats.swap_pages = swap_pages;
	} else if (commit_stats.swap_pages != swap_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return EAGAIN;
	}

	/* Publishes the limit; a zero limit means nothing can ever commit. */
	commit_stats.physical_pages = physical_pages;
	commit_stats.limit_pages = commit_stats.physical_pages + commit_stats.swap_pages;
	if (commit_stats.limit_pages == 0) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return ENOMEM;
	}

	commit_initialized = 1;

	spin_unlock_irqrestore(&commit_lock, irq);

	/* Reports the initialized accounting. */
	return 0;
}

/*
 * Replaces the swap capacity in the commit limit.
 *
 * The caller states the capacity it expects to replace, so a concurrent
 * change is detected.  Before initialization the new capacity only seeds
 * the accounting.
 */
int
vm_commit_resize_swap(
	uint64_t expected_pages,
	uint64_t replacement_pages)
{
	uint64_t limit;
	unsigned long irq;

	/* Rejects a resize based on a stale capacity. */
	irq = spin_lock_irqsave(&commit_lock);
	if (commit_stats.swap_pages != expected_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return EAGAIN;
	}

	/* Rejects a limit that would overflow. */
	if (replacement_pages > UINT64_MAX - commit_stats.physical_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return EOVERFLOW;
	}

	/* Rejects a limit below the pages already committed. */
	limit = commit_stats.physical_pages + replacement_pages;
	if (commit_initialized && commit_stats.used_pages > limit) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return ENOMEM;
	}

	/* Publishes the new capacity, seeding the accounting when early. */
	commit_stats.swap_pages = replacement_pages;
	commit_stats.limit_pages = limit;
	if (!commit_initialized)
		commit_swap_seeded = 1;

	spin_unlock_irqrestore(&commit_lock, irq);

	/* Reports the accepted resize. */
	return 0;
}

/*
 * Commits pages against the limit.
 */
int
vm_commit_reserve(
	size_t bytes)
{
	uint64_t pages;
	unsigned long irq;

	/* Rejects an empty or unaligned reservation. */
	if (bytes == 0 || (bytes & (VM_COMMIT_PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	pages = bytes / VM_COMMIT_PAGE_SIZE;

	/* A reservation before initialization is a programming error. */
	irq = spin_lock_irqsave(&commit_lock);
	if (!commit_initialized) {
		spin_unlock_irqrestore(&commit_lock, irq);
		HAL_FATAL("VM commit before initialization");
	}

	/* Refuses to exceed the limit. */
	if (pages > commit_stats.limit_pages - commit_stats.used_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return ENOMEM;
	}

	commit_stats.used_pages += pages;

	spin_unlock_irqrestore(&commit_lock, irq);

	/* Reports the committed pages. */
	return 0;
}

/*
 * Returns committed pages to the limit.
 */
void
vm_commit_release(
	size_t bytes)
{
	uint64_t pages;
	unsigned long irq;

	/* An empty or unaligned release is a programming error. */
	if (bytes == 0 || (bytes & (VM_COMMIT_PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid VM commit release");
	pages = bytes / VM_COMMIT_PAGE_SIZE;

	/* A release before initialization is a programming error. */
	irq = spin_lock_irqsave(&commit_lock);
	if (!commit_initialized) {
		spin_unlock_irqrestore(&commit_lock, irq);
		HAL_FATAL("VM commit release before initialization");
	}

	/* Releasing more than was committed is a programming error. */
	if (pages > commit_stats.used_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		HAL_FATAL("VM commit accounting underflow");
	}

	commit_stats.used_pages -= pages;

	spin_unlock_irqrestore(&commit_lock, irq);
}

/*
 * Copies the current commit statistics.
 */
void
vm_commit_get_stats(
	struct vm_commit_stats *output)
{
	unsigned long irq;

	/* Ignores a missing record. */
	if (output == NULL)
		return;

	/* Copies the statistics under the lock. */
	irq = spin_lock_irqsave(&commit_lock);

	kern_memcpy(output, &commit_stats, sizeof(*output));

	spin_unlock_irqrestore(&commit_lock, irq);
}
