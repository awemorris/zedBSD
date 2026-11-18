/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The passes of fsck (BUG-312).
 *
 * 1. Inodes: every inode the groups mark allocated must have a file type
 *    the kernel knows, fields the kernel accepts when it loads the inode
 *    (decode_inode_raw in src/drivers/fs/ufs.c), and blocks that lie in a
 *    group's data area and belong to no other inode.  An inode with a
 *    block outside the data area, or one an earlier inode owns, is
 *    cleared (the earlier owner keeps the block).  Fields that can be
 *    corrected (nanoseconds, a directory's length, the attribute length,
 *    the block count) are corrected.
 * 2. Directories (directory.c): the walk from the root, the entries, the
 *    dot entries, the parents and the references.
 * 3. Connection (directory.c): what the walk did not reach.
 * 4. Link counts: each inode's count is the number of entries that name
 *    it.
 * 5. Maps and counts: the blocks of every inode are counted again, and
 *    each group's inode and free maps and counts and the superblock's
 *    totals are rebuilt from them; the clean flag is set.
 *
 * Without -y every problem is only reported.
 */

#include "fsck.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The greatest nanoseconds a time can hold. */
#define FSCK_NSEC_LIMIT		1000000000U

/* The longest inline symbolic link target (the kernel's 120 bytes in the pointers). */
#define FSCK_SHORTLINK_MAX	120U

static int fsck_pass_inodes(struct fsck_volume *volume);
static void fsck_check_inode(struct fsck_volume *volume, uint32_t number);
static int fsck_inode_blocks(struct fsck_volume *volume, uint32_t number, int *bad, int *duplicate, uint64_t *data_blocks, uint64_t *ext_blocks);
static int fsck_claim(struct fsck_volume *volume, uint64_t fragment, int *bad, int *duplicate);
static int fsck_claim_indirect(struct fsck_volume *volume, uint64_t fragment, unsigned depth, int *bad, int *duplicate, uint64_t *count);
static void fsck_release_claimed(struct fsck_volume *volume);
static int fsck_has_blocks(const struct fsck_volume *volume, uint16_t mode, uint64_t size);
static void fsck_pass_links(struct fsck_volume *volume);
static int fsck_pass_maps(struct fsck_volume *volume);
static void fsck_group_maps(struct fsck_volume *volume, uint32_t group, uint64_t totals[4]);
static uint8_t fsck_entry_type(uint16_t mode);
static const char *fsck_kind_name(uint16_t mode);

/*
 * Runs every pass.  Returns 0, EINVAL when the volume is beyond what fsck
 * repairs (the root directory, a cylinder group), or an I/O error.
 */
int
fsck_check(
	struct fsck_volume *volume)
{
	int error;

	/* 1: inodes and their blocks. */
	error = fsck_pass_inodes(volume);
	if (error != 0)
		return error;

	/* 2 and 3: directories, then what they do not reach. */
	error = fsck_directories(volume);
	if (error != 0)
		return error;
	error = fsck_connect(volume);
	if (error != 0)
		return error;

	/* 4: link counts. */
	fsck_pass_links(volume);
	if (volume->io_error != 0)
		return volume->io_error;

	/* 5: maps, counts and totals. */
	error = fsck_pass_maps(volume);
	if (error != 0)
		return error;

	/* Succeeded: every pass ran. */
	return 0;
}

/* Reports a problem, and whether it is repaired. */
void
fsck_problem(
	struct fsck_volume *volume,
	int repaired,
	const char *format,
	...)
{
	va_list arguments;
	const char *outcome;

	/* Counted, and what is done about it. */
	volume->problems++;
	outcome = "not repaired";
	if (repaired && volume->repair) {
		outcome = "repaired";
		volume->repairs++;
	} else if (repaired) {
		outcome = "would be repaired with -y";
	}

	/* One line. */
	printf("fsck: %s: ", volume->name);
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf(" (%s)\n", outcome);
}

