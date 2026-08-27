/*
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */

#include "kern/devfs.h"
#include "kern/backing-claim.h"
#include "kern/block-identity.h"
#include "kern/cdev.h"
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/mount.h"
#include "kern/namei.h"
#include "kern/tty.h"

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
#define DEVFS_HIGH __attribute__((section(".hightext")))

typedef char devfs_ino_must_be_64_bit[(sizeof(ino_t) >= 8) ? 1 : -1];
typedef char devfs_dev_must_fit_32_bit[(sizeof(dev_t) <= 4) ? 1 : -1];

struct devfs_node {
	const struct cdev *device;
	struct inode *inode;
};

struct devfs_dir_entry {
	char name[DEVFS_NAME_MAX];
	ino_t ino;
	enum inode_type type;
};

struct devfs_dir_state {
	unsigned count;
	struct devfs_dir_entry entries[DEVFS_ENTRY_MAX];
};

static struct devfs_node nodes[CDEV_MAX] __attribute__((section(".vfs_bss")));
static unsigned node_count __attribute__((section(".vfs_bss")));
static struct inode *devfs_root __attribute__((section(".vfs_bss")));
static struct inode *devfs_shm __attribute__((section(".vfs_bss")));
static struct inode *devfs_pts __attribute__((section(".vfs_bss")));
static struct inode *devfs_input __attribute__((section(".vfs_bss")));
static const struct file_ops devfs_block_ops;

static DEVFS_HIGH int
component_equal(const struct componentname *component, const char *text)
{
	size_t length = strlen(text);
	return component->cn_namelen == length &&
		!memcmp(component->cn_nameptr, text, length);
}

static DEVFS_HIGH int
devfs_lookup(struct inode *directory, const struct componentname *component,
	     struct inode **result)
{
	struct disk_info info;
	struct inode *inode;
	char name[DEVFS_NAME_MAX];
	unsigned i;

