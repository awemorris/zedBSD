/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Filesystem interface
 */

#ifndef ZEDBSD_FS_H
#define ZEDBSD_FS_H

#include <stdint.h>

#ifndef ZEDBSD_PATH_MAX
#define ZEDBSD_PATH_MAX			256
#endif

#define ZEDBSD_FS_PRIVATE_WORDS		160
#define ZEDBSD_FILE_PRIVATE_WORDS	8

struct boot_volume;
struct bootfs;
struct bootfs_file;

typedef int (*boot_volume_read_fn)(const void *context, uint32_t lba, void *buffer);
typedef int (*boot_volume_write_fn)(void *context, uint32_t lba, const void *buffer);
typedef void (*bootfs_read_progress_fn)(void *context, uint32_t bytes);

/*
 * Stable internal results.  The public Boolean entry points below are kept as
 * compatibility wrappers while vmunix callers are migrated incrementally.
 */
enum bootfs_result {
	ZEDBSD_FS_OK = 0,
	ZEDBSD_FS_NOT_FOUND,
	ZEDBSD_FS_INVALID_PATH,
	ZEDBSD_FS_READ_ONLY,
	ZEDBSD_FS_NO_SPACE,
	ZEDBSD_FS_IO_ERROR,
	ZEDBSD_FS_CORRUPT,
	ZEDBSD_FS_UNSUPPORTED,
	ZEDBSD_FS_INVALID_ARGUMENT,
	ZEDBSD_FS_EXISTS,
	ZEDBSD_FS_NOT_EMPTY,
	ZEDBSD_FS_IS_DIRECTORY,
};

/*
 * A partition-sized view of a BIOS block device. LBA values passed to
 * the generic helpers are relative to start_lba; callbacks receive
 * absolute physical LBAs.  A NULL write callback makes the volume
 * read-only.
 */
struct boot_volume {
	void *context;
	uint32_t start_lba;
	uint16_t sector_size;
	boot_volume_read_fn read;
	boot_volume_write_fn write;
};

struct bootfs_dirent {
	char name[ZEDBSD_PATH_MAX];
	uint64_t size;
	uint8_t attributes;
};

struct bootfs_driver {
	const char *name;
	enum bootfs_result (*probe)(const struct boot_volume *volume);
	enum bootfs_result (*mount)(struct bootfs *filesystem);
	enum bootfs_result (*create)(struct bootfs *filesystem, const char *path, struct bootfs_file *file);
	enum bootfs_result (*mkdir)(struct bootfs *filesystem, const char *path);
	enum bootfs_result (*unlink)(struct bootfs *filesystem, const char *path);
	enum bootfs_result (*rmdir)(struct bootfs *filesystem, const char *path);
	enum bootfs_result (*rename)(struct bootfs *filesystem, const char *old_path, const char *new_path);
	enum bootfs_result (*open)(struct bootfs *filesystem, const char *path, struct bootfs_file *file);
	enum bootfs_result (*read)(struct bootfs_file *file, uint64_t offset, void *buffer, uint32_t length, bootfs_read_progress_fn progress, void *progress_context);
	enum bootfs_result (*write)(struct bootfs_file *file, uint64_t offset, const void *buffer, uint32_t length);
	enum bootfs_result (*truncate)(struct bootfs_file *file, uint64_t size);
	enum bootfs_result (*flush)(struct bootfs_file *file);
	enum bootfs_result (*readdir)(struct bootfs *filesystem, const char *path, unsigned index, struct bootfs_dirent *entry);
	enum bootfs_result (*stat)(struct bootfs *filesystem, const char *path, struct bootfs_dirent *entry);
	enum bootfs_result (*contiguous_lba)(struct bootfs_file *file, uint32_t *absolute_lba);
};

struct bootfs {
	const struct bootfs_driver *driver;
	struct boot_volume volume;
	uint32_t private_data[ZEDBSD_FS_PRIVATE_WORDS];
};

struct bootfs_file {
	struct bootfs *filesystem;
	uint64_t size;
	uint32_t private_data[ZEDBSD_FILE_PRIVATE_WORDS];
};

int
boot_volume_read(
	const struct boot_volume *volume,
	uint32_t lba,
	void *buffer);

enum bootfs_result
boot_volume_read_result(
	const struct boot_volume *volume,
	uint32_t lba,
	void *buffer);

int
boot_volume_write(
	struct boot_volume *volume,
	uint32_t lba,
	const void *buffer);

enum bootfs_result
boot_volume_write_result(
	struct boot_volume *volume,
	uint32_t lba,
	const void *buffer);

int
bootfs_mount(
	struct bootfs *filesystem,
	const struct boot_volume *volume,
	const struct bootfs_driver *const *drivers,
	unsigned driver_count);

enum bootfs_result
bootfs_mount_result(
	struct bootfs *filesystem,
	const struct boot_volume *volume,
	const struct bootfs_driver *const *drivers,
	unsigned driver_count);

void
bootfs_reset(
	struct bootfs *filesystem);

int
bootfs_open(
	struct bootfs *filesystem,
	const char *path,
	struct bootfs_file *file);

enum bootfs_result
bootfs_open_result(
	struct bootfs *filesystem,
	const char *path,
	struct bootfs_file *file);

enum bootfs_result
bootfs_create_result(
	struct bootfs *filesystem,
	const char *path,
	struct bootfs_file *file);

enum bootfs_result
bootfs_mkdir_result(
	struct bootfs *,
	const char *);

enum bootfs_result
bootfs_unlink_result(
	struct bootfs *,
	const char *);

enum bootfs_result
bootfs_rmdir_result(
	struct bootfs *,
	const char *);

enum bootfs_result
bootfs_rename_result(
	struct bootfs *,
	const char *,
	const char *);

int
bootfs_file_read(
	struct bootfs_file *file,
	uint64_t offset,
	void *buffer,
	uint32_t length);

int
bootfs_file_read_progress(
	struct bootfs_file *file,
	uint64_t offset,
	void *buffer,
	uint32_t length,
	bootfs_read_progress_fn progress,
	void *progress_context);

enum bootfs_result
bootfs_file_read_result(
	struct bootfs_file *file,
	uint64_t offset,
	void *buffer,
	uint32_t length,
	bootfs_read_progress_fn progress,
	void *progress_context);

enum bootfs_result
bootfs_file_write_result(
	struct bootfs_file *file,
	uint64_t offset,
	const void *buffer,
	uint32_t length);

enum bootfs_result
bootfs_file_truncate_result(
	struct bootfs_file *file,
	uint64_t size);

enum bootfs_result
bootfs_file_flush_result(
	struct bootfs_file *file);

int
bootfs_readdir(
	struct bootfs *filesystem,
	const char *path,
	unsigned index,
	struct bootfs_dirent *entry);

enum bootfs_result
bootfs_readdir_result(
	struct bootfs *filesystem,
	const char *path,
	unsigned index,
	struct bootfs_dirent *entry);

enum bootfs_result
bootfs_stat_result(
	struct bootfs *filesystem,
	const char *path,
	struct bootfs_dirent *entry);

int
bootfs_file_contiguous_lba(
	struct bootfs_file *file,
	uint32_t *absolute_lba);

enum bootfs_result
bootfs_file_contiguous_lba_result(
	struct bootfs_file *file,
	uint32_t *absolute_lba);

#endif