/*
 * Reports the fragment a file's logical block lives at, 0 for a hole, or
 * UINT64_MAX after an I/O error or for a block past the triple indirect.
 */
uint64_t
fsck_file_block(
	struct fsck_volume *volume,
	uint32_t number,
	uint64_t logical)
{
	uint8_t *raw;
	uint64_t pointer;
	uint64_t span;
	uint64_t index;
	unsigned depth;
	unsigned level;
	int valid;
	int error;

	/* The inode. */
	raw = fsck_inode(volume, number);
	if (raw == NULL)
		return UINT64_MAX;

	/* A direct block. */
	if (logical < UFS_NDADDR) {
		pointer = fsck_get64(volume, raw, UFS_DI_DB + logical * 8U);
		return pointer;
	}

	/* Past the direct blocks. */
	logical -= UFS_NDADDR;

	/* Which indirect pointer, and how far into its tree. */
	span = volume->super.nindir;
	for (depth = 1; depth <= UFS_NIADDR; depth++) {
		if (logical < span)
			break;
		logical -= span;
		span *= volume->super.nindir;
	}

	/* Past the triple indirect. */
	if (depth > UFS_NIADDR)
		return UINT64_MAX;

	/* Down the tree, one indirect block per level. */
	pointer = fsck_get64(volume, raw, UFS_DI_IB + (depth - 1U) * 8U);
	for (level = depth; level > 0; level--) {
		if (pointer == 0)
			return 0;
		valid = fsck_valid_block(volume, pointer);
		if (!valid)
			return 0;
		error = fsck_read_block(volume, pointer, volume->indirect[level - 1U]);
		if (error != 0)
			return UINT64_MAX;
		span /= volume->super.nindir;
		index = logical / span;
		logical %= span;
		pointer = fsck_get64(volume, volume->indirect[level - 1U], index * 8U);
	}

	/* Succeeded: the block, or 0 for a hole. */
	return pointer;
}

/* Clears an inode: its bytes are zeroed, and fsck counts it free. */
void
fsck_clear_inode(
	struct fsck_volume *volume,
	uint32_t number)
{
	uint8_t *raw;

	/* The inode's bytes. */
	raw = fsck_inode(volume, number);
	if (raw != NULL && volume->repair) {
		memset(raw, 0, UFS_DINODE_SIZE);
		fsck_inode_dirty(volume);
	}

	/* Succeeded: free from here on. */
	volume->state[number] = FSCK_INODE_FREE;
}

/*
 * Pass 1: each inode a group's map marks allocated.  Returns 0, EINVAL
 * when the root directory's inode is not a valid directory, or an I/O
 * error.
 */
static int
fsck_pass_inodes(
	struct fsck_volume *volume)
{
	uint8_t *block;
	uint32_t used_offset;
	uint32_t group;
	uint32_t index;
	uint32_t number;
	int used;

	/* Every group's allocated inodes; 0 and 1 are reserved and kept as they are. */
	for (group = 0; group < volume->super.ncg; group++) {
		block = fsck_group(volume, group);
		used_offset = fsck_get32(volume, block, UFS_CG_IUSEDOFF);
		for (index = 0; index < volume->super.ipg; index++) {
			number = group * volume->super.ipg + index;
			if (number < UFS_ROOT_INO)
				continue;
			used = fsck_bit(block + used_offset, index);
			if (!used)
				continue;
			fsck_check_inode(volume, number);
			if (volume->io_error != 0)
				return volume->io_error;
		}
	}

	/* The root directory must have survived. */
	if ((volume->state[UFS_ROOT_INO] & FSCK_INODE_KIND_MASK) != FSCK_INODE_DIR) {
		fprintf(stderr, "fsck: %s: the root directory's inode is damaged; the volume cannot be repaired\n", volume->name);
		return EINVAL;
	}

	/* Succeeded: every allocated inode is a file fsck knows. */
	return 0;
}

