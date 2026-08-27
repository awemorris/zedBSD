/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/swap-source.h>

#include <kern/backing-claim.h>
#include <kern/buf.h>
#include <kern/disk.h>
#include <kern/fat-vfs.h>
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

static int source_set_control_enter(struct kern_swap_source_set *);
static void source_set_control_leave(struct kern_swap_source_set *);

static void
active_extents_add(unsigned count)
{
	unsigned long irq = spin_lock_irqsave(&swap_source_lock);

	active_extent_count += count;
	spin_unlock_irqrestore(&swap_source_lock, irq);
}

static void
active_extents_remove(unsigned count)
{
	unsigned long irq = spin_lock_irqsave(&swap_source_lock);

	active_extent_count = active_extent_count >= count ?
	    active_extent_count - count : 0U;
	spin_unlock_irqrestore(&swap_source_lock, irq);
}

static int
collect_extent(uint64_t file_block, uint64_t disk_block, uint32_t count,
	       void *argument)
{
	struct file_swap_data *data = argument;
	struct fat_swap_extent *extent;

	if (count == 0)
		return EINVAL;
	if (data->extent_count >= FAT_SWAP_EXTENT_MAX) {
		data->extent_error = E2BIG;
		return E2BIG;
	}
	extent = &data->extents[data->extent_count++];
	extent->file_block = file_block;
	extent->disk_block = disk_block;
	extent->block_count = count;
	return 0;
}

static int
validate_extents(struct file_swap_data *data, uint64_t bytes)
{
	uint64_t expected = 0;
	uint64_t blocks = bytes / 512U;
	unsigned index;

	if (bytes % 512U != 0 || data->extent_count == 0)
		return EIO;
	for (index = 0; index < data->extent_count; index++) {
		const struct fat_swap_extent *extent = &data->extents[index];

		if (extent->file_block != expected || extent->block_count == 0 ||
		    extent->file_block > UINT64_MAX - extent->block_count ||
		    extent->disk_block > UINT64_MAX - extent->block_count ||
		    extent->disk_block + extent->block_count >
			data->disk->d_block_count)
			return EIO;
		expected += extent->block_count;
	}
	return expected == blocks ? 0 : EIO;
}

static int
swap_disk_write(struct disk *disk, uint64_t block, uint32_t count,
	const void *page, const struct backing_claim *claim)
{
	return disk_write_direct_claimed != NULL ?
	    disk_write_direct_claimed(disk, block, count, page, claim) :
	    disk_write_direct(disk, block, count, page);
}

