/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Swap sources and the swap source set.
 *
 * A source is a swap file on a FAT mount, addressed through its
 * collected extents, or a raw partition.  Both are guarded by a backing
 * claim so that no other writer can reach their blocks.  The set holds
 * up to four sources, activates them as one transaction at boot, and
 * adds or drains them at runtime under a control flag with the commit
 * accounting adjusted around every change.
 */

#include <kern/swap-source.h>

#include <kern/backing-claim.h>
#include <kern/buf.h>
#include <kern/disk.h>
#include <kern/fat.h>
#include <kern/file.h>
#include <kern/inode.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <kern/mount.h>
#include <kern/vm-commit.h>
#include <kern/vm-reclaim.h>

#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <limits.h>
#include <sys/stat.h>
#include <string.h>

#define FAT_SWAP_EXTENT_MAX 1024U

extern int disk_write_direct_claimed(struct disk *, uint64_t, uint32_t,
    const void *, const struct backing_claim *) __attribute__((weak));
extern int mount_sync(struct mount *) __attribute__((weak));
extern int mount_disk_writable_busy(struct disk *) __attribute__((weak));
extern int buf_invalidate(struct disk *, uint64_t, uint64_t, unsigned)
    __attribute__((weak));
extern int vm_reclaim_drain_swap_source_cancelable(unsigned,
    int (*)(void *), void *) __attribute__((weak));

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

static unsigned active_extent_count;
static struct spinlock swap_source_lock = {
	{ 0 }, LOCK_RANK_SWAP, "swap sources", 0, 0
};

struct swap_disk_range {
	struct disk *leaf;
	uint64_t first;
	uint64_t last;
};

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
	    file->f_inode->i_mount->m_type != &fat_filesystem_type) {
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
	error = fat_file_extents(file, collect_extent, data);
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
	irq = spin_lock_irqsave(&swap_source_lock);
	kern_swap_source_init(&set->range[id].source);
	source_metadata_changed_locked(set);
	spin_unlock_irqrestore(&swap_source_lock, irq);
rollback_prepared:
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

	error = 0;
	irq = spin_lock_irqsave(&swap_source_lock);
	if (set->control_busy)
		error = EBUSY;
	else
		set->control_busy = 1;
	spin_unlock_irqrestore(&swap_source_lock, irq);
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
