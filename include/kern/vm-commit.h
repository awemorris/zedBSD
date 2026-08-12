/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Strict user virtual-memory commitment accounting.
 */
#ifndef ZEDBSD_KERN_VM_COMMIT_H
#define ZEDBSD_KERN_VM_COMMIT_H

#include <stddef.h>
#include <stdint.h>

#define VM_COMMIT_PAGE_SIZE 4096U
#define VM_COMMIT_SYSTEM_RESERVE_PAGES 64U

struct vm_commit_stats {
	uint64_t physical_pages;
	uint64_t swap_pages;
	uint64_t limit_pages;
	uint64_t used_pages;
};

int vm_commit_init(void);
int vm_commit_reserve(size_t bytes);
void vm_commit_release(size_t bytes);
void vm_commit_get_stats(struct vm_commit_stats *);
int vm_commit_can_shutdown_swap(void);

#endif
