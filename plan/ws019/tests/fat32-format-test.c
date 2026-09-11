/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/mkfs/fat32-format.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned calls, fail_at, short_at, interrupt_at;
static unsigned writes, flushes;
static uint64_t metadata_end, backup_offset;

ssize_t fat_test_pwrite(int fd, const void *buffer, size_t size, off_t offset)
{
	const unsigned char *bytes = buffer;
	assert(offset >= 0 && (uint64_t)offset + size <= metadata_end);
	if (size >= 512 && bytes[510] == 0x55 && bytes[511] == 0xaa) {
		if (offset == 0)
			assert(flushes >= 3);
		if ((uint64_t)offset == backup_offset)
			assert(flushes >= 2);
	}
	calls++;
	writes++;
	if (calls == fail_at || calls == interrupt_at) {
		errno = calls == interrupt_at ? EINTR : ENOSPC;
		return -1;
	}
	if (calls == short_at)
		return pwrite(fd, buffer, size - 1, offset);
	return pwrite(fd, buffer, size, offset);
}

ssize_t fat_test_pread(int fd, void *buffer, size_t size, off_t offset)
{
	calls++;
	if (calls == fail_at || calls == interrupt_at) {
		errno = calls == interrupt_at ? EINTR : EIO;
		return -1;
	}
	if (calls == short_at)
		return pread(fd, buffer, size - 1, offset);
	return pread(fd, buffer, size, offset);
}

int fat_test_fsync(int fd)
{
	calls++;
	flushes++;
	if (calls == fail_at) {
		errno = EIO;
		return -1;
	}
	return fsync(fd);
}

static void reset(void)
{
	calls = fail_at = short_at = interrupt_at = writes = flushes = 0;
}

int main(int argc, char **argv)
{
	struct fat32_format_geometry g, invalid;
	uint32_t sector_size;
	uint64_t bytes, offset;
	unsigned write_calls, read_calls, n;
	unsigned char sentinel[512], actual[512], dirty[32768];
	int fd;

	assert(argc == 3);
	sector_size = (uint32_t)strtoul(argv[2], NULL, 10);
	bytes = (uint64_t)sector_size * 131072;
	assert(fat32_format_geometry(131072, sector_size, 2048, 0x12345678, &g) == 0);
	assert(g.clusters >= 65525);
	metadata_end = (uint64_t)(g.data_sector + g.sectors_per_cluster) * sector_size;
	backup_offset = (uint64_t)6 * sector_size;
	assert((uint64_t)g.fat_sectors * sector_size / 4 >= g.clusters + 2);
	assert(fat32_format_geometry(65525, sector_size, 0, 1, &invalid) == EINVAL);
	assert(fat32_format_geometry(UINT64_MAX, sector_size, 0, 1, &invalid) == EINVAL);
	assert(fat32_format_geometry(131072, 513, 0, 1, &invalid) == EINVAL);
	assert(fat32_format_geometry(131072, sector_size, UINT64_MAX, 1, &invalid) == EOVERFLOW);
	if (sector_size == 4096) {
		assert(fat32_format_geometry(UINT32_MAX, sector_size, 0, 1, &invalid) == EINVAL);
		assert(fat32_format_geometry(UINT32_C(2000000000), sector_size, 0, 1, &invalid) == 0);
	} else {
		assert(fat32_format_geometry(UINT32_MAX, sector_size, 0, 1, &invalid) == 0);
	}
	assert(invalid.clusters <= 0x0fffffee);
	assert(invalid.sectors_per_cluster * sector_size <= 32768);
	assert(fat32_format_geometry(131072, sector_size, 0, 1, NULL) == EINVAL);
	fd = open(argv[1], O_RDWR | O_CREAT | O_EXCL, 0600);
	assert(fd >= 0 && ftruncate(fd, (off_t)bytes) == 0);
	memset(sentinel, 0x6d, sizeof(sentinel));
	memset(dirty, 0xa5, sizeof(dirty));
	offset = (uint64_t)(g.data_sector + g.sectors_per_cluster) * sector_size;
	assert(pwrite(fd, sentinel, sizeof(sentinel), (off_t)offset) == sizeof(sentinel));
	assert(pwrite(fd, sentinel, sizeof(sentinel), (off_t)(bytes - 512)) == sizeof(sentinel));
	/* Dirty all future metadata, including unused FAT entries and root bytes. */
	while (offset > 0) {
		n = offset > sizeof(dirty) ? sizeof(dirty) : (unsigned)offset;
		offset -= n;
		assert(pwrite(fd, dirty, n, (off_t)offset) == n);
	}
	reset();
	invalid = g;
	invalid.data_sector++;
	assert(fat32_format_write(fd, &invalid) == EINVAL && calls == 0);
	assert(fat32_format_write(fd, &g) == 0);
	write_calls = calls;
	assert(flushes == 4 && writes > 4);
	reset();
	assert(fat32_format_verify(fd, &g) == 0);
	read_calls = calls;
	assert(writes == 0 && flushes == 0);
	/* Every write or flush can fail without returning success. */
	for (n = 1; n <= write_calls; n++) {
		reset(); fail_at = n;
		assert(fat32_format_write(fd, &g) != 0);
		assert(calls == n);
	}
	reset(); short_at = 1;
	assert(fat32_format_write(fd, &g) == EIO);
	reset(); interrupt_at = 1;
	assert(fat32_format_write(fd, &g) == 0);
	for (n = 1; n <= read_calls; n++) {
		reset(); fail_at = n;
		assert(fat32_format_verify(fd, &g) == EIO);
		assert(calls == n);
	}
	reset(); short_at = 1;
	assert(fat32_format_verify(fd, &g) == EIO);
	reset(); interrupt_at = 1;
	assert(fat32_format_verify(fd, &g) == 0);
	/* Detect corruption in boot, FSInfo, backup, both FATs and empty root. */
	for (n = 0; n < 7; n++) {
		uint32_t sectors[7];
		sectors[0] = 0; sectors[1] = 1; sectors[2] = 6; sectors[3] = 7;
		sectors[4] = 32; sectors[5] = 32 + g.fat_sectors;
		sectors[6] = g.data_sector;
		offset = (uint64_t)sectors[n] * sector_size + 100;
		assert(pread(fd, actual, 1, (off_t)offset) == 1);
		actual[0] ^= 0x80;
		assert(pwrite(fd, actual, 1, (off_t)offset) == 1);
		reset(); assert(fat32_format_verify(fd, &g) == EIO);
		actual[0] ^= 0x80;
		assert(pwrite(fd, actual, 1, (off_t)offset) == 1);
	}
	offset = (uint64_t)(g.data_sector + g.sectors_per_cluster) * sector_size;
	assert(pread(fd, actual, 512, (off_t)offset) == 512 && !memcmp(actual, sentinel, 512));
	assert(pread(fd, actual, 512, (off_t)(bytes - 512)) == 512 && !memcmp(actual, sentinel, 512));
	reset(); assert(fat32_format_verify(fd, &g) == 0);
	assert(close(fd) == 0);
	printf("FAT32 %u-byte sectors: geometry, %u write/flush faults, %u read faults, corruption and preserved data PASS\n",
	    sector_size, write_calls, read_calls);
	return 0;
}
