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
#include <sys/statvfs.h>

#define FAT_MOUNT_MAX MOUNT_MAX
#define FAT_INODE_MAX 256U
#define FAT_FILE_MAX 96U
#define FAT_ATTRIBUTE_READ_ONLY 0x01U
#define FAT_ATTRIBUTE_DIRECTORY 0x10U
#define FAT_INODE_ORPHANED 0x01U
#define FAT_MUTATION __attribute__((section(".hightext")))
#define FAT_EPOCH_1980 315532800L
#define FAT_METADATA_MAX 32

struct fat_metadata {
	char path[ZEDBSD_PATH_MAX];
	mode_t mode;
	uid_t uid;
	gid_t gid;
};

struct fat_mount_state {
	struct zedbsd_filesystem legacy;
	struct mutex lock;
	struct fat_metadata metadata[FAT_METADATA_MAX];
	unsigned metadata_count;
	uint8_t used;
};

struct fat_inode_slot {
	struct fat_inode_info info;
	char path[ZEDBSD_PATH_MAX];
	uint8_t used;
};

struct fat_file_state {
	struct zedbsd_file legacy;
	struct inode *owner;
	uint8_t used;
};

static struct fat_mount_state fat_mounts[FAT_MOUNT_MAX]
	__attribute__((section(".vfs_bss")));
static struct fat_inode_slot fat_inodes[FAT_INODE_MAX]
	__attribute__((section(".vfs_bss")));
static struct fat_file_state fat_files[FAT_FILE_MAX]
	__attribute__((section(".vfs_bss")));
static struct spinlock fat_pool_lock = {
	{ 0 }, LOCK_RANK_INODE, "FAT object pools", 0, 0
};

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
	case ZEDBSD_FS_NOT_EMPTY: return ENOTEMPTY;
	case ZEDBSD_FS_IS_DIRECTORY: return EISDIR;
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

static int
fat_metadata_number(const char *text, unsigned base, uint32_t *value)
{
	uint32_t result = 0;
	if (*text == '\0') return EINVAL;
	while (*text != '\0') {
		unsigned digit = (unsigned)(*text++ - '0');
		if (digit >= base || result > (UINT32_MAX - digit) / base)
			return EINVAL;
		result = result * base + digit;
	}
	*value = result;
	return 0;
}

static void
fat_metadata_load(struct fat_mount_state *state)
{
	struct zedbsd_file file;
	char buffer[4096];
	uint32_t length, offset = 0;

	memset(&file, 0, sizeof(file));
	if (zedbsd_fs_open_result(&state->legacy, "etc/unixmode", &file) !=
	    ZEDBSD_FS_OK)
		return;
	length = file.size < sizeof(buffer) - 1U ? (uint32_t)file.size :
	    (uint32_t)sizeof(buffer) - 1U;
	if (zedbsd_file_read_result(&file, 0, buffer, length, NULL, NULL) !=
	    ZEDBSD_FS_OK)
		return;
	buffer[length] = '\0';
	while (offset < length && state->metadata_count < FAT_METADATA_MAX) {
		struct fat_metadata *metadata =
		    &state->metadata[state->metadata_count];
		char *line = buffer + offset, *mode, *uid, *gid, *end;
		uint32_t mode_value, uid_value, gid_value;

		end = strchr(line, '\n');
		if (end != NULL) *end = '\0';
		offset += (uint32_t)strlen(line) + (end != NULL ? 1U : 0U);
		mode = strchr(line, ':'); if (mode == NULL) continue; *mode++ = '\0';
		uid = strchr(mode, ':'); if (uid == NULL) continue; *uid++ = '\0';
		gid = strchr(uid, ':'); if (gid == NULL) continue; *gid++ = '\0';
		if (strchr(gid, ':') != NULL || line[0] == '/' || line[0] == '\0' ||
		    strlen(line) >= sizeof(metadata->path) ||
		    fat_metadata_number(mode, 8, &mode_value) != 0 ||
		    fat_metadata_number(uid, 10, &uid_value) != 0 ||
		    fat_metadata_number(gid, 10, &gid_value) != 0 ||
		    mode_value > 07777U)
			continue;
		strcpy(metadata->path, line);
		metadata->mode = (mode_t)mode_value;
		metadata->uid = (uid_t)uid_value;
		metadata->gid = (gid_t)gid_value;
		state->metadata_count++;
	}
}

