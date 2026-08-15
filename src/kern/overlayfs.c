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
#define OVERLAY_HIGH __attribute__((section(".hightext")))

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
	struct path metadata_root;
	char journal_base[4];
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
static const struct file_ops overlay_regular_ops;
static const struct file_ops overlay_directory_ops;
static void overlay_retire_inode(struct inode *) OVERLAY_HIGH;
static int overlay_directory_empty(struct inode *) OVERLAY_HIGH;

typedef char overlay_record_path_must_fit[
	(ZEDBSD_PATH_MAX - 1U <= OVERLAY_PATH_RECORD_MAX) ? 1 : -1];

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
	static const uint8_t bin_id[4] = { 'B', 'I', 'N', 0 };
	static const uint8_t lib_id[4] = { 'L', 'I', 'B', 0 };
	return !strcmp(state->journal_base, "bin") ? bin_id : lib_id;
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
	char name[9];
	int error, flags;
	name[0] = state->journal_base[0];
	name[1] = state->journal_base[1];
	name[2] = state->journal_base[2];
	name[3] = (char)('0' + slot);
	strcpy(name + 4, ".log");
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = 0;
	error = inode_lookup(state->metadata_root.p_inode, &component, &inode);
	if (error != 0)
		return error;
	if (inode->i_type != INODE_REG || inode->i_size != OVERLAY_JOURNAL_BYTES) {
		inode_release(inode);
		return EINVAL;
	}
	path_init(&path);
	path_set(&path, state->metadata_root.p_mount, inode);
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
		error = mount_sync(state->metadata_root.p_mount);
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

static OVERLAY_HIGH const struct path *
overlay_visible(const struct overlay_inode_info *info)
{
	return info->upper.p_inode != NULL ? &info->upper : &info->lower;
}

