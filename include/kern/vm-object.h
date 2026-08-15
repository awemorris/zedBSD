/*
 * VM object
 * Copyright (C) 2026 Awe Morris
 *
 * Shared file-backed virtual-memory objects.
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_VM_OBJECT_H
#define ZEDBSD_KERN_VM_OBJECT_H

#include <hal/hal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct file;
struct inode;
struct vm_page;

#define VM_OBJECT_PAGE_DIRTY 0x0001U
#define VM_OBJECT_PAGE_BUSY  0x0002U

struct vm_object_page {
	off_t offset;
	struct hal_pmem pmem;
	unsigned flags;
	unsigned mapping_count;
	struct vm_page *mappings;
	struct vm_object_page *next;
};

struct vm_object {
	unsigned usecount;
	/* file is retained for reads; write_file carries write capability. */
	struct file *file;
	struct file *write_file;
	struct inode *inode;
	struct vm_object_page *pages;
	int writeback_error;
	struct vm_object *next;
};

int vm_object_get_shared(struct file *, struct vm_object **);
void vm_object_ref(struct vm_object *);
void vm_object_put(struct vm_object *);
int vm_object_fault(struct vm_object *, off_t, struct vm_object_page **);
void vm_object_mapping_add(struct vm_object_page *, struct vm_page *);
void vm_object_mapping_remove(struct vm_object_page *, struct vm_page *);
void vm_object_mark_dirty(struct vm_object_page *);
int vm_object_sync_range(struct vm_object *, off_t, size_t, int);
int vm_object_sync_inode(struct inode *);
void vm_object_truncate_inode(struct inode *, off_t);
int vm_object_reclaim_one(void);
unsigned vm_object_count(void);
unsigned vm_object_page_count(void);

#endif
