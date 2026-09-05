/*
 * WS001 p022: UFS pathname-socket rollback-failure fixture.
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <kern/disk.h>
#include <kern/file.h>
#include <kern/inode.h>
#include <kern/kmem.h>
#include <kern/mount.h>
#include <kern/namei.h>
#include <kern/pipe.h>
#include <kern/quota.h>

#include "src/drivers/fs/ufs1/ufs1-disk.h"
#include "src/drivers/fs/ufs1/ufs1-endian.h"
#include "src/drivers/fs/ufs2/ufs2-consistency.h"
#include "src/drivers/fs/ufs2/ufs2-disk.h"
#include "src/drivers/fs/ufs2/ufs2-endian.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static unsigned write_calls;
static unsigned release_calls;
static int write_error;
static struct inode *released_inode;
static struct inode *allocated_inode;
static uint8_t fake_cg[UFS2_SECTOR_SIZE];
static uint8_t fake_directory[UFS2_SECTOR_SIZE];
static unsigned directory_write_calls;
static int actual_create_mode;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr,                                        \
			    "ws001-p022 UFS socket fault: failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                  \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

#define CHECK_ERROR(expression, wanted)                                    \
	do {                                                                 \
		int result_ = (expression);                                  \
		int wanted_ = (wanted);                                      \
		checks++;                                                    \
		if (result_ != wanted_) {                                    \
			fprintf(stderr,                                        \
			    "ws001-p022 UFS socket fault: failed at %s:%d: "  \
			    "got %d wanted %d\n", __FILE__, __LINE__,         \
			    result_, wanted_);                                  \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

/* These mirrors are private test scaffolding, not an ABI. */
struct fixture_ufs1_mount_state {
	struct ufs1_super super;
	struct mutex namespace_lock;
	struct mutex lock;
	uint8_t *cg;
	uint32_t cg_iusedoff;
	uint32_t cg_freeoff;
	uint32_t cg_nextfreeoff;
	uint32_t active_cg;
	uint32_t rotor_cg;
	int writable;
};

struct fixture_ufs1_inode_info {
	struct inode inode;
	uint32_t direct[UFS1_NDADDR];
	uint32_t indirect[UFS1_NIADDR];
	uint32_t disk_flags;
	uint32_t blocks;
	uint32_t generation;
	uint8_t shortlink[60];
};

struct fixture_ufs2_mount_state {
	struct ufs2_super super;
	struct mutex namespace_lock;
	struct mutex lock;
	struct mutex journal_lock;
	uint8_t *cg;
	uint32_t cg_iusedoff;
	uint32_t cg_freeoff;
	uint32_t cg_nextfreeoff;
	uint32_t active_cg;
	uint32_t rotor_cg;
	struct ufs2_journal journal;
	struct ufs2_snapshot snapshot;
	struct ufs2_snapshot_entry *snapshot_map;
	struct disk *snapshot_disk;
	struct mutex snapshot_lock;
	struct quota_state quota;
	int journal_enabled;
	int snapshot_available;
	int writable;
};

struct fixture_ufs2_inode_info {
	struct inode inode;
	uint64_t extattr[UFS2_NXADDR];
	uint32_t extattr_size;
	uint64_t direct[UFS2_NDADDR];
	uint64_t indirect[UFS2_NIADDR];
	uint32_t disk_flags;
	uint64_t blocks;
	uint32_t generation;
	uint8_t shortlink[120];
};

#if defined(WS001_P022_UFS1)
extern int ws001_ufs1_restore_directory_block(struct inode *, uint32_t,
    const uint8_t *, int);
extern int ws001_ufs1_discard_new_inode_after_error(struct inode *, int, int);
extern int ws001_ufs1_mknod(struct inode *, const struct componentname *,
    const struct inode_creation_request *, struct inode **);
#elif defined(WS001_P022_UFS2)
extern int ws001_ufs2_restore_directory_block(struct inode *, uint64_t,
    const uint8_t *, int);
extern int ws001_ufs2_discard_new_inode_after_error(struct inode *, int, int);
extern int ws001_ufs2_mknod(struct inode *, const struct componentname *,
    const struct inode_creation_request *, struct inode **);
