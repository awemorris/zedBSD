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
#include <kern/atomic.h>
#include <kern/lock.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct vm_layout {
	uintptr_t user_minimum;
	uintptr_t user_limit;
	uintptr_t brk_limit;
	uintptr_t mmap_base;
	uintptr_t stack_top;
};

extern struct vm_layout vm_layout;
void vmspace_layout_init(void);
int vmspace_user_range_valid(uintptr_t, size_t);

struct file;
struct vm_object;
struct vm_object_page;
struct vm_private_page;
struct vm_region;
struct vmspace;

#define VM_PAGE_RESIDENT 0x0001U
#define VM_PAGE_DIRTY    0x0002U
#define VM_PAGE_BUSY     0x0004U
#define VM_PAGE_SWAPPED  0x0008U

#define VM_MAPPING_COW    0x0100U
#define VM_MAPPING_MAPPED 0x0200U
#define VM_MAPPING_BUSY   0x0400U
/* Transient mprotect transaction markers; never visible after the call. */
#define VM_MAPPING_PROTECT_REMOVED 0x0800U
#define VM_MAPPING_PROTECT_ADDED   0x1000U
/* Reclaim unmapped this PTE outside the metadata locks. */
#define VM_MAPPING_RECLAIM_UNMAPPED 0x2000U
/* Reclaim write-protected this PTE before its final dirty-bit snapshot. */
#define VM_MAPPING_RECLAIM_PROTECTED 0x4000U

#define VM_REGION_STACK     0x0001U
#define VM_REGION_GUARD     0x0002U
#define VM_REGION_IMMUTABLE 0x0004U
#define VM_REGION_BRK       0x0008U
#define VM_REGION_SHARED    0x0010U
#define VM_REGION_ELF_ZERO_TAIL 0x0020U

enum vm_region_backing {
	VM_BACKING_ANON = 0,
	VM_BACKING_FILE,
};

struct vm_private_page {
	/* Mapping references plus explicit transient I/O/lifetime holds. */
	refcount_t refs;
	/* Serializes backing state without being held across I/O. */
	struct spinlock state_lock;
	struct wait_queue state_waitq;
	uint64_t generation;
	/* Short metadata operations exclude a new I/O owner. */
	unsigned active_operations;
	/* Pins keep the resident frame stable without retaining vm_page metadata. */
	unsigned pin_count;
	/* Reverse mappings only; transient refs do not affect COW reuse. */
	unsigned mapping_count;
	struct hal_pmem pmem;
	unsigned flags;
	uint32_t swap_slot;
	struct vm_page *mappings;
	struct vm_private_page *queue_next;
};

struct vm_page {
	uintptr_t address;
	unsigned wire_count;
	unsigned flags;
	struct vmspace *vm;
	struct vm_region *region;
	struct vm_private_page *private_page;
	struct vm_page *private_next;
	struct vm_object_page *object_page;
	struct vm_page *object_next;
	struct vm_page *next;
};

struct vm_region {
	uintptr_t start;
	size_t size;
	uint32_t prot;
	/* Maximum permissions established when the mapping is created.  In
	 * particular, a read-only MAP_SHARED descriptor must not become a
	 * writable mapping through a later mprotect(). */
	uint32_t max_prot;
	unsigned flags;
	size_t commit_size;
	/* Pins this metadata while a fault performs lockless backing I/O. */
	unsigned hold_count;
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
	refcount_t refs;
	/* Intrusive link used only after the final reference is released. */
	struct vmspace *reap_next;
	/* Serializes region/page metadata; generation changes on every commit. */
	struct mutex lock;
	struct wait_queue fault_waitq;
	uint64_t generation;
	struct vm_region *regions;
	uintptr_t entry;
	uintptr_t brk_start;
	uintptr_t brk_current;
	/* Initialized data + BSS charged to RLIMIT_DATA before heap growth. */
	uint64_t static_data_bytes;
	uintptr_t stack_guard_bottom;
	uintptr_t stack_bottom;
	uintptr_t stack_top;
	uint64_t address_limit;
	uint64_t data_limit;
	uint64_t mapped_virtual_bytes;
	uint64_t stack_limit;
};

