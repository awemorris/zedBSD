/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Boots filesystem-backed stdio for kernel mode Noct.
 * This will be removed after moving Noct to userspace.
 */

#include "libc/stdio-fs.h"
#include "kern/env.h"
#include "kern/fs.h"
#include "kern/namespace.h"
#include "kern/file.h"
#include "kern/namei.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STREAM_READ 1U
#define STREAM_WRITE 2U

/* Small standalone libc tests do not link the Boots environment store. */
extern const char *boots_env_get(
	const struct boots_environment *environment,
	const char *name) __attribute__((weak));

struct filesystem_stream {
	FILE stream;
	struct boots_file file;
	struct file *vfile;
	struct filesystem_stream *next;
};

static struct boots_filesystem *active_filesystem;
static struct boots_namespace *active_namespace;
static struct boots_environment *active_environment;
static struct cwdinfo *active_context;
static struct filesystem_stream *open_streams;
static char current_directory[BOOTS_PATH_MAX] = "/";

static int result_errno(enum boots_fs_result result)
{
	switch (result) {
	case BOOTS_FS_NOT_FOUND:
		return ENOENT;
	case BOOTS_FS_READ_ONLY:
		return EROFS;
	case BOOTS_FS_NO_SPACE:
		return ENOSPC;
	case BOOTS_FS_INVALID_PATH:
	case BOOTS_FS_INVALID_ARGUMENT:
		return EINVAL;
	default:
		return EIO;
	}
}

static struct filesystem_stream *filesystem_stream(FILE *stream)
{
	struct filesystem_stream *candidate;

	for (candidate = open_streams; candidate != NULL;
	     candidate = candidate->next)
		if (&candidate->stream == stream)
			return candidate;
	return NULL;
}

void boots_stdio_set_filesystem(struct boots_filesystem *filesystem)
{
	active_filesystem = filesystem;
}

void boots_stdio_set_namespace(struct boots_namespace *namespace)
{
	const char *name;

	active_namespace = namespace;
	active_context = NULL;
	current_directory[0] = '/';
	current_directory[1] = '\0';
	name = boots_namespace_default_name(namespace);
	if (name != NULL && strlen(name) + 2U <= sizeof(current_directory)) {
		current_directory[1] = '\0';
		strcat(current_directory, name);
	}
}

void boots_stdio_set_context(struct cwdinfo *context)
{
	active_context = context;
	active_namespace = NULL;
	active_filesystem = NULL;
}

void boots_stdio_set_environment(struct boots_environment *environment)
{
	active_environment = environment;
}