#else
#error select one WS001 p022 UFS fixture
#endif

int mutex_owned(struct mutex *mutex) { return mutex->locked != 0; }

void
mutex_lock(struct mutex *mutex)
{
	CHECK(!mutex->locked);
	mutex->locked = 1;
}

void
mutex_unlock(struct mutex *mutex)
{
	CHECK(mutex->locked);
	mutex->locked = 0;
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	(void)mutex;
	(void)rank;
	(void)name;
	return 0;
}

void *
kern_malloc(size_t size)
{
	return malloc(size);
}

void *
kern_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void
kern_free(void *pointer)
{
	free(pointer);
}

int
disk_read(struct disk *disk, uint64_t block, uint32_t count, void *buffer)
{
	CHECK(disk != NULL);
	CHECK(buffer != NULL);
	if (!actual_create_mode)
		return EIO;
	if (block == 1U && count == 1U) {
		if (buffer != fake_cg)
			memcpy(buffer, fake_cg, sizeof(fake_cg));
	} else if (block == 20U && count == 1U) {
		memcpy(buffer, fake_directory, sizeof(fake_directory));
	} else {
		memset(buffer, 0, (size_t)count * UFS2_SECTOR_SIZE);
	}
	return 0;
}

int
disk_read_direct(struct disk *disk, uint64_t block, uint32_t count,
    void *buffer)
{
	return disk_read(disk, block, count, buffer);
}

int
disk_sync(struct disk *disk)
{
	(void)disk;
	return 0;
}

int
disk_write(struct disk *disk, uint64_t block, uint32_t count,
    const void *buffer)
{
	CHECK(disk != NULL);
	CHECK(buffer != NULL);
	write_calls++;
	if (actual_create_mode) {
		if (block == 1U && count == 1U)
			memcpy(fake_cg, buffer, sizeof(fake_cg));
		if (block == 20U && count == 1U) {
			directory_write_calls++;
			/* Model a device that completed the namespace write before
			 * reporting failure.  The following restoration write fails, so
			 * the inert dirent and allocated inode must remain paired. */
			if (directory_write_calls == 1U)
				memcpy(fake_directory, buffer,
				    sizeof(fake_directory));
			return directory_write_calls == 1U ? EIO : ENOSPC;
		}
		return 0;
	}
	CHECK(block == 2U);
	CHECK(count == 1U);
	return write_error;
}

void
inode_release(struct inode *inode)
{
	CHECK(inode != NULL);
	release_calls++;
	released_inode = inode;
}

struct inode *
inode_alloc(struct mount *mount)
{
	CHECK(actual_create_mode != 0);
	CHECK(mount != NULL);
	CHECK(allocated_inode != NULL);
	allocated_inode->i_mount = mount;
	return allocated_inode;
}

int
inode_get(struct mount *mount, ino_t ino, struct inode **result)
{
	(void)mount;
	(void)ino;
	if (result != NULL)
		*result = NULL;
	return ENOENT;
}

int
inode_creation_prepare(struct inode *parent, struct inode *inode,
    const struct inode_creation_request *request)
{
	CHECK(parent != NULL);
	CHECK(inode == allocated_inode);
	CHECK(request != NULL);
	inode->i_type = request->type;
	inode->i_mode = request->mode;
	inode->i_uid = request->uid;
	inode->i_gid = request->gid;
	inode->i_rdev = request->rdev;
	inode->i_special = request->special;
	return 0;
}

int
inode_sync(struct inode *inode)
{
	(void)inode;
	return 0;
}

const struct file_ops fifo_file_ops = { 0 };

int
ufs2_snapshot_preserve(struct ufs2_snapshot *snapshot, uint64_t first,
    uint32_t count)
{
	(void)snapshot;
	(void)first;
	(void)count;
	return 0;
}

int
ufs2_journal_commit(struct ufs2_journal *journal, uint64_t target,
    const void *payload, uint32_t sectors)
{
	(void)journal;
	(void)target;
	(void)payload;
	(void)sectors;
	return 0;
}