static void
fat_metadata_apply(struct mount *mountp, const char *path, struct inode *inode)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	unsigned i;
	for (i = 0; state != NULL && i < state->metadata_count; i++)
		if (!strcmp(state->metadata[i].path, path)) {
			inode->i_mode = (inode->i_mode & S_IFMT) |
			    state->metadata[i].mode;
			inode->i_uid = state->metadata[i].uid;
			inode->i_gid = state->metadata[i].gid;
			break;
		}
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
	unsigned long irq;
	(void)mountp;
	irq = spin_lock_irqsave(&fat_pool_lock);
	for (i = 0; i < FAT_INODE_MAX; i++) {
		if (!fat_inodes[i].used) {
			fat_inodes[i].used = 1;
			memset(&fat_inodes[i].info, 0,
			       sizeof(fat_inodes[i].info));
			fat_inodes[i].path[0] = '\0';
			spin_unlock_irqrestore(&fat_pool_lock, irq);
			return &fat_inodes[i].info.fi_inode;
		}
	}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
	return NULL;
}

static void
fat_free_inode(struct inode *inode)
{
	struct fat_inode_slot *slot = fat_slot(inode);
	unsigned long irq;
	if (slot != NULL) {
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(slot, 0, sizeof(*slot));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
	}
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
fat_leap_year(int year)
{
	return (year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0);
}

static int
fat_month_days(int year, int month)
{
	static const uint8_t days[] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};
	return month == 2 && fat_leap_year(year) ? 29 : days[month - 1];
}

static time_t
fat_decode_time(uint16_t date, uint16_t time)
{
	int year, month, day, days = 0;
	int64_t seconds;

	if (date == 0)
		return 0;
	year = 1980 + ((date >> 9) & 0x7f);
	month = (date >> 5) & 0x0f;
	day = date & 0x1f;
	if (month < 1 || month > 12 || day < 1 ||
	    day > fat_month_days(year, month))
		return 0;
	for (int y = 1970; y < year; y++)
		days += fat_leap_year(y) ? 366 : 365;
	for (int m = 1; m < month; m++)
		days += fat_month_days(year, m);
	days += day - 1;
	seconds = (int64_t)days * 86400 + ((time >> 11) & 0x1f) * 3600 +
		((time >> 5) & 0x3f) * 60 + (time & 0x1f) * 2;
#ifdef ZEDBSD_USER_ABI_LP64
	return (time_t)seconds;
#else
	return seconds > INT32_MAX ? (time_t)INT32_MAX : (time_t)seconds;
#endif
}

static FAT_MUTATION int
fat_encode_time(time_t seconds, uint16_t *date, uint16_t *time)
{
	int64_t days, remainder;
	int year = 1970, month = 1;

	if (seconds < FAT_EPOCH_1980)
		return EOVERFLOW;
	days = seconds / 86400;
	remainder = seconds % 86400;
	while (days >= (fat_leap_year(year) ? 366 : 365)) {
		days -= fat_leap_year(year) ? 366 : 365;
		year++;
	}
	if (year > 2107)
		return EOVERFLOW;
	while (days >= fat_month_days(year, month)) {
		days -= fat_month_days(year, month);
		month++;
	}
	*date = (uint16_t)(((year - 1980) << 9) | (month << 5) |
		(days + 1));
	*time = (uint16_t)(((remainder / 3600) << 11) |
		(((remainder / 60) % 60) << 5) | ((remainder % 60) / 2));
	return 0;
}

static void
fat_load_inode_times(struct mount *mountp, struct inode *inode,
			 uint32_t lba, uint16_t offset)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	const uint8_t *sector;
	const uint8_t *raw;

	if (state == NULL || zedbsd_fat_read_sector_result(&state->legacy, lba,
	    &sector) != ZEDBSD_FS_OK)
		return;
	raw = sector + offset;
	inode->i_atime.tv_sec = fat_decode_time(zedbsd_fat_get16(raw + 18), 0);
	inode->i_mtime.tv_sec = fat_decode_time(zedbsd_fat_get16(raw + 24),
		zedbsd_fat_get16(raw + 22));
	inode->i_ctime.tv_sec = fat_decode_time(zedbsd_fat_get16(raw + 16),
		zedbsd_fat_get16(raw + 14));
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
		/* FAT has no execute bit.  Mount regular files with the executable
		 * default expected by this boot/userland volume. */
		inode->i_mode = S_IFREG | 0755U;
	}
	if (attributes & FAT_ATTRIBUTE_READ_ONLY)
		inode->i_mode &= ~(mode_t)0222U;
	fat_metadata_apply(mountp, path, inode);
	fat_load_inode_times(mountp, inode, lba, offset);
	*result = inode;
	return 0;
}

static int fat_lookup(struct inode *, const struct componentname *,
		      struct inode **);
static int fat_lookup_casefold(struct inode *, const struct componentname *,
			       struct inode **);
static int fat_create(struct inode *, const struct componentname *, mode_t,
		      struct inode **);
static int fat_mkdir(struct inode *, const struct componentname *, mode_t,
		     struct inode **);
