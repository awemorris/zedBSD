/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_KERN_IO_EPOCH_H
#define KERN_KERN_IO_EPOCH_H

#include <kern/atomic.h>

/* Logical owner state; it never substitutes for draining the owner itself. */
struct io_epoch {
	volatile uint64_t dirty;
	volatile uint64_t stable;
	volatile uint64_t active;
};

/* Marks logical mutation before its bytes reach a lower cache or BIO. */
static __inline void
io_epoch_mark(
	struct io_epoch *epoch)
{
	uint64_t current;

	current = atomic_u64_load_acquire(&epoch->dirty);
	while (current != UINT64_MAX) {
		if (atomic_u64_compare_exchange(&epoch->dirty, &current, current + 1U))
			return;
	}
}

/* Pins an in-progress logical owner before publishing its dirty generation. */
static __inline void
io_epoch_begin(
	struct io_epoch *epoch)
{
	(void)atomic_u64_fetch_add_release(&epoch->active, 1);
	io_epoch_mark(epoch);
}

/* Releases an owner after its lower mutation or retained dirty state is resolved. */
static __inline void
io_epoch_end(
	struct io_epoch *epoch)
{
	uint64_t current;

	current = atomic_u64_load_acquire(&epoch->active);
	while (current != 0) {
		if (atomic_u64_compare_exchange(&epoch->active, &current, current - 1U))
			return;
	}
}

/* Captures the logical prefix an owner must drain. */
static __inline uint64_t
io_epoch_target(
	const struct io_epoch *epoch)
{
	return atomic_u64_load_acquire(&epoch->dirty);
}

/* Records only a successfully drained captured prefix, without wrapping. */
static __inline void
io_epoch_complete(
	struct io_epoch *epoch,
	uint64_t target)
{
	uint64_t current;

	if (target == UINT64_MAX || atomic_u64_load_acquire(&epoch->active) != 0)
		return;
	current = atomic_u64_load_acquire(&epoch->stable);
	while (current < target) {
		if (atomic_u64_compare_exchange(&epoch->stable, &current, target))
			return;
	}
}

#endif
