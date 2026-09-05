/*
 * Direct upper/lower overlay filesystem
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "kern/overlayfs.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/namecache.h"
#include "kern/namei.h"
#include "kern/pipe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#define OVERLAY_INODE_MAX 256U
#define OVERLAY_IDENTITY_MAX 128U
#define OVERLAY_METADATA_MAX 128U
#define OVERLAY_JOURNAL_BYTES (128U * 1024U)
#define OVERLAY_RECORD_BYTES 512U
#define OVERLAY_SLOT_SECTORS (OVERLAY_JOURNAL_BYTES / OVERLAY_RECORD_BYTES)
#define OVERLAY_PATH_RECORD_MAX 468U
#ifdef ZEDBSD_OVERLAY_CONTENT_HOST_TEST
/* Keep host-test functions independently discardable.  The kernel link still
 * collects the complete overlay implementation in high memory. */
#define OVERLAY_HIGH
#else
#define OVERLAY_HIGH __attribute__((section(".hightext")))
#endif

#define OVERLAY_META_WHITEOUT 0x01U
#define OVERLAY_META_OPAQUE 0x02U
#define OVERLAY_OP_ADD_WHITEOUT 1U
#define OVERLAY_OP_REMOVE_WHITEOUT 2U
#define OVERLAY_OP_SET_OPAQUE 3U
#define OVERLAY_OP_CLEAR_OPAQUE 4U

enum overlay_identity_state {
	OVERLAY_ID_FREE,
	OVERLAY_ID_ACTIVE,
	OVERLAY_ID_RETIRED,
};

struct overlay_identity {
	ino_t ino;
	uint8_t state;
	char path[ZEDBSD_PATH_MAX];
};

struct overlay_metadata {
	uint8_t used;
	uint8_t flags;
	uint64_t sequence;
	char path[ZEDBSD_PATH_MAX];
};

struct overlay_mount_state {
	struct path upper_root;
	struct path lower_root;
	unsigned flags;
	ino_t next_ino;
	struct overlay_identity identities[OVERLAY_IDENTITY_MAX];
	struct overlay_metadata metadata[OVERLAY_METADATA_MAX];
	struct file *journal[2];
	unsigned active_slot;
	unsigned next_sector;
	uint64_t epoch;
	uint64_t sequence;
	uint32_t journal_generation;
	uint16_t temp_counter;
	struct mutex copy_up_lock;
};

struct overlay_journal_view {
	int valid;
	uint64_t epoch;
	uint64_t sequence;
	unsigned next_sector;
	uint32_t digest;
	struct overlay_metadata metadata[OVERLAY_METADATA_MAX];
};

struct overlay_inode_info {
	struct path upper;
	struct path lower;
	unsigned identity_index;
	char path[ZEDBSD_PATH_MAX];
};

struct overlay_inode_slot {
	struct inode inode;
	struct overlay_inode_info info;
	uint8_t used;
};

struct overlay_file_info {
	struct file *real;
};

enum overlay_dir_phase {
	OVERLAY_DIR_UPPER,
	OVERLAY_DIR_LOWER,
	OVERLAY_DIR_DONE,
};

struct overlay_dir_cursor {
	enum overlay_dir_phase phase;
	struct file *active;
};

static struct overlay_inode_slot overlay_inodes[OVERLAY_INODE_MAX]
	__attribute__((section(".vfs_bss")));

static const struct inode_ops overlay_inode_ops;
static const struct filesystem_type overlay_filesystem_type;

static int
overlay_layers_supported(const struct overlay_mount_args *args)
{
	if (args == NULL)
		return 0;
	return !((args->upper.p_mount != NULL &&
	    args->upper.p_mount->m_type == &overlay_filesystem_type) ||
	    (args->lower.p_mount != NULL &&
	    args->lower.p_mount->m_type == &overlay_filesystem_type));
}
static const struct file_ops overlay_regular_ops;
static const struct file_ops overlay_directory_ops;
static void overlay_retire_inode(struct inode *) OVERLAY_HIGH;
static int overlay_directory_empty(struct inode *) OVERLAY_HIGH;
static int overlay_find_relative(struct mount *, const char *, struct inode **)
	OVERLAY_HIGH;

#ifndef ZEDBSD_OVERLAY_CONTENT_HOST_TEST
typedef char overlay_record_path_must_fit[
	(ZEDBSD_PATH_MAX - 1U <= OVERLAY_PATH_RECORD_MAX) ? 1 : -1];
#endif

static OVERLAY_HIGH uint16_t
overlay_get16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static OVERLAY_HIGH uint32_t
overlay_get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static OVERLAY_HIGH uint64_t
overlay_get64(const uint8_t *p)
{
	return (uint64_t)overlay_get32(p) | (uint64_t)overlay_get32(p + 4) << 32;
}

static OVERLAY_HIGH void
overlay_put16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}

static OVERLAY_HIGH void
overlay_put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}

static OVERLAY_HIGH void
overlay_put64(uint8_t *p, uint64_t value)
{
	overlay_put32(p, (uint32_t)value);
	overlay_put32(p + 4, (uint32_t)(value >> 32));
}

static OVERLAY_HIGH uint32_t
overlay_crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
	size_t i;
	while (length-- != 0) {
		crc ^= *data++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xedb88320U &
				(uint32_t)-(int32_t)(crc & 1U));
	}
	return crc;
}

static OVERLAY_HIGH uint32_t
overlay_record_crc(const uint8_t record[OVERLAY_RECORD_BYTES])
{
	return overlay_crc_update(0xffffffffU, record, 508U) ^ 0xffffffffU;
}

static OVERLAY_HIGH int
overlay_all_zero(const uint8_t *data, size_t length)
{
	while (length-- != 0)
		if (*data++ != 0)
			return 0;
	return 1;
}

static OVERLAY_HIGH const uint8_t *
overlay_id(const struct overlay_mount_state *state)
{
	static const uint8_t overlay_id[4] = { 'Z', 'O', 'V', 'L' };
	(void)state;
	return overlay_id;
}

static OVERLAY_HIGH int
overlay_metadata_find(const struct overlay_metadata entries[OVERLAY_METADATA_MAX],
		      const char *path)
{
	unsigned i;
	for (i = 0; i < OVERLAY_METADATA_MAX; i++)
		if (entries[i].used && !strcmp(entries[i].path, path))
			return (int)i;
	return -1;
}

static OVERLAY_HIGH int
overlay_metadata_apply(struct overlay_metadata entries[OVERLAY_METADATA_MAX],
		       const char *path, unsigned opcode, uint64_t sequence)
{
	int index = overlay_metadata_find(entries, path);
	unsigned i, bit;
	if (opcode == OVERLAY_OP_ADD_WHITEOUT ||
	    opcode == OVERLAY_OP_REMOVE_WHITEOUT)
		bit = OVERLAY_META_WHITEOUT;
	else if (opcode == OVERLAY_OP_SET_OPAQUE ||
		 opcode == OVERLAY_OP_CLEAR_OPAQUE)
		bit = OVERLAY_META_OPAQUE;
	else
		return EINVAL;
	if (index < 0) {
		if (opcode == OVERLAY_OP_REMOVE_WHITEOUT ||
		    opcode == OVERLAY_OP_CLEAR_OPAQUE)
			return 0;
		for (i = 0; i < OVERLAY_METADATA_MAX; i++)
			if (!entries[i].used) {
				index = (int)i;
				memset(&entries[i], 0, sizeof(entries[i]));
				entries[i].used = 1;
				strcpy(entries[i].path, path);
				break;
			}
		if (index < 0)
			return ENOSPC;
	}
	if (opcode == OVERLAY_OP_ADD_WHITEOUT || opcode == OVERLAY_OP_SET_OPAQUE)
		entries[index].flags |= (uint8_t)bit;
	else
		entries[index].flags &= (uint8_t)~bit;
	entries[index].sequence = sequence;
	if (entries[index].flags == 0)
		memset(&entries[index], 0, sizeof(entries[index]));
	return 0;
}

static OVERLAY_HIGH unsigned
overlay_metadata_flags(const struct overlay_mount_state *state,
		       const char *path)
{
	int index = overlay_metadata_find(state->metadata, path);
	return index >= 0 ? state->metadata[index].flags : 0;
}

static OVERLAY_HIGH uint32_t
overlay_metadata_digest(const struct overlay_metadata entries[OVERLAY_METADATA_MAX])
{
	uint32_t crc = 0xffffffffU;
	char previous[ZEDBSD_PATH_MAX];
	unsigned emitted = 0;
	previous[0] = '\0';
	for (;;) {
		int best = -1;
		unsigned i;
		for (i = 0; i < OVERLAY_METADATA_MAX; i++) {
			if (!entries[i].used ||
			    (emitted != 0 && strcmp(entries[i].path, previous) <= 0))
				continue;
			if (best < 0 || strcmp(entries[i].path,
			    entries[best].path) < 0)
				best = (int)i;
		}
		if (best < 0)
			break;
		crc = overlay_crc_update(crc, (const uint8_t *)entries[best].path,
			strlen(entries[best].path) + 1U);
		crc = overlay_crc_update(crc, &entries[best].flags, 1U);
		strcpy(previous, entries[best].path);
		emitted++;
	}
	return crc ^ 0xffffffffU;
}

static OVERLAY_HIGH int
overlay_read_record(struct file *file, unsigned sector,
		    uint8_t record[OVERLAY_RECORD_BYTES])
{
	ssize_t count;
	if (sector >= OVERLAY_SLOT_SECTORS)
		return EOVERFLOW;
	count = file_pread(file, record, OVERLAY_RECORD_BYTES,
		(off_t)(sector * OVERLAY_RECORD_BYTES));
	return count == OVERLAY_RECORD_BYTES ? 0 : count < 0 ? (int)-count : EIO;
}

static OVERLAY_HIGH int
overlay_record_valid(const uint8_t record[OVERLAY_RECORD_BYTES])
{
	return overlay_get32(record + 508) == overlay_record_crc(record);
}

static OVERLAY_HIGH int
overlay_snapshot_apply(struct overlay_journal_view *view,
		       const uint8_t record[OVERLAY_RECORD_BYTES],
		       const uint8_t id[4])
{
	uint32_t length = overlay_get32(record + 0x0c);
	unsigned flags = overlay_get16(record + 0x0a);
	uint64_t sequence = overlay_get64(record + 0x18);
	char path[ZEDBSD_PATH_MAX];
	unsigned i;
	if (memcmp(record, "ZOVLSNP\0", 8) || overlay_get16(record + 8) != 1 ||
	    (flags & ~(OVERLAY_META_WHITEOUT | OVERLAY_META_OPAQUE)) != 0 ||
	    flags == 0 || length == 0 || length >= ZEDBSD_PATH_MAX ||
	    length > OVERLAY_PATH_RECORD_MAX ||
	    overlay_get64(record + 0x10) != view->epoch || sequence == 0 ||
	    sequence > view->sequence || memcmp(record + 0x20, id, 4) ||
	    overlay_get32(record + 0x24) != 0 ||
	    !overlay_all_zero(record + 0x28 + length,
		508U - (0x28U + length)))
		return EINVAL;
	memcpy(path, record + 0x28, length);
	path[length] = '\0';
	if (strchr(path, '/') == path || overlay_metadata_find(view->metadata, path) >= 0)
		return EINVAL;
	for (i = 0; i < OVERLAY_METADATA_MAX; i++)
		if (!view->metadata[i].used) {
			view->metadata[i].used = 1;
			view->metadata[i].flags = (uint8_t)flags;
			view->metadata[i].sequence = sequence;
			strcpy(view->metadata[i].path, path);
			return 0;
		}
	return ENOSPC;
}

static OVERLAY_HIGH int
overlay_operation_apply(struct overlay_journal_view *view,
		        const uint8_t record[OVERLAY_RECORD_BYTES],
		        const uint8_t id[4])
{
	uint32_t length = overlay_get32(record + 0x0c);
	unsigned opcode = overlay_get16(record + 0x0a);
	uint64_t sequence = overlay_get64(record + 0x18);
	char path[ZEDBSD_PATH_MAX];
	if (memcmp(record, "ZOVLOP\0\0", 8) || overlay_get16(record + 8) != 1 ||
	    opcode < OVERLAY_OP_ADD_WHITEOUT ||
	    opcode > OVERLAY_OP_CLEAR_OPAQUE || length == 0 ||
	    length >= ZEDBSD_PATH_MAX || length > OVERLAY_PATH_RECORD_MAX ||
	    overlay_get64(record + 0x10) != view->epoch ||
	    sequence != view->sequence + 1U || memcmp(record + 0x20, id, 4) ||
	    overlay_get32(record + 0x24) != 0 ||
	    !overlay_all_zero(record + 0x28 + length,
		508U - (0x28U + length)))
		return EINVAL;
	memcpy(path, record + 0x28, length);
	path[length] = '\0';
	if (overlay_metadata_apply(view->metadata, path, opcode, sequence) != 0)
		return ENOSPC;
	view->sequence = sequence;
	return 0;
}

