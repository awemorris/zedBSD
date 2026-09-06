/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The target UFS1 generator's geometry, corruption, and I/O fault fixtures.
 */

#include "userland/base/mkfs/ufs1-format.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_BYTES (UINT64_C(32) * 1024U * 1024U)
#define NO_FAULT (-1L)
#define FAULT_ERROR 1
#define FAULT_SHORT 2
#define FAULT_INTERRUPT 3

static unsigned checks;
static long write_calls;
static long read_calls;
static uint64_t written_bytes;
static uint64_t read_bytes;
static long write_fault = NO_FAULT;
static long read_fault = NO_FAULT;
static int write_kind;
static int read_kind;

ssize_t ufs_test_pwrite(int fd, const void *buffer, size_t length, off_t offset);
ssize_t ufs_test_pread(int fd, void *buffer, size_t length, off_t offset);
static void require_check(int condition, const char *description);
static void reset_faults(void);
static void check_size_boundaries(int fd);
static void check_io_faults(int fd);
static void check_corruption(int fd);
static void check_prefilled(int fd, const char *reference_path);
static void build_image(int fd, uint64_t bytes);

/*
 * Exercises the formatter and leaves its default image for builder comparison.
 */
int
main(
	int argc,
	char **argv)
{
	int output;
	int scratch;
	int error;

	/* Require caller-owned disposable image paths. */
	require_check(argc == 4, "output, scratch, and reference image arguments");
	output = open(argv[1], O_CREAT | O_EXCL | O_RDWR, 0600);
	require_check(output >= 0, "create output fixture");
	scratch = open(argv[2], O_CREAT | O_EXCL | O_RDWR, 0600);
	require_check(scratch >= 0, "create scratch fixture");

	/* Check geometry limits, malformed transfers, and damaged metadata. */
	check_size_boundaries(scratch);
	check_io_faults(scratch);
	check_corruption(scratch);
	check_prefilled(scratch, argv[3]);

	/* Retain fresh zero backing for whole-file equality with the host builder. */
	error = ftruncate(output, (off_t)DEFAULT_BYTES);
	require_check(error == 0, "size default output");

	/* Produce and validate the maintained default data image. */
	reset_faults();
	build_image(output, DEFAULT_BYTES);
	error = close(output);
	require_check(error == 0, "close default output");

	/* Release the disposable scratch file. */
	error = close(scratch);
	require_check(error == 0, "close scratch fixture");
	error = unlink(argv[2]);
	require_check(error == 0, "remove scratch fixture");

	/* Report the completed focused checks. */
	printf("UFS1 formatter: %u checks PASS\n", checks);
	return 0;
}

/*
 * Injects one controlled write result around the production transfer path.
 */
ssize_t
ufs_test_pwrite(
	int fd,
	const void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t count;
	long call;

	/* Apply the selected fault exactly once at its scheduled call. */
	call = write_calls++;
	if (call == write_fault) {
		/* Return a positive partial count without completing the record. */
		if (write_kind == FAULT_SHORT)
			return (ssize_t)(length - 1U);

		/* Distinguish a retryable interruption from a permanent failure. */
		errno = write_kind == FAULT_INTERRUPT ? EINTR : ENOSPC;
		return -1;
	}

	/* Forward ordinary writes to the host's disposable file. */
	written_bytes += length;
	count = pwrite(fd, buffer, length, offset);
	return count;
}

/*
 * Injects one controlled read result around the production verification path.
 */
ssize_t
ufs_test_pread(
	int fd,
	void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t count;
	long call;

	/* Apply the selected fault exactly once at its scheduled call. */
	call = read_calls++;
	if (call == read_fault) {
		/* Report a truncated metadata record. */
		if (read_kind == FAULT_SHORT)
			return (ssize_t)(length - 1U);

		/* Distinguish retryable interruption from permanent media failure. */
		errno = read_kind == FAULT_INTERRUPT ? EINTR : EIO;
		return -1;
	}

	/* Forward ordinary reads to the host's disposable file. */
	read_bytes += length;
	count = pread(fd, buffer, length, offset);
	return count;
}

/* Stops the fixture at the first failed behavioral assertion. */
static void
require_check(
	int condition,
	const char *description)
{
	/* Count every assertion and explain any failure. */
	checks++;
	if (!condition) {
		fprintf(stderr, "FAIL: %s (errno %d)\n", description, errno);
		exit(1);
	}
}

/* Restores default production I/O forwarding. */
static void
reset_faults(void)
{
	/* Clear both schedules and their observed call counts. */
	write_fault = NO_FAULT;
	read_fault = NO_FAULT;
	write_calls = 0;
	read_calls = 0;
	written_bytes = 0;
	read_bytes = 0;
	write_kind = 0;
	read_kind = 0;
}

