/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Linked into the build of sftp-server that interop-host.sh feeds fully
 * random packets: every file system call sftp-server-main.c makes is
 * answered here without a system call, so a random path (an absolute
 * one, "..", anything) can never open, remove, rename, chmod, chown or
 * truncate a real file.  The answers vary with a counter, so success and
 * failure paths of the server both run: "files" are fake descriptors from
 * STUB_FD_BASE on, a directory is a fake stream with three entries, a
 * path is "/stub".  The script checks that sftp-server-main.c calls no
 * file system function this file does not answer (it fails closed).
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The first fake descriptor; the server never gets a real one from here. */
#define STUB_FD_BASE	1000

/* A counter that decides the next answer: even succeeds, odd fails. */
static unsigned stub_counter;

/* The fake directory stream and its entries. */
static char stub_directory_object;
static struct dirent stub_entry;
static unsigned stub_entries_left;

static int
stub_next(int error)
{
	stub_counter++;
	if ((stub_counter & 1U) != 0) {
		errno = error;
		return -1;
	}

	return 0;
}

static void
stub_fill_stat(struct stat *status, int directory)
{
	memset(status, 0, sizeof(*status));
	status->st_mode = (mode_t)(directory ? (S_IFDIR | 0755) : (S_IFREG | 0644));
	status->st_size = (off_t)(stub_counter * 37U);
	status->st_nlink = 1;
	status->st_uid = 1000;
	status->st_gid = 1000;
	status->st_mtime = (time_t)1000000000;
}

int
open(const char *path, int flags, ...)
{
	(void)path;
	(void)flags;
	if (stub_next(ENOENT) != 0)
		return -1;
	return STUB_FD_BASE + (int)(stub_counter % 64U);
}

int
close(int descriptor)
{
	if (descriptor < STUB_FD_BASE) {
		errno = EBADF;
		return -1;
	}

	return 0;
}

int
fstat(int descriptor, struct stat *status)
{
	if (descriptor < STUB_FD_BASE) {
		errno = EBADF;
		return -1;
	}

	stub_fill_stat(status, (stub_counter % 7U) == 0);
	return 0;
}

int
stat(const char *path, struct stat *status)
{
	(void)path;
	if (stub_next(ENOENT) != 0)
		return -1;
	stub_fill_stat(status, (stub_counter % 3U) == 0);
	return 0;
}

int
lstat(const char *path, struct stat *status)
{
	(void)path;
	if (stub_next(EACCES) != 0)
		return -1;
	stub_fill_stat(status, (stub_counter % 3U) == 0);
	return 0;
}

ssize_t
pread(int descriptor, void *data, size_t size, off_t offset)
{
	size_t length;

	(void)offset;
	if (descriptor < STUB_FD_BASE) {
		errno = EBADF;
		return -1;
	}

	if (stub_next(EIO) != 0)
		return -1;
	length = size;
	if (length > (stub_counter % 5000U))
		length = stub_counter % 5000U;
	memset(data, 'x', length);
	return (ssize_t)length;
}

ssize_t
pwrite(int descriptor, const void *data, size_t size, off_t offset)
{
	(void)data;
	(void)offset;
	if (descriptor < STUB_FD_BASE) {
		errno = EBADF;
		return -1;
	}

	if (stub_next(ENOSPC) != 0)
		return -1;
	return (ssize_t)size;
}

int
ftruncate(int descriptor, off_t size)
{
	(void)descriptor;
	(void)size;
	return stub_next(EINVAL);
}

int
truncate(const char *path, off_t size)
{
	(void)path;
	(void)size;
	return stub_next(EACCES);
}

int
fchown(int descriptor, uid_t user, gid_t group)
{
	(void)descriptor;
	(void)user;
	(void)group;
	return stub_next(EPERM);
}

int
chown(const char *path, uid_t user, gid_t group)
{
	(void)path;
	(void)user;
	(void)group;
	return stub_next(EPERM);
}

int
fchmod(int descriptor, mode_t mode)
{
	(void)descriptor;
	(void)mode;
	return stub_next(EPERM);
}

int
chmod(const char *path, mode_t mode)
{
	(void)path;
	(void)mode;
	return stub_next(ENOENT);
}

int
futimens(int descriptor, const struct timespec times[2])
{
	(void)descriptor;
	(void)times;
	return stub_next(EPERM);
}

int
utimensat(int directory, const char *path, const struct timespec times[2], int flags)
{
	(void)directory;
	(void)path;
	(void)times;
	(void)flags;
	return stub_next(ENOENT);
}

DIR *
opendir(const char *path)
{
	(void)path;
	if (stub_next(ENOENT) != 0)
		return NULL;
	stub_entries_left = 3;
	return (DIR *)(void *)&stub_directory_object;
}

struct dirent *
readdir(DIR *directory)
{
	if (directory != (DIR *)(void *)&stub_directory_object || stub_entries_left == 0)
		return NULL;
	stub_entries_left--;
	memset(&stub_entry, 0, sizeof(stub_entry));
	strcpy(stub_entry.d_name, stub_entries_left == 2 ? "." : (stub_entries_left == 1 ? ".." : "entry"));
	return &stub_entry;
}

int
closedir(DIR *directory)
{
	if (directory != (DIR *)(void *)&stub_directory_object) {
		errno = EBADF;
		return -1;
	}

	return 0;
}

int
unlink(const char *path)
{
	(void)path;
	return stub_next(ENOENT);
}

int
mkdir(const char *path, mode_t mode)
{
	(void)path;
	(void)mode;
	return stub_next(EEXIST);
}

int
rmdir(const char *path)
{
	(void)path;
	return stub_next(ENOTEMPTY);
}

int
symlink(const char *target, const char *path)
{
	(void)target;
	(void)path;
	return stub_next(EEXIST);
}

int
link(const char *path, const char *target)
{
	(void)path;
	(void)target;
	return stub_next((stub_counter & 2U) != 0 ? EEXIST : EPERM);
}

int
rename(const char *path, const char *target)
{
	(void)path;
	(void)target;
	return stub_next(ENOENT);
}

char *
realpath(const char *path, char *resolved)
{
	(void)path;
	(void)resolved;
	if (stub_next(ENOENT) != 0)
		return NULL;
	return strdup("/stub");
}

ssize_t
readlink(const char *path, char *target, size_t size)
{
	(void)path;
	if (stub_next(EINVAL) != 0)
		return -1;
	if (size < 4)
		return 0;
	memcpy(target, "stub", 4);
	return 4;
}

struct passwd *
getpwuid(uid_t user)
{
	(void)user;
	return NULL;
}

struct group *
getgrgid(gid_t group)
{
	(void)group;
	return NULL;
}
