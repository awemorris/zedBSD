/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The UFS checker (fsck, BUG-312): the volume it works on and its passes.
 *
 * fsck reads a UFS volume through its device (or an image file), checks
 * every allocated inode and its blocks, the directories from the root
 * down, the link counts, and the cylinder groups' maps and counts and the
 * superblock's totals, and with -y repairs what it finds.  It works on a
 * volume whose batched journal the kernel has already replayed (init runs
 * it on the read-only root the kernel mounted), and it leaves the journal
 * file and the areas past the end of the file system (the v2 journal and
 * the snapshot) alone.
 */

#ifndef USERLAND_BASE_FSCK_FSCK_H
#define USERLAND_BASE_FSCK_FSCK_H

#include "userland/base/mkfs/ufs-disk.h"

#include <stddef.h>
#include <stdint.h>

/* The exit codes, as FreeBSD's fsck answers: clean, repaired, or problems left. */
#define FSCK_EXIT_CLEAN		0
#define FSCK_EXIT_REPAIRED	4
#define FSCK_EXIT_PROBLEMS	8

/* The kinds of file an inode can hold (UFS_IFMT of its mode). */
#define FSCK_IFMT		0170000U
#define FSCK_IFIFO		0010000U
#define FSCK_IFCHR		0020000U
#define FSCK_IFDIR		0040000U
#define FSCK_IFBLK		0060000U
#define FSCK_IFREG		0100000U
#define FSCK_IFLNK		0120000U
#define FSCK_IFSOCK		0140000U

/* What fsck knows of an inode. */
#define FSCK_INODE_FREE		0U
#define FSCK_INODE_FILE		1U
#define FSCK_INODE_DIR		2U

/* Flags beside an inode's kind: reached from the root. */
#define FSCK_INODE_REACHED	0x10U
#define FSCK_INODE_KIND_MASK	0x0fU

/*
 * The volume and what the passes found.
 *
 * The cylinder groups are read whole and written back when changed; the
 * inodes are read and written one inode block at a time.
 */
struct fsck_volume {
	/* The device and how it is used: written only with -y. */
	int fd;
	int repair;
	const char *name;

	/* The superblock, as read and as decoded, and whether it was changed. */
	uint8_t super_raw[UFS_SBLOCK_SIZE];
	struct ufs_super super;
	uint32_t inode_count;
	int super_dirty;

	/* Every cylinder group's block, and which were changed. */
	uint8_t *groups;
	uint8_t *group_dirty;

	/* The one inode block held, its fragment, and whether it was changed. */
	uint8_t *inode_block;
	uint64_t inode_block_fragment;
	int inode_block_valid;
	int inode_block_dirty;

	/* A block for reading data and directory blocks, and one for indirect blocks per depth. */
	uint8_t *data;
	uint8_t *indirect[UFS_NIADDR];

	/* Per inode: its kind and flags, its directory entry type, the names that point at it, and a directory's parent. */
	uint8_t *state;
	uint8_t *dtype;
	uint32_t *references;
	uint32_t *parent;

	/* Per fragment: whether an inode owns it. */
	uint8_t *owned;

	/* The fragments the inode being checked has claimed, to give back if it is cleared. */
	uint64_t *claimed;
	size_t claimed_count;
	size_t claimed_capacity;

	/* The directory lost+found, once found or made (0: none). */
	uint32_t lost_found;

	/* Problems found, problems repaired, and an I/O error that ends the run. */
	unsigned problems;
	unsigned repairs;
	int io_error;

	/* Totals for the last line. */
	uint64_t files;
	uint64_t used_blocks;
};

/*
 * ==== The volume (volume.c) ====
 */

/* Opens a device or image and reads its superblock and cylinder groups: 0, or an errno. */
int fsck_volume_open(struct fsck_volume *volume, const char *name, int repair);

/* Writes back what was changed, makes it durable and closes the device: 0, or an errno. */
int fsck_volume_close(struct fsck_volume *volume);

/* Reads or writes one block at a fragment. */
int fsck_read_block(struct fsck_volume *volume, uint64_t fragment, uint8_t *buffer);
int fsck_write_block(struct fsck_volume *volume, uint64_t fragment, const uint8_t *buffer);

/* Returns an inode's 256 bytes in the held inode block, or NULL after an I/O error. */
uint8_t *fsck_inode(struct fsck_volume *volume, uint32_t number);

/* Marks the held inode block as changed. */
void fsck_inode_dirty(struct fsck_volume *volume);

/* Reports a cylinder group's block, the first fragment of a group, and how many fragments it has. */
uint8_t *fsck_group(struct fsck_volume *volume, uint32_t group);
uint64_t fsck_group_start(const struct fsck_volume *volume, uint32_t group);
uint32_t fsck_group_fragments(const struct fsck_volume *volume, uint32_t group);

/* Reports whether a fragment starts a whole block of some group's data area. */
int fsck_valid_block(const struct fsck_volume *volume, uint64_t fragment);

/* Reads and writes 16, 32 and 64-bit values in the volume's byte order. */
uint16_t fsck_get16(const struct fsck_volume *volume, const uint8_t *buffer, size_t offset);
uint32_t fsck_get32(const struct fsck_volume *volume, const uint8_t *buffer, size_t offset);
uint64_t fsck_get64(const struct fsck_volume *volume, const uint8_t *buffer, size_t offset);
void fsck_put16(const struct fsck_volume *volume, uint8_t *buffer, size_t offset, uint16_t value);
void fsck_put32(const struct fsck_volume *volume, uint8_t *buffer, size_t offset, uint32_t value);
void fsck_put64(const struct fsck_volume *volume, uint8_t *buffer, size_t offset, uint64_t value);

/* Tests and sets a bit of a map. */
int fsck_bit(const uint8_t *map, uint64_t bit);
void fsck_set_bit(uint8_t *map, uint64_t bit, int value);

/*
 * ==== The passes (check.c, directory.c) ====
 */

/* Runs every pass: 0, or an errno when the volume could not be read or is beyond repair. */
int fsck_check(struct fsck_volume *volume);

/* Reports a problem; repaired says whether -y repairs it. */
void fsck_problem(struct fsck_volume *volume, int repaired, const char *format, ...) __attribute__((format(printf, 3, 4)));

/* Reports the fragment of a file's logical block (0 for a hole), or UINT64_MAX after an error. */
uint64_t fsck_file_block(struct fsck_volume *volume, uint32_t number, uint64_t logical);

/* Clears an inode: its bytes are zeroed and fsck forgets its blocks. */
void fsck_clear_inode(struct fsck_volume *volume, uint32_t number);

/* Walks the directories from the root down: entries, dot entries, parents and references. */
int fsck_directories(struct fsck_volume *volume);

/* Connects what the walk did not reach: to lost+found, or freed when nothing names it. */
int fsck_connect(struct fsck_volume *volume);

#endif /* USERLAND_BASE_FSCK_FSCK_H */
