/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The volume fsck works on (BUG-312): the device or image, its superblock
 * (decoded by mkfs's ufs_super_decode), its cylinder groups held whole,
 * and its inodes one inode block at a time.
 */

#include "fsck.h"

#include "userland/base/mkfs/ufs-endian.h"
#include "userland/base/mkfs/ufs-super.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fsck_pread(struct fsck_volume *volume, uint64_t offset, void *buffer, size_t length);
static int fsck_pwrite(struct fsck_volume *volume, uint64_t offset, const void *buffer, size_t length);
static int fsck_inode_flush(struct fsck_volume *volume);
static int fsck_groups_load(struct fsck_volume *volume);

/*
 * Opens a device or image, reads and decodes its superblock and reads
 * every cylinder group.  Returns 0, EOPNOTSUPP for a volume that is not
 * UFS, EINVAL for a superblock or group that does not hold together, or
 * the I/O's error.
 */
int
fsck_volume_open(
	struct fsck_volume *volume,
	const char *name,
	int repair)
{
	struct stat status;
	uint64_t sectors;
	uint32_t depth;
	int flags;
	int error;

	/* Nothing is open yet. */
	memset(volume, 0, sizeof(*volume));
	volume->fd = -1;
	volume->name = name;
	volume->repair = repair;

	/* The device, written only when repairing. */
	flags = O_RDONLY;
	if (repair)
		flags = O_RDWR;
	volume->fd = open(name, flags);
	if (volume->fd < 0)
		return errno;

	/* The medium's size, which the superblock must fit in (a device's is not known here). */
	error = fstat(volume->fd, &status);
	if (error != 0)
		return errno;
	sectors = UINT64_MAX / UFS_SECTOR_SIZE;
	if ((status.st_mode & S_IFMT) == S_IFREG)
		sectors = (uint64_t)status.st_size / UFS_SECTOR_SIZE;

	/* The superblock. */
	error = fsck_pread(volume, UFS_SBLOCK_OFFSET, volume->super_raw, sizeof(volume->super_raw));
	if (error != 0)
		return error;
	error = ufs_super_decode(volume->super_raw, sizeof(volume->super_raw), sectors, &volume->super);
	if (error != 0)
		return error;
	volume->inode_count = volume->super.ncg * volume->super.ipg;

	/* The buffers: a data block, an indirect block per depth, the inode block. */
	volume->data = malloc(volume->super.bsize);
	volume->inode_block = malloc(volume->super.bsize);
	if (volume->data == NULL || volume->inode_block == NULL)
		return ENOMEM;
	for (depth = 0; depth < UFS_NIADDR; depth++) {
		volume->indirect[depth] = malloc(volume->super.bsize);
		if (volume->indirect[depth] == NULL)
			return ENOMEM;
	}

	/* The maps of every inode and fragment. */
	volume->state = calloc(volume->inode_count, 1);
	volume->dtype = calloc(volume->inode_count, 1);
	volume->references = calloc(volume->inode_count, sizeof(uint32_t));
	volume->parent = calloc(volume->inode_count, sizeof(uint32_t));
	volume->owned = calloc((size_t)(volume->super.size / 8U + 1U), 1);
	if (volume->state == NULL || volume->dtype == NULL || volume->references == NULL || volume->parent == NULL || volume->owned == NULL)
		return ENOMEM;

	/* Every cylinder group. */
	error = fsck_groups_load(volume);
	if (error != 0)
		return error;

	/* Succeeded: the volume can be checked. */
	return 0;
}

/*
 * Writes back the inode block and the changed groups and superblock,
 * makes them durable and closes the device.  Returns 0, or the first
 * error.
 */
int
fsck_volume_close(
	struct fsck_volume *volume)
{
	uint32_t group;
	uint64_t fragment;
	uint32_t depth;
	int error;
	int first;

	/* What was changed goes back when repairing; the first error is kept. */
	first = 0;
	if (volume->repair && volume->fd >= 0 && volume->groups != NULL) {
		error = fsck_inode_flush(volume);
		if (error != 0 && first == 0)
			first = error;
		for (group = 0; group < volume->super.ncg; group++) {
			if (!volume->group_dirty[group])
				continue;
			fragment = fsck_group_start(volume, group) + volume->super.cblkno;
			error = fsck_write_block(volume, fragment, fsck_group(volume, group));
			if (error != 0 && first == 0)
				first = error;
		}

		/* The superblock when its totals or clean flag changed. */
		if (volume->super_dirty) {
			error = fsck_pwrite(volume, UFS_SBLOCK_OFFSET, volume->super_raw, sizeof(volume->super_raw));
			if (error != 0 && first == 0)
				first = error;
		}

		/* All of it durable. */
		error = fsync(volume->fd);
		if (error != 0 && first == 0)
			first = errno;
	}

	/* The device and the memory. */
	if (volume->fd >= 0)
		(void)close(volume->fd);
	volume->fd = -1;
	for (depth = 0; depth < UFS_NIADDR; depth++)
		free(volume->indirect[depth]);
	free(volume->data);
	free(volume->inode_block);
	free(volume->groups);
	free(volume->group_dirty);
	free(volume->state);
	free(volume->dtype);
	free(volume->references);
	free(volume->parent);
	free(volume->owned);
	free(volume->claimed);

	/* Reports the first failure. */
	if (first != 0)
		return first;

	/* Succeeded: the repairs are on the device. */
	return 0;
}

/* Reads one block at a fragment: 0, or the I/O's error. */
int
fsck_read_block(
	struct fsck_volume *volume,
	uint64_t fragment,
	uint8_t *buffer)
{
	/* The block, at the fragment's byte offset. */
	return fsck_pread(volume, fragment * volume->super.fsize, buffer, volume->super.bsize);
}

/* Writes one block at a fragment: 0, or the I/O's error (nothing is written without -y). */
int
fsck_write_block(
	struct fsck_volume *volume,
	uint64_t fragment,
	const uint8_t *buffer)
{
	/* Without -y nothing reaches the device. */
	if (!volume->repair)
		return 0;

	/* Succeeded or not, the block at the fragment's byte offset. */
	return fsck_pwrite(volume, fragment * volume->super.fsize, buffer, volume->super.bsize);
}

/*
 * Returns an inode's 256 bytes in the held inode block, reading the block
 * (and writing back the one held before) when needed.  NULL after an I/O
 * error, which is also recorded.
 */
uint8_t *
fsck_inode(
	struct fsck_volume *volume,
	uint32_t number)
{
	uint32_t group;
	uint32_t index;
	uint64_t fragment;
	int error;

	/* The inode's group, its block in the group's table, and its place in the block. */
	group = number / volume->super.ipg;
	index = number % volume->super.ipg;
	fragment = fsck_group_start(volume, group) + volume->super.iblkno + (uint64_t)(index / volume->super.inopb) * volume->super.frag;

	/* Another block: the held one goes back first. */
	if (!volume->inode_block_valid || volume->inode_block_fragment != fragment) {
		error = fsck_inode_flush(volume);
		if (error == 0)
			error = fsck_read_block(volume, fragment, volume->inode_block);
		if (error != 0) {
			volume->io_error = error;
			volume->inode_block_valid = 0;
			return NULL;
		}

		/* The block held from here on. */
		volume->inode_block_fragment = fragment;
		volume->inode_block_valid = 1;
	}

	/* Succeeded: the inode's bytes. */
	return volume->inode_block + (size_t)(index % volume->super.inopb) * UFS_DINODE_SIZE;
}

/* Marks the held inode block as changed. */
void
fsck_inode_dirty(
	struct fsck_volume *volume)
{
	/* Succeeded: it goes back before another is read, and at the end. */
	volume->inode_block_dirty = 1;
}

/* Reports a cylinder group's block. */
uint8_t *
fsck_group(
	struct fsck_volume *volume,
	uint32_t group)
{
	/* Succeeded: the group's block among all of them. */
	return volume->groups + (size_t)group * volume->super.bsize;
}

/* Reports the first fragment of a cylinder group (cgbase with the old rotation offset). */
uint64_t
fsck_group_start(
	const struct fsck_volume *volume,
	uint32_t group)
{
	/* Succeeded: the group's base. */
	return (uint64_t)group * volume->super.fpg + (uint64_t)volume->super.cgoffset * (group & ~volume->super.cgmask);
}

/* Reports how many fragments a cylinder group has (the last may be short). */
uint32_t
fsck_group_fragments(
	const struct fsck_volume *volume,
	uint32_t group)
{
	uint64_t start;

	/* The group's fragments, up to the end of the volume. */
	start = fsck_group_start(volume, group);
	if (volume->super.size - start < volume->super.fpg)
		return (uint32_t)(volume->super.size - start);

	/* Succeeded: a whole group. */
	return volume->super.fpg;
}

/* Reports whether a fragment starts a whole block of some group's data area. */
int
fsck_valid_block(
	const struct fsck_volume *volume,
	uint64_t fragment)
{
	uint64_t group;
	uint64_t start;
	uint64_t local;
	uint32_t fragments;

	/* The group the fragment falls in (the rotation offset is zero on every volume made now). */
	group = fragment / volume->super.fpg;
	if (group >= volume->super.ncg)
		return 0;
	start = fsck_group_start(volume, (uint32_t)group);
	if (fragment < start)
		return 0;
	local = fragment - start;

	/* In the data area, block-aligned, and whole. */
	if (local < volume->super.dblkno)
		return 0;
	if ((local % volume->super.frag) != 0)
		return 0;
	fragments = fsck_group_fragments(volume, (uint32_t)group);
	if (local + volume->super.frag > fragments)
		return 0;

	/* Succeeded: a block of the data area. */
	return 1;
}

/* Reads a 16-bit value in the volume's byte order. */
uint16_t
fsck_get16(
	const struct fsck_volume *volume,
	const uint8_t *buffer,
	size_t offset)
{
	/* Succeeded: the value. */
	return ufs_get16(buffer, offset, volume->super.swapped);
}

/* Reads a 32-bit value in the volume's byte order. */
uint32_t
fsck_get32(
	const struct fsck_volume *volume,
	const uint8_t *buffer,
	size_t offset)
{
	/* Succeeded: the value. */
	return ufs_get32(buffer, offset, volume->super.swapped);
}

/* Reads a 64-bit value in the volume's byte order. */
uint64_t
fsck_get64(
	const struct fsck_volume *volume,
	const uint8_t *buffer,
	size_t offset)
{
	/* Succeeded: the value. */
	return ufs_get64(buffer, offset, volume->super.swapped);
}

/* Writes a 16-bit value in the volume's byte order. */
void
fsck_put16(
	const struct fsck_volume *volume,
	uint8_t *buffer,
	size_t offset,
	uint16_t value)
{
	/* Succeeded: the value is stored. */
	ufs_put16(buffer, offset, value, volume->super.swapped);
}

/* Writes a 32-bit value in the volume's byte order. */
void
fsck_put32(
	const struct fsck_volume *volume,
	uint8_t *buffer,
	size_t offset,
	uint32_t value)
{
	/* Succeeded: the value is stored. */
	ufs_put32(buffer, offset, value, volume->super.swapped);
}

/* Writes a 64-bit value in the volume's byte order. */
void
fsck_put64(
	const struct fsck_volume *volume,
	uint8_t *buffer,
	size_t offset,
	uint64_t value)
{
	/* Succeeded: the value is stored. */
	ufs_put64(buffer, offset, value, volume->super.swapped);
}

/* Tests a bit of a map, the lowest bit of each byte first. */
int
fsck_bit(
	const uint8_t *map,
	uint64_t bit)
{
	/* Succeeded: the bit. */
	return (map[bit / 8U] >> (bit % 8U)) & 1U;
}

/* Sets or clears a bit of a map. */
void
fsck_set_bit(
	uint8_t *map,
	uint64_t bit,
	int value)
{
	/* Set, or clear. */
	if (value) {
		map[bit / 8U] = (uint8_t)(map[bit / 8U] | (1U << (bit % 8U)));
		return;
	}

	/* Succeeded: cleared. */
	map[bit / 8U] = (uint8_t)(map[bit / 8U] & ~(1U << (bit % 8U)));
}

/* Reads bytes at an offset of the device, all of them: 0, or EIO for a short read. */
static int
fsck_pread(
	struct fsck_volume *volume,
	uint64_t offset,
	void *buffer,
	size_t length)
{
	ssize_t got;
	size_t done;

	/* Every byte, through interruptions and short reads. */
	done = 0;
	while (done < length) {
		got = pread(volume->fd, (uint8_t *)buffer + done, length - done, (off_t)(offset + done));
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			return errno;
		if (got == 0)
			return EIO;
		done += (size_t)got;
	}

	/* Succeeded: the bytes are read. */
	return 0;
}

/* Writes bytes at an offset of the device, all of them: 0, or the I/O's error. */
static int
fsck_pwrite(
	struct fsck_volume *volume,
	uint64_t offset,
	const void *buffer,
	size_t length)
{
	ssize_t put;
	size_t done;

	/* Every byte, through interruptions and short writes. */
	done = 0;
	while (done < length) {
		put = pwrite(volume->fd, (const uint8_t *)buffer + done, length - done, (off_t)(offset + done));
		if (put < 0 && errno == EINTR)
			continue;
		if (put < 0)
			return errno;
		if (put == 0)
			return EIO;
		done += (size_t)put;
	}

	/* Succeeded: the bytes are written. */
	return 0;
}

/* Writes the held inode block back when it was changed: 0, or the I/O's error. */
static int
fsck_inode_flush(
	struct fsck_volume *volume)
{
	int error;

	/* Nothing held, or nothing changed. */
	if (!volume->inode_block_valid || !volume->inode_block_dirty)
		return 0;

	/* The block goes back. */
	error = fsck_write_block(volume, volume->inode_block_fragment, volume->inode_block);
	if (error != 0)
		return error;
	volume->inode_block_dirty = 0;

	/* Succeeded: the block is written. */
	return 0;
}

/*
 * Reads every cylinder group and checks what fsck cannot repair: the
 * magic, the group's number and size, and the maps' offsets.  Returns 0,
 * EINVAL, ENOMEM, or the I/O's error.
 */
static int
fsck_groups_load(
	struct fsck_volume *volume)
{
	uint8_t *block;
	uint32_t group;
	uint32_t used;
	uint32_t free_map;
	uint32_t next;
	uint32_t magic;
	uint32_t number;
	uint32_t fragments;
	uint32_t expected;
	int error;

	/* Every group's block. */
	volume->groups = malloc((size_t)volume->super.ncg * volume->super.bsize);
	volume->group_dirty = calloc(volume->super.ncg, 1);
	if (volume->groups == NULL || volume->group_dirty == NULL)
		return ENOMEM;

	/* Each group: read, then the parts the passes rely on. */
	for (group = 0; group < volume->super.ncg; group++) {
		block = fsck_group(volume, group);
		error = fsck_read_block(volume, fsck_group_start(volume, group) + volume->super.cblkno, block);
		if (error != 0)
			return error;

		/* The magic, the number and the size. */
		magic = fsck_get32(volume, block, UFS_CG_MAGIC);
		number = fsck_get32(volume, block, UFS_CG_CGX);
		fragments = fsck_get32(volume, block, UFS_CG_NDBLK);
		expected = fsck_group_fragments(volume, group);
		if (magic != UFS_CG_MAGIC_VALUE || number != group || fragments != expected) {
			fprintf(stderr, "fsck: %s: cylinder group %u is damaged (magic, number or size); it cannot be repaired\n", volume->name, group);
			return EINVAL;
		}

		/* The two maps fit the block and hold every inode and fragment. */
		used = fsck_get32(volume, block, UFS_CG_IUSEDOFF);
		free_map = fsck_get32(volume, block, UFS_CG_FREEOFF);
		next = fsck_get32(volume, block, UFS_CG_NEXTFREEOFF);
		if (used == 0 || used >= free_map || free_map >= next || next > volume->super.bsize ||
		    (free_map - used) * 8U < volume->super.ipg || (next - free_map) * 8U < volume->super.fpg) {
			fprintf(stderr, "fsck: %s: cylinder group %u has damaged map offsets; it cannot be repaired\n", volume->name, group);
			return EINVAL;
		}
	}

	/* Succeeded: every group is held. */
	return 0;
}
