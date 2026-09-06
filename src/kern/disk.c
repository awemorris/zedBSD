/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

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
#include "kern/backing-claim.h"
#include "kern/buf.h"
#include "kern/sched.h"
#include "kern/atomic.h"
#include "kern/test-checkpoint.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>
#include <zedbsd/block.h>

_Static_assert(DISK_NAME_MAX >= sizeof("nvme0n4294967295"),
    "DISK_NAME_MAX must represent every 32-bit NVMe namespace ID");

extern struct thread *thread_current(void) __attribute__((weak));
extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));

#define DISK_ALLOCATED 1U
#define DISK_LIVE 2U
#define DISK_GONE 3U
#define DISK_HIGH __attribute__((section(".hightext")))
#ifdef ZEDBSD_STORAGE_HOST_TEST
#undef DISK_HIGH
#define DISK_HIGH
#endif

static struct disk disks[DISK_MAX];
static uint8_t disk_used[DISK_MAX];
static struct disk *disk_head;
static unsigned live_count;
static dev_t next_dev = 1;
static atomic_uint_t disk_registry_lock;

static bool disk_lock(void);
static void disk_unlock(bool enabled);
static int disk_index(const struct disk *disk);
static void zero_bytes(void *p, size_t n);
static int name_equal(const char *a, const char *b);
static int name_valid(const char name[DISK_NAME_MAX]);
static int sd_name(char name[DISK_NAME_MAX], unsigned number);
static int name_append_unsigned(char name[DISK_NAME_MAX], unsigned *at, unsigned value);
static void disk_copy_info(const struct disk *disk, struct disk_info *info);
static int disk_transfer_direct(struct disk *disk, enum bio_op op, uint64_t block, uint32_t count, void *data, const struct backing_claim *claim);
static struct disk *disk_leaf(struct disk *disk);
static int disk_reload_idle(struct disk *parent);
static int disk_reload_replace_locked(struct disk *parent, struct disk **new_disks, unsigned count);
static int disk_cached_transfer(struct disk *disk, uint64_t block, uint32_t count, void *data, int write);

/*
 * Reserves one otherwise idle physical disk for partition-table reload.
 */
int
disk_reload_begin(
	struct disk *parent)
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
	    parent->d_parent != NULL ||
	    (parent->d_flags & DISK_PARTITION) != 0 ||
	    owner == NULL) {
		error = EINVAL;
	} else if (parent->d_reload_owner != NULL) {
		error = EBUSY;
	} else {
		error = disk_reload_idle(parent);
		if (error == 0)
			parent->d_reload_owner = owner;
	}
	disk_unlock(enabled);

	/* Reports whether this caller acquired the admission gate. */
	return error;
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
	return error;
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
	if (disk->d_parent != NULL && disk_leaf(disk)->d_reload_owner != NULL) {
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
	    backing_mutation_begin_disk(disk, 0, disk->d_block_count, NULL,
					&guard) != 0)
		return;

	/* Only a live disk is unlinked. */
	enabled = disk_lock();
	if (disk_leaf(disk)->d_reload_owner != NULL)
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
	disk->d_state = DISK_GONE;
out:
	disk_unlock(enabled);
	backing_mutation_end(&guard);
}

/*
 * Removes a live disk from the registry when nothing uses it.
 *
 * Open handles, in-flight I/O, or outside references keep the disk with
 * EBUSY; resident buffers are flushed and invalidated first.
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
	    disk_leaf(disk)->d_reload_owner != NULL) {
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
	disk->d_state = DISK_GONE;
	disk_unlock(enabled);
	error = 0;
out:
	backing_mutation_end(&guard);

	/* Reports the removal result. */
	return error;
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
	if (disk->d_parent != NULL)
		(void)refcount_put_not_last(&parent->d_refs);
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

	enabled = disk_lock();

	/* Ignores a missing or unknown disk. */
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

	enabled = disk_lock();

	/* Ignores a missing or unknown disk; the slot outlives the last reference. */
	if (disk != NULL && disk_index(disk) >= 0)
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

	/* Clears every slot and restarts device numbering. */
	enabled = disk_lock();
	zero_bytes(disks, sizeof(disks));
	for (i = 0; i < DISK_MAX; i++)
		disk_used[i] = 0;
	disk_head = NULL;
	live_count = 0;
	next_dev = 1;
	disk_unlock(enabled);
}

