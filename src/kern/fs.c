/*
 * zedBSD filesystem dispatch
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/fs.h"

#include <stddef.h>

static void clear_bytes(void *pointer, uint32_t length)
{
	uint8_t *bytes = pointer;

	while (length--)
		*bytes++ = 0;
}

enum zedbsd_fs_result zedbsd_volume_read_result(
	const struct zedbsd_volume *volume, uint32_t lba, void *buffer)
{
	if (!volume || !buffer || volume->sector_size != 512)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!volume->read)
		return ZEDBSD_FS_UNSUPPORTED;
	if (lba + volume->start_lba < lba)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	return volume->read(volume->context, volume->start_lba + lba, buffer) ?
		ZEDBSD_FS_OK : ZEDBSD_FS_IO_ERROR;
}

int zedbsd_volume_read(const struct zedbsd_volume *volume, uint32_t lba,
		       void *buffer)
{
	return zedbsd_volume_read_result(volume, lba, buffer) == ZEDBSD_FS_OK;
}

enum zedbsd_fs_result zedbsd_volume_write_result(
	struct zedbsd_volume *volume, uint32_t lba, const void *buffer)
{
	if (!volume || !buffer || volume->sector_size != 512)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!volume->write)
		return ZEDBSD_FS_READ_ONLY;
	if (lba + volume->start_lba < lba)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	return volume->write(volume->context, volume->start_lba + lba, buffer) ?
		ZEDBSD_FS_OK : ZEDBSD_FS_IO_ERROR;
}

int zedbsd_volume_write(struct zedbsd_volume *volume, uint32_t lba,
			const void *buffer)
{
	return zedbsd_volume_write_result(volume, lba, buffer) == ZEDBSD_FS_OK;
}

void zedbsd_fs_reset(struct zedbsd_filesystem *filesystem)
{
	if (filesystem)
		clear_bytes(filesystem, sizeof(*filesystem));
}

enum zedbsd_fs_result zedbsd_fs_mount_result(
	struct zedbsd_filesystem *filesystem, const struct zedbsd_volume *volume,
	const struct zedbsd_filesystem_driver *const *drivers,
	unsigned driver_count)
{
	enum zedbsd_fs_result last = ZEDBSD_FS_UNSUPPORTED;

	if (!filesystem || !volume || !drivers || !volume->read)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	for (unsigned i = 0; i < driver_count; i++) {
		const struct zedbsd_filesystem_driver *driver = drivers[i];
		enum zedbsd_fs_result result;

		if (!driver || !driver->probe || !driver->mount || !driver->open ||
		    !driver->read || !driver->readdir)
			continue;
		result = driver->probe(volume);
		if (result != ZEDBSD_FS_OK) {
			if (result != ZEDBSD_FS_UNSUPPORTED ||
			    last == ZEDBSD_FS_UNSUPPORTED)
				last = result;
			continue;
		}
		zedbsd_fs_reset(filesystem);
		filesystem->driver = driver;
		filesystem->volume = *volume;
		result = driver->mount(filesystem);
		if (result == ZEDBSD_FS_OK)
			return result;
		last = result;
	}
	zedbsd_fs_reset(filesystem);
	return last;
}

int zedbsd_fs_mount(struct zedbsd_filesystem *filesystem,
		    const struct zedbsd_volume *volume,
		    const struct zedbsd_filesystem_driver *const *drivers,
		    unsigned driver_count)
{
	return zedbsd_fs_mount_result(filesystem, volume, drivers,
				      driver_count) == ZEDBSD_FS_OK;
}

enum zedbsd_fs_result zedbsd_fs_open_result(
	struct zedbsd_filesystem *filesystem, const char *path,
	struct zedbsd_file *file)
{
	enum zedbsd_fs_result result;

	if (!filesystem || !filesystem->driver || !path || !*path || !file)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	clear_bytes(file, sizeof(*file));
	file->filesystem = filesystem;
	result = filesystem->driver->open(filesystem, path, file);
	if (result == ZEDBSD_FS_OK)
		return result;
	clear_bytes(file, sizeof(*file));
	return result;
}

int zedbsd_fs_open(struct zedbsd_filesystem *filesystem, const char *path,
		   struct zedbsd_file *file)
{
	return zedbsd_fs_open_result(filesystem, path, file) == ZEDBSD_FS_OK;
}

enum zedbsd_fs_result zedbsd_fs_create_result(
	struct zedbsd_filesystem *filesystem, const char *path,
	struct zedbsd_file *file)
{
	enum zedbsd_fs_result result;

	if (!filesystem || !filesystem->driver || !path || !*path || !file)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!filesystem->driver->create)
		return ZEDBSD_FS_READ_ONLY;
	clear_bytes(file, sizeof(*file));
	file->filesystem = filesystem;
	result = filesystem->driver->create(filesystem, path, file);
	if (result == ZEDBSD_FS_OK)
		return result;
	clear_bytes(file, sizeof(*file));
	return result;
}

static enum zedbsd_fs_result
path_operation(struct zedbsd_filesystem *filesystem, const char *path,
	       enum zedbsd_fs_result (*operation)(struct zedbsd_filesystem *,
						    const char *))
{
	if (filesystem == NULL || filesystem->driver == NULL || path == NULL ||
	    *path == '\0')
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (operation == NULL)
		return ZEDBSD_FS_UNSUPPORTED;
	return operation(filesystem, path);
}

enum zedbsd_fs_result
zedbsd_fs_mkdir_result(struct zedbsd_filesystem *filesystem, const char *path)
{
	return path_operation(filesystem, path,
		filesystem != NULL && filesystem->driver != NULL ?
		filesystem->driver->mkdir : NULL);
}

enum zedbsd_fs_result
zedbsd_fs_unlink_result(struct zedbsd_filesystem *filesystem, const char *path)
{
	return path_operation(filesystem, path,
		filesystem != NULL && filesystem->driver != NULL ?
		filesystem->driver->unlink : NULL);
}

enum zedbsd_fs_result
zedbsd_fs_rmdir_result(struct zedbsd_filesystem *filesystem, const char *path)
{
	return path_operation(filesystem, path,
		filesystem != NULL && filesystem->driver != NULL ?
		filesystem->driver->rmdir : NULL);
}

enum zedbsd_fs_result
zedbsd_fs_rename_result(struct zedbsd_filesystem *filesystem,
			const char *old_path, const char *new_path)
{
	if (filesystem == NULL || filesystem->driver == NULL || old_path == NULL ||
	    new_path == NULL || *old_path == '\0' || *new_path == '\0')
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (filesystem->driver->rename == NULL)
		return ZEDBSD_FS_UNSUPPORTED;
	return filesystem->driver->rename(filesystem, old_path, new_path);
}

enum zedbsd_fs_result zedbsd_file_read_result(
	struct zedbsd_file *file, uint64_t offset, void *buffer, uint32_t length,
	zedbsd_read_progress_t progress, void *progress_context)
{
	if (!file || !file->filesystem || !file->filesystem->driver || !buffer ||
	    offset > file->size || length > file->size - offset)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!length)
		return ZEDBSD_FS_OK;
	return file->filesystem->driver->read(file, offset, buffer, length,
					     progress, progress_context);
}

int zedbsd_file_read_progress(struct zedbsd_file *file, uint64_t offset,
			      void *buffer, uint32_t length,
			      zedbsd_read_progress_t progress,
			      void *progress_context)
{
	return zedbsd_file_read_result(file, offset, buffer, length, progress,
				       progress_context) == ZEDBSD_FS_OK;
}

int zedbsd_file_read(struct zedbsd_file *file, uint64_t offset, void *buffer,
		     uint32_t length)
{
	return zedbsd_file_read_progress(file, offset, buffer, length, 0, 0);
}

enum zedbsd_fs_result zedbsd_file_write_result(
	struct zedbsd_file *file, uint64_t offset, const void *buffer,
	uint32_t length)
{
	if (!file || !file->filesystem || !file->filesystem->driver || !buffer ||
	    (uint64_t)length > UINT64_MAX - offset)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!file->filesystem->driver->write)
		return ZEDBSD_FS_READ_ONLY;
	return file->filesystem->driver->write(file, offset, buffer, length);
}

enum zedbsd_fs_result zedbsd_file_truncate_result(struct zedbsd_file *file,
						  uint64_t size)
{
	if (!file || !file->filesystem || !file->filesystem->driver)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!file->filesystem->driver->truncate)
		return ZEDBSD_FS_READ_ONLY;
	return file->filesystem->driver->truncate(file, size);
}

enum zedbsd_fs_result zedbsd_file_flush_result(struct zedbsd_file *file)
{
	if (!file || !file->filesystem || !file->filesystem->driver)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!file->filesystem->driver->flush)
		return ZEDBSD_FS_READ_ONLY;
	return file->filesystem->driver->flush(file);
}

enum zedbsd_fs_result zedbsd_fs_readdir_result(
	struct zedbsd_filesystem *filesystem, const char *path, unsigned index,
	struct zedbsd_dirent *entry)
{
	if (!filesystem || !filesystem->driver || !entry)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	clear_bytes(entry, sizeof(*entry));
	return filesystem->driver->readdir(filesystem, path ? path : "", index,
					   entry);
}

int zedbsd_fs_readdir(struct zedbsd_filesystem *filesystem, const char *path,
		      unsigned index, struct zedbsd_dirent *entry)
{
	return zedbsd_fs_readdir_result(filesystem, path, index, entry) ==
		ZEDBSD_FS_OK;
}

enum zedbsd_fs_result zedbsd_fs_stat_result(
	struct zedbsd_filesystem *filesystem, const char *path,
	struct zedbsd_dirent *entry)
{
	if (!filesystem || !filesystem->driver || !path || !*path || !entry)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!filesystem->driver->stat)
		return ZEDBSD_FS_UNSUPPORTED;
	clear_bytes(entry, sizeof(*entry));
	return filesystem->driver->stat(filesystem, path, entry);
}

enum zedbsd_fs_result zedbsd_file_contiguous_lba_result(
	struct zedbsd_file *file, uint32_t *absolute_lba)
{
	if (!file || !file->filesystem || !file->filesystem->driver ||
	    !absolute_lba)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!file->filesystem->driver->contiguous_lba)
		return ZEDBSD_FS_UNSUPPORTED;
	return file->filesystem->driver->contiguous_lba(file, absolute_lba);
}

int zedbsd_file_contiguous_lba(struct zedbsd_file *file,
			       uint32_t *absolute_lba)
{
	return zedbsd_file_contiguous_lba_result(file, absolute_lba) ==
		ZEDBSD_FS_OK;
}