static OVERLAY_HIGH int
overlay_validate_slot(struct overlay_mount_state *state, unsigned slot,
		      struct overlay_journal_view *view)
{
	uint8_t record[OVERLAY_RECORD_BYTES];
	uint8_t commit[OVERLAY_RECORD_BYTES];
	const uint8_t *id = overlay_id(state);
	uint32_t snapshot_count, commit_sector, digest;
	uint64_t last_sequence;
	unsigned sector;
	int error;
	memset(view, 0, sizeof(*view));
	error = overlay_read_record(state->journal[slot], 0, record);
	if (error != 0)
		return error;
	if (!overlay_record_valid(record) || memcmp(record, "ZOVLSLT\0", 8) ||
	    overlay_get16(record + 8) != 1 || overlay_get16(record + 0x0a) != 48 ||
	    overlay_get32(record + 0x0c) != OVERLAY_RECORD_BYTES ||
	    memcmp(record + 0x10, id, 4) || overlay_get32(record + 0x14) != 0 ||
	    overlay_get64(record + 0x18) == 0 ||
	    !overlay_all_zero(record + 0x30, 508U - 0x30U))
		return 0;
	view->epoch = overlay_get64(record + 0x18);
	snapshot_count = overlay_get32(record + 0x20);
	commit_sector = overlay_get32(record + 0x24);
	last_sequence = overlay_get64(record + 0x28);
	if (snapshot_count > OVERLAY_METADATA_MAX ||
	    commit_sector != 1U + snapshot_count ||
	    commit_sector >= OVERLAY_SLOT_SECTORS)
		return 0;
	view->sequence = last_sequence;
	digest = overlay_crc_update(0xffffffffU, record, sizeof(record));
	for (sector = 1; sector <= snapshot_count; sector++) {
		error = overlay_read_record(state->journal[slot], sector, record);
		if (error != 0)
			return error;
		if (!overlay_record_valid(record) ||
		    overlay_snapshot_apply(view, record, id) != 0)
			return 0;
		digest = overlay_crc_update(digest, record, sizeof(record));
	}
	error = overlay_read_record(state->journal[slot], commit_sector, commit);
	if (error != 0)
		return error;
	if (!overlay_record_valid(commit) || memcmp(commit, "ZOVLCMT\0", 8) ||
	    overlay_get16(commit + 8) != 1 || overlay_get16(commit + 0x0a) != 0 ||
	    memcmp(commit + 0x0c, id, 4) ||
	    overlay_get64(commit + 0x10) != view->epoch ||
	    overlay_get32(commit + 0x18) != snapshot_count ||
	    overlay_get32(commit + 0x1c) != commit_sector ||
	    overlay_get64(commit + 0x20) != last_sequence ||
	    overlay_get32(commit + 0x28) != (digest ^ 0xffffffffU) ||
	    !overlay_all_zero(commit + 0x2c, 508U - 0x2cU))
		return 0;
	sector = commit_sector + 1U;
	while (sector < OVERLAY_SLOT_SECTORS) {
		error = overlay_read_record(state->journal[slot], sector, record);
		if (error != 0)
			return error;
		if (overlay_all_zero(record, sizeof(record)) ||
		    !overlay_record_valid(record) ||
		    overlay_operation_apply(view, record, id) != 0)
			break;
		sector++;
	}
	view->next_sector = sector;
	view->digest = overlay_metadata_digest(view->metadata);
	view->valid = 1;
	return 0;
}

static OVERLAY_HIGH int
overlay_open_journal(struct overlay_mount_state *state, unsigned slot)
{
	struct componentname component;
	struct inode *inode;
	struct path path;
	char name[7];
	int error, flags;
	strcpy(name, ".zovl0");
	name[5] = (char)('0' + slot);
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = 0;
	error = inode_lookup(state->upper_root.p_inode, &component, &inode);
	if (error != 0)
		return error;
	if (inode->i_type != INODE_REG || inode->i_size != OVERLAY_JOURNAL_BYTES) {
		inode_release(inode);
		return EINVAL;
	}
	path_init(&path);
	path_set(&path, state->upper_root.p_mount, inode);
	inode_release(inode);
	flags = state->flags == OVERLAY_READ_WRITE ? O_RDWR : O_RDONLY;
	error = file_open_resolved(&path, flags, &state->journal[slot]);
	path_release(&path);
	return error;
}

static OVERLAY_HIGH int
overlay_journal_load(struct overlay_mount_state *state)
{
	struct overlay_journal_view *views[2];
	unsigned chosen;
	int error, first_error, second_error;
	views[0] = kern_calloc(1, sizeof(*views[0]));
	views[1] = kern_calloc(1, sizeof(*views[1]));
	if (views[0] == NULL || views[1] == NULL) {
		error = ENOMEM;
		goto out;
	}
	error = overlay_open_journal(state, 0);
	if (error != 0)
		goto out;
	error = overlay_open_journal(state, 1);
	if (error != 0)
		goto out;
	first_error = overlay_validate_slot(state, 0, views[0]);
	second_error = overlay_validate_slot(state, 1, views[1]);
	if (first_error != 0 && second_error != 0) {
		error = first_error;
		goto out;
	}
	if (!views[0]->valid && !views[1]->valid) {
		error = EINVAL;
		goto out;
	}
	if (!views[0]->valid)
		chosen = 1;
	else if (!views[1]->valid)
		chosen = 0;
	else if (views[0]->epoch != views[1]->epoch)
		chosen = views[0]->epoch > views[1]->epoch ? 0 : 1;
	else if (views[0]->sequence != views[1]->sequence)
		chosen = views[0]->sequence > views[1]->sequence ? 0 : 1;
	else if (views[0]->digest != views[1]->digest) {
		error = EINVAL;
		goto out;
	} else
		chosen = 0;
	memcpy(state->metadata, views[chosen]->metadata,
	       sizeof(state->metadata));
	state->active_slot = chosen;
	state->epoch = views[chosen]->epoch;
	state->sequence = views[chosen]->sequence;
	state->next_sector = views[chosen]->next_sector;
	error = 0;
out:
	if (views[0] != NULL)
		kern_free(views[0]);
	if (views[1] != NULL)
		kern_free(views[1]);
	return error;
}

static OVERLAY_HIGH int
overlay_write_record(struct file *file, unsigned sector,
		     const uint8_t record[OVERLAY_RECORD_BYTES])
{
	ssize_t count;
	if (sector >= OVERLAY_SLOT_SECTORS)
		return ENOSPC;
	count = file_pwrite(file, record, OVERLAY_RECORD_BYTES,
		(off_t)(sector * OVERLAY_RECORD_BYTES));
	return count == OVERLAY_RECORD_BYTES ? 0 : count < 0 ? (int)-count : EIO;
}

static OVERLAY_HIGH unsigned
overlay_metadata_count(const struct overlay_metadata entries[OVERLAY_METADATA_MAX])
{
	unsigned i, count = 0;
	for (i = 0; i < OVERLAY_METADATA_MAX; i++)
		if (entries[i].used)
			count++;
	return count;
}

static OVERLAY_HIGH int
overlay_metadata_sorted_index(const struct overlay_metadata entries[OVERLAY_METADATA_MAX],
			      const char *after)
{
	int best = -1;
	unsigned i;
	for (i = 0; i < OVERLAY_METADATA_MAX; i++) {
		if (!entries[i].used || (after != NULL &&
		    strcmp(entries[i].path, after) <= 0))
			continue;
		if (best < 0 || strcmp(entries[i].path, entries[best].path) < 0)
			best = (int)i;
	}
	return best;
}

static OVERLAY_HIGH int
overlay_journal_compact(struct overlay_mount_state *state)
{
	uint8_t record[OVERLAY_RECORD_BYTES];
	uint32_t digest;
	uint64_t epoch;
	unsigned count, commit_sector, sector, target;
	char previous[ZEDBSD_PATH_MAX];
	int index, error;
	const uint8_t *id = overlay_id(state);
	if (state->epoch == UINT64_MAX)
		return ENOSPC;
	epoch = state->epoch + 1U;
	target = state->active_slot ^ 1U;
	count = overlay_metadata_count(state->metadata);
	commit_sector = 1U + count;
	if (commit_sector + 1U >= OVERLAY_SLOT_SECTORS)
		return ENOSPC;
	memset(record, 0, sizeof(record));
	memcpy(record, "ZOVLSLT\0", 8);
	overlay_put16(record + 8, 1);
	overlay_put16(record + 0x0a, 48);
	overlay_put32(record + 0x0c, OVERLAY_RECORD_BYTES);
	memcpy(record + 0x10, id, 4);
	overlay_put64(record + 0x18, epoch);
	overlay_put32(record + 0x20, count);
	overlay_put32(record + 0x24, commit_sector);
	overlay_put64(record + 0x28, state->sequence);
	overlay_put32(record + 508, overlay_record_crc(record));
	error = overlay_write_record(state->journal[target], 0, record);
	if (error != 0)
		return error;
	digest = overlay_crc_update(0xffffffffU, record, sizeof(record));
	previous[0] = '\0';
	for (sector = 1; sector <= count; sector++) {
		size_t length;
		index = overlay_metadata_sorted_index(state->metadata,
			sector == 1 ? NULL : previous);
		if (index < 0)
			return EIO;
		length = strlen(state->metadata[index].path);
		memset(record, 0, sizeof(record));
		memcpy(record, "ZOVLSNP\0", 8);
		overlay_put16(record + 8, 1);
		overlay_put16(record + 0x0a, state->metadata[index].flags);
		overlay_put32(record + 0x0c, (uint32_t)length);
		overlay_put64(record + 0x10, epoch);
		overlay_put64(record + 0x18,
			state->metadata[index].sequence);
		memcpy(record + 0x20, id, 4);
		memcpy(record + 0x28, state->metadata[index].path, length);
		overlay_put32(record + 508, overlay_record_crc(record));
		error = overlay_write_record(state->journal[target], sector, record);
		if (error != 0)
			return error;
		digest = overlay_crc_update(digest, record, sizeof(record));
		strcpy(previous, state->metadata[index].path);
	}
	memset(record, 0, sizeof(record));
	memcpy(record, "ZOVLCMT\0", 8);
	overlay_put16(record + 8, 1);
	memcpy(record + 0x0c, id, 4);
	overlay_put64(record + 0x10, epoch);
	overlay_put32(record + 0x18, count);
	overlay_put32(record + 0x1c, commit_sector);
	overlay_put64(record + 0x20, state->sequence);
	overlay_put32(record + 0x28, digest ^ 0xffffffffU);
	overlay_put32(record + 508, overlay_record_crc(record));
	error = overlay_write_record(state->journal[target], commit_sector, record);
	if (error == 0)
		error = file_fsync(state->journal[target]);
	if (error == 0)
		error = mount_sync(state->upper_root.p_mount);
	if (error != 0)
		return error;
	state->active_slot = target;
	state->epoch = epoch;
	state->next_sector = commit_sector + 1U;
	return 0;
}

static OVERLAY_HIGH int
overlay_journal_append(struct overlay_mount_state *state, unsigned opcode,
		       const char *path)
{
	uint8_t record[OVERLAY_RECORD_BYTES];
	uint64_t sequence;
	size_t length;
	int index, error;
	unsigned i;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	if (path == NULL || path[0] == '\0' || path[0] == '/')
		return EINVAL;
	length = strlen(path);
	if (length >= ZEDBSD_PATH_MAX || length > OVERLAY_PATH_RECORD_MAX)
		return ENAMETOOLONG;
	index = overlay_metadata_find(state->metadata, path);
	if (index < 0 && (opcode == OVERLAY_OP_ADD_WHITEOUT ||
	    opcode == OVERLAY_OP_SET_OPAQUE)) {
		for (i = 0; i < OVERLAY_METADATA_MAX; i++)
			if (!state->metadata[i].used)
				break;
		if (i == OVERLAY_METADATA_MAX)
			return ENOSPC;
	}
	if (state->sequence == UINT64_MAX)
		return ENOSPC;
	if (state->next_sector >= OVERLAY_SLOT_SECTORS) {
		error = overlay_journal_compact(state);
		if (error != 0)
			return error;
	}
	sequence = state->sequence + 1U;
	memset(record, 0, sizeof(record));
	memcpy(record, "ZOVLOP\0\0", 8);
	overlay_put16(record + 8, 1);
	overlay_put16(record + 0x0a, (uint16_t)opcode);
	overlay_put32(record + 0x0c, (uint32_t)length);
	overlay_put64(record + 0x10, state->epoch);
	overlay_put64(record + 0x18, sequence);
	memcpy(record + 0x20, overlay_id(state), 4);
	memcpy(record + 0x28, path, length);
	overlay_put32(record + 508, overlay_record_crc(record));
	error = overlay_write_record(state->journal[state->active_slot],
		state->next_sector, record);
	if (error == 0)
		error = file_fsync(state->journal[state->active_slot]);
	if (error != 0)
		return error;
	error = overlay_metadata_apply(state->metadata, path, opcode, sequence);
	if (error != 0)
		return error;
	state->sequence = sequence;
	state->next_sector++;
	state->journal_generation++;
	return 0;
}

static OVERLAY_HIGH struct overlay_inode_info *
overlay_info(const struct inode *inode)
{
	return inode != NULL ? inode->i_data : NULL;
}

static OVERLAY_HIGH int
overlay_slot_index(const struct inode *inode)
{
	unsigned i;
	for (i = 0; i < OVERLAY_INODE_MAX; i++)
		if (&overlay_inodes[i].inode == inode)
			return (int)i;
	return -1;
}

static OVERLAY_HIGH struct inode *
overlay_alloc_inode(struct mount *mountp)
{
	unsigned i;
	(void)mountp;
	for (i = 0; i < OVERLAY_INODE_MAX; i++)
		if (!overlay_inodes[i].used) {
			overlay_inodes[i].used = 1;
			memset(&overlay_inodes[i].info, 0,
			       sizeof(overlay_inodes[i].info));
			return &overlay_inodes[i].inode;
		}
	return NULL;
}

static OVERLAY_HIGH void
overlay_free_inode(struct inode *inode)
{
	int index = overlay_slot_index(inode);
	if (index >= 0)
		memset(&overlay_inodes[index], 0, sizeof(overlay_inodes[index]));
}

enum overlay_path_selection {
	OVERLAY_PATH_VISIBLE,
	OVERLAY_PATH_UPPER,
	OVERLAY_PATH_LOWER,
};

/* overlay_inode_info path members are mutable cache state.  The overlay
 * inode's ordinary lock is their publication lock; consumers take referenced
 * snapshots and never retain a pointer into the mutable pair. */
static OVERLAY_HIGH const struct path *
overlay_select_path_locked(const struct overlay_inode_info *info,
	enum overlay_path_selection selection)
{
	if (selection == OVERLAY_PATH_UPPER)
		return &info->upper;
	if (selection == OVERLAY_PATH_LOWER)
		return &info->lower;
	return info->upper.p_inode != NULL ? &info->upper : &info->lower;
}

static OVERLAY_HIGH int
overlay_path_snapshot(struct inode *inode,
	enum overlay_path_selection selection, struct path *result)
{
	struct overlay_inode_info *info;
	const struct path *selected;

	if (result == NULL)
		return EINVAL;
	path_init(result);
	info = overlay_info(inode);
	if (info == NULL)
		return EIO;
	mutex_lock(&inode->i_lock);
	selected = overlay_select_path_locked(info, selection);
	if (selected->p_inode != NULL)
		path_set(result, selected->p_mount, selected->p_inode);
	mutex_unlock(&inode->i_lock);
	return result->p_inode != NULL ? 0 : ENOENT;
}

