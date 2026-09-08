/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Swap.
 *
 * One unit holds the slot store that owns swapped page contents, the
 * sources that turn files and partitions into backing, the on-disk header
 * format, the control interface that adds and removes sources, and the
 * boot-time activation.
 */

#include "kern/swap.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include <errno.h>
#include <hal/hal.h>
#include <stddef.h>
#include <string.h>
#include <kern/swap-boot.h>
#include <kern/block-identity.h>
#include <kern/boot.h>
#include <kern/disk.h>
#include <kern/mount.h>
#include <kern/swap-control.h>
#include <kern/inode.h>
#include <kern/lock.h>
#include <kern/signal.h>
#include <kern/swap.h>
#include <kern/swap-source.h>
#include <kern/backing-claim.h>
#include <kern/buf.h>
#include <kern/fat.h>
#include <kern/file.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <kern/vm-commit.h>
#include <kern/vm-reclaim.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#define FAT_SWAP_EXTENT_MAX 1024U

struct fat_swap_extent {
	uint64_t file_block;
	uint64_t disk_block;
	uint32_t block_count;
};

struct file_swap_data {
	struct disk *disk;
	struct inode *inode;
	struct backing_claim *claim;
	unsigned disk_opened;
	struct fat_swap_extent extents[FAT_SWAP_EXTENT_MAX];
	unsigned extent_count;
	int extent_error;
};

struct raw_swap_data {
	struct disk *disk;
	struct backing_claim *claim;
	uint32_t blocks_per_page;
};

struct swap_disk_range {
	struct disk *leaf;
	uint64_t first;
	uint64_t last;
};

static struct swap_backend *system_backend;

static struct spinlock swap_lock = {
	{ 0 }, LOCK_RANK_SWAP, "swap backend", 0, 0
};

static struct kern_swap_control_registration control_registration;

static unsigned control_registered;

static unsigned control_busy;

static struct spinlock control_lock = {
	{ 0 }, LOCK_RANK_SWAP, "swap control", 0, 0
};

static unsigned active_extent_count;

static struct spinlock swap_source_lock = {
	{ 0 }, LOCK_RANK_SWAP, "swap sources", 0, 0
};

static int swap_manager_enable_transition(struct swap_backend *backend, int *enabled_here);
static void swap_manager_disable_empty(struct swap_backend *backend);
static int swap_io(struct swap_backend *backend, uint32_t slot, void *page, int write);
static int is_boot_reference(const char *value);
extern struct thread *thread_current(void);
static int selector_validate(const char *selector);
static int selector_is_disk(const char *selector);
static int control_enter(struct kern_swap_control_registration *registration);
static void control_leave(void);
static int control_snapshot_registration(struct kern_swap_control_registration *registration);
static int control_resolve_identity(const struct kern_swap_control_registration *control, const char *selector, struct path *path, struct disk **disk, struct inode **identity_inode);
static void control_release_identity(struct path *path, struct disk *disk, struct inode *identity_inode);
static int control_source_identity_matches(const struct kern_swap_source *source, struct disk *disk, struct inode *identity_inode);
static int control_revalidate_identity(const struct kern_swap_control_registration *control, const char *selector, const struct kern_swap_source *source);
static int control_cancelled(void *argument);
static uint32_t get32(const uint8_t *p);
static uint16_t get16(const uint8_t *p);
static uint64_t get64(const uint8_t *p);
extern unsigned vm_object_cache_drain(struct mount *) __attribute__((weak));
extern int disk_write_direct_claimed(struct disk *, uint64_t, uint32_t,
    const void *, const struct backing_claim *) __attribute__((weak));
extern int mount_sync(struct mount *) __attribute__((weak));
extern int mount_disk_writable_busy(struct disk *) __attribute__((weak));
extern int buf_invalidate(struct disk *, uint64_t, uint64_t, unsigned)
    __attribute__((weak));
extern int vm_reclaim_drain_swap_source_cancelable(unsigned,
    int (*)(void *), void *) __attribute__((weak));
static void source_metadata_changed_locked(struct kern_swap_source_set *set);
static void active_extents_add(unsigned count);
static void active_extents_remove(unsigned count);
static int collect_extent(uint64_t file_block, uint64_t disk_block, uint32_t count, void *argument);
static int validate_extents(struct file_swap_data *data, uint64_t bytes);
static int swap_disk_write(struct disk *disk, uint64_t block, uint32_t count, const void *page, const struct backing_claim *claim);
static int file_swap_io(struct file_swap_data *data, uint32_t slot, void *page, int write);
static int file_swap_read(void *argument, uint32_t slot, void *page);
static int file_swap_write(void *argument, uint32_t slot, const void *page);
static int file_swap_flush(void *argument);
static void file_swap_destroy(void *argument);
static int raw_swap_io(struct raw_swap_data *data, uint32_t slot, void *page, int write);
static int raw_swap_read(void *argument, uint32_t slot, void *page);
static int raw_swap_write(void *argument, uint32_t slot, const void *page);
static int raw_swap_flush(void *argument);
static void raw_swap_destroy(void *argument);
static int source_identity_equal(const struct kern_swap_source *left, const struct kern_swap_source *right);
static int source_identity_matches(const struct kern_swap_source *source, struct disk *identity_disk, struct inode *identity_inode);
static int swap_disk_range_resolve(struct disk *disk, struct swap_disk_range *range);
static int source_set_control_enter(struct kern_swap_source_set *set);
static void source_set_control_leave(struct kern_swap_source_set *set);
static int source_set_current_total(struct kern_swap_source_set *set, uint32_t *total);

static const struct swap_backend_ops file_swap_ops = {
	.read_page = file_swap_read,
	.write_page = file_swap_write,
	.flush = file_swap_flush,
	.destroy = file_swap_destroy,
};

static const struct swap_backend_ops raw_swap_ops = {
	.read_page = raw_swap_read,
	.write_page = raw_swap_write,
	.flush = raw_swap_flush,
	.destroy = raw_swap_destroy,
};

/*
 * Initializes an empty, disabled backend.
 */
void
swap_init(
	struct swap_backend *backend)
{
	/* Ignores a missing backend. */
	if (backend != NULL)
		memset(backend, 0, sizeof(*backend));
}

/*
 * Encodes a source and local slot as a global slot number.
 */
int
swap_slot_encode(
	unsigned source_id,
	uint32_t local_slot,
	uint32_t *slot)
{
	/* Rejects a source or slot out of range, or a missing result. */
	if (source_id >= SWAP_SOURCE_COUNT ||
	    local_slot > SWAP_SLOT_LOCAL_MASK ||
	    slot == NULL)
		return EINVAL;

	*slot = ((uint32_t)source_id << SWAP_SLOT_SOURCE_SHIFT) | local_slot;

	/* Reports the encoded slot. */
	return 0;
}

/*
 * Decodes a global slot number into its source and local slot.
 */
int
swap_slot_decode(
	uint32_t slot,
	unsigned *source_id,
	uint32_t *local_slot)
{
	/* Rejects a slot with bits outside the encoding. */
	if ((slot & ~SWAP_SLOT_VALID_MASK) != 0)
		return EINVAL;

	/* Reports the parts that were asked for. */
	if (source_id != NULL)
		*source_id = (slot & SWAP_SLOT_SOURCE_MASK) >>
		    SWAP_SLOT_SOURCE_SHIFT;
	if (local_slot != NULL)
		*local_slot = slot & SWAP_SLOT_LOCAL_MASK;
	return 0;
}

/*
 * Enables an empty backend.
 */
int
swap_manager_enable(
	struct swap_backend *backend)
{
	int error;

	error = swap_manager_enable_transition(backend, NULL);

	/* Reports the enable result. */
	return error;
}

/*
 * Prepares a source with its driver hooks and slot bitmap.
 *
 * The source is not visible to allocation until it is published.
 */
int
swap_source_prepare(
	struct swap_backend *backend,
	unsigned source_id,
	const struct swap_backend_ops *ops,
	void *data,
	uint32_t page_size,
	uint32_t slot_count)
{
	struct swap_backend_source *source;
	size_t bytes;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	unsigned long irq;

	/* Rejects a missing operand, incomplete hooks, or a bad geometry. */
	if (backend == NULL ||
	    ops == NULL ||
	    ops->read_page == NULL ||
	    ops->write_page == NULL ||
	    source_id >= SWAP_SOURCE_COUNT ||
	    page_size != SWAP_PAGE_SIZE ||
	    slot_count == 0)
		return EINVAL;
	if (slot_count > SWAP_SOURCE_MAX_SLOTS)
		return EOVERFLOW;
#if SIZE_MAX <= UINT32_MAX
	if ((size_t)slot_count > SIZE_MAX - 7U ||
	    (size_t)slot_count > SIZE_MAX / sizeof(*slot_inflight))
		return EOVERFLOW;
#endif

	/* Allocates the bitmap and the per-slot I/O bookkeeping. */
	bytes = ((size_t)slot_count + 7U) / 8U;
	bitmap = kern_calloc(1, bytes);
	slot_inflight = kern_calloc(slot_count, sizeof(*slot_inflight));
	slot_pending_free = kern_calloc(slot_count,
	    sizeof(*slot_pending_free));
	if (bitmap == NULL ||
	    slot_inflight == NULL ||
	    slot_pending_free == NULL) {
		kern_free(slot_pending_free);
		kern_free(slot_inflight);
		kern_free(bitmap);
		return ENOMEM;
	}

	/* Installs them in an inactive source of an enabled backend. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled ||
	    backend->shutting_down ||
	    source->state != SWAP_SOURCE_STATE_INACTIVE) {
		spin_unlock_irqrestore(&swap_lock, irq);
		kern_free(slot_pending_free);
		kern_free(slot_inflight);
		kern_free(bitmap);
		return EBUSY;
	}
	source->bitmap = bitmap;
	source->slot_inflight = slot_inflight;
	source->slot_pending_free = slot_pending_free;
	source->ops = ops;
	source->data = data;
	source->page_size = page_size;
	source->slot_count = slot_count;
	source->free_slots = slot_count;
	source->state = SWAP_SOURCE_STATE_PREPARED;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the prepared source. */
	return 0;
}

/*
 * Makes a prepared source available for allocation.
 */
int
swap_source_publish(
	struct swap_backend *backend,
	unsigned source_id)
{
	struct swap_backend_source *source;
	unsigned long irq;

	/* Rejects a missing backend or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Only a prepared source of an enabled backend can be published. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	if (source->state != SWAP_SOURCE_STATE_PREPARED) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	if (backend->slot_count > UINT32_MAX - source->slot_count ||
	    backend->free_slots > UINT32_MAX - source->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EOVERFLOW;
	}

	/* Adds its slots to the backend totals. */
	backend->slot_count += source->slot_count;
	backend->free_slots += source->slot_count;
	backend->source_count++;
	source->state = SWAP_SOURCE_STATE_ACTIVE;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the published source. */
	return 0;
}

/*
 * Discards a prepared source that will not be published.
 */
int
swap_source_cancel_prepare(
	struct swap_backend *backend,
	unsigned source_id)
{
	struct swap_backend_source *source;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	unsigned long irq;

	/* Rejects a missing backend or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Only a prepared source of an enabled backend can be cancelled. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	if (source->state != SWAP_SOURCE_STATE_PREPARED) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}

	/* Keeps a tombstone until all manager-owned metadata is released. */
	bitmap = source->bitmap;
	slot_inflight = source->slot_inflight;
	slot_pending_free = source->slot_pending_free;
	source->state = SWAP_SOURCE_STATE_REMOVING;
	source->ops = NULL;
	source->data = NULL;
	source->bitmap = NULL;
	source->slot_inflight = NULL;
	source->slot_pending_free = NULL;
	spin_unlock_irqrestore(&swap_lock, irq);
	kern_free(slot_pending_free);
	kern_free(slot_inflight);
	kern_free(bitmap);

	/* Clears the tombstone. */
	irq = spin_lock_irqsave(&swap_lock);
	memset(source, 0, sizeof(*source));
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the cancelled source. */
	return 0;
}

