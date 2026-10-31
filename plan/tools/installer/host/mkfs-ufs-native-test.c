/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of mkfs's native UFS profiles.
 *
 *     mkfs-ufs-native-test write PROFILE SECTOR BYTES FILE
 *     mkfs-ufs-native-test check SECTOR BYTES FILE
 *
 * write formats FILE with one profile (ordinary, feature or native; the
 * sector size is used by the native profile only), so the same sizes
 * formatted by the formatter before and after a change can be compared
 * byte for byte.  Built with OLD_API, it calls the native functions
 * without the sector size, as they were.
 *
 * check formats FILE with the native profile for SECTOR, verifies it with
 * the formatter, decodes the superblock with mkfs's decoder, compares the
 * capacity report with the superblock, checks that the tail after the
 * volume names no tail journal or snapshot (no "ZUJ2", "ZSL1" or "ZSN1"
 * record: the kernel gives the volume the batched journal), and mounts it
 * with the loaders' reader on a medium of SECTOR-byte blocks.
 */

#include "ufs-format.h"
#include "ufs-super.h"
#include "bootloader/common/ufs-read.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A volume file read in the medium's blocks. */
struct file_medium {
	int fd;
	uint32_t block_size;
};

static int medium_read(void *context, uint64_t block, uint32_t count, void *buffer);
static int open_volume(const char *path, uint64_t bytes);
static int write_command(char **argv);
static int check_command(char **argv);
static int tail_has_record(int fd, uint64_t bytes);
static void check(int condition, const char *what);

/* The reader's work storage, too large for the stack. */
static struct zbl_ufs_work work;

/* How many checks failed. */
static int failures;

/*
 * Runs one command.
 */
int
main(
	int argc,
	char **argv)
{
	/* Takes the command and its operands. */
	if (argc == 6 && strcmp(argv[1], "write") == 0)
		return write_command(argv);
	if (argc == 5 && strcmp(argv[1], "check") == 0)
		return check_command(argv);
	fprintf(stderr, "usage: mkfs-ufs-native-test write PROFILE SECTOR BYTES FILE\n");
	fprintf(stderr, "       mkfs-ufs-native-test check SECTOR BYTES FILE\n");
	return 2;
}

/* Formats one file with one profile. */
static int
write_command(
	char **argv)
{
	uint64_t bytes;
	uint32_t sector;
	int fd;
	int error;

	/* Creates the sparse volume file. */
	sector = (uint32_t)strtoul(argv[3], NULL, 10);
	bytes = strtoull(argv[4], NULL, 10);
	fd = open_volume(argv[5], bytes);
	if (fd < 0)
		return 1;

	/* Formats it with the named profile. */
	if (strcmp(argv[2], "ordinary") == 0) {
		error = ufs_format_write(fd, bytes);
	} else if (strcmp(argv[2], "feature") == 0) {
		error = ufs_format_feature_write(fd, bytes);
	} else if (strcmp(argv[2], "native") == 0) {
#ifdef OLD_API
		(void)sector;
		error = ufs_format_native_write(fd, bytes);
#else
		error = ufs_format_native_write(fd, bytes, sector);
#endif
	} else {
		error = EINVAL;
	}
	close(fd);
	if (error != 0) {
		fprintf(stderr, "write %s %s: %s\n", argv[2], argv[4], strerror(error));
		return 1;
	}
	return 0;
}