static OVERLAY_HIGH int
overlay_info_snapshot(struct inode *inode, struct path *upper,
	struct path *lower, char relative[ZEDBSD_PATH_MAX])
{
	struct overlay_inode_info *info = overlay_info(inode);

	if (upper != NULL)
		path_init(upper);
	if (lower != NULL)
		path_init(lower);
	if (info == NULL)
		return EIO;
	mutex_lock(&inode->i_lock);
	if (upper != NULL && info->upper.p_inode != NULL)
		path_set(upper, info->upper.p_mount, info->upper.p_inode);
	if (lower != NULL && info->lower.p_inode != NULL)
		path_set(lower, info->lower.p_mount, info->lower.p_inode);
	if (relative != NULL)
		strcpy(relative, info->path);
	mutex_unlock(&inode->i_lock);
	return 0;
}

static OVERLAY_HIGH int
overlay_temporary_name(const char *name)
{
	unsigned i;
	if (name == NULL || strlen(name) != 10U || name[0] != 'o' ||
	    name[1] != 'v' || strcmp(name + 6, ".tmp"))
		return 0;
	for (i = 2; i < 6; i++)
		if (!((name[i] >= '0' && name[i] <= '9') ||
		      (name[i] >= 'a' && name[i] <= 'f')))
			return 0;
	return 1;
}

static OVERLAY_HIGH int
overlay_reserved_name(const char *name)
{
	return name != NULL && (!strcmp(name, ".zovl0") ||
	    !strcmp(name, ".zovl1") || overlay_temporary_name(name));
}

static OVERLAY_HIGH int
overlay_component_text(const struct componentname *component,
		       char name[NAME_MAX + 1U])
{
	if (component == NULL || component->cn_namelen == 0 ||
	    component->cn_namelen > NAME_MAX)
		return EINVAL;
	memcpy(name, component->cn_nameptr, component->cn_namelen);
	name[component->cn_namelen] = '\0';
	return 0;
}

static OVERLAY_HIGH int
overlay_join(const char *parent, const char *name,
	     char result[ZEDBSD_PATH_MAX])
{
	size_t parent_length = strlen(parent), name_length = strlen(name);
	if (name_length == 0 || strchr(name, '/') != NULL ||
	    parent_length + (parent_length != 0) + name_length >= ZEDBSD_PATH_MAX)
		return ENAMETOOLONG;
	memcpy(result, parent, parent_length);
	if (parent_length != 0)
		result[parent_length++] = '/';
	memcpy(result + parent_length, name, name_length + 1U);
	return 0;
}

static OVERLAY_HIGH int
overlay_identity_get(struct overlay_mount_state *state, const char *path,
		     unsigned *index_out, ino_t *ino_out, int *created_out)
{
	unsigned i, free_index = OVERLAY_IDENTITY_MAX;
	if (created_out != NULL)
		*created_out = 0;
	for (i = 0; i < OVERLAY_IDENTITY_MAX; i++) {
		if (state->identities[i].state == OVERLAY_ID_ACTIVE &&
		    !strcmp(state->identities[i].path, path)) {
			*index_out = i;
			*ino_out = state->identities[i].ino;
			return 0;
		}
		if (free_index == OVERLAY_IDENTITY_MAX &&
		    state->identities[i].state == OVERLAY_ID_FREE)
			free_index = i;
	}
	if (free_index == OVERLAY_IDENTITY_MAX || state->next_ino == 0)
		return ENOSPC;
	state->identities[free_index].state = OVERLAY_ID_ACTIVE;
	state->identities[free_index].ino = state->next_ino++;
	strcpy(state->identities[free_index].path, path);
	*index_out = free_index;
	*ino_out = state->identities[free_index].ino;
	if (created_out != NULL)
		*created_out = 1;
	return 0;
}

static OVERLAY_HIGH int
overlay_lookup_real(const struct path *directory,
		    const struct componentname *component, struct path *result)
{
	struct inode *inode;
	int error;
	path_init(result);
	if (directory->p_inode == NULL)
		return ENOENT;
	error = inode_lookup(directory->p_inode, component, &inode);
	if (error != 0)
		return error;
	path_set(result, directory->p_mount, inode);
	inode_release(inode);
	return 0;
}

static OVERLAY_HIGH void
overlay_refresh_locked(struct inode *inode)
{
	struct overlay_inode_info *info = overlay_info(inode);
	const struct inode *visible =
		overlay_select_path_locked(info, OVERLAY_PATH_VISIBLE)->p_inode;
	if (visible == NULL)
		return;
	inode->i_type = visible->i_type;
	inode->i_mode = visible->i_mode;
	inode->i_linkcount = visible->i_linkcount;
	inode->i_uid = visible->i_uid;
	inode->i_gid = visible->i_gid;
	inode->i_size = visible->i_size;
	inode->i_rdev = visible->i_rdev;
	inode->i_atime = visible->i_atime;
	inode->i_mtime = visible->i_mtime;
	inode->i_ctime = visible->i_ctime;
#ifndef ZEDBSD_OVERLAY_CONTENT_HOST_TEST
	inode->i_op = &overlay_inode_ops;
	inode->i_fop = inode->i_type == INODE_DIR ? &overlay_directory_ops :
		inode->i_type == INODE_REG ? &overlay_regular_ops :
		inode->i_type == INODE_FIFO ? &fifo_file_ops : NULL;
#endif
}

static OVERLAY_HIGH void
overlay_refresh(struct inode *inode)
{
	if (inode == NULL || overlay_info(inode) == NULL)
		return;
	mutex_lock(&inode->i_lock);
	overlay_refresh_locked(inode);
	mutex_unlock(&inode->i_lock);
}

static OVERLAY_HIGH int
overlay_make_inode(struct mount *mountp, const char *relative,
		   struct path *upper, struct path *lower, struct inode **result)
{
	struct overlay_mount_state *state = mountp->m_data;
	struct overlay_inode_info *info;
	struct inode *inode;
	unsigned identity;
	ino_t ino;
	int error, identity_created;
	error = overlay_identity_get(state, relative, &identity, &ino,
	    &identity_created);
	if (error != 0)
		return error;
	if (inode_get(mountp, ino, result) == 0) {
		info = overlay_info(*result);
		if (info == NULL) {
			inode_release(*result);
			*result = NULL;
			return EIO;
		}
		/* The cached inode owns its path pair.  Overlay mutations update it
		 * explicitly under i_lock; ordinary lookup must not release and replace
		 * those references while readers are taking snapshots.  Direct external
		 * mutation of the private upper/lower mounts is outside the contract. */
		overlay_refresh(*result);
		return 0;
	}
	inode = inode_alloc(mountp);
	if (inode == NULL) {
		if (identity_created)
			memset(&state->identities[identity], 0,
			    sizeof(state->identities[identity]));
		return ENOSPC;
	}
	{
		int slot = overlay_slot_index(inode);
		if (slot < 0) {
			inode_release(inode);
			if (identity_created)
				memset(&state->identities[identity], 0,
				    sizeof(state->identities[identity]));
			return EIO;
		}
		info = &overlay_inodes[slot].info;
	}
	path_init(&info->upper);
	path_init(&info->lower);
	if (upper != NULL && upper->p_inode != NULL)
		path_set(&info->upper, upper->p_mount, upper->p_inode);
	if (lower != NULL && lower->p_inode != NULL)
		path_set(&info->lower, lower->p_mount, lower->p_inode);
	info->identity_index = identity;
	strcpy(info->path, relative);
	inode->i_ino = ino;
	inode->i_data = info;
	overlay_refresh(inode);
	*result = inode;
	return 0;
}

static OVERLAY_HIGH int
overlay_lookup(struct inode *directory, const struct componentname *component,
	       struct inode **result)
{
	struct path upper_directory, lower_directory, upper, lower;
	char name[NAME_MAX + 1U], parent_path[ZEDBSD_PATH_MAX];
	char relative[ZEDBSD_PATH_MAX];
	int upper_error, lower_error, error;
	if (directory->i_type != INODE_DIR || overlay_info(directory) == NULL)
		return ENOTDIR;
	if (component->cn_namelen == 1U && component->cn_nameptr[0] == '.') {
		inode_ref(directory);
		*result = directory;
		return 0;
	}
	error = overlay_info_snapshot(directory, &upper_directory,
	    &lower_directory, parent_path);
	if (error != 0)
		return error;
	if (component->cn_namelen == 2U && component->cn_nameptr[0] == '.' &&
	    component->cn_nameptr[1] == '.') {
		char *slash;

		if (parent_path[0] == '\0') {
			inode_ref(directory->i_mount->m_root);
			*result = directory->i_mount->m_root;
			error = 0;
			goto out_directories;
		}
		slash = strrchr(parent_path, '/');
		if (slash == NULL)
			parent_path[0] = '\0';
		else
			*slash = '\0';
		error = overlay_find_relative(directory->i_mount, parent_path,
		    result);
		goto out_directories;
	}
	error = overlay_component_text(component, name);
	if (error != 0)
		goto out_directories;
	if (overlay_reserved_name(name)) {
		error = ENOENT;
		goto out_directories;
	}
	error = overlay_join(parent_path, name, relative);
	if (error != 0)
		goto out_directories;
	path_init(&upper);
	path_init(&lower);
	upper_error = overlay_lookup_real(&upper_directory, component, &upper);
	if (upper_error == 0)
		lower_error = (overlay_metadata_flags(
		    directory->i_mount->m_data, parent_path) &
		    OVERLAY_META_OPAQUE) != 0 ? ENOENT :
		    overlay_lookup_real(&lower_directory, component, &lower);
	else if ((overlay_metadata_flags(directory->i_mount->m_data,
		 relative) & OVERLAY_META_WHITEOUT) != 0 ||
		 (overlay_metadata_flags(directory->i_mount->m_data,
		 parent_path) & OVERLAY_META_OPAQUE) != 0)
		lower_error = ENOENT;
	else
		lower_error = overlay_lookup_real(&lower_directory, component, &lower);
	if (upper_error != 0 && upper_error != ENOENT)
		error = upper_error;
	else if (lower_error != 0 && lower_error != ENOENT)
		error = lower_error;
	else if (upper_error != 0 && lower_error != 0)
		error = ENOENT;
	else {
		/* A non-directory upper hides every lower object. */
		if (upper.p_inode != NULL && upper.p_inode->i_type != INODE_DIR)
			path_release(&lower);
		else if (upper.p_inode != NULL && lower.p_inode != NULL &&
			 lower.p_inode->i_type != INODE_DIR)
			path_release(&lower);
		error = overlay_make_inode(directory->i_mount, relative,
			upper.p_inode != NULL ? &upper : NULL,
			lower.p_inode != NULL ? &lower : NULL, result);
	}
	path_release(&upper);
	path_release(&lower);
	out_directories:
	path_release(&upper_directory);
	path_release(&lower_directory);
	return error;
}

static OVERLAY_HIGH int
overlay_getattr(struct inode *inode, struct stat *status)
{
	struct path visible;
	int error;

	if (status == NULL)
		return EINVAL;
	error = overlay_path_snapshot(inode, OVERLAY_PATH_VISIBLE, &visible);
	if (error != 0)
		return error == ENOENT ? EIO : error;
	error = inode_getattr(visible.p_inode, status);
	path_release(&visible);
	if (error == 0) {
		overlay_refresh(inode);
		status->st_ino = inode->i_ino;
		status->st_dev = 0;
	}
	return error;
}

static OVERLAY_HIGH int
overlay_find_relative(struct mount *mountp, const char *relative,
		      struct inode **result)
{
	struct inode *current, *next;
	const char *at = relative;
	if (mountp == NULL || relative == NULL || result == NULL)
		return EINVAL;
	current = mountp->m_root;
	inode_ref(current);
	if (*at == '\0') {
		*result = current;
		return 0;
	}
	while (*at != '\0') {
		struct componentname component;
		const char *end = strchr(at, '/');
		int error;
		component.cn_nameptr = at;
		component.cn_namelen = end != NULL ? (size_t)(end - at) : strlen(at);
		component.cn_flags = end == NULL ? COMPONENT_LAST : 0;
		error = inode_lookup(current, &component, &next);
		inode_release(current);
		if (error != 0)
			return error;
		current = next;
		if (end == NULL)
			break;
		at = end + 1;
	}
	*result = current;
	return 0;
}

static OVERLAY_HIGH int
overlay_split_path(const char *path, char parent[ZEDBSD_PATH_MAX],
		   struct componentname *name)
{
	const char *slash = strrchr(path, '/');
	if (path == NULL || path[0] == '\0' || name == NULL)
		return EINVAL;
	if (slash == NULL) {
		parent[0] = '\0';
		name->cn_nameptr = path;
	} else {
		size_t length = (size_t)(slash - path);
		memcpy(parent, path, length);
		parent[length] = '\0';
		name->cn_nameptr = slash + 1;
	}
	name->cn_namelen = strlen(name->cn_nameptr);
	name->cn_flags = COMPONENT_LAST;
	return name->cn_namelen != 0 ? 0 : EINVAL;
}

static OVERLAY_HIGH void
overlay_publish_upper(struct inode *inode, const struct path *upper,
	int clear_lower, const char *relative)
{
	struct overlay_inode_info *info = overlay_info(inode);
	struct path replacement, old_upper, old_lower;

	path_init(&replacement);
	path_init(&old_upper);
	path_init(&old_lower);
	if (info == NULL || upper == NULL || upper->p_inode == NULL)
		return;
	/* Take the replacement references before entering the publication lock;
	 * release displaced references only after readers can no longer select
	 * them. */
	path_set(&replacement, upper->p_mount, upper->p_inode);
	mutex_lock(&inode->i_lock);
	old_upper = info->upper;
	info->upper = replacement;
	path_init(&replacement);
	if (clear_lower) {
		old_lower = info->lower;
		path_init(&info->lower);
	}
	if (relative != NULL)
		strcpy(info->path, relative);
	overlay_refresh_locked(inode);
	mutex_unlock(&inode->i_lock);
	path_release(&old_upper);
	path_release(&old_lower);
	path_release(&replacement);
}

