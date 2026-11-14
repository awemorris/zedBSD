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

static int stub_next(int error);
static void stub_fill_stat(struct stat *status, int directory);

/* Counts a call and fails every other one with the given error. */
static int
stub_next(int error)
{
	/* Counts this call. */
	stub_counter++;

	/* Fails the odd calls with the error and reports it in errno. */
	if ((stub_counter & 1U) != 0) {
		errno = error;
		return -1;
	}

	/* Succeeded: this call passes. */
	return 0;
}

/* Fills in a plausible status of a directory or of a regular file. */
static void
stub_fill_stat(struct stat *status, int directory)
{
	mode_t mode;

	/* Picks the file type and its permissions. */
	mode = S_IFREG | 0644;
	if (directory != 0)
		mode = S_IFDIR | 0755;

	/* Fills in the status, with a size that varies with the call count. */
	memset(status, 0, sizeof(*status));
	status->st_mode = mode;
	status->st_size = (off_t)(stub_counter * 37U);
	status->st_nlink = 1;
	status->st_uid = 1000;
	status->st_gid = 1000;
	status->st_mtime = (time_t)1000000000;
}

int
open(const char *path, int flags, ...)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;
	(void)flags;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(ENOENT);
	if (failed != 0)
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

	/* Succeeded: every descriptor of the stub closes. */
	return 0;
}

int
fstat(int descriptor, struct stat *status)
{
	if (descriptor < STUB_FD_BASE) {
		errno = EBADF;
		return -1;
	}

	/* Describes the descriptor as a directory on every seventh call, as a file otherwise. */
	stub_fill_stat(status, (stub_counter % 7U) == 0);

	/* Succeeded: the status is filled in. */
	return 0;
}

int
stat(const char *path, struct stat *status)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(ENOENT);
	if (failed != 0)
		return -1;
	stub_fill_stat(status, (stub_counter % 3U) == 0);
	return 0;
}

int
lstat(const char *path, struct stat *status)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(EACCES);
	if (failed != 0)
		return -1;
	stub_fill_stat(status, (stub_counter % 3U) == 0);
	return 0;
}

ssize_t
pread(int descriptor, void *data, size_t size, off_t offset)
{
	size_t length;
	int failed;

	/* The arguments the stub does not look at. */
	(void)offset;

	/* Refuses a descriptor that the stub did not hand out. */
	if (descriptor < STUB_FD_BASE) {
		errno = EBADF;
		return -1;
	}

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(EIO);
	if (failed != 0)
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
	int failed;

	/* The arguments the stub does not look at. */
	(void)data;
	(void)offset;

	/* Refuses a descriptor that the stub did not hand out. */
	if (descriptor < STUB_FD_BASE) {
		errno = EBADF;
		return -1;
	}

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(ENOSPC);
	if (failed != 0)
		return -1;
	return (ssize_t)size;
}

int
ftruncate(int descriptor, off_t size)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)descriptor;
	(void)size;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(EINVAL);

	/* Reports the stub's answer. */
	return failed;
}

int
truncate(const char *path, off_t size)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;
	(void)size;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(EACCES);

	/* Reports the stub's answer. */
	return failed;
}

int
fchown(int descriptor, uid_t user, gid_t group)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)descriptor;
	(void)user;
	(void)group;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(EPERM);

	/* Reports the stub's answer. */
	return failed;
}

int
chown(const char *path, uid_t user, gid_t group)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;
	(void)user;
	(void)group;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(EPERM);

	/* Reports the stub's answer. */
	return failed;
}

int
fchmod(int descriptor, mode_t mode)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)descriptor;
	(void)mode;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(EPERM);

	/* Reports the stub's answer. */
	return failed;
}

int
chmod(const char *path, mode_t mode)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;
	(void)mode;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(ENOENT);

	/* Reports the stub's answer. */
	return failed;
}

int
futimens(int descriptor, const struct timespec times[2])
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)descriptor;
	(void)times;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(EPERM);

	/* Reports the stub's answer. */
	return failed;
}

int
utimensat(int directory, const char *path, const struct timespec times[2], int flags)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)directory;
	(void)path;
	(void)times;
	(void)flags;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(ENOENT);

	/* Reports the stub's answer. */
	return failed;
}

DIR *
opendir(const char *path)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(ENOENT);
	if (failed != 0)
		return NULL;
	stub_entries_left = 3;
	return (DIR *)(void *)&stub_directory_object;
}

struct dirent *
readdir(DIR *directory)
{
	const char *name;

	/* Ends the listing for a foreign stream or when all three entries were given. */
	if (directory != (DIR *)(void *)&stub_directory_object || stub_entries_left == 0)
		return NULL;

	/* Picks the entry name: the listing is "." then ".." then one ordinary name. */
	stub_entries_left--;
	name = "entry";
	if (stub_entries_left == 2)
		name = ".";
	else if (stub_entries_left == 1)
		name = "..";

	/* Builds the entry. */
	memset(&stub_entry, 0, sizeof(stub_entry));
	strcpy(stub_entry.d_name, name);

	/* Hands out the one entry object the stub owns. */
	return &stub_entry;
}

int
closedir(DIR *directory)
{
	if (directory != (DIR *)(void *)&stub_directory_object) {
		errno = EBADF;
		return -1;
	}

	/* Succeeded: the stream closes. */
	return 0;
}

int
unlink(const char *path)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(ENOENT);

	/* Reports the stub's answer. */
	return failed;
}

int
mkdir(const char *path, mode_t mode)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;
	(void)mode;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(EEXIST);

	/* Reports the stub's answer. */
	return failed;
}

int
rmdir(const char *path)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(ENOTEMPTY);

	/* Reports the stub's answer. */
	return failed;
}

int
symlink(const char *target, const char *path)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)target;
	(void)path;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(EEXIST);

	/* Reports the stub's answer. */
	return failed;
}

int
link(const char *path, const char *target)
{
	int failed;
	int error;

	/* The arguments the stub does not look at. */
	(void)path;
	(void)target;

	/* The stub fails on every other call, with the error this call stands for. */
	error = EPERM;
	if ((stub_counter & 2U) != 0)
		error = EEXIST;
	failed = stub_next(error);

	/* Reports the stub's answer. */
	return failed;
}

int
rename(const char *path, const char *target)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;
	(void)target;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(ENOENT);

	/* Reports the stub's answer. */
	return failed;
}

char *
realpath(const char *path, char *resolved)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;
	(void)resolved;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(ENOENT);
	if (failed != 0)
		return NULL;
	return strdup("/stub");
}

ssize_t
readlink(const char *path, char *target, size_t size)
{
	int failed;

	/* The arguments the stub does not look at. */
	(void)path;

	/* The stub fails on every other call, with the error this call stands for. */
	failed = stub_next(EINVAL);
	if (failed != 0)
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
