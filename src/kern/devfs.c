/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The device filesystem.
 *
 * /dev is synthesized from the character device registry, the disk
 * registry, and the pseudo-terminal table.  Character device inodes are
 * ephemeral and own a reference on the device generation they name, so a
 * re-registered device gets a fresh inode.  The fixed shm, pts, and
 * input directories are recreated whenever they are evicted.  Block
 * device files do sector-granular I/O through a bounce buffer under a
 * backing mutation guard.
 */

#include "kern/devfs.h"
#include "kern/backing-claim.h"
#include "kern/block-identity.h"
#include "kern/buf.h"
#include "kern/cdev.h"
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/mount.h"
#include "kern/namei.h"
#include "kern/tty.h"
#include "kern/uaccess.h"
#include "kern/partition.h"
#include "kern/cred.h"

#include <zedbsd/block.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/statvfs.h>

#define DEVFS_BLOCK_INO_BASE 0x100000000ULL
#define DEVFS_NAME_MAX 32U
#define DEVFS_ENTRY_MAX (CDEV_MAX + DISK_MAX + 10U)
#define DEVFS_SHM_INO 2U
#define DEVFS_PTS_INO 3U
#define DEVFS_INPUT_INO 4U
#define DEVFS_PTS_INO_BASE 0x200000000ULL
#define DEVFS_CHAR_INO_BASE 0x300000000ULL
#define DEVFS_HIGH __attribute__((section(".hightext")))
#ifdef ZEDBSD_STORAGE_HOST_TEST
#undef DEVFS_HIGH
#define DEVFS_HIGH
#endif

typedef char devfs_ino_must_be_64_bit[(sizeof(ino_t) >= 8) ? 1 : -1];
#if !defined(ZEDBSD_DEVFS_HOST_TEST)
typedef char devfs_dev_must_fit_32_bit[(sizeof(dev_t) <= 4) ? 1 : -1];
#endif

enum devfs_block_io_direction {
	DEVFS_BLOCK_IO_READ,
	DEVFS_BLOCK_IO_WRITE
};

struct devfs_dir_entry {
	char name[DEVFS_NAME_MAX];
	ino_t ino;
	enum inode_type type;
	uint64_t generation;
	int character;
};

struct devfs_dir_state {
	unsigned count;
	struct devfs_dir_entry entries[DEVFS_ENTRY_MAX];
};

struct devfs_block_io_range {
	uint64_t device_bytes;
	uint64_t position;
	size_t length;
};

static DEVFS_HIGH int component_equal(const struct componentname *component, const char *text);
static DEVFS_HIGH int component_copy(const struct componentname *component, char *name, size_t capacity);
static DEVFS_HIGH int event_name(const char *name);
static DEVFS_HIGH int devfs_cdev_inode(struct inode *directory, struct cdev *device, struct inode **result);
static DEVFS_HIGH int devfs_fixed_inode(struct inode *directory, ino_t number, struct inode **result);
static DEVFS_HIGH int devfs_lookup(struct inode *directory, const struct componentname *component, struct inode **result);
static DEVFS_HIGH int devfs_getattr(struct inode *inode, struct stat *status);
static DEVFS_HIGH int dir_name_exists(const struct devfs_dir_state *state, const char *name);
static DEVFS_HIGH void devfs_directory_add_cdevs(struct devfs_dir_state *state, int input_directory);
static DEVFS_HIGH int devfs_dir_entry_live(const struct devfs_dir_entry *entry);
static DEVFS_HIGH int devfs_dir_open(struct file *file);
static DEVFS_HIGH int devfs_dir_close(struct file *file);
static DEVFS_HIGH int devfs_readdir(struct file *file, struct dirent *entry, int *eof);
static DEVFS_HIGH int block_open(struct file *file);
static DEVFS_HIGH int block_close(struct file *file);
static DEVFS_HIGH ssize_t block_pread(struct file *file, void *buffer, size_t length, off_t offset);
static DEVFS_HIGH ssize_t block_read(struct file *file, void *buffer, size_t length);
static DEVFS_HIGH ssize_t block_pwrite(struct file *file, const void *buffer, size_t length, off_t offset);
static DEVFS_HIGH ssize_t block_write(struct file *file, const void *buffer, size_t length);
static DEVFS_HIGH int block_fsync(struct file *file);
static DEVFS_HIGH int block_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static DEVFS_HIGH int devfs_mount_impl(struct mount *mountp);
static DEVFS_HIGH int devfs_statvfs(struct mount *mountp, struct statvfs *result);
static void devfs_cdev_release(void *data);
static void devfs_release_inode(struct inode *inode);

static const struct file_ops devfs_block_ops = {
	.open = block_open,
	.close = block_close,
	.read = block_read,
	.write = block_write,
	.pread = block_pread,
	.pwrite = block_pwrite,
	.ioctl = block_ioctl,
	.fsync = block_fsync,
};

static const struct inode_ops devfs_inode_ops = {
	.lookup = devfs_lookup,
	.getattr = devfs_getattr,
};