static OVERLAY_HIGH void
overlay_install_upper(struct inode *inode, const struct path *upper)
{
	overlay_publish_upper(inode, upper, 0, NULL);
}

#define OVERLAY_MATERIALIZATION_MAX ((ZEDBSD_PATH_MAX / 2U) + 1U)

struct overlay_materialization_entry {
	struct overlay_materialization_entry *next;
	struct inode *directory;
	struct path parent_upper;
	struct path created_upper;
	char name[NAME_MAX + 1U];
};

struct overlay_materialization_transaction {
	struct overlay_materialization_entry *created;
	unsigned count;
};

static OVERLAY_HIGH void
overlay_clear_upper_if(struct inode *inode, const struct path *expected)
{
	struct overlay_inode_info *info = overlay_info(inode);
	struct path removed;

	path_init(&removed);
	if (info == NULL || expected == NULL)
		return;
	mutex_lock(&inode->i_lock);
	if (info->upper.p_mount == expected->p_mount &&
	    info->upper.p_inode == expected->p_inode) {
		removed = info->upper;
		path_init(&info->upper);
		overlay_refresh_locked(inode);
	}
	mutex_unlock(&inode->i_lock);
	path_release(&removed);
}

/* A leaf operation may need to materialize several lower-only ancestors.
 * Keep those allocations provisional until the leaf commits.  On failure,
 * remove them from deepest to shallowest and preserve any entry whose rmdir
 * failed as the authoritative upper while quarantining the mount read-only. */
static OVERLAY_HIGH int
overlay_materialization_complete(struct overlay_mount_state *state,
	struct overlay_materialization_transaction *transaction, int error)
{
	struct overlay_materialization_entry *entry, *next;
	/* A prior leaf rollback may already have quarantined the mount.  Preserve
	 * that first cleanup errno while still attempting every ancestor cleanup. */
	int cleanup_error = state->flags == OVERLAY_READ_ONLY ? error : 0;
	int one_error;

	for (entry = transaction->created; entry != NULL; entry = next) {
		next = entry->next;
		if (error != 0) {
			struct componentname name;

			name.cn_nameptr = entry->name;
			name.cn_namelen = strlen(entry->name);
			name.cn_flags = COMPONENT_LAST;
			one_error = inode_rmdir(entry->parent_upper.p_inode, &name);
			if (one_error == 0)
				overlay_clear_upper_if(entry->directory,
				    &entry->created_upper);
			if (cleanup_error == 0)
				cleanup_error = one_error;
			one_error = mount_sync(entry->parent_upper.p_mount);
			if (cleanup_error == 0)
				cleanup_error = one_error;
		}
		path_release(&entry->parent_upper);
		path_release(&entry->created_upper);
		inode_release(entry->directory);
		kern_free(entry);
	}
	transaction->created = NULL;
	transaction->count = 0;
	if (cleanup_error != 0) {
		state->flags = OVERLAY_READ_ONLY;
		return cleanup_error;
	}
	return error;
}

static OVERLAY_HIGH int
overlay_ensure_upper_dir_tracked(struct inode *directory,
	struct overlay_materialization_transaction *transaction)
{
	struct overlay_mount_state *state;
	struct overlay_materialization_entry *pending = NULL;
	struct inode *parent, *created;
	struct inode_creation_request request;
	struct path upper, lower, parent_upper;
	struct componentname name;
	char relative[ZEDBSD_PATH_MAX], parent_path[ZEDBSD_PATH_MAX];
	int error, cleanup_error, sync_error;
	int created_new = 0;

	if (overlay_info(directory) == NULL || directory->i_type != INODE_DIR)
		return ENOTDIR;
	state = directory->i_mount->m_data;
	error = overlay_info_snapshot(directory, &upper, &lower, relative);
	if (error != 0)
		return error;
	if (upper.p_inode != NULL) {
		error = upper.p_inode->i_type == INODE_DIR ? 0 : ENOTDIR;
		goto out_paths;
	}
	if (directory == directory->i_mount->m_root)
		{ error = EIO; goto out_paths; }
	error = overlay_split_path(relative, parent_path, &name);
	if (error != 0)
		goto out_paths;
	error = overlay_find_relative(directory->i_mount, parent_path, &parent);
	if (error != 0)
		goto out_paths;
	error = overlay_ensure_upper_dir_tracked(parent, transaction);
	if (error == 0)
		error = inode_creation_request_preserve(
			lower.p_inode != NULL ? lower.p_inode : directory,
			&request);
	path_init(&parent_upper);
	if (error == 0)
		error = overlay_path_snapshot(parent, OVERLAY_PATH_UPPER,
		    &parent_upper);
	if (error == 0 && transaction != NULL) {
		if (transaction->count >= OVERLAY_MATERIALIZATION_MAX)
			error = ENAMETOOLONG;
		else {
			pending = kern_calloc(1, sizeof(*pending));
			if (pending == NULL)
				error = ENOMEM;
		}
	}
	if (error == 0)
		error = inode_mkdir(parent_upper.p_inode, &name,
			&request, &created);
	if (error == 0)
		created_new = 1;
	if (error == EEXIST) {
		if (pending != NULL) {
			kern_free(pending);
			pending = NULL;
		}
		error = inode_lookup(parent_upper.p_inode, &name,
			&created);
	}
	if (error == 0) {
		struct path created_path;

		path_init(&created_path);
		path_set(&created_path, parent_upper.p_mount, created);
		/* A newly materialized directory is not published to the overlay
		 * inode until its upper namespace entry is durable.  Otherwise a
		 * failed sync leaves a visible upper directory after returning an
		 * error.  An EEXIST lookup observes an already committed directory
		 * and needs no new durability transaction. */
		if (created_new)
			error = mount_sync(parent_upper.p_mount);
		if (error == 0) {
			overlay_install_upper(directory, &created_path);
			if (created_new && transaction != NULL) {
				pending->directory = directory;
				inode_ref(directory);
				path_init(&pending->parent_upper);
				path_set(&pending->parent_upper,
				    parent_upper.p_mount, parent_upper.p_inode);
				path_init(&pending->created_upper);
				path_set(&pending->created_upper,
				    created_path.p_mount, created_path.p_inode);
				memcpy(pending->name, name.cn_nameptr,
				    name.cn_namelen);
				pending->name[name.cn_namelen] = '\0';
				pending->next = transaction->created;
				transaction->created = pending;
				transaction->count++;
				pending = NULL;
			}
		} else if (created_new) {
			cleanup_error = inode_rmdir(parent_upper.p_inode, &name);
			/* A failed rmdir leaves the complete created directory in the
			 * upper namespace.  Keep it authoritative before quarantining the
			 * mount; a successful rmdir leaves no live entry to publish even if
			 * the following durability sync fails. */
			if (cleanup_error != 0)
				overlay_install_upper(directory, &created_path);
			sync_error = mount_sync(parent_upper.p_mount);
			if (cleanup_error == 0)
				cleanup_error = sync_error;
			if (cleanup_error != 0) {
				state->flags = OVERLAY_READ_ONLY;
				error = cleanup_error;
			}
		}
		path_release(&created_path);
		inode_release(created);
	}
	path_release(&parent_upper);
	inode_release(parent);
	out_paths:
	if (pending != NULL)
		kern_free(pending);
	path_release(&upper);
	path_release(&lower);
	return error;
}

static OVERLAY_HIGH int
overlay_ensure_upper_dir(struct inode *directory)
{
	struct overlay_materialization_transaction transaction = { NULL, 0U };
	struct overlay_mount_state *state = directory->i_mount->m_data;
	int error;

	error = overlay_ensure_upper_dir_tracked(directory, &transaction);
	return overlay_materialization_complete(state, &transaction, error);
}

static OVERLAY_HIGH void
overlay_temp_name(uint16_t number, char name[11])
{
	static const char hex[] = "0123456789abcdef";
	name[0] = 'o'; name[1] = 'v';
	name[2] = hex[(number >> 12) & 15U];
	name[3] = hex[(number >> 8) & 15U];
	name[4] = hex[(number >> 4) & 15U];
	name[5] = hex[number & 15U];
	name[6] = '.'; name[7] = 't'; name[8] = 'm'; name[9] = 'p';
	name[10] = '\0';
}

static OVERLAY_HIGH int
overlay_copy_up_regular(struct inode *inode)
{
	struct overlay_mount_state *state = inode->i_mount->m_data;
	struct overlay_materialization_transaction materialization = {
		NULL, 0U
	};
	struct inode *parent = NULL, *temp_inode = NULL;
	struct file *source = NULL, *destination = NULL;
	struct path upper, lower, parent_upper, temp_path, final_path;
	struct componentname final_name, temp_name_component;
	struct inode_creation_request request;
	char relative[ZEDBSD_PATH_MAX], parent_path[ZEDBSD_PATH_MAX];
	char temp_name[11];
	uint8_t *buffer = NULL;
	off_t offset = 0;
	int error = 0, cleanup_error;
	int renamed = 0, final_removed = 0, retain_materialization = 0;
	int entered_transaction = 0;
	unsigned attempts;

	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	/* Generic preparation has already committed the upper before taking
	 * i_io_lock. Inner metadata/truncate calls must not reacquire namespace.
	 * Callers without i_io still join the gate below: another namespace
	 * operation may have published provisional ancestors pending rollback. */
	if (mutex_owned(&inode->i_io_lock)) {
		error = overlay_path_snapshot(inode, OVERLAY_PATH_UPPER, &upper);
		if (error == 0)
			path_release(&upper);
		return error == ENOENT ? EDEADLK : error;
	}
	path_init(&upper);
	path_init(&lower);
	path_init(&parent_upper);
	path_init(&temp_path);
	path_init(&final_path);
	/* Ordinary namespace syscalls already own this gate.  A writable open or
	 * truncate may enter copy-up after path resolution, so join the same gate
	 * here to exclude create/unlink/rename of the final name. */
	if (!mutex_owned(inode->i_mount->m_vfs_transaction_lock)) {
		mount_vfs_transaction_enter(inode->i_mount);
		entered_transaction = 1;
	}
	mutex_lock(&state->copy_up_lock);
	/* A cleanup failure may have quarantined the mount while this caller
	 * waited for the namespace/copy-up gates.  The check above is only a fast
	 * path; revalidate the authoritative state before creating any upper
	 * object. */
	if (state->flags != OVERLAY_READ_WRITE) {
		error = EROFS;
		goto out;
	}
	/* The first waiter may have completed the copy while this caller slept. */
	error = overlay_info_snapshot(inode, &upper, &lower, relative);
	if (error != 0)
		goto out;
	if (upper.p_inode != NULL) {
		error = 0;
		goto out;
	}
	if (lower.p_inode == NULL || lower.p_inode->i_type != INODE_REG) {
		error = EINVAL;
		goto out;
	}
	error = overlay_split_path(relative, parent_path, &final_name);
	if (error == 0)
		error = overlay_find_relative(inode->i_mount, parent_path, &parent);
	if (error == 0)
		error = overlay_ensure_upper_dir_tracked(parent, &materialization);
	if (error == 0)
		error = overlay_path_snapshot(parent, OVERLAY_PATH_UPPER,
		    &parent_upper);
	if (error == 0)
		error = inode_creation_request_preserve(lower.p_inode,
		    &request);
	if (error != 0)
		goto out;
	for (attempts = 0; attempts < 65536U; attempts++) {
		overlay_temp_name(state->temp_counter++, temp_name);
		temp_name_component.cn_nameptr = temp_name;
		temp_name_component.cn_namelen = 10;
		temp_name_component.cn_flags = COMPONENT_LAST;
		error = inode_create(parent_upper.p_inode,
			&temp_name_component, &request, &temp_inode);
		if (error == 0)
			break;
		if (error != EEXIST)
			goto out;
	}
	if (temp_inode == NULL) { error = ENOSPC; goto out; }
	path_set(&temp_path, parent_upper.p_mount, temp_inode);
	error = file_open_resolved(&lower, O_RDONLY, &source);
	if (error == 0)
		error = file_open_resolved(&temp_path, O_RDWR, &destination);
	buffer = kern_malloc(4096U);
	if (error == 0 && buffer == NULL)
		error = ENOMEM;
	while (error == 0 && offset < lower.p_inode->i_size) {
		size_t wanted = (size_t)(lower.p_inode->i_size - offset);
		ssize_t count, written;
		if (wanted > 4096U) wanted = 4096U;
		count = file_pread(source, buffer, wanted, offset);
		if (count != (ssize_t)wanted) {
			error = count < 0 ? (int)-count : EIO;
			break;
		}
		written = file_pwrite(destination, buffer, wanted, offset);
		if (written != (ssize_t)wanted) {
			error = written < 0 ? (int)-written : ENOSPC;
			break;
		}
		offset += (off_t)wanted;
	}
	/* Writes may update timestamps and clear set-id bits.  Reapply the
	 * PRESERVE contract while the reserved temporary name is still hidden. */
	if (error == 0)
		error = inode_creation_prepare(
		    parent_upper.p_inode, temp_inode, &request);
	if (error == 0)
		error = file_fsync(destination);
	if (destination != NULL) { int close_error = file_close(destination); destination = NULL; if (error == 0) error = close_error; }
	if (source != NULL) { (void)file_close(source); source = NULL; }
	if (error == 0) {
		error = inode_rename(parent_upper.p_inode,
			&temp_name_component, parent_upper.p_inode,
			&final_name, 0);
		if (error == 0)
			renamed = 1;
	}
	if (renamed) {
		error = mount_sync(parent_upper.p_mount);
		if (error == 0) {
			path_set(&final_path, parent_upper.p_mount, temp_inode);
			overlay_install_upper(inode, &final_path);
		} else {
			int original_error = error;
			int sync_error;

			cleanup_error = inode_unlink(parent_upper.p_inode,
				&final_name);
			if (cleanup_error == 0)
				final_removed = 1;
			sync_error = mount_sync(parent_upper.p_mount);
			if (cleanup_error == 0)
				cleanup_error = sync_error;
			if (cleanup_error == 0) {
				error = original_error;
			} else {
				/* If unlink itself failed, the complete renamed upper is still
				 * authoritative.  Publish it before freezing so readers never
				 * select stale lower contents.  A removed-but-not-durable name
				 * remains unpublished in the live namespace. */
				if (!final_removed) {
					path_set(&final_path, parent_upper.p_mount,
					    temp_inode);
					overlay_install_upper(inode, &final_path);
					retain_materialization = 1;
				}
				state->flags = OVERLAY_READ_ONLY;
				error = cleanup_error;
			}
		}
	}
out:
	if (buffer != NULL) kern_free(buffer);
	if (destination != NULL) (void)file_close(destination);
	if (source != NULL) (void)file_close(source);
	if (!renamed && temp_inode != NULL && parent_upper.p_inode != NULL) {
		int sync_error;

		cleanup_error = inode_unlink(parent_upper.p_inode,
			&temp_name_component);
		sync_error = mount_sync(parent_upper.p_mount);
		if (cleanup_error == 0)
			cleanup_error = sync_error;
		if (cleanup_error != 0) {
			state->flags = OVERLAY_READ_ONLY;
			error = cleanup_error;
		}
	}
	if (retain_materialization)
		(void)overlay_materialization_complete(state, &materialization, 0);
	else
		error = overlay_materialization_complete(state, &materialization,
		    error);
	if (temp_inode != NULL) inode_release(temp_inode);
	if (parent != NULL) inode_release(parent);
	path_release(&upper);
	path_release(&lower);
	path_release(&parent_upper);
	path_release(&temp_path);
	path_release(&final_path);
	mutex_unlock(&state->copy_up_lock);
	if (entered_transaction)
		mount_vfs_transaction_leave(inode->i_mount);
	return error;
}