/* Writes, flushes, and verifies a correctly sized disposable image. */
static void
build_image(
	int fd,
	uint64_t bytes)
{
	int error;
	struct stat status;

	/* Size the fixture before invoking the formatter's no-resize API. */
	error = ftruncate(fd, (off_t)bytes);
	require_check(error == 0, "size image fixture");
	error = ufs1_format_write(fd, bytes);
	require_check(error == 0, "write valid image");

	/* Flush before checking the production-decoded initialized tree. */
	error = fsync(fd);
	require_check(error == 0, "flush image fixture");
	error = ufs1_format_verify(fd, bytes);
	require_check(error == 0, "verify valid image");

	/* Confirm the writer preserves the caller-selected size. */
	error = fstat(fd, &status);
	require_check(error == 0, "stat image fixture");
	require_check((uint64_t)status.st_size == bytes, "preserve exact file size");
}

/* Checks both geometry boundaries and the final partial filesystem block. */
static void
check_size_boundaries(
	int fd)
{
	static const uint64_t rejected[] = {
		0U, 1024U, UFS1_FORMAT_MIN_BYTES - 1024U,
		UFS1_FORMAT_MIN_BYTES + 1U, UFS1_FORMAT_MAX_BYTES + 1024U,
		UINT64_MAX
	};
	size_t index;
	int error;

	/* Reject every unsupported size before issuing any file operation. */
	reset_faults();
	for (index = 0; index < sizeof(rejected) / sizeof(rejected[0]); index++) {
		error = ufs1_format_validate_size(rejected[index]);
		require_check(error == EINVAL, "reject unsupported geometry");
		error = ufs1_format_write(fd, rejected[index]);
		require_check(error == EINVAL, "reject unsupported write size");
		error = ufs1_format_verify(fd, rejected[index]);
		require_check(error == EINVAL, "reject unsupported verify size");
	}

	/* Confirm validation failures cannot mutate or inspect the descriptor. */
	require_check(write_calls == 0, "invalid sizes do not write");
	require_check(read_calls == 0, "invalid sizes do not read");

	/* Exercise minimum, odd fragment count, and maximum bitmap capacity. */
	build_image(fd, UFS1_FORMAT_MIN_BYTES);
	build_image(fd, UFS1_FORMAT_MIN_BYTES + 1024U);
	build_image(fd, UFS1_FORMAT_MAX_BYTES);
}

/* Checks failures in zeroing, data construction, and final publication. */
static void
check_io_faults(
	int fd)
{
	long writes;
	long reads;
	long positions[4];
	unsigned index;
	uint32_t magic;
	ssize_t count;
	int kind;
	int error;

	/* Discover the default operation counts using the smallest valid image. */
	reset_faults();
	build_image(fd, UFS1_FORMAT_MIN_BYTES);
	writes = write_calls;
	reads = read_calls;
	positions[0] = 0;
	positions[1] = 1;
	positions[2] = writes / 2;
	positions[3] = writes - 1;

	/* Refuse permanent and partial writes at each publication stage. */
	for (kind = FAULT_ERROR; kind <= FAULT_SHORT; kind++) {
		/* Visit zero-fill, journal, and primary-superblock transfer points. */
		for (index = 0; index < 4U; index++) {
			reset_faults();
			write_fault = positions[index];
			write_kind = kind;
			error = ufs1_format_write(fd, UFS1_FORMAT_MIN_BYTES);
			require_check(error == (kind == FAULT_ERROR ? ENOSPC : EIO), "propagate incomplete write");
			require_check(write_calls == positions[index] + 1, "stop immediately after write fault");

			/* Keep the old primary invalid after a later construction failure. */
			if (positions[index] != 0) {
				count = pread(fd, &magic, sizeof(magic), 8192U + 1372U);
				require_check(count == (ssize_t)sizeof(magic), "read failed format primary magic");
				require_check(magic == 0, "withhold primary after incomplete reformat");
			}
		}
	}

	/* Retry an interrupted write exactly once and produce a valid image. */
	reset_faults();
	write_fault = positions[2];
	write_kind = FAULT_INTERRUPT;
	build_image(fd, UFS1_FORMAT_MIN_BYTES);
	require_check(write_calls == writes + 1, "retry interrupted write");
	positions[1] = 1;
	positions[2] = reads / 2;
	positions[3] = reads - 1;

	/* Refuse failed and short reads throughout metadata verification. */
	for (kind = FAULT_ERROR; kind <= FAULT_SHORT; kind++) {
		/* Visit the production probe and later deterministic record checks. */
		for (index = 0; index < 4U; index++) {
			reset_faults();
			read_fault = positions[index];
			read_kind = kind;
			error = ufs1_format_verify(fd, UFS1_FORMAT_MIN_BYTES);
			require_check(error == EIO, "reject incomplete verification read");
			require_check(read_calls == positions[index] + 1, "stop immediately after read fault");
		}
	}

	/* Retry a single interrupted read without weakening short-read checks. */
	reset_faults();
	read_fault = 0;
	read_kind = FAULT_INTERRUPT;
	error = ufs1_format_verify(fd, UFS1_FORMAT_MIN_BYTES);
	require_check(error == 0, "retry interrupted verification read");
	require_check(read_calls == reads + 1, "perform exactly one read retry");
	reset_faults();
}