static const struct file_ops devfs_directory_ops = {
	.open = devfs_dir_open,
	.close = devfs_dir_close,
	.readdir = devfs_readdir,
};

const struct filesystem_type devfs_type = {
	.fs_name = "devfs",
	.fs_flags = FILESYSTEM_NODEV,
	.mount = devfs_mount_impl,
	.statvfs = devfs_statvfs,
};

/* Clamps one byte range to the device and reports the transfer it permits. */
static inline int
devfs_block_io_range_prepare(
	uint64_t block_count,
	uint32_t block_size,
	int64_t offset,
	size_t requested,
	enum devfs_block_io_direction direction,
	struct devfs_block_io_range *range)
{
	uint64_t device_bytes, position, available;

	/* Rejects a malformed request or an unknown direction. */
	if (range == NULL || block_count == 0U || block_size == 0U ||
	    offset < 0 || (direction != DEVFS_BLOCK_IO_READ &&
	    direction != DEVFS_BLOCK_IO_WRITE))
		return EINVAL;

	/* Measures the device and the request without overflowing. */
	if (block_count > UINT64_MAX / block_size)
		return EOVERFLOW;
	device_bytes = block_count * block_size;
	position = (uint64_t)offset;
	if (requested != 0U && (uint64_t)requested > UINT64_MAX - position)
		return EOVERFLOW;

	/* Describes the whole request before any clamping. */
	range->device_bytes = device_bytes;
	range->position = position;
	range->length = requested;
	if (requested == 0U)
		return 0;

	/* A read past the end is empty; a write past the end has no room. */
	if (position >= device_bytes) {
		range->length = 0U;
		return direction == DEVFS_BLOCK_IO_READ ? 0 : ENOSPC;
	}

	/* Shortens a read that runs off the end, and refuses such a write. */
	available = device_bytes - position;
	if ((uint64_t)requested > available) {
		if (direction == DEVFS_BLOCK_IO_WRITE)
			return ENOSPC;
		range->length = (size_t)available;
	}

	/* Reports the permitted transfer. */
	return 0;
}

/* Splits the next block-aligned piece out of a remaining byte range. */
static inline int
devfs_block_io_piece(
	uint64_t position,
	size_t remaining,
	uint32_t block_size,
	uint64_t *block,
	size_t *within,
	size_t *count)
{
	size_t offset, amount;

	/* Rejects a malformed request. */
	if (remaining == 0U || block_size == 0U || block == NULL ||
	    within == NULL || count == NULL)
		return EINVAL;

	/* Takes the part of the containing block that the range still needs. */
	offset = (size_t)(position % block_size);
	amount = (size_t)block_size - offset;
	if (amount > remaining)
		amount = remaining;
	*block = position / block_size;
	*within = offset;
	*count = amount;
	return 0;
}

/* Tests whether a path component equals a literal name. */
static DEVFS_HIGH int
component_equal(
	const struct componentname *component,
	const char *text)
{
	size_t length;

	/* The lengths and the bytes must agree. */
	length = strlen(text);
	if (component->cn_namelen != length)
		return 0;
	if (memcmp(component->cn_nameptr, text, length) != 0)
		return 0;

	/* Reports a matching component. */
	return 1;
}

/* Copies one bounded path component into a terminated device name. */
static DEVFS_HIGH int
component_copy(
	const struct componentname *component,
	char *name,
	size_t capacity)
{
	/* An empty or overlong component names no device. */
	if (component->cn_namelen == 0 ||
	    component->cn_namelen >= capacity)
		return ENOENT;

	/* Copies and terminates the name. */
	memcpy(name, component->cn_nameptr, component->cn_namelen);
	name[component->cn_namelen] = '\0';

	/* Reports the copied name. */
	return 0;
}

/* Identifies names owned by the /dev/input event namespace. */
static DEVFS_HIGH int
event_name(
	const char *name)
{
	/* Event nodes are named event<n>. */
	if (strncmp(name, "event", 5) != 0)
		return 0;
	return 1;
}

/* Creates one ephemeral inode which owns a cdev-generation reference. */
static DEVFS_HIGH int
devfs_cdev_inode(
	struct inode *directory,
	struct cdev *device,
	struct inode **result)
{
	struct inode *inode;
	uint64_t number;
	mode_t mode;

	/* Creates a generation-unique inode and gives it one cdev reference. */
	number = cdev_generation(device);
	if (number > UINT64_MAX - DEVFS_CHAR_INO_BASE)
		return EOVERFLOW;
	inode = inode_alloc(directory->i_mount);
	if (inode == NULL)
		return ENOSPC;

	/* Input event nodes are group-readable only. */
	if (event_name(device->name))
		mode = 0640U;
	else
		mode = 0666U;