/* Reject an upper or visible-lower collision before changing the upper
 * namespace.  A whiteout deliberately makes its matching lower name
 * recreatable; an opaque parent makes all of its lower children invisible. */
static OVERLAY_HIGH int
overlay_new_preflight(struct inode *directory,
	const struct componentname *name, char text[NAME_MAX + 1U],
	char relative[ZEDBSD_PATH_MAX], struct inode **hidden_lower)
{
	struct overlay_mount_state *state = directory->i_mount->m_data;
	struct path upper, lower;
	struct inode *found = NULL;
	char parent_path[ZEDBSD_PATH_MAX];
	unsigned flags, parent_flags;
	int error;

	if (hidden_lower != NULL)
		*hidden_lower = NULL;
	if (overlay_info(directory) == NULL || directory->i_type != INODE_DIR)
		return ENOTDIR;
	error = overlay_info_snapshot(directory, &upper, &lower, parent_path);
	if (error != 0)
		return error;
	error = overlay_component_text(name, text);
	if (error != 0 || overlay_reserved_name(text))
		{ error = error != 0 ? error : EINVAL; goto out; }
	error = overlay_join(parent_path, text, relative);
	if (error != 0)
		goto out;
	if (upper.p_inode != NULL) {
		error = inode_lookup(upper.p_inode, name, &found);
		if (error == 0) {
			error = EEXIST;
			goto out;
		}
		if (error != ENOENT)
			goto out;
	}
	flags = overlay_metadata_flags(state, relative);
	parent_flags = overlay_metadata_flags(state, parent_path);
	if (lower.p_inode == NULL || (parent_flags & OVERLAY_META_OPAQUE) != 0) {
		error = 0;
		goto out;
	}
	error = inode_lookup(lower.p_inode, name, &found);
	if (error == ENOENT) {
		error = 0;
		goto out;
	}
	if (error != 0)
		goto out;
	if ((flags & OVERLAY_META_WHITEOUT) == 0) {
		error = EEXIST;
		goto out;
	}
	if (hidden_lower != NULL) {
		*hidden_lower = found;
		found = NULL;
	}
	error = 0;
out:
	if (found != NULL)
		inode_release(found);
	path_release(&upper);
	path_release(&lower);
	return error;
}

static OVERLAY_HIGH int
overlay_finish_new(struct inode *directory,
	const struct componentname *name, const char *relative,
	int directory_object, int opaque_added, struct inode **result)
{
	struct overlay_mount_state *state = directory->i_mount->m_data;
	struct path upper;
	int error, cleanup_error = 0, one_error, whiteout_removed = 0;

	error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER, &upper);
	if (error != 0)
		return error == ENOENT ? EIO : error;
	error = mount_sync(upper.p_mount);
	if (error == 0 && (overlay_metadata_flags(state, relative) &
	    OVERLAY_META_WHITEOUT) != 0) {
		error = overlay_journal_append(state,
		    OVERLAY_OP_REMOVE_WHITEOUT, relative);
		if (error == 0)
			whiteout_removed = 1;
	}
	if (error == 0) {
		namecache_remove(directory, name);
		error = overlay_lookup(directory, name, result);
	}
	if (error == 0) {
		path_release(&upper);
		return 0;
	}

	/* Try every rollback step even if an earlier one fails.  Restoring the
	 * whiteout first hides the new upper object while it is removed. */
	if (whiteout_removed) {
		one_error = overlay_journal_append(state,
		    OVERLAY_OP_ADD_WHITEOUT, relative);
		if (cleanup_error == 0)
			cleanup_error = one_error;
	}
	one_error = directory_object ?
	    inode_rmdir(upper.p_inode, name) :
	    inode_unlink(upper.p_inode, name);
	if (cleanup_error == 0)
		cleanup_error = one_error;
	if (opaque_added) {
		one_error = overlay_journal_append(state,
		    OVERLAY_OP_CLEAR_OPAQUE, relative);
		if (cleanup_error == 0)
			cleanup_error = one_error;
	}
	one_error = mount_sync(upper.p_mount);
	if (cleanup_error == 0)
		cleanup_error = one_error;
	if (cleanup_error != 0)
		state->flags = OVERLAY_READ_ONLY;
	namecache_remove(directory, name);
	if (result != NULL)
		*result = NULL;
	path_release(&upper);
	return cleanup_error != 0 ? cleanup_error : error;
}

static OVERLAY_HIGH int
overlay_create(struct inode *directory, const struct componentname *name,
	       const struct inode_creation_request *request,
	       struct inode **result)
{
	struct overlay_mount_state *state = directory->i_mount->m_data;
	struct overlay_materialization_transaction materialization = {
		NULL, 0U
	};
	struct path upper;
	struct inode *created = NULL;
	char text[NAME_MAX + 1U], relative[ZEDBSD_PATH_MAX];
	int error;
	if (request == NULL || request->type != INODE_REG || result == NULL)
		return EINVAL;
	path_init(&upper);
	*result = NULL;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_new_preflight(directory, name, text, relative, NULL);
	if (error == 0)
		error = overlay_ensure_upper_dir_tracked(directory,
		    &materialization);
	if (error == 0)
		error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER, &upper);
	if (error == 0)
		error = inode_create(upper.p_inode, name, request, &created);
	if (error == 0)
		inode_release(created);
	path_release(&upper);
	if (error == 0)
		error = overlay_finish_new(directory, name, relative, 0, 0,
		    result);
	return overlay_materialization_complete(state, &materialization, error);
}

static OVERLAY_HIGH int
overlay_mkdir(struct inode *directory, const struct componentname *name,
	      const struct inode_creation_request *request,
	      struct inode **result)
{
	struct overlay_mount_state *state = directory->i_mount->m_data;
	struct overlay_materialization_transaction materialization = {
		NULL, 0U
	};
	struct path upper;
	struct inode *created = NULL, *lower = NULL;
	char text[NAME_MAX + 1U], relative[ZEDBSD_PATH_MAX];
	unsigned metadata_flags;
	int error, opaque_added = 0, rollback_error;
	if (request == NULL || request->type != INODE_DIR || result == NULL)
		return EINVAL;
	path_init(&upper);
	*result = NULL;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_new_preflight(directory, name, text, relative, &lower);
	if (error != 0)
		goto out;
	error = overlay_ensure_upper_dir_tracked(directory, &materialization);
	if (error != 0)
		goto out;
	error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER, &upper);
	if (error != 0)
		goto out;
	metadata_flags = overlay_metadata_flags(state, relative);
	if (lower != NULL && lower->i_type == INODE_DIR &&
	    (metadata_flags & OVERLAY_META_WHITEOUT) != 0 &&
	    (metadata_flags & OVERLAY_META_OPAQUE) == 0) {
		error = overlay_journal_append(state, OVERLAY_OP_SET_OPAQUE, relative);
		if (error != 0)
			goto out;
		opaque_added = 1;
	}
	error = inode_mkdir(upper.p_inode, name, request, &created);
	if (error != 0)
		goto out;
	inode_release(created);
	created = NULL;
	error = overlay_finish_new(directory, name, relative, 1,
	    opaque_added, result);
	opaque_added = 0;
out:
	if (opaque_added) {
		rollback_error = overlay_journal_append(state,
		    OVERLAY_OP_CLEAR_OPAQUE, relative);
		if (rollback_error != 0) {
			state->flags = OVERLAY_READ_ONLY;
			error = rollback_error;
		}
	}
	if (created != NULL)
		inode_release(created);
	if (lower != NULL)
		inode_release(lower);
	path_release(&upper);
	return overlay_materialization_complete(state, &materialization, error);
}

static OVERLAY_HIGH void
overlay_special_clear(struct inode *inode, void *expected)
{
	if (inode == NULL)
		return;
	mutex_lock(&inode->i_lock);
	if (inode->i_special == expected)
		inode->i_special = NULL;
	mutex_unlock(&inode->i_lock);
}

static OVERLAY_HIGH int
overlay_special_transfer(struct inode *source, struct inode *destination,
	void *expected)
{
	int error = 0;

	mutex_lock(&source->i_lock);
	if (source->i_special != expected)
		error = EIO;
	else
		source->i_special = NULL;
	mutex_unlock(&source->i_lock);
	if (error != 0)
		return error;
	mutex_lock(&destination->i_lock);
	if (destination->i_special != NULL)
		error = EADDRINUSE;
	else
		destination->i_special = expected;
	mutex_unlock(&destination->i_lock);
	if (error != 0) {
		mutex_lock(&source->i_lock);
		if (source->i_special == NULL)
			source->i_special = expected;
		mutex_unlock(&source->i_lock);
	}
	return error;
}

/* Build a pathname socket under an overlay-reserved temporary name.  Its
 * endpoint is attached to the cached overlay inode before the upper rename,
 * so a successful lookup of the final name can never observe a half-bound
 * socket node. */
static OVERLAY_HIGH int
overlay_mknod_socket(struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request, const char *relative,
	struct inode **result)
{
	struct overlay_mount_state *state = directory->i_mount->m_data;
	struct componentname temporary;
	struct inode *created = NULL, *prepared = NULL;
	struct path parent_upper, temporary_path;
	char temporary_name[11];
	unsigned attempts;
	int error = 0, cleanup_error, sync_error, renamed = 0;

	path_init(&parent_upper);
	path_init(&temporary_path);
	error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER,
	    &parent_upper);
	if (error != 0)
		goto out_unlocked;
	mutex_lock(&state->copy_up_lock);
	for (attempts = 0; attempts < 65536U; attempts++) {
		overlay_temp_name(state->temp_counter++, temporary_name);
		temporary.cn_nameptr = temporary_name;
		temporary.cn_namelen = 10U;
		temporary.cn_flags = COMPONENT_LAST;
		error = inode_mknod(parent_upper.p_inode, &temporary, request,
		    &created);
		if (error == 0)
			break;
		if (error != EEXIST)
			goto out;
	}
	if (created == NULL) {
		error = ENOSPC;
		goto out;
	}
	path_set(&temporary_path, parent_upper.p_mount, created);
	error = overlay_make_inode(directory->i_mount, relative,
	    &temporary_path, NULL, &prepared);
	if (error == 0)
		error = overlay_special_transfer(created, prepared,
		    request->special);
	if (error == 0)
		error = inode_rename(parent_upper.p_inode, &temporary,
		    parent_upper.p_inode, name, 0);
	if (error == 0)
		renamed = 1;
	if (error == 0)
		error = overlay_finish_new(directory, name, relative, 0, 0,
		    result);
out:
	if (error != 0 && prepared != NULL) {
		overlay_special_clear(prepared, request->special);
		overlay_retire_inode(prepared);
	}
	if (!renamed && created != NULL) {
		/* The reserved temporary name is not visible through overlay lookup,
		 * but it is still persistent upper state.  Complete and sync its
		 * removal before reporting the original failure.  If cleanup cannot be
		 * made durable, its error is authoritative and the overlay is
		 * quarantined read-only. */
		cleanup_error = inode_unlink(parent_upper.p_inode, &temporary);
		sync_error = mount_sync(parent_upper.p_mount);
		if (cleanup_error == 0)
			cleanup_error = sync_error;
		if (cleanup_error != 0) {
			state->flags = OVERLAY_READ_ONLY;
			error = cleanup_error;
		}
	}
	if (prepared != NULL)
		inode_release(prepared);
	if (created != NULL) {
		overlay_special_clear(created, request->special);
		inode_release(created);
	}
	path_release(&temporary_path);
	mutex_unlock(&state->copy_up_lock);
	out_unlocked:
	path_release(&parent_upper);
	return error;
}

static OVERLAY_HIGH int
overlay_mknod(struct inode *directory, const struct componentname *name,
	      const struct inode_creation_request *request,
	      struct inode **result)
{
	struct overlay_mount_state *state = directory->i_mount->m_data;
	struct overlay_materialization_transaction materialization = {
		NULL, 0U
	};
	struct path upper;
	struct inode *created = NULL;
	char text[NAME_MAX + 1U], relative[ZEDBSD_PATH_MAX];
	int error;

	if (request == NULL || result == NULL)
		return EINVAL;
	path_init(&upper);
	if (request->type != INODE_SOCKET && request->type != INODE_FIFO &&
	    request->type != INODE_CHAR &&
	    request->type != INODE_BLOCK)
		return EOPNOTSUPP;
	*result = NULL;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_new_preflight(directory, name, text, relative, NULL);
	if (error == 0)
		error = overlay_ensure_upper_dir_tracked(directory,
		    &materialization);
	if (error == 0 && request->type == INODE_SOCKET)
		error = overlay_mknod_socket(directory, name, request, relative,
		    result);
	else if (error == 0)
		error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER, &upper);
	if (error == 0 && request->type != INODE_SOCKET)
		error = inode_mknod(upper.p_inode, name, request, &created);
	path_release(&upper);
	if (error == 0 && request->type != INODE_SOCKET) {
		inode_release(created);
		error = overlay_finish_new(directory, name, relative, 0, 0,
		    result);
	}
	return overlay_materialization_complete(state, &materialization, error);
}