/* Checks independent metadata equality while preserving unspecified free bytes. */
static void
check_prefilled(
	int fd,
	const char *reference_path)
{
	uint8_t actual[8192];
	uint8_t expected[8192];
	uint8_t stale[8192];
	uint64_t offset;
	uint64_t second_group;
	ssize_t count;
	int reference;
	int error;

	/* Fill every old metadata and free-data byte with a recognizable value. */
	error = ftruncate(fd, (off_t)DEFAULT_BYTES);
	require_check(error == 0, "size prefilled UFS fixture");
	memset(stale, 0x7b, sizeof(stale));
	for (offset = 0; offset < DEFAULT_BYTES; offset += sizeof(stale)) {
		count = pwrite(fd, stale, sizeof(stale), (off_t)offset);
		require_check(count == (ssize_t)sizeof(stale), "prefill old UFS contents");
	}

	/* Require bounded construction and verification on the default geometry. */
	reset_faults();
	build_image(fd, DEFAULT_BYTES);
	require_check(written_bytes <= 512U * 1024U, "format writes bounded metadata only");
	require_check(read_bytes <= 512U * 1024U, "verify reads bounded metadata only");

	/* Compare every allocated or reserved byte with the maintained backend. */
	reference = open(reference_path, O_RDONLY);
	require_check(reference >= 0, "open independent UFS reference");
	second_group = DEFAULT_BYTES / 2U;
	for (offset = 0; offset < DEFAULT_BYTES; offset += sizeof(actual)) {
		count = pread(fd, actual, sizeof(actual), (off_t)offset);
		require_check(count == (ssize_t)sizeof(actual), "read initialized UFS range");

		/* Select independently fixed metadata ranges from the builder layout. */
		if (offset < 352U * 1024U ||
		    (offset >= second_group && offset < second_group + 56U * 1024U)) {
			count = pread(reference, expected, sizeof(expected), (off_t)offset);
			require_check(count == (ssize_t)sizeof(expected), "read maintained UFS metadata");
			require_check(memcmp(actual, expected, sizeof(actual)) == 0, "replace all old UFS metadata and allocated content");
		} else {
			require_check(memcmp(actual, stale, sizeof(actual)) == 0, "leave all free UFS data unspecified");
		}
	}

	/* Release the independently generated reference image. */
	error = close(reference);
	require_check(error == 0, "close independent UFS reference");
}

/* Checks damage outside the superblock as well as production-probe rejection. */
static void
check_corruption(
	int fd)
{
	static const uint64_t offsets[] = {
		8192U + 1372U, 16U * 1024U + 200U,
		24U * 1024U + 2U * 128U, 56U * 1024U + 508U,
		184U * 1024U, 192U * 1024U, 336U * 1024U,
		(UINT64_C(2048) + 8U) * 1024U
	};
	size_t index;
	uint8_t original;
	uint8_t changed;
	ssize_t count;
	int error;

	/* Start from a complete minimum-size image. */
	reset_faults();
	build_image(fd, UFS1_FORMAT_MIN_BYTES);

	/* Corrupt and restore one field in each independent metadata structure. */
	for (index = 0; index < sizeof(offsets) / sizeof(offsets[0]); index++) {
		count = pread(fd, &original, 1U, (off_t)offsets[index]);
		require_check(count == 1, "read corruption fixture byte");
		changed = (uint8_t)(original ^ 0x80U);
		count = pwrite(fd, &changed, 1U, (off_t)offsets[index]);
		require_check(count == 1, "write corruption fixture byte");
		error = ufs1_format_verify(fd, UFS1_FORMAT_MIN_BYTES);
		require_check(error != 0, "reject corrupted metadata");
		count = pwrite(fd, &original, 1U, (off_t)offsets[index]);
		require_check(count == 1, "restore corruption fixture byte");
	}

	/* Confirm every corruption was restored completely. */
	error = ufs1_format_verify(fd, UFS1_FORMAT_MIN_BYTES);
	require_check(error == 0, "verify restored metadata");
}
