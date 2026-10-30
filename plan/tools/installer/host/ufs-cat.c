/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes one file of a UFS volume to standard output through the loaders'
 * reader, the way the loader reads the kernel.
 *
 *     ufs-cat IMAGE OFFSET BLOCK PATH
 *
 * IMAGE is a disk image, OFFSET the byte offset of the partition in it and
 * BLOCK the medium's block size the reader is given.
 */

#include "bootloader/common/ufs-read.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* A partition of a disk image read in the medium's blocks. */
struct image_medium {
	int fd;
	uint64_t offset;
	uint32_t block_size;
};

static int medium_read(void *context, uint64_t block, uint32_t count, void *buffer);

/* The reader's work storage, too large for the stack. */
static struct zbl_ufs_work work;

/* A piece of the file at a time. */
static uint8_t piece[65536];

/*
 * Mounts the partition, finds the file and writes it out.
 */
int
main(
	int argc,
	char **argv)
{
	struct image_medium medium;
	struct zbl_ufs_device device;
	struct zbl_ufs_volume volume;
	struct zbl_ufs_file file;
	enum zbl_ufs_result result;
	uint64_t done;
	size_t length;
	off_t end;

	/* Takes the image, the partition's offset and the block size. */
	if (argc != 5) {
		fprintf(stderr, "usage: ufs-cat IMAGE OFFSET BLOCK PATH\n");
		return 2;
	}
	medium.fd = open(argv[1], O_RDONLY);
	if (medium.fd < 0) {
		perror(argv[1]);
		return 1;
	}
	medium.offset = strtoull(argv[2], NULL, 10);
	medium.block_size = (uint32_t)strtoul(argv[3], NULL, 10);
	end = lseek(medium.fd, 0, SEEK_END);

	/* Mounts the partition from its offset to the image's end. */
	device.context = &medium;
	device.block_size = medium.block_size;
	device.block_count = ((uint64_t)end - medium.offset) / medium.block_size;
	device.read = medium_read;
	result = zbl_ufs_mount(&volume, &device, &work);
	if (result == ZBL_UFS_OK)
		result = zbl_ufs_lookup(&volume, argv[4], &file);
	if (result != ZBL_UFS_OK) {
		fprintf(stderr, "ufs-cat: %s: %s\n", argv[4], zbl_ufs_result_name(result));
		return 1;
	}

	/* Writes the file out in pieces. */
	for (done = 0; done < file.size; done += length) {
		length = sizeof(piece);
		if (file.size - done < length)
			length = (size_t)(file.size - done);
		result = zbl_ufs_read(&volume, &file, done, piece, length);
		if (result != ZBL_UFS_OK) {
			fprintf(stderr, "ufs-cat: read: %s\n", zbl_ufs_result_name(result));
			return 1;
		}
		if (fwrite(piece, 1, length, stdout) != length)
			return 1;
	}
	return fflush(stdout) == 0 ? 0 : 1;
}

/* Reads blocks of the partition. */
static int
medium_read(
	void *context,
	uint64_t block,
	uint32_t count,
	void *buffer)
{
	struct image_medium *medium;
	size_t bytes;
	ssize_t done;

	/* Reads the whole run or reports a failure. */
	medium = context;
	bytes = (size_t)count * medium->block_size;
	done = pread(medium->fd, buffer, bytes,
	    (off_t)(medium->offset + block * medium->block_size));
	return done == (ssize_t)bytes ? 0 : 1;
}