static int fat_unlink(struct inode *, const struct componentname *);
static int fat_rmdir(struct inode *, const struct componentname *);
static int fat_rename(struct inode *, const struct componentname *,
		      struct inode *, const struct componentname *, unsigned);
static int fat_truncate(struct inode *, off_t);
static int fat_getattr(struct inode *, struct stat *);
static int fat_setattr(struct inode *, const struct stat *, unsigned);
static void fat_reclaim(struct inode *);
static ssize_t fat_read_file(struct file *, void *, size_t);
static ssize_t fat_write_file(struct file *, const void *, size_t);
static ssize_t fat_pread_file(struct file *, void *, size_t, off_t);
static ssize_t fat_pwrite_file(struct file *, const void *, size_t, off_t);
static int fat_readdir(struct file *, struct dirent *, int *);
static int fat_open_file(struct file *);
static int fat_fsync(struct file *);
static int fat_close_file(struct file *);

static const struct inode_ops fat_inode_ops = {
	.lookup = fat_lookup,
	.lookup_casefold = fat_lookup_casefold,
	.create = fat_create,
	.mkdir = fat_mkdir,
	.unlink = fat_unlink,
	.rmdir = fat_rmdir,
	.rename = fat_rename,
	.getattr = fat_getattr,
	.setattr = fat_setattr,
	.truncate = fat_truncate,
	.sync = NULL,
	.reclaim = fat_reclaim,
};
static const struct file_ops fat_regular_ops = {
	.open = fat_open_file,
	.read = fat_read_file,
	.write = fat_write_file,
	.pread = fat_pread_file,
	.pwrite = fat_pwrite_file,
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
fat_lookup_unlocked(struct inode *directory, const struct componentname *name,
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
fat_lookup(struct inode *directory, const struct componentname *name,
	   struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_lookup_unlocked(directory, name, result);
	mutex_unlock(&state->lock);
	return error;
}

static int
fat_lookup_casefold_unlocked(struct inode *directory,
			     const struct componentname *name,
			     struct inode **result)
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
	status->st_atime = inode->i_atime.tv_sec;
	status->st_mtime = inode->i_mtime.tv_sec;
	status->st_ctime = inode->i_ctime.tv_sec;
	status->st_blksize = 512;
	status->st_blocks = inode->i_size > 0 ?
	    (blkcnt_t)(((uint64_t)inode->i_size + 511U) / 512U) : 0;
	return 0;
}

static FAT_MUTATION void
fat_put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static FAT_MUTATION int
fat_setattr_unlocked(struct inode *inode, const struct stat *status,
		     unsigned mask)
{
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	struct fat_inode_info *info = fat_inode(inode);
	uint8_t *sector, saved[32];
	uint16_t atime_date = 0, atime_time = 0;
	uint16_t mtime_date = 0, mtime_time = 0;
	mode_t permissions;
	enum zedbsd_fs_result result;
	int error;

	if (state == NULL || info == NULL || (inode->i_flags & INODE_ROOT) != 0)
		return EOPNOTSUPP;
	if ((mask & INODE_ATTR_SIZE) != 0)
		return EOPNOTSUPP;
	if ((mask & INODE_ATTR_UID) != 0 && status->st_uid != inode->i_uid)
		return EOPNOTSUPP;
	if ((mask & INODE_ATTR_GID) != 0 && status->st_gid != inode->i_gid)
		return EOPNOTSUPP;
	if (mask & INODE_ATTR_MODE) {
		permissions = status->st_mode & 07777U;
		if (permissions != 0755U && permissions != 0555U)
			return EOPNOTSUPP;
	}
	if (mask & INODE_ATTR_ATIME) {
		if (status->st_atim.tv_nsec < 0 ||
		    status->st_atim.tv_nsec >= 1000000000L)
			return EINVAL;
		error = fat_encode_time(status->st_atim.tv_sec, &atime_date,
			&atime_time);
		if (error != 0)
			return error;
	}
	if (mask & INODE_ATTR_MTIME) {
		if (status->st_mtim.tv_nsec < 0 ||
		    status->st_mtim.tv_nsec >= 1000000000L)
			return EINVAL;
		error = fat_encode_time(status->st_mtim.tv_sec, &mtime_date,
			&mtime_time);
		if (error != 0)
			return error;
	}
	result = zedbsd_fat_write_sector_result(&state->legacy,
		info->fi_dirent_lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return fs_error(result);
	memcpy(saved, sector + info->fi_dirent_offset, sizeof(saved));
	sector += info->fi_dirent_offset;
	if (mask & INODE_ATTR_MODE) {
		if ((status->st_mode & 0222U) == 0)
			sector[11] |= FAT_ATTRIBUTE_READ_ONLY;
		else
			sector[11] &= (uint8_t)~FAT_ATTRIBUTE_READ_ONLY;
	}
	if (mask & INODE_ATTR_ATIME)
		fat_put16(sector + 18, atime_date);
	if (mask & INODE_ATTR_MTIME) {
		fat_put16(sector + 22, mtime_time);
		fat_put16(sector + 24, mtime_date);
	}
	result = zedbsd_fat_mark_sector_dirty(&state->legacy);
	if (result == ZEDBSD_FS_OK)
		result = zedbsd_fat_flush(&state->legacy);
	if (result != ZEDBSD_FS_OK) {
		uint8_t *rollback;
		if (zedbsd_fat_write_sector_result(&state->legacy,
		    info->fi_dirent_lba, &rollback) == ZEDBSD_FS_OK) {
			memcpy(rollback + info->fi_dirent_offset, saved, sizeof(saved));
			(void)zedbsd_fat_mark_sector_dirty(&state->legacy);
		}
		return fs_error(result);
	}
	info->fi_attributes = sector[11];
	(void)atime_time;
	return 0;
}

static FAT_MUTATION int
fat_setattr(struct inode *inode, const struct stat *status, unsigned mask)
{
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_setattr_unlocked(inode, status, mask);
	mutex_unlock(&state->lock);
	return error;
}

static struct fat_file_state *
fat_file_get(struct file *file)
{
	struct fat_file_state *slot = NULL;
	struct fat_mount_state *mount_state;
	unsigned i;
	unsigned long irq;
	if (file->f_data != NULL)
		return file->f_data;
	irq = spin_lock_irqsave(&fat_pool_lock);
	for (i = 0; i < FAT_FILE_MAX; i++) {
		if (!fat_files[i].used) {
			fat_files[i].used = 1;
			fat_files[i].owner = file->f_inode;
			memset(&fat_files[i].legacy, 0,
			       sizeof(fat_files[i].legacy));
			slot = &fat_files[i];
			break;
		}
	}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
	if (slot == NULL)
		return NULL;
	mount_state = fat_mount_state(file->f_inode->i_mount);
	if (zedbsd_fs_open_result(&mount_state->legacy,
				  fat_path(file->f_inode), &slot->legacy) !=
	    ZEDBSD_FS_OK) {
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(slot, 0, sizeof(*slot));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
		return NULL;
	}
	/*
	 * Another open file may have extended this inode without yet flushing
	 * its FAT directory entry.  The inode is the coherent in-memory
	 * size/cluster authority for every open description.
	 */
	slot->legacy.size = (uint64_t)file->f_inode->i_size;
	zedbsd_fat_file_state(&slot->legacy)->first_cluster =
		fat_inode(file->f_inode)->fi_first_cluster;
	file->f_data = slot;
	return slot;
}

static int
fat_open_file(struct file *file)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_file_get(file) != NULL ? 0 : EIO;
	mutex_unlock(&state->lock);
	return error;
}

static ssize_t
fat_pread_file_unlocked(struct file *file, void *buffer, size_t length,
			off_t offset)
{
	struct fat_file_state *state = fat_file_get(file);
	uint32_t count;
	enum zedbsd_fs_result result;
	if (state == NULL)
		return -EIO;
	if (offset >= file->f_inode->i_size)
		return 0;
	if (length > (size_t)(file->f_inode->i_size - offset))
		length = (size_t)(file->f_inode->i_size - offset);
	count = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
	result = zedbsd_file_read_result(&state->legacy, (uint64_t)offset,
					buffer, count, NULL, NULL);
	if (result != ZEDBSD_FS_OK)
		return -fs_error(result);
	return count;
}

static void
fat_sync_inode_state(struct inode *inode, const struct zedbsd_file *file)
{
	struct fat_inode_info *info;
	const struct zedbsd_fat_file_state *state;
	unsigned i;
	unsigned long irq;

	if (inode == NULL || file == NULL)
		return;
	info = fat_inode(inode);
	state = (const struct zedbsd_fat_file_state *)file->private_data;
	info->fi_first_cluster = state->first_cluster;
	inode->i_size = (off_t)file->size;
	irq = spin_lock_irqsave(&fat_pool_lock);
	for (i = 0; i < FAT_FILE_MAX; i++) {
		struct zedbsd_fat_file_state *open_state;
		if (!fat_files[i].used || fat_files[i].owner != inode)
			continue;
		fat_files[i].legacy.size = file->size;
		open_state = zedbsd_fat_file_state(&fat_files[i].legacy);
		open_state->first_cluster = state->first_cluster;
	}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
}

static ssize_t
fat_pread_file(struct file *file, void *buffer, size_t length, off_t offset)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	ssize_t count;
	mutex_lock(&state->lock);
	count = fat_pread_file_unlocked(file, buffer, length, offset);
	mutex_unlock(&state->lock);
	return count;
}