int
ufs2_journal_init(struct ufs2_journal *journal,
    const struct ufs2_journal_io *io, uint64_t first, uint32_t count)
{
	(void)journal;
	(void)io;
	(void)first;
	(void)count;
	return EINVAL;
}

int
ufs2_journal_replay(struct ufs2_journal *journal)
{
	(void)journal;
	return EINVAL;
}

int
ufs2_snapshot_init(struct ufs2_snapshot *snapshot,
    const struct ufs2_journal_io *io, uint64_t volume, uint64_t first,
    uint32_t sectors, struct ufs2_snapshot_entry *map, size_t map_count)
{
	(void)snapshot;
	(void)io;
	(void)volume;
	(void)first;
	(void)sectors;
	(void)map;
	(void)map_count;
	return EINVAL;
}

int
ufs2_snapshot_open(struct ufs2_snapshot *snapshot)
{
	(void)snapshot;
	return EINVAL;
}

int
ufs2_snapshot_create(struct ufs2_snapshot *snapshot)
{
	(void)snapshot;
	return EINVAL;
}

int
ufs2_snapshot_read(struct ufs2_snapshot *snapshot, uint64_t first,
    uint32_t count, void *buffer)
{
	(void)snapshot;
	(void)first;
	(void)count;
	(void)buffer;
	return EINVAL;
}

int
ufs2_snapshot_delete(struct ufs2_snapshot *snapshot)
{
	(void)snapshot;
	return EINVAL;
}

int
quota_release(struct quota_state *state, uid_t uid, gid_t gid,
    uint64_t blocks, uint64_t inodes)
{
	(void)state;
	(void)uid;
	(void)gid;
	(void)blocks;
	(void)inodes;
	return 0;
}

void
quota_state_init(struct quota_state *state)
{
	memset(state, 0, sizeof(*state));
}

int
quota_enable(struct quota_state *state, enum quota_type type, int enabled)
{
	(void)state;
	(void)type;
	(void)enabled;
	return 0;
}

int
quota_enabled(struct quota_state *state, enum quota_type type, int *enabled)
{
	(void)state;
	(void)type;
	*enabled = 0;
	return 0;
}

int
quota_get_grace(struct quota_state *state, uint64_t *seconds)
{
	(void)state;
	*seconds = 0;
	return 0;
}

int
quota_set_grace(struct quota_state *state, uint64_t seconds)
{
	(void)state;
	(void)seconds;
	return 0;
}

int
quota_get(struct quota_state *state, enum quota_type type, uint32_t id,
    struct quota_record *result)
{
	(void)state;
	(void)type;
	(void)id;
	memset(result, 0, sizeof(*result));
	return 0;
}

int
quota_set(struct quota_state *state, enum quota_type type,
    const struct quota_record *source)
{
	(void)state;
	(void)type;
	(void)source;
	return 0;
}

int
quota_reserve(struct quota_state *state, uid_t uid, gid_t gid,
    uint64_t blocks, uint64_t inodes, uint64_t now,
    struct quota_charge *charge)
{
	(void)state;
	(void)uid;
	(void)gid;
	(void)blocks;
	(void)inodes;
	(void)now;
	memset(charge, 0, sizeof(*charge));
	return 0;
}

void
quota_commit(struct quota_charge *charge)
{
	(void)charge;
}

void
quota_rollback(struct quota_charge *charge)
{
	(void)charge;
}

int
quota_transfer_begin(struct quota_state *state, uid_t old_uid,
    gid_t old_gid, uid_t new_uid, gid_t new_gid, uint64_t blocks,
    uint64_t inodes, uint64_t now, struct quota_transfer *transfer)
{
	(void)state;
	(void)old_uid;
	(void)old_gid;
	(void)new_uid;
	(void)new_gid;
	(void)blocks;
	(void)inodes;
	(void)now;
	memset(transfer, 0, sizeof(*transfer));
	return 0;
}

void
quota_transfer_commit(struct quota_transfer *transfer)
{
	(void)transfer;
}

void
quota_transfer_rollback(struct quota_transfer *transfer)
{
	(void)transfer;
}

