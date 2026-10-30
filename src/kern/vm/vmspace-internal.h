/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declarations private to the address-space side of the VM.  These connect
 * vmspace.c, vmspace-metadata.c, vmspace-fault.c, vmspace-copy.c,
 * vmspace-fork.c, vmspace-map.c, vmspace-region.c, vmspace-unmap.c and the
 * device mapping part of device.c, and are not part of the kernel's public
 * VM API.
 *
 * Internal VM API: used only between the files in src/kern/vm/.  Its
 * compatibility is not guaranteed and it may change at any time.  None of
 * these declarations belongs in the public headers under include/kern.
 *
 * Function bodies and the order they take the locks are as they were in
 * the single vmspace.c: the metadata lock (kern/vm-lock.h) before
 * vm->lock, and the slab spinlocks below innermost.
 */

#ifndef KERN_VM_VMSPACE_INTERNAL_H
#define KERN_VM_VMSPACE_INTERNAL_H

#include "kern/vmspace.h"
#include "kern/vmspace_private.h"
#include "kern/lock.h"

struct file;
struct vm_object;
struct vm_page_slab;
struct vm_private_page_slab;

#define PAGE_SIZE			KERN_PAGE_SIZE
#define VM_PAGE_SLAB_SLOTS		32U
#define VM_PRIVATE_PAGE_SLAB_SLOTS	24U
#define VM_PRIVATE_PAGE_SLAB_FREE_MASK	((1U << VM_PRIVATE_PAGE_SLAB_SLOTS) - 1U)
/* About two seconds of waiting for page-table memory, as for a frame. */
#define VM_PAGE_MAP_RECLAIM_RETRIES	2000U
#define VM_PAGE_MAP_RESERVE_PAGES	4U

/* The aligned group of pages one file fault maps from what is cached. */
#define VM_FAULT_AROUND_PAGES		16U

/* How many private pages a fork shares under one hold of the VM locks. */
#define VM_FORK_BATCH			32U

/*
 * The most pages a copy between kernel and user memory pins into an array
 * on the stack.  Such a copy takes the VM locks once when its pages are
 * ready; a longer one checks its range first and allocates the array.
 */
#define VM_COPY_STACK_PAGES		4U

/* The page count at which a region indexes its pages instead of walking them. */
#define VM_REGION_INDEX_MINIMUM		32U

/*
 * The most buckets a region's index has (2 MiB of pointers on a 64-bit
 * machine): past it the chains grow longer instead (BUG-053: a region of
 * a million pages asked for 8 MiB of contiguous memory while memory was
 * short).
 */
#define VM_REGION_INDEX_MAXIMUM		262144U

/* The metadata slab lists and their locks (vmspace-metadata.c). */
extern struct spinlock vm_page_slab_lock;
extern struct vm_page_slab *vm_page_slabs;
extern struct spinlock vm_private_page_slab_lock;
extern struct vm_private_page_slab *vm_private_page_slabs;

/*
 * Defined in device.c, part of retains device storage independently of
 * descriptor and address-space lifetime.
 */
int vmspace_device_fault(struct vmspace *vm, struct vm_region *region, struct vm_page *page);

/*
 * Defined in vmspace-region.c, part of the region list of an address
 * space: range checks, insertion and lookup, the per-region page index,
 * and the search for free ranges.
 */
int vmspace_range_valid(uintptr_t start, size_t size);
int vmspace_overlaps(struct vmspace *vm, uintptr_t start, size_t size);
void vmspace_insert_region(struct vmspace *vm, struct vm_region *region);
struct vm_region *vmspace_find_region_locked(struct vmspace *vm, uintptr_t address, size_t size);
struct vm_page *vmspace_find_page(struct vm_region *region, uintptr_t address);
void vmspace_region_page_index_insert(struct vm_region *region, struct vm_page *page);
void vmspace_region_page_index_remove(struct vm_region *region, struct vm_page *page);
int vmspace_region_page_index_rebuild(struct vm_region *region, int keep);
void vmspace_region_page_index_free(struct vm_region *region);
int vmspace_find_free_range_bounded_locked(struct vmspace *vm, uintptr_t minimum, uintptr_t maximum, size_t size, size_t alignment, uintptr_t *mapped);
int vmspace_find_free_range_locked(struct vmspace *vm, uintptr_t hint, size_t size, size_t alignment, uintptr_t *mapped);
int vmspace_map_find_locked(struct vmspace *vm, uintptr_t hint, size_t size, uint32_t prot, uintptr_t *mapped);
int vmspace_map_file_find_locked(struct vmspace *vm, uintptr_t hint, size_t size, uint32_t prot, struct file *file, off_t offset, size_t data_size, struct vm_object *object, uintptr_t *mapped);
int vmspace_map_file_shared_find_locked(struct vmspace *vm, uintptr_t hint, size_t size, uint32_t prot, struct file *file, off_t offset, size_t data_size, struct vm_object *object, uintptr_t *mapped);

/*
 * Defined in vmspace-metadata.c, part of the slab allocators of the per-
 * page metadata of address spaces: the mapping descriptors (struct
 * vm_page) and the private page backings.
 */
struct vm_private_page *vmspace_private_page_alloc(void);
void vmspace_private_page_attach_new(struct vm_page *page, struct vm_private_page *backing);

/*
 * Defined in vmspace-map.c, part of mapping of anonymous, file, shared and
 * stack regions, the lookup of free ranges for them, and msync.
 */
int vmspace_map_region(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot, enum vm_region_backing backing, struct file *file, off_t file_offset, uintptr_t data_start, size_t data_size, unsigned flags, size_t commit_size, struct vm_region **result);
int vmspace_map_anon_locked(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot, struct vm_region **result);
int vmspace_map_file_locked(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot, struct file *file, off_t file_offset, uintptr_t data_start, size_t data_size, struct vm_object *object, struct vm_region **result);
int vmspace_map_file_shared_locked(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot, struct file *file, off_t file_offset, size_t data_size, struct vm_object *object, struct vm_region **result);
int vmspace_map_device_replace(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot, struct vm_device_mapping *mapping);

/*
 * Defined in vmspace-fault.c, part of demand paging of address spaces: the
 * page fault, its waits, the file, copy-on-write and fault-around fills,
 * and the queries of a page's protection and residency.
 */
void vmspace_wait_fault_event(struct vmspace *vm, uint64_t sequence);
void vmspace_wait_faults_locked(struct vmspace *vm);
int vmspace_prepare_cow_copy(struct vm_page *page, struct vm_private_page *old, struct vm_private_page **result);
int vmspace_private_page_pinned_resident(struct vm_private_page *backing);
void vmspace_fault_wake_locked(struct vmspace *vm);

/*
 * Defined in vmspace-unmap.c, part of removal and change of mappings:
 * unmap, protect, brk, the address, data and stack limits, and the
 * revocation of object pages.
 */
void vmspace_detach_page(struct vm_page *page);
void vmspace_release_detached_region_pages(struct vm_region *region);
int vmspace_replace_prepared(struct vmspace *vm, struct vm_region *prepared, struct vm_region **result);

/*
 * Defined in vmspace.c, part of process virtual-memory ownership and
 * demand paging.
 */
void vmspace_generation_advance_locked(struct vmspace *vm);

#endif