/*
 * Prepares and publishes a source, enabling the backend if needed.
 *
 * A backend enabled here is disabled again when the source cannot be
 * added.
 */
int
swap_source_add(
	struct swap_backend *backend,
	unsigned source_id,
	const struct swap_backend_ops *ops,
	void *data,
	uint32_t page_size,
	uint32_t slot_count)
{
	int enabled_here;
	int error;

	/* Enables the backend for its first source. */
	error = swap_manager_enable_transition(backend, &enabled_here);
	if (error != 0)
		return error;

	/* Prepares the source, undoing the enable on failure. */
	error = swap_source_prepare(backend, source_id, ops, data, page_size,
	    slot_count);
	if (error != 0) {
		if (enabled_here)
			swap_manager_disable_empty(backend);
		return error;
	}

	/* Publishes it, cancelling the preparation on failure. */
	error = swap_source_publish(backend, source_id);
	if (error != 0) {
		if (swap_source_cancel_prepare(backend, source_id) != 0)
			HAL_FATAL("swap source publish rollback failed");
		if (enabled_here)
			swap_manager_disable_empty(backend);
	}

	/* Reports the add result. */
	return error;
}

/*
 * Adds source zero, the boot swap source.
 */
int
swap_activate(
	struct swap_backend *backend,
	const struct swap_backend_ops *ops,
	void *data,
	uint32_t page_size,
	uint32_t slot_count)
{
	int error;

	error = swap_source_add(backend, 0, ops, data, page_size, slot_count);

	/* Reports the add result. */
	return error;
}

/*
 * Allocates a free slot from the first active source that has one.
 */
int
swap_alloc_slot(
	struct swap_backend *backend,
	uint32_t *slot)
{
	struct swap_backend_source *source;
	unsigned source_id;
	unsigned long irq;
	uint32_t index;
	uint8_t mask;

	/* Rejects a missing backend or result. */
	if (backend == NULL || slot == NULL)
		return EINVAL;

	/* Only an enabled backend allocates. */
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}

	/* Scans the active sources for a clear bitmap bit. */
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		source = &backend->source[source_id];
		if (source->state != SWAP_SOURCE_STATE_ACTIVE ||
		    source->free_slots == 0)
			continue;
		for (index = 0; index < source->slot_count; index++) {
			mask = (uint8_t)(1U << (index & 7U));
			if (!(source->bitmap[index >> 3] & mask)) {
				source->bitmap[index >> 3] |= mask;
				source->slot_pending_free[index] = 0;
				source->free_slots--;
				backend->free_slots--;
				*slot = ((uint32_t)source_id <<
				    SWAP_SLOT_SOURCE_SHIFT) | index;
				spin_unlock_irqrestore(&swap_lock, irq);
				return 0;
			}
		}
	}
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports exhausted swap. */
	return ENOSPC;
}

/*
 * Frees a slot, deferring the release while I/O on it is in flight.
 */
void
swap_free_slot(
	struct swap_backend *backend,
	uint32_t slot)
{
	struct swap_backend_source *source;
	unsigned source_id;
	uint32_t local_slot;
	uint8_t mask;
	unsigned long irq;

	/* Ignores a missing backend or a malformed slot. */
	if (backend == NULL ||
	    swap_slot_decode(slot, &source_id, &local_slot) != 0)
		return;

	/* Only a slot of an active or draining source can be freed. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled ||
	    (source->state != SWAP_SOURCE_STATE_ACTIVE &&
	     source->state != SWAP_SOURCE_STATE_DRAINING) ||
	    local_slot >= source->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return;
	}

	/* Releases the slot now, or once its I/O completes. */
	mask = (uint8_t)(1U << (local_slot & 7U));
	if (source->bitmap[local_slot >> 3] & mask) {
		if (source->slot_inflight[local_slot] != 0) {
			source->slot_pending_free[local_slot] = 1;
		} else {
			source->bitmap[local_slot >> 3] &= (uint8_t)~mask;
			source->free_slots++;
			backend->free_slots++;
		}
	}
	spin_unlock_irqrestore(&swap_lock, irq);
}

/*
 * Reads a page from a slot.
 */
int
swap_read_page(
	struct swap_backend *backend,
	uint32_t slot,
	void *page)
{
	int error;

	error = swap_io(backend, slot, page, 0);

	/* Reports the read result. */
	return error;
}

/*
 * Writes a page to a slot.
 */
int
swap_write_page(
	struct swap_backend *backend,
	uint32_t slot,
	const void *page)
{
	int error;

	error = swap_io(backend, slot, (void *)page, 1);

	/* Reports the write result. */
	return error;
}

/*
 * Flushes every active and draining source.
 *
 * The sources are held in flight while their flush hooks run outside
 * the lock; the first error is reported after every source was flushed.
 */
int
swap_flush(
	struct swap_backend *backend)
{
	const struct swap_backend_ops *ops[SWAP_SOURCE_COUNT];
	struct swap_backend_source *source;
	void *data[SWAP_SOURCE_COUNT];
	uint8_t reserved[SWAP_SOURCE_COUNT];
	unsigned long irq;
	unsigned source_id;
	int first_error;
	int error;

	first_error = 0;

	/* Rejects a missing backend. */
	if (backend == NULL)
		return EINVAL;
	memset(reserved, 0, sizeof(reserved));

	/* Only an enabled backend with no removal in progress flushes. */
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		source = &backend->source[source_id];
		if (source->state == SWAP_SOURCE_STATE_REMOVING) {
			spin_unlock_irqrestore(&swap_lock, irq);
			return EBUSY;
		}
	}

	/* Holds every live source in flight and remembers its hooks. */
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		source = &backend->source[source_id];
		if (source->state != SWAP_SOURCE_STATE_ACTIVE &&
		    source->state != SWAP_SOURCE_STATE_DRAINING)
			continue;
		reserved[source_id] = 1;
		ops[source_id] = source->ops;
		data[source_id] = source->data;
		source->inflight++;
		backend->inflight++;
	}
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Flushes outside the lock, keeping the first error. */
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		if (!reserved[source_id])
			continue;
		if (ops[source_id]->flush != NULL)
			error = ops[source_id]->flush(data[source_id]);
		else
			error = 0;
		if (first_error == 0 && error != 0)
			first_error = error;
	}

	/* Releases the in-flight holds. */
	irq = spin_lock_irqsave(&swap_lock);
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		if (reserved[source_id]) {
			backend->source[source_id].inflight--;
			backend->inflight--;
		}
	}
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the first flush failure. */
	return first_error;
}

/*
 * Stops allocation from a source so that it can be emptied and removed.
 */
int
swap_source_begin_drain(
	struct swap_backend *backend,
	unsigned source_id)
{
	struct swap_backend_source *source;
	unsigned long irq;

	/* Rejects a missing backend or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Only an active or already draining source of a live backend drains. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || source->state == SWAP_SOURCE_STATE_INACTIVE) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	if (backend->shutting_down ||
	    (source->state != SWAP_SOURCE_STATE_ACTIVE &&
	     source->state != SWAP_SOURCE_STATE_DRAINING)) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	source->state = SWAP_SOURCE_STATE_DRAINING;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the draining source. */
	return 0;
}

/*
 * Returns a draining source to allocation.
 */
int
swap_source_abort_drain(
	struct swap_backend *backend,
	unsigned source_id)
{
	struct swap_backend_source *source;
	unsigned long irq;

	/* Rejects a missing backend or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Only a draining source of a live backend can be reactivated. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || source->state == SWAP_SOURCE_STATE_INACTIVE) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	if (backend->shutting_down ||
	    source->state != SWAP_SOURCE_STATE_DRAINING) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	source->state = SWAP_SOURCE_STATE_ACTIVE;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the reactivated source. */
	return 0;
}

/*
 * Reports the state and slot counts of a source.
 */
int
swap_source_get_stats(
	struct swap_backend *backend,
	unsigned source_id,
	struct swap_source_stats *stats)
{
	struct swap_backend_source *source;
	unsigned long irq;

	/* Rejects a missing operand or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT || stats == NULL)
		return EINVAL;

	/* Samples the source under the lock. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	stats->source_id = source_id;
	stats->state = source->state;
	stats->total_slots = source->slot_count;
	stats->free_slots = source->free_slots;
	stats->allocated_slots = source->slot_count - source->free_slots;
	stats->inflight = source->inflight;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the sampled statistics. */
	return 0;
}

/*
 * Removes a drained source whose slots are all free.
 *
 * The source is flushed and destroyed outside the lock while a tombstone
 * keeps its identifier from being reused.
 */
int
swap_source_remove(
	struct swap_backend *backend,
	unsigned source_id)
{
	struct swap_backend_source *source;
	const struct swap_backend_ops *ops;
	void *data;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	uint32_t slot_count;
	unsigned long irq;
	int flush_error;

	/* Rejects a missing backend or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Only an idle, empty, draining source of a live backend is removed. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || source->state == SWAP_SOURCE_STATE_INACTIVE) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	if (backend->shutting_down ||
	    source->state != SWAP_SOURCE_STATE_DRAINING ||
	    source->inflight != 0 ||
	    source->free_slots != source->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}

	/* Excludes I/O, shutdown, and ID reuse while flush is in progress. */
	source->state = SWAP_SOURCE_STATE_REMOVING;
	source->inflight = 1;
	backend->inflight++;
	ops = source->ops;
	data = source->data;
	spin_unlock_irqrestore(&swap_lock, irq);
	if (ops->flush != NULL)
		flush_error = ops->flush(data);
	else
		flush_error = 0;

	/* A failed flush puts the source back to draining. */
	irq = spin_lock_irqsave(&swap_lock);
	backend->inflight--;
	source->inflight--;
	if (flush_error != 0) {
		source->state = SWAP_SOURCE_STATE_DRAINING;
		spin_unlock_irqrestore(&swap_lock, irq);
		return flush_error;
	}

	/*
	 * Keeps a REMOVING tombstone until callbacks and frees finish.  This
	 * prevents a concurrent add from reusing the numeric ID while the
	 * old lifecycle is still observable, without freeing memory under a
	 * lock.
	 */
	bitmap = source->bitmap;
	slot_inflight = source->slot_inflight;
	slot_pending_free = source->slot_pending_free;
	slot_count = source->slot_count;
	source->ops = NULL;
	source->data = NULL;
	source->bitmap = NULL;
	source->slot_inflight = NULL;
	source->slot_pending_free = NULL;
	backend->slot_count -= slot_count;
	backend->free_slots -= slot_count;
	backend->source_count--;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Destroys the driver state and the metadata. */
	if (ops->destroy != NULL)
		ops->destroy(data);
	kern_free(slot_pending_free);
	kern_free(slot_inflight);
	kern_free(bitmap);

	/* Clears the tombstone. */
	irq = spin_lock_irqsave(&swap_lock);
	memset(source, 0, sizeof(*source));
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the removed source. */
	return 0;
}

/*
 * Shuts an idle backend down, flushing and destroying every source.
 *
 * The backend must have no I/O in flight and every slot free.  The
 * sources are detached under the lock and flushed before they are
 * destroyed, in numeric order.
 */
int
swap_shutdown(
	struct swap_backend *backend)
{
	struct swap_backend_source detached[SWAP_SOURCE_COUNT];
	struct swap_backend_source *source;
	struct swap_backend_source *detached_source;
	unsigned long irq;
	unsigned source_id;
	int flush_error;
	int error;

	flush_error = 0;