/*
 * Copies the versioned geometry of a referenced live block device.
 */
DISK_HIGH int
disk_block_info(
	struct disk *disk,
	struct zedbsd_block_info *info)
{
	bool enabled;
	unsigned i;

	/* Validates the caller's ABI and reserved fields before touching the disk. */
	if (info == NULL ||
	    info->version != ZEDBSD_BLOCK_VERSION ||
	    info->struct_size != sizeof(*info))
		return EINVAL;
	for (i = 0; i < 4; i++) {
		if (info->reserved[i] != 0)
			return EINVAL;
	}

	/* Rejects an absent disk while its publication state is locked. */
	enabled = disk_lock();
	if (disk == NULL ||
	    disk_index(disk) < 0 ||
	    disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		return ENXIO;
	}

	/* Publishes one coherent device, parent and sector-geometry snapshot. */
	memset(info, 0, sizeof(*info));
	info->version = ZEDBSD_BLOCK_VERSION;
	info->struct_size = sizeof(*info);
	info->device = (uint32_t)disk->d_dev;
	info->parent_device = disk->d_parent != NULL ?
	    (uint32_t)disk->d_parent->d_dev : 0;
	info->flags = disk->d_flags &
	    (DISK_READ_ONLY | DISK_REMOVABLE | DISK_PARTITION);
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

	/* Copies the description of the first name match. */
	enabled = disk_lock();
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