	/* Describes the device; the inode starts dead until it is published. */
	cdev_ref(device);
	inode->i_type = INODE_CHAR;
	inode->i_ino = (ino_t)(DEVFS_CHAR_INO_BASE + number);
	inode->i_op = directory->i_op;
	inode->i_fop = &cdev_file_ops;
	inode->i_data = device;
	inode->i_special = device;
	inode->i_special_destroy = devfs_cdev_release;
	inode->i_linkcount = 1;
	inode->i_mode = S_IFCHR | mode;
	inode->i_rdev = device->rdev;
	inode->i_flags = INODE_DEAD;

	/* Closes allocation/unpublish races before exposing the ephemeral inode. */
	if (!cdev_is_published(device)) {
		devfs_release_inode(inode);
		return ENOENT;
	}

	*result = inode;

	/* Reports the created inode. */
	return 0;
}

/* Gets or recreates one evictable fixed devfs directory inode. */
static DEVFS_HIGH int
devfs_fixed_inode(
	struct inode *directory,
	ino_t number,
	struct inode **result)
{
	struct inode *inode;
	int error;

	/* Rejects a missing operand or a number that is not a fixed directory. */
	if (directory == NULL ||
	    result == NULL ||
	    (number != DEVFS_SHM_INO &&
	     number != DEVFS_PTS_INO &&
	     number != DEVFS_INPUT_INO))
		return EINVAL;

	/* The mount-local mutex permits allocation and victim reclaim to sleep. */
	mutex_lock(&directory->i_mount->m_lock);

	error = inode_get(directory->i_mount, number, &inode);
	if (error != 0) {
		/* Recreates an evicted directory with the root's operations. */
		inode = inode_alloc(directory->i_mount);
		if (inode == NULL) {
			mutex_unlock(&directory->i_mount->m_lock);
			return ENOSPC;
		}

		inode->i_type = INODE_DIR;
		inode->i_ino = number;
		inode->i_op = directory->i_op;
		inode->i_fop = directory->i_fop;
		inode->i_linkcount = 2;
		inode->i_mode = S_IFDIR | 0555U;
		inode->i_flags = INODE_NOCACHE_CHILDREN;
	}

	mutex_unlock(&directory->i_mount->m_lock);

	*result = inode;

	/* Reports the directory inode. */
	return 0;
}

/* Looks a name up in the root or one of the fixed directories. */
static DEVFS_HIGH int
devfs_lookup(
	struct inode *directory,
	const struct componentname *component,
	struct inode **result)
{
	struct cdev *device;
	struct disk_info info;
	struct inode *inode;
	char name[DEVFS_NAME_MAX];
	unsigned i;
	uint64_t number;
	unsigned char digit;
	int error;

	/* Dot stays put; dot-dot in a fixed directory goes to the root. */
	if (component_equal(component, ".") || component_equal(component, "..")) {
		if (component_equal(component, "..") &&
		    (directory->i_ino == DEVFS_SHM_INO ||
		     directory->i_ino == DEVFS_PTS_INO ||
		     directory->i_ino == DEVFS_INPUT_INO)) {
			error = inode_get(directory->i_mount, 1, result);
			return error;
		}

		inode_ref(directory);
		*result = directory;
		return 0;
	}

	/* /dev/pts names slave terminals by their decimal index. */
	if (directory->i_ino == DEVFS_PTS_INO) {
		number = 0;
		if (component->cn_namelen == 0 || component->cn_namelen > 3U)
			return ENOENT;
		for (i = 0; i < component->cn_namelen; i++) {
			digit = (unsigned char)component->cn_nameptr[i];
			if (digit < '0' || digit > '9')
				return ENOENT;
			number = number * 10U + (digit - '0');
		}

		if (number > UINT32_MAX || !tty_pty_exists((unsigned)number))
			return ENOENT;

		/* Creates the slave inode unless it is cached. */
		if (inode_get(directory->i_mount,
		    (ino_t)(DEVFS_PTS_INO_BASE + number), &inode) != 0) {
			inode = inode_alloc(directory->i_mount);
			if (inode == NULL)
				return ENOSPC;
			inode->i_type = INODE_CHAR;
			inode->i_ino = (ino_t)(DEVFS_PTS_INO_BASE + number);
			inode->i_op = directory->i_op;
			inode->i_fop = &tty_pty_slave_file_ops;
			inode->i_data = (void *)(uintptr_t)(number + 1U);
			inode->i_linkcount = 1;
			inode->i_mode = S_IFCHR | 0620U;
			inode->i_rdev = (dev_t)(0x00020000U + number);
		}

		*result = inode;
		return 0;
	}

	/* Every other name must fit a device name. */
	error = component_copy(component, name, sizeof(name));
	if (error != 0)
		return error;

	/* /dev/input holds only the event devices. */
	if (directory->i_ino == DEVFS_INPUT_INO) {
		if (!event_name(name))
			return ENOENT;
		device = cdev_find_ref(name);
		if (device == NULL)
			return ENOENT;
		error = devfs_cdev_inode(directory, device, result);
		cdev_release(device);
		return error;
	}

	/* The fixed directories live in the root. */
	if (component_equal(component, "shm")) {
		error = devfs_fixed_inode(directory, DEVFS_SHM_INO, result);
		return error;
	}