	/* A missing or disabled backend needs no shutdown. */
	if (backend == NULL)
		return 0;
	memset(detached, 0, sizeof(detached));
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return 0;
	}

	/* Every source must be idle and empty, with no transition under way. */
	if (backend->shutting_down || backend->inflight != 0) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		source = &backend->source[source_id];
		if (source->state == SWAP_SOURCE_STATE_PREPARED ||
		    source->state == SWAP_SOURCE_STATE_REMOVING ||
		    source->inflight != 0 ||
		    (source->state != SWAP_SOURCE_STATE_INACTIVE &&
		     source->free_slots != source->slot_count)) {
			spin_unlock_irqrestore(&swap_lock, irq);
			return EBUSY;
		}
	}

	/* Detaches the sources, leaving tombstones. */
	backend->shutting_down = 1;
	if (system_backend == backend)
		system_backend = NULL;
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		source = &backend->source[source_id];
		if (source->state == SWAP_SOURCE_STATE_INACTIVE)
			continue;
		detached[source_id] = *source;
		memset(source, 0, sizeof(*source));
		source->state = SWAP_SOURCE_STATE_REMOVING;
	}
	backend->slot_count = 0;
	backend->free_slots = 0;
	backend->source_count = 0;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Preserves numeric flush-before-destroy ordering from the boot backend. */
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		detached_source = &detached[source_id];
		if (detached_source->state == SWAP_SOURCE_STATE_INACTIVE)
			continue;
		if (detached_source->ops->flush != NULL)
			error = detached_source->ops->flush(detached_source->data);
		else
			error = 0;
		if (flush_error == 0 && error != 0)
			flush_error = error;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		detached_source = &detached[source_id];
		if (detached_source->state == SWAP_SOURCE_STATE_INACTIVE)
			continue;
		if (detached_source->ops->destroy != NULL)
			detached_source->ops->destroy(detached_source->data);
		kern_free(detached_source->slot_pending_free);
		kern_free(detached_source->slot_inflight);
		kern_free(detached_source->bitmap);
	}

	/* Clears the backend. */
	irq = spin_lock_irqsave(&swap_lock);
	memset(backend, 0, sizeof(*backend));
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the first flush failure. */
	return flush_error;
}

/*
 * Publishes a backend as the system swap backend.
 */
int
swap_set_system_backend(
	struct swap_backend *backend)
{
	unsigned long irq;
	int error;

	error = 0;

	/* Rejects a missing backend. */
	if (backend == NULL)
		return EINVAL;

	irq = spin_lock_irqsave(&swap_lock);

	/*
	 * Publication is part of the backend lifecycle transaction.  Check
	 * the live state while holding the same lock used by swap_shutdown();
	 * a pre-lock check can otherwise publish a backend after shutdown has
	 * detached and destroyed its data.
	 */
	if (!backend->enabled || backend->shutting_down)
		error = ENXIO;
	else if (system_backend != NULL && system_backend != backend)
		error = EBUSY;
	else
		system_backend = backend;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the publication result. */
	return error;
}

/*
 * Reads the system swap backend, or none.
 */
struct swap_backend *
swap_system_backend(
	void)
{
	struct swap_backend *backend;
	unsigned long irq;

	/* Samples the pointer under the lock. */
	irq = spin_lock_irqsave(&swap_lock);
	backend = system_backend;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the backend, or none. */
	return backend;
}

/*
 * Reports the total and free slot counts of a backend.
 */
int
swap_get_stats(
	struct swap_backend *backend,
	uint32_t *total,
	uint32_t *free_slots)
{
	unsigned long irq;

	/* Rejects a missing operand. */
	if (backend == NULL || total == NULL || free_slots == NULL)
		return EINVAL;

	/* Only an enabled backend has statistics. */
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	*total = backend->slot_count;
	*free_slots = backend->free_slots;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the sampled counts. */
	return 0;
}

/*
 * Prepares the swap source set from the boot parameters.
 *
 * On failure the set is aborted and failed_parameter names the parameter
 * that could not be prepared.
 */
int
kern_swap_boot_prepare(
	const struct kern_boot_parameters *parameters,
	struct kern_boot_source_context *boot_sources,
	struct kern_swap_source_set *swap_sources,
	unsigned *failed_parameter)
{
	struct kern_swap_source source;
	struct path path;
	struct disk *disk;
	const char *value;
	unsigned parameter;
	unsigned boot_slot;
	int error;

	error = 0;

	/* Rejects a missing operand. */
	if (parameters == NULL || boot_sources == NULL || swap_sources == NULL)
		return EINVAL;

	/* Starts with an empty set and no failed parameter. */
	kern_swap_source_set_init(swap_sources);
	if (failed_parameter != NULL)
		*failed_parameter = KERN_SWAP_SOURCE_COUNT;

	/* Prepares each named source in parameter order. */
	for (parameter = 0; parameter < KERN_SWAP_SOURCE_COUNT; parameter++) {
		value = kern_boot_parameters_swap(parameters, parameter);
		if (value == NULL)
			continue;
		kern_swap_source_init(&source);

		/* A boot reference is a swap file on a boot source. */
		if (is_boot_reference(value)) {
			path_init(&path);
			error = kern_boot_source_lookup(boot_sources, value,
			    &boot_slot, &path);
			if (error == 0)
				error = kern_swap_source_prepare_file(&path,
				    parameter, &source);
			path_release(&path);
			if (error == 0)
				error = kern_swap_source_set_diagnostic(&source, value);
			if (error == 0)
				error = kern_swap_source_set_add(swap_sources,
				    &source);
			if (error == 0)
				error = kern_boot_source_retain_slot(boot_sources,
				    boot_slot);
		} else {
			/* Anything else selects a disk used as raw swap. */
			disk = NULL;
			error = kern_boot_source_selector_validate(value);
			if (error == 0)
				error = block_identity_resolve(value, &disk);
			if (error == 0)
				error = kern_swap_source_prepare_raw(disk, parameter,
				    &source);
			if (disk != NULL)
				disk_release(disk);
			if (error == 0)
				error = kern_swap_source_set_diagnostic(&source, value);
			if (error == 0)
				error = kern_swap_source_set_add(swap_sources,
				    &source);
		}

		/* The first failure abandons the whole set. */
		if (error != 0) {
			kern_swap_source_destroy(&source);
			if (failed_parameter != NULL)
				*failed_parameter = parameter;
			(void)kern_swap_source_set_abort(swap_sources);
			return error;
		}
	}

	/* Reports the prepared set. */
	return 0;
}

/*
 * Registers the swap source set and selector resolver.
 *
 * Only one registration is accepted, and none while a control operation
 * is in progress.
 */
int
kern_swap_control_register(
	const struct kern_swap_control_registration *registration)
{
	unsigned long irq;
	int error;

	error = 0;

	/* Rejects an incomplete registration or an inactive source set. */
	if (registration == NULL ||
	    registration->sources == NULL ||
	    registration->resolver == NULL ||
	    registration->resolver->resolve_path == NULL ||
	    registration->resolver->resolve_disk == NULL ||
	    registration->resolver->validate_raw == NULL ||
	    !registration->sources->active)
		return EINVAL;

	/* Installs the registration unless one exists or is in use. */
	irq = spin_lock_irqsave(&control_lock);
	if (control_registered || control_busy) {
		error = EBUSY;
	} else {
		control_registration = *registration;
		control_registered = 1;
	}
	spin_unlock_irqrestore(&control_lock, irq);

	/* Reports the registration result. */
	return error;
}

/*
 * Adds the swap source a selector names.
 *
 * The selector is resolved, the source is prepared as a file or a raw
 * disk, its identity is checked against the lookup and against a second
 * resolution of the selector, and it is published to the source set.
 */
int
kern_swap_control_add(
	const char *selector)
{
	struct kern_swap_control_registration control;
	struct kern_swap_source source;
	struct path path;
	struct disk *disk;
	struct inode *identity_inode;
	unsigned source_id;
	int error;

	/* Validates the selector and takes the control operation. */
	error = selector_validate(selector);
	if (error != 0)
		return error;
	error = control_enter(&control);
	if (error != 0)
		return error;

	/* Resolves the selector to a disk and possibly a file. */
	kern_swap_source_init(&source);
	error = control_resolve_identity(&control, selector, &path, &disk,
	    &identity_inode);
	if (error != 0)
		goto out;

	/* Refuses a source that is already published. */
	if (disk != NULL)
		error = kern_swap_source_set_find_identity(control.sources, disk,
		    identity_inode, &source_id);
	else
		error = ENOENT;
	if (error == 0) {
		error = EEXIST;
		goto out_release;
	}
	if (error != ENOENT)
		goto out_release;

	/* Prepares the source as a file or as a validated raw disk. */
	if (identity_inode != NULL) {
		error = kern_swap_source_prepare_file(&path, 0, &source);
	} else {
		error = control.resolver->validate_raw(control.resolver_context,
		    disk);
		if (error == 0)
			error = kern_swap_source_prepare_raw(disk, 0, &source);
	}
	if (error != 0)
		goto out_release;

	/* The prepared source must first match the retained lookup object. */
	if (!control_source_identity_matches(&source, disk, identity_inode)) {
		error = EAGAIN;
		goto out_destroy;
	}

	/* The selector must still name the source after the backing claim. */
	error = control_revalidate_identity(&control, selector, &source);
	if (error != 0)
		goto out_destroy;

	/* Publishes the source under its selector. */
	error = kern_swap_source_set_diagnostic(&source, selector);
	if (error == 0)
		error = kern_swap_source_set_runtime_add(control.sources, &source,
		    NULL);
out_destroy:
	kern_swap_source_destroy(&source);
out_release:
	control_release_identity(&path, disk, identity_inode);
out:
	control_leave();

	/* Reports the add result. */
	return error;
}

/*
 * Removes the swap source a selector names.
 *
 * The removal migrates the source's pages elsewhere and can be cancelled
 * by a signal to the calling thread.
 */
int
kern_swap_control_remove(
	const char *selector)
{
	struct kern_swap_control_registration control;
	struct path path;
	struct disk *disk;
	struct inode *identity_inode;
	unsigned source_id;
	int error;

	/* Validates the selector and takes the control operation. */
	error = selector_validate(selector);
	if (error != 0)
		return error;
	error = control_enter(&control);
	if (error != 0)
		return error;

	/* Resolves the selector to the published source. */
	error = control_resolve_identity(&control, selector, &path, &disk,
	    &identity_inode);
	if (error != 0)
		goto out;
	if (disk != NULL)
		error = kern_swap_source_set_find_identity(control.sources, disk,
		    identity_inode, &source_id);
	else
		error = ENOENT;

	/* Removes it, letting a signal cancel the migration. */
	if (error == 0)
		error = kern_swap_source_set_runtime_remove_cancelable(
		    control.sources, source_id, control_cancelled,
		    thread_current());
	control_release_identity(&path, disk, identity_inode);
out:
	control_leave();

	/* Reports the removal result. */
	return error;
}

/*
 * Describes one swap source by identifier.
 */
int
kern_swap_control_get(
	unsigned source_id,
	struct kern_swap_control_source_info *result)
{
	struct kern_swap_control_registration control;
	struct kern_swap_source_snapshot snapshot;
	int error;