enum vmspace_pinned_page_kind {
	VMSPACE_PINNED_NONE = 0,
	VMSPACE_PINNED_PRIVATE,
	VMSPACE_PINNED_OBJECT,
};

/*
 * One physical-page identity captured while the complete user range was
 * stable under the VM metadata locks.  The backing pin, rather than the
 * vm_page reverse mapping, owns the storage after the snapshot is published.
 */
struct vmspace_pinned_page {
	enum vmspace_pinned_page_kind kind;
	union {
		struct vm_private_page *private_page;
		struct vm_object_page *object_page;
	} owner;
	/* Private-page pins copy directly through this immutable frame snapshot. */
	struct hal_pmem memory;
};

extern struct vmspace kernel_vmspace;
struct vmspace *vmspace_create(void);
struct vm_page *vm_page_alloc_metadata(void);
void vm_page_free_metadata(struct vm_page *);
int vmspace_tryref(struct vmspace *);
void vmspace_ref(struct vmspace *);
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
int vmspace_set_brk_start(struct vmspace *, uintptr_t, uint64_t);
int vmspace_brk(struct vmspace *, uintptr_t, uintptr_t *);
struct vm_region *vmspace_find_region(struct vmspace *, uintptr_t, size_t);
int vmspace_shared_mapping_key(struct vmspace *, uintptr_t, size_t,
			       struct vm_object **, uintptr_t *);
int vmspace_find_free_range(struct vmspace *, uintptr_t, size_t, size_t,
			    uintptr_t *);
int vmspace_find_free_range_bounded(struct vmspace *, uintptr_t, uintptr_t,
				    size_t, size_t, uintptr_t *);
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
/* Revoke and detach every PTE for one shared object page.  The object page
 * must be BUSY, which prevents a new fault publication while the PTE
 * shootdowns run without VM/object locks. */
int vmspace_object_page_revoke(struct vm_object_page *, uint32_t *);
int vmspace_pin_user_pages(struct vmspace *, uintptr_t, size_t, uint32_t,
	struct vmspace_pinned_page *, size_t);
void vmspace_unpin_user_pages(struct vmspace_pinned_page *, size_t);
int vmspace_wire_range(struct vmspace *, uintptr_t, size_t, uint32_t);
void vmspace_unwire_range(struct vmspace *, uintptr_t, size_t);
int vmspace_copy_to(struct vmspace *, uintptr_t, const void *, size_t);
int vmspace_copy_from(struct vmspace *, void *, uintptr_t, size_t);
/* Release a reference.  The ordinary form may sleep while destroying it. */
void vmspace_put(struct vmspace *);
/* Scheduler retirement uses the non-sleeping form; a reaper destroys it. */
void vmspace_put_deferred(struct vmspace *);
/* Register the retained, non-sleeping wakeup used by the deferred reaper.
 * Registration is process-lifetime: notify and its argument must remain valid
 * until replaced.  Passing NULL unregisters it. */
void vmspace_set_reaper_notify(void (*)(void *), void *);
unsigned vmspace_reap_pending(void);
unsigned vmspace_count(void);
int vmspace_set_address_limit(struct vmspace *, uint64_t);
int vmspace_set_data_limit(struct vmspace *, uint64_t);
void vmspace_set_stack_limit(struct vmspace *, uint64_t);
uint64_t vmspace_address_cap(void);

uint32_t vm_page_effective_prot(const struct vm_page *);
int vm_private_page_is_resident(const struct vm_page *);
uintptr_t vm_private_page_vaddr(const struct vm_page *);
void vm_page_replace_private(struct vm_page *, struct vm_private_page *);
/* Success keeps a short backing operation active until the caller publishes
 * the new reverse/PTE metadata and calls vm_private_page_operation_end(). */
int vm_page_share_private(struct vm_page *, struct vm_page *);

#endif