	if (component_equal(component, "pts")) {
		error = devfs_fixed_inode(directory, DEVFS_PTS_INO, result);
		return error;
	}

	if (component_equal(component, "input")) {
		error = devfs_fixed_inode(directory, DEVFS_INPUT_INO, result);
		return error;
	}

	/* Keeps input event nodes exclusively below /dev/input. */
	device = cdev_find_ref(name);
	if (device != NULL && !event_name(name)) {
		error = devfs_cdev_inode(directory, device, result);
		cdev_release(device);
		return error;
	}

	if (device != NULL)
		cdev_release(device);

	/* Anything else must be a disk. */
	if (component->cn_namelen >= sizeof(name))
		return ENOENT;
	if (disk_get_info(name, &info) != 0)
		return ENOENT;

	/* Creates the block inode unless it is cached. */
	if (inode_get(directory->i_mount,
	    (ino_t)(DEVFS_BLOCK_INO_BASE + (uint64_t)info.dev), &inode) != 0) {
		inode = inode_alloc(directory->i_mount);
		if (inode == NULL)
			return ENOSPC;
		inode->i_type = INODE_BLOCK;
		inode->i_ino = (ino_t)(DEVFS_BLOCK_INO_BASE + (uint64_t)info.dev);
		inode->i_op = directory->i_op;
		inode->i_fop = &devfs_block_ops;
		inode->i_linkcount = 1;
		inode->i_mode = S_IFBLK | 0600U;
		inode->i_rdev = info.dev;
	}

	*result = inode;

	/* Reports the block inode. */
	return 0;
}

/* Describes a devfs inode from its own fields. */
static DEVFS_HIGH int
devfs_getattr(
	struct inode *inode,
	struct stat *status)
{
	memset(status, 0, sizeof(*status));
	status->st_ino = inode->i_ino;
	status->st_mode = inode->i_mode;
	status->st_nlink = inode->i_linkcount;
	status->st_rdev = inode->i_rdev;
	status->st_uid = inode->i_uid;
	status->st_gid = inode->i_gid;
	status->st_blksize = 512;

	/* Reports the described inode. */
	return 0;
}

/* Tests whether a directory snapshot already holds a name. */
static DEVFS_HIGH int
dir_name_exists(
	const struct devfs_dir_state *state,
	const char *name)
{
	unsigned i;

	/* Searches the snapshot in order. */
	for (i = 0; i < state->count; i++) {
		if (!strcmp(state->entries[i].name, name))
			return 1;
	}

	/* Reports a new name. */
	return 0;
}

/* Copies the current cdev namespace into one bounded directory snapshot. */
static DEVFS_HIGH void
devfs_directory_add_cdevs(
	struct devfs_dir_state *state,
	int input_directory)
{
	struct cdev *snapshot[CDEV_MAX];
	struct cdev *device;
	struct devfs_dir_entry *entry;
	uint64_t generation;
	unsigned count;
	unsigned index;
	int input;

	/* Lists the devices that belong in this directory, with their generation. */
	count = cdev_snapshot(snapshot, CDEV_MAX);
	for (index = 0; index < count; index++) {
		device = snapshot[index];
		input = event_name(device->name);
		generation = cdev_generation(device);
		if (input == input_directory &&
		    generation <= UINT64_MAX - DEVFS_CHAR_INO_BASE &&
		    state->count < DEVFS_ENTRY_MAX) {
			entry = &state->entries[state->count++];
			memcpy(entry->name, device->name, DEVFS_NAME_MAX);
			entry->name[DEVFS_NAME_MAX - 1U] = '\0';
			entry->ino = (ino_t)(DEVFS_CHAR_INO_BASE + generation);
			entry->type = INODE_CHAR;
			entry->generation = generation;
			entry->character = 1;
		}

		cdev_release(device);
	}
}

/* Revalidates an old directory entry against the visible cdev generation. */
static DEVFS_HIGH int
devfs_dir_entry_live(
	const struct devfs_dir_entry *entry)
{
	struct cdev *device;
	int live;

	/* Only character devices can be unpublished. */
	if (!entry->character)
		return 1;

	/* The name must still be published under the same generation. */
	device = cdev_find_ref(entry->name);
	if (device == NULL)
		return 0;
	live = cdev_generation(device) == entry->generation;
	cdev_release(device);

	/* Reports whether the entry is still valid. */
	return live;
}

/* Snapshots a directory's entries into the open file. */
static DEVFS_HIGH int
devfs_dir_open(
	struct file *file)
{
	struct disk_info disks[DISK_MAX];
	struct devfs_dir_state *state;
	struct devfs_dir_entry *entry;
	unsigned indices[8];
	unsigned count;
	unsigned disk_count;
	unsigned index;
	unsigned value;
	unsigned used;
	unsigned digit_index;
	char digits[4];
	int error;

	disk_count = 0;

	/* Allocates an empty snapshot. */
	state = kern_malloc(sizeof(*state));
	if (state == NULL)
		return ENFILE;
	memset(state, 0, sizeof(*state));