	/* Rejects a missing result or an impossible identifier. */
	if (result == NULL || source_id >= KERN_SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Snapshots the source through the registered set. */
	error = control_snapshot_registration(&control);
	if (error != 0)
		return error;
	error = kern_swap_source_set_snapshot(control.sources, source_id,
	    &snapshot);
	if (error != 0)
		return error;

	/* Copies the snapshot into the caller's record. */
	memset(result, 0, sizeof(*result));
	result->source_id = snapshot.source_id;
	result->state = snapshot.state;
	result->header_version = snapshot.header_version;
	result->total_pages = snapshot.total_pages;
	result->used_pages = snapshot.used_pages;
	memcpy(result->uuid, snapshot.uuid, sizeof(result->uuid));
	memcpy(result->label, snapshot.label, sizeof(result->label));
	memcpy(result->source, snapshot.diagnostic, sizeof(result->source));

	/* Reports the described source. */
	return 0;
}

/*
 * Computes the production FNV checksum with a zero checksum field.
 */
uint32_t
swap_header_checksum(
	const uint8_t *header)
{
	uint32_t hash = 2166136261U;
	unsigned checksum_offset;
	unsigned i;
	uint8_t byte;

	/* Rejects an absent header before inspecting its magic. */
	if (header == NULL)
		return 0;

	/* Selects the checksum field of the encoded version. */
	checksum_offset = memcmp(header, "ZEDSWAP2", 8U) == 0 ? 60U : 28U;

	/* Includes all header bytes with the checksum field treated as zero. */
	for (i = 0; i < ZEDBSD_SWAP_HEADER_SIZE; i++) {
		byte = i >= checksum_offset &&
		    i < checksum_offset + 4U ? 0 : header[i];
		hash = (hash ^ byte) * 16777619U;
	}

	/* Returns the on-disk checksum value. */
	return hash;
}

/*
 * Validates the production swap header against its backing length.
 */
int
swap_header_parse(
	const uint8_t *header,
	uint64_t backing_bytes,
	struct swap_header_info *result)
{
	static const uint8_t magic_v1[8] = {
		'Z', 'E', 'D', 'S', 'W', 'A', 'P', '1'
	};
	static const uint8_t magic_v2[8] = {
		'Z', 'E', 'D', 'S', 'W', 'A', 'P', '2'
	};
	struct swap_header_info parsed;
	uint64_t slots;
	uint8_t byte;
	unsigned i;
	int terminated;

	/* Requires a header page and complete data slots. */
	if (header == NULL || backing_bytes < SWAP_PAGE_SIZE * 2ULL ||
	    backing_bytes % SWAP_PAGE_SIZE != 0)
		return EINVAL;

	/* Initializes the result before selecting a supported header version. */
	memset(&parsed, 0, sizeof(parsed));
	if (memcmp(header, magic_v1, sizeof(magic_v1)) == 0) {
		/* Validates the legacy fixed-size header. */
		if ((backing_bytes != ZEDBSD_SWAP_FILE_MIN_BYTES &&
		     backing_bytes != ZEDBSD_SWAP_FILE_MAX_BYTES) ||
		    get32(header + 8U) != 1U ||
		    get32(header + 12U) != ZEDBSD_SWAP_HEADER_SIZE ||
		    get32(header + 16U) != SWAP_PAGE_SIZE ||
		    get32(header + 20U) != backing_bytes ||
		    get32(header + 28U) != swap_header_checksum(header))
			return EINVAL;

		/* Confirms the exact legacy slot count. */
		slots = backing_bytes / SWAP_PAGE_SIZE - 1U;
		if (get32(header + 24U) != slots)
			return EINVAL;

		/* Rejects nonzero bytes reserved by version one. */
		for (i = 32U; i < ZEDBSD_SWAP_HEADER_SIZE; i++) {
			/* Requires the reserved byte to be zero. */
			if (header[i] != 0U)
				return EINVAL;
		}

		/* Publishes the validated legacy geometry. */
		parsed.version = 1U;
		parsed.backing_bytes = backing_bytes;
		parsed.slot_count = slots;
	} else if (memcmp(header, magic_v2, sizeof(magic_v2)) == 0) {
		/* Computes the modern format's caller-sized slot range. */
		slots = backing_bytes / SWAP_PAGE_SIZE - 1U;
		terminated = 0;

		/* Validates the version-two geometry and checksum. */
		if (get16(header + 8U) != 2U ||
		    get16(header + 10U) != ZEDBSD_SWAP_HEADER_SIZE ||
		    get32(header + 12U) != SWAP_PAGE_SIZE ||
		    get64(header + 16U) != backing_bytes ||
		    get64(header + 24U) != slots ||
		    get32(header + 60U) != swap_header_checksum(header))
			return EINVAL;

		/* Copies the optional version-two UUID. */
		for (i = 0; i < ZEDBSD_SWAP_V2_UUID_SIZE; i++)
			parsed.uuid[i] = header[32U + i];

		/* Requires a printable label with a zero-padded terminator. */
		for (i = 0; i < ZEDBSD_SWAP_V2_LABEL_SIZE; i++) {
			/* Reads one label byte before checking termination. */
			byte = header[40U + i];

			/* Rejects nonzero padding after the terminator. */
			if (terminated && byte != 0U)
				return EINVAL;

			/* Accepts the terminator or one printable ASCII byte. */
			if (!terminated && byte == 0U)
				terminated = 1;
			else if (!terminated && (byte < 0x20U || byte > 0x7eU))
				return EINVAL;

			/* Preserves the validated byte in the result. */
			parsed.label[i] = (char)byte;
		}

		/* Rejects a label without a terminator. */
		if (!terminated)
			return EINVAL;

		/* Publishes the validated version-two geometry. */
		parsed.version = 2U;
		parsed.backing_bytes = backing_bytes;
		parsed.slot_count = slots;
	} else {
		return EINVAL;
	}

	/* Copies the validated result only when requested. */
	if (result != NULL)
		*result = parsed;

	/* Reports a recognized complete swap header. */
	return 0;
}

/*
 * Validates a header without requesting decoded attributes.
 */
int
swap_header_validate(
	const uint8_t *header,
	uint64_t backing_bytes)
{
	int error;

	/* Uses the same parser for validation-only callers. */
	error = swap_header_parse(header, backing_bytes, NULL);

	/* Returns the parser's exact error. */
	return error;
}

/*
 * Formats a present swap UUID into the existing hexadecimal representation.
 */
int
swap_header_uuid_format(
	const struct swap_header_info *header,
	char *output,
	size_t capacity)
{
	static const char digits[] = "0123456789ABCDEF";
	unsigned i;
	int present = 0;

	/* Requires enough room for the hexadecimal UUID and terminator. */
	if (header == NULL || output == NULL || capacity < 17U)
		return EINVAL;

	/* Detects whether the header carries a UUID at all. */
	for (i = 0; i < ZEDBSD_SWAP_V2_UUID_SIZE; i++)
		present |= header->uuid[i] != 0U;

	/* Distinguishes an absent UUID from an all-zero printed identifier. */
	if (!present) {
		output[0] = '\0';
		return ENOENT;
	}

	/* Visits every byte of the UUID. */
	for (i = 0; i < ZEDBSD_SWAP_V2_UUID_SIZE; i++) {
		output[i * 2U] = digits[header->uuid[i] >> 4];
		output[i * 2U + 1U] = digits[header->uuid[i] & 15U];
	}

	/* Terminates the validated representation. */
	output[16] = '\0';

	/* Reports a present formatted UUID. */
	return 0;
}

/*
 * Clears a swap source description.
 */
void
kern_swap_source_init(
	struct kern_swap_source *source)
{
	if (source != NULL)
		memset(source, 0, sizeof(*source));
}

/*
 * Prepares a swap file on a writable FAT mount as a source.
 *
 * The file's extents are collected and validated, claimed against other
 * writers, and dropped from the buffer cache.  The source is not yet
 * part of any set.
 */
int
kern_swap_source_prepare_file(
	const struct path *path,
	unsigned parameter_index,
	struct kern_swap_source *source)
{
	struct file_swap_data *data;
	struct swap_header_info header_info;
	struct file *file;
	uint8_t header[ZEDBSD_SWAP_HEADER_SIZE];
	uint64_t bytes;
	struct backing_claim_extent *claim_extents;
	ssize_t header_bytes;
	int error;
	unsigned index;

	data = NULL;
	file = NULL;
	claim_extents = NULL;

	/* Rejects a missing path or source, or a parameter index out of range. */
	if (path == NULL ||
	    path->p_inode == NULL ||
	    source == NULL ||
	    parameter_index >= KERN_SWAP_SOURCE_COUNT)
		return EINVAL;
	kern_swap_source_init(source);