/* Checks one allocated inode, its fields and its blocks. */
static void
fsck_check_inode(
	struct fsck_volume *volume,
	uint32_t number)
{
	static const size_t nanoseconds[3] = { UFS_DI_ATIMENSEC, UFS_DI_MTIMENSEC, UFS_DI_CTIMENSEC };
	uint8_t *raw;
	uint64_t size;
	uint64_t recorded;
	uint64_t data_blocks;
	uint64_t ext_blocks;
	uint64_t sectors;
	uint32_t extsize;
	uint16_t mode;
	unsigned slot;
	uint64_t pointer;
	uint32_t nanosecond;
	uint8_t type;
	int needed;
	int bad;
	int duplicate;
	int error;

	/* The inode's kind and length. */
	raw = fsck_inode(volume, number);
	if (raw == NULL)
		return;
	mode = fsck_get16(volume, raw, UFS_DI_MODE);
	size = fsck_get64(volume, raw, UFS_DI_SIZE);

	/* A type the kernel has no file kind for: the slot is free. */
	type = fsck_entry_type(mode);
	if (type == 0 && (mode & FSCK_IFMT) != FSCK_IFCHR && (mode & FSCK_IFMT) != FSCK_IFBLK) {
		fsck_problem(volume, 1, "inode %u is allocated but has no file type (mode 0%o); it is freed", number, mode);
		fsck_clear_inode(volume, number);
		return;
	}

	/* A length no file of this volume can have. */
	if (size > volume->super.maxfilesize) {
		fsck_problem(volume, 1, "inode %u (%s) is %llu bytes long, longer than the volume allows; it is cleared",
		    number, fsck_kind_name(mode), (unsigned long long)size);
		fsck_clear_inode(volume, number);
		return;
	}

	/* A directory holds at least the record block that names dot, in whole record blocks. */
	if ((mode & FSCK_IFMT) == FSCK_IFDIR && size < UFS_DIRBLKSIZ) {
		fsck_problem(volume, 1, "directory %u is %llu bytes long, shorter than one record block; it is cleared", number, (unsigned long long)size);
		fsck_clear_inode(volume, number);
		return;
	}

	/* A directory's length in part of a record block is cut to whole ones. */
	if ((mode & FSCK_IFMT) == FSCK_IFDIR && (size % UFS_DIRBLKSIZ) != 0) {
		fsck_problem(volume, 1, "directory %u is %llu bytes long, not whole record blocks; it is shortened", number, (unsigned long long)size);
		size -= size % UFS_DIRBLKSIZ;
		if (volume->repair) {
			fsck_put64(volume, raw, UFS_DI_SIZE, size);
			fsck_inode_dirty(volume);
		}
	}

	/* Nanoseconds that overflow into seconds. */
	for (slot = 0; slot < 3U; slot++) {
		nanosecond = fsck_get32(volume, raw, nanoseconds[slot]);
		if (nanosecond < FSCK_NSEC_LIMIT)
			continue;
		fsck_problem(volume, 1, "inode %u has a time with more than a second of nanoseconds; they are cleared", number);
		if (volume->repair) {
			fsck_put32(volume, raw, nanoseconds[slot], 0);
			fsck_inode_dirty(volume);
		}
	}

	/* The attribute area: no longer than its pointers, and a pointer for exactly the blocks it covers. */
	extsize = fsck_get32(volume, raw, UFS_DI_EXTSIZE);
	if (extsize > UFS_NXADDR * volume->super.bsize) {
		fsck_problem(volume, 1, "inode %u has an attribute area of %u bytes, more than it can hold; it is dropped", number, extsize);
		extsize = 0;
		if (volume->repair) {
			fsck_put32(volume, raw, UFS_DI_EXTSIZE, 0);
			for (slot = 0; slot < UFS_NXADDR; slot++)
				fsck_put64(volume, raw, UFS_DI_EXTB + slot * 8U, 0);
			fsck_inode_dirty(volume);
		}
	}

	/* Each attribute pointer against the area's length. */
	for (slot = 0; slot < UFS_NXADDR; slot++) {
		needed = extsize > slot * volume->super.bsize;
		pointer = fsck_get64(volume, raw, UFS_DI_EXTB + slot * 8U);
		if (needed && pointer == 0) {
			fsck_problem(volume, 1, "inode %u's attribute area lacks block %u; it is shortened", number, slot);
			extsize = slot * volume->super.bsize;
			if (volume->repair) {
				fsck_put32(volume, raw, UFS_DI_EXTSIZE, extsize);
				fsck_inode_dirty(volume);
			}

			continue;
		}

		/* A pointer past the area. */
		if (!needed && pointer != 0) {
			fsck_problem(volume, 1, "inode %u has an attribute block past its attribute area; it is dropped", number);
			if (volume->repair) {
				fsck_put64(volume, raw, UFS_DI_EXTB + slot * 8U, 0);
				fsck_inode_dirty(volume);
			}
		}
	}

	/* The blocks: inside a data area, and owned by no earlier inode. */
	error = fsck_inode_blocks(volume, number, &bad, &duplicate, &data_blocks, &ext_blocks);
	if (error != 0) {
		volume->io_error = error;
		return;
	}

	/* A block outside the data areas, or another inode's: the inode is cleared. */
	if (bad) {
		fsck_problem(volume, 1, "inode %u (%s) has a block outside the volume's data areas; it is cleared", number, fsck_kind_name(mode));
		fsck_release_claimed(volume);
		fsck_clear_inode(volume, number);
		return;
	}

	/* Another inode's block. */
	if (duplicate) {
		fsck_problem(volume, 1, "inode %u (%s) has a block that an earlier inode owns; it is cleared", number, fsck_kind_name(mode));
		fsck_release_claimed(volume);
		fsck_clear_inode(volume, number);
		return;
	}

	/* The block count: the data blocks, with or without the attribute blocks. */
	raw = fsck_inode(volume, number);
	if (raw == NULL)
		return;
	sectors = volume->super.bsize / UFS_SECTOR_SIZE;
	recorded = fsck_get64(volume, raw, UFS_DI_BLOCKS);
	if (recorded != data_blocks * sectors && recorded != (data_blocks + ext_blocks) * sectors) {
		fsck_problem(volume, 1, "inode %u records %llu sectors, but has %llu; corrected",
		    number, (unsigned long long)recorded, (unsigned long long)((data_blocks + ext_blocks) * sectors));
		if (volume->repair) {
			fsck_put64(volume, raw, UFS_DI_BLOCKS, (data_blocks + ext_blocks) * sectors);
			fsck_inode_dirty(volume);
		}
	}

	/* Succeeded: a file fsck knows, not yet reached. */
	volume->state[number] = FSCK_INODE_FILE;
	if ((mode & FSCK_IFMT) == FSCK_IFDIR)
		volume->state[number] = FSCK_INODE_DIR;
	volume->dtype[number] = fsck_entry_type(mode);
}