	/* /dev/pts lists the live slave terminals by decimal index. */
	if (file->f_inode->i_ino == DEVFS_PTS_INO) {
		count = tty_pty_snapshot(indices,
		    sizeof(indices) / sizeof(indices[0]));
		for (index = 0;
		     index < count && index < sizeof(indices) / sizeof(indices[0]);
		     index++) {
			entry = &state->entries[state->count++];

			/* Renders the index in decimal, least significant digit first. */
			value = indices[index];
			used = 0;
			do {
				digits[used++] = (char)('0' + value % 10U);
				value /= 10U;
			} while (value != 0);
			for (digit_index = 0; digit_index < used; digit_index++)
				entry->name[digit_index] =
				    digits[used - digit_index - 1U];
			entry->name[used] = '\0';
			entry->ino =
			    (ino_t)(DEVFS_PTS_INO_BASE + indices[index]);
			entry->type = INODE_CHAR;
		}

		file->f_data = state;
		return 0;
	}

	/* /dev/input lists the event devices. */
	if (file->f_inode->i_ino == DEVFS_INPUT_INO) {
		devfs_directory_add_cdevs(state, 1);
		file->f_data = state;
		return 0;
	}

	/* The root lists the fixed directories, the other devices, and the disks. */
	strcpy(state->entries[state->count].name, "shm");
	state->entries[state->count].ino = DEVFS_SHM_INO;
	state->entries[state->count].type = INODE_DIR;
	state->count++;
	strcpy(state->entries[state->count].name, "pts");
	state->entries[state->count].ino = DEVFS_PTS_INO;
	state->entries[state->count].type = INODE_DIR;
	state->count++;
	strcpy(state->entries[state->count].name, "input");
	state->entries[state->count].ino = DEVFS_INPUT_INO;
	state->entries[state->count].type = INODE_DIR;
	state->count++;
	devfs_directory_add_cdevs(state, 0);
	error = disk_registry_snapshot(disks, DISK_MAX, &disk_count);
	if (error != 0) {
		kern_free(state);
		return error;
	}

	/* A disk whose name a character device took is hidden. */
	for (index = 0; index < disk_count; index++) {
		if (dir_name_exists(state, disks[index].name))
			continue;
		entry = &state->entries[state->count++];
		memcpy(entry->name, disks[index].name, DEVFS_NAME_MAX);
		entry->name[DEVFS_NAME_MAX - 1U] = '\0';
		entry->ino = (ino_t)(DEVFS_BLOCK_INO_BASE +
		    (uint64_t)disks[index].dev);
		entry->type = INODE_BLOCK;
	}

	file->f_data = state;

	/* Reports the opened directory. */
	return 0;
}

/* Frees a directory snapshot. */
static DEVFS_HIGH int
devfs_dir_close(
	struct file *file)
{
	/* Frees the snapshot when the open succeeded. */
	if (file->f_data != NULL)
		kern_free(file->f_data);
	file->f_data = NULL;

	/* Reports the closed directory. */
	return 0;
}

/* Reads the next live entry of a directory snapshot. */
static DEVFS_HIGH int
devfs_readdir(
	struct file *file,
	struct dirent *entry,
	int *eof)
{
	struct devfs_dir_state *state;
	unsigned index;

	/* A directory without a snapshot was never opened. */
	state = file->f_data;
	if (state == NULL)
		return EIO;

	/* Skips any generation unpublished after this directory was opened. */
	index = (unsigned)file->f_offset;
	while (index < state->count &&
	       !devfs_dir_entry_live(&state->entries[index])) {
		index++;
		file->f_offset = (off_t)index;
	}

	/* Reports the end of the snapshot. */
	if (index >= state->count) {
		*eof = 1;
		return 0;
	}

	/* Copies the entry and advances. */
	memset(entry, 0, sizeof(*entry));
	entry->d_ino = state->entries[index].ino;
	entry->d_type = state->entries[index].type;
	strcpy(entry->d_name, state->entries[index].name);
	file->f_offset++;
	*eof = 0;

	/* Reports the read entry. */
	return 0;
}

/* Opens the disk behind a block device file. */
static DEVFS_HIGH int
block_open(
	struct file *file)
{
	struct disk *disk;
	int error;

	/* Opens the disk by device number. */
	error = disk_open_by_dev(file->f_inode->i_rdev, &disk);
	if (error != 0)
		return error;

	/* The bounce buffer supports the published 512-byte and 4096-byte sectors. */
	if (disk->d_block_size != 512U && disk->d_block_size != 4096U) {
		disk_close(disk);
		return EOPNOTSUPP;
	}

	file->f_data = disk;

	/* Reports the opened disk. */
	return 0;
}

/* Closes the disk behind a block device file. */
static DEVFS_HIGH int
block_close(
	struct file *file)
{
	/* Final close owns the description; no ioctl or I/O can still borrow it. */
	if (file->f_block_claim != NULL) {
		disk_admin_end(file->f_data, file->f_block_claim);
		backing_claim_release(file->f_block_claim);
		file->f_block_claim = NULL;
	}