	/* The file must be a writable regular file on a writable FAT disk. */
	error = file_open_resolved(path, O_RDWR, &file);
	if (error != 0)
		return error;
	if (file->f_inode == NULL ||
	    file->f_inode->i_type != INODE_REG ||
	    file->f_inode->i_size < 0) {
		error = EINVAL;
		goto out;
	}
	if (file->f_inode->i_mount == NULL ||
	    file->f_inode->i_mount->m_disk == NULL ||
	    file->f_inode->i_mount->m_type != &drv_fat_filesystem_type) {
		error = EOPNOTSUPP;
		goto out;
	}
	if ((file->f_inode->i_mount->m_flags & MOUNT_READ_ONLY) != 0 ||
	    (file->f_inode->i_mount->m_disk->d_flags & DISK_READ_ONLY) != 0 ||
	    (file->f_inode->i_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0) {
		error = EROFS;
		goto out;
	}

	/* Pins the inode and disk, syncing the mount so the extents are final. */
	data = kern_calloc(1, sizeof(*data));
	if (data == NULL) {
		error = ENOMEM;
		goto out;
	}
	data->disk = file->f_inode->i_mount->m_disk;
	data->inode = file->f_inode;
	inode_ref(data->inode);
	if (mount_sync != NULL)
		error = mount_sync(file->f_inode->i_mount);
	else
		error = 0;
	if (error != 0)
		goto out;
	error = disk_open(data->disk);
	if (error != 0)
		goto out;
	data->disk_opened = 1;
	/* Retires optional caches before reserving independent backing ownership. */
	if (vm_object_cache_drain != NULL)
		(void)vm_object_cache_drain(NULL);
	error = backing_claim_prepare_inode(file->f_inode, BACKING_CLAIM_SWAP,
	    &data->claim);
	if (error != 0)
		goto out;

	/* Reads and checks the swap header. */
	bytes = (uint64_t)file->f_inode->i_size;
	header_bytes = file_pread(file, header, sizeof(header), 0);
	if (header_bytes != (ssize_t)sizeof(header)) {
		if (header_bytes < 0)
			error = (int)-header_bytes;
		else
			error = EIO;
		goto out;
	}
	if (swap_header_parse(header, bytes, &header_info) != 0 ||
	    header_info.slot_count == 0 ||
	    header_info.slot_count > UINT32_MAX) {
		error = EINVAL;
		goto out;
	}
	if (data->disk->d_block_size != 512U) {
		error = EIO;
		goto out;
	}

	/* Collects the extents and claims them. */
	error = drv_fat_file_extents(file, collect_extent, data);
	if (error != 0) {
		if (data->extent_error != 0)
			error = data->extent_error;
		else
			error = EIO;
		goto out;
	}
	error = validate_extents(data, bytes);
	if (error != 0)
		goto out;
	claim_extents = kern_calloc(data->extent_count, sizeof(*claim_extents));
	if (claim_extents == NULL) {
		error = ENOMEM;
		goto out;
	}
	for (index = 0; index < data->extent_count; index++) {
		claim_extents[index].disk = data->disk;
		claim_extents[index].block = data->extents[index].disk_block;
		claim_extents[index].block_count =
		    data->extents[index].block_count;
	}
	error = backing_claim_finalize(data->claim, claim_extents,
	    data->extent_count);
	kern_free(claim_extents);
	claim_extents = NULL;
	if (error != 0)
		goto out;

	/* Drops the cached blocks so swap I/O never races the buffer cache. */
	for (index = 0; index < data->extent_count; index++) {
		if (buf_invalidate != NULL)
			error = buf_invalidate(data->disk,
			    data->extents[index].disk_block,
			    data->extents[index].block_count,
			    BUF_INVALIDATE_DISCARD);
		else
			error = 0;
		if (error != 0)
			goto out;
	}

	/* Marks the inode as a swap file unless it is already special. */
	mutex_lock(&data->inode->i_lock);
	if ((data->inode->i_flags & (INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
		mutex_unlock(&data->inode->i_lock);
		error = EBUSY;
		goto out;
	}
	data->inode->i_flags |= INODE_SWAPFILE;
	mutex_unlock(&data->inode->i_lock);

	/* Fills the source description. */
	source->ops = &file_swap_ops;
	source->data = data;
	source->identity_disk = data->disk;
	source->identity_inode = data->inode;
	source->slot_count = (uint32_t)header_info.slot_count;
	source->header_version = header_info.version;
	memcpy(source->uuid, header_info.uuid, sizeof(source->uuid));
	memcpy(source->label, header_info.label, sizeof(source->label));
	source->parameter_index = parameter_index;
	active_extents_add(data->extent_count);
	data = NULL;
	error = 0;
out:
	if (file != NULL)
		(void)file_close(file);
	if (data != NULL)
		backing_claim_release(data->claim);
	if (data != NULL && data->disk_opened)
		disk_close(data->disk);
	if (data != NULL && data->inode != NULL)
		inode_release(data->inode);
	if (data != NULL)
		kern_free(data);
	kern_free(claim_extents);

	/* Reports the preparation result. */
	return error;
}

/*
 * Prepares a raw partition as a swap source.
 *
 * The partition must be writable, unmounted for writing, and carry a
 * valid swap header in its first page.
 */
int
kern_swap_source_prepare_raw(
	struct disk *disk,
	unsigned parameter_index,
	struct kern_swap_source *source)
{
	struct raw_swap_data *data;
	struct swap_header_info header_info;
	uint8_t *header_page;
	uint64_t bytes;
	uint32_t blocks_per_page;
	int error;
	struct backing_claim *claim;

	data = NULL;
	header_page = NULL;

	/* Rejects anything but a writable partition whose blocks tile a page. */
	if (disk == NULL ||
	    source == NULL ||
	    parameter_index >= KERN_SWAP_SOURCE_COUNT ||
	    (disk->d_flags & DISK_PARTITION) == 0 ||
	    (disk->d_flags & DISK_READ_ONLY) != 0 ||
	    disk->d_block_size == 0 ||
	    SWAP_PAGE_SIZE % disk->d_block_size != 0 ||
	    disk->d_block_count > UINT64_MAX / disk->d_block_size)
		return EINVAL;
	kern_swap_source_init(source);
	blocks_per_page = SWAP_PAGE_SIZE / disk->d_block_size;
	bytes = disk->d_block_count * disk->d_block_size;

	/* Opens the disk and refuses one with a writable mount. */
	error = disk_open(disk);
	if (error != 0)
		return error;
	if (mount_disk_writable_busy != NULL)
		error = mount_disk_writable_busy(disk);
	else
		error = 0;
	if (error != 0) {
		disk_close(disk);
		return error;
	}

	/* Claims the whole partition. */
	claim = NULL;
	error = backing_claim_prepare_disk(disk, 0, disk->d_block_count,
	    BACKING_CLAIM_SWAP, &claim);
	if (error != 0) {
		disk_close(disk);
		return error;
	}
	data = kern_calloc(1, sizeof(*data));
	if (data == NULL) {
		backing_claim_release(claim);
		disk_close(disk);
		return ENOMEM;
	}
	data->claim = claim;

	/* Reads and checks the swap header. */
	header_page = kern_malloc(SWAP_PAGE_SIZE);
	if (header_page == NULL) {
		backing_claim_release(data->claim);
		kern_free(data);
		disk_close(disk);
		return ENOMEM;
	}
	error = disk_read_direct(disk, 0, blocks_per_page, header_page);
	if (error != 0) {
		kern_free(header_page);
		backing_claim_release(data->claim);
		kern_free(data);
		disk_close(disk);
		return error;
	}
	if (swap_header_parse(header_page, bytes, &header_info) != 0 ||
	    header_info.slot_count == 0 ||
	    header_info.slot_count > UINT32_MAX) {
		kern_free(header_page);
		backing_claim_release(data->claim);
		kern_free(data);
		disk_close(disk);
		return EINVAL;
	}
	kern_free(header_page);

	/* Fills the source description. */
	disk_ref(disk);
	data->disk = disk;
	data->blocks_per_page = blocks_per_page;
	source->ops = &raw_swap_ops;
	source->data = data;
	source->identity_disk = disk;
	source->slot_count = (uint32_t)header_info.slot_count;
	source->header_version = header_info.version;
	memcpy(source->uuid, header_info.uuid, sizeof(source->uuid));
	memcpy(source->label, header_info.label, sizeof(source->label));
	source->parameter_index = parameter_index;

	/* Reports the prepared source. */
	return 0;
}

/*
 * Releases a prepared source that was never added to a set.
 */
void
kern_swap_source_destroy(
	struct kern_swap_source *source)
{
	/* Ignores a missing source. */
	if (source == NULL)
		return;

	/* Lets the backend release its data, then clears the description. */
	if (source->ops != NULL &&
	    source->ops->destroy != NULL &&
	    source->data != NULL)
		source->ops->destroy(source->data);
	kern_swap_source_init(source);
}

/*
 * Records a diagnostic text for a source.
 */
int
kern_swap_source_set_diagnostic(
	struct kern_swap_source *source,
	const char *diagnostic)
{
	size_t length;

	/* Rejects a missing source or text. */
	if (source == NULL || diagnostic == NULL)
		return EINVAL;

	/* The text must be non-empty and fit the field. */
	for (length = 0; length <= KERN_SWAP_SOURCE_TEXT_MAX; length++) {
		if (diagnostic[length] == '\0')
			break;
	}
	if (length == 0 || length > KERN_SWAP_SOURCE_TEXT_MAX)
		return EINVAL;
	memcpy(source->diagnostic, diagnostic, length + 1U);

	/* Reports the recorded text. */
	return 0;
}

/*
 * Initializes an empty, inactive source set.
 */
void
kern_swap_source_set_init(
	struct kern_swap_source_set *set)
{
	if (set != NULL) {
		memset(set, 0, sizeof(*set));
		swap_init(&set->backend);
	}
}

/*
 * Adds a prepared source to an inactive set.
 *
 * The set takes over the description and clears the caller's copy.
 */
int
kern_swap_source_set_add(
	struct kern_swap_source_set *set,
	struct kern_swap_source *source)
{
	uint64_t total;
	unsigned source_id;
	unsigned index;
	int error;
	const struct kern_swap_source *existing;
	unsigned long irq;

	total = 0;

	/* Rejects an incomplete source description. */
	if (set == NULL ||
	    source == NULL ||
	    source->ops == NULL ||
	    source->ops->read_page == NULL ||
	    source->ops->write_page == NULL ||
	    source->ops->destroy == NULL ||
	    source->data == NULL ||
	    source->slot_count == 0 ||
	    source->slot_count > SWAP_SOURCE_MAX_SLOTS ||
	    source->parameter_index >= KERN_SWAP_SOURCE_COUNT ||
	    (source->identity_inode == NULL && source->identity_disk == NULL))
		return EINVAL;

	/* Only an inactive set with a free slot accepts sources. */
	error = source_set_control_enter(set);
	if (error != 0)
		return error;
	if (set->active || set->count >= KERN_SWAP_SOURCE_COUNT) {
		error = EINVAL;
		goto out;
	}
	source_id = source->parameter_index;
	if (set->range[source_id].source.ops != NULL) {
		error = EEXIST;
		goto out;
	}

	/* The identity must be new and the slot total must fit 32 bits. */
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		existing = &set->range[index].source;
		if (existing->ops == NULL)
			continue;
		total += existing->slot_count;
		if (source_identity_equal(source, existing)) {
			error = EEXIST;
			goto out;
		}
	}
	if (total > UINT32_MAX - source->slot_count) {
		error = EOVERFLOW;
		goto out;
	}

	/* Takes over the description. */
	irq = spin_lock_irqsave(&swap_source_lock);
	set->range[source_id].source = *source;
	set->count++;
	source_metadata_changed_locked(set);
	spin_unlock_irqrestore(&swap_source_lock, irq);
	kern_swap_source_init(source);
	error = 0;
out:
	source_set_control_leave(set);

	/* Reports the add result. */
	return error;
}

/*
 * Checks that no raw source overlaps the native root partition.
 */
int
kern_swap_source_set_validate_native_root(
	const struct kern_swap_source_set *set,
	struct disk *root_disk)
{
	struct swap_disk_range root_range;
	unsigned index;
	int error;
	const struct kern_swap_source *source;
	struct swap_disk_range swap_range;

	/* Rejects a missing set or root disk. */
	if (set == NULL || root_disk == NULL)
		return EINVAL;

	/* Resolves the root's leaf range. */
	error = swap_disk_range_resolve(root_disk, &root_range);
	if (error != 0)
		return error;

	/* Compares every raw source's leaf range against it. */
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		source = &set->range[index].source;
		if (source->ops == NULL)
			continue;
		if (source->identity_inode != NULL)
			continue;
		if (source->identity_disk == NULL)
			return EINVAL;
		error = swap_disk_range_resolve(source->identity_disk,
		    &swap_range);
		if (error != 0)
			return error;
		if ((swap_range.leaf == root_range.leaf ||
		     swap_range.leaf->d_dev == root_range.leaf->d_dev) &&
		    swap_range.first <= root_range.last &&
		    root_range.first <= swap_range.last)
			return EEXIST;
	}

	/* Reports no overlap. */
	return 0;
}

/*
 * Maps a global swap slot to its source and local slot.
 */
int
kern_swap_source_set_map(
	const struct kern_swap_source_set *set,
	uint32_t global_slot,
	unsigned *source_index,
	uint32_t *local_slot)
{
	unsigned source_id;
	uint32_t local;

	/* Rejects a missing set or an undecodable slot. */
	if (set == NULL ||
	    swap_slot_decode(global_slot, &source_id, &local) != 0)
		return EINVAL;

	/* The source must exist and hold the local slot. */
	if (set->range[source_id].source.ops == NULL ||
	    local >= set->range[source_id].source.slot_count)
		return ERANGE;
	if (source_index != NULL)
		*source_index = source_id;
	if (local_slot != NULL)
		*local_slot = local;

	/* Reports the mapped slot. */
	return 0;
}

/*
 * Activates a set as the system swap backend in one transaction.
 *
 * Every source is prepared, the commit limit grown, and the sources
 * published; any failure rolls all of it back and destroys the sources.
 */
int
kern_swap_source_set_activate(
	struct kern_swap_source_set *set)
{
	uint64_t total;
	unsigned prepared;
	unsigned published;
	unsigned index;
	int manager_enabled;
	int commit_grown;
	int error;
	struct kern_swap_source *source;

	total = 0;
	prepared = 0;
	published = 0;
	manager_enabled = 0;
	commit_grown = 0;

	/* Rejects a missing set. */
	if (set == NULL)
		return EINVAL;

	/* Only an inactive set can be activated. */
	error = source_set_control_enter(set);
	if (error != 0)
		return error;
	if (set->active) {
		error = EINVAL;
		goto out;
	}

	/* The slot total must fit 32 bits. */
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		source = &set->range[index].source;
		if (source->ops == NULL)
			continue;
		total += source->slot_count;
		if (total > UINT32_MAX) {
			error = EOVERFLOW;
			goto fail;
		}
	}
	error = swap_manager_enable(&set->backend);
	if (error != 0)
		goto fail;
	manager_enabled = 1;

	/*
	 * Publish the empty manager first.  This reserves the singleton backend
	 * even when boot supplied no swap source.
	 */
	error = swap_set_system_backend(&set->backend);
	if (error != 0)
		goto fail;

	/* Prepares every source, grows the commit limit, then publishes them. */
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		source = &set->range[index].source;
		if (source->ops == NULL)
			continue;
		error = swap_source_prepare(&set->backend, index, source->ops,
		    source->data, SWAP_PAGE_SIZE, source->slot_count);
		if (error != 0)
			goto fail;
		prepared |= 1U << index;
	}
	error = vm_commit_resize_swap(0, total);
	if (error != 0)
		goto fail;
	commit_grown = 1;
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		if ((prepared & (1U << index)) == 0)
			continue;
		error = swap_source_publish(&set->backend, index);
		if (error != 0)
			goto fail;
		prepared &= ~(1U << index);
		published |= 1U << index;
	}
	set->active = 1;
	error = 0;
	goto out;

