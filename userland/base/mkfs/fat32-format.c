/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Portable FAT32 metadata initialization for an already admitted volume. */
#include "fat32-format.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define FAT32_RESERVED 32U
#define FAT32_CHUNK 32768U

static void put16(unsigned char *, unsigned, uint32_t);
static void put32(unsigned char *, unsigned, uint32_t);
static int geometry_valid(const struct fat32_format_geometry *);
static void make_sector(const struct fat32_format_geometry *, uint32_t,
    unsigned char *);
static int transfer(int, unsigned char *, size_t, uint64_t, int);
static int run(int, const struct fat32_format_geometry *, int);

/* Derives enough FAT entries without truncating the requested volume. */
int
fat32_format_geometry(uint64_t sectors, uint32_t sector_size,
    uint64_t hidden, uint32_t volume_id, struct fat32_format_geometry *out)
{
	uint64_t low, high, mid, entries, clusters, data, bytes;
	uint32_t spc;
	off_t last;

	if (out == NULL)
		return EINVAL;
	memset(out, 0, sizeof(*out));
	if (sector_size != 512 && sector_size != 1024 &&
	    sector_size != 2048 && sector_size != 4096)
		return EINVAL;
	if (sectors <= FAT32_RESERVED || sectors > UINT32_MAX)
		return EINVAL;
	if (hidden > UINT32_MAX)
		return EOVERFLOW;

	/* The codec uses the host's real off_t, including ILP32 builds. */
	bytes = sectors * sector_size;
	last = (off_t)(bytes - 1);
	if (last < 0 || (uint64_t)last != bytes - 1)
		return EOVERFLOW;

	for (spc = 1; spc <= 32768U / sector_size; spc *= 2) {
		/* Binary search the smallest FAT that describes its own data area. */
		low = 1;
		high = ((sectors / spc + 2) * 4 + sector_size - 1) / sector_size;
		if (FAT32_RESERVED + 2 * high >= sectors)
			continue;
		while (low < high) {
			mid = low + (high - low) / 2;
			clusters = (sectors - FAT32_RESERVED - 2 * mid) / spc;
			entries = mid * sector_size / 4;
			if (entries >= clusters + 2)
				high = mid;
			else
				low = mid + 1;
		}
		data = FAT32_RESERVED + 2 * low;
		clusters = (sectors - data) / spc;
		/* Cluster numbers must not enter FAT32's reserved value range. */
		if (clusters < 65525)
			continue;
		if (clusters > UINT32_C(0x0fffffee))
			continue;
		out->sector_size = sector_size;
		out->sectors = (uint32_t)sectors;
		out->sectors_per_cluster = spc;
		out->fat_sectors = (uint32_t)low;
		out->clusters = (uint32_t)clusters;
		out->data_sector = (uint32_t)data;
		out->hidden_sectors = (uint32_t)hidden;
		out->volume_id = volume_id;
		return 0;
	}
	return EINVAL;
}

int
fat32_format_write(int fd, const struct fat32_format_geometry *geometry)
{
	return run(fd, geometry, 0);
}

int
fat32_format_verify(int fd, const struct fat32_format_geometry *geometry)
{
	return run(fd, geometry, 1);
}

static void
put16(unsigned char *buffer, unsigned offset, uint32_t value)
{
	buffer[offset] = (unsigned char)value;
	buffer[offset + 1] = (unsigned char)(value >> 8);
}

static void
put32(unsigned char *buffer, unsigned offset, uint32_t value)
{
	put16(buffer, offset, value);
	put16(buffer, offset + 2, value >> 16);
}

/* Rejects inconsistent caller geometry before the first read or write. */
static int
geometry_valid(const struct fat32_format_geometry *g)
{
	struct fat32_format_geometry expected;
	int error;

	if (g == NULL)
		return EINVAL;
	error = fat32_format_geometry(g->sectors, g->sector_size,
	    g->hidden_sectors, g->volume_id, &expected);
	if (error != 0)
		return error;
	if (g->sectors_per_cluster != expected.sectors_per_cluster ||
	    g->fat_sectors != expected.fat_sectors ||
	    g->clusters != expected.clusters || g->data_sector != expected.data_sector)
		return EINVAL;
	return 0;
}