static OVERLAY_HIGH int
overlay_reserved_name(const char *name)
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
		     unsigned *index_out, ino_t *ino_out)
{
	unsigned i, free_index = OVERLAY_IDENTITY_MAX;
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
overlay_refresh(struct inode *inode)
{
	struct overlay_inode_info *info = overlay_info(inode);
	const struct inode *visible = overlay_visible(info)->p_inode;
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
	inode->i_op = &overlay_inode_ops;
	inode->i_fop = inode->i_type == INODE_DIR ?
		&overlay_directory_ops : &overlay_regular_ops;
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
	int error;
	error = overlay_identity_get(state, relative, &identity, &ino);
	if (error != 0)
		return error;
	if (inode_get(mountp, ino, result) == 0)
		return 0;
	inode = inode_alloc(mountp);
	if (inode == NULL)
		return ENOSPC;
	{
		int slot = overlay_slot_index(inode);
		if (slot < 0) {
			inode_release(inode);
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
	struct overlay_inode_info *parent = overlay_info(directory);
	struct path upper, lower;
	char name[NAME_MAX + 1U], relative[ZEDBSD_PATH_MAX];
	int upper_error, lower_error, error;
	if (directory->i_type != INODE_DIR || parent == NULL)
		return ENOTDIR;
	error = overlay_component_text(component, name);
	if (error != 0)
		return error;
	if (overlay_reserved_name(name))
		return ENOENT;
	error = overlay_join(parent->path, name, relative);
	if (error != 0)
		return error;
	path_init(&upper);
	path_init(&lower);
	upper_error = overlay_lookup_real(&parent->upper, component, &upper);
	if (upper_error == 0)
		lower_error = (overlay_metadata_flags(
		    directory->i_mount->m_data, parent->path) &
		    OVERLAY_META_OPAQUE) != 0 ? ENOENT :
		    overlay_lookup_real(&parent->lower, component, &lower);
	else if ((overlay_metadata_flags(directory->i_mount->m_data,
		 relative) & OVERLAY_META_WHITEOUT) != 0 ||
		 (overlay_metadata_flags(directory->i_mount->m_data,
		 parent->path) & OVERLAY_META_OPAQUE) != 0)
		lower_error = ENOENT;
	else
		lower_error = overlay_lookup_real(&parent->lower, component, &lower);
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
	return error;
}

static OVERLAY_HIGH int
overlay_getattr(struct inode *inode, struct stat *status)
{
	struct overlay_inode_info *info = overlay_info(inode);
	int error;
	if (info == NULL || overlay_visible(info)->p_inode == NULL)
		return EIO;
	overlay_refresh(inode);
	error = inode_getattr(overlay_visible(info)->p_inode, status);
	if (error == 0) {
		status->st_ino = inode->i_ino;
		status->st_dev = 0;
		status->st_mode = inode->i_mode;
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
overlay_install_upper(struct inode *inode, const struct path *upper)
{
	struct overlay_inode_info *info = overlay_info(inode);
	path_release(&info->upper);
	path_set(&info->upper, upper->p_mount, upper->p_inode);
	overlay_refresh(inode);
}

static OVERLAY_HIGH int
overlay_ensure_upper_dir(struct inode *directory)
{
	struct overlay_inode_info *info = overlay_info(directory);
	struct inode *parent, *created;
	struct path upper;
	struct componentname name;
	char parent_path[ZEDBSD_PATH_MAX];
	int error;
	if (info == NULL || directory->i_type != INODE_DIR)
		return ENOTDIR;
	if (info->upper.p_inode != NULL)
		return info->upper.p_inode->i_type == INODE_DIR ? 0 : ENOTDIR;
	if (directory == directory->i_mount->m_root)
		return EIO;
	error = overlay_split_path(info->path, parent_path, &name);
	if (error != 0)
		return error;
	error = overlay_find_relative(directory->i_mount, parent_path, &parent);
	if (error != 0)
		return error;
	error = overlay_ensure_upper_dir(parent);
	if (error == 0)
		error = inode_mkdir(overlay_info(parent)->upper.p_inode, &name,
			info->lower.p_inode != NULL ? info->lower.p_inode->i_mode : 0755U,
			&created);
	if (error == EEXIST)
		error = inode_lookup(overlay_info(parent)->upper.p_inode, &name,
			&created);
	if (error == 0) {
		path_init(&upper);
		path_set(&upper, overlay_info(parent)->upper.p_mount, created);
		overlay_install_upper(directory, &upper);
		path_release(&upper);
		inode_release(created);
		error = mount_sync(info->upper.p_mount);
	}
	inode_release(parent);
	return error;
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
	struct overlay_inode_info *info = overlay_info(inode);
	struct inode *parent = NULL, *temp_inode = NULL, *final_inode = NULL;
	struct file *source = NULL, *destination = NULL;
	struct path temp_path, final_path;
	struct componentname final_name, temp_name_component;
	char parent_path[ZEDBSD_PATH_MAX], temp_name[11];
	uint8_t *buffer = NULL;
	off_t offset = 0;
	int error = 0, renamed = 0;
	unsigned attempts;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	if (info->upper.p_inode != NULL)
		return 0;
	if (info->lower.p_inode == NULL || info->lower.p_inode->i_type != INODE_REG)
		return EINVAL;
	path_init(&temp_path);
	path_init(&final_path);
	error = overlay_split_path(info->path, parent_path, &final_name);
	if (error == 0)
		error = overlay_find_relative(inode->i_mount, parent_path, &parent);
	if (error == 0)
		error = overlay_ensure_upper_dir(parent);
	if (error != 0)
		goto out;
	for (attempts = 0; attempts < 65536U; attempts++) {
		overlay_temp_name(state->temp_counter++, temp_name);
		temp_name_component.cn_nameptr = temp_name;
		temp_name_component.cn_namelen = 10;
		temp_name_component.cn_flags = COMPONENT_LAST;
		error = inode_create(overlay_info(parent)->upper.p_inode,
			&temp_name_component, info->lower.p_inode->i_mode, &temp_inode);
		if (error == 0)
			break;
		if (error != EEXIST)
			goto out;
	}
	if (temp_inode == NULL) { error = ENOSPC; goto out; }
	path_set(&temp_path, overlay_info(parent)->upper.p_mount, temp_inode);
	error = file_open_resolved(&info->lower, O_RDONLY, &source);
	if (error == 0)
		error = file_open_resolved(&temp_path, O_RDWR, &destination);
	buffer = kern_malloc(4096U);
	if (error == 0 && buffer == NULL)
		error = ENOMEM;
	while (error == 0 && offset < info->lower.p_inode->i_size) {
		size_t wanted = (size_t)(info->lower.p_inode->i_size - offset);
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
	if (error == 0)
		error = file_fsync(destination);
	if (destination != NULL) { int close_error = file_close(destination); destination = NULL; if (error == 0) error = close_error; }
	if (source != NULL) { (void)file_close(source); source = NULL; }
	if (error == 0) {
		error = inode_rename(overlay_info(parent)->upper.p_inode,
			&temp_name_component, overlay_info(parent)->upper.p_inode,
			&final_name, 0);
		if (error == 0) renamed = 1;
	}
	if (error == 0)
		error = mount_sync(overlay_info(parent)->upper.p_mount);
	if (error == 0)
		error = inode_lookup(overlay_info(parent)->upper.p_inode,
			&final_name, &final_inode);
	if (error == 0) {
		path_set(&final_path, overlay_info(parent)->upper.p_mount, final_inode);
		overlay_install_upper(inode, &final_path);
	}
out:
	if (buffer != NULL) kern_free(buffer);
	if (destination != NULL) (void)file_close(destination);
	if (source != NULL) (void)file_close(source);
	if (!renamed && temp_inode != NULL && parent != NULL)
		(void)inode_unlink(overlay_info(parent)->upper.p_inode,
			&temp_name_component);
	if (final_inode != NULL) inode_release(final_inode);
	if (temp_inode != NULL) inode_release(temp_inode);
	if (parent != NULL) inode_release(parent);
	path_release(&temp_path);
	path_release(&final_path);
	return error;
}

static OVERLAY_HIGH int
overlay_create(struct inode *directory, const struct componentname *name,
	       mode_t mode, struct inode **result)
{
	struct overlay_mount_state *state = directory->i_mount->m_data;
	struct overlay_inode_info *parent = overlay_info(directory);
	struct inode *created;
	char text[NAME_MAX + 1U], relative[ZEDBSD_PATH_MAX];
	int error;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_component_text(name, text);
	if (error != 0 || overlay_reserved_name(text))
		return error != 0 ? error : EINVAL;
	error = overlay_join(parent->path, text, relative);
	if (error == 0)
		error = overlay_ensure_upper_dir(directory);
	if (error == 0)
		error = inode_create(parent->upper.p_inode, name, mode, &created);
	if (error != 0)
		return error;
	inode_release(created);
	error = mount_sync(parent->upper.p_mount);
	if (error == 0 && (overlay_metadata_flags(state, relative) &
	    OVERLAY_META_WHITEOUT) != 0)
		error = overlay_journal_append(state,
			OVERLAY_OP_REMOVE_WHITEOUT, relative);
	if (error == 0) {
		namecache_remove(directory, name);
		error = overlay_lookup(directory, name, result);
	}
	return error;
}

static OVERLAY_HIGH int
overlay_mkdir(struct inode *directory, const struct componentname *name,
	      mode_t mode, struct inode **result)
{
	struct overlay_mount_state *state = directory->i_mount->m_data;
	struct overlay_inode_info *parent = overlay_info(directory);
	struct inode *created, *lower = NULL;
	char text[NAME_MAX + 1U], relative[ZEDBSD_PATH_MAX];
	int error;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_component_text(name, text);
	if (error != 0 || overlay_reserved_name(text))
		return error != 0 ? error : EINVAL;
	error = overlay_join(parent->path, text, relative);
	if (error != 0)
		return error;
	if (parent->lower.p_inode != NULL)
		(void)inode_lookup(parent->lower.p_inode, name, &lower);
	if (lower != NULL && lower->i_type == INODE_DIR &&
	    (overlay_metadata_flags(state, relative) & OVERLAY_META_WHITEOUT)) {
		error = overlay_journal_append(state, OVERLAY_OP_SET_OPAQUE, relative);
		if (error != 0) goto out;
	}
	error = overlay_ensure_upper_dir(directory);
	if (error == 0)
		error = inode_mkdir(parent->upper.p_inode, name, mode, &created);
	if (error != 0) goto out;
	inode_release(created);
	error = mount_sync(parent->upper.p_mount);
	if (error == 0 && (overlay_metadata_flags(state, relative) &
	    OVERLAY_META_WHITEOUT))
		error = overlay_journal_append(state,
			OVERLAY_OP_REMOVE_WHITEOUT, relative);
	if (error == 0) {
		namecache_remove(directory, name);
		error = overlay_lookup(directory, name, result);
	}
out:
	if (lower != NULL) inode_release(lower);
	return error;
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
		if (!overlay_inodes[i].used ||
		    overlay_inodes[i].inode.i_mount != mountp)
			continue;
		info = &overlay_inodes[i].info;
		if (!overlay_path_is_below(info->path, old_path))
			continue;
		strcpy(updated, new_path);
		strcat(updated, info->path + old_length);
		strcpy(info->path, updated);
	}
}

static OVERLAY_HIGH int
overlay_rename(struct inode *old_directory,
	       const struct componentname *old_name,
	       struct inode *new_directory,
	       const struct componentname *new_name, unsigned flags)
{
	struct overlay_mount_state *state = old_directory->i_mount->m_data;
	struct overlay_inode_info *old_parent = overlay_info(old_directory);
	struct overlay_inode_info *new_parent = overlay_info(new_directory);
	struct overlay_inode_info *source_info;
	struct inode *source = NULL, *target = NULL, *new_upper = NULL;
	struct path new_upper_path;
	char old_text[NAME_MAX + 1U], new_text[NAME_MAX + 1U];
	char old_relative[ZEDBSD_PATH_MAX], new_relative[ZEDBSD_PATH_MAX];
	int error;
	unsigned identity;
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
	error = overlay_join(old_parent->path, old_text, old_relative);
	if (error == 0)
		error = overlay_join(new_parent->path, new_text, new_relative);
	if (error == 0)
		error = overlay_lookup(old_directory, old_name, &source);
	if (error != 0)
		goto out;
	source_info = overlay_info(source);
	(void)overlay_lookup(new_directory, new_name, &target);
	if (source->i_type == INODE_DIR) {
		if (source_info->upper.p_inode == NULL ||
		    source_info->lower.p_inode != NULL) {
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
	} else if (source_info->upper.p_inode == NULL) {
		error = overlay_copy_up_regular(source);
		if (error != 0)
			goto out;
	}
	error = overlay_ensure_upper_dir(new_directory);
	if (error != 0)
		goto out;
	if (source_info->lower.p_inode != NULL) {
		error = overlay_journal_append(state,
			OVERLAY_OP_ADD_WHITEOUT, old_relative);
		if (error != 0)
			goto out;
	}
	error = inode_rename(old_parent->upper.p_inode, old_name,
		new_parent->upper.p_inode, new_name, 0);
	if (error == 0)
		error = mount_sync(new_parent->upper.p_mount);
	if (error != 0)
		goto out;
	if ((overlay_metadata_flags(state, new_relative) &
	    OVERLAY_META_WHITEOUT) != 0) {
		error = overlay_journal_append(state,
			OVERLAY_OP_REMOVE_WHITEOUT, new_relative);
		if (error != 0)
			goto out;
	}
	error = inode_lookup(new_parent->upper.p_inode, new_name, &new_upper);
	if (error != 0)
		goto out;
	path_set(&new_upper_path, new_parent->upper.p_mount, new_upper);
	identity = source_info->identity_index;
	if (source->i_type == INODE_DIR)
		overlay_repath_commit(state, source->i_mount, old_relative,
			new_relative);
	else {
		strcpy(state->identities[identity].path, new_relative);
		strcpy(source_info->path, new_relative);
	}
	path_release(&source_info->lower);
	overlay_install_upper(source, &new_upper_path);
	if (target != NULL && target != source)
		overlay_retire_inode(target);
	namecache_remove(old_directory, old_name);
	namecache_remove(new_directory, new_name);
	error = 0;
out:
	if (new_upper != NULL) inode_release(new_upper);
	if (target != NULL) inode_release(target);
	if (source != NULL) inode_release(source);
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
	struct overlay_inode_info *parent = overlay_info(directory);
	struct overlay_inode_info *target_info;
	struct inode *target;
	char text[NAME_MAX + 1U], relative[ZEDBSD_PATH_MAX];
	int error;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_component_text(name, text);
	if (error != 0 || overlay_reserved_name(text))
		return error != 0 ? error : EINVAL;
	error = overlay_join(parent->path, text, relative);
	if (error == 0)
		error = overlay_lookup(directory, name, &target);
	if (error != 0)
		return error;
	target_info = overlay_info(target);
	if ((target->i_type == INODE_DIR) != removing_directory) {
		error = removing_directory ? ENOTDIR : EISDIR;
		goto out;
	}
	if (removing_directory && (error = overlay_directory_empty(target)) != 0)
		goto out;
	if (target_info->lower.p_inode != NULL) {
		error = overlay_journal_append(state,
			OVERLAY_OP_ADD_WHITEOUT, relative);
		if (error != 0)
			goto out;
	}
	if (target_info->upper.p_inode != NULL) {
		error = removing_directory ?
			inode_rmdir(parent->upper.p_inode, name) :
			inode_unlink(parent->upper.p_inode, name);
		if (error != 0)
			goto out;
		error = mount_sync(parent->upper.p_mount);
		if (error != 0)
			goto out;
	}
	namecache_remove(directory, name);
	overlay_retire_inode(target);
	error = 0;
out:
	inode_release(target);
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
overlay_truncate(struct inode *inode, off_t size)
{
	struct overlay_mount_state *state = inode->i_mount->m_data;
	struct overlay_inode_info *info = overlay_info(inode);
	int error;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_copy_up_regular(inode);
	if (error == 0)
		error = inode_truncate(info->upper.p_inode, size);
	if (error == 0) {
		extern void vm_object_truncate_inode(struct inode *, off_t)
			__attribute__((weak));
		if (vm_object_truncate_inode != NULL)
			vm_object_truncate_inode(info->upper.p_inode, size);
		overlay_refresh(inode);
		error = mount_sync(info->upper.p_mount);
	}
	return error;
}

static OVERLAY_HIGH int
overlay_setattr(struct inode *inode, const struct stat *status, unsigned mask)
{
	struct overlay_mount_state *state = inode->i_mount->m_data;
	struct overlay_inode_info *info = overlay_info(inode);
	int error;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	if (inode->i_type == INODE_REG)
		error = overlay_copy_up_regular(inode);
	else if (inode->i_type == INODE_DIR)
		error = overlay_ensure_upper_dir(inode);
	else
		error = EOPNOTSUPP;
	if (error == 0)
		error = inode_setattr(info->upper.p_inode, status, mask);
	if (error == 0) {
		overlay_refresh(inode);
		error = mount_sync(info->upper.p_mount);
	}
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
	.unlink = overlay_unlink,
	.rmdir = overlay_rmdir,
	.rename = overlay_rename,
	.getattr = overlay_getattr,
	.setattr = overlay_setattr,
	.truncate = overlay_truncate,
	.reclaim = overlay_reclaim,
};

static OVERLAY_HIGH int
overlay_regular_open(struct file *file)
{
	struct overlay_inode_info *inode_info = overlay_info(file->f_inode);
	struct overlay_file_info *info;
	const struct path *visible;
	int error, real_flags;
	if (inode_info == NULL)
		return EIO;
	if ((file->f_flags & O_ACCMODE) != O_RDONLY) {
		error = overlay_copy_up_regular(file->f_inode);
		if (error != 0)
			return error;
	}
	visible = overlay_visible(inode_info);
	info = kern_malloc(sizeof(*info));
	if (info == NULL)
		return ENOMEM;
	real_flags = file->f_flags & ~(O_CREAT | O_EXCL | O_TRUNC);
	error = file_open_resolved(visible, real_flags, &info->real);
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
	return info != NULL ? file_pread(info->real, buffer, size, offset) : -EIO;
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
	count = file_pwrite(info->real, buffer, size, offset);
	if (count >= 0)
		overlay_refresh(file->f_inode);
	return count;
}

static OVERLAY_HIGH ssize_t
overlay_write(struct file *file, const void *buffer, size_t size)
{
	off_t offset = (file->f_flags & O_APPEND) != 0 ?
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
	struct overlay_inode_info *info = overlay_info(file->f_inode);
	const struct path *path = cursor->phase == OVERLAY_DIR_UPPER ?
		&info->upper : &info->lower;
	if (cursor->phase == OVERLAY_DIR_LOWER &&
	    (overlay_metadata_flags(file->f_inode->i_mount->m_data,
	     info->path) & OVERLAY_META_OPAQUE) != 0)
		return ENOENT;
	if (path->p_inode == NULL || path->p_inode->i_type != INODE_DIR)
		return ENOENT;
	return file_open_resolved(path, O_RDONLY | O_DIRECTORY, &cursor->active);
}

static OVERLAY_HIGH int
overlay_dir_upper_has(struct overlay_inode_info *info, const char *name)
{
	struct componentname component;
	struct inode *found;
	int error;
	if (info->upper.p_inode == NULL)
		return 0;
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = 0;
	error = inode_lookup(info->upper.p_inode, &component, &found);
	if (error == 0)
		inode_release(found);
	return error == 0;
}

static OVERLAY_HIGH int
overlay_dir_child_hidden(struct inode *directory, const char *name)
{
	struct overlay_inode_info *info = overlay_info(directory);
	char relative[ZEDBSD_PATH_MAX];
	if (overlay_join(info->path, name, relative) != 0)
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
	struct overlay_inode_info *info = overlay_info(file->f_inode);
	if (cursor == NULL || info == NULL)
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
		     (overlay_dir_upper_has(info, real_entry.d_name) ||
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

static const struct file_ops overlay_directory_ops = {
	.open = overlay_dir_open,
	.readdir = overlay_readdir,
	.seek = overlay_dir_seek,
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
			if (!overlay_reserved_name(entry.d_name))
				continue;
			component.cn_nameptr = entry.d_name;
			component.cn_namelen = strlen(entry.d_name);
			component.cn_flags = COMPONENT_LAST;
			error = inode_lookup(directory->p_inode, &component, &child);
			if (error != 0)
				break;
			if (child->i_type != INODE_REG) {
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
	    args->lower.p_inode == NULL || args->metadata_root.p_inode == NULL ||
	    args->upper.p_inode->i_type != INODE_DIR ||
	    args->lower.p_inode->i_type != INODE_DIR ||
	    args->metadata_root.p_inode->i_type != INODE_DIR ||
	    (args->flags != OVERLAY_READ_ONLY &&
	     args->flags != OVERLAY_READ_WRITE) || args->journal_base == NULL ||
	    (strcmp(args->journal_base, "bin") &&
	     strcmp(args->journal_base, "lib")))
		return EINVAL;
	state = kern_calloc(1, sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	path_set(&state->upper_root, args->upper.p_mount, args->upper.p_inode);
	path_set(&state->lower_root, args->lower.p_mount, args->lower.p_inode);
	path_set(&state->metadata_root, args->metadata_root.p_mount,
		 args->metadata_root.p_inode);
	strcpy(state->journal_base, args->journal_base);
	state->flags = args->flags;
	state->next_ino = 2;
	state->identities[0].state = OVERLAY_ID_ACTIVE;
	state->identities[0].ino = 1;
	state->identities[0].path[0] = '\0';
	mountp->m_data = state;
	error = overlay_journal_load(state);
	if (error != 0)
		goto fail_state;
	error = overlay_cleanup_temps(&state->upper_root, 0, &visited, &deleted);
	if (error == 0 && deleted != 0)
		error = mount_sync(state->upper_root.p_mount);
	if (error != 0)
		goto fail_state;
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
	path_release(&state->metadata_root);
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
	path_release(&state->metadata_root);
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