	/* Counts the live disks, then copies them when they fit. */
	enabled = disk_lock();
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
	    disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		return ENXIO;
	}

	/* Holds a temporary lifetime reference across the driver call. */
	if (disk_leaf(disk)->d_reload_owner != NULL) {
		disk_unlock(enabled);
		return EBUSY;
	}
	disk->d_opening++;
	refcount_get(&disk->d_refs);
	disk_unlock(enabled);
	if (disk->d_ops != NULL && disk->d_ops->open != NULL)
		error = disk->d_ops->open(disk);

	/* Counts the open only while the disk is still live. */
	enabled = disk_lock();
	disk->d_opening--;
	if (error == 0) {
		if (disk->d_state != DISK_LIVE) {
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

	/* Reports the open result. */
	return error;
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

	/* Reports the open result. */
	return error;
}

/*
 * Closes an open disk, calling the driver's close on every close.
 */
void
disk_close(
	struct disk *disk)
{
	bool enabled;

	enabled = disk_lock();

	/* Ignores a missing, unknown, or unopened disk. */
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
	    disk->d_state != DISK_LIVE) {
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

	/* Reports the driver's result. */
	return error;
}

/*
 * Submits a bio to the leaf disk that owns its blocks.
 *
 * The block range is checked at every level of the partition stack,
 * and the leaf's in-flight count and reference are held until
 * bio_complete().
 */
int
bio_submit(
	struct disk *disk,
	struct bio *bio)
{
	struct disk *leaf;
	uint64_t mapped;
	int error;
	bool enabled;

	/* Rejects a missing operand, a reused bio, or a dead disk. */
	if (disk == NULL ||
	    bio == NULL ||
	    bio->b_state != BIO_NEW ||
	    disk->d_state != DISK_LIVE)
		return EINVAL;

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
	if (disk->d_state != DISK_LIVE || leaf->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		return ENXIO;
	}

	/* Only the reload owner may issue direct I/O to the reserved physical disk. */
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

	/* Hands the bio to the driver, undoing the pin when it refuses. */
	error = leaf->d_ops->submit(leaf, bio);
	if (error != 0) {
		enabled = disk_lock();
		leaf->d_inflight--;
		(void)refcount_put_not_last(&leaf->d_refs);
		bio->b_state = BIO_NEW;
		bio->b_leaf_disk = NULL;
		disk_unlock(enabled);
	}

	/* Reports the submission result. */
	return error;
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
	bio->b_error = error;
	bio->b_transferred = transferred;
	bio->b_state = BIO_COMPLETED;
	waitq_wake_all(&bio->b_waitq);
	spin_unlock_irqrestore(&bio->b_lock, bio_irq);

	/* Unpins the leaf. */
	enabled = disk_lock();
	if (leaf != NULL && leaf->d_inflight != 0)
		leaf->d_inflight--;
	if (leaf != NULL)
		(void)refcount_put_not_last(&leaf->d_refs);
	done = bio->b_done;
	disk_unlock(enabled);

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
	if (disk->d_state != DISK_LIVE)
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
	if (bio == NULL || !bio->b_initialized)
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
			error = waitq_sleep(&bio->b_waitq, &bio->b_lock,
					    sequence, 0, 0);
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

	/* Reports the completion result. */
	return error;
}

/*
 * Flushes a disk's write cache and waits for the flush.
 */
int
bio_flush(
	struct disk *disk)
{
	struct bio bio;
	int error;

	/* Submits a flush bio and waits for it. */
	memset(&bio, 0, sizeof(bio));
	bio.b_op = BIO_FLUSH;
	error = bio_submit(disk, &bio);
	if (error != 0)
		return error;
	error = bio_wait(&bio);

	/* Reports the flush result. */
	return error;
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

	error = disk_transfer_direct(disk, BIO_READ, block, count, data, NULL);

	/* Reports the transfer result. */
	return error;
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

	error = disk_transfer_direct(disk, BIO_WRITE, block, count, (void *)data,
				     NULL);

	/* Reports the transfer result. */
	return error;
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
	error = disk_transfer_direct(disk, BIO_WRITE, block, count, (void *)data,
				     claim);

	/* Reports the transfer result. */
	return error;
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
	error = bio_flush(disk);

	/* Reports the flush result. */
	return error;
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

	error = disk_cached_transfer(disk, block, count, data, 0);

	/* Reports the read result. */
	return error;
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
	struct backing_mutation_guard guard;
	int error;

	/* Excludes conflicting claims on the range for the write. */
	error = backing_mutation_begin_disk(disk, block, count, NULL, &guard);
	if (error != 0)
		return error;
	error = disk_cached_transfer(disk, block, count, (void *)data, 1);
	backing_mutation_end(&guard);

	/* Reports the write result. */
	return error;
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
	struct backing_mutation_guard guard;
	int error;

	/* Excludes conflicting claims under the filesystem's own guard. */
	error = backing_mutation_begin_disk_filesystem(disk, block, count,
						       &guard);
	if (error != 0)
		return error;
	error = disk_cached_transfer(disk, block, count, (void *)data, 1);
	backing_mutation_end(&guard);

	/* Reports the write result. */
	return error;
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
static int
disk_cached_transfer(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *data,
	int write)
{
	struct disk *leaf;
	bool enabled;
	int error;

	/* Admits only a live disk and the reload owner's physical-disk requests. */
	enabled = disk_lock();
	if (disk == NULL || disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		return ENXIO;
	}
	leaf = disk_leaf(disk);
	if (leaf->d_reload_owner != NULL &&
	    (disk != leaf || thread_current == NULL ||
	     leaf->d_reload_owner != thread_current())) {
		disk_unlock(enabled);
		return EBUSY;
	}
	leaf->d_cache_users++;
	disk_unlock(enabled);

	/* Retains cache ownership even when no physical I/O is required. */
	if (write)
		error = buf_write(disk, block, count, data);
	else
		error = buf_read(disk, block, count, data);
	enabled = disk_lock();
	leaf->d_cache_users--;
	disk_unlock(enabled);

	/* Preserves the cache operation's result. */
	return error;
}

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
	const struct backing_claim *claim)
{
	uint8_t *bytes;
	struct backing_mutation_guard guard;
	int guarded;
	int error;
	uint32_t chunk;
	struct bio bio;
	size_t expected;

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
		block += chunk;
		count -= chunk;
		bytes += expected;
	}
	if (guarded)
		backing_mutation_end(&guard);

	/* Reports the completed transfer. */
	return 0;
}