fail:
	/* Undoes the commit growth, the preparations, and the manager. */
	if (commit_grown && vm_commit_resize_swap(total, 0) != 0)
		HAL_FATAL("boot swap commitment rollback failed");
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		if ((prepared & (1U << index)) != 0 &&
		    swap_source_cancel_prepare(&set->backend, index) != 0)
			HAL_FATAL("boot swap prepare rollback failed");
	}
	if (manager_enabled && swap_shutdown(&set->backend) != 0)
		HAL_FATAL("boot swap manager rollback failed");

	/* A published source was destroyed by the shutdown; the rest are destroyed here. */
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		if ((published & (1U << index)) != 0)
			kern_swap_source_init(&set->range[index].source);
		else
			kern_swap_source_destroy(&set->range[index].source);
	}
	set->count = 0;
out:
	source_set_control_leave(set);

	/* Reports the activation result. */
	return error;
}

/*
 * Adds a prepared source to an active set at runtime.
 *
 * The source takes the first free id, which is reported through
 * source_id.  The commit limit is grown before the source is published
 * and shrunk again on failure.
 */
int
kern_swap_source_set_runtime_add(
	struct kern_swap_source_set *set,
	struct kern_swap_source *source,
	unsigned *source_id)
{
	uint32_t old_total;
	uint32_t new_total;
	unsigned id;
	unsigned index;
	int commit_grown;
	int error;
	const struct kern_swap_source *existing;
	unsigned long irq;

	commit_grown = 0;

	/* Rejects an incomplete source description. */
	if (set == NULL ||
	    source == NULL ||
	    source->ops == NULL ||
	    source->ops->read_page == NULL ||
	    source->ops->write_page == NULL ||
	    source->ops->destroy == NULL ||
	    source->data == NULL ||
	    source->slot_count == 0 ||
	    source->slot_count > SWAP_SOURCE_MAX_SLOTS ||
	    (source->identity_inode == NULL && source->identity_disk == NULL))
		return EINVAL;

	/* Only an active set with a free slot accepts sources. */
	error = source_set_control_enter(set);
	if (error != 0)
		return error;
	if (!set->active || set->count >= KERN_SWAP_SOURCE_COUNT) {
		if (!set->active)
			error = ENXIO;
		else
			error = ENOSPC;
		goto out;
	}
	for (id = 0; id < KERN_SWAP_SOURCE_COUNT; id++) {
		if (set->range[id].source.ops == NULL)
			break;
	}
	if (id == KERN_SWAP_SOURCE_COUNT) {
		error = ENOSPC;
		goto out;
	}

	/* The identity must be new. */
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		existing = &set->range[index].source;
		if (existing->ops != NULL && source_identity_equal(source, existing)) {
			error = EEXIST;
			goto out;
		}
	}

	/* The slot total must fit 32 bits. */
	error = source_set_current_total(set, &old_total);
	if (error != 0)
		goto out;
	if (old_total > UINT32_MAX - source->slot_count) {
		error = EOVERFLOW;
		goto out;
	}
	new_total = old_total + source->slot_count;

	/* Prepares the source and grows the commit limit. */
	error = swap_source_prepare(&set->backend, id, source->ops, source->data,
	    SWAP_PAGE_SIZE, source->slot_count);
	if (error != 0)
		goto out;
	error = vm_commit_resize_swap(old_total, new_total);
	if (error != 0)
		goto rollback_prepared;
	commit_grown = 1;

	/*
	 * Metadata is visible before ACTIVE publication.  A concurrent snapshot
	 * therefore sees either an internal PREPARED source (reported inactive)
	 * or a complete ACTIVE source, never ACTIVE with an empty identity.
	 */
	irq = spin_lock_irqsave(&swap_source_lock);
	set->range[id].source = *source;
	source_metadata_changed_locked(set);
	spin_unlock_irqrestore(&swap_source_lock, irq);
	error = swap_source_publish(&set->backend, id);
	if (error != 0)
		goto rollback_metadata;

	/* Records the assigned id and takes over the description. */
	source->parameter_index = id;
	irq = spin_lock_irqsave(&swap_source_lock);
	set->range[id].source.parameter_index = id;
	set->count++;
	source_metadata_changed_locked(set);
	spin_unlock_irqrestore(&swap_source_lock, irq);
	kern_swap_source_init(source);
	if (source_id != NULL)
		*source_id = id;
	error = 0;
	goto out;

rollback_metadata:
	/* Forgets the half-added source. */
	irq = spin_lock_irqsave(&swap_source_lock);
	kern_swap_source_init(&set->range[id].source);
	source_metadata_changed_locked(set);
	spin_unlock_irqrestore(&swap_source_lock, irq);
rollback_prepared:
	/* Gives back the commitment and the prepared backend slot. */
	if (commit_grown && vm_commit_resize_swap(new_total, old_total) != 0)
		HAL_FATAL("runtime swap commitment rollback failed");
	if (swap_source_cancel_prepare(&set->backend, id) != 0)
		HAL_FATAL("runtime swap prepare rollback failed");
out:
	source_set_control_leave(set);

	/* Reports the add result. */
	return error;
}

/*
 * Drains and removes a source from an active set.
 */
int
kern_swap_source_set_runtime_remove(
	struct kern_swap_source_set *set,
	unsigned source_id)
{
	int error;

	error = kern_swap_source_set_runtime_remove_cancelable(set, source_id,
	    NULL, NULL);

	/* Reports the removal result. */
	return error;
}

/*
 * Drains and removes a source, polling a cancel callback during the drain.
 *
 * The commit limit is shrunk before the drain so that no new pages can
 * be committed against the departing source, and restored on failure.
 */
int
kern_swap_source_set_runtime_remove_cancelable(
	struct kern_swap_source_set *set,
	unsigned source_id,
	kern_swap_source_cancel_fn cancel,
	void *cancel_argument)
{
	struct swap_source_stats stats;
	uint32_t old_total;
	uint32_t new_total;
	int error;
	unsigned long irq;

	/* Rejects a missing set or an id out of range. */
	if (set == NULL || source_id >= KERN_SWAP_SOURCE_COUNT)
		return EINVAL;

	/* The source must be present and active. */
	error = source_set_control_enter(set);
	if (error != 0)
		return error;
	if (!set->active || set->range[source_id].source.ops == NULL) {
		error = ENOENT;
		goto out;
	}
	error = source_set_current_total(set, &old_total);
	if (error != 0)
		goto out;
	error = swap_source_get_stats(&set->backend, source_id, &stats);
	if (error != 0 || stats.state != SWAP_SOURCE_STATE_ACTIVE) {
		if (error == 0)
			error = EBUSY;
		goto out;
	}

	/* Shrinks the commit limit and starts the drain. */
	new_total = old_total - stats.total_slots;
	error = vm_commit_resize_swap(old_total, new_total);
	if (error != 0)
		goto out;
	error = swap_source_begin_drain(&set->backend, source_id);
	if (error != 0) {
		if (vm_commit_resize_swap(new_total, old_total) != 0)
			HAL_FATAL("runtime swap commitment restore failed");
		goto out;
	}

	/* Drains the pages, cancelably when a callback and the routine exist. */
	if (cancel == NULL || vm_reclaim_drain_swap_source_cancelable == NULL)
		error = vm_reclaim_drain_swap_source(source_id);
	else
		error = vm_reclaim_drain_swap_source_cancelable(source_id, cancel,
		    cancel_argument);
	if (error == 0)
		error = swap_source_remove(&set->backend, source_id);
	if (error != 0) {
		if (vm_commit_resize_swap(new_total, old_total) != 0)
			HAL_FATAL("runtime swap commitment restore failed");
		if (swap_source_abort_drain(&set->backend, source_id) != 0)
			HAL_FATAL("runtime swap drain rollback failed");
		goto out;
	}

	/* Clears the description. */
	irq = spin_lock_irqsave(&swap_source_lock);
	kern_swap_source_init(&set->range[source_id].source);
	set->count--;
	source_metadata_changed_locked(set);
	spin_unlock_irqrestore(&swap_source_lock, irq);
out:
	source_set_control_leave(set);

	/* Reports the removal result. */
	return error;
}

/*
 * Finds the source with a disk and inode identity.
 */
int
kern_swap_source_set_find_identity(
	const struct kern_swap_source_set *set,
	struct disk *identity_disk,
	struct inode *identity_inode,
	unsigned *source_id)
{
	unsigned long irq;
	unsigned index;
	int error;
	const struct kern_swap_source *source;

	error = ENOENT;

	/* Rejects a missing set, disk, or result. */
	if (set == NULL || identity_disk == NULL || source_id == NULL)
		return EINVAL;

	/* Compares each present source's identity. */
	irq = spin_lock_irqsave(&swap_source_lock);
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		source = &set->range[index].source;
		if (source->ops != NULL && source_identity_matches(source,
		    identity_disk, identity_inode)) {
			*source_id = index;
			error = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&swap_source_lock, irq);

	/* Reports the lookup result. */
	return error;
}

/*
 * Takes a consistent snapshot of one source's metadata and statistics.
 *
 * The metadata generation is compared around the statistics call so
 * that a concurrent change is retried rather than mixed.
 */
int
kern_swap_source_set_snapshot(
	struct kern_swap_source_set *set,
	unsigned source_id,
	struct kern_swap_source_snapshot *snapshot)
{
	struct swap_source_stats stats;
	struct kern_swap_source metadata;
	uint64_t generation;
	unsigned attempt;
	int error;
	unsigned long irq;

	/* Rejects a missing set or result, or an id out of range. */
	if (set == NULL || source_id >= KERN_SWAP_SOURCE_COUNT || snapshot == NULL)
		return EINVAL;

	/* Retries until the metadata is unchanged across the statistics read. */
	for (attempt = 0; attempt < 8U; attempt++) {
		irq = spin_lock_irqsave(&swap_source_lock);
		generation = set->metadata_generation;
		metadata = set->range[source_id].source;
		spin_unlock_irqrestore(&swap_source_lock, irq);
		error = swap_source_get_stats(&set->backend, source_id, &stats);
		if (error != 0)
			return error;
		irq = spin_lock_irqsave(&swap_source_lock);
		if (generation == set->metadata_generation) {
			spin_unlock_irqrestore(&swap_source_lock, irq);
			break;
		}
		spin_unlock_irqrestore(&swap_source_lock, irq);
	}
	if (attempt == 8U)
		return EBUSY;

	/* An absent or merely prepared source reports inactive. */
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->source_id = source_id;
	if (stats.state == SWAP_SOURCE_STATE_INACTIVE ||
	    stats.state == SWAP_SOURCE_STATE_PREPARED) {
		snapshot->state = SWAP_SOURCE_STATE_INACTIVE;
		return 0;
	}
	if (metadata.ops == NULL)
		return EBUSY;

	/* Fills the snapshot from both. */
	if (stats.state == SWAP_SOURCE_STATE_ACTIVE)
		snapshot->state = SWAP_SOURCE_STATE_ACTIVE;
	else
		snapshot->state = SWAP_SOURCE_STATE_DRAINING;
	snapshot->header_version = metadata.header_version;
	snapshot->total_pages = stats.total_slots;
	snapshot->used_pages = stats.allocated_slots;
	memcpy(snapshot->uuid, metadata.uuid, sizeof(snapshot->uuid));
	memcpy(snapshot->label, metadata.label, sizeof(snapshot->label));
	memcpy(snapshot->diagnostic, metadata.diagnostic,
	    sizeof(snapshot->diagnostic));

	/* Reports the filled snapshot. */
	return 0;
}

/*
 * Tears a set down, shutting down an active backend or destroying
 * unactivated sources.
 */
int
kern_swap_source_set_abort(
	struct kern_swap_source_set *set)
{
	uint32_t total;
	unsigned index;
	int error;
	unsigned long irq;

	total = 0;

	/* Rejects a missing set. */
	if (set == NULL)
		return EINVAL;
	error = source_set_control_enter(set);
	if (error != 0)
		return error;

