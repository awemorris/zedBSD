/*
 * WS001 p016: deterministic directory-fsync dispatch/order fixture.
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <kern/file.h>
#include <kern/overlayfs.h>

#if defined(WS001_P023_UFS1_MUTATION) || \
    defined(WS001_P023_UFS2_MUTATION)
#include <kern/disk.h>
#include <kern/kmem.h>
#include <kern/mount.h>
#include <kern/namei.h>
#endif

#if defined(WS001_P023_UFS1_MUTATION)
#include "drivers/fs/ufs1/ufs1-disk.h"
#include "drivers/fs/ufs1/ufs1-endian.h"
#elif defined(WS001_P023_UFS2_MUTATION)
#include <kern/quota.h>
#include "drivers/fs/ufs2/ufs2-consistency.h"
#include "drivers/fs/ufs2/ufs2-disk.h"
#include "drivers/fs/ufs2/ufs2-endian.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr, "ws001-p016: failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                  \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

#define CHECK_ERROR(expression, wanted)                                    \
	do {                                                                 \
		int check_result_ = (expression);                             \
		int check_wanted_ = (wanted);                                 \
		checks++;                                                    \
		if (check_result_ != check_wanted_) {                         \
			fprintf(stderr,                                        \
			    "ws001-p016: failed at %s:%d: %s returned %d, "  \
			    "wanted %d\n",                                    \
			    __FILE__, __LINE__, #expression, check_result_,      \
			    check_wanted_);                                      \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

#if defined(WS001_P016_VFS)

static unsigned explicit_calls;
static unsigned inode_calls;
static int explicit_result;
static int inode_result;

void
mutex_lock(struct mutex *mutex)
{
	(void)mutex;
}

void
mutex_unlock(struct mutex *mutex)
{
	(void)mutex;
}

int
inode_sync(struct inode *inode)
{
	CHECK(inode != NULL);
	inode_calls++;
	return inode_result;
}

static int
explicit_fsync(struct file *file)
{
	CHECK(file != NULL);
	explicit_calls++;
	return explicit_result;
}

static void
reset_dispatch(void)
{
	explicit_calls = 0;
	inode_calls = 0;
	explicit_result = 0;
	inode_result = 0;
}

static void
test_vfs_dispatch(void)
{
	static const struct file_ops explicit_ops = {
		.fsync = explicit_fsync,
	};
	static const struct file_ops no_fsync_ops = { 0 };
	struct inode inode;
	struct file file;

	memset(&inode, 0, sizeof(inode));
	memset(&file, 0, sizeof(file));
	file.f_inode = &inode;

	reset_dispatch();
	inode.i_type = INODE_DIR;
	file.f_ops = &explicit_ops;
	explicit_result = EIO;
	CHECK_ERROR(file_fsync(&file), EIO);
	CHECK(explicit_calls == 1U);
	CHECK(inode_calls == 0U);

	reset_dispatch();
	file.f_ops = &no_fsync_ops;
	CHECK_ERROR(file_fsync(&file), EOPNOTSUPP);
	CHECK(explicit_calls == 0U);
	CHECK(inode_calls == 0U);

	reset_dispatch();
	file.f_ops = NULL;
	CHECK_ERROR(file_fsync(&file), EOPNOTSUPP);
	CHECK(inode_calls == 0U);

	reset_dispatch();
	inode.i_type = INODE_REG;
	file.f_ops = &no_fsync_ops;
	inode_result = ENOSPC;
	CHECK_ERROR(file_fsync(&file), ENOSPC);
	CHECK(inode_calls == 1U);

	CHECK_ERROR(file_fsync(NULL), EINVAL);
}

int
main(void)
{
	test_vfs_dispatch();
	printf("ws001-p016 VFS dispatch: PASS (%u checks)\n", checks);
	return EXIT_SUCCESS;
}

#elif defined(WS001_P016_UFS)

static char events[16];
static size_t event_count;
static int inode_result;
static int disk_result;
static struct inode *expected_inode;
static struct disk *expected_disk;

extern int ufs1_file_sync(struct file *);
extern int ufs2_file_sync(struct file *);

static void
event(char value)
{
	CHECK(event_count + 1U < sizeof(events));
	events[event_count++] = value;
	events[event_count] = '\0';
}

int
inode_sync(struct inode *inode)
{
	CHECK(inode == expected_inode);
	event('I');
	return inode_result;
}

int
disk_sync(struct disk *disk)
{
	CHECK(disk == expected_disk);
	event('D');
	return disk_result;
}

static void
reset_order(void)
{
	memset(events, 0, sizeof(events));
	event_count = 0;
	inode_result = 0;
	disk_result = 0;
}

static void
test_ufs_one(const char *name, int (*sync_function)(struct file *))
{
	struct disk disk;
	struct mount mount;
	struct inode inode;
	struct file file;

	(void)name;
	memset(&disk, 0, sizeof(disk));
	memset(&mount, 0, sizeof(mount));
	memset(&inode, 0, sizeof(inode));
	memset(&file, 0, sizeof(file));
	mount.m_disk = &disk;
	inode.i_mount = &mount;
	file.f_inode = &inode;
	expected_inode = &inode;
	expected_disk = &disk;

	reset_order();
	CHECK_ERROR(sync_function(&file), 0);
	CHECK(strcmp(events, "ID") == 0);

	reset_order();
	inode_result = EIO;
	CHECK_ERROR(sync_function(&file), EIO);
	CHECK(strcmp(events, "I") == 0);

	reset_order();
	disk_result = ENOSPC;
	CHECK_ERROR(sync_function(&file), ENOSPC);
	CHECK(strcmp(events, "ID") == 0);

	reset_order();
	CHECK_ERROR(sync_function(NULL), EINVAL);
	CHECK(event_count == 0U);
}

int
main(void)
{
	test_ufs_one("ufs1", ufs1_file_sync);
	test_ufs_one("ufs2", ufs2_file_sync);
	printf("ws001-p016 UFS order/error propagation: PASS (%u checks)\n",
	    checks);
	return EXIT_SUCCESS;
}

#elif defined(WS001_P023_UFS1_MUTATION)

/*
 * q050 adds a production-backed namespace-mutation cell to the earlier
 * directory-fsync dispatch/order fixture.  dir_replace() is the exact UFS1
 * primitive used by rename-over-existing.  The object build makes only that
 * private symbol visible; these mirrors are fixture-local and are not ABI.
 */
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