int
quota_rebuild_add(struct quota_state *state, uid_t uid, gid_t gid,
    uint64_t blocks, uint64_t inodes)
{
	(void)state;
	(void)uid;
	(void)gid;
	(void)blocks;
	(void)inodes;
	return 0;
}

int
quota_export_config(struct quota_state *state, void *buffer,
    size_t capacity, size_t *length)
{
	(void)state;
	(void)buffer;
	(void)capacity;
	*length = 0;
	return 0;
}

int
quota_import_config(struct quota_state *state, const void *buffer,
    size_t length)
{
	(void)state;
	(void)buffer;
	(void)length;
	return 0;
}

void
bio_complete(struct bio *bio, int error, size_t transferred)
{
	(void)bio;
	(void)error;
	(void)transferred;
}

struct disk *
disk_alloc(void)
{
	return calloc(1, sizeof(struct disk));
}

int
disk_create(struct disk *disk)
{
	(void)disk;
	return 0;
}

int
disk_destroy(struct disk *disk)
{
	free(disk);
	return 0;
}

int
disk_gone_if_idle(struct disk *disk)
{
	(void)disk;
	return 0;
}

void
clock_realtime(time_t *seconds, long *nanoseconds)
{
	*seconds = 1;
	*nanoseconds = 0;
}

int
ufs1_super_decode(const void *buffer, size_t length, uint64_t sectors,
    struct ufs1_super *super)
{
	(void)buffer;
	(void)length;
	(void)sectors;
	(void)super;
	return EINVAL;
}

int
ufs2_super_decode(const void *buffer, size_t length, uint64_t sectors,
    struct ufs2_super *super)
{
	(void)buffer;
	(void)length;
	(void)sectors;
	(void)super;
	return EINVAL;
}

static void
special_destroy(void *special)
{
	(void)special;
	CHECK(0);
}

static void
reset_fixture(void)
{
	write_calls = 0;
	release_calls = 0;
	write_error = ENOSPC;
	released_inode = NULL;
	allocated_inode = NULL;
	directory_write_calls = 0;
	actual_create_mode = 0;
}

static void
initialize_directory_block(void)
{
	memset(fake_directory, 0, sizeof(fake_directory));
	fake_directory[6] = 4U;
	fake_directory[7] = 1U;
	fake_directory[8] = '.';
#if defined(WS001_P022_UFS1)
	ufs1_put32(fake_directory, 0, UFS1_ROOT_INO, 0);
	ufs1_put16(fake_directory, 4, UFS1_DIRBLKSIZ, 0);
#else
	ufs2_put32(fake_directory, 0, UFS2_ROOT_INO, 0);
	ufs2_put16(fake_directory, 4, UFS2_DIRBLKSIZ, 0);
#endif
}

static int
directory_entry_maps(const char *wanted, uint32_t wanted_ino)
{
	size_t offset = 0;

	while (offset + 8U <= sizeof(fake_directory)) {
		uint16_t record_length;
		uint32_t ino;
		uint8_t name_length;

#if defined(WS001_P022_UFS1)
		ino = ufs1_get32(fake_directory, offset, 0);
		record_length = ufs1_get16(fake_directory, offset + 4U, 0);
#else
		ino = ufs2_get32(fake_directory, offset, 0);
		record_length = ufs2_get16(fake_directory, offset + 4U, 0);
#endif
		name_length = fake_directory[offset + 7U];
		if (record_length < 8U || offset + record_length >
		    sizeof(fake_directory) || name_length > record_length - 8U)
			return 0;
		if (ino == wanted_ino && name_length == strlen(wanted) &&
		    memcmp(fake_directory + offset + 8U, wanted,
		    name_length) == 0)
			return 1;
		offset += record_length;
	}
	return 0;
}