	if (component_equal(component, ".") || component_equal(component, "..")) {
		struct inode *target = component_equal(component, "..") &&
		    (directory == devfs_pts || directory == devfs_input)
		    ? devfs_root
		    : directory;
		inode_ref(target);
		*result = target;
		return 0;
	}
	if (directory == devfs_pts) {
		uint64_t number = 0;
		if (component->cn_namelen == 0 || component->cn_namelen > 3U)
			return ENOENT;
		for (i = 0; i < component->cn_namelen; i++) {
			unsigned char digit =
			    (unsigned char)component->cn_nameptr[i];
			if (digit < '0' || digit > '9')
				return ENOENT;
			number = number * 10U + (digit - '0');
		}
		if (number > UINT32_MAX || !tty_pty_exists((unsigned)number))
			return ENOENT;
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
	if (directory == devfs_input) {
		for (i = 0; i < node_count; i++)
			if (!strncmp(nodes[i].device->name, "event", 5) &&
			    component_equal(component, nodes[i].device->name)) {
				inode_ref(nodes[i].inode);
				*result = nodes[i].inode;
				return 0;
			}
		return ENOENT;
	}
	if (component_equal(component, "shm")) {
		inode_ref(devfs_shm);
		*result = devfs_shm;
		return 0;
	}
	if (component_equal(component, "pts")) {
		inode_ref(devfs_pts);
		*result = devfs_pts;
		return 0;
	}
	if (component_equal(component, "input")) {
		inode_ref(devfs_input);
		*result = devfs_input;
		return 0;
	}
	for (i = 0; i < node_count; i++)
		if (component_equal(component, nodes[i].device->name)) {
			inode_ref(nodes[i].inode);
			*result = nodes[i].inode;
			return 0;
		}
	if (component->cn_namelen >= sizeof(name))
		return ENOENT;
	memcpy(name, component->cn_nameptr, component->cn_namelen);
	name[component->cn_namelen] = '\0';
	if (disk_get_info(name, &info) != 0)
		return ENOENT;
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
	return 0;
}

static DEVFS_HIGH int
devfs_getattr(struct inode *inode, struct stat *status)
{
	memset(status, 0, sizeof(*status));
	status->st_ino = inode->i_ino;
	status->st_mode = inode->i_mode;
	status->st_nlink = inode->i_linkcount;
	status->st_rdev = inode->i_rdev;
	status->st_uid = inode->i_uid;
	status->st_gid = inode->i_gid;
	status->st_blksize = 512;
	return 0;
}

static DEVFS_HIGH int
dir_name_exists(const struct devfs_dir_state *state, const char *name)
{
	unsigned i;
	for (i = 0; i < state->count; i++)
		if (!strcmp(state->entries[i].name, name))
			return 1;
	return 0;
}

static DEVFS_HIGH int
devfs_dir_open(struct file *file)
{
	struct disk_info disks[DISK_MAX];
	struct devfs_dir_state *state;
	unsigned i, disk_count = 0;
	int error;

	state = kern_malloc(sizeof(*state));
	if (state == NULL)
		return ENFILE;
	memset(state, 0, sizeof(*state));
	if (file->f_inode == devfs_pts) {
		unsigned indices[8];
		unsigned count = tty_pty_snapshot(indices,
		    sizeof(indices) / sizeof(indices[0]));
		for (i = 0; i < count && i < sizeof(indices) / sizeof(indices[0]);
		    i++) {
			struct devfs_dir_entry *entry =
			    &state->entries[state->count++];
			unsigned value = indices[i];
			char digits[4];
			unsigned used = 0, j;
			do { digits[used++] = (char)('0' + value % 10U);
			    value /= 10U; } while (value != 0);
			for (j = 0; j < used; j++)
				entry->name[j] = digits[used - j - 1U];
			entry->name[used] = '\0';
			entry->ino = (ino_t)(DEVFS_PTS_INO_BASE + indices[i]);
			entry->type = INODE_CHAR;
		}
		file->f_data = state;
		return 0;
	}
	if (file->f_inode == devfs_input) {
		for (i = 0; i < node_count; i++) {
			struct devfs_dir_entry *entry;
			if (strncmp(nodes[i].device->name, "event", 5) != 0)
				continue;
			entry = &state->entries[state->count++];
			strncpy(entry->name, nodes[i].device->name,
				DEVFS_NAME_MAX - 1U);
			entry->ino = nodes[i].inode->i_ino;
			entry->type = INODE_CHAR;
		}
		file->f_data = state;
		return 0;
	}
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
	for (i = 0; i < node_count; i++) {
		struct devfs_dir_entry *entry = &state->entries[state->count++];
		if (!strncmp(nodes[i].device->name, "event", 5)) {
			state->count--;
			continue;
		}
		strncpy(entry->name, nodes[i].device->name, DEVFS_NAME_MAX - 1U);
		entry->ino = nodes[i].inode->i_ino;
		entry->type = INODE_CHAR;
	}
	error = disk_registry_snapshot(disks, DISK_MAX, &disk_count);
	if (error != 0) {
		kern_free(state);
		return error;
	}
	for (i = 0; i < disk_count; i++) {
		struct devfs_dir_entry *entry;
		if (dir_name_exists(state, disks[i].name))
			continue;
		entry = &state->entries[state->count++];
		strncpy(entry->name, disks[i].name, DEVFS_NAME_MAX - 1U);
		entry->ino = (ino_t)(DEVFS_BLOCK_INO_BASE + (uint64_t)disks[i].dev);
		entry->type = INODE_BLOCK;
	}
	file->f_data = state;
	return 0;
}

static DEVFS_HIGH int
devfs_dir_close(struct file *file)
{
	if (file->f_data != NULL)
		kern_free(file->f_data);
	file->f_data = NULL;
	return 0;
}

static DEVFS_HIGH int
devfs_readdir(struct file *file, struct dirent *entry, int *eof)
{
	struct devfs_dir_state *state = file->f_data;
	unsigned index = (unsigned)file->f_offset;
	if (state == NULL)
		return EIO;
	if (index >= state->count) {
		*eof = 1;
		return 0;
	}
	memset(entry, 0, sizeof(*entry));
	entry->d_ino = state->entries[index].ino;
	entry->d_type = state->entries[index].type;
	strcpy(entry->d_name, state->entries[index].name);
	file->f_offset++;
	*eof = 0;
	return 0;
}

static DEVFS_HIGH int
block_open(struct file *file)
{
	struct disk *disk;
	int error = disk_open_by_dev(file->f_inode->i_rdev, &disk);
	if (error != 0)
		return error;
	if (disk->d_block_size != 512U) {
		disk_close(disk);
		return EOPNOTSUPP;
	}
	file->f_data = disk;
	return 0;
}

static DEVFS_HIGH int
block_close(struct file *file)
{
	if (file->f_data != NULL)
		disk_close(file->f_data);
	file->f_data = NULL;
	return 0;
}

static DEVFS_HIGH ssize_t
block_pread(struct file *file, void *buffer, size_t length, off_t offset)
{
	struct disk *disk = file->f_data;
	uint8_t bounce[512];
	uint8_t *output = buffer;
	uint64_t size, position;
	size_t total = 0;
	int error;
	if (disk == NULL || offset < 0)
		return -EINVAL;
	size = disk->d_block_count * 512U;
	position = (uint32_t)offset;
	if (position >= size)
		return 0;
	if ((uint64_t)length > size - position)
		length = (size_t)(size - position);
	while (total < length) {
		uint64_t block = position / 512U;
		size_t within = (size_t)(position & 511U);
		size_t count = 512U - within;
		if (count > length - total)
			count = length - total;
		error = disk_read(disk, block, 1, bounce);
		if (error != 0)
			return total != 0 ? (ssize_t)total : -error;
		memcpy(output + total, bounce + within, count);
		total += count;
		position += count;
	}
	return (ssize_t)total;
}

static DEVFS_HIGH ssize_t
block_read(struct file *file, void *buffer, size_t length)
{
	ssize_t done = block_pread(file, buffer, length, file->f_offset);
	if (done > 0)
		file->f_offset += done;
	return done;
}

static DEVFS_HIGH ssize_t
block_pwrite(struct file *file, const void *buffer, size_t length, off_t offset)
{
	struct disk *disk = file->f_data;
	struct backing_mutation_guard guard;
	uint8_t bounce[512];
	const uint8_t *input = buffer;
	uint64_t size, position;
	size_t total = 0;
	int error;
	if (disk == NULL || offset < 0)
		return -EINVAL;
	if ((disk->d_flags & DISK_READ_ONLY) != 0)
		return -EROFS;
	size = disk->d_block_count * 512U;
	position = (uint32_t)offset;
	if (position >= size || (uint64_t)length > size - position)
		return -ENOSPC;
	if (length != 0) {
		uint64_t first = position / 512U;
		uint64_t last = (position + length - 1U) / 512U;
		error = backing_mutation_begin_disk(disk, first,
		    last - first + 1U, NULL, &guard);
		if (error != 0)
			return -error;
	} else {
		memset(&guard, 0, sizeof(guard));
	}
	while (total < length) {
		uint64_t block = position / 512U;
		size_t within = (size_t)(position & 511U);
		size_t count = 512U - within;
		if (count > length - total)
			count = length - total;
		if (within != 0 || count != 512U) {
			error = disk_read(disk, block, 1, bounce);
			if (error != 0) {
				backing_mutation_end(&guard);
				return total != 0 ? (ssize_t)total : -error;
			}
		} else {
			memset(bounce, 0, sizeof(bounce));
		}
		memcpy(bounce + within, input + total, count);
		error = disk_write(disk, block, 1, bounce);
		if (error != 0) {
			backing_mutation_end(&guard);
			return total != 0 ? (ssize_t)total : -error;
		}
		total += count;
		position += count;
	}
	backing_mutation_end(&guard);
	return (ssize_t)total;
}

static DEVFS_HIGH ssize_t
block_write(struct file *file, const void *buffer, size_t length)
{
	ssize_t done = block_pwrite(file, buffer, length, file->f_offset);
	if (done > 0)
		file->f_offset += done;
	return done;
}

static DEVFS_HIGH int
block_fsync(struct file *file)
{
	return file->f_data != NULL ? disk_sync(file->f_data) : ENXIO;
}

static DEVFS_HIGH int
block_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	if (file->f_data != NULL && request == BLKGETIDENTITY)
		return argument != 0 ? block_identity_get(file->f_data,
		    (struct block_identity *)argument) : EFAULT;
	return file->f_data != NULL ?
		disk_ioctl(file->f_data, request, (void *)argument) : ENXIO;
}

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

