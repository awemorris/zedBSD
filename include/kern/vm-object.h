/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * VM object
 *
 * Shared file-backed virtual-memory objects.
 */

#ifndef ZEDBSD_KERN_VM_OBJECT_H
#define ZEDBSD_KERN_VM_OBJECT_H

#include <hal/hal.h>
#include <kern/atomic.h>
#include <kern/lock.h>
#include <kern/waitq.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct file;
struct inode;
struct mount;
struct vm_page;
struct writeback_budget;
struct writeback_ticket;

#define VM_OBJECT_PAGE_DIRTY	0x0001U
#define VM_OBJECT_PAGE_BUSY	0x0002U
#define VM_OBJECT_PAGE_WRITEBACK	0x0004U
#define VM_OBJECT_PAGE_ERROR	0x0008U
#define VM_OBJECT_PAGE_ORPHANED	0x0010U

#define VM_OBJECT_RETAINED_WRITEBACK	0x00000001U
#define VM_OBJECT_DETACHING	0x00000002U
#define VM_OBJECT_RESIZING	0x00000004U
#define VM_OBJECT_CONTENT	0x00000008U
#define VM_OBJECT_ANONYMOUS	0x00000010U
#define VM_OBJECT_CACHE_REFERENCE	0x00000020U

/* Caller-owned speculative fill; zero-initialize and never copy a live token.
 * Request whole pages; prepare clips the final page only at authoritative EOF. */
#define VM_OBJECT_PREFETCH_PAGES 16U
struct disk;
struct vm_object;
struct vm_object_page;
struct vm_object_prefetch {
	struct vm_object *object;
	struct disk *disk;
	struct vm_object_page *pages[VM_OBJECT_PREFETCH_PAGES];
	off_t offset;
	size_t length;
	uint64_t size_generation;
	uint64_t content_generation;
	unsigned count;
};
int vm_object_prefetch_prepare(struct inode *inode, off_t offset, size_t length, struct vm_object_prefetch *fill);
/* Valid completion consumes the token, including short/error/stale results. */
int vm_object_prefetch_complete(struct vm_object_prefetch *fill, const void *bytes, ssize_t received, size_t *published);
void vm_object_prefetch_abort(struct vm_object_prefetch *fill);

int vm_object_read_coherent_useful(struct inode *, off_t, void *, size_t, ssize_t *, size_t *);

size_t vm_object_reclaim_clean(size_t target);

struct vm_object_page {
	uint64_t prefetch_size_generation;
	unsigned prefetch_valid;
	unsigned prefetch_frontier;
	unsigned prefetch_used;
	/* Optional ordinary-write dirty ownership; separate from physical charge. */
	struct writeback_budget *writeback_budget;
	struct vm_object *owner;
	struct vm_page_slab *metadata_slab;
	off_t offset;
	struct hal_pmem pmem;
	unsigned flags;
	int error;
	unsigned mapping_count;

	/*
	 * Pins the page between fault lookup and page-table publication.
	 */
	unsigned hold_count;

	/*
	 * Subset of hold_count owned by page-vector/uaccess pins.
	 */
	unsigned pin_count;
	uint64_t dirty_generation;
	uint64_t write_generation;
	uint64_t write_dirty_generation;
	uint64_t content_generation;
	struct vm_page *mappings;
	struct vm_object_page *next;

	/* Intrusive ordered lookup; page identity stays stable across rotations. */
	struct vm_object_page *index_left;
	struct vm_object_page *index_right;
	unsigned index_height;
	struct vm_object_page *dirty_previous;
	struct vm_object_page *dirty_next;
	unsigned dirty_linked;
};

struct vm_object {
	uint64_t registry_generation;
	/*
	 * One registry reference plus one reference for every mapped region.
	 */
	refcount_t refs;

	/*
	 * Protected by the VM object registry lock; zero permits retention.
	 */
	unsigned mapping_count;