#ifndef OLD_API
/* Formats, verifies, decodes and mounts one native volume. */
static int
check_command(
	char **argv)
{
	struct ufs_format_capacity capacity;
	struct ufs_super super;
	struct file_medium medium;
	struct zbl_ufs_device device;
	struct zbl_ufs_volume volume;
	struct zbl_ufs_file file;
	enum zbl_ufs_result result;
	uint8_t buffer[8192];
	uint64_t bytes;
	uint32_t sector, fragment;
	ssize_t count;
	int fd;
	int error;

	/* Formats and verifies the volume the way mkfs does. */
	sector = (uint32_t)strtoul(argv[2], NULL, 10);
	bytes = strtoull(argv[3], NULL, 10);
	fd = open_volume(argv[4], bytes);
	if (fd < 0)
		return 1;
	error = ufs_format_native_validate_size(bytes, sector);
	check(error == 0, "validate the size");
	error = ufs_format_native_write(fd, bytes, sector);
	check(error == 0, "write");
	error = ufs_format_native_verify(fd, bytes, sector);
	check(error == 0, "verify");

	/* Decodes the superblock and checks the fragment the sector calls for. */
	count = pread(fd, buffer, sizeof(buffer), 65536);
	check(count == (ssize_t)sizeof(buffer), "read the superblock");
	error = ufs_super_decode(buffer, sizeof(buffer), bytes / 512U, &super);
	check(error == 0, "decode the superblock");
	fragment = sector == 512U ? 1024U : 4096U;
	check(super.fsize == fragment, "the fragment size");
	check(super.bsize == fragment * 8U, "the block size");
	check(super.fsbtodb == (fragment == 1024U ? 1U : 3U), "the sector shift");
	check(super.sblkno * fragment == 65536U, "the superblock's fragment");
	check(super.dblkno % 8U == 0, "the data starts on a block");
	check(super.ipg == 256U, "256 inodes per group");
	check((uint64_t)super.fpg * fragment <= UINT64_C(63936) * 1024U, "a group spans at most the 1 KiB profile's");

	/* Checks the capacity report against the superblock it describes. */
	error = ufs_format_native_capacity(bytes, sector, &capacity);
	check(error == 0, "capacity");
	check(capacity.allocation_size == super.bsize, "the allocation unit is the block");
	check(capacity.free_bytes == super.cstotal_nbfree * super.bsize, "free bytes match the free blocks");
	check(capacity.free_inodes == super.cstotal_nifree, "free inodes match");
	printf("sector %u bytes %llu: fsize %u bsize %u ncg %u fpg %u free %llu inodes %llu\n",
	    sector, (unsigned long long)bytes, super.fsize, super.bsize, super.ncg, super.fpg,
	    (unsigned long long)capacity.free_bytes, (unsigned long long)capacity.free_inodes);

	/* The tail after the volume holds no journal or snapshot record. */
	check(!tail_has_record(fd, bytes), "no tail journal or snapshot");

	/* Mounts the volume with the reader on blocks of the disk's sector. */
	medium.fd = fd;
	medium.block_size = sector;
	device.context = &medium;
	device.block_size = sector;
	device.block_count = bytes / sector;
	device.read = medium_read;
	result = zbl_ufs_mount(&volume, &device, &work);
	check(result == ZBL_UFS_OK, "mount with the reader");
	if (result != ZBL_UFS_OK)
		printf("mount: %s\n", zbl_ufs_result_name(result));

	/* Reads the empty root through the reader. */
	if (result == ZBL_UFS_OK) {
		result = zbl_ufs_lookup(&volume, "/boot/vmunix", &file);
		check(result == ZBL_UFS_NOT_FOUND, "an empty root has no kernel");
		result = zbl_ufs_lookup(&volume, "/", &file);
		check(result == ZBL_UFS_NOT_REGULAR || result == ZBL_UFS_BAD_PATH, "the root is no file");
	}

	/* Mounts a 4 KiB-fragment volume on 512-byte blocks too. */
	if (sector == 4096U) {
		medium.block_size = 512U;
		device.block_size = 512U;
		device.block_count = bytes / 512U;
		result = zbl_ufs_mount(&volume, &device, &work);
		check(result == ZBL_UFS_OK, "mount on 512-byte blocks");
	}

	/* Refuses the sizes no profile is made for. */
	error = ufs_format_native_validate_size(bytes, 2048U);
	check(error == EOPNOTSUPP, "a 2048-byte sector");
	if (sector == 4096U) {
		error = ufs_format_native_validate_size(bytes + 1024U, sector);
		check(error == EINVAL, "a size that is no whole number of fragments");
	}

	/* Reports the verdict. */
	close(fd);
	if (failures != 0) {
		printf("check %u %llu: FAIL (%d)\n", sector, (unsigned long long)bytes, failures);
		return 1;
	}
	return 0;
}

/* Reports whether a sector of the tail after the volume starts a tail record. */
static int
tail_has_record(
	int fd,
	uint64_t bytes)
{
	uint8_t sector[512];
	uint64_t offset;
	ssize_t count;

	/* Reads each sector of the reserved tail and the alignment padding. */
	offset = bytes > (UINT64_C(3) << 20) ? bytes - (UINT64_C(3) << 20) : 0;
	for (; offset + sizeof(sector) <= bytes; offset += sizeof(sector)) {
		count = pread(fd, sector, sizeof(sector), (off_t)offset);
		if (count != (ssize_t)sizeof(sector))
			return 1;
		if (memcmp(sector, "ZUJ2", 4) == 0 ||
		    memcmp(sector, "ZSL1", 4) == 0 ||
		    memcmp(sector, "ZSN1", 4) == 0)
			return 1;
	}

	/* None is there. */
	return 0;
}
#else
/* The old formatter has no check command. */
static int
check_command(
	char **argv)
{
	(void)argv;
	return 2;
}
#endif

/* Creates an empty volume file of the given size. */
static int
open_volume(
	const char *path,
	uint64_t bytes)
{
	int fd;

	/* Truncates any old contents so the file is sparse and zero. */
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		perror(path);
		return -1;
	}
	if (ftruncate(fd, (off_t)bytes) != 0) {
		perror(path);
		close(fd);
		return -1;
	}
	return fd;
}

/* Reads blocks of the volume file. */
static int
medium_read(
	void *context,
	uint64_t block,
	uint32_t count,
	void *buffer)
{
	struct file_medium *medium;
	size_t bytes;
	ssize_t done;

	/* Reads the whole run or reports a failure. */
	medium = context;
	bytes = (size_t)count * medium->block_size;
	done = pread(medium->fd, buffer, bytes, (off_t)(block * medium->block_size));
	if (done != (ssize_t)bytes)
		return 1;
	return 0;
}

/* Counts and prints a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* Reports a failure by name. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}