#if defined(WS001_P022_UFS1)
static void
initialize_ufs1_cg(void)
{
	memset(fake_cg, 0, sizeof(fake_cg));
	ufs1_put32(fake_cg, UFS1_CG_MAGIC, UFS1_CG_MAGIC_VALUE, 0);
	ufs1_put32(fake_cg, UFS1_CG_CGX, 0, 0);
	ufs1_put32(fake_cg, UFS1_CG_NDBLK, 64U, 0);
	ufs1_put32(fake_cg, UFS1_CG_NDIR, 1U, 0);
	ufs1_put32(fake_cg, UFS1_CG_NBFREE, 40U, 0);
	ufs1_put32(fake_cg, UFS1_CG_NIFREE, 13U, 0);
	ufs1_put32(fake_cg, UFS1_CG_NFFREE, 0, 0);
	ufs1_put32(fake_cg, UFS1_CG_IUSEDOFF, 128U, 0);
	ufs1_put32(fake_cg, UFS1_CG_FREEOFF, 160U, 0);
	ufs1_put32(fake_cg, UFS1_CG_NEXTFREEOFF, 192U, 0);
	fake_cg[128] = 0x07U;
}
#else
static void
initialize_ufs2_cg(void)
{
	memset(fake_cg, 0, sizeof(fake_cg));
	ufs2_put32(fake_cg, UFS2_CG_MAGIC, UFS2_CG_MAGIC_VALUE, 0);
	ufs2_put32(fake_cg, UFS2_CG_CGX, 0, 0);
	ufs2_put32(fake_cg, UFS2_CG_NDBLK, 64U, 0);
	ufs2_put32(fake_cg, UFS2_CG_NDIR, 1U, 0);
	ufs2_put32(fake_cg, UFS2_CG_NBFREE, 40U, 0);
	ufs2_put32(fake_cg, UFS2_CG_NIFREE, 13U, 0);
	ufs2_put32(fake_cg, UFS2_CG_NFFREE, 0, 0);
	ufs2_put32(fake_cg, UFS2_CG_IUSEDOFF, 128U, 0);
	ufs2_put32(fake_cg, UFS2_CG_FREEOFF, 160U, 0);
	ufs2_put32(fake_cg, UFS2_CG_NEXTFREEOFF, 192U, 0);
	fake_cg[128] = 0x07U;
}
#endif

#if defined(WS001_P022_UFS1)
static void
test_ufs1(void)
{
	struct fixture_ufs1_mount_state state;
	struct fixture_ufs1_inode_info directory;
	struct fixture_ufs1_inode_info child;
	struct mount mount;
	struct disk disk;
	uint8_t original[UFS1_SECTOR_SIZE];
	int endpoint;
	int cleanup;

	memset(&state, 0, sizeof(state));
	memset(&directory, 0, sizeof(directory));
	memset(&child, 0, sizeof(child));
	memset(&mount, 0, sizeof(mount));
	memset(&disk, 0, sizeof(disk));
	memset(original, 0x5a, sizeof(original));
	state.super.bsize = UFS1_SECTOR_SIZE;
	state.super.frag = 1U;
	state.super.size = 64U;
	state.writable = 1;
	mount.m_data = &state;
	mount.m_disk = &disk;
	directory.inode.i_mount = &mount;
	child.inode.i_mount = &mount;
	child.inode.i_type = INODE_SOCKET;
	child.inode.i_ino = 17U;
	child.inode.i_special = &endpoint;
	child.inode.i_special_destroy = special_destroy;
	reset_fixture();

	cleanup = ws001_ufs1_restore_directory_block(&directory.inode, 2U,
	    original, EIO);
	CHECK_ERROR(cleanup, ENOSPC);
	CHECK(write_calls == 1U);
	CHECK(state.writable == 0);
	CHECK_ERROR(ws001_ufs1_discard_new_inode_after_error(&child.inode, 0,
	    cleanup), ENOSPC);
	CHECK(release_calls == 1U);
	CHECK(released_inode == &child.inode);
	CHECK(child.inode.i_special == NULL);
	CHECK(child.inode.i_special_destroy == NULL);
	CHECK(child.inode.i_ino == 17U);
	CHECK((child.inode.i_flags & INODE_DEAD) == 0U);
	CHECK(state.writable == 0);
}

