/*
 * Boots filesystem dispatch
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/fs.h"

static void clear_bytes(void *pointer, uint32_t length)
{
	uint8_t *bytes = pointer;

	while (length--)
		*bytes++ = 0;
}

enum boots_fs_result boots_volume_read_result(
	const struct boots_volume *volume, uint32_t lba, void *buffer)
{
	if (!volume || !buffer || volume->sector_size != 512)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!volume->read)
		return BOOTS_FS_UNSUPPORTED;
	if (lba + volume->start_lba < lba)
		return BOOTS_FS_INVALID_ARGUMENT;
	return volume->read(volume->context, volume->start_lba + lba, buffer) ?
		BOOTS_FS_OK : BOOTS_FS_IO_ERROR;
}

int boots_volume_read(const struct boots_volume *volume, uint32_t lba,
		       void *buffer)
{
	return boots_volume_read_result(volume, lba, buffer) == BOOTS_FS_OK;
}

enum boots_fs_result boots_volume_write_result(
	struct boots_volume *volume, uint32_t lba, const void *buffer)
{
	if (!volume || !buffer || volume->sector_size != 512)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!volume->write)
		return BOOTS_FS_READ_ONLY;
	if (lba + volume->start_lba < lba)
		return BOOTS_FS_INVALID_ARGUMENT;
	return volume->write(volume->context, volume->start_lba + lba, buffer) ?
		BOOTS_FS_OK : BOOTS_FS_IO_ERROR;
}

int boots_volume_write(struct boots_volume *volume, uint32_t lba,
			const void *buffer)
{
	return boots_volume_write_result(volume, lba, buffer) == BOOTS_FS_OK;
}

void boots_fs_reset(struct boots_filesystem *filesystem)
{
	if (filesystem)
		clear_bytes(filesystem, sizeof(*filesystem));
}

enum boots_fs_result boots_fs_mount_result(
	struct boots_filesystem *filesystem, const struct boots_volume *volume,
	const struct boots_filesystem_driver *const *drivers,
	unsigned driver_count)
{
	enum boots_fs_result last = BOOTS_FS_UNSUPPORTED;

	if (!filesystem || !volume || !drivers || !volume->read)
		return BOOTS_FS_INVALID_ARGUMENT;
	for (unsigned i = 0; i < driver_count; i++) {
		const struct boots_filesystem_driver *driver = drivers[i];
		enum boots_fs_result result;

		if (!driver || !driver->probe || !driver->mount || !driver->open ||
		    !driver->read || !driver->readdir)
			continue;
		result = driver->probe(volume);
		if (result != BOOTS_FS_OK) {
			if (result != BOOTS_FS_UNSUPPORTED ||
			    last == BOOTS_FS_UNSUPPORTED)
				last = result;
			continue;
		}
		boots_fs_reset(filesystem);
		filesystem->driver = driver;
		filesystem->volume = *volume;
		result = driver->mount(filesystem);
		if (result == BOOTS_FS_OK)
			return result;
		last = result;
	}
	boots_fs_reset(filesystem);
	return last;
}

int boots_fs_mount(struct boots_filesystem *filesystem,
		    const struct boots_volume *volume,
		    const struct boots_filesystem_driver *const *drivers,
		    unsigned driver_count)
{
	return boots_fs_mount_result(filesystem, volume, drivers,
				      driver_count) == BOOTS_FS_OK;
}

enum boots_fs_result boots_fs_open_result(
	struct boots_filesystem *filesystem, const char *path,
	struct boots_file *file)
{
	enum boots_fs_result result;

	if (!filesystem || !filesystem->driver || !path || !*path || !file)
		return BOOTS_FS_INVALID_ARGUMENT;
	clear_bytes(file, sizeof(*file));
	file->filesystem = filesystem;
	result = filesystem->driver->open(filesystem, path, file);
	if (result == BOOTS_FS_OK)
		return result;
	clear_bytes(file, sizeof(*file));
	return result;
}

int boots_fs_open(struct boots_filesystem *filesystem, const char *path,
		   struct boots_file *file)
{
	return boots_fs_open_result(filesystem, path, file) == BOOTS_FS_OK;
}

enum boots_fs_result boots_fs_create_result(
	struct boots_filesystem *filesystem, const char *path,
	struct boots_file *file)
{
	enum boots_fs_result result;

	if (!filesystem || !filesystem->driver || !path || !*path || !file)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!filesystem->driver->create)
		return BOOTS_FS_READ_ONLY;
	clear_bytes(file, sizeof(*file));
	file->filesystem = filesystem;
	result = filesystem->driver->create(filesystem, path, file);
	if (result == BOOTS_FS_OK)
		return result;
	clear_bytes(file, sizeof(*file));
	return result;
}

enum boots_fs_result boots_file_read_result(
	struct boots_file *file, uint64_t offset, void *buffer, uint32_t length,
	boots_read_progress_t progress, void *progress_context)
{
	if (!file || !file->filesystem || !file->filesystem->driver || !buffer ||
	    offset > file->size || length > file->size - offset)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!length)
		return BOOTS_FS_OK;
	return file->filesystem->driver->read(file, offset, buffer, length,
					     progress, progress_context);
}

int boots_file_read_progress(struct boots_file *file, uint64_t offset,
			      void *buffer, uint32_t length,
			      boots_read_progress_t progress,
			      void *progress_context)
{
	return boots_file_read_result(file, offset, buffer, length, progress,
				       progress_context) == BOOTS_FS_OK;
}

int boots_file_read(struct boots_file *file, uint64_t offset, void *buffer,
		     uint32_t length)
{
	return boots_file_read_progress(file, offset, buffer, length, 0, 0);
}

enum boots_fs_result boots_file_write_result(
	struct boots_file *file, uint64_t offset, const void *buffer,
	uint32_t length)
{
	if (!file || !file->filesystem || !file->filesystem->driver || !buffer ||
	    (uint64_t)length > UINT64_MAX - offset)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!file->filesystem->driver->write)
		return BOOTS_FS_READ_ONLY;
	return file->filesystem->driver->write(file, offset, buffer, length);
}

enum boots_fs_result boots_file_truncate_result(struct boots_file *file,
						  uint64_t size)
{
	if (!file || !file->filesystem || !file->filesystem->driver)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!file->filesystem->driver->truncate)
		return BOOTS_FS_READ_ONLY;
	return file->filesystem->driver->truncate(file, size);
}

enum boots_fs_result boots_file_flush_result(struct boots_file *file)
{
	if (!file || !file->filesystem || !file->filesystem->driver)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!file->filesystem->driver->flush)
		return BOOTS_FS_READ_ONLY;
	return file->filesystem->driver->flush(file);
}

enum boots_fs_result boots_fs_readdir_result(
	struct boots_filesystem *filesystem, const char *path, unsigned index,
	struct boots_dirent *entry)
{
	if (!filesystem || !filesystem->driver || !entry)
		return BOOTS_FS_INVALID_ARGUMENT;
	clear_bytes(entry, sizeof(*entry));
	return filesystem->driver->readdir(filesystem, path ? path : "", index,
					   entry);
}

int boots_fs_readdir(struct boots_filesystem *filesystem, const char *path,
		      unsigned index, struct boots_dirent *entry)
{
	return boots_fs_readdir_result(filesystem, path, index, entry) ==
		BOOTS_FS_OK;
}

enum boots_fs_result boots_fs_stat_result(
	struct boots_filesystem *filesystem, const char *path,
	struct boots_dirent *entry)
{
	if (!filesystem || !filesystem->driver || !path || !*path || !entry)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!filesystem->driver->stat)
		return BOOTS_FS_UNSUPPORTED;
	clear_bytes(entry, sizeof(*entry));
	return filesystem->driver->stat(filesystem, path, entry);
}

enum boots_fs_result boots_file_contiguous_lba_result(
	struct boots_file *file, uint32_t *absolute_lba)
{
	if (!file || !file->filesystem || !file->filesystem->driver ||
	    !absolute_lba)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!file->filesystem->driver->contiguous_lba)
		return BOOTS_FS_UNSUPPORTED;
	return file->filesystem->driver->contiguous_lba(file, absolute_lba);
}

int boots_file_contiguous_lba(struct boots_file *file,
			       uint32_t *absolute_lba)
{
	return boots_file_contiguous_lba_result(file, absolute_lba) ==
		BOOTS_FS_OK;
}
