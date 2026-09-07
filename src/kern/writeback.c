/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/* Optional dirty admission; policy and actual VM writes are separate stages. */
#include <kern/writeback.h>
#include <kern/cache-memory.h>
#include <kern/disk.h>
#include <kern/atomic.h>
#include <kern/page.h>
#include <hal/hal.h>
#include <errno.h>
#include <string.h>

#define WRITEBACK_GLOBAL_MAX (64ULL * 1024U * 1024U)
static atomic_uint_t budget_guard;
static struct writeback_budget *budgets[WRITEBACK_DEVICE_MAX];
static uint64_t dirty_bytes;
static uint64_t reserved_bytes;
static uint64_t live_tickets;
static uint64_t refusals;

static bool budget_lock(void);
static void budget_unlock(bool enabled);
static int budget_registered(struct writeback_budget *budget);
static void budget_current_limits(struct writeback_budget_stats *stats);

void
writeback_budget_limits(uint64_t target, uint64_t *high, uint64_t *low, uint64_t *device_high)
{
	uint64_t limit;

	/* Divide before multiplying, and cap before any unbounded arithmetic. */
	limit = target >= WRITEBACK_GLOBAL_MAX * 5U / 2U ? WRITEBACK_GLOBAL_MAX : target / 5U * 2U;
	limit &= ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
	*high = limit;
	*low = (limit / 2U) & ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
	*device_high = (limit / WRITEBACK_DEVICE_MAX) & ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
}

static void
budget_current_limits(struct writeback_budget_stats *stats)
{
	struct cache_memory_stats cache;

	cache_memory_get_stats(&cache);
	writeback_budget_limits(cache.target_bytes, &stats->high, &stats->low, &stats->device_high);
}

int
writeback_budget_attach(struct writeback_budget *budget, struct disk *leaf)
{
	struct writeback_budget_stats limits;
	unsigned index;
	unsigned available;
	int error;
	bool enabled;

	if (budget == NULL || leaf == NULL || leaf->d_parent != NULL)
		return EINVAL;
	budget_current_limits(&limits);
	if (limits.device_high < WRITEBACK_TICKET_BYTES)
		return ENOMEM;
	disk_ref(leaf);
	enabled = budget_lock();
	available = WRITEBACK_DEVICE_MAX;
	error = 0;
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		if (budgets[index] == budget || (budgets[index] != NULL && budgets[index]->disk == leaf)) {
			error = EEXIST;
			break;
		}
		if (budgets[index] == NULL && available == WRITEBACK_DEVICE_MAX)
			available = index;
	}
	if (error == 0 && available == WRITEBACK_DEVICE_MAX)
		error = EAGAIN;
	if (error == 0) {
		memset(budget, 0, sizeof(*budget));
		budget->disk = leaf;
		budgets[available] = budget;
	}
	budget_unlock(enabled);
	if (error != 0)
		disk_release(leaf);
	return error;
}

int
writeback_budget_detach(struct writeback_budget *budget)
{
	struct disk *disk;
	unsigned index;
	bool enabled;

	enabled = budget_lock();
	if (!budget_registered(budget)) {
		budget_unlock(enabled);
		return ENOENT;
	}
	if (budget->dirty != 0 || budget->reserved != 0 || budget->tickets != 0) {
		budget_unlock(enabled);
		return EBUSY;
	}
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++) {
		if (budgets[index] == budget)
			budgets[index] = NULL;
	}
	disk = budget->disk;
	memset(budget, 0, sizeof(*budget));
	budget_unlock(enabled);
	disk_release(disk);
	return 0;
}

int
writeback_budget_quiesce(struct writeback_budget *budget, int enabled)
{
	bool irq;

	if (enabled != 0 && enabled != 1)
		return EINVAL;
	irq = budget_lock();
	if (!budget_registered(budget)) {
		budget_unlock(irq);
		return ENOENT;
	}
	budget->quiescing = (unsigned)enabled;
	budget_unlock(irq);
	return 0;
}

