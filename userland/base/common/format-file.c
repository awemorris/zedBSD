/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Initializes an existing file while retaining exclusive mutation ownership.
 */

#include "format-file.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zedbsd/fcntl.h>

static int same_object(const struct stat *expected, const struct stat *actual);
static int close_files(int writer, int reader, int error);
static int verify_file(int writer, const char *path, const struct stat *expected, const struct format_file_ops *ops);
static int write_file(int writer, const char *path, const struct stat *expected, const struct format_file_ops *ops);

/*
 * Formats and verifies one pre-sized regular file without releasing its lease.
 */
int
format_file_run(
	const char *path,
	const struct format_file_ops *ops,
	uint64_t *size_out)
{
	struct stat expected;
	struct stat opened;
	int writer;
	int error;

	/* Rejects unsupported objects before opening devices or blocking FIFOs. */
	if (lstat(path, &expected) < 0)
		return errno;

	/* Requires an existing, unaliased regular file with positive length. */
	if (!S_ISREG(expected.st_mode) || expected.st_size <= 0)
		return EINVAL;

	/* Refuses multiple directory entries for the supplied object. */
	if (expected.st_nlink != 1)
		return EBUSY;

	/* Checks format geometry before acquiring any mutation authority. */
	error = ops->validate_size((uint64_t)expected.st_size);
	if (error != 0)
		return error;

	/* Avoids blocking if a special file replaces the checked regular object. */
	writer = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (writer < 0)
		return errno;

	/* Detects replacement between the pathname check and open. */
	if (fstat(writer, &opened) < 0) {
		error = close_files(writer, -1, errno);
		return error;
	}
	error = same_object(&expected, &opened);
	if (error != 0) {
		error = close_files(writer, -1, error);
		return error;
	}

	/* Retains the writer through generation, flush, reopen and validation. */
	error = write_file(writer, path, &expected, ops);
	error = close_files(writer, -1, error);
	if (error != 0)
		return error;

	/* Publishes the size only after all descriptors close successfully. */
	*size_out = (uint64_t)expected.st_size;
	return 0;
}

/*
 * Checks existing bytes without acquiring mutation or activation authority.
 *
 * Identity checks detect replacement but do not exclude concurrent writes.
 */
int
format_file_verify(
	const char *path,
	int (*validate_size)(uint64_t),
	int (*verify)(int, uint64_t),
	uint64_t *size_out)
{
	struct stat expected;
	struct stat actual;
	int reader;
	int error;

	/* Rejects special objects before opening a potentially blocking name. */
	error = lstat(path, &expected);
	if (error < 0)
		return errno;

	/* Requires the same regular-file geometry as the formatter. */
	if (!S_ISREG(expected.st_mode) || expected.st_size <= 0)
		return EINVAL;

	/* Refuses names with ambiguous installation ownership. */
	if (expected.st_nlink != 1)
		return EBUSY;

	/* Validates size before reading any content. */
	error = validate_size((uint64_t)expected.st_size);
	if (error != 0)
		return error;

	/* Opens only a reader, with no format reservation or synchronization. */
	reader = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (reader < 0)
		return errno;

	/* Checks descriptor identity before invoking the content verifier. */
	error = fstat(reader, &actual);
	if (error < 0)
		error = errno;
	else
		error = same_object(&expected, &actual);

	/* Compares all required bytes only on the caller-selected object. */
	if (error == 0)
		error = verify(reader, (uint64_t)expected.st_size);

	/* Detects descriptor geometry changes during the read. */
	if (error == 0) {
		error = fstat(reader, &actual);
		if (error < 0)
			error = errno;
		else
			error = same_object(&expected, &actual);
	}

	/* Detects pathname replacement while the checked descriptor is open. */
	if (error == 0) {
		error = lstat(path, &actual);
		if (error < 0)
			error = errno;
		else
			error = same_object(&expected, &actual);
	}

	/* Closes on every path without replacing an earlier verification error. */
	error = close_files(-1, reader, error);
	if (error != 0)
		return error;

	/* Publishes size only after a successful close. */
	*size_out = (uint64_t)expected.st_size;
	return 0;
}