/* Emits only metadata; free data clusters are never initialized or verified. */
static void
make_sector(const struct fat32_format_geometry *g, uint32_t sector,
    unsigned char *buffer)
{
	memset(buffer, 0, g->sector_size);
	if (sector == 0 || sector == 6) {
		/* This ESP carries EFI programs, not executable BIOS boot code. */
		buffer[0] = 0xeb;
		buffer[1] = 0xfe;
		buffer[2] = 0x90;
		memcpy(buffer + 3, "zedBSD  ", 8);
		put16(buffer, 11, g->sector_size);
		buffer[13] = (unsigned char)g->sectors_per_cluster;
		put16(buffer, 14, FAT32_RESERVED);
		buffer[16] = 2;
		buffer[21] = 0xf8;
		put16(buffer, 24, 63);
		put16(buffer, 26, 255);
		put32(buffer, 28, g->hidden_sectors);
		put32(buffer, 32, g->sectors);
		put32(buffer, 36, g->fat_sectors);
		put32(buffer, 44, 2);
		put16(buffer, 48, 1);
		put16(buffer, 50, 6);
		buffer[64] = 0x80;
		buffer[66] = 0x29;
		put32(buffer, 67, g->volume_id);
		memcpy(buffer + 71, "NO NAME    ", 11);
		memcpy(buffer + 82, "FAT32   ", 8);
		put16(buffer, 510, 0xaa55);
	} else if (sector == 1 || sector == 7) {
		put32(buffer, 0, UINT32_C(0x41615252));
		put32(buffer, 484, UINT32_C(0x61417272));
		put32(buffer, 488, g->clusters - 1);
		put32(buffer, 492, 3);
		put32(buffer, 508, UINT32_C(0xaa550000));
	} else if (sector == FAT32_RESERVED ||
	    sector == FAT32_RESERVED + g->fat_sectors) {
		/* Reserved entries and the empty root's end-of-chain marker. */
		put32(buffer, 0, UINT32_C(0x0ffffff8));
		put32(buffer, 4, UINT32_C(0x0fffffff));
		put32(buffer, 8, UINT32_C(0x0fffffff));
	}
}

/* Retry interrupted calls, but report short I/O as an incomplete operation. */
static int
transfer(int fd, unsigned char *buffer, size_t length, uint64_t offset, int read)
{
	ssize_t done;

	do {
		if (read)
			done = pread(fd, buffer, length, (off_t)offset);
		else
			done = pwrite(fd, buffer, length, (off_t)offset);
	} while (done < 0 && errno == EINTR);
	if (done < 0)
		return errno;
	if ((size_t)done != length)
		return EIO;
	return 0;
}

static int
run(int fd, const struct fat32_format_geometry *g, int verify)
{
	unsigned char *buffer, *actual;
	uint32_t sector, end, count, index;
	size_t length;
	int error;

	error = geometry_valid(g);
	if (error != 0)
		return error;
	buffer = malloc(FAT32_CHUNK * 2);
	if (buffer == NULL)
		return ENOMEM;
	actual = buffer + FAT32_CHUNK;
	end = g->data_sector + g->sectors_per_cluster;

	/* Invalidate both old boot records before replacing filesystem metadata. */
	if (!verify) {
		memset(buffer, 0, g->sector_size);
		error = transfer(fd, buffer, g->sector_size, 0, 0);
		if (error == 0)
			error = transfer(fd, buffer, g->sector_size,
			    (uint64_t)6 * g->sector_size, 0);
		if (error == 0 && fsync(fd) < 0)
			error = errno;
	}
	for (sector = 0; error == 0 && sector < end; sector += count) {
		count = FAT32_CHUNK / g->sector_size;
		if (count > end - sector)
			count = end - sector;
		for (index = 0; index < count; index++) {
			make_sector(g, sector + index, buffer + index * g->sector_size);
			if (!verify && (sector + index == 0 || sector + index == 6))
				memset(buffer + index * g->sector_size, 0, g->sector_size);
		}
		length = (size_t)count * g->sector_size;
		if (verify) {
			error = transfer(fd, actual, length,
			    (uint64_t)sector * g->sector_size, 1);
			if (error == 0 && memcmp(buffer, actual, length) != 0)
				error = EIO;
		} else {
			error = transfer(fd, buffer, length,
			    (uint64_t)sector * g->sector_size, 0);
		}
	}

	/* Flush allocation metadata before publishing backup, then primary BPB. */
	if (!verify && error == 0) {
		if (fsync(fd) < 0)
			error = errno;
		make_sector(g, 6, buffer);
		if (error == 0)
			error = transfer(fd, buffer, g->sector_size,
			    (uint64_t)6 * g->sector_size, 0);
		if (error == 0 && fsync(fd) < 0)
			error = errno;
		if (error == 0)
			error = transfer(fd, buffer, g->sector_size, 0, 0);
		if (error == 0 && fsync(fd) < 0)
			error = errno;
	}
	free(buffer);
	return error;
}
