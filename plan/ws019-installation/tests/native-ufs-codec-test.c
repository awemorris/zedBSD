/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned operation, fail_at, short_at;
static uint64_t medium;
static ssize_t native_pread(int, void *, size_t, off_t);
static ssize_t native_pwrite(int, const void *, size_t, off_t);
static int native_fsync(int);
#define pread native_pread
#define pwrite native_pwrite
#define fsync native_fsync
#include "userland/base/mkfs/ufs-format.c"
#undef pread
#undef pwrite
#undef fsync

static int event(void)
{
	operation++;
	if (operation == fail_at) { errno = EIO; return -1; }
	return 0;
}
static ssize_t native_pread(int fd, void *data, size_t bytes, off_t offset)
{
	assert(offset >= 0 && (uint64_t)offset + bytes <= medium);
	if (event() < 0) return -1;
	return pread(fd, data, bytes - (operation == short_at), offset);
}
static ssize_t native_pwrite(int fd, const void *data, size_t bytes, off_t offset)
{
	assert(offset >= 0 && (uint64_t)offset + bytes <= medium);
	if (event() < 0) return -1;
	return pwrite(fd, data, bytes - (operation == short_at), offset);
}
static int native_fsync(int fd)
{
	if (event() < 0) return -1;
	return fsync(fd);
}
static void reset(void)
{
	operation = fail_at = short_at = 0;
}

int main(int argc, char **argv)
{
	struct format_context context;
	struct ufs_super super;
	struct ufs_format_capacity capacity;
	unsigned char block[8192], marker[512], actual[512];
	uint64_t bytes, tail_rounded;
	unsigned n, writes, reads;
	int fd;

	assert(argc == 3);
	bytes = strtoull(argv[2], NULL, 10);
	medium = bytes;
	assert(ufs_format_native_validate_size(bytes) == 0);
	assert(ufs_format_native_validate_size(UINT64_MAX) != 0);
	assert(ufs_format_native_validate_size(4194303) != 0);
	assert(ufs_format_native_validate_size(UINT64_C(1) << 60) == EOVERFLOW);
	/* Exercise actual header/counter generation above 32-bit fragment range. */
	assert(initialize_context(&context, -1, UINT64_C(5) << 40, 0, FORMAT_NATIVE) == 0);
	assert(context.fragments > UINT32_MAX);
	make_super(&context, block);
	assert(ufs_super_decode(block, sizeof(block), (UINT64_C(5) << 40) / 512, &super) == 0);
	assert(super.size == context.fragments && super.cstotal_ndir == 1);
	assert(ufs_format_native_capacity(UINT64_C(5) << 40, &capacity) == 0);
	assert(capacity.free_bytes == super.cstotal_nbfree * 8192);
	assert(capacity.free_inodes == super.cstotal_nifree && capacity.allocation_size == 8192);
	assert(operation == 0);
	assert(ufs_format_native_capacity(bytes, NULL) == EINVAL);
	assert(ufs_format_native_capacity(0, &capacity) == EINVAL);
	assert(capacity.free_bytes == 0 && capacity.free_inodes == 0);
	/* Every near-empty final-group boundary must retain metadata room. */
	tail_rounded = ((uint64_t)FORMAT_TAIL_BYTES + 1023) / 1024 * 1024;
	for (n = 0; n <= 160; n++) {
		assert(initialize_context(&context, -1,
		    ((uint64_t)FORMAT_MAX_FPG * 2 + n) * 1024 + tail_rounded,
		    0, FORMAT_NATIVE) == 0);
		assert(context.fragments - (uint64_t)(context.ncg - 1) * context.fpg > 144);
	}
	fd = open(argv[1], O_CREAT | O_EXCL | O_RDWR, 0600);
	assert(fd >= 0 && ftruncate(fd, (off_t)bytes) == 0);
	memset(marker, 0x65, sizeof(marker));
	assert(pwrite(fd, marker, 512, 152 * 1024) == 512);
	reset(); assert(ufs_format_native_write(fd, bytes) == 0); writes = operation;
	reset(); assert(ufs_format_native_verify(fd, bytes) == 0); reads = operation;
	assert(pread(fd, block, sizeof(block), 65536) == sizeof(block));
	assert(ufs_super_decode(block, sizeof(block), bytes / 512, &super) == 0);
	assert(ufs_format_native_capacity(bytes, &capacity) == 0);
	assert(capacity.free_bytes == super.cstotal_nbfree * 8192);
	assert(capacity.free_inodes == super.cstotal_nifree);
	if (bytes == 4194304) {
		for (n = 1; n <= writes; n++) {
			reset(); fail_at = n;
			assert(ufs_format_native_write(fd, bytes) == EIO && operation == n);
		}
		reset(); short_at = 1;
		assert(ufs_format_native_write(fd, bytes) == EIO);
		reset(); assert(ufs_format_native_write(fd, bytes) == 0);
		for (n = 1; n <= reads; n++) {
			reset(); fail_at = n;
			assert(ufs_format_native_verify(fd, bytes) == EIO && operation == n);
		}
		reset(); short_at = 1;
		assert(ufs_format_native_verify(fd, bytes) == EIO);
	}
	reset(); assert(ufs_format_native_verify(fd, bytes) == 0);
	assert(pread(fd, actual, 512, 152 * 1024) == 512 && !memcmp(actual, marker, 512));
	/* Corrupt root directory, allocation map, inode and persistence locator. */
	assert(initialize_context(&context, fd, bytes, 0, FORMAT_NATIVE) == 0);
	for (n = 0; n < 4; n++) {
		uint64_t offset;
		offset = n == 0 ? 144 * 1024 : n == 1 ? 72 * 1024 :
		    n == 2 ? 80 * 1024 + 512 : context.fragments * 1024;
		assert(pread(fd, actual, 1, (off_t)offset) == 1);
		actual[0] ^= 0x20;
		assert(pwrite(fd, actual, 1, (off_t)offset) == 1);
		assert(ufs_format_native_verify(fd, bytes) != 0);
		actual[0] ^= 0x20;
		assert(pwrite(fd, actual, 1, (off_t)offset) == 1);
	}
	assert(close(fd) == 0);
	printf("native UFS %llu bytes, wide geometry, bounds, corruption and %u/%u operation paths PASS\n",
	    (unsigned long long)bytes, writes, reads);
	return 0;
}