static ssize_t
fat_read_file(struct file *file, void *buffer, size_t length)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	ssize_t count;
	mutex_lock(&state->lock);
	count = fat_pread_file_unlocked(file, buffer, length, file->f_offset);
	if (count > 0)
		file->f_offset += count;
	mutex_unlock(&state->lock);
	return count;
}

static ssize_t
fat_pwrite_file_unlocked(struct file *file, const void *buffer, size_t length,
			 off_t offset)
{
	struct fat_file_state *state = fat_file_get(file);
	uint32_t count = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
	enum zedbsd_fs_result result;
	if (state == NULL)
		return -EIO;
	if (offset < 0)
		return -EINVAL;
	if ((uint64_t)offset > UINT32_MAX ||
	    (uint64_t)count > UINT32_MAX - (uint64_t)offset)
		return -EFBIG;
	result = zedbsd_file_write_result(&state->legacy,
					 (uint64_t)offset, buffer, count);
	if (result != ZEDBSD_FS_OK)
		return -fs_error(result);
	fat_sync_inode_state(file->f_inode, &state->legacy);
	return count;
}

static ssize_t
fat_pwrite_file(struct file *file, const void *buffer, size_t length,
		off_t offset)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	ssize_t count;
	mutex_lock(&state->lock);
	count = fat_pwrite_file_unlocked(file, buffer, length, offset);
	mutex_unlock(&state->lock);
	return count;
}