	/*
	 * Metadata-using operations which final put must drain before detach.
	 */
	unsigned active_operations;

	/* Registry-protected subset owning only unpublished prefetch frames. */
	unsigned prefetch_operations;

	/*
	 * Lookup lifetime holds waiting for DETACHING to finish.
	 */
	unsigned registry_waiters;

	struct spinlock lock;
	struct wait_queue page_waitq;

	/*
	 * Wakes inode lookups after final-mapping teardown leaves DETACHING.
	 */
	struct wait_queue registry_waitq;
	uint64_t generation;

	/*
	 * Authoritative EOF for faults/writeback while an object is published.
	 */
	off_t logical_size;

	uint64_t size_generation;
	uint64_t resize_generation;
	uint64_t content_generation;
	unsigned flags;

	/*
	 * Anonymous shared objects reserve their backing commitment once.  A
	 * fork adds mapping references without charging the same object again.
	 */
	size_t commit_size;
	/*
	 * file is retained for reads; write_file carries write capability.
	 */
	struct file *file;

	struct file *write_file;
	struct inode *inode;
	struct vm_object_page *pages;
	struct vm_object_page *page_index;
	struct vm_object_page *dirty_pages;

	/*
	 * Resize-discarded pages retained only until pre-existing pins drain.
	 */
	struct vm_object_page *orphan_pages;

	int writeback_error;
	struct vm_object *next;
};

/*
 * One EOF-changing filesystem operation.  Begin/commit/abort are called with
 * inode->i_io_lock held.  Prepare is deliberately called without that lock:
 * it may wait for a fault or writeback which was already committed to taking
 * i_io_lock when begin published the transaction.
 */
struct vm_object_resize {
	struct inode *inode;
	off_t old_size;
	off_t target_size;
	uint64_t generation;
	unsigned active;
	unsigned prepared;
};

/*
 * A normal filesystem write and its resident MAP_SHARED cache update.  Begin
 * and finish run with inode->i_io_lock held.  Prepare drops that mutex while
 * it revokes PTEs and performs old-dirty writeback.  A positive backend result
 * is committed as exactly that prefix; a negative result is aborted.
 */
struct vm_object_content {
	/* Borrowed only during a successfully prepared delayed content commit. */
	struct writeback_ticket *writeback_ticket;
	struct inode *inode;
	struct vm_object_resize *resize_owner;
	off_t offset;
	size_t length;
	uint64_t generation;
	unsigned active;
	unsigned prepared;
};

struct backing_claim;
int vm_object_backing_busy(const struct backing_claim *);

/* Optional clean retention; prepare runs before inode/position locks. */
void vm_object_cache_prepare(struct file *file);
int vm_object_cache_pin(struct inode *inode, struct vm_object **result);
void vm_object_cache_unpin(struct vm_object *object);
/* Caller owns a cache pin and no page pointer; other page users exclude reclaim. */
size_t vm_object_cache_reclaim_pinned(struct vm_object *object, size_t target);
int vm_object_writeback_prepare(struct file *file, struct vm_object **result);
void vm_object_writeback_release(struct vm_object *object);
int vm_object_content_prepare_delayed(struct vm_object_content *content, struct vm_object *object, struct writeback_ticket *ticket);
unsigned vm_object_cache_drain(struct mount *mount);
/* Non-destructive preflight; caller excludes new mount users through later commit. */
int vm_object_discard_mount_check(struct mount *mount);
/* Closed admission required. NULL inode counts mount paths; otherwise inode paths. */
int vm_object_discard_mount_refs(struct mount *, struct inode *, unsigned *);
/* Commit only after closed admission and complete cross-layer preflight. */
int vm_object_discard_mount(struct mount *mount, uint64_t *dirty_bytes);

int
vm_object_get_shared(
	struct file *file,
	struct vm_object **result);

int
vm_object_create_anonymous(
	size_t size,
	struct vm_object **result);

void
vm_object_ref(
	struct vm_object *object);