/*
 * Claims an inode's blocks in the owned map: its direct and indirect
 * blocks (none for a device, a short symbolic link) and its attribute
 * blocks.  The claims are kept in the claimed list so that a cleared
 * inode gives them back.  Returns 0 with the findings, or an I/O error.
 */
static int
fsck_inode_blocks(
	struct fsck_volume *volume,
	uint32_t number,
	int *bad,
	int *duplicate,
	uint64_t *data_blocks,
	uint64_t *ext_blocks)
{
	uint64_t direct[UFS_NDADDR];
	uint64_t indirect[UFS_NIADDR];
	uint64_t ext[UFS_NXADDR];
	uint64_t size;
	uint8_t *raw;
	uint16_t mode;
	unsigned slot;
	int has_blocks;
	int claim;
	int error;

	/* Nothing found or claimed yet; the pointers, copied: reading indirect blocks may move the held inode block. */
	*bad = 0;
	*duplicate = 0;
	*data_blocks = 0;
	*ext_blocks = 0;
	volume->claimed_count = 0;
	raw = fsck_inode(volume, number);
	if (raw == NULL)
		return volume->io_error;
	mode = fsck_get16(volume, raw, UFS_DI_MODE);
	size = fsck_get64(volume, raw, UFS_DI_SIZE);
	for (slot = 0; slot < UFS_NDADDR; slot++)
		direct[slot] = fsck_get64(volume, raw, UFS_DI_DB + slot * 8U);
	for (slot = 0; slot < UFS_NIADDR; slot++)
		indirect[slot] = fsck_get64(volume, raw, UFS_DI_IB + slot * 8U);
	for (slot = 0; slot < UFS_NXADDR; slot++)
		ext[slot] = fsck_get64(volume, raw, UFS_DI_EXTB + slot * 8U);

	/* The data blocks of a file that has them. */
	has_blocks = fsck_has_blocks(volume, mode, size);
	if (has_blocks) {
		for (slot = 0; slot < UFS_NDADDR && !*bad && !*duplicate; slot++) {
			if (direct[slot] == 0)
				continue;
			claim = fsck_claim(volume, direct[slot], bad, duplicate);
			if (claim == 0)
				(*data_blocks)++;
		}

		/* The trees below the indirect pointers. */
		for (slot = 0; slot < UFS_NIADDR && !*bad && !*duplicate; slot++) {
			error = fsck_claim_indirect(volume, indirect[slot], slot + 1U, bad, duplicate, data_blocks);
			if (error != 0)
				return error;
		}
	}

	/* The attribute blocks. */
	for (slot = 0; slot < UFS_NXADDR && !*bad && !*duplicate; slot++) {
		if (ext[slot] == 0)
			continue;
		claim = fsck_claim(volume, ext[slot], bad, duplicate);
		if (claim == 0)
			(*ext_blocks)++;
	}

	/* Succeeded: the findings are reported. */
	return 0;
}