	/* An active set releases its commitment and shuts the backend down. */
	if (set->active) {
		error = source_set_current_total(set, &total);
		if (error != 0)
			goto out;
		error = vm_commit_resize_swap(total, 0);
		if (error != 0)
			goto out;
		error = swap_shutdown(&set->backend);
		if (error == EBUSY) {
			if (vm_commit_resize_swap(0, total) != 0)
				HAL_FATAL("swap abort commitment restore failed");
			goto out;
		}
		irq = spin_lock_irqsave(&swap_source_lock);
		for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++)
			kern_swap_source_init(&set->range[index].source);
		set->count = 0;
		set->active = 0;
		source_metadata_changed_locked(set);
		spin_unlock_irqrestore(&swap_source_lock, irq);
		goto out;
	}

	/* An inactive set destroys its prepared sources. */
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++)
		kern_swap_source_destroy(&set->range[index].source);
	set->count = 0;
	error = 0;
out:
	source_set_control_leave(set);

	/* Reports the abort result. */
	return error;
}

/*
 * Counts the extents of every active swap file.
 */
unsigned
kern_swap_source_file_extent_count(
	void)
{
	unsigned count;
	unsigned long irq;

	irq = spin_lock_irqsave(&swap_source_lock);
	count = active_extent_count;
	spin_unlock_irqrestore(&swap_source_lock, irq);

	/* Reports the sampled count. */
	return count;
}

/*
 * Reports the number of extents of the active file swap source.
 */
unsigned
swap_fat_extent_count(
	void)
{
	unsigned count;

	/* Asks the swap source layer. */
	count = kern_swap_source_file_extent_count();

	/* Reports the extent count. */
	return count;
}

/* Enables an empty backend, reporting whether this call did it. */
static int
swap_manager_enable_transition(
	struct swap_backend *backend,
	int *enabled_here)
{
	unsigned long irq;
	unsigned source_id;

	/* Rejects a missing backend. */
	if (enabled_here != NULL)
		*enabled_here = 0;
	if (backend == NULL)
		return EINVAL;

	/* An enabled backend stays enabled; a stopping one refuses. */
	irq = spin_lock_irqsave(&swap_lock);
	if (backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	if (backend->enabled) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return 0;
	}

	/* A disabled backend must be completely empty. */
	if (backend->slot_count != 0 ||
	    backend->free_slots != 0 ||
	    backend->source_count != 0 ||
	    backend->inflight != 0) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		if (backend->source[source_id].state !=
		    SWAP_SOURCE_STATE_INACTIVE) {
			spin_unlock_irqrestore(&swap_lock, irq);
			return EBUSY;
		}
	}

	/* Enables it. */
	backend->enabled = 1;
	if (enabled_here != NULL)
		*enabled_here = 1;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the enabled backend. */
	return 0;
}

/* Undoes a private enable of a backend that stayed empty. */
static void
swap_manager_disable_empty(
	struct swap_backend *backend)
{
	unsigned long irq;
	unsigned source_id;

	/*
	 * Leaves alone anything another control path may have published in
	 * the meantime: the system backend, any source, or in-flight I/O.
	 */
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled ||
	    backend->shutting_down ||
	    system_backend == backend ||
	    backend->slot_count != 0 ||
	    backend->free_slots != 0 ||
	    backend->source_count != 0 ||
	    backend->inflight != 0) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		if (backend->source[source_id].state !=
		    SWAP_SOURCE_STATE_INACTIVE) {
			spin_unlock_irqrestore(&swap_lock, irq);
			return;
		}
	}
	backend->enabled = 0;
	spin_unlock_irqrestore(&swap_lock, irq);
}

/* Reads or writes one slot through its source, counting the I/O in flight. */
static int
swap_io(
	struct swap_backend *backend,
	uint32_t slot,
	void *page,
	int write)
{
	struct swap_backend_source *source;
	const struct swap_backend_ops *ops;
	void *data;
	unsigned source_id;
	uint32_t local_slot;
	unsigned long irq;
	uint8_t mask;
	int error;

	/* Rejects a missing operand or a malformed slot. */
	if (backend == NULL || page == NULL)
		return EINVAL;
	if (swap_slot_decode(slot, &source_id, &local_slot) != 0)
		return EINVAL;

	/* The slot must be allocated in a live source and not being freed. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled ||
	    backend->shutting_down ||
	    (source->state != SWAP_SOURCE_STATE_ACTIVE &&
	     source->state != SWAP_SOURCE_STATE_DRAINING) ||
	    local_slot >= source->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	mask = (uint8_t)(1U << (local_slot & 7U));
	if (!(source->bitmap[local_slot >> 3] & mask) ||
	    source->slot_pending_free[local_slot]) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EINVAL;
	}

	/* Holds the slot and source in flight while the driver works. */
	backend->inflight++;
	source->inflight++;
	source->slot_inflight[local_slot]++;
	ops = source->ops;
	data = source->data;
	spin_unlock_irqrestore(&swap_lock, irq);
	if (write)
		error = ops->write_page(data, local_slot, page);
	else
		error = ops->read_page(data, local_slot, page);

	/* Releases the holds, completing a free deferred behind the I/O. */
	irq = spin_lock_irqsave(&swap_lock);
	source->slot_inflight[local_slot]--;
	if (source->slot_inflight[local_slot] == 0 &&
	    source->slot_pending_free[local_slot]) {
		source->slot_pending_free[local_slot] = 0;
		source->bitmap[local_slot >> 3] &= (uint8_t)~mask;
		source->free_slots++;
		backend->free_slots++;
	}
	source->inflight--;
	backend->inflight--;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the driver's result. */
	return error;
}

/* Tests whether a swap parameter is a boot<slot>:<path> reference. */
static int
is_boot_reference(
	const char *value)
{
	/* A reference starts with "boot" and carries a colon. */
	if (value == NULL)
		return 0;
	if (strncmp(value, "boot", 4U) != 0)
		return 0;
	if (strchr(value, ':') == NULL)
		return 0;

	/* Reports a boot reference. */
	return 1;
}

/* Checks that a selector is present, non-empty, and within the text limit. */
static int
selector_validate(
	const char *selector)
{
	size_t length;

	/* Rejects a missing selector. */
	if (selector == NULL)
		return EINVAL;

	/* Measures the selector, stopping just past the limit. */
	for (length = 0; length <= KERN_SWAP_SOURCE_TEXT_MAX; length++) {
		if (selector[length] == '\0')
			break;
	}

	/* Rejects an empty or overlong selector. */
	if (length == 0 || length > KERN_SWAP_SOURCE_TEXT_MAX)
		return EINVAL;

	/* Reports a usable selector. */
	return 0;
}

/* Tests whether a selector names a disk rather than a file path. */
static int
selector_is_disk(
	const char *selector)
{
	/* Device paths and identity selectors name disks. */
	if (strncmp(selector, "/dev/", 5U) == 0)
		return 1;
	if (strncmp(selector, "UUID=", 5U) == 0)
		return 1;
	if (strncmp(selector, "PARTUUID=", 9U) == 0)
		return 1;

	/* Anything else is a file path. */
	return 0;
}

/* Takes the single control operation and copies the registration. */
static int
control_enter(
	struct kern_swap_control_registration *registration)
{
	unsigned long irq;
	int error;

	error = 0;

	/* Refuses without a registration or while another operation runs. */
	irq = spin_lock_irqsave(&control_lock);
	if (!control_registered) {
		error = ENXIO;
	} else if (control_busy) {
		error = EBUSY;
	} else {
		control_busy = 1;
		*registration = control_registration;
	}
	spin_unlock_irqrestore(&control_lock, irq);

	/* Reports whether the operation was taken. */
	return error;
}

/* Releases the control operation. */
static void
control_leave(
	void)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&control_lock);
	control_busy = 0;
	spin_unlock_irqrestore(&control_lock, irq);
}

/* Copies the registration without taking the control operation. */
static int
control_snapshot_registration(
	struct kern_swap_control_registration *registration)
{
	unsigned long irq;
	int error;

	error = 0;

	/* Copies the registration when there is one. */
	irq = spin_lock_irqsave(&control_lock);
	if (!control_registered)
		error = ENXIO;
	else
		*registration = control_registration;
	spin_unlock_irqrestore(&control_lock, irq);

	/* Reports whether a registration was copied. */
	return error;
}

/* Resolves a selector to a disk, and for a file path also to its inode. */
static int
control_resolve_identity(
	const struct kern_swap_control_registration *control,
	const char *selector,
	struct path *path,
	struct disk **disk,
	struct inode **identity_inode)
{
	int error;

	path_init(path);
	*disk = NULL;
	*identity_inode = NULL;

	/* A disk selector resolves directly to a disk. */
	if (selector_is_disk(selector)) {
		error = control->resolver->resolve_disk(control->resolver_context,
		    selector, disk);
		return error;
	}

	/* A file path resolves to a mounted inode. */
	error = control->resolver->resolve_path(control->resolver_context,
	    selector, path);
	if (error != 0)
		return error;
	if (path->p_inode == NULL || path->p_mount == NULL) {
		path_release(path);
		path_init(path);
		return EINVAL;
	}

	/*
	 * Canonical file identity follows the inode's owning filesystem, not
	 * the namespace mount which may be a diskless bind wrapper.  A regular
	 * file whose owning filesystem has no disk still has a valid pathname
	 * lifetime; keep that path so prepare_file() can return
	 * backend-specific EOPNOTSUPP.  Successful preparations necessarily
	 * supply a disk-backed identity before publication.
	 */
	if (path->p_inode->i_mount != NULL)
		*disk = path->p_inode->i_mount->m_disk;
	else
		*disk = NULL;
	*identity_inode = path->p_inode;

	/* Reports the resolved file. */
	return 0;
}

/* Releases whatever control_resolve_identity() retained. */
static void
control_release_identity(
	struct path *path,
	struct disk *disk,
	struct inode *identity_inode)
{
	/* A file holds its path; a disk selector holds only the disk. */
	if (identity_inode != NULL)
		path_release(path);
	else if (disk != NULL)
		disk_release(disk);
}

/* Tests whether a prepared source is backed by the looked-up disk and inode. */
static int
control_source_identity_matches(
	const struct kern_swap_source *source,
	struct disk *disk,
	struct inode *identity_inode)
{
	/* Both sides must have a disk, and agree on whether there is a file. */
	if (source == NULL ||
	    disk == NULL ||
	    source->identity_disk == NULL ||
	    (source->identity_inode == NULL) != (identity_inode == NULL))
		return 0;

	/* The disks must be the same object or the same device. */
	if (source->identity_disk != disk &&
	    source->identity_disk->d_dev != disk->d_dev)
		return 0;

	/* A raw source matches; a file source must be the same inode. */
	if (identity_inode == NULL)
		return 1;
	if (source->identity_inode == identity_inode)
		return 1;
	if (source->identity_inode->i_ino == identity_inode->i_ino)
		return 1;

	/* Reports a different file. */
	return 0;
}

/* Resolves a selector again after the backing claim and checks the match. */
static int
control_revalidate_identity(
	const struct kern_swap_control_registration *control,
	const char *selector,
	const struct kern_swap_source *source)
{
	struct path path;
	struct disk *disk;
	struct inode *identity_inode;
	int error;

	/*
	 * Preparing a source establishes the backing claim which excludes
	 * subsequent unlink, rename, rebind, and disk teardown.  Resolve the
	 * spelling once more after that exclusion point: otherwise a mutation
	 * which completed between the first lookup and claim acquisition could
	 * publish an active source whose diagnostic selector no longer
	 * identifies it.
	 */
	error = control_resolve_identity(control, selector, &path, &disk,
	    &identity_inode);
	if (error != 0) {
		if (error == ENOMEM)
			return ENOMEM;
		return EAGAIN;
	}

	/* A selector that now names something else is a lost race. */
	if (control_source_identity_matches(source, disk, identity_inode))
		error = 0;
	else
		error = EAGAIN;
	control_release_identity(&path, disk, identity_inode);

	/* Reports whether the selector still names the source. */
	return error;
}

