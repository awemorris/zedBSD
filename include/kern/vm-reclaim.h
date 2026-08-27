/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Virtual memory reclaim
 */

#ifndef ZEDBSD_KERN_VM_RECLAIM_H
#define ZEDBSD_KERN_VM_RECLAIM_H

#include <stdint.h>

struct vm_page;
struct vm_private_page;
struct hal_pmem;

struct vm_reclaim_stats {
	uint32_t resident;
	uint32_t swapped;
	uint32_t page_ins;
	uint32_t page_outs;
	uint32_t reclaims;
	uint32_t io_errors;
	uint32_t faults;
	uint32_t anonymous_resident;
	uint32_t file_resident;
	uint32_t wired;
	uint32_t busy;
	uint32_t dirty;
	uint32_t clean;
};

extern struct vm_reclaim_stats vm_reclaim_counters;

void
vm_reclaim_init(void);

void
vm_private_page_init(
	struct vm_private_page *backing);

void
vm_private_page_ref(
	struct vm_private_page *backing);

void
vm_private_page_put(
	struct vm_private_page *backing);

/*
 * The blocking acquire requires a caller-owned lifetime reference.
 */
int
vm_private_page_io_acquire(
	struct vm_private_page *backing);

/*
 * On success the nonblocking form also returns an I/O lifetime reference.
 */
int
vm_private_page_io_try_acquire(
	struct vm_private_page *backing);

void
vm_private_page_io_release(
	struct vm_private_page *backing);

int
vm_private_page_wait_idle(
	struct vm_private_page *backing);

/*
 * A successful short operation includes a backing lifetime reference.
 */
int
vm_private_page_operation_try_begin(
	struct vm_private_page *backing);

void
vm_private_page_operation_end(
	struct vm_private_page *backing);

void
vm_private_page_mark_dirty(
	struct vm_private_page *backing);

int
vm_private_page_pin(
	struct vm_private_page *backing,
	struct hal_pmem *memory);

void
vm_private_page_unpin(
	struct vm_private_page *backing);

void
vm_page_track(
	struct vm_page *page);

void
vm_page_untrack(
	struct vm_page *page);

int
vm_reclaim_one(
	struct vm_page *avoid);

/*
 * Fault paths which retain BUSY VM/object state must not enter object
 * writeback, because an object content or resize transaction can be waiting
 * for that retained state.  This bounded form considers private mappings
 * only and never waits on an object transaction.  EAGAIN means that no
 * eligible private backing was available.
 */
int
vm_reclaim_private_one(
	struct vm_page *avoid);

void
vm_page_note_in(
	struct vm_page *page);

void
vm_reclaim_note_fault(void);

void
vm_reclaim_get_stats(
	struct vm_reclaim_stats *output);

#endif