/* Claims one block: 0, or -1 with bad or duplicate set. */
static int
fsck_claim(
	struct fsck_volume *volume,
	uint64_t fragment,
	int *bad,
	int *duplicate)
{
	uint64_t *grown;
	uint32_t part;
	int valid;
	int owned;

	/* A block of a data area. */
	valid = fsck_valid_block(volume, fragment);
	if (!valid) {
		*bad = 1;
		return -1;
	}

	/* Not owned already. */
	for (part = 0; part < volume->super.frag; part++) {
		owned = fsck_bit(volume->owned, fragment + part);
		if (owned) {
			*duplicate = 1;
			return -1;
		}
	}

	/* Recorded, so that a cleared inode gives it back. */
	if (volume->claimed_count == volume->claimed_capacity) {
		grown = realloc(volume->claimed, (volume->claimed_capacity * 2U + 64U) * sizeof(uint64_t));
		if (grown == NULL) {
			*bad = 1;
			return -1;
		}

		/* The list, twice as long. */
		volume->claimed = grown;
		volume->claimed_capacity = volume->claimed_capacity * 2U + 64U;
	}

	/* The claim, and the block owned. */
	volume->claimed[volume->claimed_count] = fragment;
	volume->claimed_count++;
	for (part = 0; part < volume->super.frag; part++)
		fsck_set_bit(volume->owned, fragment + part, 1);

	/* Succeeded: owned. */
	return 0;
}

/*
 * Claims an indirect block and, below it, the blocks it names (depth 1
 * names data blocks).  Returns 0, or an I/O error.
 */