static void
test_ufs1_mknod_caller(void)
{
	struct fixture_ufs1_mount_state state;
	struct fixture_ufs1_inode_info directory;
	struct fixture_ufs1_inode_info child;
	struct inode_creation_request request;
	struct componentname name = { "sock", 4U, COMPONENT_LAST };
	struct inode *result = (struct inode *)(uintptr_t)1U;
	struct mount mount;
	struct disk disk;
	int endpoint;

	memset(&state, 0, sizeof(state));
	memset(&directory, 0, sizeof(directory));
	memset(&child, 0, sizeof(child));
	memset(&request, 0, sizeof(request));
	memset(&mount, 0, sizeof(mount));
	memset(&disk, 0, sizeof(disk));
	reset_fixture();
	initialize_ufs1_cg();
	initialize_directory_block();
	state.super.cblkno = 1U;
	state.super.iblkno = 2U;
	state.super.dblkno = 10U;
	state.super.size = 64U;
	state.super.dsize = 54U;
	state.super.ncg = 1U;
	state.super.bsize = UFS1_SECTOR_SIZE;
	state.super.fsize = UFS1_SECTOR_SIZE;
	state.super.frag = 1U;
	state.super.inopb = 4U;
	state.super.ipg = 16U;
	state.super.fpg = 64U;
	state.super.cgsize = 192U;
	state.super.cstotal_nbfree = 40U;
	state.super.cstotal_nifree = 13U;
	state.cg = fake_cg;
	state.writable = 1;
	mount.m_data = &state;
	mount.m_disk = &disk;
	directory.inode.i_mount = &mount;
	directory.inode.i_type = INODE_DIR;
	directory.inode.i_ino = UFS1_ROOT_INO;
	directory.inode.i_size = UFS1_DIRBLKSIZ;
	directory.direct[0] = 20U;
	request.origin = INODE_CREATION_USER;
	request.type = INODE_SOCKET;
	request.mode = 0770U;
	request.uid = 41U;
	request.gid = 42U;
	request.special = &endpoint;
	allocated_inode = &child.inode;
	actual_create_mode = 1;

	CHECK_ERROR(ws001_ufs1_mknod(&directory.inode, &name, &request,
	    &result), ENOSPC);
	CHECK(result == NULL);
	CHECK(directory_write_calls == 2U);
	CHECK(state.writable == 0);
	CHECK(release_calls == 1U);
	CHECK(released_inode == &child.inode);
	CHECK(child.inode.i_special == NULL);
	CHECK(child.inode.i_special_destroy == NULL);
	CHECK(child.inode.i_ino == 3U);
	CHECK(child.inode.i_linkcount == 1U);
	CHECK((child.inode.i_flags & INODE_DEAD) == 0U);
	CHECK((fake_cg[128] & 0x08U) != 0U);
	CHECK(directory_entry_maps("sock", 3U));
	CHECK(directory.inode.i_size == UFS1_DIRBLKSIZ);
	CHECK(directory.direct[0] == 20U);
}
#endif

#if defined(WS001_P022_UFS2)
static void
test_ufs2(void)
{
	struct fixture_ufs2_mount_state state;
	struct fixture_ufs2_inode_info directory;
	struct fixture_ufs2_inode_info child;
	struct mount mount;
	struct disk disk;
	uint8_t original[UFS2_SECTOR_SIZE];
	int endpoint;
	int cleanup;

	memset(&state, 0, sizeof(state));
	memset(&directory, 0, sizeof(directory));
	memset(&child, 0, sizeof(child));
	memset(&mount, 0, sizeof(mount));
	memset(&disk, 0, sizeof(disk));
	memset(original, 0xa5, sizeof(original));
	state.super.bsize = UFS2_SECTOR_SIZE;
	state.super.frag = 1U;
	state.super.size = 64U;
	state.writable = 1;
	mount.m_data = &state;
	mount.m_disk = &disk;
	directory.inode.i_mount = &mount;
	child.inode.i_mount = &mount;
	child.inode.i_type = INODE_SOCKET;
	child.inode.i_ino = 23U;
	child.inode.i_special = &endpoint;
	child.inode.i_special_destroy = special_destroy;
	reset_fixture();

	cleanup = ws001_ufs2_restore_directory_block(&directory.inode, 2U,
	    original, EIO);
	CHECK_ERROR(cleanup, ENOSPC);
	CHECK(write_calls == 1U);
	CHECK(state.writable == 0);
	CHECK_ERROR(ws001_ufs2_discard_new_inode_after_error(&child.inode, 0,
	    cleanup), ENOSPC);
	CHECK(release_calls == 1U);
	CHECK(released_inode == &child.inode);
	CHECK(child.inode.i_special == NULL);
	CHECK(child.inode.i_special_destroy == NULL);
	CHECK(child.inode.i_ino == 23U);
	CHECK((child.inode.i_flags & INODE_DEAD) == 0U);
	CHECK(state.writable == 0);
}