static ssize_t
fat_write_file(struct file *file, const void *buffer, size_t length)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	off_t offset;
	ssize_t count;
	mutex_lock(&state->lock);
	offset = (file->f_flags & O_APPEND) != 0 ?
		file->f_inode->i_size : file->f_offset;
	count = fat_pwrite_file_unlocked(file, buffer, length, offset);
	if (count > 0)
		file->f_offset = offset + count;
	mutex_unlock(&state->lock);
	return count;
}

static int
fat_readdir_unlocked(struct file *file, struct dirent *entry, int *eof)
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
fat_readdir(struct file *file, struct dirent *entry, int *eof)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_readdir_unlocked(file, entry, eof);
	mutex_unlock(&state->lock);
	return error;
}

static int
fat_close_file(struct file *file)
{
	struct fat_file_state *state = file->f_data;
	struct fat_mount_state *mount_state = file->f_inode != NULL ?
		fat_mount_state(file->f_inode->i_mount) : NULL;
	unsigned long irq;
	int error = 0;
	if (mount_state != NULL)
		mutex_lock(&mount_state->lock);
	if (state != NULL) {
		if ((file->f_flags & O_ACCMODE) != O_RDONLY &&
		    file->f_inode != NULL &&
		    (file->f_inode->i_flags & INODE_DEAD) == 0) {
			error = fs_error(zedbsd_file_flush_result(&state->legacy));
			if (error == 0)
				fat_sync_inode_state(file->f_inode, &state->legacy);
		} else if (file->f_inode != NULL &&
			 (file->f_inode->i_flags & INODE_DEAD) != 0)
			error = fs_error(zedbsd_fat_flush(
				&fat_mount_state(file->f_inode->i_mount)->legacy));
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(state, 0, sizeof(*state));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
		file->f_data = NULL;
	}
	if (mount_state != NULL)
		mutex_unlock(&mount_state->lock);
	return error;
}

static int
fat_fsync(struct file *file)
{
	struct fat_mount_state *mount_state =
		fat_mount_state(file->f_inode->i_mount);
	struct fat_file_state *state;
	int error;
	mutex_lock(&mount_state->lock);
	state = fat_file_get(file);
	if (state == NULL)
		error = EIO;
	else if (file->f_inode != NULL &&
	    (file->f_inode->i_flags & INODE_DEAD) != 0)
		error = fs_error(zedbsd_fat_flush(&mount_state->legacy));
	else
		error = fs_error(zedbsd_file_flush_result(&state->legacy));
	if (error == 0)
		error = disk_sync(file->f_inode->i_mount->m_disk);
	mutex_unlock(&mount_state->lock);
	return error;
}

static int
fat_lookup_casefold(struct inode *directory,
		    const struct componentname *name, struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_lookup_casefold_unlocked(directory, name, result);
	mutex_unlock(&state->lock);
	return error;
}

static int
fat_truncate(struct inode *inode, off_t size)
{
	struct zedbsd_file file;
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	enum zedbsd_fs_result result;
	if (size < 0)
		return EINVAL;
	if ((uint64_t)size > UINT32_MAX)
		return EFBIG;
	mutex_lock(&state->lock);
	result = zedbsd_fs_open_result(&state->legacy, fat_path(inode), &file);
	if (result == ZEDBSD_FS_OK)
		result = zedbsd_file_truncate_result(&file, (uint64_t)size);
	if (result == ZEDBSD_FS_OK)
		result = zedbsd_file_flush_result(&file);
	if (result == ZEDBSD_FS_OK)
		fat_sync_inode_state(inode, &file);
	mutex_unlock(&state->lock);
	return fs_error(result);
}