	/* Closes the disk when the open succeeded. */
	if (file->f_data != NULL)
		disk_close(file->f_data);
	file->f_data = NULL;

	/* Reports the closed disk. */
	return 0;
}

/* Reads bytes from a disk at an offset through the bounce buffer. */
static DEVFS_HIGH ssize_t
block_pread_data(
	struct file *file,
	void *buffer,
	size_t length,
	off_t offset)
{
	struct disk *disk;
	struct devfs_block_io_range range;
	uint8_t bounce[4096];
	uint8_t *output;
	uint64_t position;
	uint64_t block;
	uint32_t block_size;
	size_t total;
	size_t within;
	size_t count;
	int error;

	disk = file->f_data;
	output = buffer;
	total = 0;

	/* Rejects a file without a disk or a disk with another sector size. */
	if (disk == NULL)
		return -EINVAL;
	block_size = disk->d_block_size;
	if (block_size != 512U && block_size != 4096U)
		return -EOPNOTSUPP;

	/* Clips the range to the disk. */
	error = devfs_block_io_range_prepare(disk->d_block_count, block_size,
	    (int64_t)offset, length, DEVFS_BLOCK_IO_READ, &range);
	if (error != 0)
		return -error;
	position = range.position;
	length = range.length;

	/* Reads one sector at a time, reporting a short read on a late error. */
	while (total < length) {
		error = devfs_block_io_piece(position, length - total,
		    block_size, &block, &within, &count);
		if (error != 0) {
			if (total != 0)
				return (ssize_t)total;
			return -error;
		}

		if (file->f_block_claim != NULL)
			error = disk_read_direct(disk, block, 1, bounce);
		else
			error = disk_read(disk, block, 1, bounce);
		if (error != 0) {
			if (total != 0)
				return (ssize_t)total;
			return -error;
		}

		memcpy(output + total, bounce + within, count);
		total += count;
		position += count;
	}

	/* Reports the bytes read. */
	return (ssize_t)total;
}

static DEVFS_HIGH ssize_t
block_pread(struct file *file, void *buffer, size_t length, off_t offset)
{
	ssize_t result;
	int error;

	if (file->f_block_claim == NULL)
		return block_pread_data(file, buffer, length, offset);
	error = disk_admin_io_begin(file->f_data, file->f_block_claim);
	if (error != 0)
		return -error;
	result = block_pread_data(file, buffer, length, offset);
	disk_admin_io_end(file->f_data, file->f_block_claim);
	return result;
}

/* Reads from a disk at the file offset and advances it. */
static DEVFS_HIGH ssize_t
block_read(
	struct file *file,
	void *buffer,
	size_t length)
{
	ssize_t done;

	/* Reads at the current offset and advances past what was read. */
	done = block_pread(file, buffer, length, file->f_offset);
	if (done > 0)
		file->f_offset += done;

	/* Reports the read result. */
	return done;
}

/* Writes bytes to a disk at an offset through the bounce buffer. */
static DEVFS_HIGH ssize_t
block_pwrite_data(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset)
{
	struct disk *disk;
	struct backing_mutation_guard guard;
	struct devfs_block_io_range range;
	uint8_t bounce[4096];
	const uint8_t *input;
	uint64_t position;
	uint64_t first;
	uint64_t last;
	uint64_t block;
	uint32_t block_size;
	size_t total;
	size_t within;
	size_t count;
	int error;

	disk = file->f_data;
	input = buffer;
	total = 0;

	/* Rejects a missing, read-only, or oddly sectored disk. */
	if (disk == NULL)
		return -EINVAL;
	if ((disk->d_flags & DISK_READ_ONLY) != 0)
		return -EROFS;
	block_size = disk->d_block_size;
	if (block_size != 512U && block_size != 4096U)
		return -EOPNOTSUPP;

	/* Clips the range to the disk. */
	error = devfs_block_io_range_prepare(disk->d_block_count, block_size,
	    (int64_t)offset, length, DEVFS_BLOCK_IO_WRITE, &range);
	if (error != 0)
		return -error;
	position = range.position;

	/* Excludes other mutations of the touched sectors. */
	length = range.length;
	if (length != 0) {
		first = position / block_size;
		last = (position + length - 1U) / block_size;
		error = backing_mutation_begin_disk(disk, first,
		    last - first + 1U, file->f_block_claim, &guard);
		if (error != 0)
			return -error;
	} else {
		memset(&guard, 0, sizeof(guard));
	}
	/* Reserved I/O bypasses the cache. Invalidate overlapping clean lines
	 * while foreign cache admissions and descriptor operations are excluded. */
	if (file->f_block_claim != NULL && length != 0) {
		error = buf_invalidate(disk, first, last - first + 1U, 0);
		if (error != 0) {
			backing_mutation_end(&guard);
			return -error;
		}
	}

	/* Writes one sector at a time, merging partial sectors with the disk. */
	while (total < length) {
		error = devfs_block_io_piece(position, length - total,
		    block_size, &block, &within, &count);
		if (error != 0) {
			backing_mutation_end(&guard);
			if (total != 0)
				return (ssize_t)total;
			return -error;
		}

		if (within != 0 || count != block_size) {
			if (file->f_block_claim != NULL)
				error = disk_read_direct(disk, block, 1, bounce);
			else
				error = disk_read(disk, block, 1, bounce);
			if (error != 0) {
				backing_mutation_end(&guard);
				if (total != 0)
					return (ssize_t)total;
				return -error;
			}
		} else {
			memset(bounce, 0, sizeof(bounce));
		}

		memcpy(bounce + within, input + total, count);
		if (file->f_block_claim != NULL)
			error = disk_write_direct_claimed(disk, block, 1, bounce, file->f_block_claim);
		else
			error = disk_write(disk, block, 1, bounce);
		if (error != 0) {
			backing_mutation_end(&guard);
			if (total != 0)
				return (ssize_t)total;
			return -error;
		}

		total += count;
		position += count;
	}

	backing_mutation_end(&guard);

	/* Reports the bytes written. */
	return (ssize_t)total;
}

