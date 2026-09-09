/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Real sparse files, independent builder images, and exact interval faults. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "userland/base/mkswap/swap-format.h"

static unsigned checks;
static int read_only, fault_kind;
static uint64_t fault_offset = UINT64_MAX;
ssize_t pristine_pread(int, void *, size_t, off_t);
ssize_t pristine_pwrite(int, const void *, size_t, off_t);
static void require(int condition, const char *why);
static void corrupt(int fd, uint64_t bytes, uint64_t offset, int (*verify)(int, uint64_t));
static void image_cases(int fd, uint64_t bytes, int feature);
static void intervals(void);

/* Include the complete implementation to test its private interval arithmetic. */
#define pread pristine_pread
#define pwrite pristine_pwrite
#include "userland/base/mkfs/ufs-format.c"
#undef pread
#undef pwrite

static void require(int condition, const char *why)
{
	checks++;
	if (!condition) {
		fprintf(stderr, "pristine content failure: %s (%u)\n", why, checks);
		exit(1);
	}
}

ssize_t pristine_pwrite(int fd, const void *buffer, size_t bytes, off_t offset)
{
	require(!read_only, "verification never writes");
	return pwrite(fd, buffer, bytes, offset);
}

ssize_t pristine_pread(int fd, void *buffer, size_t bytes, off_t offset)
{
	if ((uint64_t)offset == fault_offset) {
		fault_offset = UINT64_MAX;
		if (fault_kind == 1) {
			errno = EACCES;
			return -1;
		}
		if (fault_kind == 2)
			return pread(fd, buffer, bytes - 1U, offset);
		errno = EINTR;
		return -1;
	}
	return pread(fd, buffer, bytes, offset);
}

static void corrupt(int fd, uint64_t bytes, uint64_t offset, int (*verify)(int, uint64_t))
{
	uint8_t original, changed;
	int error;
	require(pread(fd, &original, 1, (off_t)offset) == 1, "read mutation byte");
	changed = original ^ 0x5a;
	require(pwrite(fd, &changed, 1, (off_t)offset) == 1, "write independent corruption");
	read_only = 1;
	error = verify(fd, bytes);
	require(error != 0, "reject corrupted initial image");
	read_only = 0;
	require(pwrite(fd, &original, 1, (off_t)offset) == 1, "restore mutation byte");
}

static void image_cases(int fd, uint64_t bytes, int feature)
{
	static const uint64_t offsets[] = { 0, 65535, 65536, 73728, 81920,
		147456, 148475, 450559, 450560, 451071, 451072, 1048577 };
	int (*verify)(int, uint64_t);
	size_t index;
	int error;
	read_only = 0;
	require(ftruncate(fd, 0) == 0 && ftruncate(fd, (off_t)bytes) == 0, "fresh zero backing");
	verify = ufs_format_pristine;
	if (feature) {
		error = ufs_format_feature_write(fd, bytes);
		verify = ufs_format_feature_pristine;
	} else
		error = ufs_format_write(fd, bytes);
	require(error == 0, "generate supported geometry");
	read_only = 1;
	error = verify(fd, bytes);
	require(error == 0, "full geometry and partial tail pass");
	read_only = 0;
	for (index = 0; index < sizeof(offsets) / sizeof(offsets[0]); index++)
		corrupt(fd, bytes, offsets[index], verify);
	corrupt(fd, bytes, bytes - 1U, verify);
	corrupt(fd, bytes, bytes - 1024U, verify);
}

