/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/swap-fat.h"
#include "kern/swap.h"
#include "kern/fat-vfs.h"
#include "kern/file.h"
#include "kern/namei.h"
#include "kern/kmem.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#define FAT_SWAP_EXTENT_MAX 1024U
#define BLOCKS_PER_PAGE (SWAP_PAGE_SIZE / 512U)

struct fat_swap_extent {
	uint64_t file_block;
	uint64_t disk_block;
	uint32_t block_count;
};

struct fat_swap_data {
	struct disk *disk;
	struct inode *inode;
	struct fat_swap_extent extents[FAT_SWAP_EXTENT_MAX];
	unsigned extent_count;
};

static struct swap_backend fat_swap_backend;
static unsigned active_extent_count;

unsigned swap_fat_extent_count(void)
{
	return active_extent_count;
}

static int collect_extent(uint64_t file_block, uint64_t disk_block,
			  uint32_t count, void *argument)
{
	struct fat_swap_data *data = argument;
	struct fat_swap_extent *extent;
	if (data->extent_count >= FAT_SWAP_EXTENT_MAX)
		return E2BIG;
	extent = &data->extents[data->extent_count++];
	extent->file_block = file_block;
	extent->disk_block = disk_block;
	extent->block_count = count;
	return 0;
}

static int fat_swap_io(struct fat_swap_data *data, uint32_t slot, void *page,
		       int write)
{
	uint64_t file_block = ((uint64_t)slot + 1U) * BLOCKS_PER_PAGE;
	uint32_t remaining = BLOCKS_PER_PAGE;
	uint8_t *bytes = page;

	while (remaining != 0) {
		struct fat_swap_extent *extent = NULL;
		uint32_t offset, count;
		unsigned i;
		for (i = 0; i < data->extent_count; i++)
			if (file_block >= data->extents[i].file_block &&
			    file_block - data->extents[i].file_block <
			    data->extents[i].block_count) {
				extent = &data->extents[i];
				break;
			}
		if (extent == NULL)
			return EIO;
		offset = (uint32_t)(file_block - extent->file_block);
		count = extent->block_count - offset;
		if (count > remaining) count = remaining;
		if (data->disk->d_max_transfer_blocks != 0 &&
		    count > data->disk->d_max_transfer_blocks)
			count = data->disk->d_max_transfer_blocks;
		if ((write ? disk_write(data->disk, extent->disk_block + offset,
					 count, bytes) :
		     disk_read(data->disk, extent->disk_block + offset,
			       count, bytes)) != 0)
			return EIO;
		file_block += count;
		remaining -= count;
		bytes += count * 512U;
	}
	return 0;
}

static int fat_swap_read(void *argument, uint32_t slot, void *page)
{
	return fat_swap_io(argument, slot, page, 0);
}

static int fat_swap_write(void *argument, uint32_t slot, const void *page)
{
	return fat_swap_io(argument, slot, (void *)page, 1);
}

static int fat_swap_flush(void *argument)
{
	return bio_flush(((struct fat_swap_data *)argument)->disk);
}

static void fat_swap_destroy(void *argument)
{
	struct fat_swap_data *data = argument;
	data->inode->i_flags &= ~INODE_SWAPFILE;
	active_extent_count = 0;
	inode_release(data->inode);
	disk_close(data->disk);
	kern_free(data);
}

int swap_fat_activate(struct cwdinfo *cwd, const char *mount_path)
{
	static const struct swap_backend_ops ops = {
		.read_page = fat_swap_read, .write_page = fat_swap_write,
		.flush = fat_swap_flush, .destroy = fat_swap_destroy,
	};
	struct fat_swap_data *data = NULL;
	struct file *file = NULL;
	uint8_t header[ZEDBSD_SWAP_HEADER_SIZE];
	char path[ZEDBSD_PATH_MAX];
	size_t length;
	int error;

	if (cwd == NULL || mount_path == NULL)
		return EINVAL;
	length = strlen(mount_path);
	if (length + sizeof("/swapfile") > sizeof(path))
		return ENAMETOOLONG;
	memcpy(path, mount_path, length);
	memcpy(path + length, "/swapfile", sizeof("/swapfile"));
	error = file_openat(cwd, path, O_RDWR, 0, &file);
	if (error != 0)
		return error;
	if (file->f_inode == NULL || file->f_inode->i_type != INODE_REG ||
	    file->f_inode->i_size != (off_t)ZEDBSD_SWAP_FILE_BYTES ||
	    (file->f_inode->i_mount->m_flags & MOUNT_READ_ONLY) ||
	    (file->f_inode->i_mount->m_disk->d_flags & DISK_READ_ONLY)) {
		error = EINVAL;
		goto out;
	}
	if (file_pread(file, header, sizeof(header), 0) != (ssize_t)sizeof(header) ||
	    swap_header_validate(header, (uint32_t)file->f_inode->i_size) != 0) {
		error = EINVAL;
		goto out;
	}
	data = kern_calloc(1, sizeof(*data));
	if (data == NULL) {
		error = ENOMEM;
		goto out;
	}
	data->disk = file->f_inode->i_mount->m_disk;
	if (fat_file_extents(file, collect_extent, data) != 0 ||
	    data->extent_count == 0) {
		error = EIO;
		goto out;
	}
	error = disk_open(data->disk);
	if (error != 0)
		goto out;
	data->inode = file->f_inode;
	inode_ref(data->inode);
	data->inode->i_flags |= INODE_SWAPFILE;
	swap_init(&fat_swap_backend);
	error = swap_activate(&fat_swap_backend, &ops, data, SWAP_PAGE_SIZE,
			      ZEDBSD_SWAP_DATA_SLOTS);
	if (error != 0) {
		data->inode->i_flags &= ~INODE_SWAPFILE;
		inode_release(data->inode);
		disk_close(data->disk);
		goto out;
	}
	swap_set_system_backend(&fat_swap_backend);
	active_extent_count = data->extent_count;
	data = NULL;
out:
	if (file != NULL)
		(void)file_close(file);
	if (data != NULL)
		kern_free(data);
	return error;
}
