/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The directory passes of fsck (BUG-312).
 *
 * Pass 2 walks the directories from the root down.  A directory is a
 * whole number of 512-byte record blocks; each record is an inode number,
 * the record's length, an entry type and a name, and the records of a
 * record block fill it exactly.  The first record block starts with "."
 * (the directory itself) and ".." (its parent).  A record that does not
 * hold together ends its record block: the rest becomes one empty record.
 * An entry naming a free inode, a second name of a directory, or a "." or
 * ".." elsewhere is emptied.  The walk counts the names of every inode.
 *
 * Pass 3 connects what the walk did not reach: a directory, or a file
 * that still has a link, gets a name "#<inode>" in /lost+found (made when
 * the volume has none); a file with no link left (unlinked while open
 * when the machine stopped) is freed.
 */

#include "fsck.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The bytes a record needs for a name of this length: the header, and the name rounded to four. */
#define FSCK_RECORD_BYTES(length)	(8U + (((uint32_t)(length) + 3U) & ~3U))

/* The record header: inode number, record length, type and name length. */
#define FSCK_RECORD_INO		0U
#define FSCK_RECORD_RECLEN	4U
#define FSCK_RECORD_TYPE	6U
#define FSCK_RECORD_NAMLEN	7U
#define FSCK_RECORD_NAME	8U

/* The entry type of a directory. */
#define FSCK_TYPE_DIR		4U

/* The longest name fsck writes ("#" and an inode number). */
#define FSCK_NAME_MAX		16U

/* The rounds of pass 3: the tops of lost subtrees, the other directories, the files. */
#define FSCK_ROUND_TOPS		0U
#define FSCK_ROUND_DIRS		1U
#define FSCK_ROUND_FILES	2U
#define FSCK_ROUNDS		3U

static int fsck_connect_directory(struct fsck_volume *volume, uint32_t number, unsigned round, uint32_t *queue);
static int fsck_connect_file(struct fsck_volume *volume, uint32_t number);
static int fsck_name_lost(struct fsck_volume *volume, uint32_t number, uint8_t type, uint32_t *directory);
static int fsck_walk(struct fsck_volume *volume, uint32_t first, uint32_t *queue);
static int fsck_scan_directory(struct fsck_volume *volume, uint32_t directory, uint32_t *queue, uint32_t *tail);
static void fsck_scan_chunk(struct fsck_volume *volume, uint32_t directory, uint8_t *chunk, int first, int *dirty, uint32_t *queue, uint32_t *tail);
static void fsck_scan_entry(struct fsck_volume *volume, uint32_t directory, uint8_t *chunk, uint32_t pos, int *dirty, uint32_t *queue, uint32_t *tail);
static uint32_t fsck_dot_entry(struct fsck_volume *volume, uint32_t directory, uint8_t *chunk, uint32_t pos, uint32_t reclen, const char *dots, uint32_t number, int *dirty);
static void fsck_put_record(struct fsck_volume *volume, uint8_t *chunk, uint32_t pos, uint32_t number, uint32_t reclen, uint8_t type, const char *name);
static int fsck_record_valid(const struct fsck_volume *volume, const uint8_t *chunk, uint32_t pos);
static int fsck_record_named(const uint8_t *chunk, uint32_t pos, const char *name);
static uint32_t fsck_lost_found(struct fsck_volume *volume);
static uint32_t fsck_make_lost_found(struct fsck_volume *volume);
static uint32_t fsck_find_entry(struct fsck_volume *volume, uint32_t directory, const char *name);
static int fsck_add_entry(struct fsck_volume *volume, uint32_t directory, const char *name, uint32_t number, uint8_t type);
static int fsck_add_in_record(struct fsck_volume *volume, uint8_t *chunk, uint32_t needed, const char *name, uint32_t number, uint8_t type);
static int fsck_add_record_block(struct fsck_volume *volume, uint32_t directory, uint8_t *block, const char *name, uint32_t number, uint8_t type);
static uint64_t fsck_allocate_block(struct fsck_volume *volume);
static uint32_t fsck_allocate_inode(struct fsck_volume *volume);
static uint32_t fsck_dotdot(struct fsck_volume *volume, uint32_t directory);

/*
 * Pass 2: walks the directories from the root down.  Returns 0, or an
 * I/O error.
 */
int
fsck_directories(
	struct fsck_volume *volume)
{
	uint32_t *queue;
	int error;

	/* The queue of directories to scan; each directory enters it once. */
	queue = malloc((size_t)volume->inode_count * sizeof(uint32_t));
	if (queue == NULL)
		return ENOMEM;

	/* The root is its own parent. */
	volume->parent[UFS_ROOT_INO] = UFS_ROOT_INO;
	volume->state[UFS_ROOT_INO] |= FSCK_INODE_REACHED;
	error = fsck_walk(volume, UFS_ROOT_INO, queue);
	free(queue);

