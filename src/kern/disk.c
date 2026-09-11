/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The disk registry and block I/O submission.
 *
 * Disks live in a fixed table of slots named sdX or nvmeCnN.  A
 * partition is a child disk that maps its blocks onto an offset of its
 * parent, so every bio is resolved down to the leaf that owns the
 * storage before it is handed to that leaf's driver.  Direct transfers
 * bypass the buffer cache; filesystem reads and writes go through it.
 */

#include "kern/disk.h"
#include "kern/io-stats.h"
#include "kern/backing-claim.h"
#include "kern/buf.h"
#include "kern/sched.h"
#include "kern/atomic.h"
#include "kern/test-checkpoint.h"
#include <kern/bio-async.h>
#include <kern/cache-memory.h>
#include <kern/io-pool.h>
#include <kern/inode.h>
#include <kern/thread.h>
#include <kern/page.h>
#include <errno.h>
#include <limits.h>
#include <hal/hal.h>
#include <kern/pmem.h>
#include <string.h>
#include <uapi/block.h>

#define DISK_ALLOCATED		1U
#define DISK_LIVE		2U
#define DISK_GONE		3U
#define DISK_HIGH		__attribute__((section(".hightext")))
#define ASYNC_ENDPOINTS		4U
#define ASYNC_SLOTS		4U
#define ASYNC_QUEUE_LIMIT	2U
#define ASYNC_OFF		0U
#define ASYNC_BUILDING		1U
#define ASYNC_LIVE		2U
#define ASYNC_STOPPING		3U
#define REQUEST_FREE		0U
#define REQUEST_PREPARING	1U
#define REQUEST_READY		2U
#define REQUEST_QUEUED		3U
#define REQUEST_RUNNING		4U
#define REQUEST_DONE		5U
#define REQUEST_RELEASING	6U

struct bio_async_request {
	struct bio bio;
	struct bio_async_endpoint *endpoint;
	struct bio_async_request *next;
	struct backing_mutation_guard guard;
	const struct backing_claim *authorization;
	struct disk *disk;
	struct disk *cache_token;
	struct io_context context;
	refcount_t refs;
	void *payload;
	bio_async_callback callback;
	void *argument;
	uint64_t epoch;
	size_t transferred;
	int error;
	unsigned state;
	unsigned callback_active;
};

struct bio_async_endpoint {
	struct spinlock lock;
	struct wait_queue wake;
	struct disk *leaf;
	struct disk *cache_token;
	struct kern_pmem memory;
	struct bio_async_request *requests;
	struct bio_async_request *head;
	struct bio_async_request *tail;
	unsigned state;
	unsigned used;
	unsigned queued;
	unsigned started;
};

extern struct thread *thread_current(void) __attribute__((weak));
extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));
extern void io_error_record(struct io_error_state *, int) __attribute__((weak));
extern void *io_pool_borrow(size_t, size_t *) __attribute__((weak));
extern void io_pool_release(void *) __attribute__((weak));

static struct disk disks[DISK_MAX];
static uint8_t disk_used[DISK_MAX];
static struct disk *disk_head;
static unsigned live_count;
static unsigned admin_count;
static dev_t next_dev = 1;
static atomic_uint_t disk_registry_lock;
struct bio_async_endpoint;
static atomic_uint_t async_initialized;
static struct spinlock async_registry;
static struct bio_async_endpoint async_endpoints[ASYNC_ENDPOINTS];

static void disk_write_accept(struct disk *leaf, struct bio *bio);
static void disk_write_retire(struct disk *leaf, struct bio *bio, int error, size_t transferred);
static void disk_persistence_invalidate_locked(struct disk *leaf);
static bool disk_lock(void);
static void disk_unlock(bool enabled);
static int disk_index(const struct disk *disk);
static void zero_bytes(void *p, size_t n);
static int name_equal(const char *a, const char *b);
static int name_valid(const char name[DISK_NAME_MAX]);
static int sd_name(char name[DISK_NAME_MAX], unsigned number);
static int name_append_unsigned(char name[DISK_NAME_MAX], unsigned *at, unsigned value);
static void disk_copy_info(const struct disk *disk, struct disk_info *info);
static int bio_admit(struct disk *disk, struct bio *bio);
static int bio_dispatch(struct bio *bio);
static int disk_transfer_direct(struct disk *disk, enum bio_op op, uint64_t block, uint32_t count, void *data, const struct backing_claim *claim, uint32_t *completed, const struct io_context *context);
static struct disk *disk_leaf(struct disk *disk);
static int disk_reload_idle(struct disk *parent);
static int disk_admin_idle(struct disk *target);
static int disk_admin_conflict_locked(struct disk *, uint64_t, uint64_t, int);
static int disk_media_idle_locked(struct disk *parent, unsigned extra);
static int disk_reload_replace_locked(struct disk *parent, struct disk **new_disks, unsigned count);
static int disk_cache_enter(struct disk *disk, struct disk **leaf_out);
static void disk_cache_leave(struct disk *leaf);
static int disk_cached_transfer(struct disk *disk, uint64_t block, uint32_t count, void *data, int write, struct buf_view *view, const struct io_context *context);
static int async_initialize(void);
static void async_worker(void *argument);
static void async_complete(struct bio *bio);
static void async_unlink_locked(struct bio_async_endpoint *endpoint, struct bio_async_request *request);

_Static_assert(DISK_NAME_MAX >= sizeof("nvme0n4294967295"), "DISK_NAME_MAX must represent every 32-bit NVMe namespace ID");

/* Computes a canonical interval without acquiring a second registry lock.
 * Allocated prospective children are supported for namespace admission. */
static int
disk_admin_span(struct disk *disk, uint64_t block, uint64_t count,
	struct disk **leaf, uint64_t *first)
{
	struct disk *parent;
	unsigned depth = 0;

	if (disk == NULL || count == 0 || block >= disk->d_block_count ||
	    count > disk->d_block_count - block)
		return EINVAL;
	while (disk->d_parent != NULL) {
		parent = disk->d_parent;
		if (++depth > DISK_MAX || disk->d_block_size != parent->d_block_size ||
		    block > UINT64_MAX - disk->d_parent_offset)
			return EINVAL;
		block += disk->d_parent_offset;
		if (block >= parent->d_block_count || count > parent->d_block_count - block)
			return EINVAL;
		disk = parent;
	}
	*leaf = disk;
	*first = block;
	return 0;
}

/* A closed logical volume excludes every overlapping physical alias. */
static int
disk_admin_conflict_locked(struct disk *disk, uint64_t block, uint64_t count,
	int allow_owner)
{
	struct disk *leaf;
	struct disk *reserved_leaf;
	struct disk *reserved;
	uint64_t first;
	uint64_t reserved_first;
	struct thread *thread;

	/* Ordinary I/O pays no registry scan when administration is inactive. */
	if (admin_count == 0)
		return 0;
	if (disk_admin_span(disk, block, count, &leaf, &first) != 0)
		return EBUSY;
	thread = thread_current != NULL ? thread_current() : NULL;
	for (reserved = disk_head; reserved != NULL; reserved = reserved->d_next) {
		if (reserved->d_admin_owner == NULL)
			continue;
		if (disk_admin_span(reserved, 0, reserved->d_block_count,
		    &reserved_leaf, &reserved_first) != 0)
			return EBUSY;
		if (leaf != reserved_leaf || first >= reserved_first + reserved->d_block_count ||
		    reserved_first >= first + count)
			continue;
		if (!allow_owner || thread == NULL || reserved->d_admin_thread != thread ||
		    first < reserved_first || count > reserved->d_block_count - (first - reserved_first))
			return EBUSY;
	}
	return 0;
}

/* Requires one target description and no other overlapping volume owner. */
static int
disk_admin_idle(struct disk *target)
{
	struct disk *leaf;
	struct disk *other_leaf;
	struct disk *other;
	uint64_t first;
	uint64_t other_first;

	if (disk_admin_span(target, 0, target->d_block_count, &leaf, &first) != 0)
		return EINVAL;
	if (target == leaf)
		return disk_reload_idle(target);
	/* Initial quiescence is conservatively leaf-wide; later disjoint I/O is allowed. */
	if (leaf->d_inflight != 0 || leaf->d_cache_users != 0)
		return EBUSY;
	for (other = disk_head; other != NULL; other = other->d_next) {
		if (disk_admin_span(other, 0, other->d_block_count, &other_leaf, &other_first) != 0)
			return EBUSY;
		if (leaf != other_leaf || first >= other_first + other->d_block_count ||
		    other_first >= first + target->d_block_count)
			continue;
		if (other->d_open_count != (other == target ? 1U : 0U) ||
		    other->d_opening != 0 || other->d_closing != 0)
			return EBUSY;
		/* The physical parent's references include its nonoverlapping children. */
		if (other != target && other != leaf && refcount_load(&other->d_refs) != 1)
			return EBUSY;
	}
	return 0;
}

/* Closes new-open admission while one description owns the raw backing claim. */
int
disk_admin_begin(
	struct disk *parent,
	const struct backing_claim *owner)
{
	bool enabled;
	int error;

	enabled = disk_lock();
	if (parent == NULL || owner == NULL || disk_index(parent) < 0) {
		error = EINVAL;
	} else if (parent->d_state != DISK_LIVE || disk_media_status(parent) != 0) {
		error = ENXIO;
	} else if (parent->d_media_backing != NULL ||
	    (parent->d_flags & DISK_FILE_BACKED) != 0 ||
	    (parent->d_parent != NULL &&
	    (parent->d_parent->d_parent != NULL || (parent->d_flags & DISK_PARTITION) == 0 ||
	    (parent->d_parent->d_flags & DISK_FILE_BACKED) != 0))) {
		error = EOPNOTSUPP;
	} else if (((parent->d_flags | disk_leaf(parent)->d_flags) & DISK_READ_ONLY) != 0) {
		error = EROFS;
	} else if (disk_leaf(parent)->d_reload_owner != NULL ||
	    disk_admin_conflict_locked(parent, 0, parent->d_block_count, 0) != 0) {
		error = EBUSY;
	} else {
		error = disk_admin_idle(parent);
		if (error == 0) {
			parent->d_admin_owner = owner;
			admin_count++;
		}
	}
	disk_unlock(enabled);
	return error;
}

/* Permits one serialized syscall to use the reserved disk and its cache. */
int
disk_admin_io_begin(
	struct disk *parent,
	const struct backing_claim *owner)
{
	struct thread *thread;
	bool enabled;
	int error;

	thread = thread_current != NULL ? thread_current() : NULL;
	enabled = disk_lock();
	if (parent == NULL || parent->d_state != DISK_LIVE || disk_media_status(parent) != 0) {
		error = ENXIO;
	} else if (owner == NULL || parent->d_admin_owner != owner || thread == NULL) {
		error = EINVAL;
	} else if (parent->d_admin_thread != NULL) {
		error = EBUSY;
	} else {
		parent->d_admin_thread = thread;
		error = 0;
	}
	disk_unlock(enabled);
	return error;
}

void
disk_admin_io_end(
	struct disk *parent,
	const struct backing_claim *owner)
{
	bool enabled;

	enabled = disk_lock();
	if (parent != NULL && parent->d_admin_owner == owner &&
	    thread_current != NULL && parent->d_admin_thread == thread_current())
		parent->d_admin_thread = NULL;
	disk_unlock(enabled);
}

/* Called only after the description's final operation has completed. */
void
disk_admin_end(
	struct disk *parent,
	const struct backing_claim *owner)
{
	bool enabled;

	enabled = disk_lock();
	if (parent != NULL && owner != NULL && parent->d_admin_owner == owner) {
		parent->d_admin_owner = NULL;
		admin_count--;
	}
	disk_unlock(enabled);
}

/*
 * Reserves one otherwise idle physical disk for partition-table reload.
 */
int
disk_reload_begin(
	struct disk *parent)
{
	return disk_reload_begin_claimed(parent, NULL);
}