static void intervals(void)
{
	struct format_context context;
	struct format_extent extents[4];
	int error;
	memset(&context, 0, sizeof(context));
	context.medium_bytes = 100;
	context.extents = extents;
	context.extent_capacity = 4;
	error = record_extent(&context, 11, 3);
	require(error == 0, "partial-byte extent");
	error = record_extent(&context, 3, 2);
	require(error == 0, "out of order extent");
	error = record_extent(&context, 5, 6);
	require(error == 0 && context.extent_count == 1, "bridge exact neighbors");
	require(extents[0].start == 3 && extents[0].end == 14, "precise merged interval");
	error = record_extent(&context, 2, 1);
	require(error == 0 && extents[0].start == 2, "merge successor");
	error = record_extent(&context, 14, 1);
	require(error == 0 && extents[0].end == 15, "merge predecessor");
	require(record_extent(&context, 3, 1) == EINVAL, "reject contained overlap");
	require(record_extent(&context, 1, 2) == EINVAL, "reject right overlap");
	require(record_extent(&context, 14, 3) == EINVAL, "reject left overlap");
	require(record_extent(&context, UINT64_MAX, 2) == EINVAL, "reject wrapped offset");
	require(record_extent(&context, 99, 2) == EINVAL, "reject medium overrun");
	require(record_extent(&context, 99, 0) == EINVAL, "reject empty extent");
	require(record_extent(&context, 99, 1) == 0, "accept exact final byte");
	context.extent_capacity = context.extent_count;
	require(record_extent(&context, 20, 1) == EOVERFLOW, "enforce metadata bound");
}

int main(int argc, char **argv)
{
	static const uint64_t sizes[] = { UFS_FORMAT_MIN_BYTES,
		33555456, 268435456, UFS_FORMAT_MAX_BYTES };
	struct stat status;
	int fd, error, kind, profile;
	size_t index;
	uint64_t bytes;
	require(argc == 5, "independent UFS, swap, feature and scratch paths");
	intervals();
	for (index = 1; index <= 3; index++) {
		fd = open(argv[index], O_RDONLY);
		require(fd >= 0 && fstat(fd, &status) == 0, "open independent canonical image");
		read_only = 1;
		if (index == 1)
			error = ufs_format_pristine(fd, (uint64_t)status.st_size);
		else if (index == 2)
			error = swap_format_pristine(fd, (uint64_t)status.st_size);
		else
			error = ufs_format_feature_pristine(fd, (uint64_t)status.st_size);
		require(error == 0, "independent builder exact bytes accepted");
		require(close(fd) == 0, "close reference");
	}
	fd = open(argv[4], O_RDWR | O_CREAT | O_EXCL, 0600);
	require(fd >= 0, "create disposable sparse file");
	for (profile = 0; profile <= 1; profile++) {
		for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++)
			image_cases(fd, sizes[index], profile);
	}
	image_cases(fd, UFS_FORMAT_MIN_BYTES, 0);
	read_only = 1;
	for (kind = 1; kind <= 3; kind++) {
		fault_kind = kind;
		fault_offset = UFS_SBLOCK_OFFSET;
		error = ufs_format_pristine(fd, UFS_FORMAT_MIN_BYTES);
		require(error == (kind == 1 ? EACCES : kind == 2 ? EIO : 0), "metadata read failure semantics");
		fault_offset = FORMAT_CG0_USED * FORMAT_FRAGMENT;
		error = ufs_format_pristine(fd, UFS_FORMAT_MIN_BYTES);
		require(error == (kind == 1 ? EACCES : kind == 2 ? EIO : 0), "unused gap read failure semantics");
	}
	read_only = 0;
	bytes = 8192;
	require(ftruncate(fd, 0) == 0 && ftruncate(fd, (off_t)bytes) == 0, "fresh swap backing");
	require(swap_format_write(fd, bytes) == 0, "write minimal swap");
	read_only = 1;
	require(swap_format_pristine(fd, bytes) == 0, "minimal swap pristine");
	for (kind = 1; kind <= 3; kind++) {
		fault_kind = kind;
		fault_offset = 4096;
		error = swap_format_pristine(fd, bytes);
		require(error == (kind == 1 ? EACCES : kind == 2 ? EIO : 0), "slot read failure semantics");
	}
	read_only = 0;
	corrupt(fd, bytes, 60, swap_format_pristine);
	corrupt(fd, bytes, 4095, swap_format_pristine);
	corrupt(fd, bytes, 4096, swap_format_pristine);
	corrupt(fd, bytes, bytes - 1U, swap_format_pristine);
	require(ftruncate(fd, 4096) == 0, "truncate swap before read");
	read_only = 1;
	require(swap_format_pristine(fd, bytes) == EIO, "reject missing slot");
	require(close(fd) == 0, "close scratch");
	printf("Pristine content: %u checks PASS\n", checks);
	return 0;
}