	/* Reports an I/O error. */
	if (error != 0)
		return error;

	/* Succeeded: what the root reaches is counted. */
	return 0;
}

/*
 * Pass 3: connects what the walk did not reach, in three rounds: the
 * directories whose ".." does not name another unreached directory (so
 * that a lost subtree keeps its shape), the other directories, then the
 * files.  Returns 0, or an I/O error.
 */
int
fsck_connect(
	struct fsck_volume *volume)
{
	uint32_t *queue;
	uint32_t number;
	unsigned round;
	uint8_t kind;
	int error;

	/* The queue a connected directory's subtree is walked with. */
	queue = malloc((size_t)volume->inode_count * sizeof(uint32_t));
	if (queue == NULL)
		return ENOMEM;

	/* Each round, each allocated inode not reached. */
	error = 0;
	for (round = 0; round < FSCK_ROUNDS && error == 0; round++) {
		for (number = UFS_ROOT_INO + 1U; number < volume->inode_count && error == 0; number++) {
			if ((volume->state[number] & FSCK_INODE_REACHED) != 0)
				continue;
			kind = volume->state[number] & FSCK_INODE_KIND_MASK;
			if (kind == FSCK_INODE_DIR && round != FSCK_ROUND_FILES)
				error = fsck_connect_directory(volume, number, round, queue);
			else if (kind == FSCK_INODE_FILE && round == FSCK_ROUND_FILES)
				error = fsck_connect_file(volume, number);
		}
	}

	/* The queue is no longer needed. */
	free(queue);
	if (error != 0)
		return error;

	/* Succeeded: everything left is reached. */
	return 0;
}

/*
 * Connects a directory the walk did not reach to /lost+found and walks
 * it.  In the first round a directory whose ".." names another unreached
 * directory waits for that one.  Without -y the subtree is walked as if it
 * were moved, so that what is below it is reported once.  Returns 0, or
 * an I/O error.
 */
static int
fsck_connect_directory(
	struct fsck_volume *volume,
	uint32_t number,
	unsigned round,
	uint32_t *queue)
{
	uint32_t parent;
	uint32_t lost;
	uint8_t parent_state;
	int error;

	/* The parent its ".." names. */
	parent = fsck_dotdot(volume, number);
	parent_state = FSCK_INODE_FREE;
	if (parent < volume->inode_count && parent != number)
		parent_state = volume->state[parent];

	/* In the first round, a directory below another lost one waits. */
	if (round == FSCK_ROUND_TOPS && (parent_state & FSCK_INODE_KIND_MASK) == FSCK_INODE_DIR && (parent_state & FSCK_INODE_REACHED) == 0)
		return 0;

	/* Moved to /lost+found, or, without -y, as if it were. */
	fsck_problem(volume, 1, "directory %u is not reached from the root; it is moved to /lost+found/#%u", number, number);
	lost = UFS_ROOT_INO;
	if (parent < volume->inode_count)
		lost = parent;
	if (volume->repair) {
		error = fsck_name_lost(volume, number, FSCK_TYPE_DIR, &lost);
		if (error != 0)
			return 0;
	}

	/* Reached through its new name; its subtree is walked. */
	volume->references[number]++;
	volume->parent[number] = lost;
	volume->state[number] |= FSCK_INODE_REACHED;
	error = fsck_walk(volume, number, queue);
	if (error != 0)
		return error;

	/* Succeeded: the subtree is reached. */
	return 0;
}

/*
 * Connects a file nothing names: freed when no link is left (it was
 * unlinked while open), else named in /lost+found.  Returns 0, or an I/O
 * error.
 */
static int
fsck_connect_file(
	struct fsck_volume *volume,
	uint32_t number)
{
	uint8_t *raw;
	uint32_t lost;
	uint16_t links;
	int error;

	/* Its link count. */
	raw = fsck_inode(volume, number);
	if (raw == NULL)
		return volume->io_error;
	links = fsck_get16(volume, raw, UFS_DI_NLINK);

	/* No link left: freed. */
	if (links == 0) {
		fsck_problem(volume, 1, "inode %u was unlinked but is still allocated; it is freed", number);
		fsck_clear_inode(volume, number);
		return 0;
	}

	/* Moved to /lost+found, or, without -y, as if it were. */
	fsck_problem(volume, 1, "inode %u is not named by any directory; it is moved to /lost+found/#%u", number, number);
	if (volume->repair) {
		error = fsck_name_lost(volume, number, volume->dtype[number], &lost);
		if (error != 0)
			return 0;
	}

	/* Succeeded: reached through its new name. */
	volume->references[number]++;
	volume->state[number] |= FSCK_INODE_REACHED;
	return 0;
}

/*
 * Names an inode "#<inode>" in /lost+found, made when needed.  Returns 0
 * with the directory, or ENOSPC (reported) when it cannot be named.
 */