static OVERLAY_HIGH int
overlay_symlink(struct inode *directory, const struct componentname *name,
	const char *target, const struct inode_creation_request *request,
	struct inode **result)
{
	struct overlay_mount_state *state = directory->i_mount->m_data;
	struct overlay_materialization_transaction materialization = {
		NULL, 0U
	};
	struct path upper;
	struct inode *created = NULL;
	char text[NAME_MAX + 1U], relative[ZEDBSD_PATH_MAX];
	int error;

	if (target == NULL || request == NULL ||
	    request->type != INODE_SYMLINK || result == NULL)
		return EINVAL;
	path_init(&upper);
	*result = NULL;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_new_preflight(directory, name, text, relative, NULL);
	if (error == 0)
		error = overlay_ensure_upper_dir_tracked(directory,
		    &materialization);
	if (error == 0)
		error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER, &upper);
	if (error == 0)
		error = inode_symlink(upper.p_inode, name, target, request,
		    &created);
	path_release(&upper);
	if (error == 0) {
		inode_release(created);
		error = overlay_finish_new(directory, name, relative, 0, 0,
		    result);
	}
	return overlay_materialization_complete(state, &materialization, error);
}

static OVERLAY_HIGH ssize_t
overlay_readlink(struct inode *inode, char *buffer, size_t capacity)
{
	struct path visible;
	ssize_t result;
	int error;

	error = overlay_path_snapshot(inode, OVERLAY_PATH_VISIBLE, &visible);
	if (error != 0)
		return -(ssize_t)(error == ENOENT ? EIO : error);
	result = inode_readlink(visible.p_inode, buffer, capacity);
	path_release(&visible);
	return result;
}

static OVERLAY_HIGH int
overlay_path_is_below(const char *path, const char *root)
{
	size_t length = strlen(root);
	return !strncmp(path, root, length) &&
		(path[length] == '\0' || path[length] == '/');
}

static OVERLAY_HIGH int
overlay_repath_preflight(struct overlay_mount_state *state,
			 const char *old_path, const char *new_path,
			 const struct inode *replaced)
{
	unsigned i, j;
	size_t old_length = strlen(old_path), new_length = strlen(new_path);
	char candidate[ZEDBSD_PATH_MAX];
	for (i = 0; i < OVERLAY_IDENTITY_MAX; i++) {
		const char *suffix;
		if (state->identities[i].state != OVERLAY_ID_ACTIVE ||
		    !overlay_path_is_below(state->identities[i].path, old_path))
			continue;
		suffix = state->identities[i].path + old_length;
		if (new_length + strlen(suffix) >= sizeof(candidate))
			return ENAMETOOLONG;
		strcpy(candidate, new_path);
		strcat(candidate, suffix);
		for (j = 0; j < OVERLAY_IDENTITY_MAX; j++) {
			if (state->identities[j].state != OVERLAY_ID_ACTIVE ||
			    overlay_path_is_below(state->identities[j].path, old_path) ||
			    strcmp(state->identities[j].path, candidate))
				continue;
			if (replaced == NULL || overlay_info(replaced)->identity_index != j)
				return EEXIST;
		}
	}
	return 0;
}

static OVERLAY_HIGH void
overlay_repath_commit(struct overlay_mount_state *state,
		      struct mount *mountp, const char *old_path,
		      const char *new_path)
{
	unsigned i;
	size_t old_length = strlen(old_path);
	char updated[ZEDBSD_PATH_MAX];
	for (i = 0; i < OVERLAY_IDENTITY_MAX; i++) {
		if (state->identities[i].state != OVERLAY_ID_ACTIVE ||
		    !overlay_path_is_below(state->identities[i].path, old_path))
			continue;
		strcpy(updated, new_path);
		strcat(updated, state->identities[i].path + old_length);
		strcpy(state->identities[i].path, updated);
	}
	for (i = 0; i < OVERLAY_INODE_MAX; i++) {
		struct overlay_inode_info *info;
		struct inode *inode;
		if (!overlay_inodes[i].used ||
		    overlay_inodes[i].inode.i_mount != mountp)
			continue;
		inode = &overlay_inodes[i].inode;
		info = &overlay_inodes[i].info;
		mutex_lock(&inode->i_lock);
		if (overlay_path_is_below(info->path, old_path)) {
			strcpy(updated, new_path);
			strcat(updated, info->path + old_length);
			strcpy(info->path, updated);
		}
		mutex_unlock(&inode->i_lock);
	}
}

static OVERLAY_HIGH int
overlay_rename(struct inode *old_directory,
	       const struct componentname *old_name,
	       struct inode *new_directory,
	       const struct componentname *new_name, unsigned flags)
{
	struct overlay_mount_state *state = old_directory->i_mount->m_data;
	struct overlay_inode_info *source_info;
	struct inode *source = NULL, *target = NULL;
	struct path old_parent_upper, new_parent_upper, old_parent_lower;
	struct path source_upper, source_lower, new_upper_path;
	char old_text[NAME_MAX + 1U], new_text[NAME_MAX + 1U];
	char old_parent_path[ZEDBSD_PATH_MAX], new_parent_path[ZEDBSD_PATH_MAX];
	char old_relative[ZEDBSD_PATH_MAX], new_relative[ZEDBSD_PATH_MAX];
	int error;
	unsigned identity;
	path_init(&old_parent_upper);
	path_init(&old_parent_lower);
	path_init(&new_parent_upper);
	path_init(&source_upper);
	path_init(&source_lower);
	path_init(&new_upper_path);
	if (flags != 0)
		return EINVAL;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_component_text(old_name, old_text);
	if (error == 0)
		error = overlay_component_text(new_name, new_text);
	if (error != 0 || overlay_reserved_name(old_text) ||
	    overlay_reserved_name(new_text))
		return error != 0 ? error : EINVAL;
	error = overlay_info_snapshot(old_directory, NULL, &old_parent_lower, old_parent_path);
	if (error == 0)
		error = overlay_info_snapshot(new_directory, NULL, NULL,
		    new_parent_path);
	if (error == 0)
		error = overlay_join(old_parent_path, old_text, old_relative);
	if (error == 0)
		error = overlay_join(new_parent_path, new_text, new_relative);
	if (error == 0)
		error = overlay_lookup(old_directory, old_name, &source);
	if (error != 0)
		goto out;
	source_info = overlay_info(source);
	error = overlay_info_snapshot(source, &source_upper, &source_lower, NULL);
	if (error != 0)
		goto out;
	/* The visible upper may omit its hidden lower. Renaming that upper must
	 * still whiteout the old backing name, just like unlink. */
	if (source_lower.p_inode == NULL && old_parent_lower.p_inode != NULL) {
		error = overlay_lookup_real(&old_parent_lower, old_name, &source_lower);
		if (error != 0 && error != ENOENT)
			goto out;
	}
	error = overlay_lookup(new_directory, new_name, &target);
	if (error == ENOENT)
		error = 0;
	else if (error != 0)
		goto out;
	if (source->i_type == INODE_DIR) {
		if (source_upper.p_inode == NULL || source_lower.p_inode != NULL) {
			error = EXDEV;
			goto out;
		}
		if (target != NULL && target->i_type != INODE_DIR) {
			error = ENOTDIR;
			goto out;
		}
		if (target != NULL &&
		    (error = overlay_directory_empty(target)) != 0)
			goto out;
		error = overlay_repath_preflight(state, old_relative,
			new_relative, target);
		if (error != 0)
			goto out;
	} else if (target != NULL && target->i_type == INODE_DIR) {
		error = EISDIR;
		goto out;
	} else if (source_upper.p_inode == NULL) {
		error = overlay_copy_up_regular(source);
		if (error != 0)
			goto out;
		path_release(&source_upper);
		path_release(&source_lower);
		error = overlay_info_snapshot(source, &source_upper, &source_lower,
		    NULL);
		if (error != 0)
			goto out;
	}
	error = overlay_ensure_upper_dir(new_directory);
	if (error != 0)
		goto out;
	error = overlay_path_snapshot(old_directory, OVERLAY_PATH_UPPER,
	    &old_parent_upper);
	if (error == 0)
		error = overlay_path_snapshot(new_directory, OVERLAY_PATH_UPPER,
		    &new_parent_upper);
	if (error != 0)
		goto out;
	if (source_lower.p_inode != NULL) {
		error = overlay_journal_append(state,
			OVERLAY_OP_ADD_WHITEOUT, old_relative);
		if (error != 0)
			goto out;
	}
	error = inode_rename(old_parent_upper.p_inode, old_name,
		new_parent_upper.p_inode, new_name, 0);
	if (error != 0)
		goto out;
	/* The backend rename is the namespace commit.  Mirror it in the cached
	 * overlay inode before durability work which may report a later error. */
	path_set(&new_upper_path, new_parent_upper.p_mount,
	    source_upper.p_inode);
	identity = source_info->identity_index;
	if (source->i_type == INODE_DIR)
		overlay_repath_commit(state, source->i_mount, old_relative,
			new_relative);
	else
		strcpy(state->identities[identity].path, new_relative);
	overlay_publish_upper(source, &new_upper_path, 1, new_relative);
	if (target != NULL && target != source)
		overlay_retire_inode(target);
	/* Reject delayed pre-rename lookups even when the following sync fails. */
	inode_dir_changed(old_directory);
	if (new_directory != old_directory)
		inode_dir_changed(new_directory);
	if (source->i_type == INODE_DIR && old_directory != new_directory)
		inode_dir_changed(source);
	namecache_remove(old_directory, old_name);
	namecache_remove(new_directory, new_name);
	error = mount_sync(new_parent_upper.p_mount);
	if (error != 0)
		goto out;
	if ((overlay_metadata_flags(state, new_relative) &
	    OVERLAY_META_WHITEOUT) != 0) {
		error = overlay_journal_append(state,
			OVERLAY_OP_REMOVE_WHITEOUT, new_relative);
		if (error != 0)
			goto out;
	}
	error = 0;
out:
	if (target != NULL) inode_release(target);
	if (source != NULL) inode_release(source);
	path_release(&old_parent_upper);
	path_release(&old_parent_lower);
	path_release(&new_parent_upper);
	path_release(&source_upper);
	path_release(&source_lower);
	path_release(&new_upper_path);
	return error;
}

static OVERLAY_HIGH void
overlay_retire_inode(struct inode *inode)
{
	struct overlay_mount_state *state = inode->i_mount->m_data;
	struct overlay_inode_info *info = overlay_info(inode);
	if (info->identity_index < OVERLAY_IDENTITY_MAX)
		state->identities[info->identity_index].state = OVERLAY_ID_RETIRED;
	inode->i_flags |= INODE_DEAD;
}

static OVERLAY_HIGH int
overlay_directory_empty(struct inode *inode)
{
	struct path path;
	struct file *file;
	struct dirent entry;
	int eof = 0, error;
	path_init(&path);
	path_set(&path, inode->i_mount, inode);
	error = file_open_resolved(&path, O_RDONLY | O_DIRECTORY, &file);
	path_release(&path);
	if (error != 0)
		return error;
	error = file_readdir(file, &entry, &eof);
	(void)file_close(file);
	return error != 0 ? error : eof ? 0 : ENOTEMPTY;
}

static OVERLAY_HIGH int
overlay_remove(struct inode *directory, const struct componentname *name,
	       int removing_directory)
{
	struct overlay_mount_state *state = directory->i_mount->m_data;
	struct path parent_upper, parent_lower, target_upper, target_lower;
	struct inode *target;
	char text[NAME_MAX + 1U], parent_path[ZEDBSD_PATH_MAX];
	char relative[ZEDBSD_PATH_MAX];
	int error;
	path_init(&parent_upper);
	path_init(&parent_lower);
	path_init(&target_upper);
	path_init(&target_lower);
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_component_text(name, text);
	if (error != 0 || overlay_reserved_name(text))
		return error != 0 ? error : EINVAL;
	error = overlay_info_snapshot(directory, &parent_upper, &parent_lower,
	    parent_path);
	if (error == 0)
		error = overlay_join(parent_path, text, relative);
	if (error == 0)
		error = overlay_lookup(directory, name, &target);
	if (error != 0) {
		path_release(&parent_upper);
		path_release(&parent_lower);
		return error;
	}
	error = overlay_info_snapshot(target, &target_upper, &target_lower, NULL);
	if (error != 0)
		goto out;
	if ((target->i_type == INODE_DIR) != removing_directory) {
		error = removing_directory ? ENOTDIR : EISDIR;
		goto out;
	}
	if (removing_directory && (error = overlay_directory_empty(target)) != 0)
		goto out;
	/* A regular upper hides its lower path in the visible inode. Check the
	 * backing directory too, or unlink would resurrect the hidden entry. */
	if (target_lower.p_inode == NULL && parent_lower.p_inode != NULL) {
		error = overlay_lookup_real(&parent_lower, name, &target_lower);
		if (error != 0 && error != ENOENT)
			goto out;
	}
	if (target_lower.p_inode != NULL) {
		error = overlay_journal_append(state,
			OVERLAY_OP_ADD_WHITEOUT, relative);
		if (error != 0)
			goto out;
	}
	if (target_upper.p_inode != NULL) {
		error = removing_directory ?
			inode_rmdir(parent_upper.p_inode, name) :
			inode_unlink(parent_upper.p_inode, name);
		if (error != 0)
			goto out;
	}
	/* Removal is committed in the live namespace even if durability fails.
	 * Publish invalidation before sync; generic callers only do it on success. */
	inode_dir_changed(directory);
	namecache_remove(directory, name);
	overlay_retire_inode(target);
	error = target_upper.p_inode != NULL ? mount_sync(parent_upper.p_mount) : 0;
out:
	path_release(&parent_lower);
	inode_release(target);
	path_release(&parent_upper);
	path_release(&target_upper);
	path_release(&target_lower);
	return error;
}

static OVERLAY_HIGH int
overlay_unlink(struct inode *directory, const struct componentname *name)
{
	return overlay_remove(directory, name, 0);
}