#define FIXTURE_SECTOR_SIZE UFS1_SECTOR_SIZE
#define FIXTURE_SECTORS 64U
#define FIXTURE_DIRECTORY_FRAGMENT 10U
#define FIXTURE_OLD_INO 41U
#define FIXTURE_NEW_INO 99U

struct fixture_write_result {
	int error;
	int commit;
};

static uint8_t fixture_disk[FIXTURE_SECTORS * FIXTURE_SECTOR_SIZE];
static struct fixture_write_result write_results[4];
static unsigned read_calls;
static unsigned write_calls;

extern int ufs1_dir_replace(struct inode *, const struct componentname *,
    uint32_t, uint8_t, uint32_t *, uint8_t *);

void
mutex_lock(struct mutex *mutex)
{
	CHECK(mutex != NULL);
}

void
mutex_unlock(struct mutex *mutex)
{
	CHECK(mutex != NULL);
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
disk_read(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	CHECK(disk != NULL);
	CHECK(data != NULL);
	CHECK(count <= FIXTURE_SECTORS);
	CHECK(block <= FIXTURE_SECTORS - count);
	memcpy(data, fixture_disk + block * FIXTURE_SECTOR_SIZE,
	    (size_t)count * FIXTURE_SECTOR_SIZE);
	read_calls++;
	return 0;
}

int
disk_write(struct disk *disk, uint64_t block, uint32_t count,
    const void *data)
{
	struct fixture_write_result result;

	CHECK(disk != NULL);
	CHECK(data != NULL);
	CHECK(count <= FIXTURE_SECTORS);
	CHECK(block <= FIXTURE_SECTORS - count);
	CHECK(write_calls < sizeof(write_results) / sizeof(write_results[0]));
	result = write_results[write_calls++];
	if (result.commit)
		memcpy(fixture_disk + block * FIXTURE_SECTOR_SIZE, data,
		    (size_t)count * FIXTURE_SECTOR_SIZE);
	return result.error;
}

static uint8_t *
directory_block(void)
{
	return fixture_disk + FIXTURE_DIRECTORY_FRAGMENT * FIXTURE_SECTOR_SIZE;
}

static void
initialize_directory(struct fixture_ufs1_mount_state *state,
    struct fixture_ufs1_inode_info *directory, struct mount *mount,
    struct disk *disk)
{
	uint8_t *block;

	memset(fixture_disk, 0xa5, sizeof(fixture_disk));
	memset(write_results, 0, sizeof(write_results));
	memset(state, 0, sizeof(*state));
	memset(directory, 0, sizeof(*directory));
	memset(mount, 0, sizeof(*mount));
	memset(disk, 0, sizeof(*disk));
	read_calls = 0;
	write_calls = 0;

	state->super.bsize = FIXTURE_SECTOR_SIZE;
	state->super.fsize = FIXTURE_SECTOR_SIZE;
	state->super.frag = 1;
	state->super.fsbtodb = 0;
	state->super.size = FIXTURE_SECTORS;
	state->writable = 1;
	mount->m_data = state;
	mount->m_disk = disk;
	disk->d_block_size = FIXTURE_SECTOR_SIZE;
	disk->d_block_count = FIXTURE_SECTORS;
	directory->inode.i_mount = mount;
	directory->inode.i_type = INODE_DIR;
	directory->inode.i_size = UFS1_DIRBLKSIZ;
	directory->direct[0] = FIXTURE_DIRECTORY_FRAGMENT;

	block = directory_block();
	memset(block, 0, FIXTURE_SECTOR_SIZE);
	ufs1_put32(block, 0, FIXTURE_OLD_INO, 0);
	ufs1_put16(block, 4, UFS1_DIRBLKSIZ, 0);
	block[6] = 8;
	block[7] = 6;
	memcpy(block + 8, "victim", 6);
}

static void
test_ufs1_namespace_write_failure(void)
{
	static const struct componentname victim = { "victim", 6, 0 };
	struct fixture_ufs1_mount_state state;
	struct fixture_ufs1_inode_info directory;
	struct mount mount;
	struct disk disk;
	uint8_t original[FIXTURE_SECTOR_SIZE];
	uint32_t old_number;
	uint8_t old_type;

	initialize_directory(&state, &directory, &mount, &disk);
	memcpy(original, directory_block(), sizeof(original));
	write_results[0] = (struct fixture_write_result){ 0, 1 };
	CHECK_ERROR(ufs1_dir_replace(&directory.inode, &victim,
	    FIXTURE_NEW_INO, 4, &old_number, &old_type), 0);
	CHECK(old_number == FIXTURE_OLD_INO);
	CHECK(old_type == 8);
	CHECK(ufs1_get32(directory_block(), 0, 0) == FIXTURE_NEW_INO);
	CHECK(directory_block()[6] == 4);
	CHECK(memcmp(directory_block() + 8, "victim", 6) == 0);
	CHECK(read_calls == 1U);
	CHECK(write_calls == 1U);

	/* A conventional failed write leaves the old block, and the production
	 * rollback still rewrites the captured original before returning EIO. */
	initialize_directory(&state, &directory, &mount, &disk);
	memcpy(original, directory_block(), sizeof(original));
	write_results[0] = (struct fixture_write_result){ EIO, 0 };
	write_results[1] = (struct fixture_write_result){ 0, 1 };
	CHECK_ERROR(ufs1_dir_replace(&directory.inode, &victim,
	    FIXTURE_NEW_INO, 4, &old_number, &old_type), EIO);
	CHECK(memcmp(directory_block(), original, sizeof(original)) == 0);
	CHECK(read_calls == 1U);
	CHECK(write_calls == 2U);
	CHECK(state.writable == 1);

	/* A backend may report failure after changing media.  The same rollback
	 * must remove that uncertain namespace mutation as well. */
	initialize_directory(&state, &directory, &mount, &disk);
	memcpy(original, directory_block(), sizeof(original));
	write_results[0] = (struct fixture_write_result){ EIO, 1 };
	write_results[1] = (struct fixture_write_result){ 0, 1 };
	CHECK_ERROR(ufs1_dir_replace(&directory.inode, &victim,
	    FIXTURE_NEW_INO, 4, &old_number, &old_type), EIO);
	CHECK(memcmp(directory_block(), original, sizeof(original)) == 0);
	CHECK(write_calls == 2U);
	CHECK(state.writable == 1);

	/* If even rollback fails, its error wins and the mount is quarantined. */
	initialize_directory(&state, &directory, &mount, &disk);
	write_results[0] = (struct fixture_write_result){ EIO, 1 };
	write_results[1] = (struct fixture_write_result){ ENOSPC, 0 };
	CHECK_ERROR(ufs1_dir_replace(&directory.inode, &victim,
	    FIXTURE_NEW_INO, 4, &old_number, &old_type), ENOSPC);
	CHECK(write_calls == 2U);
	CHECK(state.writable == 0);
}

int
main(void)
{
	test_ufs1_namespace_write_failure();
	printf("ws001-p023 UFS1 namespace write/rollback: PASS (%u checks)\n",
	    checks);
	return EXIT_SUCCESS;
}

#elif defined(WS001_P023_UFS2_MUTATION)

/* UFS2 intentionally receives its own complete fixture implementation. */
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

#define FIXTURE_SECTOR_SIZE UFS2_SECTOR_SIZE
#define FIXTURE_SECTORS 64U
#define FIXTURE_DIRECTORY_FRAGMENT 10U
#define FIXTURE_OLD_INO 41U
#define FIXTURE_NEW_INO 99U

struct fixture_write_result {
	int error;
	int commit;
};

static uint8_t fixture_disk[FIXTURE_SECTORS * FIXTURE_SECTOR_SIZE];
static struct fixture_write_result write_results[4];
static unsigned read_calls;
static unsigned write_calls;
static unsigned inactive_consistency_calls;

extern int ufs2_dir_replace(struct inode *, const struct componentname *,
    uint32_t, uint8_t, uint32_t *, uint8_t *);

void
mutex_lock(struct mutex *mutex)
{
	CHECK(mutex != NULL);
}

void
mutex_unlock(struct mutex *mutex)
{
	CHECK(mutex != NULL);
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
disk_read(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	CHECK(disk != NULL);
	CHECK(data != NULL);
	CHECK(count <= FIXTURE_SECTORS);
	CHECK(block <= FIXTURE_SECTORS - count);
	memcpy(data, fixture_disk + block * FIXTURE_SECTOR_SIZE,
	    (size_t)count * FIXTURE_SECTOR_SIZE);
	read_calls++;
	return 0;
}

int
disk_write(struct disk *disk, uint64_t block, uint32_t count,
    const void *data)
{
	struct fixture_write_result result;

	CHECK(disk != NULL);
	CHECK(data != NULL);
	CHECK(count <= FIXTURE_SECTORS);
	CHECK(block <= FIXTURE_SECTORS - count);
	CHECK(write_calls < sizeof(write_results) / sizeof(write_results[0]));
	result = write_results[write_calls++];
	if (result.commit)
		memcpy(fixture_disk + block * FIXTURE_SECTOR_SIZE, data,
		    (size_t)count * FIXTURE_SECTOR_SIZE);
	return result.error;
}

int
ufs2_snapshot_preserve(struct ufs2_snapshot *snapshot, uint64_t first,
    uint32_t count)
{
	(void)snapshot;
	(void)first;
	(void)count;
	inactive_consistency_calls++;
	return EIO;
}

int
ufs2_journal_commit(struct ufs2_journal *journal, uint64_t target,
    const void *payload, uint32_t sectors)
{
	(void)journal;
	(void)target;
	(void)payload;
	(void)sectors;
	inactive_consistency_calls++;
	return EIO;
}

static uint8_t *
directory_block(void)
{
	return fixture_disk + FIXTURE_DIRECTORY_FRAGMENT * FIXTURE_SECTOR_SIZE;
}

static void
initialize_directory(struct fixture_ufs2_mount_state *state,
    struct fixture_ufs2_inode_info *directory, struct mount *mount,
    struct disk *disk)
{
	uint8_t *block;

	memset(fixture_disk, 0xa5, sizeof(fixture_disk));
	memset(write_results, 0, sizeof(write_results));
	memset(state, 0, sizeof(*state));
	memset(directory, 0, sizeof(*directory));
	memset(mount, 0, sizeof(*mount));
	memset(disk, 0, sizeof(*disk));
	read_calls = 0;
	write_calls = 0;
	inactive_consistency_calls = 0;

	state->super.bsize = FIXTURE_SECTOR_SIZE;
	state->super.fsize = FIXTURE_SECTOR_SIZE;
	state->super.frag = 1;
	state->super.fsbtodb = 0;
	state->super.size = FIXTURE_SECTORS;
	state->writable = 1;
	mount->m_data = state;
	mount->m_disk = disk;
	disk->d_block_size = FIXTURE_SECTOR_SIZE;
	disk->d_block_count = FIXTURE_SECTORS;
	directory->inode.i_mount = mount;
	directory->inode.i_type = INODE_DIR;
	directory->inode.i_size = UFS2_DIRBLKSIZ;
	directory->direct[0] = FIXTURE_DIRECTORY_FRAGMENT;

	block = directory_block();
	memset(block, 0, FIXTURE_SECTOR_SIZE);
	ufs2_put32(block, 0, FIXTURE_OLD_INO, 0);
	ufs2_put16(block, 4, UFS2_DIRBLKSIZ, 0);
	block[6] = 8;
	block[7] = 6;
	memcpy(block + 8, "victim", 6);
}

static void
test_ufs2_namespace_write_failure(void)
{
	static const struct componentname victim = { "victim", 6, 0 };
	struct fixture_ufs2_mount_state state;
	struct fixture_ufs2_inode_info directory;
	struct mount mount;
	struct disk disk;
	uint8_t original[FIXTURE_SECTOR_SIZE];
	uint32_t old_number;
	uint8_t old_type;

	initialize_directory(&state, &directory, &mount, &disk);
	memcpy(original, directory_block(), sizeof(original));
	write_results[0] = (struct fixture_write_result){ 0, 1 };
	CHECK_ERROR(ufs2_dir_replace(&directory.inode, &victim,
	    FIXTURE_NEW_INO, 4, &old_number, &old_type), 0);
	CHECK(old_number == FIXTURE_OLD_INO);
	CHECK(old_type == 8);
	CHECK(ufs2_get32(directory_block(), 0, 0) == FIXTURE_NEW_INO);
	CHECK(directory_block()[6] == 4);
	CHECK(memcmp(directory_block() + 8, "victim", 6) == 0);
	CHECK(read_calls == 1U);
	CHECK(write_calls == 1U);
	CHECK(inactive_consistency_calls == 0U);

	initialize_directory(&state, &directory, &mount, &disk);
	memcpy(original, directory_block(), sizeof(original));
	write_results[0] = (struct fixture_write_result){ EIO, 0 };
	write_results[1] = (struct fixture_write_result){ 0, 1 };
	CHECK_ERROR(ufs2_dir_replace(&directory.inode, &victim,
	    FIXTURE_NEW_INO, 4, &old_number, &old_type), EIO);
	CHECK(memcmp(directory_block(), original, sizeof(original)) == 0);
	CHECK(read_calls == 1U);
	CHECK(write_calls == 2U);
	CHECK(state.writable == 1);
	CHECK(inactive_consistency_calls == 0U);

	initialize_directory(&state, &directory, &mount, &disk);
	memcpy(original, directory_block(), sizeof(original));
	write_results[0] = (struct fixture_write_result){ EIO, 1 };
	write_results[1] = (struct fixture_write_result){ 0, 1 };
	CHECK_ERROR(ufs2_dir_replace(&directory.inode, &victim,
	    FIXTURE_NEW_INO, 4, &old_number, &old_type), EIO);
	CHECK(memcmp(directory_block(), original, sizeof(original)) == 0);
	CHECK(write_calls == 2U);
	CHECK(state.writable == 1);
	CHECK(inactive_consistency_calls == 0U);

	initialize_directory(&state, &directory, &mount, &disk);
	write_results[0] = (struct fixture_write_result){ EIO, 1 };
	write_results[1] = (struct fixture_write_result){ ENOSPC, 0 };
	CHECK_ERROR(ufs2_dir_replace(&directory.inode, &victim,
	    FIXTURE_NEW_INO, 4, &old_number, &old_type), ENOSPC);
	CHECK(write_calls == 2U);
	CHECK(state.writable == 0);
	CHECK(inactive_consistency_calls == 0U);
}

int
main(void)
{
	test_ufs2_namespace_write_failure();
	printf("ws001-p023 UFS2 namespace write/rollback: PASS (%u checks)\n",
	    checks);
	return EXIT_SUCCESS;
}

#elif defined(WS001_P016_OVERLAY)

/*
 * overlay_directory_fsync() is intentionally private.  This fixture links
 * that exact production function after making only its object symbol visible.
 * These layout mirrors hold the otherwise-private state needed to reach its
 * upper-directory, journal, and mount boundaries; they are not a public ABI.
 */
#define FIXTURE_OVERLAY_IDENTITY_MAX 128U
#define FIXTURE_OVERLAY_METADATA_MAX 128U

struct fixture_overlay_identity {
	ino_t ino;
	uint8_t state;
	char path[ZEDBSD_PATH_MAX];
};

struct fixture_overlay_metadata {
	uint8_t used;
	uint8_t flags;
	uint64_t sequence;
	char path[ZEDBSD_PATH_MAX];
};

struct fixture_overlay_mount_state {
	struct path upper_root;
	struct path lower_root;
	unsigned flags;
	ino_t next_ino;
	struct fixture_overlay_identity identities[FIXTURE_OVERLAY_IDENTITY_MAX];
	struct fixture_overlay_metadata metadata[FIXTURE_OVERLAY_METADATA_MAX];
	struct file *journal[2];
	unsigned active_slot;
	unsigned next_sector;
	uint64_t epoch;
	uint64_t sequence;
	uint32_t journal_generation;
	uint16_t temp_counter;
};

struct fixture_overlay_inode_info {
	struct path upper;
	struct path lower;
	unsigned identity_index;
	char path[ZEDBSD_PATH_MAX];
};

static char events[32];
static size_t event_count;
static int open_result;
static int upper_result;
static int close_result;
static int journal_result;
static int mount_result;
static struct path *expected_upper_path;
static struct file *expected_upper_file;
static struct file *expected_journal_file;
static struct mount *expected_upper_mount;

extern int overlay_directory_fsync(struct file *);

void
mutex_lock(struct mutex *mutex)
{
	(void)mutex;
}

void
mutex_unlock(struct mutex *mutex)
{
	(void)mutex;
}

void
path_init(struct path *path)
{
	memset(path, 0, sizeof(*path));
}

void
path_set(struct path *path, struct mount *mount, struct inode *inode)
{
	path->p_mount = mount;
	path->p_inode = inode;
}

void
path_release(struct path *path)
{
	path_init(path);
}

static void
event(char value)
{
	CHECK(event_count + 1U < sizeof(events));
	events[event_count++] = value;
	events[event_count] = '\0';
}

int
file_open_resolved(const struct path *resolved, int flags,
    struct file **result)
{
	CHECK(resolved != NULL);
	CHECK(resolved->p_mount == expected_upper_path->p_mount);
	CHECK(resolved->p_inode == expected_upper_path->p_inode);
	CHECK((flags & (O_RDONLY | O_DIRECTORY)) ==
	    (O_RDONLY | O_DIRECTORY));
	event('O');
	if (open_result != 0)
		return open_result;
	*result = expected_upper_file;
	return 0;
}

int
file_fsync(struct file *file)
{
	if (file == expected_upper_file) {
		event('U');
		return upper_result;
	}
	CHECK(file == expected_journal_file);
	event('J');
	return journal_result;
}

int
file_close(struct file *file)
{
	CHECK(file == expected_upper_file);
	event('C');
	return close_result;
}

int
mount_sync(struct mount *mount)
{
	CHECK(mount == expected_upper_mount);
	event('M');
	return mount_result;
}

static void
reset_order(void)
{
	memset(events, 0, sizeof(events));
	event_count = 0;
	open_result = 0;
	upper_result = 0;
	close_result = 0;
	journal_result = 0;
	mount_result = 0;
}

static void
test_overlay_order(void)
{
	struct fixture_overlay_mount_state state;
	struct fixture_overlay_inode_info info;
	struct mount overlay_mount;
	struct mount upper_mount;
	struct inode overlay_inode;
	struct inode upper_inode;
	struct file overlay_file;
	struct file upper_file;
	struct file journal_file;

	memset(&state, 0, sizeof(state));
	memset(&info, 0, sizeof(info));
	memset(&overlay_mount, 0, sizeof(overlay_mount));
	memset(&upper_mount, 0, sizeof(upper_mount));
	memset(&overlay_inode, 0, sizeof(overlay_inode));
	memset(&upper_inode, 0, sizeof(upper_inode));
	memset(&overlay_file, 0, sizeof(overlay_file));
	memset(&upper_file, 0, sizeof(upper_file));
	memset(&journal_file, 0, sizeof(journal_file));

	state.flags = OVERLAY_READ_WRITE;
	state.upper_root.p_mount = &upper_mount;
	state.journal[0] = &journal_file;
	state.active_slot = 0;
	info.upper.p_mount = &upper_mount;
	info.upper.p_inode = &upper_inode;
	overlay_mount.m_data = &state;
	overlay_inode.i_mount = &overlay_mount;
	overlay_inode.i_data = &info;
	overlay_file.f_inode = &overlay_inode;
	expected_upper_path = &info.upper;
	expected_upper_file = &upper_file;
	expected_journal_file = &journal_file;
	expected_upper_mount = &upper_mount;

	reset_order();
	CHECK_ERROR(overlay_directory_fsync(&overlay_file), 0);
	CHECK(strcmp(events, "OUCJM") == 0);

	reset_order();
	open_result = ENOENT;
	CHECK_ERROR(overlay_directory_fsync(&overlay_file), ENOENT);
	CHECK(strcmp(events, "O") == 0);

	reset_order();
	upper_result = EIO;
	CHECK_ERROR(overlay_directory_fsync(&overlay_file), EIO);
	CHECK(strcmp(events, "OUC") == 0);

	reset_order();
	close_result = EBADF;
	CHECK_ERROR(overlay_directory_fsync(&overlay_file), EBADF);
	CHECK(strcmp(events, "OUC") == 0);

	reset_order();
	journal_result = ENOSPC;
	CHECK_ERROR(overlay_directory_fsync(&overlay_file), ENOSPC);
	CHECK(strcmp(events, "OUCJ") == 0);

	reset_order();
	mount_result = EROFS;
	CHECK_ERROR(overlay_directory_fsync(&overlay_file), EROFS);
	CHECK(strcmp(events, "OUCJM") == 0);

	reset_order();
	info.upper.p_inode = NULL;
	CHECK_ERROR(overlay_directory_fsync(&overlay_file), 0);
	CHECK(strcmp(events, "JM") == 0);
	info.upper.p_inode = &upper_inode;

	reset_order();
	state.journal[0] = NULL;
	CHECK_ERROR(overlay_directory_fsync(&overlay_file), EIO);
	CHECK(strcmp(events, "OUC") == 0);
	state.journal[0] = &journal_file;

	reset_order();
	state.flags = OVERLAY_READ_ONLY;
	CHECK_ERROR(overlay_directory_fsync(&overlay_file), 0);
	CHECK(event_count == 0U);

	CHECK_ERROR(overlay_directory_fsync(NULL), EINVAL);
}

int
main(void)
{
	test_overlay_order();
	printf("ws001-p016 overlay order/error propagation: PASS (%u checks)\n",
	    checks);
	return EXIT_SUCCESS;
}

#else
#error select one WS001 p016 fixture
#endif