static int
fat_create_unlocked(struct inode *directory, const struct componentname *name,
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
fat_create(struct inode *directory, const struct componentname *name,
	   mode_t mode, struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_create_unlocked(directory, name, mode, result);
	mutex_unlock(&state->lock);
	return error;
}

static FAT_MUTATION void
fat_orphan(struct inode *inode)
{
	struct fat_inode_info *info;
	if (inode == NULL)
		return;
	info = fat_inode(inode);
	info->fi_flags |= FAT_INODE_ORPHANED;
	inode->i_flags |= INODE_DEAD;
	namecache_purge_inode(inode);
}

static FAT_MUTATION void
fat_release_orphan(struct inode *inode)
{
	if (inode == NULL)
		return;
	inode_release(inode);
	inode_free(inode);
}

static FAT_MUTATION int
fat_mkdir_unlocked(struct inode *directory, const struct componentname *name,
		   mode_t mode, struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	char path[ZEDBSD_PATH_MAX];
	int error;
	(void)mode;

	error = join_path(fat_path(directory), name, path);
	if (error != 0)
		return error;
	error = fs_error(zedbsd_fs_mkdir_result(&state->legacy, path));
	if (error != 0)
		return error;
	namecache_remove(directory, name);
	return fat_stat_path(directory->i_mount, path, result);
}

static FAT_MUTATION int
fat_mkdir(struct inode *directory, const struct componentname *name,
	  mode_t mode, struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_mkdir_unlocked(directory, name, mode, result);
	mutex_unlock(&state->lock);
	return error;
}

static FAT_MUTATION int
fat_remove_inode_unlocked(struct inode *directory,
			  const struct componentname *name,
			  int remove_directory, struct inode **orphaned)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *victim = NULL;
	char path[ZEDBSD_PATH_MAX];
	int error;

	error = join_path(fat_path(directory), name, path);
	if (error != 0)
		return error;
	*orphaned = NULL;
	error = fat_lookup_unlocked(directory, name, &victim);
	if (error != 0)
		return error;
	error = fs_error(remove_directory ?
		zedbsd_fs_rmdir_result(&state->legacy, path) :
		zedbsd_fs_unlink_result(&state->legacy, path));
	if (error == 0) {
		namecache_remove(directory, name);
		fat_orphan(victim);
		*orphaned = victim;
	}
	if (error != 0)
		inode_release(victim);
	return error;
}

static FAT_MUTATION int
fat_unlink(struct inode *directory, const struct componentname *name)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *orphaned;
	int error;
	mutex_lock(&state->lock);
	error = fat_remove_inode_unlocked(directory, name, 0, &orphaned);
	mutex_unlock(&state->lock);
	if (orphaned != NULL)
		fat_release_orphan(orphaned);
	return error;
}

static FAT_MUTATION int
fat_rmdir(struct inode *directory, const struct componentname *name)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *orphaned;
	int error;
	mutex_lock(&state->lock);
	error = fat_remove_inode_unlocked(directory, name, 1, &orphaned);
	mutex_unlock(&state->lock);
	if (orphaned != NULL)
		fat_release_orphan(orphaned);
	return error;
}

static FAT_MUTATION int
fat_path_descendant(const char *parent, const char *path)
{
	size_t length = strlen(parent);
	return length != 0 && !memcmp(parent, path, length) &&
		path[length] == '/';
}

static FAT_MUTATION void
fat_repath_descendants(struct mount *mountp, const char *old_path,
		       const char *new_path)
{
	size_t old_length = strlen(old_path), new_length = strlen(new_path);
	unsigned i;
	unsigned long irq = spin_lock_irqsave(&fat_pool_lock);

	for (i = 0; i < FAT_INODE_MAX; i++) {
		char replacement[ZEDBSD_PATH_MAX];
		size_t suffix;
		if (!fat_inodes[i].used ||
		    fat_inodes[i].info.fi_inode.i_mount != mountp ||
		    !fat_path_descendant(old_path, fat_inodes[i].path))
			continue;
		suffix = strlen(fat_inodes[i].path + old_length);
		if (new_length + suffix >= sizeof(replacement))
			continue;
		memcpy(replacement, new_path, new_length);
		memcpy(replacement + new_length, fat_inodes[i].path + old_length,
			suffix + 1U);
		strcpy(fat_inodes[i].path, replacement);
	}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
}

