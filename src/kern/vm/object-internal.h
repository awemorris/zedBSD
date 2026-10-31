/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declarations private to the object side of the VM: the registry, content,
 * fault, sync, cache, prefetch and page parts.  These connect object.c,
 * object-content.c, object-fault.c, object-sync.c, object-cache.c,
 * object-prefetch.c and object-page.c and are not part of the kernel's
 * public VM API.
 *
 * Internal VM API: used only between the files in src/kern/vm/.  Its
 * compatibility is not guaranteed and it may change at any time.  None of
 * these declarations belongs in the public headers under include/kern.
 *
 * The registry below is guarded by the spin word taken through
 * vm_object_registry_lock() and vm_object_registry_unlock() (object.c),
 * with interrupts disabled.  Function bodies and the order they take the
 * VM locks are as they were in vm.c: the metadata lock before any other VM
 * lock (kern/vm-lock.h).
 */

#ifndef KERN_VM_OBJECT_INTERNAL_H
#define KERN_VM_OBJECT_INTERNAL_H

#include "kern/vm-object.h"
#include "kern/cache-memory.h"
#include "kern/lock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct file;
struct inode;
struct mount;

#define PAGE_SIZE			KERN_PAGE_SIZE
#define VM_OBJECT_DATA			__attribute__((section(".vfs_bss")))
#define VM_OBJECT_FAULT_RECLAIM_RETRIES	4U
#define VM_OBJECT_BACKING_SNAPSHOT_MAX	1024U
#define VM_OBJECT_CACHE_OBJECTS		256U
#define VM_OBJECT_CACHE_PAGES		16U /* Bounded transfer vector, not retention. */
/* A read with at least MIN bytes left goes to its buffer in runs of up to MAX bytes, not through pages. */
#define VM_OBJECT_READ_THROUGH_MIN	(256U * 1024U)
#define VM_OBJECT_READ_THROUGH_MAX	(1024U * 1024U)
#define VM_OBJECT_HASH_BITS		10U
#define VM_OBJECT_HASH_BUCKETS		(1U << VM_OBJECT_HASH_BITS)

/* How many VM_FREE_WAIT_MS waits an object-page fault makes before it gives up, ten seconds. */
#define VM_OBJECT_FAULT_WAITS		1000U

/*
 * The registry of shared objects, in most-recently-published order.
 *
 * A file-backed object lives here for as long as any mapping, cache
 * reference or write-back holds it, so a second mapping of the same inode
 * finds the object that already exists.
 */
extern struct vm_object *shared_objects;

/*
 * The registry's objects again, chained by the inode they cache.
 *
 * A file read or fault finds its inode's object here instead of walking
 * every published object with interrupts disabled.  The chains hold
 * exactly the objects of shared_objects, and change under the same lock.
 */
extern struct vm_object *object_hash[VM_OBJECT_HASH_BUCKETS];

/*
 * How many objects the registry holds.
 */
extern unsigned object_count;

/*
 * The generation stamped on the object published next.
 *
 * It only ever increases, so a stale reference to a slot that has been
 * reused is recognized by the generation that came with it.
 */
extern uint64_t object_registry_generation;

/*
 * The use count stamped on an object each time it is admitted or referenced.
 *
 * It only ever increases under the registry lock, so the cache object with
 * the lowest stamp is the least recently used one.
 */
extern uint64_t object_use_generation;

/*
 * How many registry entries exist only because the page cache asked for them.
 *
 * These are bounded by VM_OBJECT_CACHE_OBJECTS: an optional admission is
 * refused once that many are already held.
 */
extern unsigned cache_objects;

/*
 * How many pages every object holds together.
 */
extern atomic_uint_t object_pages;

/*
 * Defined in object.c, part of the registry of shared objects and their
 * lifetime: admission, lookup by inode, references, generations and
 * destruction.
 */