int
writeback_ticket_reserve(struct writeback_budget *budget, struct writeback_ticket *ticket)
{
	struct writeback_budget_stats limits;
	bool enabled;
	uint64_t used;
	uint64_t device_used;

	if (ticket == NULL)
		return EINVAL;
	if (ticket->budget != NULL)
		return EBUSY;
	memset(ticket, 0, sizeof(*ticket));
	budget_current_limits(&limits);
	enabled = budget_lock();
	if (!budget_registered(budget) || budget->quiescing) {
		budget_unlock(enabled);
		return EAGAIN;
	}
	used = dirty_bytes + reserved_bytes;
	device_used = budget->dirty + budget->reserved;
	if (used > limits.high || WRITEBACK_TICKET_BYTES > limits.high - used ||
	    device_used > limits.device_high || WRITEBACK_TICKET_BYTES > limits.device_high - device_used) {
		budget->refusals++;
		refusals++;
		budget_unlock(enabled);
		return EAGAIN;
	}
	reserved_bytes += WRITEBACK_TICKET_BYTES;
	live_tickets++;
	budget->reserved += WRITEBACK_TICKET_BYTES;
	budget->tickets++;
	ticket->budget = budget;
	ticket->reserved = WRITEBACK_TICKET_BYTES;
	budget_unlock(enabled);
	return 0;
}

void
writeback_ticket_commit(struct writeback_ticket *ticket, size_t bytes)
{
	struct writeback_budget *budget;
	bool enabled;

	if (bytes == 0)
		return;
	enabled = budget_lock();
	if (ticket == NULL || !budget_registered(ticket->budget) ||
	    bytes > ticket->reserved || (bytes & (ZEDBSD_PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid dirty credit commit");
	budget = ticket->budget;
	ticket->reserved -= bytes;
	budget->reserved -= bytes;
	reserved_bytes -= bytes;
	budget->dirty += bytes;
	dirty_bytes += bytes;
	budget_unlock(enabled);
}

void
writeback_ticket_release(struct writeback_ticket *ticket)
{
	struct writeback_budget *budget;
	bool enabled;

	if (ticket == NULL || ticket->budget == NULL)
		return;
	enabled = budget_lock();
	budget = ticket->budget;
	if (!budget_registered(budget) || budget->tickets == 0 ||
	    budget->reserved < ticket->reserved || live_tickets == 0)
		HAL_FATAL("invalid dirty credit release");
	budget->reserved -= ticket->reserved;
	reserved_bytes -= ticket->reserved;
	budget->tickets--;
	live_tickets--;
	memset(ticket, 0, sizeof(*ticket));
	budget_unlock(enabled);
}

void
writeback_budget_clean(struct writeback_budget *budget, size_t bytes)
{
	bool enabled;

	if (bytes == 0)
		return;
	enabled = budget_lock();
	if (!budget_registered(budget) || bytes > budget->dirty ||
	    (bytes & (ZEDBSD_PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid dirty credit retirement");
	budget->dirty -= bytes;
	dirty_bytes -= bytes;
	budget_unlock(enabled);
}

void
writeback_budget_snapshot(struct writeback_budget_stats *stats)
{
	unsigned index;
	bool enabled;

	if (stats == NULL)
		return;
	memset(stats, 0, sizeof(*stats));
	budget_current_limits(stats);
	enabled = budget_lock();
	stats->dirty = dirty_bytes;
	stats->reserved = reserved_bytes;
	stats->tickets = live_tickets;
	stats->refusals = refusals;
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++)
		if (budgets[index] != NULL)
			stats->devices++;
	budget_unlock(enabled);
}

/*
 * Copies one registered device budget without exposing unsynchronized counters.
 * The disk pointer is borrowed; this snapshot never grants a lifetime reference.
 */
int
writeback_budget_read(
	struct writeback_budget *budget,
	struct writeback_budget *snapshot)
{
	bool enabled;

	/* Validates the destination before entering the accounting guard. */
	if (snapshot == NULL || snapshot == budget)
		return EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	enabled = budget_lock();
	if (!budget_registered(budget)) {
		budget_unlock(enabled);
		return ENOENT;
	}
	*snapshot = *budget;
	budget_unlock(enabled);
	return 0;
}

static int
budget_registered(struct writeback_budget *budget)
{
	unsigned index;

	if (budget == NULL)
		return 0;
	for (index = 0; index < WRITEBACK_DEVICE_MAX; index++)
		if (budgets[index] == budget)
			return 1;
	return 0;
}

static bool
budget_lock(void)
{
	bool enabled;

	enabled = hal_irq_disable();
	while (!atomic_try_acquire_zero(&budget_guard))
		hal_compiler_barrier();
	return enabled;
}

static void
budget_unlock(bool enabled)
{
	atomic_store_release(&budget_guard, 0);
	if (enabled)
		hal_irq_enable();
}