void
vm_object_put(
	struct vm_object *object);

int
vm_object_fault(
	struct vm_object *object,
	off_t offset,
	struct vm_object_page **result);

/*
 * Pin object-page storage and its owner independently of reverse-mapping
 * metadata.  A page-vector uaccess pin may therefore survive concurrent
 * unmap/final vm_object_put.  Resize may orphan the old frame instead of
 * waiting for it; final detach waits for the accompanying operation/ref.
 * A pinned frame is read or modified through the helpers below so writes are
 * linearized against writeback/orphan publication.
 */
int
vm_object_page_pin(
	struct vm_object_page *page);

void
vm_object_page_unpin(
	struct vm_object_page *page);

int
vm_object_page_pin_read(
	struct vm_object_page *page,
	size_t offset,
	void *buffer,
	size_t length);

int
vm_object_page_pin_write(
	struct vm_object_page *page,
	size_t offset,
	const void *buffer,
	size_t length);

void
vm_object_fault_release(
	struct vm_object_page *page);

void
vm_object_mapping_add(
	struct vm_object_page *object_page,
	struct vm_page *mapping);

void
vm_object_mapping_remove(
	struct vm_object_page *object_page,
	struct vm_page *mapping);
/*
 * VM reverse-mapping transactions use this only while vm_metadata and the
 * owning object's lock are both held.
 */
void
vm_object_mapping_remove_locked(
	struct vm_object_page *object_page,
	struct vm_page *mapping);

void
vm_object_mark_dirty(
	struct vm_object_page *page);

int
vm_object_sync_range(
	struct vm_object *object,
	off_t offset,
	size_t size,
	int flags);

/* Caller-owned scratch allows an independent syncer to drain under pressure. */
int vm_object_sync_inode_buffer(struct inode *inode, void *scratch, size_t capacity);
int vm_object_sync_mount_buffer(struct mount *mount, void *scratch, size_t capacity);

int
vm_object_sync_inode(
	struct inode *inode);

int
vm_object_inode_io_wait(
	struct inode *inode);

int
vm_object_inode_resize_active(
	struct inode *inode);

int
vm_object_content_read_begin(
	struct inode *inode);

void
vm_object_content_read_end(
	struct inode *inode);

/*
 * Registry snapshot used under i_io_lock to close the no-object fallback
 * race.  Returns one for a published cache, zero for none, or -errno.
 */
int
vm_object_cache_published(
	struct inode *inode);

int
vm_object_content_begin(
	struct file *file,
	off_t offset,
	size_t length,
	struct vm_object_resize *resize_owner,
	struct vm_object_content *content);

int
vm_object_content_prepare(
	struct vm_object_content *content);

void
vm_object_content_commit(
	struct vm_object_content *content,
	const void *buffer,
	size_t committed);

void
vm_object_content_abort(
	struct vm_object_content *content);
/*
 * Return ENOENT when no published cache exists; callers may then use the
 * backend while retaining i_io_lock.
 */
int
vm_object_read_coherent(
	struct inode *inode,
	off_t offset,
	void *buffer,
	size_t length,
	ssize_t *result);

int
vm_object_resize_begin(
	struct inode *inode,
	off_t target_size,
	struct vm_object_resize *resize);

int
vm_object_resize_prepare(
	struct vm_object_resize *resize);

void
vm_object_resize_commit(
	struct vm_object_resize *resize,
	off_t logical_size);

void
vm_object_resize_abort(
	struct vm_object_resize *resize);

/*
 * Transitional test/helper entry point.  Production EOF mutations use the
 * begin/prepare/commit transaction through inode_truncate/file I/O.
 */
void
vm_object_truncate_inode(
	struct inode *inode,
	off_t size);

int
vm_object_reclaim_one(void);

unsigned
vm_object_count(void);

unsigned
vm_object_page_count(void);

unsigned
vm_object_retained_count(void);

#endif