static int
fsck_claim_indirect(
	struct fsck_volume *volume,
	uint64_t fragment,
	unsigned depth,
	int *bad,
	int *duplicate,
	uint64_t *count)
{
	uint8_t *block;
	uint64_t pointer;
	uint32_t index;
	int claim;
	int error;

	/* Nothing below a missing pointer. */
	if (fragment == 0)
		return 0;

	/* The indirect block itself is a block of the file. */
	claim = fsck_claim(volume, fragment, bad, duplicate);
	if (claim != 0)
		return 0;
	(*count)++;

	/* What it names. */
	block = volume->indirect[depth - 1U];
	error = fsck_read_block(volume, fragment, block);
	if (error != 0)
		return error;
	for (index = 0; index < volume->super.nindir && !*bad && !*duplicate; index++) {
		pointer = fsck_get64(volume, block, index * 8U);
		if (pointer == 0)
			continue;
		if (depth == 1) {
			claim = fsck_claim(volume, pointer, bad, duplicate);
			if (claim == 0)
				(*count)++;
			continue;
		}

		/* A deeper tree. */
		error = fsck_claim_indirect(volume, pointer, depth - 1U, bad, duplicate, count);
		if (error != 0)
			return error;
	}

	/* Succeeded: the tree is claimed, or a finding stopped it. */
	return 0;
}

/* Gives back the blocks the inode being checked claimed. */
static void
fsck_release_claimed(
	struct fsck_volume *volume)
{
	size_t index;
	uint32_t part;

	/* Each claim. */
	for (index = 0; index < volume->claimed_count; index++) {
		for (part = 0; part < volume->super.frag; part++)
			fsck_set_bit(volume->owned, volume->claimed[index] + part, 0);
	}

	/* Succeeded: nothing is claimed. */
	volume->claimed_count = 0;
}

/* Reports whether a file of this kind and length has block pointers (not a device or a short link). */
static int
fsck_has_blocks(
	const struct fsck_volume *volume,
	uint16_t mode,
	uint64_t size)
{
	/* A device keeps its number where the first pointer would be. */
	if ((mode & FSCK_IFMT) == FSCK_IFCHR || (mode & FSCK_IFMT) == FSCK_IFBLK)
		return 0;

	/* A short link keeps its target there. */
	if ((mode & FSCK_IFMT) == FSCK_IFLNK && size <= volume->super.maxsymlinklen && size <= FSCK_SHORTLINK_MAX)
		return 0;

	/* Succeeded: the pointers name blocks. */
	return 1;
}

/* Pass 4: each reached inode's link count is the number of entries naming it. */
static void
fsck_pass_links(
	struct fsck_volume *volume)
{
	uint8_t *raw;
	uint32_t number;
	uint32_t references;
	uint16_t links;

	/* Every inode the walk reached. */
	for (number = UFS_ROOT_INO; number < volume->inode_count; number++) {
		if ((volume->state[number] & FSCK_INODE_REACHED) == 0)
			continue;
		references = volume->references[number];
		raw = fsck_inode(volume, number);
		if (raw == NULL)
			return;
		links = fsck_get16(volume, raw, UFS_DI_NLINK);
		if (links == references || references > UINT16_MAX)
			continue;
		fsck_problem(volume, 1, "inode %u has a link count of %u, but %u entries name it; corrected", number, links, references);
		if (volume->repair) {
			fsck_put16(volume, raw, UFS_DI_NLINK, (uint16_t)references);
			fsck_inode_dirty(volume);
		}
	}
}

/*
 * Pass 5: the blocks of every live inode are counted again (fsck_check's
 * clearing may have freed some), and every group's maps and counts and the
 * superblock's totals are rebuilt.  Returns 0, or an I/O error.
 */
static int
fsck_pass_maps(
	struct fsck_volume *volume)
{
	static const size_t total_offsets[4] = {
		UFS_FS_CSTOTAL_NDIR, UFS_FS_CSTOTAL_NBFREE, UFS_FS_CSTOTAL_NIFREE, UFS_FS_CSTOTAL_NFFREE
	};
	static const char *const total_names[4] = { "directories", "free blocks", "free inodes", "free fragments" };
	uint64_t totals[4];
	uint64_t recorded;
	uint64_t data_blocks;
	uint64_t ext_blocks;
	uint32_t number;
	uint32_t group;
	unsigned slot;
	int bad;
	int duplicate;
	int error;