/* Compares the immutable identity and caller-selected size. */
static int
same_object(
	const struct stat *expected,
	const struct stat *actual)
{
	/* Refuses a changed name, kind, hard-link count or size. */
	if (!S_ISREG(actual->st_mode) ||
	    expected->st_dev != actual->st_dev ||
	    expected->st_ino != actual->st_ino ||
	    expected->st_size != actual->st_size ||
	    actual->st_nlink != 1)
		return EBUSY;

	/* Reports the same caller-sized object. */
	return 0;
}

/* Closes descriptors while preserving the first operation failure. */
static int
close_files(
	int writer,
	int reader,
	int error)
{
	int close_error;

	/* Closes the verification reader before relinquishing write exclusion. */
	if (reader >= 0) {
		close_error = close(reader);
		if (close_error < 0 && error == 0)
			error = errno;
	}

	/* Releases the descriptor-owned reservation even on failure. */
	if (writer >= 0) {
		close_error = close(writer);
		if (close_error < 0 && error == 0)
			error = errno;
	}

	/* Reports the earliest failure, including final-close failure. */
	return error;
}

/* Reopens and decodes the result while the original reservation remains held. */
static int
verify_file(
	int writer,
	const char *path,
	const struct stat *expected,
	const struct format_file_ops *ops)
{
	struct stat actual;
	int reader;
	int error;

	/* Avoids blocking before rejecting a replacement FIFO or special file. */
	reader = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (reader < 0)
		return errno;

	/* Confirms the new descriptor still refers to the reserved object. */
	if (fstat(reader, &actual) < 0) {
		error = close_files(-1, reader, errno);
		return error;
	}
	error = same_object(expected, &actual);
	if (error != 0) {
		error = close_files(-1, reader, error);
		return error;
	}

	/* Runs the same format decoder used by the production kernel. */
	error = ops->verify(reader, (uint64_t)expected->st_size);
	error = close_files(-1, reader, error);
	if (error != 0)
		return error;

	/* Rechecks the reserved descriptor after all generation and reads. */
	if (fstat(writer, &actual) < 0)
		return errno;
	error = same_object(expected, &actual);
	if (error != 0)
		return error;

	/* Detects a changed parent path as well as final-component replacement. */
	if (lstat(path, &actual) < 0)
		return errno;
	error = same_object(expected, &actual);

	/* Returns the final identity result without claiming partial success. */
	return error;
}

/* Acquires the reservation before the first content write. */
static int
write_file(
	int writer,
	const char *path,
	const struct stat *expected,
	const struct format_file_ops *ops)
{
	struct zedbsd_file_format_reserve request;
	struct stat actual;
	int error;

	/* Requests fixed-size exclusive mutation and activation ownership. */
	memset(&request, 0, sizeof(request));
	request.version = ZEDBSD_FILE_FORMAT_VERSION;
	request.struct_size = sizeof(request);
	request.size_bytes = (uint64_t)expected->st_size;
	if (ioctl(writer, ZEDBSD_FILE_FORMAT_RESERVE, &request) < 0)
		return errno;

	/* Rechecks the name after reservation and before any content mutation. */
	if (lstat(path, &actual) < 0)
		return errno;
	error = same_object(expected, &actual);
	if (error != 0)
		return error;

	/* Writes the bounded deterministic format into the existing file. */
	error = ops->write(writer, (uint64_t)expected->st_size);
	if (error != 0)
		return error;

	/* Requires durable metadata before reopening and probing the result. */
	if (fsync(writer) < 0)
		return errno;
	error = verify_file(writer, path, expected, ops);

	/* Returns the verified result while the caller still owns the lease. */
	return error;
}