static OVERLAY_HIGH int
overlay_rmdir(struct inode *directory, const struct componentname *name)
{
	return overlay_remove(directory, name, 1);
}

static OVERLAY_HIGH int
overlay_truncate_upper(struct inode *inode,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	struct path upper;
	struct inode_truncate_request inner_request;
	struct inode_truncate_result inner_result;
	int error;

	if (request == NULL || result == NULL || overlay_info(inode) == NULL) {
		return EINVAL;
	}
	result->actual_size = inode->i_size;
	result->limit_exceeded = 0;
	inner_request = *request;
	/* A credential-less stacked mutation has already crossed a content
	 * boundary, so the authoritative inode must conservatively remove
	 * set-id state.  Credential-bearing UAPI calls retain normal rules. */
	if (inner_request.credential == NULL)
		inner_request.content_change = 1;
	error = overlay_path_snapshot(inode, OVERLAY_PATH_UPPER, &upper);
	if (error == 0)
		error = inode_truncate_transaction(upper.p_inode,
		    &inner_request, &inner_result);
	else
		memset(&inner_result, 0, sizeof(inner_result));
	result->limit_exceeded = inner_result.limit_exceeded;
	/* Refresh on every outcome.  The final inode may have cleared set-id or
	 * partially changed EOF before a later backend/durability error. */
	overlay_refresh(inode);
	result->actual_size = inode->i_size;
	if (error == 0)
		error = mount_sync(upper.p_mount);
	path_release(&upper);
	return error;
}

/* Prepare lower-only metadata/content before generic code takes i_io_lock.
 * Namespace mutations may hold that lock while reading parent attributes,
 * so materialization cannot acquire their gate from inside an I/O callback. */
static OVERLAY_HIGH int
overlay_prepare_mutation(struct inode *inode)
{
	struct overlay_mount_state *state = inode->i_mount->m_data;
	struct path upper;
	int error, entered;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	/* An already-owned I/O domain implies an outer preparation. Otherwise
	 * join even for an existing upper: it may belong to an in-flight ancestor
	 * materialization which can still roll back while holding namespace. */
	if (mutex_owned(&inode->i_io_lock)) {
		error = overlay_path_snapshot(inode, OVERLAY_PATH_UPPER, &upper);
		if (error == 0)
			path_release(&upper);
		return error == ENOENT ? EDEADLK : error;
	}
	entered = mount_vfs_transaction_join(inode->i_mount);
	if (state->flags != OVERLAY_READ_WRITE) {
		error = EROFS;
		goto out;
	}
	error = overlay_path_snapshot(inode, OVERLAY_PATH_UPPER, &upper);
	if (error == 0) {
		path_release(&upper);
	} else if (error == ENOENT) {
		if (inode->i_type == INODE_REG)
			error = overlay_copy_up_regular(inode);
		else if (inode->i_type == INODE_DIR)
			error = overlay_ensure_upper_dir(inode);
		else
			error = EOPNOTSUPP;
	}
out:
	if (entered)
		mount_vfs_transaction_leave(inode->i_mount);
	return error;
}

static OVERLAY_HIGH int
overlay_truncate_limited(struct inode *inode,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	struct overlay_mount_state *state = inode->i_mount->m_data;
	int error;

	if (request == NULL || result == NULL)
		return EINVAL;
	result->actual_size = inode->i_size;
	result->limit_exceeded = 0;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_copy_up_regular(inode);
	if (error != 0) {
		overlay_refresh(inode);
		result->actual_size = inode->i_size;
		return error;
	}
	return overlay_truncate_upper(inode, request, result);
}

static OVERLAY_HIGH int
overlay_truncate(struct inode *inode, off_t size)
{
	const struct inode_truncate_request request = {
		.size = size,
		.growth_limit = UINT64_MAX,
		.credential = NULL,
		.content_change = 1,
	};
	struct inode_truncate_result result;

	return overlay_truncate_limited(inode, &request, &result);
}

static OVERLAY_HIGH int
overlay_setattr(struct inode *inode, const struct stat *status, unsigned mask)
{
	struct overlay_mount_state *state = inode->i_mount->m_data;
	struct path upper;
	int error;
	path_init(&upper);
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	if (inode->i_type == INODE_REG)
		error = overlay_copy_up_regular(inode);
	else if (inode->i_type == INODE_DIR)
		error = overlay_ensure_upper_dir(inode);
	else
		error = EOPNOTSUPP;
	if (error == 0)
		error = overlay_path_snapshot(inode, OVERLAY_PATH_UPPER, &upper);
	if (error == 0)
		error = inode_setattr(upper.p_inode, status, mask);
	if (error == 0) {
		overlay_refresh(inode);
		error = mount_sync(upper.p_mount);
	}
	path_release(&upper);
	return error;
}

static OVERLAY_HIGH void
overlay_reclaim(struct inode *inode)
{
	struct overlay_mount_state *state = inode->i_mount != NULL ?
		inode->i_mount->m_data : NULL;
	struct overlay_inode_info *info = overlay_info(inode);
	if (info == NULL)
		return;
	path_release(&info->upper);
	path_release(&info->lower);
	if (state != NULL && info->identity_index < OVERLAY_IDENTITY_MAX &&
	    state->identities[info->identity_index].state == OVERLAY_ID_RETIRED)
		memset(&state->identities[info->identity_index], 0,
		       sizeof(state->identities[info->identity_index]));
}

static const struct inode_ops overlay_inode_ops = {
	.lookup = overlay_lookup,
	.create = overlay_create,
	.mkdir = overlay_mkdir,
	.mknod = overlay_mknod,
	.unlink = overlay_unlink,
	.rmdir = overlay_rmdir,
	.rename = overlay_rename,
	.symlink = overlay_symlink,
	.readlink = overlay_readlink,
	.getattr = overlay_getattr,
	.prepare_mutation = overlay_prepare_mutation,
	.setattr = overlay_setattr,
	.truncate = overlay_truncate,
	.truncate_limited = overlay_truncate_limited,
	.reclaim = overlay_reclaim,
};

static OVERLAY_HIGH int
overlay_regular_open(struct file *file)
{
	struct overlay_file_info *info;
	struct path visible;
	int error, real_flags;
	if (overlay_info(file->f_inode) == NULL)
		return EIO;
	if ((file_status_flags_get(file) & O_ACCMODE) != O_RDONLY) {
		error = overlay_copy_up_regular(file->f_inode);
		if (error != 0)
			return error;
	}
	error = overlay_path_snapshot(file->f_inode, OVERLAY_PATH_VISIBLE,
	    &visible);
	if (error != 0)
		return error == ENOENT ? EIO : error;
	info = kern_malloc(sizeof(*info));
	if (info == NULL) {
		path_release(&visible);
		return ENOMEM;
	}
	real_flags = file_status_flags_get(file) & ~(O_CREAT | O_EXCL | O_TRUNC);
	error = file_open_resolved(&visible, real_flags, &info->real);
	path_release(&visible);
	if (error != 0) {
		kern_free(info);
		return error;
	}
	file->f_data = info;
	file->f_vm_inode = file_vm_inode(info->real);
	return 0;
}

static OVERLAY_HIGH ssize_t
overlay_pread(struct file *file, void *buffer, size_t size, off_t offset)
{
	struct overlay_file_info *info = file->f_data;
	/* The outer file owns the shared-cache transaction for f_vm_inode.  The
	 * lower call is backend I/O within that transaction, not a second normal
	 * read which could wait on the outer CONTENT gate. */
	return info != NULL ? file_pread_internal(info->real, buffer, size,
	    offset, FILE_IO_VM_OBJECT) : -EIO;
}

static OVERLAY_HIGH ssize_t
overlay_pread_internal(struct file *file, void *buffer, size_t size,
	off_t offset, unsigned flags)
{
	struct overlay_file_info *info = file->f_data;
	return info != NULL ? file_pread_internal(info->real, buffer, size,
	    offset, flags) : -EIO;
}

static OVERLAY_HIGH ssize_t
overlay_read(struct file *file, void *buffer, size_t size)
{
	ssize_t count = overlay_pread(file, buffer, size, file->f_offset);
	if (count > 0)
		file->f_offset += count;
	return count;
}

static OVERLAY_HIGH ssize_t
overlay_pwrite(struct file *file, const void *buffer, size_t size, off_t offset)
{
	struct overlay_file_info *info = file->f_data;
	ssize_t count;
	if (info == NULL)
		return -EIO;
	count = file_pwrite_internal(info->real, buffer, size, offset,
	    FILE_IO_VM_OBJECT);
	/* The final inode may have cleared set-id before a later backend error.
	 * Always mirror that irreversible metadata transition to the visible inode. */
	overlay_refresh(file->f_inode);
	return count;
}

static OVERLAY_HIGH ssize_t
overlay_pwrite_internal(struct file *file, const void *buffer, size_t size,
	off_t offset, unsigned flags, const struct ucred *credential)
{
	struct overlay_file_info *info = file->f_data;
	ssize_t count;
	if (info == NULL)
		return -EIO;
	count = file_pwrite_internal_cred(info->real, buffer, size, offset,
	    flags | FILE_IO_VM_OBJECT, credential);
	overlay_refresh(file->f_inode);
	return count;
}

#ifdef ZEDBSD_OVERLAY_CONTENT_HOST_TEST
/* Exercise the real stacking callback without exposing overlay-private state
 * in a test ABI.  The caller owns both files for the duration of this call. */
static int
overlay_host_truncate_limited(struct inode *inode,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	return overlay_truncate_upper(inode, request, result);
}

ssize_t
overlay_content_host_pwrite(struct file *outer, struct file *real,
	const void *buffer, size_t size, off_t offset, unsigned flags,
	const struct ucred *credential)
{
	struct overlay_file_info file_info;
	struct overlay_inode_info inode_info;
	const struct inode_ops *saved_inode_ops;
	const struct file_ops *saved_file_ops;
	void *saved_file_data, *saved_inode_data;
	ssize_t count;

	if (outer == NULL || outer->f_inode == NULL || real == NULL ||
	    real->f_inode == NULL)
		return -EINVAL;
	memset(&file_info, 0, sizeof(file_info));
	memset(&inode_info, 0, sizeof(inode_info));
	file_info.real = real;
	inode_info.upper.p_inode = real->f_inode;
	saved_file_data = outer->f_data;
	saved_inode_data = outer->f_inode->i_data;
	saved_inode_ops = outer->f_inode->i_op;
	saved_file_ops = outer->f_inode->i_fop;
	outer->f_data = &file_info;
	outer->f_inode->i_data = &inode_info;
	count = overlay_pwrite_internal(outer, buffer, size, offset, flags,
	    credential);
	outer->f_data = saved_file_data;
	outer->f_inode->i_data = saved_inode_data;
	outer->f_inode->i_op = saved_inode_ops;
	outer->f_inode->i_fop = saved_file_ops;
	return count;
}

int
overlay_content_host_truncate(struct inode *outer, struct inode *real,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	struct overlay_mount_state state;
	struct overlay_inode_info inode_info;
	struct filesystem_type upper_type;
	struct mount outer_mount, upper_mount;
	struct inode_ops host_ops;
	const struct inode_ops *saved_ops;
	struct mount *saved_mount;
	void *saved_data;
	int error;

	if (outer == NULL || real == NULL || request == NULL || result == NULL)
		return EINVAL;
	memset(&state, 0, sizeof(state));
	memset(&inode_info, 0, sizeof(inode_info));
	memset(&upper_type, 0, sizeof(upper_type));
	memset(&outer_mount, 0, sizeof(outer_mount));
	memset(&upper_mount, 0, sizeof(upper_mount));
	memset(&host_ops, 0, sizeof(host_ops));
	state.flags = OVERLAY_READ_WRITE;
	outer_mount.m_data = &state;
	upper_mount.m_type = &upper_type;
	inode_info.upper.p_mount = &upper_mount;
	inode_info.upper.p_inode = real;
	host_ops.truncate_limited = overlay_host_truncate_limited;
	saved_ops = outer->i_op;
	saved_mount = outer->i_mount;
	saved_data = outer->i_data;
	outer->i_op = &host_ops;
	outer->i_mount = &outer_mount;
	outer->i_data = &inode_info;
	error = inode_truncate_transaction(outer, request, result);
	outer->i_op = saved_ops;
	outer->i_mount = saved_mount;
	outer->i_data = saved_data;
	return error;
}

int
overlay_content_host_layers_supported(int upper_overlay, int lower_overlay)
{
	struct mount upper, lower;
	struct overlay_mount_args args;

	memset(&upper, 0, sizeof(upper));
	memset(&lower, 0, sizeof(lower));
	memset(&args, 0, sizeof(args));
	if (upper_overlay)
		upper.m_type = &overlay_filesystem_type;
	if (lower_overlay)
		lower.m_type = &overlay_filesystem_type;
	args.upper.p_mount = &upper;
	args.lower.p_mount = &lower;
	return overlay_layers_supported(&args) ? 0 : EOPNOTSUPP;
}
#endif

static OVERLAY_HIGH ssize_t
overlay_write(struct file *file, const void *buffer, size_t size)
{
	off_t offset = (file_status_flags_get(file) & O_APPEND) != 0 ?
		file->f_inode->i_size : file->f_offset;
	ssize_t count = overlay_pwrite(file, buffer, size, offset);
	if (count > 0)
		file->f_offset = offset + count;
	return count;
}

static OVERLAY_HIGH int
overlay_regular_fsync(struct file *file)
{
	struct overlay_file_info *info = file->f_data;
	int error = info != NULL ? file_fsync(info->real) : EIO;
	if (error == 0)
		error = mount_sync(file->f_inode->i_mount);
	return error;
}

static OVERLAY_HIGH int
overlay_regular_close(struct file *file)
{
	struct overlay_file_info *info = file->f_data;
	int error = 0;
	if (info != NULL) {
		error = file_close(info->real);
		kern_free(info);
	}
	file->f_data = NULL;
	return error;
}

static const struct file_ops overlay_regular_ops = {
	.open = overlay_regular_open,
	.read = overlay_read,
	.write = overlay_write,
	.pread = overlay_pread,
	.pwrite = overlay_pwrite,
	.pread_internal = overlay_pread_internal,
	.pwrite_internal = overlay_pwrite_internal,
	.fsync = overlay_regular_fsync,
	.close = overlay_regular_close,
};

