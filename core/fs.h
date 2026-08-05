/*
 * Boots filesystem interface
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_FS_H
#define BOOTS_FS_H

#include <stdint.h>

#define BOOTS_PATH_MAX 256
#define BOOTS_FS_PRIVATE_WORDS 16
#define BOOTS_FILE_PRIVATE_WORDS 8

struct boots_volume;
struct boots_filesystem;
struct boots_file;

typedef int (*boots_volume_read_t)(const void *context, uint32_t lba,
				    void *buffer);
typedef int (*boots_volume_write_t)(void *context, uint32_t lba,
				     const void *buffer);
typedef void (*boots_read_progress_t)(void *context, uint32_t bytes);

/* Stable internal results.  The public Boolean entry points below are kept as
 * compatibility wrappers while BOOT.SYS callers are migrated incrementally. */
enum boots_fs_result {
	BOOTS_FS_OK = 0,
	BOOTS_FS_NOT_FOUND,
	BOOTS_FS_INVALID_PATH,
	BOOTS_FS_READ_ONLY,
	BOOTS_FS_NO_SPACE,
	BOOTS_FS_IO_ERROR,
	BOOTS_FS_CORRUPT,
	BOOTS_FS_UNSUPPORTED,
	BOOTS_FS_INVALID_ARGUMENT,
};

/* A partition-sized view of a BIOS block device. LBA values passed to the
 * generic helpers are relative to start_lba; callbacks receive absolute
 * physical LBAs.  A NULL write callback makes the volume read-only. */
struct boots_volume {
	void *context;
	uint32_t start_lba;
	uint16_t sector_size;
	boots_volume_read_t read;
	boots_volume_write_t write;
};

struct boots_dirent {
	char name[BOOTS_PATH_MAX];
	uint64_t size;
	uint8_t attributes;
};

struct boots_filesystem_driver {
	const char *name;
	enum boots_fs_result (*probe)(const struct boots_volume *volume);
	enum boots_fs_result (*mount)(struct boots_filesystem *filesystem);
	enum boots_fs_result (*create)(struct boots_filesystem *filesystem,
					const char *path,
					struct boots_file *file);
	enum boots_fs_result (*open)(struct boots_filesystem *filesystem,
				      const char *path,
				      struct boots_file *file);
	enum boots_fs_result (*read)(struct boots_file *file, uint64_t offset,
				      void *buffer, uint32_t length,
				      boots_read_progress_t progress,
				      void *progress_context);
	enum boots_fs_result (*write)(struct boots_file *file, uint64_t offset,
				       const void *buffer, uint32_t length);
	enum boots_fs_result (*truncate)(struct boots_file *file, uint64_t size);
	enum boots_fs_result (*flush)(struct boots_file *file);
	enum boots_fs_result (*readdir)(struct boots_filesystem *filesystem,
					 const char *path, unsigned index,
					 struct boots_dirent *entry);
	enum boots_fs_result (*stat)(struct boots_filesystem *filesystem,
				      const char *path,
				      struct boots_dirent *entry);
	enum boots_fs_result (*contiguous_lba)(struct boots_file *file,
						uint32_t *absolute_lba);
};

struct boots_filesystem {
	const struct boots_filesystem_driver *driver;
	struct boots_volume volume;
	uint32_t private_data[BOOTS_FS_PRIVATE_WORDS];
};

struct boots_file {
	struct boots_filesystem *filesystem;
	uint64_t size;
	uint32_t private_data[BOOTS_FILE_PRIVATE_WORDS];
};

int boots_volume_read(const struct boots_volume *volume, uint32_t lba,
		       void *buffer);
enum boots_fs_result boots_volume_read_result(
	const struct boots_volume *volume, uint32_t lba, void *buffer);
int boots_volume_write(struct boots_volume *volume, uint32_t lba,
			const void *buffer);
enum boots_fs_result boots_volume_write_result(
	struct boots_volume *volume, uint32_t lba, const void *buffer);
int boots_fs_mount(struct boots_filesystem *filesystem,
		    const struct boots_volume *volume,
		    const struct boots_filesystem_driver *const *drivers,
		    unsigned driver_count);
enum boots_fs_result boots_fs_mount_result(
	struct boots_filesystem *filesystem, const struct boots_volume *volume,
	const struct boots_filesystem_driver *const *drivers,
	unsigned driver_count);
void boots_fs_reset(struct boots_filesystem *filesystem);
int boots_fs_open(struct boots_filesystem *filesystem, const char *path,
		   struct boots_file *file);
enum boots_fs_result boots_fs_open_result(
	struct boots_filesystem *filesystem, const char *path,
	struct boots_file *file);
enum boots_fs_result boots_fs_create_result(
	struct boots_filesystem *filesystem, const char *path,
	struct boots_file *file);
int boots_file_read(struct boots_file *file, uint64_t offset, void *buffer,
		     uint32_t length);
int boots_file_read_progress(struct boots_file *file, uint64_t offset,
			      void *buffer, uint32_t length,
			      boots_read_progress_t progress,
			      void *progress_context);
enum boots_fs_result boots_file_read_result(
	struct boots_file *file, uint64_t offset, void *buffer, uint32_t length,
	boots_read_progress_t progress, void *progress_context);
enum boots_fs_result boots_file_write_result(
	struct boots_file *file, uint64_t offset, const void *buffer,
	uint32_t length);
enum boots_fs_result boots_file_truncate_result(struct boots_file *file,
						  uint64_t size);
enum boots_fs_result boots_file_flush_result(struct boots_file *file);
int boots_fs_readdir(struct boots_filesystem *filesystem, const char *path,
		      unsigned index, struct boots_dirent *entry);
enum boots_fs_result boots_fs_readdir_result(
	struct boots_filesystem *filesystem, const char *path, unsigned index,
	struct boots_dirent *entry);
enum boots_fs_result boots_fs_stat_result(
	struct boots_filesystem *filesystem, const char *path,
	struct boots_dirent *entry);
int boots_file_contiguous_lba(struct boots_file *file,
			       uint32_t *absolute_lba);
enum boots_fs_result boots_file_contiguous_lba_result(
	struct boots_file *file, uint32_t *absolute_lba);

#endif
