/*
 * vmspace
 * Copyright (C) 2026 Awe Morris
 * Process virtual-memory ownership.
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_VMSPACE_H
#define ZEDBSD_KERN_VMSPACE_H

#include <hal/hal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define VM_USER_MIN 0x00001000U
#define VM_USER_TOP 0x80000000U

struct file;
struct vm_object;
struct vm_object_page;
struct vm_region;
struct vmspace;

#define VM_PAGE_RESIDENT 0x0001U
#define VM_PAGE_DIRTY    0x0002U
#define VM_PAGE_BUSY     0x0004U
#define VM_PAGE_SWAPPED  0x0008U

#define VM_REGION_STACK     0x0001U
#define VM_REGION_GUARD     0x0002U
#define VM_REGION_IMMUTABLE 0x0004U
#define VM_REGION_BRK       0x0008U
#define VM_REGION_SHARED    0x0010U

#define VM_BRK_TOP 0x10000000U

enum vm_region_backing {
	VM_BACKING_ANON = 0,
	VM_BACKING_FILE,
};

struct vm_page {
	uintptr_t address;
	struct hal_pmem pmem;
	unsigned wire_count;
	unsigned flags;
	uint32_t swap_slot;
	struct vmspace *vm;
	struct vm_region *region;
	struct vm_object_page *object_page;
	struct vm_page *object_next;
	struct vm_page *next;
	struct vm_page *queue_next;
};

struct vm_region {
	uintptr_t start;
	size_t size;
	uint32_t prot;
	unsigned flags;
	size_t commit_size;
	enum vm_region_backing backing;
	struct file *file;
	struct vm_object *object;
	off_t file_offset;
	uintptr_t data_start;
	size_t data_size;
	struct vm_page *pages;
	struct vm_region *next;
};

struct vmspace {
	hal_space_t space;
	unsigned usecount;
	struct vm_region *regions;
	uintptr_t entry;
	uintptr_t brk_start;
	uintptr_t brk_current;
	uintptr_t stack_guard_bottom;
	uintptr_t stack_bottom;
	uintptr_t stack_top;
};

extern struct vmspace kernel_vmspace;
struct vmspace *vmspace_create(void);
int vmspace_fork(struct vmspace *, struct vmspace **);
int vmspace_map_anon(struct vmspace *, uintptr_t, size_t, uint32_t,
		     struct vm_region **);
int vmspace_map_anon_fixed_noreplace(struct vmspace *, uintptr_t, size_t,
				      uint32_t, struct vm_region **);
int vmspace_map_file(struct vmspace *, uintptr_t, size_t, uint32_t,
		     struct file *, off_t, uintptr_t, size_t,
		     struct vm_region **);
int vmspace_map_file_shared(struct vmspace *, uintptr_t, size_t, uint32_t,
			    struct file *, off_t, size_t,
			    struct vm_region **);
int vmspace_map_stack(struct vmspace *, uintptr_t, size_t, size_t);
int vmspace_set_brk_start(struct vmspace *, uintptr_t);
int vmspace_brk(struct vmspace *, uintptr_t, uintptr_t *);
struct vm_region *vmspace_find_region(struct vmspace *, uintptr_t, size_t);
int vmspace_map_find(struct vmspace *, uintptr_t, size_t, uint32_t,
		     uintptr_t *);
int vmspace_map_file_find(struct vmspace *, uintptr_t, size_t, uint32_t,
			  struct file *, off_t, size_t, uintptr_t *);
int vmspace_map_file_shared_find(struct vmspace *, uintptr_t, size_t,
				 uint32_t, struct file *, off_t, size_t,
				 uintptr_t *);
int vmspace_unmap(struct vmspace *, uintptr_t, size_t);
int vmspace_protect(struct vmspace *, uintptr_t, size_t, uint32_t);
int vmspace_sync(struct vmspace *, uintptr_t, size_t, int);
int vmspace_check(struct vmspace *, uintptr_t, size_t, uint32_t);
int vmspace_fault(struct vmspace *, uintptr_t, uint32_t);
int vmspace_wire_range(struct vmspace *, uintptr_t, size_t, uint32_t);
void vmspace_unwire_range(struct vmspace *, uintptr_t, size_t);
int vmspace_copy_to(struct vmspace *, uintptr_t, const void *, size_t);
int vmspace_copy_from(struct vmspace *, void *, uintptr_t, size_t);
void vmspace_free(struct vmspace *);

#endif