int
disk_reload_begin_claimed(
	struct disk *parent,
	const struct backing_claim *claim)
{
	struct thread *owner;
	bool enabled;
	int error;

	/* Pins the reload to the current thread under the registry lock. */
	owner = thread_current != NULL ? thread_current() : NULL;
	enabled = disk_lock();
	if (parent == NULL ||
	    disk_index(parent) < 0 ||
	    parent->d_state != DISK_LIVE ||
	    disk_media_status(parent) != 0 ||
	    parent->d_parent != NULL ||
	    (parent->d_flags & DISK_PARTITION) != 0 ||
	    owner == NULL) {
		error = EINVAL;
	} else if (parent->d_reload_owner != NULL ||
	    (parent->d_admin_owner != NULL && parent->d_admin_owner != claim) ||
	    disk_admin_conflict_locked(parent, 0, parent->d_block_count, 1) != 0) {
		error = EBUSY;
	} else {
		error = disk_reload_idle(parent);
		if (error == 0)
			parent->d_reload_owner = owner;
	}

	disk_unlock(enabled);

	/* Reports whether this caller acquired the admission gate. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Releases the current thread's partition reload reservation.
 */
void
disk_reload_end(
	struct disk *parent)
{
	bool enabled;

	/* Only the reserving thread may reopen ordinary disk admission. */
	enabled = disk_lock();
	if (parent != NULL &&
	    thread_current != NULL &&
	    parent->d_reload_owner == thread_current())
		parent->d_reload_owner = NULL;

	disk_unlock(enabled);
}

/*
 * Replaces an idle disk's complete child namespace in one checked commit.
 */
int
disk_reload_replace(
	struct disk *parent,
	struct disk **new_disks,
	unsigned count)
{
	bool enabled;
	int error;

	/* Keeps validation and publication under one registry transaction. */
	enabled = disk_lock();
	error = disk_reload_replace_locked(parent, new_disks, count);
	disk_unlock(enabled);

	/* Reports the unchanged namespace or the completed replacement. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Allocates a registry slot for a disk that is not yet published.
 */
struct disk *
disk_alloc(
	void)
{
	unsigned i;
	bool enabled;

	enabled = disk_lock();

	/* Takes the first free slot and initializes it. */
	for (i = 0; i < DISK_MAX; i++) {
		if (disk_used[i])
			continue;
		disk_used[i] = 1;
		zero_bytes(&disks[i], sizeof(disks[i]));
		disks[i].d_state = DISK_ALLOCATED;
		refcount_init(&disks[i].d_refs, 1);
		spin_init(&disks[i].d_lock, LOCK_RANK_DISK, "disk");
		waitq_init(&disks[i].d_waitq, "disk state");
		disk_unlock(enabled);
		return &disks[i];
	}

	disk_unlock(enabled);

	/* Reports a full registry. */
	return NULL;
}

/*
 * Gives an allocated disk the first unused sdX name.
 */
int
disk_alloc_sd_name(
	struct disk *disk)
{
	char candidate[DISK_NAME_MAX];
	unsigned number;
	unsigned i;
	bool enabled;
	int used;

	/* Rejects a missing disk or one that is not merely allocated. */
	if (disk == NULL)
		return EINVAL;
	enabled = disk_lock();
	if (disk_index(disk) < 0 || disk->d_state != DISK_ALLOCATED) {
		disk_unlock(enabled);
		return EINVAL;
	}

	/* Tries each name in order until one is unused by another slot. */
	for (number = 0; number < DISK_MAX; number++) {
		used = 0;
		if (sd_name(candidate, number) != 0)
			break;
		for (i = 0; i < DISK_MAX; i++) {
			if (disk_used[i] && &disks[i] != disk &&
			    name_equal(disks[i].d_name, candidate)) {
				used = 1;
				break;
			}
		}

		if (!used) {
			for (i = 0; i < DISK_NAME_MAX; i++) {
				disk->d_name[i] = candidate[i];
				if (candidate[i] == '\0')
					break;
			}

			disk_unlock(enabled);
			return 0;
		}
	}

	disk_unlock(enabled);

	/* Reports that every name is taken. */
	return ENOSPC;
}

/*
 * Gives an allocated disk its nvmeCnN name.
 */
int
disk_alloc_nvme_name(
	struct disk *disk,
	unsigned controller,
	unsigned namespace_id)
{
	char candidate[DISK_NAME_MAX];
	unsigned at;
	unsigned i;
	bool enabled;

	at = 0;

	/* Rejects a missing disk or the reserved namespace zero. */
	if (disk == NULL || namespace_id == 0)
		return EINVAL;

	/* Builds the name from the controller and namespace numbers. */
	candidate[at++] = 'n';
	candidate[at++] = 'v';
	candidate[at++] = 'm';
	candidate[at++] = 'e';
	if (name_append_unsigned(candidate, &at, controller) != 0 ||
	    at + 1U >= DISK_NAME_MAX)
		return ENAMETOOLONG;
	candidate[at++] = 'n';
	if (name_append_unsigned(candidate, &at, namespace_id) != 0)
		return ENAMETOOLONG;
	candidate[at] = '\0';

	/* The disk must be merely allocated and the name unused. */
	enabled = disk_lock();
	if (disk_index(disk) < 0 || disk->d_state != DISK_ALLOCATED) {
		disk_unlock(enabled);
		return EINVAL;
	}

	for (i = 0; i < DISK_MAX; i++) {
		if (disk_used[i] && &disks[i] != disk &&
		    name_equal(disks[i].d_name, candidate)) {
			disk_unlock(enabled);
			return EEXIST;
		}
	}

	for (i = 0; i <= at; i++)
		disk->d_name[i] = candidate[i];
	disk_unlock(enabled);

	/* Reports the assigned name. */
	return 0;
}

/*
 * Publishes an allocated disk in the registry.
 *
 * A disk without a parent needs a driver that can submit I/O; a
 * partition inherits its parent's driver.
 */
int
disk_create(
	struct disk *disk)
{
	struct disk **tail;
	struct disk *found;
	bool enabled;

	/* Rejects an incomplete description. */
	if (disk == NULL ||
	    disk->d_state != DISK_ALLOCATED ||
	    !name_valid(disk->d_name) ||
	    disk->d_block_size == 0 ||
	    disk->d_block_count == 0 ||
	    (disk->d_parent == NULL &&
	     (disk->d_ops == NULL || disk->d_ops->submit == NULL)))
		return EINVAL;

	/* The disk must be merely allocated and its name unique. */
	enabled = disk_lock();
	if (disk_index(disk) < 0 || disk->d_state != DISK_ALLOCATED) {
		disk_unlock(enabled);
		return EINVAL;
	}

	/* A reload owns every child namespace change beneath its physical disk. */
	if (disk->d_media_backing != NULL &&
	    (disk->d_parent != NULL || disk_index(disk->d_media_backing) < 0 ||
	     disk->d_media_backing->d_state != DISK_LIVE ||
	     disk_media_status(disk->d_media_backing) != 0)) {
		disk_unlock(enabled);
		return ENXIO;
	}

	if (disk->d_parent != NULL && disk_media_status(disk->d_parent) != 0) {
		disk_unlock(enabled);
		return ENXIO;
	}

	if (disk->d_parent != NULL &&
	    (disk_leaf(disk)->d_reload_owner != NULL ||
	    disk_admin_conflict_locked(disk, 0, disk->d_block_count, 0) != 0)) {
		disk_unlock(enabled);
		return EBUSY;
	}

	for (found = disk_head; found != NULL; found = found->d_next) {
		if (name_equal(found->d_name, disk->d_name)) {
			disk_unlock(enabled);
			return EEXIST;
		}
	}

	if (next_dev == 0) {
		disk_unlock(enabled);
		return ENOSPC;
	}

	/* Assigns the device number and appends the disk to the live list. */
	disk->d_dev = next_dev++;
	disk->d_state = DISK_LIVE;
	disk->d_next = NULL;
	if (disk->d_parent != NULL)
		refcount_get(&disk->d_parent->d_refs);
	if (disk->d_media_backing != NULL)
		refcount_get(&disk->d_media_backing->d_refs);
	tail = &disk_head;
	while (*tail != NULL)
		tail = &(*tail)->d_next;
	*tail = disk;
	live_count++;
	disk_unlock(enabled);

	/* Reports the published disk. */
	return 0;
}

/*
 * Removes a disk from the registry after its hardware went away.
 *
 * The disk slot stays allocated until every reference is dropped and
 * disk_destroy() frees it.
 */
void
disk_gone(
	struct disk *disk)
{
	struct disk **link;
	struct backing_mutation_guard guard;
	bool enabled;

	/* Ignores a missing disk or one whose backing cannot be quiesced. */
	if (disk == NULL ||
	    backing_mutation_begin_disk(disk,
					0,
					disk->d_block_count,
					NULL,
					&guard) != 0)
		return;

	/* Only a live disk is unlinked. */
	enabled = disk_lock();
	if (disk_leaf(disk)->d_reload_owner != NULL ||
	    disk_admin_conflict_locked(disk, 0, disk->d_block_count, 0) != 0)
		goto out;
	if (disk->d_state == DISK_GONE)
		goto out;
	if (disk->d_state != DISK_LIVE)
		goto out;
	for (link = &disk_head; *link != NULL; link = &(*link)->d_next) {
		if (*link == disk) {
			*link = disk->d_next;
			live_count--;
			break;
		}
	}

	disk->d_next = NULL;
	disk_persistence_invalidate(disk);
	disk->d_state = DISK_GONE;

out:
	disk_unlock(enabled);
	backing_mutation_end(&guard);
}

/*
 * Removes a live disk from the registry when nothing uses it.
 *
 * Open handles, in-flight I/O, or outside references keep the disk with
 * EBUSY, resident buffers are flushed and invalidated first.
 */
DISK_HIGH int
disk_gone_if_idle(
	struct disk *disk)
{
	struct disk **link;
	struct backing_mutation_guard guard;
	bool enabled;
	int error;

	/* Rejects a missing disk or one whose backing cannot be quiesced. */
	if (disk == NULL)
		return EINVAL;
	error = backing_mutation_begin_disk(disk, 0, disk->d_block_count, NULL,
					    &guard);
	if (error != 0)
		return error;

	/* A first idle check avoids flushing a disk that is plainly busy. */
	enabled = disk_lock();
	if (disk == NULL ||
	    disk_index(disk) < 0 ||
	    disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		error = ENXIO;
		goto out;
	}

	if (disk->d_open_count != 0 ||
	    disk->d_inflight != 0 ||
	    disk->d_opening != 0 ||
	    disk->d_closing != 0 ||
	    disk_leaf(disk)->d_reload_owner != NULL ||
	    disk_admin_conflict_locked(disk, 0, disk->d_block_count, 0) != 0) {
		disk_unlock(enabled);
		error = EBUSY;
		goto out;
	}

	disk_unlock(enabled);

	/*
	 * Resident buffers pin their leaf disk.  Flush and invalidate them
	 * before applying the final external-reference test.
	 */
	error = buf_invalidate_disk(disk, 0);
	if (error != 0)
		goto out;

	/* Unlinks the disk only when it is still live and unreferenced. */
	enabled = disk_lock();
	if (disk_index(disk) < 0 || disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		error = ENXIO;
		goto out;
	}

	if (disk->d_open_count != 0 ||
	    disk->d_inflight != 0 ||
	    disk->d_opening != 0 ||
	    disk->d_closing != 0 ||
	    disk_leaf(disk)->d_reload_owner != NULL ||
	    disk_admin_conflict_locked(disk, 0, disk->d_block_count, 0) != 0 ||
	    refcount_load(&disk->d_refs) != 1) {
		disk_unlock(enabled);
		error = EBUSY;
		goto out;
	}

	for (link = &disk_head; *link != NULL; link = &(*link)->d_next) {
		if (*link == disk) {
			*link = disk->d_next;
			live_count--;
			break;
		}
	}

	disk->d_next = NULL;
	disk_persistence_invalidate(disk);
	disk->d_state = DISK_GONE;
	disk_unlock(enabled);
	error = 0;

out:
	backing_mutation_end(&guard);

	/* Reports why the removal failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Frees the registry slot of an unpublished or gone disk.
 */
DISK_HIGH int
disk_destroy(
	struct disk *disk)
{
	int i;
	struct disk *parent;
	bool enabled;

	enabled = disk_lock();

	/* Rejects an unknown slot, a live disk, or one still in use. */
	i = disk_index(disk);
	if (i < 0 || !disk_used[i]) {
		disk_unlock(enabled);
		return EINVAL;
	}

	if (disk->d_state == DISK_LIVE) {
		disk_unlock(enabled);
		return EBUSY;
	}

	if (disk->d_state != DISK_ALLOCATED && disk->d_state != DISK_GONE) {
		disk_unlock(enabled);
		return EINVAL;
	}

	if (disk->d_open_count != 0 ||
	    disk->d_inflight != 0 ||
	    disk->d_opening != 0 ||
	    disk->d_closing != 0 ||
	    refcount_load(&disk->d_refs) != 1) {
		disk_unlock(enabled);
		return EBUSY;
	}

	/* Drops the parent reference and clears the slot. */
	parent = disk->d_parent;
	if (disk->d_state == DISK_GONE && parent != NULL)
		(void)refcount_put_not_last(&parent->d_refs);
	if (disk->d_state == DISK_GONE && disk->d_media_backing != NULL)
		(void)refcount_put_not_last(&disk->d_media_backing->d_refs);
	zero_bytes(disk, sizeof(*disk));
	disk_used[i] = 0;
	disk_unlock(enabled);

	/* Reports the freed slot. */
	return 0;
}

/*
 * Looks up a live disk by name and takes a reference.
 */
struct disk *
disk_find(
	const char *name)
{
	struct disk *disk;
	bool enabled;

	/* Rejects a missing name. */
	if (name == NULL)
		return NULL;

	/* Searches the live list and references a match. */
	enabled = disk_lock();
	for (disk = disk_head; disk != NULL; disk = disk->d_next) {
		if (name_equal(disk->d_name, name))
			break;
	}

	if (disk != NULL) {
		KERN_TEST_CHECKPOINT(KERN_TEST_DISK_LOOKUP_BEFORE_REF, disk);
		refcount_get(&disk->d_refs);
	}

	disk_unlock(enabled);

	/* Reports the referenced disk, or NULL. */
	return disk;
}

/*
 * Looks up a live disk by device number and takes a reference.
 */
struct disk *
disk_find_by_dev(
	dev_t dev)
{
	struct disk *disk;
	bool enabled;

	enabled = disk_lock();

	/* Searches the live list and references a match. */
	for (disk = disk_head; disk != NULL; disk = disk->d_next) {
		if (disk->d_dev == dev)
			break;
	}

	if (disk != NULL)
		refcount_get(&disk->d_refs);

	disk_unlock(enabled);

	/* Reports the referenced disk, or NULL. */
	return disk;
}

/*
 * Counts the live disks.
 */
unsigned
disk_count(
	void)
{
	unsigned count;
	bool enabled;

	enabled = disk_lock();
	count = live_count;
	disk_unlock(enabled);

	/* Reports the sampled count. */
	return count;
}

/*
 * Counts the bios in flight across every live disk.
 */
unsigned
disk_inflight_count(
	void)
{
	struct disk *disk;
	unsigned count;
	bool enabled;
	unsigned long irq;

	count = 0;
	enabled = disk_lock();

	/* Sums each disk's in-flight count under its own lock. */
	for (disk = disk_head; disk != NULL; disk = disk->d_next) {
		irq = spin_lock_irqsave(&disk->d_lock);
		count += disk->d_inflight;
		spin_unlock_irqrestore(&disk->d_lock, irq);
	}

	disk_unlock(enabled);

	/* Reports the sampled total. */
	return count;
}

/*
 * Takes a reference to the live disk at an index in registration order.
 */
struct disk *
disk_at(
	unsigned index)
{
	struct disk *disk;
	bool enabled;

	enabled = disk_lock();

	/* Walks the live list to the requested position. */
	disk = disk_head;
	while (disk != NULL && index != 0) {
		disk = disk->d_next;
		index--;
	}

	if (disk != NULL)
		refcount_get(&disk->d_refs);

	disk_unlock(enabled);

	/* Reports the referenced disk, or NULL past the end. */
	return disk;
}

/*
 * Takes a reference to a registered disk.
 */
void
disk_ref(
	struct disk *disk)
{
	bool enabled;

	/* Ignores a missing or unknown disk. */
	enabled = disk_lock();
	if (disk != NULL && disk_index(disk) >= 0)
		refcount_get(&disk->d_refs);
	disk_unlock(enabled);
}

/*
 * Drops a reference to a registered disk.
 */
void
disk_release(
	struct disk *disk)
{
	bool enabled;

	/* Ignores a missing or unknown disk; the slot outlives the last reference. */
	enabled = disk_lock();
	if (disk != NULL && disk_index(disk) >= 0)
		(void)refcount_put_not_last(&disk->d_refs);
	disk_unlock(enabled);
}

/*
 * Acquires a separately counted resident-buffer pin before cache publication.
 */
int
disk_buffer_acquire(
	struct disk *disk)
{
	bool enabled;
	int error;

	/* Takes a cache reference on a live device with present media. */
	enabled = disk_lock();
	error = 0;
	if (disk == NULL || disk_index(disk) < 0 || disk->d_state != DISK_LIVE ||
	    disk_media_status(disk) != 0)
		error = ENXIO;
	else if (disk->d_buffer_refs == UINT_MAX)
		error = EOVERFLOW;
	else {
		disk->d_buffer_refs++;
		refcount_get(&disk->d_refs);
	}

	disk_unlock(enabled);

	/* Reports whether the reference was granted. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Releases one resident-buffer pin, including after medium revocation.
 */
void
disk_buffer_release(
	struct disk *disk)
{
	bool enabled;

	/* Gives one cache reference back. */
	enabled = disk_lock();

	if (disk == NULL || disk_index(disk) < 0 || disk->d_buffer_refs == 0)
		HAL_FATAL("disk buffer reference underflow");

	disk->d_buffer_refs--;

	(void)refcount_put_not_last(&disk->d_refs);

	disk_unlock(enabled);
}

/*
 * Empties the registry and the buffer cache.
 */
void
disk_registry_reset(
	void)
{
	unsigned i;
	bool enabled;

	/* Drops every buffer before the disks they reference vanish. */
	buf_reset();

	/*
	 * Clears every slot and restarts device numbering.
	 */

	enabled = disk_lock();

	zero_bytes(disks, sizeof(disks));
	for (i = 0; i < DISK_MAX; i++)
		disk_used[i] = 0;
	disk_head = NULL;
	live_count = 0;
	admin_count = 0;
	next_dev = 1;

	disk_unlock(enabled);
}

/*
 * Copies the versioned geometry of a referenced live block device.
 */
DISK_HIGH int
disk_block_info(
	struct disk *disk,
	struct kern_block_info *info)
{
	bool enabled;
	unsigned i;

	/* Validates the caller's ABI and reserved fields before touching the disk. */
	if (info == NULL ||
	    info->version != KERN_BLOCK_VERSION ||
	    info->struct_size != sizeof(*info))
		return EINVAL;

	for (i = 0; i < 4; i++) {
		if (info->reserved[i] != 0)
			return EINVAL;
	}

	enabled = disk_lock();

	/* Rejects an absent disk while its publication state is locked. */
	if (disk == NULL ||
	    disk_index(disk) < 0 ||
	    disk->d_state != DISK_LIVE || disk_media_status(disk) != 0) {
		disk_unlock(enabled);
		return ENXIO;
	}

	/* Publishes one coherent device, parent and sector-geometry snapshot. */
	memset(info, 0, sizeof(*info));
	info->version = KERN_BLOCK_VERSION;
	info->struct_size = sizeof(*info);
	info->device = (uint32_t)disk->d_dev;
	info->parent_device = disk->d_parent != NULL ? (uint32_t)disk->d_parent->d_dev : 0;
	info->flags = disk->d_flags & (DISK_READ_ONLY | DISK_REMOVABLE | DISK_PARTITION);

	/* Exposes the backing capability without leaking internal flag values. */
	if ((disk->d_flags & DISK_FILE_BACKED) != 0)
		info->flags |= KERN_BLOCK_FILE_BACKED;

	info->sector_size = disk->d_block_size;
	info->sector_count = disk->d_block_count;
	info->parent_offset = disk->d_parent_offset;
	memcpy(info->name, disk->d_name, sizeof(info->name));

	disk_unlock(enabled);

	/* Reports the completed snapshot. */
	return 0;
}

/*
 * Copies the description of a live disk found by name.
 */
DISK_HIGH int
disk_get_info(
	const char *name,
	struct disk_info *result)
{
	struct disk *disk;
	bool enabled;

	/* Rejects a missing name or result. */
	if (name == NULL || result == NULL)
		return EINVAL;

	enabled = disk_lock();

	/* Copies the description of the first name match. */
	for (disk = disk_head; disk != NULL; disk = disk->d_next) {
		if (name_equal(name, disk->d_name)) {
			disk_copy_info(disk, result);
			disk_unlock(enabled);
			return 0;
		}
	}

	disk_unlock(enabled);

	/* Reports an unknown name. */
	return ENOENT;
}

/*
 * Copies the descriptions of every live disk.
 *
 * The count is always reported; the copy happens only when the array
 * can hold every entry.
 */
DISK_HIGH int
disk_registry_snapshot(
	struct disk_info *entries,
	unsigned capacity,
	unsigned *count_out)
{
	struct disk *disk;
	unsigned count;
	bool enabled;

	count = 0;

	/* Rejects a missing count or a capacity without an array. */
	if (count_out == NULL || (capacity != 0 && entries == NULL))
		return EINVAL;

	enabled = disk_lock();

	/* Counts the live disks, then copies them when they fit. */
	for (disk = disk_head; disk != NULL; disk = disk->d_next)
		count++;
	*count_out = count;
	if (count > capacity) {
		disk_unlock(enabled);
		return ENOSPC;
	}
	count = 0;
	for (disk = disk_head; disk != NULL; disk = disk->d_next)
		disk_copy_info(disk, &entries[count++]);

	disk_unlock(enabled);

	/* Reports the copied snapshot. */
	return 0;
}

/*
 * Opens a live disk, calling the driver's open on every open.
 *
 * Each successful open holds a reference until the matching close.
 */
int
disk_open(
	struct disk *disk)
{
	int error;
	bool enabled;

	error = 0;

	enabled = disk_lock();

	/* Rejects a missing, unknown, or dead disk. */
	if (disk == NULL ||
	    disk_index(disk) < 0 ||
	    disk->d_state != DISK_LIVE || disk_media_status(disk) != 0) {
		disk_unlock(enabled);
		return ENXIO;
	}

	/* Holds a temporary lifetime reference across the driver call. */
	if (disk_leaf(disk)->d_reload_owner != NULL ||
	    disk_admin_conflict_locked(disk, 0, disk->d_block_count, 0) != 0) {
		disk_unlock(enabled);
		return EBUSY;
	}

	disk->d_opening++;
	refcount_get(&disk->d_refs);

	disk_unlock(enabled);

	if (disk->d_ops != NULL && disk->d_ops->open != NULL)
		error = disk->d_ops->open(disk);

	enabled = disk_lock();

	/* Counts the open only while the disk is still live. */
	disk->d_opening--;
	if (error == 0) {
		if (disk->d_state != DISK_LIVE || disk_media_status(disk) != 0) {
			error = ENXIO;
		} else {
			disk->d_open_count++;
			refcount_get(&disk->d_refs);
		}
	}
	(void)refcount_put_not_last(&disk->d_refs);

	disk_unlock(enabled);

	/* Undoes the driver open of a disk that died meanwhile. */
	if (error == ENXIO && disk->d_ops != NULL && disk->d_ops->close != NULL)
		disk->d_ops->close(disk);

	/* Reports why the open failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Opens a live disk found by device number.
 */
DISK_HIGH int
disk_open_by_dev(
	dev_t dev,
	struct disk **result)
{
	struct disk *disk;
	int error;
	bool enabled;

	/* Rejects a missing result. */
	if (result == NULL)
		return EINVAL;
	*result = NULL;

	/* Finds and references the disk. */
	enabled = disk_lock();
	for (disk = disk_head; disk != NULL; disk = disk->d_next) {
		if (disk->d_dev == dev)
			break;
	}

	if (disk == NULL) {
		disk_unlock(enabled);
		return ENXIO;
	}

	refcount_get(&disk->d_refs);
	disk_unlock(enabled);

	/* Opens it and drops the lookup reference. */
	error = disk_open(disk);
	disk_release(disk);
	if (error == 0)
		*result = disk;

	/* Reports why the open failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Closes an open disk, calling the driver's close on every close.
 */
void
disk_close(
	struct disk *disk)
{
	bool enabled;

	/* Ignores a missing, unknown, or unopened disk. */
	enabled = disk_lock();
	if (disk == NULL || disk_index(disk) < 0 || disk->d_open_count == 0) {
		disk_unlock(enabled);
		return;
	}

	disk->d_open_count--;
	disk->d_closing++;
	disk_unlock(enabled);

	/* Tells the driver, then drops the open's reference. */
	if (disk->d_ops != NULL && disk->d_ops->close != NULL)
		disk->d_ops->close(disk);

	enabled = disk_lock();
	disk->d_closing--;
	disk_unlock(enabled);

	disk_release(disk);
}

/*
 * Forwards a control request to a live disk's driver.
 */
int
disk_ioctl(
	struct disk *disk,
	unsigned long request,
	void *argument)
{
	int error;
	bool enabled;

	enabled = disk_lock();

	/* Rejects a missing, unknown, or dead disk. */
	if (disk == NULL ||
	    disk_index(disk) < 0 ||
	    disk->d_state != DISK_LIVE || disk_media_status(disk) != 0) {
		disk_unlock(enabled);
		return ENXIO;
	}
	refcount_get(&disk->d_refs);

	disk_unlock(enabled);

	/* Forwards the request when the driver handles controls. */
	if (disk->d_ops == NULL || disk->d_ops->ioctl == NULL) {
		disk_release(disk);
		return EOPNOTSUPP;
	}

	error = disk->d_ops->ioctl(disk, request, argument);

	disk_release(disk);

	/* Reports why the driver's failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Invalidates proof without changing outstanding request ownership.
 */
void
disk_persistence_invalidate(
	struct disk *disk)
{
	struct disk *leaf;
	unsigned long irq;

	/* Invalidates the live object's physical ancestry under its state lock. */
	if (disk == NULL)
		return;

	leaf = disk_leaf(disk);

	irq = spin_lock_irqsave(&leaf->d_lock);

	disk_persistence_invalidate_locked(leaf);
	if (leaf->d_media_epoch != UINT64_MAX)
		leaf->d_media_epoch++;

	spin_unlock_irqrestore(&leaf->d_lock, irq);
}

/*
 * Retires durability proof while the same-medium command owner recovers.
 */
void
disk_persistence_forget(
	struct disk *disk)
{
	struct disk *leaf;
	unsigned long irq;

	/* Forgets the persistence record of the physical device. */
	if (disk == NULL)
		return;

	leaf = disk_leaf(disk);

	irq = spin_lock_irqsave(&leaf->d_lock);

	disk_persistence_invalidate_locked(leaf);

	spin_unlock_irqrestore(&leaf->d_lock, irq);
}

/*
 * Closes the original medium without touching its outstanding owners.
 */
void
disk_media_revoke(
	struct disk *disk)
{
	struct disk *leaf;
	bool enabled;

	/* Marks the media of the physical device gone, once. */
	if (disk == NULL)
		return;

	leaf = disk_leaf(disk);

	enabled = disk_lock();
	if (!atomic_raw_load_acquire(&leaf->d_media_revoked)) {
		atomic_raw_store_release(&leaf->d_media_revoked, 1U);
		disk_persistence_invalidate(leaf);
	}
	disk_unlock(enabled);
}

/*
 * Retires unused revoked ancestry without writing its old buffers to media.
 */
int
disk_media_retire(
	struct disk *disk)
{
	struct backing_mutation_guard guard;
	struct disk **link, *child;
	bool enabled;
	int error, index;

	/* Excludes every claim on the device before retiring it. */
	error = backing_mutation_begin_retired_disk(disk, &guard);
	if (error != 0)
		return error;

	enabled = disk_lock();

	/* Requires the device to be completely idle. */
	error = disk_media_idle_locked(disk, 0);
	if (error != 0) {
		disk_unlock(enabled);
		backing_mutation_end(&guard);
		return error;
	}

	/* The extra pin also excludes a concurrent retirement attempt. */
	refcount_get(&disk->d_refs);

	disk_unlock(enabled);

	error = buf_discard_media(disk);
	enabled = disk_lock();

	if (error == 0)
		error = disk_media_idle_locked(disk, 1);

	if (error == 0 && disk->d_buffer_refs != 0)
		error = EBUSY;

	if (error == 0) {
		/* All fallible checks precede removal of any published child. */
		for (link = &disk_head; *link != NULL;) {
			child = *link;
			if (child != disk && child->d_parent != disk) {
				link = &child->d_next;
				continue;
			}

			*link = child->d_next;
			live_count--;
			if (child == disk) {
				disk->d_next = NULL;
				disk->d_state = DISK_GONE;
			} else {
				index = disk_index(child);
				(void)refcount_put_not_last(&disk->d_refs);
				zero_bytes(child, sizeof(*child));
				disk_used[index] = 0;
			}
		}
	}

	(void)refcount_put_not_last(&disk->d_refs);

	disk_unlock(enabled);
	backing_mutation_end(&guard);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Tests media admission independently of reference ownership and transport.
 */
int
disk_media_status(
	const struct disk *disk)
{
	if (disk == NULL)
		return ENXIO;
	while (disk != NULL) {
		if (atomic_raw_load_acquire(&disk->d_media_revoked))
			return ENXIO;
		if (disk->d_parent != NULL)
			disk = disk->d_parent;
		else
			disk = disk->d_media_backing;
	}

	return 0;
}

/* Retains legacy refusal semantics: a refused synchronous BIO has no callback. */
int
bio_submit(
	struct disk *disk,
	struct bio *bio)
{
	struct disk *leaf;
	bool enabled;
	int error;

	/* Admits the request, which pins the device it names. */
	error = bio_admit(disk, bio);
	if (error != 0)
		return error;

	/* Hands it to the driver, undoing the admission on a refusal. */
	leaf = bio->b_leaf_disk;
	error = bio_dispatch(bio);
	if (error != 0) {
		disk_write_retire(leaf, bio, error, 0);
		enabled = disk_lock();
		leaf->d_inflight--;
		(void)refcount_put_not_last(&leaf->d_refs);
		bio->b_state = BIO_NEW;
		bio->b_leaf_disk = NULL;
		disk_unlock(enabled);
	}

	/* Reports why the submission failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Completes a submitted bio on behalf of its driver.
 *
 * Waiters are woken, the leaf is unpinned, and the completion callback
 * runs last without any lock held.
 */
void
bio_complete(
	struct bio *bio,
	int error,
	size_t transferred)
{
	struct disk *leaf;
	unsigned long bio_irq;
	bool enabled;
	void (*done)(struct bio *);

	/* Ignores a missing or never-submitted bio. */
	if (bio == NULL || !bio->b_initialized)
		return;

	/* Records the result once and wakes the waiters. */
	bio_irq = spin_lock_irqsave(&bio->b_lock);

	if (bio->b_state != BIO_SUBMITTED) {
		spin_unlock_irqrestore(&bio->b_lock, bio_irq);
		return;
	}

	leaf = bio->b_leaf_disk;
	if (leaf != NULL && disk_media_status(leaf) != 0) {
		error = ESTALE;
		transferred = 0;
	}

	if (error == 0 && leaf != NULL && bio->b_op == BIO_WRITE &&
	    transferred != (uint64_t)bio->b_block_count * leaf->d_block_size)
		error = EIO;

	io_stats_record(bio->b_op == BIO_FLUSH ?
			IO_COMPLETE_FLUSH :
			(bio->b_op == BIO_READ ?
			 IO_COMPLETE_READ :
			 IO_COMPLETE_WRITE),
			transferred);

	if (error != 0)
		io_stats_record(IO_COMPLETE_ERROR, transferred);

	if (error != 0 &&
	    leaf != NULL &&
	    bio->b_op != BIO_READ &&
	    io_error_record != NULL)
		io_error_record(&leaf->d_write_error, error);

	bio->b_error = error;
	bio->b_transferred = transferred;

	/* Captures all caller-owned fields before making storage reusable. */
	done = bio->b_done;
	disk_write_retire(leaf, bio, error, transferred);

	/* Unpins the leaf. */
	enabled = disk_lock();
	if (leaf != NULL && leaf->d_inflight != 0)
		leaf->d_inflight--;
	if (leaf != NULL)
		(void)refcount_put_not_last(&leaf->d_refs);
	disk_unlock(enabled);

	/* Publishes completion only after all leaf accounting is finished. */
	bio->b_state = BIO_COMPLETED;
	waitq_wake_all(&bio->b_waitq);

	spin_unlock_irqrestore(&bio->b_lock, bio_irq);

	/* Runs the completion callback unlocked. */
	if (done != NULL)
		done(bio);
}

/*
 * Maps a block range down the partition stack to its leaf disk.
 */
int
disk_resolve_range(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	struct disk **leaf_out,
	uint64_t *mapped_out)
{
	struct disk *leaf;
	uint64_t mapped;
	struct disk *parent;

	/* Rejects a missing operand, a dead disk, or a range outside it. */
	if (disk == NULL ||
	    leaf_out == NULL ||
	    mapped_out == NULL ||
	    count == 0)
		return EINVAL;
	if (disk->d_state != DISK_LIVE || disk_media_status(disk) != 0)
		return ENXIO;
	if (block >= disk->d_block_count || count > disk->d_block_count - block)
		return EOVERFLOW;

	/* Adds each level's offset, requiring a live parent of equal block size. */
	leaf = disk;
	mapped = block;
	while (leaf->d_parent != NULL) {
		parent = leaf->d_parent;
		if (parent->d_state != DISK_LIVE)
			return ENXIO;
		if (parent->d_block_size != leaf->d_block_size)
			return EOPNOTSUPP;
		if (mapped > UINT64_MAX - leaf->d_parent_offset)
			return EOVERFLOW;
		mapped += leaf->d_parent_offset;
		leaf = parent;
	}

	if (mapped >= leaf->d_block_count ||
	    count > leaf->d_block_count - mapped)
		return EOVERFLOW;

	*leaf_out = leaf;
	*mapped_out = mapped;

	/* Reports the resolved leaf range. */
	return 0;
}

/*
 * Waits for a submitted bio to complete.
 *
 * Without a current thread, as during early boot, the wait spins.
 */
int
bio_wait(
	struct bio *bio)
{
	struct thread *thread;
	unsigned long irq;
	int error;
	uint64_t sequence;

	/* Rejects a missing or never-submitted bio. */
	if (bio == NULL || !bio->b_initialized || bio->b_done != NULL)
		return EINVAL;

	/* Finds the current thread when the scheduler is linked in. */
	if (thread_current != NULL)
		thread = thread_current();
	else
		thread = NULL;

	/* A thread sleeps on the completion queue. */
	if (thread != NULL) {
		irq = spin_lock_irqsave(&bio->b_lock);
		while (bio->b_state == BIO_SUBMITTED) {
			sequence = waitq_sequence(&bio->b_waitq);
			error = waitq_sleep(&bio->b_waitq,
					    &bio->b_lock,
					    sequence,
					    0,
					    0);
			if (error != 0 && error != EAGAIN) {
				spin_unlock_irqrestore(&bio->b_lock, irq);
				return error;
			}
		}

		if (bio->b_state == BIO_COMPLETED)
			error = bio->b_error;
		else
			error = EINVAL;

		spin_unlock_irqrestore(&bio->b_lock, irq);
		return error;
	}

	/* Without a thread the wait polls the state. */
	for (;;) {
		irq = spin_lock_irqsave(&bio->b_lock);
		if (bio->b_state != BIO_SUBMITTED)
			break;
		spin_unlock_irqrestore(&bio->b_lock, irq);
		hal_compiler_barrier();
	}

	if (bio->b_state == BIO_COMPLETED)
		error = bio->b_error;
	else
		error = EINVAL;

	spin_unlock_irqrestore(&bio->b_lock, irq);

	/* Reports why the completion failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Flushes a disk's write cache and waits for the flush.
 */
int
bio_flush(
	struct disk *disk)
{
	struct disk *leaf;
	struct bio bio;
	struct thread *thread;
	uint64_t target, epoch, sequence;
	unsigned long irq;
	int error, reusable;

	/* Retains reload admission even when a proof avoids physical I/O. */
	error = disk_cache_enter(disk, &leaf);
	if (error != 0)
		return error;

	thread = thread_current != NULL ? thread_current() : NULL;

	irq = spin_lock_irqsave(&leaf->d_lock);

	target = leaf->d_write_accepted;

	/* Waits for earlier flush ownership and holes in this captured prefix. */
	while (leaf->d_flush_busy || leaf->d_write_completed < target) {
		sequence = waitq_sequence(&leaf->d_waitq);
		if (thread == NULL) {
			spin_unlock_irqrestore(&leaf->d_lock, irq);
			hal_compiler_barrier();
			irq = spin_lock_irqsave(&leaf->d_lock);
		} else {
			error = waitq_sleep(&leaf->d_waitq, &leaf->d_lock, sequence, 0, 0);
			if (error != 0 && error != EAGAIN) {
				spin_unlock_irqrestore(&leaf->d_lock, irq);
				disk_cache_leave(leaf);
				return error;
			}
		}
	}

	/* Uses only an explicit driver guarantee in the same unexpired epoch. */
	epoch = leaf->d_persist_epoch;
	reusable = (leaf->d_flags & DISK_FLUSH_PROOF) != 0 &&
		target != UINT64_MAX && epoch != UINT64_MAX &&
		leaf->d_stable_valid && leaf->d_stable_epoch == epoch &&
		leaf->d_write_stable >= target;
	if (reusable) {
		spin_unlock_irqrestore(&leaf->d_lock, irq);
		disk_cache_leave(leaf);
		return 0;
	}

	leaf->d_flush_busy = 1;

	spin_unlock_irqrestore(&leaf->d_lock, irq);

	/* Issues a real barrier after the complete captured prefix. */
	memset(&bio, 0, sizeof(bio));
	bio.b_op = BIO_FLUSH;
	error = bio_submit(disk, &bio);
	if (error == 0)
		error = bio_wait(&bio);

	/* Publishes this target only; later accepted writes need another barrier. */
	irq = spin_lock_irqsave(&leaf->d_lock);

	if (error == 0 && (leaf->d_flags & DISK_FLUSH_PROOF) != 0 &&
	    epoch == leaf->d_persist_epoch &&
	    epoch != UINT64_MAX && target != UINT64_MAX) {
		leaf->d_write_stable = target;
		leaf->d_stable_epoch = epoch;
		leaf->d_stable_valid = 1;
	} else {
		disk_persistence_invalidate_locked(leaf);
	}

	leaf->d_flush_busy = 0;
	waitq_wake_all(&leaf->d_waitq);

	spin_unlock_irqrestore(&leaf->d_lock, irq);

	disk_cache_leave(leaf);

	/* Preserves the physical barrier result for upper dirty owners. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads blocks from a disk, bypassing the buffer cache.
 */
int
disk_read_direct(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *data)
{
	int error;

	/* Reports why the transfer failed. */
	error = disk_transfer_direct(disk, BIO_READ, block, count, data, NULL, NULL, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes blocks to a disk, bypassing the buffer cache.
 */
int
disk_write_direct(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data)
{
	int error;

	/* Writes without an I/O context. */
	error = disk_write_direct_context(disk, block, count, data, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes blocks from a disk, bypassing the buffer cache, under one I/O
 * context.
 *
 * The context describes the caller's ownership chain; a NULL context means
 * an unattributed write.
 */
int
disk_write_direct_context(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data,
	const struct io_context *context)
{
	int error;

	/* Reports why the transfer failed. */
	error = disk_transfer_direct(disk, BIO_WRITE, block, count, (void *)data,
				     NULL, NULL, context);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports only the prefix covered by completely successful direct BIOs.
 * A failing or short BIO contributes nothing, even if it reports some bytes.
 */
int
disk_transfer_progress(
	struct disk *disk,
	enum bio_op op,
	uint64_t block,
	uint32_t count,
	void *data,
	uint32_t *completed)
{
	int error;

	/* Transfers without an I/O context. */
	error = disk_transfer_progress_context(
		disk,
		op,
		block,
		count,
		data,
		completed,
		NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports only the prefix covered by completely successful direct BIOs, under
 * one I/O context.
 *
 * A failing or short BIO contributes nothing, even if it reports some bytes.
 */
int
disk_transfer_progress_context(
	struct disk *disk,
	enum bio_op op,
	uint64_t block,
	uint32_t count,
	void *data,
	uint32_t *completed,
	const struct io_context *context)
{
	int error;

	/* Requires an output and a data operation. */
	if (completed == NULL)
		return EINVAL;
	*completed = 0;
	if (op != BIO_READ && op != BIO_WRITE)
		return EINVAL;

	/* Reports the transfer outcome independently of its confirmed prefix. */
	error = disk_transfer_direct(disk, op, block, count, data, NULL, completed, context);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes blocks directly under a backing claim that authorizes them.
 */
int
disk_write_direct_claimed(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data,
	const struct backing_claim *claim)
{
	int error;

	/* Rejects a missing claim. */
	if (claim == NULL)
		return EINVAL;

	/* Reports why the transfer failed. */
	error = disk_transfer_direct(disk, BIO_WRITE, block, count, (void *)data,
				     claim, NULL, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes back every dirty buffer of a disk and flushes its cache.
 */
int
disk_sync(
	struct disk *disk)
{
	int error;

	/* Writes the buffers back before flushing the device. */
	error = buf_sync(disk);
	if (error != 0)
		return error;

	/* Reports why the flush failed. */
	error = bio_flush(disk);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads blocks through the buffer cache.
 */
int
disk_read(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *data)
{
	int error;

	/* Reports why the read failed. */
	error = disk_cached_transfer(disk, block, count, data, 0, NULL, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Copies metadata while retaining only bounded common-cache reference tokens.
 */
int
disk_read_view(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *data,
	struct buf_view *view)
{
	int error;

	/* Applies the same lifecycle admission as ordinary cached I/O. */

	/* Reports the failure. */
	error = disk_cached_transfer(disk, block, count, data, 0, view, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Validates metadata copies only while their disk remains admitted and unchanged.
 */
int
disk_view_matches(
	struct disk *disk,
	const struct buf_view *view)
{
	struct disk *leaf;
	int matches;
	int error;

	/* Rejects a different mount/device lifetime or an inadmissible disk. */
	if (view == NULL || view->disk != disk)
		return 0;

	error = disk_cache_enter(disk, &leaf);
	if (error != 0)
		return 0;

	matches = buf_view_matches(view);
	disk_cache_leave(leaf);

	/* Reports a valid copy at this observation point. */
	return matches;
}

/*
 * Writes blocks through the buffer cache under a backing mutation guard.
 */
int
disk_write(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data)
{
	int error;

	/* Writes without an I/O context. */
	error = disk_write_context(disk, block, count, data, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes blocks through the buffer cache under a backing mutation guard and
 * one I/O context.
 *
 * The context is validated before any range is claimed, so a malformed
 * context never reaches the cache.
 */
int
disk_write_context(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data,
	const struct io_context *context)
{
	struct backing_mutation_guard guard;
	int error;

	error = io_context_validate(context);
	if (error != 0)
		return error;

	/* Excludes conflicting claims on the range for the write. */
	error = backing_mutation_begin_disk(disk, block, count, NULL, &guard);
	if (error != 0)
		return error;
	error = disk_cached_transfer(disk, block, count, (void *)data, 1, NULL, context);
	backing_mutation_end(&guard);

	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes blocks through the buffer cache on behalf of a mounted filesystem.
 */
int
disk_write_filesystem(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data)
{
	int error;

	/* Writes without an I/O context. */
	error = disk_write_filesystem_context(disk, block, count, data, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes blocks through the buffer cache on behalf of a mounted filesystem,
 * under one I/O context.
 *
 * The context is validated before any range is claimed, so a malformed
 * context never reaches the cache.
 */
int
disk_write_filesystem_context(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data,
	const struct io_context *context)
{
	struct backing_mutation_guard guard;
	int error;

	error = io_context_validate(context);
	if (error != 0)
		return error;

	/* Excludes conflicting claims under the filesystem's own guard. */
	error = backing_mutation_begin_disk_filesystem(disk, block, count,
						       &guard);
	if (error != 0)
		return error;
	error = disk_cached_transfer(disk, block, count, (void *)data, 1, NULL, context);
	backing_mutation_end(&guard);

	/* Reports why the write failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Admits file-cache access under the block device lifecycle barrier.
 */
int
disk_cache_acquire(
	struct disk *disk,
	struct disk **leaf)
{
	int error;

	/* Requires an output token before taking the device reference. */
	if (leaf == NULL)
		return EINVAL;
	*leaf = NULL;

	/* Reports the acquired token or the lifecycle refusal. */
	error = disk_cache_enter(disk, leaf);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Releases a previously admitted file-cache access.
 */
void
disk_cache_release(
	struct disk *leaf)
{
	/* Ignores callers whose filesystem has no block backing. */
	if (leaf == NULL)
		return;
	disk_cache_leave(leaf);
}

int
bio_async_enable(struct disk *disk)
{
	struct bio_async_endpoint *endpoint;
	struct bio_async_endpoint *candidate;
	struct disk *token;
	struct thread *thread;
	struct kern_pmem memory;
	size_t allocation_size;
	unsigned long registry_irq;
	unsigned long irq;
	unsigned index;
	size_t controls;
	int error;

	error = async_initialize();
	if (error != 0)
		return error;
	error = disk_cache_acquire(disk, &token);
	if (error != 0)
		return error;
	endpoint = NULL;
	registry_irq = spin_lock_irqsave(&async_registry);

	for (index = 0; index < ASYNC_ENDPOINTS; index++) {
		candidate = &async_endpoints[index];
		if (candidate->leaf == token && candidate->state != ASYNC_OFF) {
			error = candidate->state == ASYNC_LIVE ? 0 : EBUSY;
			spin_unlock_irqrestore(&async_registry, registry_irq);
			disk_cache_release(token);
			return error;
		}

		if (candidate->state == ASYNC_OFF && endpoint == NULL)
			endpoint = candidate;
	}

	if (endpoint == NULL) {
		spin_unlock_irqrestore(&async_registry, registry_irq);
		disk_cache_release(token);
		return EAGAIN;
	}

	irq = spin_lock_irqsave(&endpoint->lock);

	endpoint->leaf = token;
	endpoint->state = ASYNC_BUILDING;

	spin_unlock_irqrestore(&endpoint->lock, irq);
	spin_unlock_irqrestore(&async_registry, registry_irq);

	controls = (ASYNC_SLOTS * sizeof(struct bio_async_request) + KERN_PAGE_SIZE - 1U) &
	    ~(size_t)(KERN_PAGE_SIZE - 1U);

	allocation_size = controls + ASYNC_SLOTS * KERN_IO_BATCH_MAX;
	memset(&memory, 0, sizeof(memory));
	memory.size = allocation_size;
	error = hal_pmem_alloc(allocation_size, KERN_PAGE_SIZE,
			       &memory.paddr);
	if (error != HAL_OK || hal_pmem_to_kernel(memory.paddr) == NULL) {
		error = ENOMEM;
		goto failed_memory;
	}

	error = cache_memory_reserve(CACHE_MEMORY_IO_POOL, memory.size, 0);
	if (error != 0)
		goto failed_memory;
	cache_memory_commit(CACHE_MEMORY_IO_POOL, memory.size);
	memset(hal_pmem_to_kernel(memory.paddr), 0, memory.size);
	if (!endpoint->started) {
		error = kthread_create(async_worker, endpoint, SCHED_PRIORITY_DEFAULT, &thread);
		if (error != 0) {
			cache_memory_release(CACHE_MEMORY_IO_POOL, memory.size);
			goto failed_memory;
		}

		endpoint->started = 1;
		thread_start(thread);
	}

	registry_irq = spin_lock_irqsave(&async_registry);
	irq = spin_lock_irqsave(&endpoint->lock);

	endpoint->memory = memory;
	endpoint->cache_token = token;
	endpoint->requests = hal_pmem_to_kernel(memory.paddr);
	for (index = 0; index < ASYNC_SLOTS; index++) {
		endpoint->requests[index].endpoint = endpoint;
		endpoint->requests[index].payload = (char *)hal_pmem_to_kernel(memory.paddr) + controls + index * KERN_IO_BATCH_MAX;
	}

	endpoint->state = ASYNC_LIVE;

	spin_unlock_irqrestore(&endpoint->lock, irq);
	spin_unlock_irqrestore(&async_registry, registry_irq);

	return 0;

failed_memory:
	if (memory.size != 0 && hal_pmem_free(&memory.paddr, memory.size) != HAL_OK)
		HAL_FATAL("BIO endpoint rollback failed");
	registry_irq = spin_lock_irqsave(&async_registry);
	irq = spin_lock_irqsave(&endpoint->lock);

	endpoint->leaf = NULL;
	endpoint->state = ASYNC_OFF;

	spin_unlock_irqrestore(&endpoint->lock, irq);
	spin_unlock_irqrestore(&async_registry, registry_irq);

	disk_cache_release(token);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

int
bio_async_disable(struct disk *disk)
{
	struct bio_async_endpoint *endpoint;
	struct disk *leaf;
	struct disk *token;
	struct kern_pmem memory;
	unsigned long registry_irq;
	unsigned long irq;
	unsigned index;
	int error;

	if (disk == NULL || atomic_load_acquire(&async_initialized) != 2)
		return EINVAL;
	leaf = disk_leaf(disk);
	registry_irq = spin_lock_irqsave(&async_registry);

	for (index = 0; index < ASYNC_ENDPOINTS; index++) {
		endpoint = &async_endpoints[index];
		if (endpoint->leaf != leaf)
			continue;
		irq = spin_lock_irqsave(&endpoint->lock);
		if (endpoint->state != ASYNC_LIVE || endpoint->used != 0) {
			spin_unlock_irqrestore(&endpoint->lock, irq);
			spin_unlock_irqrestore(&async_registry, registry_irq);
			return EBUSY;
		}

		endpoint->state = ASYNC_STOPPING;
		memory = endpoint->memory;
		token = endpoint->cache_token;
		spin_unlock_irqrestore(&endpoint->lock, irq);
		spin_unlock_irqrestore(&async_registry, registry_irq);
		error = hal_pmem_free(&memory.paddr, memory.size);
		registry_irq = spin_lock_irqsave(&async_registry);
		irq = spin_lock_irqsave(&endpoint->lock);
		if (error != HAL_OK) {
			endpoint->state = ASYNC_LIVE;
			spin_unlock_irqrestore(&endpoint->lock, irq);
			spin_unlock_irqrestore(&async_registry, registry_irq);
			return EIO;
		}

		cache_memory_release(CACHE_MEMORY_IO_POOL, endpoint->memory.size);
		memset(&endpoint->memory, 0, sizeof(endpoint->memory));
		endpoint->requests = NULL;
		endpoint->cache_token = NULL;
		endpoint->leaf = NULL;
		endpoint->state = ASYNC_OFF;
		spin_unlock_irqrestore(&endpoint->lock, irq);
		spin_unlock_irqrestore(&async_registry, registry_irq);
		disk_cache_release(token);
		return 0;
	}

	spin_unlock_irqrestore(&async_registry, registry_irq);

	return ENOENT;
}

int
bio_async_prepare(struct disk *disk, enum bio_op op, uint64_t block,
    uint32_t count, const void *write_data, const struct backing_claim *claim,
    const struct io_context *context, bio_async_callback callback, void *argument,
    struct bio_async_request **result)
{
	struct bio_async_request *request;
	struct bio_async_endpoint *endpoint;
	struct io_context inherited;
	struct disk *token;
	unsigned long irq;
	unsigned index;
	unsigned slot;
	size_t bytes;
	int error;

	if (result == NULL)
		return EINVAL;
	*result = NULL;
	if (disk == NULL || disk->d_block_size == 0 ||
	    (op != BIO_READ && op != BIO_WRITE && op != BIO_FLUSH) ||
	    (op == BIO_WRITE && write_data == NULL) ||
	    (op != BIO_WRITE && write_data != NULL) ||
	    (op == BIO_FLUSH ? count != 0 : count == 0) ||
	    count > KERN_IO_BATCH_MAX / disk->d_block_size)
		return EINVAL;
	error = io_context_child(&inherited, context, IO_CONTEXT_DRAIN);
	if (error != 0)
		return error;
	if (atomic_load_acquire(&async_initialized) != 2)
		return EOPNOTSUPP;
	error = disk_cache_acquire(disk, &token);
	if (error != 0)
		return error;
	if ((disk->d_max_transfer_blocks != 0 && count > disk->d_max_transfer_blocks) ||
	    (token->d_max_transfer_blocks != 0 && count > token->d_max_transfer_blocks)) {
		disk_cache_release(token);
		return E2BIG;
	}

	request = NULL;
	for (index = 0; index < ASYNC_ENDPOINTS && request == NULL; index++) {
		endpoint = &async_endpoints[index];
		irq = spin_lock_irqsave(&endpoint->lock);
		if (endpoint->state == ASYNC_LIVE && endpoint->leaf == token) {
			for (slot = 0; slot < ASYNC_SLOTS; slot++) {
				if (endpoint->requests[slot].state == REQUEST_FREE) {
					request = &endpoint->requests[slot];
					request->state = REQUEST_PREPARING;
					endpoint->used++;
					break;
				}
			}
		}

		spin_unlock_irqrestore(&endpoint->lock, irq);
	}

	if (request == NULL) {
		disk_cache_release(token);
		return EAGAIN;
	}

	endpoint = request->endpoint;
	refcount_init(&request->refs, 1);
	memset(&request->bio, 0, sizeof(request->bio));
	memset(&request->guard, 0, sizeof(request->guard));
	request->disk = disk;
	disk_ref(disk);
	request->cache_token = token;
	request->context = inherited;
	request->authorization = claim;
	if (inherited.origin_inode != NULL)
		inode_ref(inherited.origin_inode);
	backing_claim_ref(inherited.claim);
	backing_claim_ref(claim);
	request->callback = callback;
	request->argument = argument;
	request->next = NULL;
	request->error = 0;
	request->transferred = 0;
	request->callback_active = 0;
	irq = spin_lock_irqsave(&token->d_lock);

	request->epoch = token->d_media_epoch;

	spin_unlock_irqrestore(&token->d_lock, irq);

	if (request->epoch == UINT64_MAX) {
		bio_async_release(request);
		return EOVERFLOW;
	}

	if (op == BIO_WRITE) {
		error = backing_mutation_begin_disk(disk, block, count, claim, &request->guard);
		if (error != 0) {
			bio_async_release(request);
			return error;
		}
	}

	bytes = (size_t)count * disk->d_block_size;
	if (op == BIO_WRITE)
		memcpy(request->payload, write_data, bytes);
	request->bio.b_context = inherited;
	request->bio.b_op = op;
	request->bio.b_block = block;
	request->bio.b_block_count = count;
	request->bio.b_data = op == BIO_FLUSH ? NULL : request->payload;
	request->bio.b_done = async_complete;
	irq = spin_lock_irqsave(&endpoint->lock);

	request->state = REQUEST_READY;

	spin_unlock_irqrestore(&endpoint->lock, irq);

	*result = request;
	return 0;
}

int
bio_async_submit(struct bio_async_request *request)
{
	struct bio_async_endpoint *endpoint;
	unsigned long irq;
	int error;

	if (request == NULL)
		return EINVAL;
	endpoint = request->endpoint;
	irq = spin_lock_irqsave(&endpoint->lock);

	if (request->state != REQUEST_READY || endpoint->state != ASYNC_LIVE) {
		spin_unlock_irqrestore(&endpoint->lock, irq);
		return EINVAL;
	}

	if (endpoint->queued == ASYNC_QUEUE_LIMIT) {
		spin_unlock_irqrestore(&endpoint->lock, irq);
		return EAGAIN;
	}

	error = bio_admit(request->disk, &request->bio);
	if (error != 0) {
		spin_unlock_irqrestore(&endpoint->lock, irq);
		return error;
	}

	bio_async_ref(request);
	request->state = REQUEST_QUEUED;
	if (endpoint->tail != NULL)
		endpoint->tail->next = request;
	else
		endpoint->head = request;
	endpoint->tail = request;
	endpoint->queued++;
	waitq_wake_all(&endpoint->wake);

	spin_unlock_irqrestore(&endpoint->lock, irq);

	return 0;
}

int
bio_async_cancel(struct bio_async_request *request)
{
	struct bio_async_endpoint *endpoint;
	unsigned long irq;
	int error;

	if (request == NULL)
		return EINVAL;
	endpoint = request->endpoint;
	irq = spin_lock_irqsave(&endpoint->lock);

	if (request->state != REQUEST_QUEUED) {
		error = request->state == REQUEST_DONE ? EALREADY : EBUSY;
		spin_unlock_irqrestore(&endpoint->lock, irq);
		return error;
	}

	async_unlink_locked(endpoint, request);
	request->state = REQUEST_RUNNING;

	spin_unlock_irqrestore(&endpoint->lock, irq);

	bio_complete(&request->bio, ECANCELED, 0);
	bio_async_release(request);
	return 0;
}

int
bio_async_wait(struct bio_async_request *request)
{
	struct bio_async_endpoint *endpoint;
	unsigned long irq;
	uint64_t sequence;
	int error;

	if (request == NULL)
		return EINVAL;
	endpoint = request->endpoint;
	irq = spin_lock_irqsave(&endpoint->lock);

	while (request->state != REQUEST_DONE) {
		if (request->state != REQUEST_QUEUED && request->state != REQUEST_RUNNING) {
			spin_unlock_irqrestore(&endpoint->lock, irq);
			return EINVAL;
		}

		sequence = waitq_sequence(&endpoint->wake);
		error = waitq_sleep(&endpoint->wake, &endpoint->lock, sequence, 0, 0);
		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&endpoint->lock, irq);
			return error;
		}
	}

	error = request->error;

	spin_unlock_irqrestore(&endpoint->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

int
bio_async_result(struct bio_async_request *request, const void **data, size_t *transferred)
{
	unsigned long irq;
	int error;

	if (request == NULL || data == NULL || transferred == NULL)
		return EINVAL;
	*data = NULL;
	*transferred = 0;
	irq = spin_lock_irqsave(&request->endpoint->lock);

	if (request->state != REQUEST_DONE) {
		spin_unlock_irqrestore(&request->endpoint->lock, irq);
		return EAGAIN;
	}

	error = request->error;
	*transferred = request->transferred;
	if (error == 0 && request->bio.b_op == BIO_READ)
		*data = request->payload;

	spin_unlock_irqrestore(&request->endpoint->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

void
bio_async_ref(struct bio_async_request *request)
{
	if (request != NULL)
		refcount_get(&request->refs);
}

void
bio_async_release(struct bio_async_request *request)
{
	struct bio_async_endpoint *endpoint;
	unsigned long irq;

	if (request == NULL || !refcount_put(&request->refs))
		return;
	endpoint = request->endpoint;
	irq = spin_lock_irqsave(&endpoint->lock);

	if (request->state == REQUEST_QUEUED || request->state == REQUEST_RUNNING)
		HAL_FATAL("releasing active BIO ownership");
	request->state = REQUEST_RELEASING;

	spin_unlock_irqrestore(&endpoint->lock, irq);

	backing_mutation_end(&request->guard);
	backing_claim_release((struct backing_claim *)request->authorization);
	backing_claim_release((struct backing_claim *)request->context.claim);
	if (request->context.origin_inode != NULL)
		inode_release(request->context.origin_inode);
	disk_release(request->disk);
	disk_cache_release(request->cache_token);
	irq = spin_lock_irqsave(&endpoint->lock);

	request->state = REQUEST_FREE;
	endpoint->used--;

	spin_unlock_irqrestore(&endpoint->lock, irq);
}

/* Keeps normal BIO admission, sequencing, claims and error-prefix semantics. */
int
disk_transfer_vector_context(
	struct disk *disk,
	enum bio_op op,
	uint64_t block,
	const struct disk_vector *vectors,
	unsigned count,
	uint32_t *completed,
	const struct io_context *context)
{
	void *scratch;
	size_t total, capacity, offset, length, confirmed;
	uint32_t blocks, progress;
	unsigned index;
	int contiguous, error;

	if (completed == NULL)
		return EINVAL;
	*completed = 0;
	if (disk == NULL || vectors == NULL || count == 0 || count > DISK_VECTOR_MAX ||
	    (op != BIO_READ && op != BIO_WRITE) || disk->d_block_size == 0)
		return EINVAL;
	total = 0;
	contiguous = 1;
	for (index = 0; index < count; index++) {
		length = vectors[index].length;
		if (vectors[index].data == NULL || length == 0 ||
		    length % disk->d_block_size != 0 || length > KERN_IO_BATCH_MAX - total ||
		    length > UINTPTR_MAX - (uintptr_t)vectors[index].data)
			return EINVAL;
		if (index != 0 && (uintptr_t)vectors[index - 1U].data +
		    vectors[index - 1U].length != (uintptr_t)vectors[index].data)
			contiguous = 0;
		total += length;
	}

	blocks = (uint32_t)(total / disk->d_block_size);
	if (block >= disk->d_block_count || blocks > disk->d_block_count - block)
		return EINVAL;
	if (contiguous) {
		io_stats_record(IO_DISK_VECTOR_BATCH, total);
		return disk_transfer_progress_context(disk, op, block, blocks,
		    vectors[0].data, completed, context);
	}

	/* Normal admission uses one existing bounded scratch run, never an allocation. */
	scratch = NULL;
	capacity = 0;
	if (io_pool_borrow != NULL && io_pool_release != NULL)
		scratch = io_pool_borrow(total, &capacity);
	if (scratch != NULL && capacity < total) {
		io_pool_release(scratch);
		scratch = NULL;
	}

	if (scratch != NULL) {
		if (op == BIO_WRITE) {
			offset = 0;
			for (index = 0; index < count; index++) {
				memcpy((char *)scratch + offset, vectors[index].data, vectors[index].length);
				offset += vectors[index].length;
			}
		}

		io_stats_record(IO_DISK_VECTOR_BATCH, total);
		error = disk_transfer_progress_context(disk, op, block, blocks,
		    scratch, completed, context);
		if (*completed > blocks)
			HAL_FATAL("disk vector confirmed prefix exceeds request");
		confirmed = (size_t)*completed * disk->d_block_size;
		if (op == BIO_READ) {
			offset = 0;
			for (index = 0; index < count && offset < confirmed; index++) {
				length = vectors[index].length;
				if (length > confirmed - offset)
					length = confirmed - offset;
				memcpy(vectors[index].data, (char *)scratch + offset, length);
				offset += length;
			}
		}

		io_pool_release(scratch);
		return error;
	}

	/* Pool exhaustion retains progress through validated caller-owned spans. */
	io_stats_record(IO_DISK_VECTOR_SPLIT, total);
	for (index = 0; index < count; index++) {
		blocks = (uint32_t)(vectors[index].length / disk->d_block_size);
		error = disk_transfer_progress_context(disk, op, block + *completed,
		    blocks, vectors[index].data, &progress, context);
		if (progress > blocks)
			HAL_FATAL("disk vector split prefix exceeds request");
		*completed += progress;
		if (error != 0)
			return error;
		if (progress != blocks)
			return EIO;
	}

	return 0;
}

/* Checks external users separately from resident cache pins; registry locked. */
static int
disk_media_idle_locked(
	struct disk *parent,
	unsigned extra)
{
	struct disk *child;
	uint64_t expected;

	/* Only a live physical device whose media is gone may retire. */
	if (parent == NULL || disk_index(parent) < 0 ||
	    parent->d_state != DISK_LIVE || parent->d_parent != NULL ||
	    !atomic_raw_load_acquire(&parent->d_media_revoked))
		return EINVAL;

	/* Refuses a device that still has users or work in flight. */
	if (parent->d_open_count || parent->d_opening || parent->d_closing ||
	    parent->d_inflight || parent->d_cache_users || parent->d_reload_owner || parent->d_admin_owner)
		return EBUSY;

	/* Counts the references the caller is allowed to still hold. */
	expected = UINT64_C(1) + extra + parent->d_buffer_refs;
	for (child = disk_head; child != NULL; child = child->d_next) {
		if (child == parent || disk_leaf(child) != parent)
			continue;
		if (child->d_parent != parent || !(child->d_flags & DISK_PARTITION) ||
		    child->d_open_count || child->d_opening || child->d_closing ||
		    child->d_inflight || child->d_cache_users || child->d_buffer_refs ||
		    child->d_reload_owner || child->d_admin_owner || refcount_load(&child->d_refs) != 1)
			return EBUSY;
		expected++;
	}

	return refcount_load(&parent->d_refs) == expected ? 0 : EBUSY;
}

/* Retires a proof epoch; saturation never aliases a previously valid proof. */
static void
disk_persistence_invalidate_locked(
	struct disk *leaf)
{
	leaf->d_stable_valid = 0;
	if (leaf->d_persist_epoch != UINT64_MAX)
		leaf->d_persist_epoch++;
}

/* Appends an accepted write before the driver can complete it. */
static void
disk_write_accept(
	struct disk *leaf,
	struct bio *bio)
{
	unsigned long irq;

	/* Serializes the accepted frontier and intrusive outstanding order. */
	if (bio->b_op != BIO_WRITE)
		return;
	irq = spin_lock_irqsave(&leaf->d_lock);

	if (leaf->d_write_accepted != UINT64_MAX)
		leaf->d_write_accepted++;
	else
		disk_persistence_invalidate_locked(leaf);
	bio->b_write_sequence = leaf->d_write_accepted;
	bio->b_write_previous = leaf->d_write_tail;
	bio->b_write_next = NULL;
	if (leaf->d_write_tail != NULL)
		leaf->d_write_tail->b_write_next = bio;
	else
		leaf->d_write_head = bio;
	leaf->d_write_tail = bio;

	spin_unlock_irqrestore(&leaf->d_lock, irq);
}

/* Removes caller-owned storage before completion publication, preserving holes. */
static void
disk_write_retire(
	struct disk *leaf,
	struct bio *bio,
	int error,
	size_t transferred)
{
	unsigned long irq;
	uint64_t expected;

	/* Treats every transport error as an uncertain persistence boundary. */
	irq = spin_lock_irqsave(&leaf->d_lock);

	expected = (uint64_t)bio->b_block_count * leaf->d_block_size;
	if (error != 0 || (bio->b_op == BIO_WRITE && transferred != expected))
		disk_persistence_invalidate_locked(leaf);

	/* Detaches the write even when later writes completed first. */
	if (bio->b_op == BIO_WRITE) {
		if (bio->b_write_previous != NULL)
			bio->b_write_previous->b_write_next = bio->b_write_next;
		else
			leaf->d_write_head = bio->b_write_next;
		if (bio->b_write_next != NULL)
			bio->b_write_next->b_write_previous = bio->b_write_previous;
		else
			leaf->d_write_tail = bio->b_write_previous;
		bio->b_write_previous = NULL;
		bio->b_write_next = NULL;
		if (leaf->d_write_head != NULL)
			leaf->d_write_completed = leaf->d_write_head->b_write_sequence - 1U;
		else
			leaf->d_write_completed = leaf->d_write_accepted;
		waitq_wake_all(&leaf->d_waitq);
	}

	spin_unlock_irqrestore(&leaf->d_lock, irq);
}

/*
 * Submits a bio to the leaf disk that owns its blocks.
 *
 * The block range is checked at every level of the partition stack,
 * and the leaf's in-flight count and reference are held until
 * bio_complete().
 */
static int
bio_admit(
	struct disk *disk,
	struct bio *bio)
{
	struct disk *leaf;
	uint64_t mapped;
	unsigned long context_irq;
	int error;
	bool enabled;

	/* Rejects a missing operand, a reused bio, or a dead disk. */
	if (disk == NULL ||
	    bio == NULL ||
	    (bio->b_op != BIO_READ && bio->b_op != BIO_WRITE && bio->b_op != BIO_FLUSH) ||
	    bio->b_state != BIO_NEW ||
	    disk->d_state != DISK_LIVE)
		return EINVAL;

	error = io_context_validate(&bio->b_context);
	if (error != 0)
		return error;

	/* Initializes the completion state on first use. */
	if (!bio->b_initialized) {
		spin_init(&bio->b_lock, LOCK_RANK_DISK, "bio");
		waitq_init(&bio->b_waitq, "bio completion");
		bio->b_initialized = 1;
	}

	/* A transfer needs a range inside the disk; a flush needs none. */
	if (bio->b_op != BIO_FLUSH) {
		if (bio->b_block_count == 0 || bio->b_data == NULL)
			return EINVAL;
		if (bio->b_block >= disk->d_block_count ||
		    bio->b_block_count > disk->d_block_count - bio->b_block)
			return EOVERFLOW;
		if (bio->b_op == BIO_WRITE && (disk->d_flags & DISK_READ_ONLY))
			return EROFS;
	} else if (bio->b_block_count != 0 || bio->b_data != NULL) {
		return EINVAL;
	}

	/* Maps the range down through the partition stack to the leaf. */
	leaf = disk;
	mapped = bio->b_block;
	while (leaf->d_parent != NULL) {
		if (mapped >= leaf->d_block_count ||
		    bio->b_block_count > leaf->d_block_count - mapped ||
		    mapped > UINT64_MAX - leaf->d_parent_offset)
			return EOVERFLOW;
		mapped += leaf->d_parent_offset;
		leaf = leaf->d_parent;
		if (leaf->d_state != DISK_LIVE)
			return ENXIO;
	}

	if (leaf->d_ops == NULL || leaf->d_ops->submit == NULL)
		return EOPNOTSUPP;
	if (bio->b_op != BIO_FLUSH &&
	    (mapped >= leaf->d_block_count ||
	     bio->b_block_count > leaf->d_block_count - mapped))
		return EOVERFLOW;

	/* Marks the bio submitted and pins the leaf. */
	enabled = disk_lock();
	if (disk->d_state != DISK_LIVE || leaf->d_state != DISK_LIVE ||
	    disk_media_status(disk) != 0) {
		disk_unlock(enabled);
		return ENXIO;
	}

	/* Only the reload owner may issue direct I/O to the reserved physical disk. */
	if (disk_admin_conflict_locked(disk,
	    bio->b_op == BIO_FLUSH ? 0 : bio->b_block,
	    bio->b_op == BIO_FLUSH ? disk->d_block_count : bio->b_block_count, 1) != 0) {
		disk_unlock(enabled);
		return EBUSY;
	}
	if (leaf->d_reload_owner != NULL &&
	    (disk != leaf || thread_current == NULL ||
	     leaf->d_reload_owner != thread_current())) {
		disk_unlock(enabled);
		return EBUSY;
	}

	bio->b_disk = disk;
	bio->b_leaf_disk = leaf;
	bio->b_mapped_block = mapped;
	bio->b_transferred = 0;
	bio->b_error = 0;
	bio->b_state = BIO_SUBMITTED;
	leaf->d_inflight++;
	refcount_get(&leaf->d_refs);
	disk_unlock(enabled);

	context_irq = spin_lock_irqsave(&leaf->d_lock);

	bio->b_context.media_disk = leaf;
	bio->b_context.media_generation = leaf->d_media_epoch;

	spin_unlock_irqrestore(&leaf->d_lock, context_irq);

	disk_write_accept(leaf, bio);

	return 0;
}

/* Driver dispatch is separated from admission for bounded worker queues. */
static int
bio_dispatch(
	struct bio *bio)
{
	struct disk *leaf;

	/* Refuses to reach a driver whose media is gone. */
	leaf = bio->b_leaf_disk;
	if (disk_media_status(leaf) != 0)
		return ESTALE;

	/* Counts the request and hands it to the driver. */
	io_stats_record(bio->b_op == BIO_FLUSH ? IO_DRIVER_FLUSH :
	    (bio->b_op == BIO_READ ? IO_DRIVER_READ : IO_DRIVER_WRITE),
	    bio->b_op == BIO_FLUSH ? 0 :
	    (uint64_t)bio->b_block_count * leaf->d_block_size);
	return leaf->d_ops->submit(leaf, bio);
}

/* Takes the registry lock with interrupts disabled, reporting whether they were enabled. */
/* Follows physical ancestry while the caller holds the registry lock. */
static struct disk *
disk_leaf(
	struct disk *disk)
{
	/* Loop/file consumers remain covered independently by backing claims. */
	while (disk->d_parent != NULL)
		disk = disk->d_parent;

	/* Reports the physical ancestor. */
	return disk;
}

/* Checks every ordinary owner excluded by a physical partition reload. */
static int
disk_reload_idle(
	struct disk *parent)
{
	struct disk *disk;

	/* The administrative open must be the physical disk's sole active user. */
	if (parent->d_open_count != 1 ||
	    parent->d_opening != 0 ||
	    parent->d_closing != 0 ||
	    parent->d_inflight != 0 ||
	    parent->d_cache_users != 0)
		return EBUSY;

	/* Refuses nested devices and every externally referenced direct child. */
	for (disk = disk_head; disk != NULL; disk = disk->d_next) {
		if (disk == parent || disk_leaf(disk) != parent)
			continue;
		if (disk->d_parent != parent ||
		    (disk->d_flags & DISK_PARTITION) == 0 ||
		    disk->d_open_count != 0 ||
		    disk->d_opening != 0 ||
		    disk->d_closing != 0 ||
		    refcount_load(&disk->d_refs) != 1)
			return EBUSY;
	}

	/* Reports that the complete ancestry is idle. */
	return 0;
}

/* Validates and commits replacement children with the registry already locked. */
static int
disk_reload_replace_locked(
	struct disk *parent,
	struct disk **new_disks,
	unsigned count)
{
	struct disk *disk;
	struct disk *other;
	struct disk **link;
	struct disk **tail;
	unsigned i;
	unsigned j;
	int error;

	/* Requires the active reload owner and a bounded candidate list. */
	if (parent == NULL ||
	    count > DISK_MAX ||
	    (count != 0 && new_disks == NULL) ||
	    thread_current == NULL ||
	    parent->d_reload_owner != thread_current())
		return EINVAL;
	error = disk_reload_idle(parent);
	if (error != 0)
		return error;
	if (next_dev == 0 || count > UINT32_MAX - (uint32_t)next_dev)
		return ENOSPC;

	/* Rejects malformed, colliding or out-of-range children before publication. */
	for (i = 0; i < count; i++) {
		disk = new_disks[i];
		if (disk == NULL ||
		    disk_index(disk) < 0 ||
		    disk->d_state != DISK_ALLOCATED ||
		    disk->d_parent != parent ||
		    (disk->d_flags & DISK_PARTITION) == 0 ||
		    disk->d_block_size != parent->d_block_size ||
		    !name_valid(disk->d_name) ||
		    disk->d_block_count == 0 ||
		    disk->d_parent_offset >= parent->d_block_count ||
		    disk->d_block_count > parent->d_block_count - disk->d_parent_offset)
			return EINVAL;

		/* Rejects duplicate candidates and names owned by another physical disk. */
		for (j = 0; j < i; j++) {
			if (name_equal(disk->d_name, new_disks[j]->d_name))
				return EEXIST;
		}

		for (other = disk_head; other != NULL; other = other->d_next) {
			if (other->d_parent != parent &&
			    name_equal(disk->d_name, other->d_name))
				return EEXIST;
		}
	}

	/* Retires old slots only after every fallible check has passed. */
	for (link = &disk_head; (disk = *link) != NULL;) {
		if (disk->d_parent != parent) {
			link = &disk->d_next;
			continue;
		}

		*link = disk->d_next;
		disk->d_next = NULL;
		disk_persistence_invalidate(disk);
	disk->d_state = DISK_GONE;
		live_count--;
	}

	/* Appends new generations and takes their physical-parent references. */
	tail = link;
	for (i = 0; i < count; i++) {
		disk = new_disks[i];
		disk->d_dev = next_dev++;
		disk->d_state = DISK_LIVE;
		disk->d_next = NULL;
		refcount_get(&parent->d_refs);
		*tail = disk;
		tail = &disk->d_next;
		live_count++;
	}

	parent->d_identity_valid = 0;

	/* Reports the complete namespace replacement. */
	return 0;
}

/* Keeps cache hits under the same reload admission boundary as physical I/O. */
/* Admits a cached access under the disk's reload and lifetime barriers. */
static int
disk_cache_enter(
	struct disk *disk,
	struct disk **leaf_out)
{
	struct disk *leaf;
	bool enabled;

	/* Requires a live device with present media. */
	enabled = disk_lock();
	if (disk == NULL || disk->d_state != DISK_LIVE ||
	    disk_media_status(disk) != 0) {
		disk_unlock(enabled);
		return ENXIO;
	}

	/* Requires its physical device to be live as well. */
	leaf = disk_leaf(disk);
	if (leaf->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		return ENXIO;
	}

	/* Excludes everyone but the thread that is reloading the partitions. */
	if (disk_admin_conflict_locked(disk, 0, disk->d_block_count, 1) != 0) {
		disk_unlock(enabled);
		return EBUSY;
	}
	if (leaf->d_reload_owner != NULL &&
	    (disk != leaf || thread_current == NULL ||
	     leaf->d_reload_owner != thread_current())) {
		disk_unlock(enabled);
		return EBUSY;
	}

	/* Admits the caller as a cache user of the physical device. */
	leaf->d_cache_users++;
	refcount_get(&leaf->d_refs);
	disk_unlock(enabled);
	*leaf_out = leaf;

	/* Returns the admitted leaf identity. */
	return 0;
}

/* Releases one cached-access admission. */
static void
disk_cache_leave(
	struct disk *leaf)
{
	bool enabled;

	enabled = disk_lock();
	leaf->d_cache_users--;
	(void)refcount_put_not_last(&leaf->d_refs);
	disk_unlock(enabled);
}

/* Performs cached I/O without holding the registry lock over buffer operations. */
static int
disk_cached_transfer(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *data,
	int write,
	struct buf_view *view,
	const struct io_context *context)
{
	struct disk *leaf;
	int error;

	/* Retains lifecycle admission even when no physical I/O is required. */
	error = disk_cache_enter(disk, &leaf);
	if (error != 0)
		return error;
	if (write)
		error = buf_write_context(disk, block, count, data, context);
	else if (view != NULL)
		error = buf_read_view(disk, block, count, data, view);
	else
		error = buf_read(disk, block, count, data);
	disk_cache_leave(leaf);

	/* Preserves the cache operation's result. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Guards the disk registry without sleeping, reporting the interrupt state. */
static bool
disk_lock(
	void)
{
	bool enabled;

	/* Disables interrupts when the HAL provides the routine. */
	if (hal_irq_disable != NULL)
		enabled = hal_irq_disable();
	else
		enabled = false;

	/* Spins until the registry lock is free. */
	while (!atomic_try_acquire_zero(&disk_registry_lock))
		hal_compiler_barrier();

	/* Reports the previous interrupt state. */
	return enabled;
}

/* Releases the registry lock and restores the interrupt state. */
static void
disk_unlock(
	bool enabled)
{
	atomic_store_release(&disk_registry_lock, 0);
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

/* Reports the registry slot of a disk, or -1 for an unknown pointer. */
static int
disk_index(
	const struct disk *disk)
{
	unsigned i;

	for (i = 0; i < DISK_MAX; i++) {
		if (&disks[i] == disk)
			return (int)i;
	}

	return -1;
}

/* Zeroes a memory range without depending on the C library. */
static void
zero_bytes(
	void *p,
	size_t n)
{
	uint8_t *q;

	q = p;
	while (n != 0) {
		*q = 0;
		q++;
		n--;
	}
}

/* Compares two disk names for equality. */
static int
name_equal(
	const char *a,
	const char *b)
{
	/* Advances past the common prefix. */
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}

	/* The names match when both ended together. */
	if (*a != *b)
		return 0;
	return 1;
}

/* Tests that a name is non-empty and terminated within the field. */
static int
name_valid(
	const char name[DISK_NAME_MAX])
{
	unsigned i;

	/* Rejects an empty name. */
	if (name[0] == '\0')
		return 0;

	/* The terminator must lie inside the field. */
	for (i = 0; i < DISK_NAME_MAX; i++) {
		if (name[i] == '\0')
			return 1;
	}

	return 0;
}

/* Formats the sdX name of a disk number, using letters as bijective base 26. */
static int
sd_name(
	char name[DISK_NAME_MAX],
	unsigned number)
{
	char reverse[DISK_NAME_MAX];
	unsigned count;
	unsigned at;
	unsigned i;

	count = 0;
	at = 2;

	/* Builds the letters least significant first. */
	name[0] = 's';
	name[1] = 'd';
	do {
		if (count >= sizeof(reverse))
			return ENAMETOOLONG;
		reverse[count++] = (char)('a' + number % 26U);
		number /= 26U;
		if (number != 0)
			number--;
	} while (number != 0);

	/* Copies them into the name in display order. */
	if (at + count >= DISK_NAME_MAX)
		return ENAMETOOLONG;
	for (i = 0; i < count; i++)
		name[at++] = reverse[count - i - 1U];
	name[at] = '\0';

	/* Reports the formatted name. */
	return 0;
}

/* Appends a decimal number to a name at a cursor. */
static int
name_append_unsigned(
	char name[DISK_NAME_MAX],
	unsigned *at,
	unsigned value)
{
	char reverse[10];
	unsigned count;
	unsigned i;

	count = 0;

	/* Builds the digits least significant first. */
	do {
		if (count == sizeof(reverse))
			return ENAMETOOLONG;
		reverse[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0);

	/* Copies them into the name in display order. */
	if (*at + count >= DISK_NAME_MAX)
		return ENAMETOOLONG;
	for (i = 0; i < count; i++)
		name[(*at)++] = reverse[count - i - 1U];

	/* Reports the appended digits. */
	return 0;
}

/* Copies the public description of a disk. */
static void
disk_copy_info(
	const struct disk *disk,
	struct disk_info *info)
{
	unsigned i;

	/* Copies the public description of the device. */
	for (i = 0; i < DISK_NAME_MAX; i++)
		info->name[i] = disk->d_name[i];
	info->dev = disk->d_dev;
	info->flags = disk->d_flags;
	info->block_size = disk->d_block_size;
	info->block_count = disk->d_block_count;
}

/* Transfers blocks with direct bios, split at the driver's transfer limit. */
static int
disk_transfer_direct(
	struct disk *disk,
	enum bio_op op,
	uint64_t block,
	uint32_t count,
	void *data,
	const struct backing_claim *claim,
	uint32_t *completed,
	const struct io_context *context)
{
	struct io_context child;
	uint8_t *bytes;
	struct backing_mutation_guard guard;
	int guarded;
	int error;
	uint32_t chunk;
	struct bio bio;
	size_t expected;

	if (completed != NULL)
		*completed = 0;
	error = io_context_child(&child, context, IO_CONTEXT_DRAIN);
	if (error != 0)
		return error;
	bytes = data;
	guarded = 0;

	/* Rejects a missing operand or a byte count that overflows. */
	if (disk == NULL ||
	    data == NULL ||
	    count == 0 ||
	    disk->d_block_size == 0 ||
	    (size_t)count > SIZE_MAX / disk->d_block_size)
		return EINVAL;

	/* A write excludes conflicting claims on the range. */
	if (op == BIO_WRITE) {
		error = backing_mutation_begin_disk(disk, block, count,
						    claim, &guard);
		if (error != 0)
			return error;
		guarded = 1;
	}

	/* Transfers one chunk at a time, waiting for each. */
	while (count != 0) {
		chunk = count;
		if (disk->d_max_transfer_blocks != 0 &&
		    chunk > disk->d_max_transfer_blocks)
			chunk = disk->d_max_transfer_blocks;
		memset(&bio, 0, sizeof(bio));
		bio.b_context = child;
		bio.b_op = op;
		bio.b_block = block;
		bio.b_block_count = chunk;
		bio.b_data = bytes;
		error = bio_submit(disk, &bio);
		if (error != 0) {
			if (guarded)
				backing_mutation_end(&guard);
			return error;
		}

		error = bio_wait(&bio);
		if (error != 0) {
			if (guarded)
				backing_mutation_end(&guard);
			return error;
		}

		/* A short transfer is an I/O error. */
		expected = (size_t)chunk * disk->d_block_size;
		if (bio.b_transferred != expected) {
			if (guarded)
				backing_mutation_end(&guard);
			return EIO;
		}

		if (completed != NULL)
			*completed += chunk;
		block += chunk;
		count -= chunk;
		bytes += expected;
	}

	if (guarded)
		backing_mutation_end(&guard);

	/* Reports the completed transfer. */
	return 0;
}

/* Initializes synchronization only; device buffers are allocated on enable. */
static int
async_initialize(void)
{
	unsigned expected;
	unsigned index;

	if (atomic_load_acquire(&async_initialized) == 2)
		return 0;
	expected = 0;
	if (!atomic_compare_exchange(&async_initialized, &expected, 1))
		return EAGAIN;
	spin_init(&async_registry, LOCK_RANK_BIO_REGISTRY, "BIO endpoint registry");
	for (index = 0; index < ASYNC_ENDPOINTS; index++) {
		spin_init(&async_endpoints[index].lock, LOCK_RANK_BIO_QUEUE, "BIO worker queue");
		waitq_init(&async_endpoints[index].wake, "BIO worker");
	}

	atomic_store_release(&async_initialized, 2);
	return 0;
}

static void
async_unlink_locked(struct bio_async_endpoint *endpoint, struct bio_async_request *request)
{
	struct bio_async_request **link;
	struct bio_async_request *previous;

	previous = NULL;
	link = &endpoint->head;
	while (*link != request) {
		if (*link == NULL)
			HAL_FATAL("missing queued BIO");
		previous = *link;
		link = &previous->next;
	}

	*link = request->next;
	if (endpoint->tail == request)
		endpoint->tail = previous;
	request->next = NULL;
	endpoint->queued--;
}

static void
async_complete(struct bio *bio)
{
	struct bio_async_request *request;
	struct bio_async_endpoint *endpoint;
	bio_async_callback callback;
	void *argument;
	unsigned long irq;
	int error;

	request = (struct bio_async_request *)bio;
	endpoint = request->endpoint;
	bio_async_ref(request);
	error = bio->b_error;
	if (error == 0 && bio->b_op != BIO_FLUSH &&
	    bio->b_transferred != (uint64_t)bio->b_block_count * request->disk->d_block_size)
		error = EIO;
	irq = spin_lock_irqsave(&request->cache_token->d_lock);

	if (error == 0 && (request->cache_token->d_state != DISK_LIVE ||
	    request->epoch != request->cache_token->d_media_epoch))
		error = ESTALE;

	spin_unlock_irqrestore(&request->cache_token->d_lock, irq);

	backing_mutation_end(&request->guard);
	irq = spin_lock_irqsave(&endpoint->lock);

	request->error = error;
	request->transferred = bio->b_transferred;
	callback = request->callback;
	argument = request->argument;
	request->callback_active = 1;
	request->state = REQUEST_DONE;
	waitq_wake_all(&endpoint->wake);

	spin_unlock_irqrestore(&endpoint->lock, irq);

	if (callback != NULL)
		callback(request, argument);
	/* The worker retains its reference until callback return and this handoff. */
	bio_async_release(request);
	irq = spin_lock_irqsave(&endpoint->lock);

	request->callback_active = 0;
	waitq_wake_all(&endpoint->wake);

	spin_unlock_irqrestore(&endpoint->lock, irq);
}

static void
async_worker(void *argument)
{
	struct bio_async_endpoint *endpoint;
	struct bio_async_request *request;
	unsigned long irq;
	unsigned long disk_irq;
	uint64_t sequence;
	int error;

	endpoint = argument;
	for (;;) {
		irq = spin_lock_irqsave(&endpoint->lock);
		while (endpoint->head == NULL) {
			sequence = waitq_sequence(&endpoint->wake);
			(void)waitq_sleep(&endpoint->wake, &endpoint->lock, sequence, 0, 0);
		}

		request = endpoint->head;
		async_unlink_locked(endpoint, request);
		request->state = REQUEST_RUNNING;
		spin_unlock_irqrestore(&endpoint->lock, irq);
		disk_irq = spin_lock_irqsave(&request->cache_token->d_lock);
		error = request->cache_token->d_state != DISK_LIVE ? ENXIO :
		    (request->epoch != request->cache_token->d_media_epoch ? ESTALE : 0);
		spin_unlock_irqrestore(&request->cache_token->d_lock, disk_irq);
		if (error == 0)
			error = bio_dispatch(&request->bio);
		if (error != 0)
			bio_complete(&request->bio, error, 0);
		irq = spin_lock_irqsave(&endpoint->lock);
		while (request->state != REQUEST_DONE || request->callback_active) {
			sequence = waitq_sequence(&endpoint->wake);
			(void)waitq_sleep(&endpoint->wake, &endpoint->lock, sequence, 0, 0);
		}

		spin_unlock_irqrestore(&endpoint->lock, irq);
		bio_async_release(request);
	}
}