int vm_object_get_shared_internal(struct file *file, struct vm_object **result, int cache_only);
bool vm_object_registry_lock(void);
void vm_object_registry_unlock(bool enabled);
struct vm_object *vm_object_find_by_inode_locked(struct inode *inode);
int vm_object_wait_registry_transition(struct vm_object *object, uint64_t sequence);
void vm_object_wake_registry_waiters(struct vm_object *object);
int vm_object_operation_begin(struct vm_object *object);
void vm_object_operation_end(struct vm_object *object);
uint64_t vm_object_next_generation(struct vm_object *object);
uint64_t vm_object_next_inode_resize_generation_locked(struct inode *inode);
uint64_t vm_object_next_inode_content_generation_locked(struct inode *inode);
int vm_object_can_destroy(struct vm_object *object);
int vm_object_unlink_locked(struct vm_object *object);
unsigned vm_object_hash_index(const struct inode *inode);
void vm_object_destroy(struct vm_object *object);
void vm_object_retain(struct vm_object *object, int error);
int vm_object_cache_retainable(struct vm_object *object);
int vm_object_inode_unnamed(const struct inode *inode);

/*
 * Defined in object-page.c, part of where object pages live: the
 * descriptor slabs, page storage, the dirty and busy tests and the AVL
 * index of an object's pages.
 */
struct vm_object_page *vm_object_find_page(struct vm_object *object, off_t offset);
int vm_object_unlink_page_locked(struct vm_object_page **head, struct vm_object_page *page);
int vm_object_page_overlaps(const struct vm_object_page *page, uint64_t start, uint64_t end);
int vm_object_has_dirty_pages_locked(struct vm_object *object);
int vm_object_has_busy_pages_locked(const struct vm_object *object);
int vm_object_has_busy_pages(struct vm_object *object);
void vm_object_clear_page_dirty_locked(struct vm_object_page *page);
void vm_object_free_page(struct vm_object_page *page);
struct vm_object_page *vm_object_alloc_page(struct vm_object *object, off_t offset, int optional);
void vm_object_release_page_storage(struct vm_object_page *page);
void vm_object_page_index_insert(struct vm_object *object, struct vm_object_page *page);
void vm_object_page_index_remove(struct vm_object *object, struct vm_object_page *page);

/*
 * Defined in object-content.c, part of changes to an object's content and
 * size: the content and resize transactions, inode I/O waits and coherent
 * reads.
 */
void vm_object_initialize_eof_locked(struct vm_object *object, struct inode *inode);
int vm_object_wait_resize(struct vm_object *object);
void vm_object_content_release_pages_locked(struct vm_object *object, uint64_t generation);

/*
 * Defined in object-sync.c, part of synchronizing dirty object pages to
 * their inode, write-back preparation and the delayed content path.
 */
void vm_object_record_writeback_error_locked(struct vm_object *object, int error);
int vm_object_write_page_data(struct vm_object *object, struct vm_object_page *page, off_t logical_size, int inode_io_owned);
int vm_object_sync_range_internal(struct vm_object *object, off_t offset, size_t size, int flags, int detaching, int resize_owner, off_t resize_target);
void vm_object_page_dirty_link(struct vm_object_page *page);
void vm_object_page_dirty_unlink(struct vm_object_page *page);
void vm_object_page_dirty_mark(struct vm_object_page *page);

/*
 * Defined in object-fault.c, part of faults on object pages, page pins and
 * the mappings an object records.
 */
int vm_object_range_has_wired_mapping(struct vm_object *object, uint64_t start, uint64_t end);

/*
 * Defined in object-cache.c, part of the object page cache: its counts,
 * pins and drains, the discard of a mount's objects and the reclaim of
 * clean object pages.
 */
int vm_object_cache_evict_one(struct mount *mount);
int vm_object_mount_reserved(const struct vm_object *object);
void vm_object_cache_discard_prepared(struct vm_object_page **pages, unsigned count);
int vm_object_cache_read_missing(struct vm_object *object, off_t offset, void *buffer, size_t length, ssize_t *result, int *handled, int *stop);

/*
 * Defined in object-prefetch.c, part of read-ahead of object pages:
 * preparing, completing and abandoning a prefetch.
 */
size_t vm_object_prefetch_consume(struct vm_object_page *page, size_t start, size_t length);
void vm_object_prefetch_retire(struct vm_object_page *page);

#endif
