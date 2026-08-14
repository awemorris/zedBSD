/*
 * Filesystem interface
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_FS_H
#define ZEDBSD_FS_H

#include <stdint.h>

#ifndef ZEDBSD_PATH_MAX
#define ZEDBSD_PATH_MAX 256
#endif
#define ZEDBSD_FS_PRIVATE_WORDS 160
#define ZEDBSD_FILE_PRIVATE_WORDS 8

struct zedbsd_volume;
struct zedbsd_filesystem;
struct zedbsd_file;

typedef int (*zedbsd_volume_read_t)(const void *context, uint32_t lba,
				    void *buffer);
typedef int (*zedbsd_volume_write_t)(void *context, uint32_t lba,
				     const void *buffer);
typedef void (*zedbsd_read_progress_t)(void *context, uint32_t bytes);

/* Stable internal results.  The public Boolean entry points below are kept as
 * compatibility wrappers while vmunix callers are migrated incrementally. */
enum zedbsd_fs_result {
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
};

/* A partition-sized view of a BIOS block device. LBA values passed to the
 * generic helpers are relative to start_lba; callbacks receive absolute
 * physical LBAs.  A NULL write callback makes the volume read-only. */
struct zedbsd_volume {
	void *context;
	uint32_t start_lba;
	uint16_t sector_size;
	zedbsd_volume_read_t read;
	zedbsd_volume_write_t write;
};

struct zedbsd_dirent {
	char name[ZEDBSD_PATH_MAX];
	uint64_t size;
	uint8_t attributes;
};

struct zedbsd_filesystem_driver {
	const char *name;
	enum zedbsd_fs_result (*probe)(const struct zedbsd_volume *volume);
	enum zedbsd_fs_result (*mount)(struct zedbsd_filesystem *filesystem);
	enum zedbsd_fs_result (*create)(struct zedbsd_filesystem *filesystem,
					const char *path,
					struct zedbsd_file *file);
	enum zedbsd_fs_result (*open)(struct zedbsd_filesystem *filesystem,
				      const char *path,
				      struct zedbsd_file *file);
	enum zedbsd_fs_result (*read)(struct zedbsd_file *file, uint64_t offset,
				      void *buffer, uint32_t length,
				      zedbsd_read_progress_t progress,
				      void *progress_context);
	enum zedbsd_fs_result (*write)(struct zedbsd_file *file, uint64_t offset,
				       const void *buffer, uint32_t length);
	enum zedbsd_fs_result (*truncate)(struct zedbsd_file *file, uint64_t size);
	enum zedbsd_fs_result (*flush)(struct zedbsd_file *file);
	enum zedbsd_fs_result (*readdir)(struct zedbsd_filesystem *filesystem,
					 const char *path, unsigned index,
					 struct zedbsd_dirent *entry);
	enum zedbsd_fs_result (*stat)(struct zedbsd_filesystem *filesystem,
				      const char *path,
				      struct zedbsd_dirent *entry);
	enum zedbsd_fs_result (*contiguous_lba)(struct zedbsd_file *file,
						uint32_t *absolute_lba);
};

struct zedbsd_filesystem {
	const struct zedbsd_filesystem_driver *driver;
	struct zedbsd_volume volume;
	uint32_t private_data[ZEDBSD_FS_PRIVATE_WORDS];
};

struct zedbsd_file {
	struct zedbsd_filesystem *filesystem;
	uint64_t size;
	uint32_t private_data[ZEDBSD_FILE_PRIVATE_WORDS];
};

int zedbsd_volume_read(const struct zedbsd_volume *volume, uint32_t lba,
		       void *buffer);
enum zedbsd_fs_result zedbsd_volume_read_result(
	const struct zedbsd_volume *volume, uint32_t lba, void *buffer);
int zedbsd_volume_write(struct zedbsd_volume *volume, uint32_t lba,
			const void *buffer);
enum zedbsd_fs_result zedbsd_volume_write_result(
	struct zedbsd_volume *volume, uint32_t lba, const void *buffer);
int zedbsd_fs_mount(struct zedbsd_filesystem *filesystem,
		    const struct zedbsd_volume *volume,
		    const struct zedbsd_filesystem_driver *const *drivers,
		    unsigned driver_count);
enum zedbsd_fs_result zedbsd_fs_mount_result(
	struct zedbsd_filesystem *filesystem, const struct zedbsd_volume *volume,
	const struct zedbsd_filesystem_driver *const *drivers,
	unsigned driver_count);
void zedbsd_fs_reset(struct zedbsd_filesystem *filesystem);
int zedbsd_fs_open(struct zedbsd_filesystem *filesystem, const char *path,
		   struct zedbsd_file *file);
enum zedbsd_fs_result zedbsd_fs_open_result(
	struct zedbsd_filesystem *filesystem, const char *path,
	struct zedbsd_file *file);
enum zedbsd_fs_result zedbsd_fs_create_result(
	struct zedbsd_filesystem *filesystem, const char *path,
	struct zedbsd_file *file);
int zedbsd_file_read(struct zedbsd_file *file, uint64_t offset, void *buffer,
		     uint32_t length);
int zedbsd_file_read_progress(struct zedbsd_file *file, uint64_t offset,
			      void *buffer, uint32_t length,
			      zedbsd_read_progress_t progress,
			      void *progress_context);
enum zedbsd_fs_result zedbsd_file_read_result(
	struct zedbsd_file *file, uint64_t offset, void *buffer, uint32_t length,
	zedbsd_read_progress_t progress, void *progress_context);
enum zedbsd_fs_result zedbsd_file_write_result(
	struct zedbsd_file *file, uint64_t offset, const void *buffer,
	uint32_t length);
enum zedbsd_fs_result zedbsd_file_truncate_result(struct zedbsd_file *file,
						  uint64_t size);
enum zedbsd_fs_result zedbsd_file_flush_result(struct zedbsd_file *file);
int zedbsd_fs_readdir(struct zedbsd_filesystem *filesystem, const char *path,
		      unsigned index, struct zedbsd_dirent *entry);
enum zedbsd_fs_result zedbsd_fs_readdir_result(
	struct zedbsd_filesystem *filesystem, const char *path, unsigned index,
	struct zedbsd_dirent *entry);
enum zedbsd_fs_result zedbsd_fs_stat_result(
	struct zedbsd_filesystem *filesystem, const char *path,
	struct zedbsd_dirent *entry);
int zedbsd_file_contiguous_lba(struct zedbsd_file *file,
			       uint32_t *absolute_lba);
enum zedbsd_fs_result zedbsd_file_contiguous_lba_result(
	struct zedbsd_file *file, uint32_t *absolute_lba);

#endif
