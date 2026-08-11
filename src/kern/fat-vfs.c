/* FAT inode/file adapter over the proven FAT12/FAT16 chain engine.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/fat-vfs.h"
#include "kern/fat.h"
#include "kern/fat16.h"
#include "kern/fat32.h"
#include "kern/namecache.h"
#include "kern/namei.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define FAT_MOUNT_MAX MOUNT_MAX
#define FAT_INODE_MAX 256U
#define FAT_FILE_MAX 64U
#define FAT_ATTRIBUTE_READ_ONLY 0x01U
#define FAT_ATTRIBUTE_DIRECTORY 0x10U

struct fat_mount_state {
	struct zedbsd_filesystem legacy;
	uint8_t used;
};

struct fat_inode_slot {
	struct fat_inode_info info;
	char path[ZEDBSD_PATH_MAX];
	uint8_t used;
};

struct fat_file_state {
	struct zedbsd_file legacy;
	uint8_t used;
};

static struct fat_mount_state fat_mounts[FAT_MOUNT_MAX]
	__attribute__((section(".vfs_bss")));
static struct fat_inode_slot fat_inodes[FAT_INODE_MAX]
	__attribute__((section(".vfs_bss")));
static struct fat_file_state fat_files[FAT_FILE_MAX]
	__attribute__((section(".vfs_bss")));

static int
fs_error(enum zedbsd_fs_result result)
{
	switch (result) {
	case ZEDBSD_FS_OK: return 0;
	case ZEDBSD_FS_NOT_FOUND: return ENOENT;
	case ZEDBSD_FS_INVALID_PATH: return EINVAL;
	case ZEDBSD_FS_READ_ONLY: return EROFS;
	case ZEDBSD_FS_NO_SPACE: return ENOSPC;
	case ZEDBSD_FS_IO_ERROR: return EIO;
	case ZEDBSD_FS_CORRUPT: return EIO;
	case ZEDBSD_FS_UNSUPPORTED: return EOPNOTSUPP;
	case ZEDBSD_FS_EXISTS: return EEXIST;
	default: return EINVAL;
	}
}

static int
volume_read(const void *context, uint32_t lba, void *buffer)
{
	return disk_read((struct disk *)context, lba, 1, buffer) == 0;
}

static int
volume_write(void *context, uint32_t lba, const void *buffer)
{
	return disk_write(context, lba, 1, buffer) == 0;
}

static struct zedbsd_volume
fat_volume(struct disk *disk)
{
	struct zedbsd_volume volume;
	memset(&volume, 0, sizeof(volume));
	volume.context = disk;
	volume.sector_size = 512;
	volume.read = volume_read;
	if (!(disk->d_flags & DISK_READ_ONLY))
		volume.write = volume_write;
	return volume;
}

static struct fat_mount_state *
fat_mount_state(struct mount *mountp)
{
	return mountp != NULL ? mountp->m_data : NULL;
}

static struct fat_inode_slot *
fat_slot(struct inode *inode)
{
	unsigned i;
	for (i = 0; i < FAT_INODE_MAX; i++)
		if (&fat_inodes[i].info.fi_inode == inode)
			return &fat_inodes[i];
	return NULL;
}

static const char *fat_path(struct inode *inode)
{
	struct fat_inode_slot *slot = fat_slot(inode);
	return slot != NULL ? slot->path : NULL;
}

static struct inode *
fat_alloc_inode(struct mount *mountp)
{
	unsigned i;
	(void)mountp;
	for (i = 0; i < FAT_INODE_MAX; i++) {
		if (!fat_inodes[i].used) {
			fat_inodes[i].used = 1;
			memset(&fat_inodes[i].info, 0,
			       sizeof(fat_inodes[i].info));
			fat_inodes[i].path[0] = '\0';
			return &fat_inodes[i].info.fi_inode;
		}
	}
	return NULL;
}

static void
fat_free_inode(struct inode *inode)
{
	struct fat_inode_slot *slot = fat_slot(inode);
	if (slot != NULL)
		memset(slot, 0, sizeof(*slot));
}

static int
join_path(const char *parent, const struct componentname *name,
	  char output[ZEDBSD_PATH_MAX])
{
	size_t parent_length = strlen(parent);
	if (name->cn_namelen == 0 || name->cn_namelen > NAME_MAX ||
	    parent_length + (parent_length != 0) + name->cn_namelen >=
	    ZEDBSD_PATH_MAX)
		return ENAMETOOLONG;
	memcpy(output, parent, parent_length);
	if (parent_length != 0)
		output[parent_length++] = '/';
	memcpy(output + parent_length, name->cn_nameptr, name->cn_namelen);
	output[parent_length + name->cn_namelen] = '\0';
	return 0;
}

static ino_t
fat_ino(uint32_t lba, uint16_t offset)
{
	return 2U + (ino_t)lba * 16U + offset / 32U;
}

static int
fat_make_inode(struct mount *mountp, const char *path,
	       const struct zedbsd_dirent *entry, uint32_t lba,
	       uint16_t offset, uint32_t first_cluster, uint8_t attributes,
	       struct inode **result)
{
	struct fat_inode_info *info;
	struct fat_inode_slot *slot;
	struct inode *inode;
	ino_t ino = fat_ino(lba, offset);
	int error = inode_get(mountp, ino, result);
	if (error == 0)
		return 0;
	inode = inode_alloc(mountp);
	if (inode == NULL)
		return ENOSPC;
	info = fat_inode(inode);
	slot = fat_slot(inode);
	if (slot == NULL || strlen(path) >= ZEDBSD_PATH_MAX) {
		inode_release(inode);
		return EINVAL;
	}
	strcpy(slot->path, path);
	info->fi_first_cluster = first_cluster;
	info->fi_dirent_lba = lba;
	info->fi_dirent_offset = offset;
	info->fi_attributes = attributes;
	inode->i_ino = ino;
	inode->i_data = info;
	inode->i_linkcount = 1;
	inode->i_uid = inode->i_gid = 0;
	inode->i_size = (off_t)entry->size;
	if (attributes & FAT_ATTRIBUTE_DIRECTORY) {
		inode->i_type = INODE_DIR;
		inode->i_mode = S_IFDIR | 0755U;
		inode->i_size = 0;
	} else {
		inode->i_type = INODE_REG;
		inode->i_mode = S_IFREG | 0644U;
	}
	if (attributes & FAT_ATTRIBUTE_READ_ONLY)
		inode->i_mode &= ~(mode_t)0222U;
	*result = inode;
	return 0;
}

static int fat_lookup(struct inode *, const struct componentname *,
		      struct inode **);
static int fat_lookup_casefold(struct inode *, const struct componentname *,
			       struct inode **);
static int fat_create(struct inode *, const struct componentname *, mode_t,
		      struct inode **);
static int fat_truncate(struct inode *, off_t);
static int fat_getattr(struct inode *, struct stat *);
static ssize_t fat_read_file(struct file *, void *, size_t);
static ssize_t fat_write_file(struct file *, const void *, size_t);
static int fat_readdir(struct file *, struct dirent *, int *);
static int fat_fsync(struct file *);
static int fat_close_file(struct file *);

static const struct inode_ops fat_inode_ops = {
	.lookup = fat_lookup,
	.lookup_casefold = fat_lookup_casefold,
	.create = fat_create,
	.getattr = fat_getattr,
	.truncate = fat_truncate,
	.sync = NULL,
};
static const struct file_ops fat_regular_ops = {
	.read = fat_read_file,
	.write = fat_write_file,
	.fsync = fat_fsync,
	.close = fat_close_file,
};
static const struct file_ops fat_directory_ops = {
	.readdir = fat_readdir,
	.close = fat_close_file,
};

static void
set_inode_ops(struct inode *inode)
{
	inode->i_op = &fat_inode_ops;
	inode->i_fop = inode->i_type == INODE_DIR ?
		&fat_directory_ops : &fat_regular_ops;
}

static int
fat_stat_path(struct mount *mountp, const char *path, struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct zedbsd_dirent entry;
	char canonical[ZEDBSD_PATH_MAX];
	const char *slash;
	size_t prefix_length;
	uint32_t lba, first_cluster;
	uint16_t offset;
	uint8_t attributes;
	int error;
	enum zedbsd_fs_result fsresult = zedbsd_fat_stat_location(
		&state->legacy, path, &entry, &lba, &offset,
		&first_cluster, &attributes);
	if (fsresult != ZEDBSD_FS_OK)
		return fs_error(fsresult);
	slash = strrchr(path, '/');
	prefix_length = slash != NULL ? (size_t)(slash - path + 1) : 0;
	if (prefix_length + strlen(entry.name) >= sizeof(canonical))
		return ENAMETOOLONG;
	memcpy(canonical, path, prefix_length);
	strcpy(canonical + prefix_length, entry.name);
	error = fat_make_inode(mountp, canonical, &entry, lba, offset,
			       first_cluster, attributes, result);
	if (error == 0)
		set_inode_ops(*result);
	return error;
}

static int
fat_stat_path_casefold(struct mount *mountp, const char *path,
		       struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct zedbsd_dirent entry;
	char canonical[ZEDBSD_PATH_MAX];
	const char *slash;
	size_t prefix_length;
	uint32_t lba, first_cluster;
	uint16_t offset;
	uint8_t attributes;
	int error;
	enum zedbsd_fs_result fsresult = zedbsd_fat_stat_location_casefold(
		&state->legacy, path, &entry, &lba, &offset,
		&first_cluster, &attributes);
	if (fsresult != ZEDBSD_FS_OK)
		return fs_error(fsresult);
	slash = strrchr(path, '/');
	prefix_length = slash != NULL ? (size_t)(slash - path + 1) : 0;
	if (prefix_length + strlen(entry.name) >= sizeof(canonical))
		return ENAMETOOLONG;
	memcpy(canonical, path, prefix_length);
	strcpy(canonical + prefix_length, entry.name);
	error = fat_make_inode(mountp, canonical, &entry, lba, offset,
			       first_cluster, attributes, result);
	if (error == 0)
		set_inode_ops(*result);
	return error;
}

static int
fat_lookup(struct inode *directory, const struct componentname *name,
	   struct inode **result)
{
	char path[ZEDBSD_PATH_MAX];
	const char *parent = fat_path(directory);
	int error;
	if (parent == NULL)
		return EIO;
	if (name->cn_namelen == 1 && name->cn_nameptr[0] == '.') {
		inode_ref(directory); *result = directory; return 0;
	}
	if (name->cn_namelen == 2 && name->cn_nameptr[0] == '.' &&
	    name->cn_nameptr[1] == '.') {
		char *slash;
		if (parent[0] == '\0') {
			inode_ref(directory); *result = directory; return 0;
		}
		strcpy(path, parent);
		slash = strrchr(path, '/');
		if (slash == NULL) {
			inode_ref(directory->i_mount->m_root);
			*result = directory->i_mount->m_root;
			return 0;
		}
		*slash = '\0';
		return fat_stat_path(directory->i_mount, path, result);
	}
	error = join_path(parent, name, path);
	return error != 0 ? error :
		fat_stat_path(directory->i_mount, path, result);
}

static int
fat_lookup_casefold(struct inode *directory,
		    const struct componentname *name, struct inode **result)
{
	char path[ZEDBSD_PATH_MAX];
	const char *parent = fat_path(directory);
	int error;

	if (parent == NULL)
		return EIO;
	error = join_path(parent, name, path);
	return error != 0 ? error :
		fat_stat_path_casefold(directory->i_mount, path, result);
}

static int
fat_getattr(struct inode *inode, struct stat *status)
{
	memset(status, 0, sizeof(*status));
	status->st_dev = inode->i_mount->m_disk->d_dev;
	status->st_ino = inode->i_ino;
	status->st_mode = inode->i_mode;
	status->st_nlink = inode->i_linkcount;
	status->st_uid = inode->i_uid;
	status->st_gid = inode->i_gid;
	status->st_size = inode->i_size;
	return 0;
}

static struct fat_file_state *
fat_file_get(struct file *file)
{
	unsigned i;
	if (file->f_data != NULL)
		return file->f_data;
	for (i = 0; i < FAT_FILE_MAX; i++) {
		if (!fat_files[i].used) {
			struct fat_mount_state *mount_state =
				fat_mount_state(file->f_inode->i_mount);
			fat_files[i].used = 1;
			memset(&fat_files[i].legacy, 0,
			       sizeof(fat_files[i].legacy));
			if (zedbsd_fs_open_result(&mount_state->legacy,
						 fat_path(file->f_inode),
						 &fat_files[i].legacy) !=
			    ZEDBSD_FS_OK) {
				fat_files[i].used = 0;
				return NULL;
			}
			file->f_data = &fat_files[i];
			return &fat_files[i];
		}
	}
	return NULL;
}

static ssize_t
fat_read_file(struct file *file, void *buffer, size_t length)
{
	struct fat_file_state *state = fat_file_get(file);
	uint32_t count;
	enum zedbsd_fs_result result;
	if (state == NULL)
		return -EIO;
	if (file->f_offset >= file->f_inode->i_size)
		return 0;
	if (length > (size_t)(file->f_inode->i_size - file->f_offset))
		length = (size_t)(file->f_inode->i_size - file->f_offset);
	count = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
	result = zedbsd_file_read_result(&state->legacy, (uint64_t)file->f_offset,
					buffer, count, NULL, NULL);
	if (result != ZEDBSD_FS_OK)
		return -fs_error(result);
	file->f_offset += count;
	return count;
}

static ssize_t
fat_write_file(struct file *file, const void *buffer, size_t length)
{
	struct fat_file_state *state = fat_file_get(file);
	uint32_t count = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
	enum zedbsd_fs_result result;
	if (state == NULL)
		return -EIO;
	result = zedbsd_file_write_result(&state->legacy,
					 (uint64_t)file->f_offset, buffer, count);
	if (result != ZEDBSD_FS_OK)
		return -fs_error(result);
	file->f_offset += count;
	if (file->f_offset > file->f_inode->i_size)
		file->f_inode->i_size = file->f_offset;
	return count;
}

static int
fat_readdir(struct file *file, struct dirent *entry, int *eof)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	struct zedbsd_dirent legacy;
	char child_path[ZEDBSD_PATH_MAX];
	struct componentname component;
	struct inode *child;
	enum zedbsd_fs_result result = zedbsd_fs_readdir_result(
		&state->legacy, fat_path(file->f_inode),
		(unsigned)file->f_offset, &legacy);
	if (result == ZEDBSD_FS_NOT_FOUND) {
		*eof = 1;
		return 0;
	}
	if (result != ZEDBSD_FS_OK)
		return fs_error(result);
	component.cn_nameptr = legacy.name;
	component.cn_namelen = strlen(legacy.name);
	component.cn_flags = COMPONENT_LAST;
	if (join_path(fat_path(file->f_inode), &component, child_path) != 0)
		return ENAMETOOLONG;
	if (fat_stat_path(file->f_inode->i_mount, child_path, &child) != 0)
		return EIO;
	memset(entry, 0, sizeof(*entry));
	entry->d_ino = child->i_ino;
	entry->d_type = child->i_type;
	strncpy(entry->d_name, legacy.name, NAME_MAX);
	entry->d_name[NAME_MAX] = '\0';
	inode_release(child);
	file->f_offset++;
	*eof = 0;
	return 0;
}

static int
fat_close_file(struct file *file)
{
	struct fat_file_state *state = file->f_data;
	int error = 0;
	if (state != NULL) {
		if ((file->f_flags & O_ACCMODE) != O_RDONLY)
			error = fs_error(zedbsd_file_flush_result(&state->legacy));
		memset(state, 0, sizeof(*state));
		file->f_data = NULL;
	}
	return error;
}

static int
fat_fsync(struct file *file)
{
	struct fat_file_state *state = fat_file_get(file);
	int error;
	if (state == NULL)
		return EIO;
	error = fs_error(zedbsd_file_flush_result(&state->legacy));
	return error != 0 ? error : bio_flush(file->f_inode->i_mount->m_disk);
}

static int
fat_truncate(struct inode *inode, off_t size)
{
	struct zedbsd_file file;
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	enum zedbsd_fs_result result;
	if (size < 0)
		return EINVAL;
	result = zedbsd_fs_open_result(&state->legacy, fat_path(inode), &file);
	if (result == ZEDBSD_FS_OK)
		result = zedbsd_file_truncate_result(&file, (uint64_t)size);
	if (result == ZEDBSD_FS_OK)
		result = zedbsd_file_flush_result(&file);
	if (result == ZEDBSD_FS_OK)
		inode->i_size = size;
	return fs_error(result);
}

static int
fat_create(struct inode *directory, const struct componentname *name,
	   mode_t mode, struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct zedbsd_file file;
	char path[ZEDBSD_PATH_MAX];
	enum zedbsd_fs_result fsresult;
	int error;
	(void)mode;
	error = join_path(fat_path(directory), name, path);
	if (error != 0)
		return error;
	fsresult = zedbsd_fs_create_result(&state->legacy, path, &file);
	if (fsresult != ZEDBSD_FS_OK)
		return fs_error(fsresult);
	fsresult = zedbsd_file_flush_result(&file);
	if (fsresult != ZEDBSD_FS_OK)
		return fs_error(fsresult);
	namecache_remove(directory, name);
	return fat_stat_path(directory->i_mount, path, result);
}

static int
fat_probe(struct disk *disk)
{
	struct zedbsd_volume volume;
	enum zedbsd_fs_result result;
	if (disk == NULL || disk->d_block_size != 512)
		return EOPNOTSUPP;
	volume = fat_volume(disk);
	result = zedbsd_fat12_driver.probe(&volume);
	if (result == ZEDBSD_FS_UNSUPPORTED)
		result = zedbsd_fat16_driver.probe(&volume);
	if (result == ZEDBSD_FS_UNSUPPORTED)
		result = zedbsd_fat32_driver.probe(&volume);
	return fs_error(result);
}

static int
fat_mount_impl(struct mount *mountp)
{
	static const struct zedbsd_filesystem_driver *const drivers[] = {
		&zedbsd_fat12_driver, &zedbsd_fat16_driver,
		&zedbsd_fat32_driver,
	};
	struct fat_mount_state *state = NULL;
	struct inode *root;
	struct fat_inode_info *info;
	unsigned i;
	enum zedbsd_fs_result result;
	for (i = 0; i < FAT_MOUNT_MAX; i++)
		if (!fat_mounts[i].used) {
			state = &fat_mounts[i];
			break;
		}
	if (state == NULL)
		return ENOSPC;
	memset(state, 0, sizeof(*state));
	state->used = 1;
	result = zedbsd_fs_mount_result(&state->legacy,
				       &(struct zedbsd_volume){
					.context = mountp->m_disk,
					.sector_size = 512,
					.read = volume_read,
					.write = (mountp->m_flags & MOUNT_READ_ONLY) ?
						NULL : volume_write,
				       }, drivers,
				       sizeof(drivers) / sizeof(drivers[0]));
	if (result != ZEDBSD_FS_OK) {
		memset(state, 0, sizeof(*state));
		return fs_error(result);
	}
	mountp->m_data = state;
	root = inode_alloc(mountp);
	if (root == NULL) {
		memset(state, 0, sizeof(*state));
		mountp->m_data = NULL;
		return ENOSPC;
	}
	info = fat_inode(root);
	root->i_type = INODE_DIR;
	root->i_ino = 1;
	root->i_mode = S_IFDIR | 0755U;
	root->i_linkcount = 1;
	root->i_flags = INODE_ROOT;
	root->i_data = info;
	set_inode_ops(root);
	mountp->m_root = root;
	return 0;
}

static int
fat_unmount_impl(struct mount *mountp)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	int error;
	if (state == NULL)
		return EINVAL;
	error = fs_error(zedbsd_fat_flush(&state->legacy));
	if (error != 0)
		return error;
	zedbsd_fat_invalidate(&state->legacy);
	memset(state, 0, sizeof(*state));
	mountp->m_data = NULL;
	return 0;
}

const struct filesystem_type fat_filesystem_type = {
	.fs_name = "fat",
	.probe = fat_probe,
	.mount = fat_mount_impl,
	.unmount = fat_unmount_impl,
	.alloc_inode = fat_alloc_inode,
	.free_inode = fat_free_inode,
};

int
fat_file_extents(struct file *file, fat_extent_cb callback, void *context)
{
	struct fat_file_state *state;
	if (file == NULL || callback == NULL || file->f_inode == NULL ||
	    file->f_inode->i_type != INODE_REG ||
	    file->f_inode->i_mount->m_type != &fat_filesystem_type)
		return EINVAL;
	state = fat_file_get(file);
	if (state == NULL)
		return EIO;
	return zedbsd_fat_file_extents(&state->legacy, callback, context) == 0 ?
		0 : EIO;
}

struct contiguous_context {
	uint64_t block;
	uint64_t expected;
	int seen;
};

static int contiguous_extent(uint64_t file_block, uint64_t disk_block,
			     uint32_t count, void *argument)
{
	struct contiguous_context *context = argument;
	if (!context->seen) {
		context->block = disk_block;
		context->expected = disk_block;
		context->seen = 1;
	}
	if (file_block + context->block != disk_block ||
	    disk_block != context->expected)
		return EOPNOTSUPP;
	context->expected += count;
	return 0;
}

int
fat_file_contiguous_block(struct file *file, struct disk **disk,
			  uint64_t *block)
{
	struct contiguous_context context = { 0 };
	int error;
	if (file == NULL || disk == NULL || block == NULL ||
	    file->f_inode->i_mount->m_type != &fat_filesystem_type)
		return EINVAL;
	error = fat_file_extents(file, contiguous_extent, &context);
	if (error != 0 || !context.seen)
		return error != 0 ? error : EIO;
	*disk = file->f_inode->i_mount->m_disk;
	*block = context.block;
	return 0;
}