	/* The owned map again, from the inodes that are left. */
	memset(volume->owned, 0, (size_t)(volume->super.size / 8U + 1U));
	volume->used_blocks = 0;
	volume->files = 0;
	for (number = UFS_ROOT_INO; number < volume->inode_count; number++) {
		if ((volume->state[number] & FSCK_INODE_KIND_MASK) == FSCK_INODE_FREE)
			continue;
		error = fsck_inode_blocks(volume, number, &bad, &duplicate, &data_blocks, &ext_blocks);
		if (error != 0)
			return error;
		if (bad || duplicate)
			fsck_problem(volume, 0, "inode %u's blocks changed while it was checked", number);
		volume->used_blocks += data_blocks + ext_blocks;
		volume->files++;
	}

	/* Each group's maps and counts. */
	memset(totals, 0, sizeof(totals));
	for (group = 0; group < volume->super.ncg; group++)
		fsck_group_maps(volume, group, totals);

	/* The superblock's totals. */
	for (slot = 0; slot < 4U; slot++) {
		recorded = fsck_get64(volume, volume->super_raw, total_offsets[slot]);
		if (recorded == totals[slot])
			continue;
		fsck_problem(volume, 1, "the superblock counts %llu %s, but the groups hold %llu; corrected",
		    (unsigned long long)recorded, total_names[slot], (unsigned long long)totals[slot]);
		if (volume->repair) {
			fsck_put64(volume, volume->super_raw, total_offsets[slot], totals[slot]);
			volume->super_dirty = 1;
		}
	}

	/* A repaired volume is clean (the kernel marks it in use when it mounts it read-write). */
	if (volume->repair && volume->super_raw[UFS_FS_CLEAN] != 1U) {
		volume->super_raw[UFS_FS_CLEAN] = 1U;
		volume->super_dirty = 1;
	}

	/* Succeeded: the maps and counts agree with the inodes. */
	return 0;
}

/*
 * Rebuilds one group's inode and free maps and its four counts from what
 * the passes found, and adds the counts to the totals (directories, free
 * blocks, free inodes, free fragments).
 */
