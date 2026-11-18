/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * posix_fallocate() without a kernel operation for it, and posix_fadvise().
 *
 * The file is extended to cover the range, and one zero byte is written
 * into each block the extension added, so that the blocks are allocated
 * now and a later write into the range does not fail for want of space.
 * Blocks the file already had are left untouched: they are allocated or
 * hold data already.
 *
 * posix_fadvise() checks its arguments and otherwise takes no action: the
 * kernel has no read-ahead or cache policy a program can steer, and advice is
 * allowed to be ignored.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

/* The largest off_t, whose width depends on the ABI. */
#define FALLOCATE_OFF_MAX \
	((off_t)(((uintmax_t)1 << (sizeof(off_t) * 8U - 1U)) - 1U))

/*
 * Makes sure the bytes [offset, offset + length) of fd have storage.
 * Returns 0 or an error number; errno is not set, as POSIX says.
 */
int
posix_fallocate(
	int fd,
	off_t offset,
	off_t length)
{
	struct stat status;
	off_t end;
	off_t block;
	off_t position;
	int saved;
	int result;

	/* Refuses a range POSIX calls invalid, or one that would wrap. */
	if (offset < 0 || length <= 0)
		return EINVAL;
	if (offset > FALLOCATE_OFF_MAX - length)
		return EFBIG;
	end = offset + length;

	/* Only a regular file can be given storage. */
	saved = errno;
	if (fstat(fd, &status) != 0) {
		result = errno;
		errno = saved;
		return result;
	}
	if (S_ISFIFO(status.st_mode) || S_ISSOCK(status.st_mode)) {
		errno = saved;
		return ESPIPE;
	}
	if (!S_ISREG(status.st_mode)) {
		errno = saved;
		return ENODEV;
	}

	/* A range inside the file already has what it needs. */
	if (end <= status.st_size) {
		errno = saved;
		return 0;
	}

	/* Extends the file, then touches each new block. */
	if (ftruncate(fd, end) != 0) {
		result = errno;
		errno = saved;
		return result;
	}
	block = status.st_blksize > 0 ? status.st_blksize : 4096;
	for (position = (status.st_size + block - 1) / block * block;
	     position < end; position += block) {
		if (pwrite(fd, "", 1, position) != 1) {
			result = errno;
			errno = saved;
			return result;
		}
	}

	/* The last byte of the range belongs to a block just added as well. */
	if (status.st_size < end && pwrite(fd, "", 1, end - 1) != 1) {
		result = errno;
		errno = saved;
		return result;
	}

	errno = saved;
	return 0;
}

/*
 * Takes advice about how the bytes [offset, offset + length) of fd will be read.
 * Returns 0 or an error number; errno is not set, as POSIX says.
 */
int
posix_fadvise(
	int fd,
	off_t offset,
	off_t length,
	int advice)
{
	struct stat status;
	int saved;
	int status_result;
	int error;

	/* Advice covers any offset, so the offset itself is never wrong. */
	(void)offset;

	/* Refuses advice that is not one of the six POSIX names. */
	if (advice < POSIX_FADV_NORMAL || advice > POSIX_FADV_NOREUSE)
		return EINVAL;

	/* A negative length names no range. */
	if (length < 0)
		return EINVAL;

	/* Finds out whether fd is open, keeping the caller's errno. */
	saved = errno;
	status_result = fstat(fd, &status);
	error = errno;
	errno = saved;
	if (status_result != 0)
		return error;

	/* A pipe, a FIFO or a socket has no file offset to give advice about. */
	if (S_ISFIFO(status.st_mode))
		return ESPIPE;
	if (S_ISSOCK(status.st_mode))
		return ESPIPE;

	/* Succeeded: the advice is accepted and has no effect. */
	return 0;
}