static DEVFS_HIGH ssize_t
block_pwrite(struct file *file, const void *buffer, size_t length, off_t offset)
{
	ssize_t result;
	int error;

	if (file->f_block_claim == NULL)
		return block_pwrite_data(file, buffer, length, offset);
	error = disk_admin_io_begin(file->f_data, file->f_block_claim);
	if (error != 0)
		return -error;
	result = block_pwrite_data(file, buffer, length, offset);
	disk_admin_io_end(file->f_data, file->f_block_claim);
	return result;
}

/* Writes to a disk at the file offset and advances it. */
static DEVFS_HIGH ssize_t
block_write(
	struct file *file,
	const void *buffer,
	size_t length)
{
	ssize_t done;

	/* Writes at the current offset and advances past what was written. */
	done = block_pwrite(file, buffer, length, file->f_offset);
	if (done > 0)
		file->f_offset += done;

	/* Reports the write result. */
	return done;
}

/* Flushes the disk behind a block device file. */
static DEVFS_HIGH int
block_fsync(
	struct file *file)
{
	int error;

	/* A file whose disk is gone cannot be flushed. */
	if (file->f_data == NULL)
		return ENXIO;

	/* Flushes the disk. */
	if (file->f_block_claim != NULL) {
		error = disk_admin_io_begin(file->f_data, file->f_block_claim);
		if (error != 0)
			return error;
	}
	error = disk_sync(file->f_data);
	if (file->f_block_claim != NULL)
		disk_admin_io_end(file->f_data, file->f_block_claim);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Acquires the raw claim and open gate together, or releases both on failure.
 * The caller holds f_lock, so descriptor I/O cannot race publication. */
static DEVFS_HIGH int
block_reserve(
	struct file *file,
	uintptr_t argument)
{
	struct zedbsd_block_info expected;
	struct zedbsd_block_info current;
	struct backing_claim *claim;
	struct disk *disk;
	int error;

	if ((file_status_flags_get(file) & O_ACCMODE) != O_RDWR)
		return EBADF;
	if (file->f_block_claim != NULL)
		return EBUSY;
	error = copyin(argument, &expected, sizeof(expected));
	if (error != 0)
		return error;
	disk = file->f_data;
	current = expected;
	error = disk_block_info(disk, &current);
	if (error != 0)
		return error;
	if (memcmp(&current, &expected, sizeof(current)) != 0)
		return ESTALE;
	if (disk->d_media_backing != NULL || (disk->d_flags & DISK_FILE_BACKED) != 0 ||
	    (disk->d_parent != NULL && (disk->d_parent->d_parent != NULL ||
	    (disk->d_flags & DISK_PARTITION) == 0 ||
	    (disk->d_parent->d_flags & DISK_FILE_BACKED) != 0)))
		return EOPNOTSUPP;
	if ((disk->d_flags & DISK_READ_ONLY) != 0)
		return EROFS;

	/* The claim closes the writer race before the registry closes new opens. */
	error = backing_claim_prepare_disk(disk, 0, disk->d_block_count,
	    BACKING_CLAIM_ADMIN, &claim);
	if (error != 0)
		return error;
	error = disk_admin_begin(disk, claim);
	if (error != 0) {
		backing_claim_release(claim);
		return error;
	}
	/* Drop old clean aliases before any direct write. A retained dirty line
	 * cannot write without its old authority, so invalidation refuses safely. */
	error = disk_admin_io_begin(disk, claim);
	if (error == 0) {
		error = buf_invalidate_disk(disk, 0);
		disk_admin_io_end(disk, claim);
	}
	if (error != 0) {
		disk_admin_end(disk, claim);
		backing_claim_release(claim);
		return error;
	}
	file->f_block_claim = claim;
	return 0;
}

/* Answers the identity query here and forwards other ioctls to the disk. */
static DEVFS_HIGH int
block_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	struct zedbsd_block_info info;
	struct ucred *cred;
	int allowed;
	int error;