static FAT_MUTATION int
fat_rename_unlocked(struct inode *old_directory,
		    const struct componentname *old_name,
		    struct inode *new_directory,
		    const struct componentname *new_name,
		    unsigned flags, struct inode **orphaned)
{
	struct fat_mount_state *state = fat_mount_state(old_directory->i_mount);
	struct inode *source = NULL, *target = NULL;
	struct fat_inode_info *info;
	struct zedbsd_dirent entry;
	char old_path[ZEDBSD_PATH_MAX], new_path[ZEDBSD_PATH_MAX];
	uint32_t lba, cluster;
	uint16_t offset;
	uint8_t attributes;
	unsigned i;
	unsigned long irq;
	int error;

	*orphaned = NULL;
	if (flags != 0)
		return EINVAL;
	error = join_path(fat_path(old_directory), old_name, old_path);
	if (error == 0)
		error = join_path(fat_path(new_directory), new_name, new_path);
	if (error != 0)
		return error;
	error = fat_lookup_unlocked(old_directory, old_name, &source);
	if (error != 0)
		return error;
	if (source->i_type == INODE_DIR &&
	    (fat_path_descendant(old_path, fat_path(new_directory)) ||
	     !strcmp(old_path, fat_path(new_directory)))) {
		inode_release(source);
		return EINVAL;
	}
	if (fat_lookup_unlocked(new_directory, new_name, &target) == 0 &&
	    target == source) {
		inode_release(target);
		inode_release(source);
		return 0;
	}
	error = fs_error(zedbsd_fs_rename_result(&state->legacy,
		old_path, new_path));
	if (error != 0) {
		if (target != NULL)
			inode_release(target);
		inode_release(source);
		return error;
	}
	if (target != NULL)
		fat_orphan(target);
	error = fs_error(zedbsd_fat_stat_location(&state->legacy, new_path,
		&entry, &lba, &offset, &cluster, &attributes));
	if (error == 0) {
		uint32_t authoritative_cluster;
		off_t authoritative_size;
		struct zedbsd_file renamed_file;

		info = fat_inode(source);
		authoritative_cluster = info->fi_first_cluster;
		authoritative_size = source->i_size;
		info->fi_dirent_lba = lba;
		info->fi_dirent_offset = offset;
		info->fi_attributes = attributes;
		/*
		 * The legacy rename engine copies the on-disk directory entry.  An
		 * open writer may have newer size/cluster state which has not reached
		 * that entry yet.  Keep the inode authoritative and repair the new
		 * directory location before exposing it to a subsequent open.
		 */
		if (source->i_type == INODE_REG &&
		    (cluster != authoritative_cluster ||
		     (off_t)entry.size != authoritative_size)) {
			error = fs_error(zedbsd_fs_open_result(&state->legacy,
				new_path, &renamed_file));
			if (error == 0) {
				renamed_file.size = (uint64_t)authoritative_size;
				zedbsd_fat_file_state(&renamed_file)->first_cluster =
					authoritative_cluster;
				zedbsd_fat_file_state(&renamed_file)->directory_dirty = 1;
				error = fs_error(zedbsd_file_flush_result(&renamed_file));
			}
		}
		source->i_ino = fat_ino(lba, offset);
		irq = spin_lock_irqsave(&fat_pool_lock);
		for (i = 0; i < FAT_FILE_MAX; i++) {
			struct zedbsd_fat_file_state *open_state;
			if (!fat_files[i].used || fat_files[i].owner != source)
				continue;
			open_state = zedbsd_fat_file_state(&fat_files[i].legacy);
			open_state->directory_lba = lba;
			open_state->directory_offset = offset;
		}
		spin_unlock_irqrestore(&fat_pool_lock, irq);
		if (source->i_type == INODE_DIR)
			fat_repath_descendants(old_directory->i_mount,
				old_path, new_path);
		irq = spin_lock_irqsave(&fat_pool_lock);
		strcpy(fat_slot(source)->path, new_path);
		spin_unlock_irqrestore(&fat_pool_lock, irq);
	}
	namecache_remove(old_directory, old_name);
	namecache_remove(new_directory, new_name);
	if (target != NULL)
		*orphaned = target;
	inode_release(source);
	return error;
}

static FAT_MUTATION int
fat_rename(struct inode *old_directory, const struct componentname *old_name,
	   struct inode *new_directory, const struct componentname *new_name,
	   unsigned flags)
{
	struct fat_mount_state *state = fat_mount_state(old_directory->i_mount);
	struct inode *orphaned;
	int error;
	mutex_lock(&state->lock);
	error = fat_rename_unlocked(old_directory, old_name, new_directory,
		new_name, flags, &orphaned);
	mutex_unlock(&state->lock);
	if (orphaned != NULL)
		fat_release_orphan(orphaned);
	return error;
}