static OVERLAY_HIGH int
overlay_dir_open(struct file *file)
{
	struct overlay_dir_cursor *cursor = kern_calloc(1, sizeof(*cursor));
	if (cursor == NULL)
		return ENOMEM;
	cursor->phase = OVERLAY_DIR_UPPER;
	file->f_data = cursor;
	return 0;
}

static OVERLAY_HIGH void
overlay_dir_drop_active(struct overlay_dir_cursor *cursor)
{
	if (cursor->active != NULL)
		(void)file_close(cursor->active);
	cursor->active = NULL;
}

static OVERLAY_HIGH int
overlay_dir_open_phase(struct file *file, struct overlay_dir_cursor *cursor)
{
	struct path path;
	char relative[ZEDBSD_PATH_MAX];
	int error;

	error = overlay_info_snapshot(file->f_inode,
	    cursor->phase == OVERLAY_DIR_UPPER ? &path : NULL,
	    cursor->phase == OVERLAY_DIR_LOWER ? &path : NULL, relative);
	if (error != 0)
		return error;
	if (cursor->phase == OVERLAY_DIR_LOWER &&
	    (overlay_metadata_flags(file->f_inode->i_mount->m_data,
	     relative) & OVERLAY_META_OPAQUE) != 0)
		error = ENOENT;
	else if (path.p_inode == NULL || path.p_inode->i_type != INODE_DIR)
		error = ENOENT;
	else
		error = file_open_resolved(&path, O_RDONLY | O_DIRECTORY,
		    &cursor->active);
	path_release(&path);
	return error;
}

static OVERLAY_HIGH int
overlay_dir_upper_has(struct inode *directory, const char *name)
{
	struct componentname component;
	struct path upper;
	struct inode *found;
	int error;
	error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER, &upper);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return 1;
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = 0;
	error = inode_lookup(upper.p_inode, &component, &found);
	if (error == 0)
		inode_release(found);
	path_release(&upper);
	return error == 0;
}

static OVERLAY_HIGH int
overlay_dir_child_hidden(struct inode *directory, const char *name)
{
	char parent[ZEDBSD_PATH_MAX], relative[ZEDBSD_PATH_MAX];
	if (overlay_info_snapshot(directory, NULL, NULL, parent) != 0 ||
	    overlay_join(parent, name, relative) != 0)
		return 1;
	return (overlay_metadata_flags(directory->i_mount->m_data, relative) &
		OVERLAY_META_WHITEOUT) != 0;
}

static OVERLAY_HIGH int
overlay_dir_emit(struct file *file, const char *name, struct dirent *entry)
{
	struct componentname component;
	struct inode *child;
	int error;
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = 0;
	error = inode_lookup(file->f_inode, &component, &child);
	if (error != 0)
		return error;
	memset(entry, 0, sizeof(*entry));
	entry->d_ino = child->i_ino;
	entry->d_type = child->i_type;
	strncpy(entry->d_name, name, NAME_MAX);
	entry->d_name[NAME_MAX] = '\0';
	inode_release(child);
	return 0;
}

static OVERLAY_HIGH int
overlay_readdir(struct file *file, struct dirent *entry, int *eof)
{
	struct overlay_dir_cursor *cursor = file->f_data;
	if (cursor == NULL || overlay_info(file->f_inode) == NULL)
		return EIO;
	while (cursor->phase != OVERLAY_DIR_DONE) {
		struct dirent real_entry;
		int real_eof = 0, error;
		if (cursor->active == NULL) {
			error = overlay_dir_open_phase(file, cursor);
			if (error == ENOENT) {
				cursor->phase++;
				continue;
			}
			if (error != 0)
				return error;
		}
		error = file_readdir(cursor->active, &real_entry, &real_eof);
		if (error != 0)
			return error;
		if (real_eof) {
			overlay_dir_drop_active(cursor);
			cursor->phase++;
			continue;
		}
		if (!strcmp(real_entry.d_name, ".") ||
		    !strcmp(real_entry.d_name, "..") ||
		    overlay_reserved_name(real_entry.d_name) ||
		    (cursor->phase == OVERLAY_DIR_LOWER &&
		     (overlay_dir_upper_has(file->f_inode, real_entry.d_name) ||
		      overlay_dir_child_hidden(file->f_inode,
		       real_entry.d_name))))
			continue;
		error = overlay_dir_emit(file, real_entry.d_name, entry);
		if (error != 0)
			return error;
		file->f_offset++;
		*eof = 0;
		return 0;
	}
	*eof = 1;
	return 0;
}

static OVERLAY_HIGH off_t
overlay_dir_seek(struct file *file, off_t offset, int whence)
{
	struct overlay_dir_cursor *cursor = file->f_data;
	if (cursor == NULL || whence != 0 || offset != 0)
		return -EINVAL;
	overlay_dir_drop_active(cursor);
	cursor->phase = OVERLAY_DIR_UPPER;
	file->f_offset = 0;
	return 0;
}

static OVERLAY_HIGH int
overlay_dir_close(struct file *file)
{
	struct overlay_dir_cursor *cursor = file->f_data;
	if (cursor != NULL) {
		overlay_dir_drop_active(cursor);
		kern_free(cursor);
	}
	file->f_data = NULL;
	return 0;
}

static OVERLAY_HIGH int
overlay_directory_fsync(struct file *file)
{
	struct overlay_mount_state *state;
	struct path upper_path;
	struct file *upper = NULL;
	int error, close_error;

	if (file == NULL || file->f_inode == NULL)
		return EINVAL;
	state = file->f_inode->i_mount->m_data;
	if (state == NULL || overlay_info(file->f_inode) == NULL)
		return EIO;
	if (state->flags == OVERLAY_READ_ONLY)
		return 0;
	error = overlay_path_snapshot(file->f_inode, OVERLAY_PATH_UPPER,
	    &upper_path);
	if (error == 0) {
		error = file_open_resolved(&upper_path,
		    O_RDONLY | O_DIRECTORY, &upper);
		path_release(&upper_path);
		if (error != 0)
			return error;
		error = file_fsync(upper);
		close_error = file_close(upper);
		if (error != 0)
			return error;
		if (close_error != 0)
			return close_error;
	} else if (error != ENOENT)
		return error;
	if (state->journal[state->active_slot] == NULL)
		return EIO;
	error = file_fsync(state->journal[state->active_slot]);
	if (error != 0)
		return error;
	return mount_sync(state->upper_root.p_mount);
}

static const struct file_ops overlay_directory_ops = {
	.open = overlay_dir_open,
	.readdir = overlay_readdir,
	.seek = overlay_dir_seek,
	.fsync = overlay_directory_fsync,
	.close = overlay_dir_close,
};

static OVERLAY_HIGH int
overlay_cleanup_temps(struct path *directory, unsigned depth,
		      unsigned *visited, unsigned *deleted)
{
	struct componentname component;
	struct inode *child;
	struct path child_path;
	struct file *file;
	struct dirent entry;
	int eof, error;
	if (depth > 16U)
		return ELOOP;
	/* Deletion can change FAT directory offsets.  Delete one and restart. */
	for (;;) {
		error = file_open_resolved(directory, O_RDONLY | O_DIRECTORY, &file);
		if (error != 0)
			return error;
		eof = 0;
		while (!eof) {
			error = file_readdir(file, &entry, &eof);
			if (error != 0 || eof)
				break;
			if (!overlay_temporary_name(entry.d_name))
				continue;
			component.cn_nameptr = entry.d_name;
			component.cn_namelen = strlen(entry.d_name);
			component.cn_flags = COMPONENT_LAST;
			error = inode_lookup(directory->p_inode, &component, &child);
			if (error != 0)
				break;
			if (child->i_type == INODE_DIR) {
				inode_release(child);
				error = EINVAL;
				break;
			}
			inode_release(child);
			if (++*deleted > 256U) {
				error = EOVERFLOW;
				break;
			}
			error = inode_unlink(directory->p_inode, &component);
			break;
		}
		(void)file_close(file);
		if (error != 0)
			return error;
		if (eof)
			break;
	}
	/* With the current directory stable, recursively inspect children. */
	error = file_open_resolved(directory, O_RDONLY | O_DIRECTORY, &file);
	if (error != 0)
		return error;
	eof = 0;
	while (!eof) {
		error = file_readdir(file, &entry, &eof);
		if (error != 0 || eof)
			break;
		if (!strcmp(entry.d_name, ".") || !strcmp(entry.d_name, ".."))
			continue;
		if (++*visited > 512U) {
			error = EOVERFLOW;
			break;
		}
		component.cn_nameptr = entry.d_name;
		component.cn_namelen = strlen(entry.d_name);
		component.cn_flags = 0;
		error = inode_lookup(directory->p_inode, &component, &child);
		if (error != 0)
			break;
		if (child->i_type == INODE_DIR) {
			path_init(&child_path);
			path_set(&child_path, directory->p_mount, child);
			inode_release(child);
			error = overlay_cleanup_temps(&child_path, depth + 1U,
				visited, deleted);
			path_release(&child_path);
		} else {
			inode_release(child);
			error = 0;
		}
		if (error != 0)
			break;
	}
	(void)file_close(file);
	return error;
}

static OVERLAY_HIGH int
overlay_mount_impl(struct mount *mountp)
{
	const struct overlay_mount_args *args = mountp->m_data;
	struct overlay_mount_state *state;
	struct inode *root;
	unsigned visited = 0, deleted = 0;
	int error;
	if (args == NULL || args->upper.p_inode == NULL ||
	    args->lower.p_inode == NULL ||
	    args->upper.p_inode->i_type != INODE_DIR ||
	    args->lower.p_inode->i_type != INODE_DIR ||
	    (args->flags != OVERLAY_READ_ONLY &&
	     args->flags != OVERLAY_READ_WRITE))
		return EINVAL;
	/* The content-transaction lock chain currently has one visible wrapper
	 * and one authoritative inode.  A recursively stacked overlay would add
	 * a middle visible inode and introduce final->middle versus middle->final
	 * lock ordering.  Reject that unsupported topology explicitly rather than
	 * silently exposing stale metadata or an ABBA deadlock. */
	if (!overlay_layers_supported(args))
		return EOPNOTSUPP;
	state = kern_calloc(1, sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	(void)mutex_init(&state->copy_up_lock, LOCK_RANK_VFS_TRANSACTION,
	    "overlay copy-up");
	path_set(&state->upper_root, args->upper.p_mount, args->upper.p_inode);
	path_set(&state->lower_root, args->lower.p_mount, args->lower.p_inode);
	state->flags = args->flags;
	state->next_ino = 2;
	state->identities[0].state = OVERLAY_ID_ACTIVE;
	state->identities[0].ino = 1;
	state->identities[0].path[0] = '\0';
	mountp->m_data = state;
	error = overlay_journal_load(state);
	if (error != 0) {
		goto fail_state;
	}
	error = overlay_cleanup_temps(&state->upper_root, 0, &visited, &deleted);
	if (error == 0 && deleted != 0)
		error = mount_sync(state->upper_root.p_mount);
	if (error != 0) {
		goto fail_state;
	}
	error = overlay_make_inode(mountp, "", &state->upper_root,
		&state->lower_root, &root);
	if (error != 0) {
		goto fail_state;
	}
	root->i_flags |= INODE_ROOT;
	mountp->m_root = root;
	return 0;
fail_state:
	if (state->journal[0] != NULL)
		(void)file_close(state->journal[0]);
	if (state->journal[1] != NULL)
		(void)file_close(state->journal[1]);
	path_release(&state->lower_root);
	path_release(&state->upper_root);
	kern_free(state);
	mountp->m_data = NULL;
	return error;
}

static OVERLAY_HIGH int
overlay_sync_mount(struct mount *mountp)
{
	struct overlay_mount_state *state = mountp->m_data;
	int error;
	if (state == NULL || state->flags == OVERLAY_READ_ONLY)
		return 0;
	error = file_fsync(state->journal[state->active_slot]);
	if (error == 0)
		error = mount_sync(state->upper_root.p_mount);
	return error;
}

static OVERLAY_HIGH int
overlay_statvfs(struct mount *mountp, struct statvfs *result)
{
	struct overlay_mount_state *state = mountp != NULL ? mountp->m_data : NULL;
	if (state == NULL || result == NULL || state->upper_root.p_mount == NULL)
		return EINVAL;
	return mount_statvfs(state->upper_root.p_mount, result);
}

static OVERLAY_HIGH void
overlay_unmount_impl(struct mount *mountp)
{
	struct overlay_mount_state *state = mountp->m_data;
	if (state == NULL)
		return;
	if (state->journal[0] != NULL)
		(void)file_close(state->journal[0]);
	if (state->journal[1] != NULL)
		(void)file_close(state->journal[1]);
	path_release(&state->lower_root);
	path_release(&state->upper_root);
	kern_free(state);
	mountp->m_data = NULL;
}

static const struct filesystem_type overlay_filesystem_type = {
	.fs_name = "overlay",
	.fs_flags = FILESYSTEM_NODEV,
	.mount = overlay_mount_impl,
	.sync = overlay_sync_mount,
	.statvfs = overlay_statvfs,
	.unmount = overlay_unmount_impl,
	.alloc_inode = overlay_alloc_inode,
	.free_inode = overlay_free_inode,
};

OVERLAY_HIGH int
overlayfs_init(void)
{
	return filesystem_register(&overlay_filesystem_type);
}

OVERLAY_HIGH int
overlay_mount_at(struct mount *namespace_root, const char *target,
		 const struct overlay_mount_args *args, struct mount **result)
{
	struct path root;
	const char *name = target;
	int error;
	if (namespace_root == NULL || target == NULL || args == NULL)
		return EINVAL;
	if (name[0] == '/')
		name++;
	if (name[0] == '\0' || strchr(name, '/') != NULL)
		return EINVAL;
	path_init(&root);
	path_set(&root, namespace_root, namespace_root->m_root);
	error = mount_at("overlay", &root, name,
		args->flags == OVERLAY_READ_ONLY ? MOUNT_READ_ONLY : 0,
		(void *)args, result);
	path_release(&root);
	return error;
}