static int
fsck_name_lost(
	struct fsck_volume *volume,
	uint32_t number,
	uint8_t type,
	uint32_t *directory)
{
	char name[FSCK_NAME_MAX];
	uint32_t lost;
	int error;

	/* The directory, and the entry in it. */
	lost = fsck_lost_found(volume);
	(void)snprintf(name, sizeof(name), "#%u", number);
	error = ENOSPC;
	if (lost != 0)
		error = fsck_add_entry(volume, lost, name, number, type);

	/* A name that could not be given. */
	if (error != 0) {
		fsck_problem(volume, 0, "inode %u could not be given a name in /lost+found", number);
		return ENOSPC;
	}

	/* Succeeded: the inode has a name. */
	*directory = lost;
	return 0;
}

/* Walks the directories below one that was just reached: 0, or an I/O error. */
static int
fsck_walk(
	struct fsck_volume *volume,
	uint32_t first,
	uint32_t *queue)
{
	uint32_t head;
	uint32_t tail;
	int error;

	/* Breadth first: a directory's parent is known before it is scanned. */
	head = 0;
	tail = 0;
	queue[tail++] = first;
	while (head < tail) {
		error = fsck_scan_directory(volume, queue[head], queue, &tail);
		if (error != 0)
			return error;
		head++;
	}

	/* Succeeded: the subtree is walked. */
	return 0;
}

/*
 * Scans one directory's record blocks.  A hole ends the directory there.
 * Returns 0, or an I/O error.
 */