/* Reports EINTR to a cancelable removal when its thread has a signal. */
static int
control_cancelled(
	void *argument)
{
	struct thread *thread;

	thread = argument;

	/* Only a thread with a pending unblocked signal cancels. */
	if (thread == NULL)
		return 0;
	if (!signal_pending_unblocked(thread))
		return 0;

	/* Reports the cancellation. */
	return EINTR;
}

/* Decodes an unaligned little-endian word. */
static uint32_t
get32(
	const uint8_t *p)
{
	/* Combines bytes in disk order. */
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Decodes an unaligned little-endian halfword. */
static uint16_t
get16(
	const uint8_t *p)
{
	/* Combines the low and high bytes. */
	return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

/* Decodes an unaligned little-endian wide word. */
static uint64_t
get64(
	const uint8_t *p)
{
	uint32_t low;
	uint32_t high;

	/* Decodes both words before combining their values. */
	low = get32(p);
	high = get32(p + 4U);

	/* Returns the complete disk value. */
	return (uint64_t)low | ((uint64_t)high << 32);
}

/* Advances the metadata generation, skipping zero. */
static void
source_metadata_changed_locked(
	struct kern_swap_source_set *set)
{
	set->metadata_generation++;
	if (set->metadata_generation == 0)
		set->metadata_generation++;
}

/* Adds to the count of active swap file extents. */
static void
active_extents_add(
	unsigned count)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&swap_source_lock);
	active_extent_count += count;
	spin_unlock_irqrestore(&swap_source_lock, irq);
}

/* Subtracts from the count of active swap file extents, saturating at zero. */
static void
active_extents_remove(
	unsigned count)
{
	unsigned long irq;

	/* Discharges the extents, saturating at zero. */
	irq = spin_lock_irqsave(&swap_source_lock);
	if (active_extent_count >= count)
		active_extent_count -= count;
	else
		active_extent_count = 0U;
	spin_unlock_irqrestore(&swap_source_lock, irq);
}

/* Records one extent reported by the FAT extent walk. */
static int
collect_extent(
	uint64_t file_block,
	uint64_t disk_block,
	uint32_t count,
	void *argument)
{
	struct file_swap_data *data;
	struct fat_swap_extent *extent;

	data = argument;

	/* Rejects an empty extent or one past the table capacity. */
	if (count == 0)
		return EINVAL;
	if (data->extent_count >= FAT_SWAP_EXTENT_MAX) {
		data->extent_error = E2BIG;
		return E2BIG;
	}

	/* Appends the extent. */
	extent = &data->extents[data->extent_count++];
	extent->file_block = file_block;
	extent->disk_block = disk_block;
	extent->block_count = count;
	return 0;
}

/* Checks that the extents cover the file contiguously and lie on the disk. */
static int
validate_extents(
	struct file_swap_data *data,
	uint64_t bytes)
{
	uint64_t expected;
	uint64_t blocks;
	unsigned index;
	const struct fat_swap_extent *extent;

	expected = 0;
	blocks = bytes / 512U;

	/* The file must be whole sectors with at least one extent. */
	if (bytes % 512U != 0 || data->extent_count == 0)
		return EIO;

	/* Each extent must start where the previous ended and fit the disk. */
	for (index = 0; index < data->extent_count; index++) {
		extent = &data->extents[index];
		if (extent->file_block != expected ||
		    extent->block_count == 0 ||
		    extent->file_block > UINT64_MAX - extent->block_count ||
		    extent->disk_block > UINT64_MAX - extent->block_count ||
		    extent->disk_block + extent->block_count >
			data->disk->d_block_count)
			return EIO;
		expected += extent->block_count;
	}

	/* The extents must cover exactly the file. */
	if (expected != blocks)
		return EIO;
	return 0;
}

/* Writes swap blocks under the backing claim when the claimed writer is linked in. */
static int
swap_disk_write(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *page,
	const struct backing_claim *claim)
{
	int error;

	if (disk_write_direct_claimed != NULL)
		error = disk_write_direct_claimed(disk, block, count, page, claim);
	else
		error = disk_write_direct(disk, block, count, page);
	return error;
}

/* Transfers one swap page of a file source through its extents. */
static int
file_swap_io(
	struct file_swap_data *data,
	uint32_t slot,
	void *page,
	int write)
{
	uint64_t file_block;
	uint32_t remaining;
	uint8_t *bytes;
	struct fat_swap_extent *extent;
	uint32_t offset;
	uint32_t count;
	unsigned index;
	int error;

	/* Slot zero is the header, so the page lies one slot further in. */
	file_block = ((uint64_t)slot + 1U) * (SWAP_PAGE_SIZE / 512U);
	remaining = SWAP_PAGE_SIZE / 512U;
	bytes = page;

	/* Transfers each extent piece of the page in turn. */
	while (remaining != 0) {
		extent = NULL;
		for (index = 0; index < data->extent_count; index++) {
			if (file_block >= data->extents[index].file_block &&
			    file_block - data->extents[index].file_block <
				data->extents[index].block_count) {
				extent = &data->extents[index];
				break;
			}
		}
		if (extent == NULL)
			return EIO;
		offset = (uint32_t)(file_block - extent->file_block);
		count = extent->block_count - offset;
		if (count > remaining)
			count = remaining;
		if (data->disk->d_max_transfer_blocks != 0 &&
		    count > data->disk->d_max_transfer_blocks)
			count = data->disk->d_max_transfer_blocks;
		if (write)
			error = swap_disk_write(data->disk,
			    extent->disk_block + offset, count, bytes, data->claim);
		else
			error = disk_read_direct(data->disk,
			    extent->disk_block + offset, count, bytes);
		if (error != 0)
			return EIO;
		file_block += count;
		remaining -= count;
		bytes += count * 512U;
	}

	/* Reports the completed transfer. */
	return 0;
}

/* Reads one page of a file source. */
static int
file_swap_read(
	void *argument,
	uint32_t slot,
	void *page)
{
	int error;

	error = file_swap_io(argument, slot, page, 0);
	return error;
}

/* Writes one page of a file source. */
static int
file_swap_write(
	void *argument,
	uint32_t slot,
	const void *page)
{
	int error;

	error = file_swap_io(argument, slot, (void *)page, 1);
	return error;
}

/* Flushes the disk under a file source. */
static int
file_swap_flush(
	void *argument)
{
	struct file_swap_data *data;
	int error;

	data = argument;
	error = bio_flush(data->disk);
	return error;
}

/* Releases a file source's inode, disk, and claim. */
static void
file_swap_destroy(
	void *argument)
{
	struct file_swap_data *data;

	data = argument;

	/* The inode is a plain file again. */
	mutex_lock(&data->inode->i_lock);
	data->inode->i_flags &= ~INODE_SWAPFILE;
	mutex_unlock(&data->inode->i_lock);

	/* Drops everything the source held. */
	active_extents_remove(data->extent_count);
	inode_release(data->inode);
	disk_close(data->disk);
	backing_claim_release(data->claim);
	kern_free(data);
}

/* Transfers one swap page of a raw source. */
static int
raw_swap_io(
	struct raw_swap_data *data,
	uint32_t slot,
	void *page,
	int write)
{
	uint64_t block;
	int error;

	/* Slot zero is the header, so the page lies one slot further in. */
	block = ((uint64_t)slot + 1U) * data->blocks_per_page;
	if (write)
		error = swap_disk_write(data->disk, block,
		    data->blocks_per_page, page, data->claim);
	else
		error = disk_read_direct(data->disk, block, data->blocks_per_page,
		    page);
	if (error != 0)
		return EIO;
	return 0;
}

/* Reads one page of a raw source. */
static int
raw_swap_read(
	void *argument,
	uint32_t slot,
	void *page)
{
	int error;

	error = raw_swap_io(argument, slot, page, 0);
	return error;
}

/* Writes one page of a raw source. */
static int
raw_swap_write(
	void *argument,
	uint32_t slot,
	const void *page)
{
	int error;

	error = raw_swap_io(argument, slot, (void *)page, 1);
	return error;
}

/* Flushes the disk under a raw source. */
static int
raw_swap_flush(
	void *argument)
{
	struct raw_swap_data *data;
	int error;

	data = argument;
	error = bio_flush(data->disk);
	return error;
}

/* Releases a raw source's disk and claim. */
static void
raw_swap_destroy(
	void *argument)
{
	struct raw_swap_data *data;

	data = argument;
	disk_close(data->disk);
	backing_claim_release(data->claim);
	disk_release(data->disk);
	kern_free(data);
}

/* Tests whether two sources name the same file or the same disk. */
static int
source_identity_equal(
	const struct kern_swap_source *left,
	const struct kern_swap_source *right)
{
	/* Two files match by inode, or by device and inode number. */
	if (left->identity_inode != NULL && right->identity_inode != NULL) {
		if (left->identity_inode == right->identity_inode)
			return 1;
		if (left->identity_disk == NULL || right->identity_disk == NULL)
			return 0;
		if (left->identity_disk->d_dev != right->identity_disk->d_dev)
			return 0;
		if (left->identity_inode->i_ino != right->identity_inode->i_ino)
			return 0;
		return 1;
	}

	/* Two raw sources match by device; a file never matches a raw source. */
	if (left->identity_inode != NULL || right->identity_inode != NULL)
		return 0;
	if (left->identity_disk == NULL || right->identity_disk == NULL)
		return 0;
	if (left->identity_disk->d_dev != right->identity_disk->d_dev)
		return 0;
	return 1;
}

/* Tests whether a source has a given disk and inode identity. */
static int
source_identity_matches(
	const struct kern_swap_source *source,
	struct disk *identity_disk,
	struct inode *identity_inode)
{
	struct kern_swap_source identity;
	int equal;

	kern_swap_source_init(&identity);
	identity.identity_disk = identity_disk;
	identity.identity_inode = identity_inode;
	equal = source_identity_equal(source, &identity);
	return equal;
}

/* Resolves the first and last block of a disk on its leaf. */
static int
swap_disk_range_resolve(
	struct disk *disk,
	struct swap_disk_range *range)
{
	struct disk *first_leaf;
	struct disk *last_leaf;
	uint64_t first;
	uint64_t last;
	int error;

	/* Rejects a missing operand or an empty disk. */
	if (disk == NULL || range == NULL || disk->d_block_count == 0)
		return EINVAL;

	/* Both ends must map onto the same leaf in order. */
	error = disk_resolve_range(disk, 0, 1, &first_leaf, &first);
	if (error != 0)
		return error;
	error = disk_resolve_range(disk, disk->d_block_count - 1U, 1,
	    &last_leaf, &last);
	if (error != 0)
		return error;
	if (first_leaf != last_leaf || last < first)
		return EIO;
	range->leaf = first_leaf;
	range->first = first;
	range->last = last;
	return 0;
}

/* Takes the set's control flag, refusing a concurrent control operation. */
static int
source_set_control_enter(
	struct kern_swap_source_set *set)
{
	unsigned long irq;
	int error;

	/* Takes the control gate, refusing a second owner. */
	error = 0;
	irq = spin_lock_irqsave(&swap_source_lock);
	if (set->control_busy)
		error = EBUSY;
	else
		set->control_busy = 1;
	spin_unlock_irqrestore(&swap_source_lock, irq);

	/* Reports whether the gate was taken. */
	return error;
}

/* Releases the set's control flag. */
static void
source_set_control_leave(
	struct kern_swap_source_set *set)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&swap_source_lock);
	set->control_busy = 0;
	spin_unlock_irqrestore(&swap_source_lock, irq);
}

/* Reads the backend's current total slot count. */
static int
source_set_current_total(
	struct kern_swap_source_set *set,
	uint32_t *total)
{
	uint32_t free_slots;
	int error;

	error = swap_get_stats(&set->backend, total, &free_slots);
	return error;
}