static int
file_swap_io(struct file_swap_data *data, uint32_t slot, void *page,
	     int write)
{
	uint64_t file_block = ((uint64_t)slot + 1U) *
	    (SWAP_PAGE_SIZE / 512U);
	uint32_t remaining = SWAP_PAGE_SIZE / 512U;
	uint8_t *bytes = page;

	while (remaining != 0) {
		struct fat_swap_extent *extent = NULL;
		uint32_t offset, count;
		unsigned index;

		for (index = 0; index < data->extent_count; index++)
			if (file_block >= data->extents[index].file_block &&
			    file_block - data->extents[index].file_block <
				data->extents[index].block_count) {
				extent = &data->extents[index];
				break;
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
		if ((write ? swap_disk_write(data->disk,
			    extent->disk_block + offset, count, bytes, data->claim) :
		     disk_read_direct(data->disk, extent->disk_block + offset,
			    count, bytes)) != 0)
			return EIO;
		file_block += count;
		remaining -= count;
		bytes += count * 512U;
	}
	return 0;
}

static int
file_swap_read(void *argument, uint32_t slot, void *page)
{
	return file_swap_io(argument, slot, page, 0);
}

static int
file_swap_write(void *argument, uint32_t slot, const void *page)
{
	return file_swap_io(argument, slot, (void *)page, 1);
}

static int
file_swap_flush(void *argument)
{
	return bio_flush(((struct file_swap_data *)argument)->disk);
}

static void
file_swap_destroy(void *argument)
{
	struct file_swap_data *data = argument;

	mutex_lock(&data->inode->i_lock);
	data->inode->i_flags &= ~INODE_SWAPFILE;
	mutex_unlock(&data->inode->i_lock);
	active_extents_remove(data->extent_count);
	inode_release(data->inode);
	disk_close(data->disk);
	backing_claim_release(data->claim);
	kern_free(data);
}

static const struct swap_backend_ops file_swap_ops = {
	.read_page = file_swap_read,
	.write_page = file_swap_write,
	.flush = file_swap_flush,
	.destroy = file_swap_destroy,
};

static int
raw_swap_io(struct raw_swap_data *data, uint32_t slot, void *page, int write)
{
	uint64_t block = ((uint64_t)slot + 1U) * data->blocks_per_page;

	return (write ? swap_disk_write(data->disk, block,
		    data->blocks_per_page, page, data->claim) :
		disk_read_direct(data->disk, block, data->blocks_per_page,
		    page)) == 0 ? 0 : EIO;
}

static int
raw_swap_read(void *argument, uint32_t slot, void *page)
{
	return raw_swap_io(argument, slot, page, 0);
}

static int
raw_swap_write(void *argument, uint32_t slot, const void *page)
{
	return raw_swap_io(argument, slot, (void *)page, 1);
}

static int
raw_swap_flush(void *argument)
{
	return bio_flush(((struct raw_swap_data *)argument)->disk);
}

static void
raw_swap_destroy(void *argument)
{
	struct raw_swap_data *data = argument;

	disk_close(data->disk);
	backing_claim_release(data->claim);
	disk_release(data->disk);
	kern_free(data);
}

static const struct swap_backend_ops raw_swap_ops = {
	.read_page = raw_swap_read,
	.write_page = raw_swap_write,
	.flush = raw_swap_flush,
	.destroy = raw_swap_destroy,
};

void
kern_swap_source_init(struct kern_swap_source *source)
{
	if (source != NULL)
		memset(source, 0, sizeof(*source));
}

int
kern_swap_source_prepare_file(const struct path *path,
			      unsigned parameter_index,
			      struct kern_swap_source *source)
{
	struct file_swap_data *data = NULL;
	struct swap_header_info header_info;
	struct file *file = NULL;
	uint8_t header[ZEDBSD_SWAP_HEADER_SIZE];
	uint64_t bytes;
	struct backing_claim_extent *claim_extents = NULL;
	ssize_t header_bytes;
	int error;

	if (path == NULL || path->p_inode == NULL || source == NULL ||
	    parameter_index >= KERN_SWAP_SOURCE_COUNT)
		return EINVAL;
	kern_swap_source_init(source);
	error = file_open_resolved(path, O_RDWR, &file);
	if (error != 0)
		return error;
	if (file->f_inode == NULL || file->f_inode->i_type != INODE_REG ||
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
	data = kern_calloc(1, sizeof(*data));
	if (data == NULL) {
		error = ENOMEM;
		goto out;
	}
	data->disk = file->f_inode->i_mount->m_disk;
	data->inode = file->f_inode;
	inode_ref(data->inode);
	error = mount_sync != NULL ? mount_sync(file->f_inode->i_mount) : 0;
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
	bytes = (uint64_t)file->f_inode->i_size;
	header_bytes = file_pread(file, header, sizeof(header), 0);
	if (header_bytes != (ssize_t)sizeof(header)) {
		error = header_bytes < 0 ? (int)-header_bytes : EIO;
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
	error = fat_file_extents(file, collect_extent, data);
	if (error != 0) {
		error = data->extent_error != 0 ? data->extent_error : EIO;
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
	{
		unsigned index;
		for (index = 0; index < data->extent_count; index++) {
			claim_extents[index].disk = data->disk;
			claim_extents[index].block = data->extents[index].disk_block;
			claim_extents[index].block_count =
			    data->extents[index].block_count;
		}
	}
	error = backing_claim_finalize(data->claim, claim_extents,
	    data->extent_count);
	kern_free(claim_extents);
	claim_extents = NULL;
	if (error != 0)
		goto out;
	{
		unsigned index;
		for (index = 0; index < data->extent_count; index++) {
			error = buf_invalidate != NULL ? buf_invalidate(data->disk,
			    data->extents[index].disk_block,
			    data->extents[index].block_count,
			    BUF_INVALIDATE_DISCARD) : 0;
			if (error != 0)
				goto out;
		}
	}
	mutex_lock(&data->inode->i_lock);
	if ((data->inode->i_flags & (INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
		mutex_unlock(&data->inode->i_lock);
		error = EBUSY;
		goto out;
	}
	data->inode->i_flags |= INODE_SWAPFILE;
	mutex_unlock(&data->inode->i_lock);
	source->ops = &file_swap_ops;
	source->data = data;
	source->identity_disk = data->disk;
	source->identity_inode = data->inode;
	source->slot_count = (uint32_t)header_info.slot_count;
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
	return error;
}

int
kern_swap_source_prepare_raw(struct disk *disk, unsigned parameter_index,
			     struct kern_swap_source *source)
{
	struct raw_swap_data *data = NULL;
	struct swap_header_info header_info;
	uint8_t *header_page = NULL;
	uint64_t bytes;
	uint32_t blocks_per_page;
	int error;

	if (disk == NULL || source == NULL ||
	    parameter_index >= KERN_SWAP_SOURCE_COUNT ||
	    (disk->d_flags & DISK_PARTITION) == 0 ||
	    (disk->d_flags & DISK_READ_ONLY) != 0 || disk->d_block_size == 0 ||
	    SWAP_PAGE_SIZE % disk->d_block_size != 0 ||
	    disk->d_block_count > UINT64_MAX / disk->d_block_size)
		return EINVAL;
	kern_swap_source_init(source);
	blocks_per_page = SWAP_PAGE_SIZE / disk->d_block_size;
	bytes = disk->d_block_count * disk->d_block_size;
	error = disk_open(disk);
	if (error != 0)
		return error;
	error = mount_disk_writable_busy != NULL ?
	    mount_disk_writable_busy(disk) : 0;
	if (error != 0) {
		disk_close(disk);
		return error;
	}
	{
		struct backing_claim *claim = NULL;
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
	}
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
	disk_ref(disk);
	data->disk = disk;
	data->blocks_per_page = blocks_per_page;
	source->ops = &raw_swap_ops;
	source->data = data;
	source->identity_disk = disk;
	source->slot_count = (uint32_t)header_info.slot_count;
	source->parameter_index = parameter_index;
	return 0;
}

void
kern_swap_source_destroy(struct kern_swap_source *source)
{
	if (source == NULL)
		return;
	if (source->ops != NULL && source->ops->destroy != NULL &&
	    source->data != NULL)
		source->ops->destroy(source->data);
	kern_swap_source_init(source);
}

void
kern_swap_source_set_init(struct kern_swap_source_set *set)
{
	if (set != NULL) {
		memset(set, 0, sizeof(*set));
		swap_init(&set->backend);
	}
}

static int
source_identity_equal(const struct kern_swap_source *left,
		      const struct kern_swap_source *right)
{
	if (left->identity_inode != NULL && right->identity_inode != NULL) {
		if (left->identity_inode == right->identity_inode)
			return 1;
		return left->identity_disk != NULL && right->identity_disk != NULL &&
		    left->identity_disk->d_dev == right->identity_disk->d_dev &&
		    left->identity_inode->i_ino == right->identity_inode->i_ino;
	}
	return left->identity_inode == NULL && right->identity_inode == NULL &&
	    left->identity_disk != NULL && right->identity_disk != NULL &&
	    left->identity_disk->d_dev == right->identity_disk->d_dev;
}

int
kern_swap_source_set_add(struct kern_swap_source_set *set,
			 struct kern_swap_source *source)
{
	uint64_t total = 0;
	unsigned source_id;
	unsigned index;
	int error;

	if (set == NULL || source == NULL || source->ops == NULL ||
	    source->ops->read_page == NULL ||
	    source->ops->write_page == NULL || source->ops->destroy == NULL ||
	    source->data == NULL || source->slot_count == 0 ||
	    source->slot_count > SWAP_SOURCE_MAX_SLOTS ||
	    source->parameter_index >= KERN_SWAP_SOURCE_COUNT ||
	    (source->identity_inode == NULL && source->identity_disk == NULL))
		return EINVAL;
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
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		const struct kern_swap_source *existing =
		    &set->range[index].source;

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
	set->range[source_id].source = *source;
	set->count++;
	kern_swap_source_init(source);
	error = 0;
out:
	source_set_control_leave(set);
	return error;
}

struct swap_disk_range {
	struct disk *leaf;
	uint64_t first;
	uint64_t last;
};

static int
swap_disk_range_resolve(struct disk *disk, struct swap_disk_range *range)
{
	struct disk *first_leaf, *last_leaf;
	uint64_t first, last;
	int error;

	if (disk == NULL || range == NULL || disk->d_block_count == 0)
		return EINVAL;
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

int
kern_swap_source_set_validate_native_root(
	const struct kern_swap_source_set *set, struct disk *root_disk)
{
	struct swap_disk_range root_range;
	unsigned index;
	int error;

	if (set == NULL || root_disk == NULL)
		return EINVAL;
	error = swap_disk_range_resolve(root_disk, &root_range);
	if (error != 0)
		return error;
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		const struct kern_swap_source *source =
		    &set->range[index].source;
		struct swap_disk_range swap_range;

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
	return 0;
}

int
kern_swap_source_set_map(const struct kern_swap_source_set *set,
			 uint32_t global_slot, unsigned *source_index,
			 uint32_t *local_slot)
{
	unsigned source_id;
	uint32_t local;

	if (set == NULL ||
	    swap_slot_decode(global_slot, &source_id, &local) != 0)
		return EINVAL;
	if (set->range[source_id].source.ops == NULL ||
	    local >= set->range[source_id].source.slot_count)
		return ERANGE;
	if (source_index != NULL)
		*source_index = source_id;
	if (local_slot != NULL)
		*local_slot = local;
	return 0;
}

int
kern_swap_source_set_activate(struct kern_swap_source_set *set)
{
	uint64_t total = 0;
	unsigned prepared = 0;
	unsigned published = 0;
	unsigned index;
	int manager_enabled = 0;
	int commit_grown = 0;
	int error;

	if (set == NULL)
		return EINVAL;
	error = source_set_control_enter(set);
	if (error != 0)
		return error;
	if (set->active) {
		error = EINVAL;
		goto out;
	}
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		struct kern_swap_source *source = &set->range[index].source;

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
	/* Publish the empty manager first.  This reserves the singleton backend
	 * even when boot supplied no swap source. */
	error = swap_set_system_backend(&set->backend);
	if (error != 0)
		goto fail;
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		struct kern_swap_source *source = &set->range[index].source;

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
	if (commit_grown && vm_commit_resize_swap(total, 0) != 0)
		HAL_FATAL("boot swap commitment rollback failed");
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++)
		if ((prepared & (1U << index)) != 0 &&
		    swap_source_cancel_prepare(&set->backend, index) != 0)
			HAL_FATAL("boot swap prepare rollback failed");
	if (manager_enabled && swap_shutdown(&set->backend) != 0)
		HAL_FATAL("boot swap manager rollback failed");
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		if ((published & (1U << index)) != 0)
			kern_swap_source_init(&set->range[index].source);
		else
			kern_swap_source_destroy(&set->range[index].source);
	}
	set->count = 0;
out:
	source_set_control_leave(set);
	return error;
}

static int
source_set_control_enter(struct kern_swap_source_set *set)
{
	unsigned long irq;
	int error = 0;

	irq = spin_lock_irqsave(&swap_source_lock);
	if (set->control_busy)
		error = EBUSY;
	else
		set->control_busy = 1;
	spin_unlock_irqrestore(&swap_source_lock, irq);
	return error;
}

static void
source_set_control_leave(struct kern_swap_source_set *set)
{
	unsigned long irq = spin_lock_irqsave(&swap_source_lock);

	set->control_busy = 0;
	spin_unlock_irqrestore(&swap_source_lock, irq);
}

static int
source_set_current_total(struct kern_swap_source_set *set, uint32_t *total)
{
	uint32_t free_slots;

	return swap_get_stats(&set->backend, total, &free_slots);
}

int
kern_swap_source_set_runtime_add(struct kern_swap_source_set *set,
	struct kern_swap_source *source, unsigned *source_id)
{
	uint32_t old_total, new_total;
	unsigned id, index;
	int commit_grown = 0;
	int error;

	if (set == NULL || source == NULL || source->ops == NULL ||
	    source->ops->read_page == NULL || source->ops->write_page == NULL ||
	    source->ops->destroy == NULL || source->data == NULL ||
	    source->slot_count == 0 ||
	    source->slot_count > SWAP_SOURCE_MAX_SLOTS ||
	    (source->identity_inode == NULL && source->identity_disk == NULL))
		return EINVAL;
	error = source_set_control_enter(set);
	if (error != 0)
		return error;
	if (!set->active || set->count >= KERN_SWAP_SOURCE_COUNT) {
		error = !set->active ? ENXIO : ENOSPC;
		goto out;
	}
	for (id = 0; id < KERN_SWAP_SOURCE_COUNT; id++)
		if (set->range[id].source.ops == NULL)
			break;
	if (id == KERN_SWAP_SOURCE_COUNT) {
		error = ENOSPC;
		goto out;
	}
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++) {
		const struct kern_swap_source *existing =
		    &set->range[index].source;

		if (existing->ops != NULL && source_identity_equal(source, existing)) {
			error = EEXIST;
			goto out;
		}
	}
	error = source_set_current_total(set, &old_total);
	if (error != 0)
		goto out;
	if (old_total > UINT32_MAX - source->slot_count) {
		error = EOVERFLOW;
		goto out;
	}
	new_total = old_total + source->slot_count;
	error = swap_source_prepare(&set->backend, id, source->ops, source->data,
	    SWAP_PAGE_SIZE, source->slot_count);
	if (error != 0)
		goto out;
	error = vm_commit_resize_swap(old_total, new_total);
	if (error != 0)
		goto rollback_prepared;
	commit_grown = 1;
	error = swap_source_publish(&set->backend, id);
	if (error != 0)
		goto rollback_prepared;
	source->parameter_index = id;
	set->range[id].source = *source;
	set->count++;
	kern_swap_source_init(source);
	if (source_id != NULL)
		*source_id = id;
	error = 0;
	goto out;

rollback_prepared:
	if (commit_grown && vm_commit_resize_swap(new_total, old_total) != 0)
		HAL_FATAL("runtime swap commitment rollback failed");
	if (swap_source_cancel_prepare(&set->backend, id) != 0)
		HAL_FATAL("runtime swap prepare rollback failed");
out:
	source_set_control_leave(set);
	return error;
}

int
kern_swap_source_set_runtime_remove(struct kern_swap_source_set *set,
	unsigned source_id)
{
	struct swap_source_stats stats;
	uint32_t old_total, new_total;
	int error;

	if (set == NULL || source_id >= KERN_SWAP_SOURCE_COUNT)
		return EINVAL;
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
		error = error != 0 ? error : EBUSY;
		goto out;
	}
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
	error = vm_reclaim_drain_swap_source(source_id);
	if (error == 0)
		error = swap_source_remove(&set->backend, source_id);
	if (error != 0) {
		if (vm_commit_resize_swap(new_total, old_total) != 0)
			HAL_FATAL("runtime swap commitment restore failed");
		if (swap_source_abort_drain(&set->backend, source_id) != 0)
			HAL_FATAL("runtime swap drain rollback failed");
		goto out;
	}
	kern_swap_source_init(&set->range[source_id].source);
	set->count--;
out:
	source_set_control_leave(set);
	return error;
}

int
kern_swap_source_set_abort(struct kern_swap_source_set *set)
{
	uint32_t total = 0;
	unsigned index;
	int error;

	if (set == NULL)
		return EINVAL;
	error = source_set_control_enter(set);
	if (error != 0)
		return error;
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
		for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++)
			kern_swap_source_init(&set->range[index].source);
		set->count = 0;
		set->active = 0;
		goto out;
	}
	for (index = 0; index < KERN_SWAP_SOURCE_COUNT; index++)
		kern_swap_source_destroy(&set->range[index].source);
	set->count = 0;
	error = 0;
out:
	source_set_control_leave(set);
	return error;
}

unsigned
kern_swap_source_file_extent_count(void)
{
	unsigned count;
	unsigned long irq = spin_lock_irqsave(&swap_source_lock);

	count = active_extent_count;
	spin_unlock_irqrestore(&swap_source_lock, irq);
	return count;
}