static int
fsck_scan_directory(
	struct fsck_volume *volume,
	uint32_t directory,
	uint32_t *queue,
	uint32_t *tail)
{
	uint8_t *raw;
	uint64_t size;
	uint64_t offset;
	uint64_t within;
	uint64_t fragment;
	int dirty;
	int error;

	/* The directory's length in whole record blocks. */
	raw = fsck_inode(volume, directory);
	if (raw == NULL)
		return volume->io_error;
	size = fsck_get64(volume, raw, UFS_DI_SIZE);
	size -= size % UFS_DIRBLKSIZ;

	/* Each block of the directory, and each record block in it. */
	for (offset = 0; offset < size; offset += volume->super.bsize) {
		fragment = fsck_file_block(volume, directory, offset / volume->super.bsize);
		if (fragment == UINT64_MAX)
			return EIO;

		/* A missing first block: the directory names nothing. */
		if (fragment == 0 && offset == 0) {
			fsck_problem(volume, 0, "directory %u has no first block", directory);
			return 0;
		}

		/* A hole later: the directory ends before it. */
		if (fragment == 0) {
			fsck_problem(volume, 1, "directory %u has a hole at byte %llu; it is shortened to it", directory, (unsigned long long)offset);
			raw = fsck_inode(volume, directory);
			if (raw == NULL)
				return volume->io_error;
			if (volume->repair) {
				fsck_put64(volume, raw, UFS_DI_SIZE, offset);
				fsck_inode_dirty(volume);
			}

			/* Nothing past the hole is scanned. */
			return 0;
		}

		/* The block's record blocks. */
		error = fsck_read_block(volume, fragment, volume->data);
		if (error != 0)
			return error;
		dirty = 0;
		for (within = 0; within < volume->super.bsize && offset + within < size; within += UFS_DIRBLKSIZ)
			fsck_scan_chunk(volume, directory, volume->data + within, offset + within == 0, &dirty, queue, tail);

		/* The block back when an entry was repaired. */
		if (dirty) {
			error = fsck_write_block(volume, fragment, volume->data);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the directory is scanned. */
	return 0;
}

/*
 * Scans one record block: the dot entries when it is the first, then each
 * entry.  Directories it names that are not yet reached join the queue.
 */
static void
fsck_scan_chunk(
	struct fsck_volume *volume,
	uint32_t directory,
	uint8_t *chunk,
	int first,
	int *dirty,
	uint32_t *queue,
	uint32_t *tail)
{
	uint32_t pos;
	uint32_t reclen;
	uint32_t record;
	int valid;

	/* Each record, in order. */
	pos = 0;
	record = 0;
	while (pos < UFS_DIRBLKSIZ) {
		/* A record that does not hold together ends the record block. */
		valid = fsck_record_valid(volume, chunk, pos);
		reclen = UFS_DIRBLKSIZ - pos;
		if (valid) {
			reclen = fsck_get16(volume, chunk, pos + FSCK_RECORD_RECLEN);
		} else {
			fsck_problem(volume, 1, "directory %u has a damaged entry at byte %u of a record block; the rest of the record block is emptied", directory, pos);
			if (volume->repair) {
				fsck_put_record(volume, chunk, pos, 0, reclen, 0, "");
				*dirty = 1;
			}
		}

		/* The first two records of the first record block are "." and "..". */
		if (first && record == 0U)
			reclen = fsck_dot_entry(volume, directory, chunk, pos, reclen, ".", directory, dirty);
		else if (first && record == 1U)
			reclen = fsck_dot_entry(volume, directory, chunk, pos, reclen, "..", volume->parent[directory], dirty);
		else if (valid)
			fsck_scan_entry(volume, directory, chunk, pos, dirty, queue, tail);

		/* The next record. */
		pos += reclen;
		record++;
	}

	/* The first record block holds both dot entries. */
	if (first && record < 2U)
		fsck_problem(volume, 0, "directory %u's first record block has no room for \"..\"", directory);
}

/*
 * Checks one entry after the dot entries: a dot entry anywhere else, a
 * free inode, or a directory's second name is removed; the entry's type
 * follows the inode's kind; the inode it names is counted and reached.
 */
static void
fsck_scan_entry(
	struct fsck_volume *volume,
	uint32_t directory,
	uint8_t *chunk,
	uint32_t pos,
	int *dirty,
	uint32_t *queue,
	uint32_t *tail)
{
	const char *name;
	uint32_t number;
	uint8_t namlen;
	uint8_t kind;
	uint8_t wanted;
	int dot;
	int dotdot;

	/* An empty record names nothing. */
	number = fsck_get32(volume, chunk, pos + FSCK_RECORD_INO);
	if (number == 0)
		return;
	namlen = chunk[pos + FSCK_RECORD_NAMLEN];
	name = (const char *)chunk + pos + FSCK_RECORD_NAME;

	/* A dot entry anywhere else, or a free or unknown inode. */
	kind = FSCK_INODE_FREE;
	if (number < volume->inode_count && number >= UFS_ROOT_INO)
		kind = volume->state[number] & FSCK_INODE_KIND_MASK;
	dot = fsck_record_named(chunk, pos, ".");
	dotdot = fsck_record_named(chunk, pos, "..");
	if (dot || dotdot || kind == FSCK_INODE_FREE) {
		fsck_problem(volume, 1, "directory %u has an entry \"%.*s\" naming inode %u, which it cannot name; it is removed", directory, namlen, name, number);
		if (volume->repair) {
			fsck_put32(volume, chunk, pos + FSCK_RECORD_INO, 0);
			*dirty = 1;
		}

		/* The entry names nothing from here on. */
		return;
	}

	/* A directory has one name; a second is removed. */
	if (kind == FSCK_INODE_DIR && (volume->state[number] & FSCK_INODE_REACHED) != 0) {
		fsck_problem(volume, 1, "directory %u has a second name \"%.*s\" in directory %u; it is removed", number, namlen, name, directory);
		if (volume->repair) {
			fsck_put32(volume, chunk, pos + FSCK_RECORD_INO, 0);
			*dirty = 1;
		}

		/* The entry names nothing from here on. */
		return;
	}

	/* The entry's type follows the inode's kind. */
	wanted = volume->dtype[number];
	if (chunk[pos + FSCK_RECORD_TYPE] != wanted) {
		fsck_problem(volume, 1, "directory %u's entry \"%.*s\" has type %u, but inode %u's kind is %u; corrected",
		    directory, namlen, name, chunk[pos + FSCK_RECORD_TYPE], number, wanted);
		if (volume->repair) {
			chunk[pos + FSCK_RECORD_TYPE] = wanted;
			*dirty = 1;
		}
	}

	/* Counted, and a directory joins the walk. */
	volume->references[number]++;
	if (kind == FSCK_INODE_DIR) {
		volume->parent[number] = directory;
		queue[(*tail)++] = number;
	}

	/* Succeeded: the inode is reached. */
	volume->state[number] |= FSCK_INODE_REACHED;
}

/*
 * Checks a dot entry: the record must be "." or ".." naming the inode
 * given; an empty record with room becomes one ("." leaves the rest of
 * the record empty, for ".." to follow).  The name it gives is counted.
 * Returns the record's length.
 */
static uint32_t
fsck_dot_entry(
	struct fsck_volume *volume,
	uint32_t directory,
	uint8_t *chunk,
	uint32_t pos,
	uint32_t reclen,
	const char *dots,
	uint32_t number,
	int *dirty)
{
	uint32_t found;
	uint32_t needed;
	uint32_t dot_bytes;
	uint32_t dotdot_bytes;
	uint8_t namlen;
	int named;

	/* What the record holds. */
	found = fsck_get32(volume, chunk, pos + FSCK_RECORD_INO);
	namlen = chunk[pos + FSCK_RECORD_NAMLEN];
	named = fsck_record_named(chunk, pos, dots);
	needed = FSCK_RECORD_BYTES(strlen(dots));

	/* The dot entry's name: only its inode and type can be wrong. */
	if (named && found != 0) {
		if (found != number) {
			fsck_problem(volume, 1, "directory %u's \"%s\" names inode %u instead of %u; corrected", directory, dots, found, number);
			if (volume->repair) {
				fsck_put32(volume, chunk, pos + FSCK_RECORD_INO, number);
				*dirty = 1;
			}
		}

		/* The type of a directory. */
		if (chunk[pos + FSCK_RECORD_TYPE] != FSCK_TYPE_DIR && volume->repair) {
			chunk[pos + FSCK_RECORD_TYPE] = FSCK_TYPE_DIR;
			*dirty = 1;
		}

		/* The name is counted. */
		volume->references[number]++;
		return reclen;
	}

	/* A record that is neither the dot entry nor empty with room. */
	if (found != 0 || reclen < needed) {
		fsck_problem(volume, 0, "directory %u's record for \"%s\" is \"%.*s\"", directory, dots, namlen, (const char *)chunk + pos + FSCK_RECORD_NAME);
		return reclen;
	}

	/* An empty record with room is made the dot entry. */
	fsck_problem(volume, 1, "directory %u has no \"%s\"; it is written", directory, dots);
	volume->references[number]++;
	if (!volume->repair)
		return reclen;

	/* "." leaves room for ".." after it. */
	*dirty = 1;
	dot_bytes = FSCK_RECORD_BYTES(1);
	dotdot_bytes = FSCK_RECORD_BYTES(2);
	if (needed == dot_bytes && reclen >= needed + dotdot_bytes) {
		fsck_put_record(volume, chunk, pos + needed, 0, reclen - needed, 0, "");
		reclen = needed;
	}

	/* Succeeded: the dot entry is written. */
	fsck_put_record(volume, chunk, pos, number, reclen, FSCK_TYPE_DIR, dots);
	return reclen;
}

/* Writes a record: header and name, the name padded with zeroes to its record bytes. */
static void
fsck_put_record(
	struct fsck_volume *volume,
	uint8_t *chunk,
	uint32_t pos,
	uint32_t number,
	uint32_t reclen,
	uint8_t type,
	const char *name)
{
	size_t length;

	/* The header, then the name. */
	length = strlen(name);
	fsck_put32(volume, chunk, pos + FSCK_RECORD_INO, number);
	fsck_put16(volume, chunk, pos + FSCK_RECORD_RECLEN, (uint16_t)reclen);
	chunk[pos + FSCK_RECORD_TYPE] = type;
	chunk[pos + FSCK_RECORD_NAMLEN] = (uint8_t)length;
	memset(chunk + pos + FSCK_RECORD_NAME, 0, reclen - FSCK_RECORD_NAME);
	memcpy(chunk + pos + FSCK_RECORD_NAME, name, length);
}

/* Reports whether the record at a position holds together inside its record block. */
static int
fsck_record_valid(
	const struct fsck_volume *volume,
	const uint8_t *chunk,
	uint32_t pos)
{
	uint32_t reclen;
	uint32_t number;
	uint8_t namlen;
	uint8_t index;
	uint8_t byte;

	/* Room for a header. */
	if (pos > UFS_DIRBLKSIZ - FSCK_RECORD_NAME)
		return 0;
	number = fsck_get32(volume, chunk, pos + FSCK_RECORD_INO);
	reclen = fsck_get16(volume, chunk, pos + FSCK_RECORD_RECLEN);
	namlen = chunk[pos + FSCK_RECORD_NAMLEN];

	/* A length in whole words inside the record block, holding the name. */
	if (reclen < FSCK_RECORD_NAME || (reclen % 4U) != 0 || pos + reclen > UFS_DIRBLKSIZ)
		return 0;
	if (FSCK_RECORD_NAME + (uint32_t)namlen > reclen)
		return 0;

	/* An empty record needs no name. */
	if (number == 0)
		return 1;

	/* A used record has a name without a slash or a zero byte. */
	if (namlen == 0)
		return 0;
	for (index = 0; index < namlen; index++) {
		byte = chunk[pos + FSCK_RECORD_NAME + index];
		if (byte == '/' || byte == '\0')
			return 0;
	}

	/* Succeeded: the record holds together. */
	return 1;
}

/* Reports whether a record's name is the one given. */
static int
fsck_record_named(
	const uint8_t *chunk,
	uint32_t pos,
	const char *name)
{
	size_t length;
	int differs;

	/* The length, then the bytes. */
	length = strlen(name);
	if (chunk[pos + FSCK_RECORD_NAMLEN] != length)
		return 0;
	differs = memcmp(chunk + pos + FSCK_RECORD_NAME, name, length);
	if (differs != 0)
		return 0;

	/* Succeeded: the same name. */
	return 1;
}

/* Finds /lost+found, or makes it: its inode, or 0 when it cannot be made. */
static uint32_t
fsck_lost_found(
	struct fsck_volume *volume)
{
	uint32_t number;

	/* Found or made before. */
	if (volume->lost_found != 0)
		return volume->lost_found;

	/* A directory named lost+found in the root. */
	number = fsck_find_entry(volume, UFS_ROOT_INO, "lost+found");
	if (number != 0 && number < volume->inode_count && (volume->state[number] & FSCK_INODE_KIND_MASK) == FSCK_INODE_DIR) {
		volume->lost_found = number;
		return number;
	}

	/* Otherwise a new one. */
	number = fsck_make_lost_found(volume);
	if (number == 0)
		return 0;

	/* Succeeded: the directory exists. */
	volume->lost_found = number;
	return number;
}

/*
 * Makes /lost+found: a directory of one record block in a new block,
 * owned by root with mode 0700, named in the root.  Returns its inode, or
 * 0 when it cannot be made.
 */
static uint32_t
fsck_make_lost_found(
	struct fsck_volume *volume)
{
	uint8_t *block;
	uint8_t *raw;
	uint64_t fragment;
	uint64_t now;
	uint32_t number;
	int error;

	/* A new inode and block. */
	number = fsck_allocate_inode(volume);
	fragment = fsck_allocate_block(volume);
	if (number == 0 || fragment == 0) {
		fprintf(stderr, "fsck: %s: no free inode or block for /lost+found\n", volume->name);
		return 0;
	}

	/* The block: "." and "..", the rest of the record block one record. */
	block = calloc(1, volume->super.bsize);
	if (block == NULL)
		return 0;
	fsck_put_record(volume, block, 0, number, FSCK_RECORD_BYTES(1), FSCK_TYPE_DIR, ".");
	fsck_put_record(volume, block, FSCK_RECORD_BYTES(1), UFS_ROOT_INO, UFS_DIRBLKSIZ - FSCK_RECORD_BYTES(1), FSCK_TYPE_DIR, "..");
	error = fsck_write_block(volume, fragment, block);
	free(block);
	if (error != 0) {
		volume->io_error = error;
		return 0;
	}

	/* The inode: a directory of one record block. */
	raw = fsck_inode(volume, number);
	if (raw == NULL)
		return 0;
	now = (uint64_t)time(NULL);
	memset(raw, 0, UFS_DINODE_SIZE);
	fsck_put16(volume, raw, UFS_DI_MODE, (uint16_t)(FSCK_IFDIR | 0700U));
	fsck_put16(volume, raw, UFS_DI_NLINK, 2);
	fsck_put32(volume, raw, UFS_DI_BLKSIZE, volume->super.bsize);
	fsck_put64(volume, raw, UFS_DI_SIZE, UFS_DIRBLKSIZ);
	fsck_put64(volume, raw, UFS_DI_BLOCKS, volume->super.bsize / UFS_SECTOR_SIZE);
	fsck_put64(volume, raw, UFS_DI_ATIME, now);
	fsck_put64(volume, raw, UFS_DI_MTIME, now);
	fsck_put64(volume, raw, UFS_DI_CTIME, now);
	fsck_put64(volume, raw, UFS_DI_BIRTHTIME, now);
	fsck_put32(volume, raw, UFS_DI_GEN, (uint32_t)now);
	fsck_put64(volume, raw, UFS_DI_DB, fragment);
	fsck_inode_dirty(volume);
	volume->state[number] = FSCK_INODE_DIR | FSCK_INODE_REACHED;
	volume->dtype[number] = FSCK_TYPE_DIR;
	volume->parent[number] = UFS_ROOT_INO;

	/* Its name in the root. */
	error = fsck_add_entry(volume, UFS_ROOT_INO, "lost+found", number, FSCK_TYPE_DIR);
	if (error != 0) {
		fprintf(stderr, "fsck: %s: the root directory has no room for lost+found\n", volume->name);
		fsck_clear_inode(volume, number);
		return 0;
	}

	/* The names it gives itself ("." and its entry) and the root (".."). */
	volume->references[number] += 2U;
	volume->references[UFS_ROOT_INO]++;
	printf("fsck: %s: /lost+found made (inode %u)\n", volume->name, number);

	/* Succeeded: the directory exists. */
	return number;
}

/* Finds a name in a directory: its inode, or 0. */
static uint32_t
fsck_find_entry(
	struct fsck_volume *volume,
	uint32_t directory,
	const char *name)
{
	uint8_t *raw;
	uint8_t *chunk;
	uint64_t size;
	uint64_t offset;
	uint64_t fragment;
	uint32_t pos;
	uint32_t reclen;
	uint32_t number;
	int valid;
	int named;
	int error;

	/* The directory's length. */
	raw = fsck_inode(volume, directory);
	if (raw == NULL)
		return 0;
	size = fsck_get64(volume, raw, UFS_DI_SIZE);

	/* Each record block, each record. */
	for (offset = 0; offset + UFS_DIRBLKSIZ <= size; offset += UFS_DIRBLKSIZ) {
		if ((offset % volume->super.bsize) == 0) {
			fragment = fsck_file_block(volume, directory, offset / volume->super.bsize);
			if (fragment == 0 || fragment == UINT64_MAX)
				return 0;
			error = fsck_read_block(volume, fragment, volume->data);
			if (error != 0)
				return 0;
		}

		/* The records of the record block. */
		chunk = volume->data + offset % volume->super.bsize;
		for (pos = 0; pos < UFS_DIRBLKSIZ; pos += reclen) {
			valid = fsck_record_valid(volume, chunk, pos);
			if (!valid)
				break;
			reclen = fsck_get16(volume, chunk, pos + FSCK_RECORD_RECLEN);
			number = fsck_get32(volume, chunk, pos + FSCK_RECORD_INO);
			named = fsck_record_named(chunk, pos, name);
			if (number != 0 && named)
				return number;
		}
	}

	/* Succeeded in learning the name is not there. */
	return 0;
}

/*
 * Adds a name to a directory: into the first record with room after its
 * own name, else into a new record block.  Returns 0, ENOSPC, or an I/O
 * error.
 */
static int
fsck_add_entry(
	struct fsck_volume *volume,
	uint32_t directory,
	const char *name,
	uint32_t number,
	uint8_t type)
{
	uint8_t *block;
	uint8_t *raw;
	uint64_t size;
	uint64_t offset;
	uint64_t fragment;
	uint32_t needed;
	int added;
	int error;

	/* A block to work in, and the directory's length. */
	block = malloc(volume->super.bsize);
	if (block == NULL)
		return ENOMEM;
	raw = fsck_inode(volume, directory);
	if (raw == NULL) {
		free(block);
		return volume->io_error;
	}

	/* A record with room after its own name, block by block. */
	size = fsck_get64(volume, raw, UFS_DI_SIZE);
	needed = FSCK_RECORD_BYTES(strlen(name));
	fragment = 0;
	for (offset = 0; offset + UFS_DIRBLKSIZ <= size; offset += UFS_DIRBLKSIZ) {
		if ((offset % volume->super.bsize) == 0) {
			fragment = fsck_file_block(volume, directory, offset / volume->super.bsize);
			if (fragment == 0 || fragment == UINT64_MAX)
				break;
			error = fsck_read_block(volume, fragment, block);
			if (error != 0) {
				free(block);
				return error;
			}
		}

		/* The record block's records. */
		added = fsck_add_in_record(volume, block + offset % volume->super.bsize, needed, name, number, type);
		if (added) {
			error = fsck_write_block(volume, fragment, block);
			free(block);
			return error;
		}
	}

	/* Otherwise a new record block. */
	error = fsck_add_record_block(volume, directory, block, name, number, type);
	free(block);
	if (error != 0)
		return error;

	/* Succeeded: the name is written. */
	return 0;
}

/* Puts a name into a record block's first record with room after its own name: nonzero when it fits. */
static int
fsck_add_in_record(
	struct fsck_volume *volume,
	uint8_t *chunk,
	uint32_t needed,
	const char *name,
	uint32_t number,
	uint8_t type)
{
	uint32_t pos;
	uint32_t reclen;
	uint32_t used;
	uint32_t holder;
	int valid;

	/* Each record. */
	for (pos = 0; pos < UFS_DIRBLKSIZ; pos += reclen) {
		valid = fsck_record_valid(volume, chunk, pos);
		if (!valid)
			return 0;
		reclen = fsck_get16(volume, chunk, pos + FSCK_RECORD_RECLEN);
		holder = fsck_get32(volume, chunk, pos + FSCK_RECORD_INO);
		used = 0;
		if (holder != 0)
			used = FSCK_RECORD_BYTES(chunk[pos + FSCK_RECORD_NAMLEN]);
		if (reclen - used < needed)
			continue;

		/* The record keeps its name and gives the rest to the new one. */
		if (used != 0)
			fsck_put16(volume, chunk, pos + FSCK_RECORD_RECLEN, (uint16_t)used);
		fsck_put_record(volume, chunk, pos + used, number, reclen - used, type, name);
		return 1;
	}

	/* Succeeded in learning that no record has room. */
	return 0;
}

/*
 * Adds a record block holding one name at the end of a directory: in its
 * last block when that has room, else in a new direct block.  block is a
 * block-sized buffer to work in.  Returns 0, ENOSPC, or an I/O error.
 */
static int
fsck_add_record_block(
	struct fsck_volume *volume,
	uint32_t directory,
	uint8_t *block,
	const char *name,
	uint32_t number,
	uint8_t type)
{
	uint8_t *raw;
	uint64_t size;
	uint64_t logical;
	uint64_t fragment;
	uint64_t blocks;
	int error;

	/* The directory's length, and the block its next record block goes in. */
	raw = fsck_inode(volume, directory);
	if (raw == NULL)
		return volume->io_error;
	size = fsck_get64(volume, raw, UFS_DI_SIZE);
	logical = size / volume->super.bsize;

	/* The last block when it has room. */
	if ((size % volume->super.bsize) != 0) {
		fragment = fsck_file_block(volume, directory, logical);
		if (fragment == 0 || fragment == UINT64_MAX)
			return EIO;
		error = fsck_read_block(volume, fragment, block);
		if (error != 0)
			return error;
	} else {
		/* A new direct block. */
		if (logical >= UFS_NDADDR)
			return ENOSPC;
		fragment = fsck_allocate_block(volume);
		if (fragment == 0)
			return ENOSPC;
		memset(block, 0, volume->super.bsize);
		raw = fsck_inode(volume, directory);
		if (raw == NULL)
			return volume->io_error;
		blocks = fsck_get64(volume, raw, UFS_DI_BLOCKS);
		fsck_put64(volume, raw, UFS_DI_DB + logical * 8U, fragment);
		fsck_put64(volume, raw, UFS_DI_BLOCKS, blocks + volume->super.bsize / UFS_SECTOR_SIZE);
		fsck_inode_dirty(volume);
	}

	/* The record block: one record holding the name. */
	fsck_put_record(volume, block, (uint32_t)(size % volume->super.bsize), number, UFS_DIRBLKSIZ, type, name);
	error = fsck_write_block(volume, fragment, block);
	if (error != 0)
		return error;

	/* The directory is a record block longer. */
	raw = fsck_inode(volume, directory);
	if (raw == NULL)
		return volume->io_error;
	fsck_put64(volume, raw, UFS_DI_SIZE, size + UFS_DIRBLKSIZ);
	fsck_inode_dirty(volume);

	/* Succeeded: the name is written. */
	return 0;
}

/* Takes the lowest whole block no inode owns, for fsck's own use: its fragment, or 0. */
static uint64_t
fsck_allocate_block(
	struct fsck_volume *volume)
{
	uint64_t start;
	uint32_t group;
	uint32_t local;
	uint32_t part;
	uint32_t fragments;
	int owned;
	int taken;

	/* Each group's data area, block by block. */
	for (group = 0; group < volume->super.ncg; group++) {
		start = fsck_group_start(volume, group);
		fragments = fsck_group_fragments(volume, group);
		local = (volume->super.dblkno + volume->super.frag - 1U) / volume->super.frag * volume->super.frag;
		for (; local + volume->super.frag <= fragments; local += volume->super.frag) {
			taken = 0;
			for (part = 0; part < volume->super.frag; part++) {
				owned = fsck_bit(volume->owned, start + local + part);
				if (owned)
					taken = 1;
			}

			/* A block none of whose fragments is owned is taken. */
			if (taken)
				continue;
			for (part = 0; part < volume->super.frag; part++)
				fsck_set_bit(volume->owned, start + local + part, 1);
			return start + local;
		}
	}

	/* Succeeded in learning the volume is full. */
	return 0;
}

/* Takes the lowest free inode above the root: its number, or 0. */
static uint32_t
fsck_allocate_inode(
	struct fsck_volume *volume)
{
	uint32_t number;

	/* Each inode after the root. */
	for (number = UFS_ROOT_INO + 1U; number < volume->inode_count; number++) {
		if ((volume->state[number] & FSCK_INODE_KIND_MASK) == FSCK_INODE_FREE)
			return number;
	}

	/* Succeeded in learning every inode is used. */
	return 0;
}

/* Reports the inode a directory's ".." names, or UINT32_MAX when it cannot be read. */
static uint32_t
fsck_dotdot(
	struct fsck_volume *volume,
	uint32_t directory)
{
	uint64_t fragment;
	uint32_t reclen;
	uint32_t number;
	int valid;
	int error;

	/* The first block's first record block. */
	fragment = fsck_file_block(volume, directory, 0);
	if (fragment == 0 || fragment == UINT64_MAX)
		return UINT32_MAX;
	error = fsck_read_block(volume, fragment, volume->data);
	if (error != 0)
		return UINT32_MAX;

	/* The second record, when the first holds together. */
	valid = fsck_record_valid(volume, volume->data, 0);
	if (!valid)
		return UINT32_MAX;
	reclen = fsck_get16(volume, volume->data, FSCK_RECORD_RECLEN);
	valid = fsck_record_valid(volume, volume->data, reclen);
	if (!valid)
		return UINT32_MAX;

	/* Succeeded: the parent it names. */
	number = fsck_get32(volume, volume->data, reclen + FSCK_RECORD_INO);
	return number;
}
