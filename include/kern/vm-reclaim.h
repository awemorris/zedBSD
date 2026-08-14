/*
 * Virtual memory reclaim
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_VM_RECLAIM_H
#define ZEDBSD_KERN_VM_RECLAIM_H

#include <stdint.h>

struct vm_page;

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

void vm_page_track(struct vm_page *);
void vm_page_untrack(struct vm_page *);
int vm_reclaim_one(struct vm_page *avoid);
void vm_page_note_in(struct vm_page *);
void vm_reclaim_note_fault(void);
void vm_reclaim_get_stats(struct vm_reclaim_stats *);

#endif