static DEVFS_HIGH int
devfs_mount_impl(struct mount *mountp)
{
	unsigned i;
	memset(nodes, 0, sizeof(nodes));
	node_count = cdev_count();
	devfs_root = inode_alloc(mountp);
	if (devfs_root == NULL)
		return ENOSPC;
	devfs_root->i_type = INODE_DIR;
	devfs_root->i_ino = 1;
	devfs_root->i_op = &devfs_inode_ops;
	devfs_root->i_fop = &devfs_directory_ops;
	devfs_root->i_linkcount = 1;
	devfs_root->i_mode = S_IFDIR | 0555U;
	devfs_root->i_flags = INODE_ROOT | INODE_NOCACHE_CHILDREN;
	devfs_shm = inode_alloc(mountp);
	if (devfs_shm == NULL)
		return ENOSPC;
	devfs_shm->i_type = INODE_DIR;
	devfs_shm->i_ino = DEVFS_SHM_INO;
	devfs_shm->i_op = &devfs_inode_ops;
	devfs_shm->i_fop = &devfs_directory_ops;
	devfs_shm->i_linkcount = 2;
	devfs_shm->i_mode = S_IFDIR | 0555U;
	devfs_shm->i_flags = INODE_NOCACHE_CHILDREN;
	devfs_pts = inode_alloc(mountp);
	if (devfs_pts == NULL)
		return ENOSPC;
	devfs_pts->i_type = INODE_DIR;
	devfs_pts->i_ino = DEVFS_PTS_INO;
	devfs_pts->i_op = &devfs_inode_ops;
	devfs_pts->i_fop = &devfs_directory_ops;
	devfs_pts->i_linkcount = 2;
	devfs_pts->i_mode = S_IFDIR | 0555U;
	devfs_pts->i_flags = INODE_NOCACHE_CHILDREN;
	devfs_input = inode_alloc(mountp);
	if (devfs_input == NULL)
		return ENOSPC;
	devfs_input->i_type = INODE_DIR;
	devfs_input->i_ino = DEVFS_INPUT_INO;
	devfs_input->i_op = &devfs_inode_ops;
	devfs_input->i_fop = &devfs_directory_ops;
	devfs_input->i_linkcount = 2;
	devfs_input->i_mode = S_IFDIR | 0555U;
	devfs_input->i_flags = INODE_NOCACHE_CHILDREN;
	for (i = 0; i < node_count; i++) {
		struct inode *inode = inode_alloc(mountp);
		if (inode == NULL)
			return ENOSPC;
		nodes[i].device = cdev_at(i);
		nodes[i].inode = inode;
		inode->i_type = INODE_CHAR;
		inode->i_ino = 5U + i;
		inode->i_op = &devfs_inode_ops;
		inode->i_fop = &cdev_file_ops;
		inode->i_data = (void *)nodes[i].device;
		inode->i_linkcount = 1;
		inode->i_mode = S_IFCHR |
		    (!strncmp(nodes[i].device->name, "event", 5) ? 0640U
								   : 0666U);
		inode->i_rdev = nodes[i].device->rdev;
	}
	mountp->m_root = devfs_root;
	return 0;
}

static DEVFS_HIGH int
devfs_statvfs(struct mount *mountp, struct statvfs *result)
{
	(void)mountp;
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	result->f_bsize = result->f_frsize = 1U;
	result->f_files = DEVFS_ENTRY_MAX;
	result->f_ffree = node_count < DEVFS_ENTRY_MAX ?
	    DEVFS_ENTRY_MAX - node_count : 0;
	result->f_favail = result->f_ffree;
	result->f_namemax = DEVFS_NAME_MAX - 1U;
	return 0;
}

const struct filesystem_type devfs_type = {
	.fs_name = "devfs",
	.fs_flags = FILESYSTEM_NODEV,
	.mount = devfs_mount_impl,
	.statvfs = devfs_statvfs,
};