	/* Requires privilege before validating an administrative reload request. */
	if (request == BLKREREADPART || request == BLKRESERVE) {
		cred = cred_current_ref();
		allowed = cred_is_superuser(cred);
		cred_release(cred);
		if (!allowed)
			return EPERM;

		/* These administrative ioctls serialize with the description's I/O. */
		mutex_lock(&file->f_lock);
		if (request == BLKRESERVE) {
			error = block_reserve(file, argument);
		} else if (argument != 0) {
			error = EINVAL;
		} else if (file->f_block_claim != NULL) {
			error = disk_admin_io_begin(file->f_data, file->f_block_claim);
			if (error == 0) {
				error = partition_reload_claimed(file->f_data, file->f_block_claim);
				disk_admin_io_end(file->f_data, file->f_block_claim);
			}
		} else {
			error = partition_reload_claimed(file->f_data, file->f_block_claim);
		}
		mutex_unlock(&file->f_lock);
		return error;
	}

	/* Copies the versioned geometry query through the user-access boundary. */
	if (request == BLKGETINFO) {
		error = copyin(argument, &info, sizeof(info));
		if (error != 0)
			return error;
		error = disk_block_info(file->f_data, &info);
		if (error != 0)
			return error;
		error = copyout(&info, argument, sizeof(info));
		return error;
	}

	/* A file whose disk is gone answers nothing. */
	if (file->f_data == NULL)
		return ENXIO;

	/* The identity query needs a result to fill. */
	if (request == BLKGETIDENTITY) {
		if (argument == 0)
			return EFAULT;
		error = block_identity_get(file->f_data,
		    (struct block_identity *)argument);
		return error;
	}

	/* Forwards everything else to the disk. */
	error = disk_ioctl(file->f_data, request, (void *)argument);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Builds the root and fixed directories of a new devfs mount. */
static DEVFS_HIGH int
devfs_mount_impl(
	struct mount *mountp)
{
	struct inode *input;
	struct inode *pts;
	struct inode *root;
	struct inode *shm;

	root = NULL;
	shm = NULL;
	pts = NULL;
	input = NULL;

	/* Constructs every fixed directory before publishing the mount root. */
	root = inode_alloc(mountp);
	if (root == NULL)
		goto no_space;

	root->i_type = INODE_DIR;
	root->i_ino = 1;
	root->i_op = &devfs_inode_ops;
	root->i_fop = &devfs_directory_ops;
	root->i_linkcount = 1;
	root->i_mode = S_IFDIR | 0555U;
	root->i_flags = INODE_NOCACHE_CHILDREN;

	if (devfs_fixed_inode(root, DEVFS_SHM_INO, &shm) != 0)
		goto no_space;

	if (devfs_fixed_inode(root, DEVFS_PTS_INO, &pts) != 0)
		goto no_space;

	if (devfs_fixed_inode(root, DEVFS_INPUT_INO, &input) != 0)
		goto no_space;

	/* Transfers root ownership and leaves fixed subdirs safely evictable. */
	root->i_flags |= INODE_ROOT;
	mountp->m_root = root;
	inode_release(shm);
	inode_release(pts);
	inode_release(input);

	/* Reports the mounted filesystem. */
	return 0;

no_space:
	devfs_release_inode(input);
	devfs_release_inode(pts);
	devfs_release_inode(shm);
	devfs_release_inode(root);

	/* Reports the exhausted inode space. */
	return ENOSPC;
}

/* Describes the devfs mount by its entry capacity. */
static DEVFS_HIGH int
devfs_statvfs(
	struct mount *mountp,
	struct statvfs *result)
{
	unsigned character_count;

	(void)mountp;

	/* Rejects a missing result. */
	if (result == NULL)
		return EINVAL;

	/* Reports the entries left after the character devices. */
	character_count = cdev_count();

	memset(result, 0, sizeof(*result));
	result->f_bsize = 1U;
	result->f_frsize = 1U;
	result->f_files = DEVFS_ENTRY_MAX;

	if (character_count < DEVFS_ENTRY_MAX)
		result->f_ffree = DEVFS_ENTRY_MAX - character_count;
	else
		result->f_ffree = 0;

	result->f_favail = result->f_ffree;
	result->f_namemax = DEVFS_NAME_MAX - 1U;

	/* Reports the description. */
	return 0;
}

/* Releases the cdev reference held by one devfs character inode. */
static void
devfs_cdev_release(
	void *data)
{
	cdev_release(data);
}

/* Marks an unpublished construction dead and drops its caller reference. */
static void
devfs_release_inode(
	struct inode *inode)
{
	/* Ignores an inode that was never allocated. */
	if (inode == NULL)
		return;

	/* A dead inode is not cached when its last reference goes. */
	inode->i_flags |= INODE_DEAD;
	inode_release(inode);
}