static void
fat_reclaim_unlocked(struct inode *inode)
{
	struct fat_inode_info *info = fat_inode(inode);
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	if ((info->fi_flags & FAT_INODE_ORPHANED) != 0 &&
	    info->fi_first_cluster != 0 && state != NULL) {
		(void)zedbsd_fat_discard_chain_result(&state->legacy,
			info->fi_first_cluster);
		info->fi_first_cluster = 0;
	}
}

static void
fat_reclaim(struct inode *inode)
{
	struct fat_inode_info *info = fat_inode(inode);
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	if (state == NULL || info == NULL ||
	    (info->fi_flags & FAT_INODE_ORPHANED) == 0 ||
	    info->fi_first_cluster == 0)
		return;
	mutex_lock(&state->lock);
	fat_reclaim_unlocked(inode);
	mutex_unlock(&state->lock);
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
	unsigned long irq;
	enum zedbsd_fs_result result;
	irq = spin_lock_irqsave(&fat_pool_lock);
	for (i = 0; i < FAT_MOUNT_MAX; i++)
		if (!fat_mounts[i].used) {
			state = &fat_mounts[i];
			memset(state, 0, sizeof(*state));
			state->used = 1;
			break;
		}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
	if (state == NULL)
		return ENOSPC;
	(void)mutex_init(&state->lock, LOCK_RANK_INODE, "FAT mount");
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
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(state, 0, sizeof(*state));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
		return fs_error(result);
	}
	fat_metadata_load(state);
	mountp->m_data = state;
	root = inode_alloc(mountp);
	if (root == NULL) {
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(state, 0, sizeof(*state));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
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
fat_sync_mount(struct mount *mountp)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	int error;
	if (state == NULL)
		return EINVAL;
	mutex_lock(&state->lock);
	error = fs_error(zedbsd_fat_flush(&state->legacy));
	if (error == 0)
		error = disk_sync(mountp->m_disk);
	mutex_unlock(&state->lock);
	return error;
}

static void
fat_unmount_impl(struct mount *mountp)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	unsigned long irq;
	if (state == NULL)
		return;
	mutex_lock(&state->lock);
	zedbsd_fat_invalidate(&state->legacy);
	mutex_unlock(&state->lock);
	irq = spin_lock_irqsave(&fat_pool_lock);
	memset(state, 0, sizeof(*state));
	spin_unlock_irqrestore(&fat_pool_lock, irq);
	mountp->m_data = NULL;
}

static int
fat_statvfs(struct mount *mountp, struct statvfs *result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct zedbsd_fat_state *fat;
	uint32_t free_clusters;
	int error;

	if (state == NULL || result == NULL)
		return EINVAL;
	mutex_lock(&state->lock);
	fat = zedbsd_fat_state(&state->legacy);
	error = fs_error(zedbsd_fat_count_free_clusters(&state->legacy,
	    &free_clusters));
	if (error == 0) {
		memset(result, 0, sizeof(*result));
		result->f_bsize = (uint64_t)fat->sectors_per_cluster * 512U;
		result->f_frsize = result->f_bsize;
		result->f_blocks = fat->cluster_count;
		result->f_bfree = free_clusters;
		result->f_bavail = free_clusters;
		/* FAT has no fixed inode table.  Use clusters as the capacity
		 * unit for the advisory file counts as well. */
		result->f_files = fat->cluster_count;
		result->f_ffree = free_clusters;
		result->f_favail = free_clusters;
		result->f_namemax = NAME_MAX;
	}
	mutex_unlock(&state->lock);
	return error;
}

const struct filesystem_type fat_filesystem_type = {
	.fs_name = "fat",
	.probe = fat_probe,
	.mount = fat_mount_impl,
	.sync = fat_sync_mount,
	.statvfs = fat_statvfs,
	.unmount = fat_unmount_impl,
	.alloc_inode = fat_alloc_inode,
	.free_inode = fat_free_inode,
};

int
fat_file_extents(struct file *file, fat_extent_cb callback, void *context)
{
	struct fat_mount_state *mount_state;
	struct fat_file_state *state;
	int error;
	if (file == NULL || callback == NULL || file->f_inode == NULL ||
	    file->f_inode->i_type != INODE_REG ||
	    file->f_inode->i_mount->m_type != &fat_filesystem_type)
		return EINVAL;
	mount_state = fat_mount_state(file->f_inode->i_mount);
	mutex_lock(&mount_state->lock);
	state = fat_file_get(file);
	if (state == NULL)
		error = EIO;
	else
		error = zedbsd_fat_file_extents(&state->legacy, callback,
			context) == 0 ? 0 : EIO;
	mutex_unlock(&mount_state->lock);
	return error;
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