static void
test_ufs2_mknod_caller(void)
{
	struct fixture_ufs2_mount_state state;
	struct fixture_ufs2_inode_info directory;
	struct fixture_ufs2_inode_info child;
	struct inode_creation_request request;
	struct componentname name = { "sock", 4U, COMPONENT_LAST };
	struct inode *result = (struct inode *)(uintptr_t)1U;
	struct mount mount;
	struct disk disk;
	int endpoint;

	memset(&state, 0, sizeof(state));
	memset(&directory, 0, sizeof(directory));
	memset(&child, 0, sizeof(child));
	memset(&request, 0, sizeof(request));
	memset(&mount, 0, sizeof(mount));
	memset(&disk, 0, sizeof(disk));
	reset_fixture();
	initialize_ufs2_cg();
	initialize_directory_block();
	state.super.cblkno = 1U;
	state.super.iblkno = 2U;
	state.super.dblkno = 10U;
	state.super.size = 64U;
	state.super.dsize = 54U;
	state.super.ncg = 1U;
	state.super.bsize = UFS2_SECTOR_SIZE;
	state.super.fsize = UFS2_SECTOR_SIZE;
	state.super.frag = 1U;
	state.super.inopb = 2U;
	state.super.ipg = 16U;
	state.super.fpg = 64U;
	state.super.cgsize = 192U;
	state.super.cstotal_nbfree = 40U;
	state.super.cstotal_nifree = 13U;
	state.cg = fake_cg;
	state.writable = 1;
	mount.m_data = &state;
	mount.m_disk = &disk;
	directory.inode.i_mount = &mount;
	directory.inode.i_type = INODE_DIR;
	directory.inode.i_ino = UFS2_ROOT_INO;
	directory.inode.i_size = UFS2_DIRBLKSIZ;
	directory.direct[0] = 20U;
	request.origin = INODE_CREATION_USER;
	request.type = INODE_SOCKET;
	request.mode = 0770U;
	request.uid = 41U;
	request.gid = 42U;
	request.special = &endpoint;
	allocated_inode = &child.inode;
	actual_create_mode = 1;

	CHECK_ERROR(ws001_ufs2_mknod(&directory.inode, &name, &request,
	    &result), ENOSPC);
	CHECK(result == NULL);
	CHECK(directory_write_calls == 2U);
	CHECK(state.writable == 0);
	CHECK(release_calls == 1U);
	CHECK(released_inode == &child.inode);
	CHECK(child.inode.i_special == NULL);
	CHECK(child.inode.i_special_destroy == NULL);
	CHECK(child.inode.i_ino == 3U);
	CHECK(child.inode.i_linkcount == 1U);
	CHECK((child.inode.i_flags & INODE_DEAD) == 0U);
	CHECK((fake_cg[128] & 0x08U) != 0U);
	CHECK(directory_entry_maps("sock", 3U));
	CHECK(directory.inode.i_size == UFS2_DIRBLKSIZ);
	CHECK(directory.direct[0] == 20U);
}
#endif

int
main(void)
{
#if defined(WS001_P022_UFS1)
	test_ufs1();
	test_ufs1_mknod_caller();
	printf("ws001-p022 UFS1 socket rollback fault: PASS (%u checks)\n",
	    checks);
#else
	test_ufs2();
	test_ufs2_mknod_caller();
	printf("ws001-p022 UFS2 socket rollback fault: PASS (%u checks)\n",
	    checks);
#endif
	return EXIT_SUCCESS;
}