FILE *fopen(const char *path, const char *mode)
{
	struct filesystem_stream *handle;
	char absolute[BOOTS_PATH_MAX];
	const char *resolved_path = path;
	enum boots_fs_result result;
	unsigned flags;

	if (path == NULL || mode == NULL ||
	    (active_filesystem == NULL && active_namespace == NULL &&
	     active_context == NULL)) {
		errno = path == NULL || mode == NULL ? EINVAL : ENOENT;
		return NULL;
	}
	if (active_namespace != NULL && path[0] != '/') {
		size_t cwd_length = strlen(current_directory);
		size_t path_length = strlen(path);

		if (cwd_length + 1U + path_length >= sizeof(absolute)) {
			errno = ENAMETOOLONG;
			return NULL;
		}
		memcpy(absolute, current_directory, cwd_length);
		if (cwd_length == 0 || absolute[cwd_length - 1U] != '/')
			absolute[cwd_length++] = '/';
		memcpy(absolute + cwd_length, path, path_length + 1U);
		resolved_path = absolute;
	}
	if (!strcmp(mode, "r") || !strcmp(mode, "rb"))
		flags = STREAM_READ;
	else if (!strcmp(mode, "w") || !strcmp(mode, "wb"))
		flags = STREAM_WRITE;
	else {
		errno = EINVAL;
		return NULL;
	}
	handle = malloc(sizeof(*handle));
	if (handle == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	memset(handle, 0, sizeof(*handle));
	if (active_context != NULL) {
		int error = file_openat(active_context, path,
			flags == STREAM_WRITE ? O_WRONLY | O_CREAT | O_TRUNC :
			O_RDONLY, 0644U, &handle->vfile);
		if (error != 0) {
			errno = error;
			free(handle);
			return NULL;
		}
		result = BOOTS_FS_OK;
	} else if (active_namespace != NULL)
		result = flags == STREAM_WRITE ?
			boots_namespace_create_result(active_namespace,
						       resolved_path,
						       &handle->file) :
			boots_namespace_open_result(active_namespace,
						     resolved_path,
						     &handle->file);
	else
		result = flags == STREAM_WRITE ?
			boots_fs_create_result(active_filesystem, resolved_path,
						&handle->file) :
			boots_fs_open_result(active_filesystem, resolved_path,
					      &handle->file);
	if (result != BOOTS_FS_OK) {
		errno = result_errno(result);
		free(handle);
		return NULL;
	}
	handle->stream.context = &handle->file;
	handle->stream.mode = flags;
	handle->next = open_streams;
	open_streams = handle;
	return &handle->stream;
}

int fflush(FILE *stream)
{
	struct filesystem_stream *handle;
	enum boots_fs_result result;
	int failed = 0;

	if (stream == NULL) {
		for (handle = open_streams; handle != NULL; handle = handle->next)
			if ((handle->stream.mode & STREAM_WRITE) &&
			    fflush(&handle->stream) == EOF)
				failed = 1;
		return failed ? EOF : 0;
	}
	if (stream == stdout || stream == stderr)
		return 0;
	handle = filesystem_stream(stream);
	if (handle == NULL) {
		errno = EINVAL;
		return EOF;
	}
	if (!(stream->mode & STREAM_WRITE))
		return 0;
	if (handle->vfile != NULL) {
		int error = file_fsync(handle->vfile);
		if (error != 0) {
			stream->error = 1; errno = error; return EOF;
		}
		return 0;
	}
	result = boots_file_flush_result(&handle->file);
	if (result != BOOTS_FS_OK) {
		stream->error = 1;
		errno = result_errno(result);
		return EOF;
	}
	return 0;
}

int fclose(FILE *stream)
{
	struct filesystem_stream **link = &open_streams;
	struct filesystem_stream *handle;
	int result;

	while (*link != NULL && &(*link)->stream != stream)
		link = &(*link)->next;
	if (*link == NULL) {
		errno = EINVAL;
		return EOF;
	}
	handle = *link;
	result = fflush(stream);
	if (handle->vfile != NULL && file_close(handle->vfile) != 0)
		result = EOF;
	*link = handle->next;
	memset(handle, 0, sizeof(*handle));
	free(handle);
	return result;
}

int boots_stdio_close_all(void)
{
	int failed = 0;

	while (open_streams != NULL)
		if (fclose(&open_streams->stream) == EOF)
			failed = 1;
	return failed ? EOF : 0;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *stream)
{
	struct filesystem_stream *handle = filesystem_stream(stream);
	uint64_t available;
	size_t total, bytes;
	enum boots_fs_result result;

	if (size != 0 && count > (size_t)-1 / size) {
		errno = EINVAL;
		return 0;
	}
	total = size * count;
	if (total == 0)
		return 0;
	if (buffer == NULL || handle == NULL || !(stream->mode & STREAM_READ)) {
		if (stream != NULL)
			stream->error = 1;
		errno = EINVAL;
		return 0;
	}
	if (handle->vfile != NULL) {
		ssize_t got = file_read(handle->vfile, buffer, total);
		if (got < 0) { stream->error = 1; errno = (int)-got; return 0; }
		stream->position = (uint64_t)handle->vfile->f_offset;
		if ((size_t)got < total) stream->eof = 1;
		return (size_t)got / size;
	}
	available = stream->position < handle->file.size ?
		handle->file.size - stream->position : 0;
	bytes = available < total ? (size_t)available : total;
	if (bytes == 0) {
		stream->eof = 1;
		return 0;
	}
	result = boots_file_read_result(&handle->file, stream->position,
					 buffer, (uint32_t)bytes, NULL, NULL);
	if (result != BOOTS_FS_OK) {
		stream->error = 1;
		errno = result_errno(result);
		return 0;
	}
	stream->position += bytes;
	if (bytes < total)
		stream->eof = 1;
	return bytes / size;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream)
{
	struct filesystem_stream *handle;
	size_t total;
	enum boots_fs_result result;

	if (size != 0 && count > (size_t)-1 / size) {
		errno = EINVAL;
		return 0;
	}
	total = size * count;
	if (stream == stdout || stream == stderr)
		return boots_console_write_bytes(buffer, total) == total ? count : 0;
	if (total == 0)
		return 0;
	handle = filesystem_stream(stream);
	if (buffer == NULL || handle == NULL || !(stream->mode & STREAM_WRITE)) {
		if (stream != NULL)
			stream->error = 1;
		errno = EINVAL;
		return 0;
	}
	if (handle->vfile != NULL) {
		ssize_t put = file_write(handle->vfile, buffer, total);
		if (put < 0) { stream->error = 1; errno = (int)-put; return 0; }
		stream->position = (uint64_t)handle->vfile->f_offset;
		return (size_t)put / size;
	}
	result = boots_file_write_result(&handle->file, stream->position,
					  buffer, (uint32_t)total);
	if (result != BOOTS_FS_OK) {
		stream->error = 1;
		errno = result_errno(result);
		return 0;
	}
	stream->position += total;
	return count;
}

int getc(FILE *stream)
{
	unsigned char byte;

	return fread(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

int fseek(FILE *stream, long offset, int whence)
{
	struct filesystem_stream *handle = filesystem_stream(stream);
	uint64_t base, position;

	if (handle == NULL || (whence != SEEK_SET && whence != SEEK_CUR &&
				 whence != SEEK_END)) {
		errno = EINVAL;
		return -1;
	}
	if (handle->vfile != NULL) {
		off_t position = file_seek(handle->vfile, offset, whence);
		if (position < 0) { errno = (int)-position; return -1; }
		stream->position = (uint64_t)position; stream->eof = 0; return 0;
	}
	base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? stream->position :
		handle->file.size;
	if (offset < 0) {
		uint64_t distance = (uint64_t)(-(offset + 1L)) + 1U;

		if (distance > base) {
			errno = EINVAL;
			return -1;
		}
		position = base - distance;
	} else {
		if ((uint64_t)offset > UINT64_MAX - base) {
			errno = EOVERFLOW;
			return -1;
		}
		position = base + (uint64_t)offset;
	}
	stream->position = position;
	stream->eof = 0;
	return 0;
}

long ftell(FILE *stream)
{
	if (filesystem_stream(stream) == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (stream->position > (uint64_t)LONG_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	return (long)stream->position;
}

char *fgets(char *buffer, int size, FILE *stream)
{
	int index = 0;

	if (buffer == NULL || size <= 0) {
		errno = EINVAL;
		return NULL;
	}
	while (index + 1 < size) {
		int character = getc(stream);

		if (character == EOF)
			break;
		buffer[index++] = (char)character;
		if (character == '\n')
			break;
	}
	if (index == 0)
		return NULL;
	buffer[index] = '\0';
	return buffer;
}

int access(const char *path, int mode)
{
	struct boots_dirent entry;
	char absolute[BOOTS_PATH_MAX];
	const char *resolved_path = path;
	enum boots_fs_result result;

	if ((active_filesystem == NULL && active_namespace == NULL &&
	     active_context == NULL) ||
	    path == NULL || mode != F_OK) {
		errno = path == NULL || mode != F_OK ? EINVAL : ENOENT;
		return -1;
	}
	if (active_context != NULL) {
		struct inode *inode;
		int error = namei_at(active_context, path, &inode);
		if (error != 0) { errno = error; return -1; }
		inode_release(inode);
		return 0;
	}
	if (active_namespace != NULL && path[0] != '/') {
		size_t cwd_length = strlen(current_directory);
		size_t path_length = strlen(path);

		if (cwd_length + 1U + path_length >= sizeof(absolute)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(absolute, current_directory, cwd_length);
		if (cwd_length == 0 || absolute[cwd_length - 1U] != '/')
			absolute[cwd_length++] = '/';
		memcpy(absolute + cwd_length, path, path_length + 1U);
		resolved_path = absolute;
	}
	result = active_namespace != NULL ?
		boots_namespace_stat_result(active_namespace, resolved_path,
					      &entry) :
		boots_fs_stat_result(active_filesystem, resolved_path, &entry);
	if (result != BOOTS_FS_OK) {
		errno = result_errno(result);
		return -1;
	}
	return 0;
}

char *getcwd(char *buffer, size_t size)
{
	const char *cwd = active_context != NULL ? fs_getcwd(active_context) :
		current_directory;
	size_t length = strlen(cwd) + 1U;

	if (buffer == NULL || size < length) {
		errno = buffer == NULL ? EINVAL : ERANGE;
		return NULL;
	}
	memcpy(buffer, cwd, length);
	return buffer;
}

int chdir(const char *path)
{
	struct boots_dirent entry;
	char absolute[BOOTS_PATH_MAX];
	const char *resolved = path;
	size_t length;

	if (path == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (active_context != NULL) {
		int error = fs_chdir(active_context, path);
		if (error != 0) { errno = error; return -1; }
		return 0;
	}
	if (path[0] == '\0' || (path[0] == '.' && path[1] == '\0'))
		return 0;
	if (active_namespace == NULL) {
		if (path[0] == '/' && path[1] == '\0')
			return 0;
		errno = ENOENT;
		return -1;
	}
	if (path[0] != '/') {
		size_t cwd_length = strlen(current_directory);
		size_t path_length = strlen(path);

		if (cwd_length + 1U + path_length >= sizeof(absolute)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(absolute, current_directory, cwd_length);
		if (cwd_length == 0 || absolute[cwd_length - 1U] != '/')
			absolute[cwd_length++] = '/';
		memcpy(absolute + cwd_length, path, path_length + 1U);
		resolved = absolute;
	}
	if (boots_namespace_stat_result(active_namespace, resolved, &entry) !=
		    BOOTS_FS_OK || !(entry.attributes & 0x10U)) {
		errno = ENOENT;
		return -1;
	}
	length = strlen(resolved);
	if (length >= sizeof(current_directory)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(current_directory, resolved, length + 1U);
	return 0;
}

char *getenv(const char *name)
{
	if (boots_env_get == NULL)
		return NULL;
	return (char *)boots_env_get(active_environment, name);
}
