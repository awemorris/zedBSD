/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot loaders' read-only UFS reader.
 *
 * It finds a file by its absolute path on a native (little-endian) UFS
 * volume and reads its bytes, for a loader that takes the kernel and the
 * logo from the root partition.  It is freestanding: the caller supplies
 * the block reads and the work storage, and nothing is allocated.
 */

#ifndef KERN_BOOTLOADER_COMMON_UFS_READ_H
#define KERN_BOOTLOADER_COMMON_UFS_READ_H

#include <stddef.h>
#include <stdint.h>

/* The largest file system block the reader takes, as the kernel does. */
#define ZBL_UFS_BLOCK_MAX 65536U

/* The size of one on-disk inode. */
#define ZBL_UFS_INODE_SIZE 256U

/* The longest directory a lookup reads through, against a looping volume. */
#define ZBL_UFS_DIRECTORY_MAX (UINT64_C(16) * 1024U * 1024U)

/* The outcome of a reader call; every value but OK stops the read. */
enum zbl_ufs_result {
	ZBL_UFS_OK = 0,
	ZBL_UFS_INVALID_ARGUMENT,
	ZBL_UFS_IO,
	ZBL_UFS_NOT_UFS,
	ZBL_UFS_SWAPPED,
	ZBL_UFS_BAD_SUPERBLOCK,
	ZBL_UFS_FRAGMENT_TOO_SMALL,
	ZBL_UFS_LARGER_THAN_PARTITION,
	ZBL_UFS_BAD_INODE,
	ZBL_UFS_BAD_BLOCK,
	ZBL_UFS_BAD_DIRECTORY,
	ZBL_UFS_BAD_PATH,
	ZBL_UFS_NOT_FOUND,
	ZBL_UFS_NOT_DIRECTORY,
	ZBL_UFS_NOT_REGULAR,
	ZBL_UFS_SYMLINK,
	ZBL_UFS_PAST_END
};

/*
 * The partition the reader reads, in the medium's own blocks.
 *
 * read() fills count blocks from block on; it returns 0 on success.  The
 * block size is a power of two from 512 to 4096.
 */
struct zbl_ufs_device {
	void *context;
	uint32_t block_size;
	uint64_t block_count;
	int (*read)(void *context, uint64_t block, uint32_t count, void *buffer);
};

/*
 * The storage one mounted volume reads through.
 *
 * data holds one block at a time; indirect keeps the last indirect block
 * read at each level (number 0 means none), so reading a file in order
 * reads each indirect block once.
 */
struct zbl_ufs_work {
	uint8_t data[ZBL_UFS_BLOCK_MAX];
	uint8_t indirect[3][ZBL_UFS_BLOCK_MAX];
	uint64_t indirect_number[3];
};

/*
 * A UFS volume the reader has checked.
 *
 * It lives as long as the caller reads files of it; the device and the
 * work storage must outlive it.
 */
struct zbl_ufs_volume {
	struct zbl_ufs_device device;
	struct zbl_ufs_work *work;
	uint64_t fragments;
	uint32_t block_size;
	uint32_t fragment_size;
	uint32_t fragments_per_block;
	uint32_t groups;
	uint32_t inodes_per_group;
	uint32_t fragments_per_group;
	uint32_t inode_block;
	uint32_t inodes_per_block;
	uint32_t pointers_per_block;
};

/*
 * One file the reader found.
 *
 * It keeps the file's inode, from which every read finds the blocks.
 */
struct zbl_ufs_file {
	uint32_t number;
	uint16_t mode;
	uint64_t size;
	uint8_t inode[ZBL_UFS_INODE_SIZE];
};

enum zbl_ufs_result zbl_ufs_mount(struct zbl_ufs_volume *volume, const struct zbl_ufs_device *device, struct zbl_ufs_work *work);
enum zbl_ufs_result zbl_ufs_lookup(struct zbl_ufs_volume *volume, const char *path, struct zbl_ufs_file *file);
enum zbl_ufs_result zbl_ufs_read(struct zbl_ufs_volume *volume, const struct zbl_ufs_file *file, uint64_t offset, void *buffer, size_t length);
const char *zbl_ufs_result_name(enum zbl_ufs_result result);

#endif