static void
fsck_group_maps(
	struct fsck_volume *volume,
	uint32_t group,
	uint64_t totals[4])
{
	uint8_t *block;
	uint8_t *used_map;
	uint8_t *free_map;
	uint64_t start;
	uint32_t fragments;
	uint32_t local;
	uint32_t part;
	uint32_t index;
	uint32_t number;
	uint32_t counts[4];
	uint32_t recorded[4];
	uint32_t free_fragments;
	uint32_t first_block;
	uint32_t offset;
	unsigned slot;
	uint8_t kind;
	int used;
	int marked;
	int owned;
	int is_free;
	int whole;
	int maps_wrong;
	int counts_wrong;

	/* The group's block, its two maps and its fragments. */
	block = fsck_group(volume, group);
	offset = fsck_get32(volume, block, UFS_CG_IUSEDOFF);
	used_map = block + offset;
	offset = fsck_get32(volume, block, UFS_CG_FREEOFF);
	free_map = block + offset;
	start = fsck_group_start(volume, group);
	fragments = fsck_group_fragments(volume, group);
	memset(counts, 0, sizeof(counts));
	maps_wrong = 0;

	/* The inode map: live inodes used, the reserved 0 and 1 as they are. */
	for (index = 0; index < volume->super.ipg; index++) {
		number = group * volume->super.ipg + index;
		marked = fsck_bit(used_map, index);
		used = marked;
		kind = FSCK_INODE_FREE;
		if (number >= UFS_ROOT_INO) {
			kind = volume->state[number] & FSCK_INODE_KIND_MASK;
			used = kind != FSCK_INODE_FREE;
		}

		/* A map bit that disagrees. */
		if (used != marked) {
			maps_wrong = 1;
			if (volume->repair)
				fsck_set_bit(used_map, index, used);
		}

		/* The counts of free inodes and directories. */
		if (!used)
			counts[2]++;
		if (kind == FSCK_INODE_DIR)
			counts[0]++;
	}

	/* The free map: the data area's fragments no inode owns are free; the metadata and the tail are not. */
	free_fragments = 0;
	for (local = 0; local < volume->super.fpg; local++) {
		/* The last group's tail lies past the volume, where nothing is owned. */
		owned = 0;
		if (local < fragments)
			owned = fsck_bit(volume->owned, start + local);
		marked = fsck_bit(free_map, local);
		is_free = local >= volume->super.dblkno && local < fragments && !owned;
		if (is_free != marked) {
			maps_wrong = 1;
			if (volume->repair)
				fsck_set_bit(free_map, local, is_free);
		}

		/* Counted when free. */
		if (is_free)
			free_fragments++;
	}

	/* Whole free blocks, and the free fragments outside them. */
	first_block = (volume->super.dblkno + volume->super.frag - 1U) / volume->super.frag * volume->super.frag;
	for (local = first_block; local + volume->super.frag <= fragments; local += volume->super.frag) {
		whole = 1;
		for (part = 0; part < volume->super.frag; part++) {
			owned = fsck_bit(volume->owned, start + local + part);
			if (owned)
				whole = 0;
		}

		/* A block none of whose fragments is owned. */
		if (whole)
			counts[1]++;
	}

	/* The fragments outside the whole blocks. */
	counts[3] = free_fragments - counts[1] * volume->super.frag;

	/* The counts as recorded, against the counts found. */
	recorded[0] = fsck_get32(volume, block, UFS_CG_NDIR);
	recorded[1] = fsck_get32(volume, block, UFS_CG_NBFREE);
	recorded[2] = fsck_get32(volume, block, UFS_CG_NIFREE);
	recorded[3] = fsck_get32(volume, block, UFS_CG_NFFREE);
	counts_wrong = 0;
	for (slot = 0; slot < 4U; slot++) {
		if (recorded[slot] != counts[slot])
			counts_wrong = 1;
	}

	/* A group whose maps or counts disagree is rebuilt. */
	if (maps_wrong || counts_wrong) {
		fsck_problem(volume, 1, "cylinder group %u: its maps or counts (%u directories, %u blocks, %u inodes, %u fragments free) disagree with the inodes (%u, %u, %u, %u); rebuilt",
		    group, recorded[0], recorded[1], recorded[2], recorded[3], counts[0], counts[1], counts[2], counts[3]);
		if (volume->repair) {
			fsck_put32(volume, block, UFS_CG_NDIR, counts[0]);
			fsck_put32(volume, block, UFS_CG_NBFREE, counts[1]);
			fsck_put32(volume, block, UFS_CG_NIFREE, counts[2]);
			fsck_put32(volume, block, UFS_CG_NFFREE, counts[3]);
			volume->group_dirty[group] = 1;
		}
	}

	/* Succeeded: the group's counts join the totals. */
	for (slot = 0; slot < 4U; slot++)
		totals[slot] += counts[slot];
}

/* Reports the directory entry type of a file mode, as the kernel writes it (dir_type), 0 for none. */
static uint8_t
fsck_entry_type(
	uint16_t mode)
{
	/* The kinds with a number. */
	switch (mode & FSCK_IFMT) {
	case FSCK_IFIFO:
		return 1U;
	case FSCK_IFDIR:
		return 4U;
	case FSCK_IFREG:
		return 8U;
	case FSCK_IFLNK:
		return 10U;
	case FSCK_IFSOCK:
		return 12U;
	default:
		break;
	}

	/* Succeeded: a device, or no kind. */
	return 0U;
}

/* Names a file mode's kind for the messages. */
static const char *
fsck_kind_name(
	uint16_t mode)
{
	/* Each kind. */
	switch (mode & FSCK_IFMT) {
	case FSCK_IFDIR:
		return "directory";
	case FSCK_IFREG:
		return "file";
	case FSCK_IFLNK:
		return "symbolic link";
	case FSCK_IFCHR:
	case FSCK_IFBLK:
		return "device";
	default:
		break;
	}

	/* Succeeded: another kind. */
	return "special file";
}
