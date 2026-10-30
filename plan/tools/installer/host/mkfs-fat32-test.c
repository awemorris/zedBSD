/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of mkfs's FAT32 writer for an ESP: 512-MiB volumes with
 * 512-byte and 4096-byte sectors written into files, verified by the
 * writer and left for fsck.fat to read.
 */

#include "userland/base/mkfs/fat32-format.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static int format_one(const char *directory, uint32_t sector_size);

/*
 * Writes a 512-MiB FAT32 volume for each sector size into DIRECTORY.
 */
int
main(
	int argc,
	char **argv)
{
	int error;

	/* Takes the scratch directory. */
	if (argc != 2) {
		fprintf(stderr, "usage: mkfs-fat32-test DIRECTORY\n");
		return 2;
	}

	/* Writes and verifies both volumes. */
	error = format_one(argv[1], 512U);
	if (error != 0)
		return 1;
	error = format_one(argv[1], 4096U);
	if (error != 0)
		return 1;

	/* Succeeded: both volumes are written. */
	printf("mkfs-fat32-test: PASS\n");
	return 0;
}

/* Writes DIRECTORY/fat32-SECTOR.img and verifies it with the writer. */
static int
format_one(
	const char *directory,
	uint32_t sector_size)
{
	struct fat32_format_geometry geometry;
	char path[4096];
	uint64_t bytes;
	int error;
	int fd;

	/* Computes the geometry of a 512-MiB ESP at 1 MiB into a disk. */
	bytes = UINT64_C(512) * 1024U * 1024U;
	error = fat32_format_geometry(bytes / sector_size, sector_size,
	    1048576U / sector_size, 0x1234abcdU, &geometry);
	if (error != 0) {
		printf("FAIL: geometry for %u-byte sectors: %d\n", sector_size, error);
		return error;
	}

	/* Makes the file of the volume's size. */
	snprintf(path, sizeof(path), "%s/fat32-%u.img", directory, sector_size);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		perror(path);
		return 1;
	}
	error = ftruncate(fd, (off_t)bytes);
	if (error != 0) {
		perror(path);
		close(fd);
		return 1;
	}

	/* Writes the volume and verifies it. */
	error = fat32_format_write(fd, &geometry);
	if (error == 0)
		error = fat32_format_verify(fd, &geometry);
	close(fd);
	if (error != 0) {
		printf("FAIL: %u-byte sectors: %d\n", sector_size, error);
		return error;
	}

	/* Succeeded: reports the geometry for the log. */
	printf("ok: %u-byte sectors, %u sectors per cluster, %u clusters\n", sector_size,
	    geometry.sectors_per_cluster, geometry.clusters);
	return 0;
}
