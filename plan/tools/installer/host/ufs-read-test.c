/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the loaders' UFS reader.
 *
 *     ufs-read-test IMAGE TREE
 *
 * IMAGE is a UFS volume made from TREE (zedimage-host ufs).  The test reads
 * the files the list below names through the reader and compares them with
 * TREE, then damages copies of the volume in memory and checks that each
 * damage is refused.
 */

#include "bootloader/common/ufs-read.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A volume image in memory, read in the medium's blocks. */
struct memory_medium {
	uint8_t *bytes;
	uint64_t size;
	uint32_t block_size;
};

static int medium_read(void *context, uint64_t block, uint32_t count, void *buffer);
static uint8_t *load_file(const char *path, uint64_t *size);
static void check(int condition, const char *what);
static void compare_file(struct zbl_ufs_volume *volume, const char *tree, const char *path);
static enum zbl_ufs_result mount_copy(struct memory_medium *medium, uint32_t block_size, struct zbl_ufs_volume *volume);

/* The reader's work storage, too large for the stack. */
static struct zbl_ufs_work work;

/* How many checks failed. */
static int failures;

/*
 * Runs the checks on IMAGE against TREE.
 */
int
main(
	int argc,
	char **argv)
{
	struct memory_medium medium;
	struct zbl_ufs_volume volume;
	struct zbl_ufs_file file;
	enum zbl_ufs_result result;
	uint8_t *image;
	uint64_t size;
	uint8_t saved[8];

	/* Takes the image and the tree. */
	if (argc != 3) {
		fprintf(stderr, "usage: ufs-read-test IMAGE TREE\n");
		return 2;
	}
	image = load_file(argv[1], &size);
	if (image == NULL)
		return 1;
	medium.bytes = image;
	medium.size = size;

	/* Mounts the volume with 512-byte medium blocks. */
	result = mount_copy(&medium, 512U, &volume);
	check(result == ZBL_UFS_OK, "mount with 512-byte blocks");
	if (result != ZBL_UFS_OK) {
		printf("mount: %s\n", zbl_ufs_result_name(result));
		return 1;
	}

	/* Reads every listed file and compares it with the tree. */
	compare_file(&volume, argv[2], "/boot/vmunix");
	compare_file(&volume, argv[2], "/boot/logo.ppm");
	compare_file(&volume, argv[2], "/etc/rc.conf");
	compare_file(&volume, argv[2], "/a/b/c/d/e/f/g/h/deep.txt");
	compare_file(&volume, argv[2], "/empty");
	compare_file(&volume, argv[2], "/many/file-0399");

	/* Refuses what is not a regular file at the end of a path. */
	result = zbl_ufs_lookup(&volume, "/boot", &file);
	check(result == ZBL_UFS_NOT_REGULAR, "a directory is not a file");
	result = zbl_ufs_lookup(&volume, "/boot/missing", &file);
	check(result == ZBL_UFS_NOT_FOUND, "a missing name");
	result = zbl_ufs_lookup(&volume, "/etc/rc.conf/x", &file);
	check(result == ZBL_UFS_NOT_DIRECTORY, "a file in the middle of a path");
	result = zbl_ufs_lookup(&volume, "/link/vmunix", &file);
	check(result == ZBL_UFS_SYMLINK, "a symbolic link on the path");
	result = zbl_ufs_lookup(&volume, "boot/vmunix", &file);
	check(result == ZBL_UFS_BAD_PATH, "a relative path");
	result = zbl_ufs_lookup(&volume, "/boot//vmunix", &file);
	check(result == ZBL_UFS_BAD_PATH, "an empty component");

	/* Refuses a read past the end of a file. */
	result = zbl_ufs_lookup(&volume, "/etc/rc.conf", &file);
	check(result == ZBL_UFS_OK, "find rc.conf");
	result = zbl_ufs_read(&volume, &file, file.size, saved, 1);
	check(result == ZBL_UFS_PAST_END, "a read past the end");

	/* Refuses the 1024-byte fragments on a medium of 4096-byte blocks. */
	result = mount_copy(&medium, 4096U, &volume);
	check(result == ZBL_UFS_FRAGMENT_TOO_SMALL, "a fragment smaller than the medium's block");

	/* Refuses a volume of the other byte order. */
	memcpy(saved, image + 65536 + 1372, 4);
	image[65536 + 1372] = 0x54;
	image[65536 + 1373] = 0x19;
	image[65536 + 1374] = 0x01;
	image[65536 + 1375] = 0x19;
	result = mount_copy(&medium, 512U, &volume);
	check(result == ZBL_UFS_SWAPPED, "a byte-swapped magic");

	/* Refuses a volume without the magic. */
	image[65536 + 1372] = 0;
	result = mount_copy(&medium, 512U, &volume);
	check(result == ZBL_UFS_NOT_UFS, "no magic");
	memcpy(image + 65536 + 1372, saved, 4);

	/* Refuses a volume that claims more fragments than its partition. */
	memcpy(saved, image + 65536 + 1080, 8);
	image[65536 + 1080 + 6] = 0x10;
	result = mount_copy(&medium, 512U, &volume);
	check(result == ZBL_UFS_LARGER_THAN_PARTITION, "a size past the partition");
	memcpy(image + 65536 + 1080, saved, 8);

	/* Refuses an inconsistent block size. */
	memcpy(saved, image + 65536 + 48, 4);
	image[65536 + 48] = 0x01;
	result = mount_copy(&medium, 512U, &volume);
	check(result == ZBL_UFS_BAD_SUPERBLOCK, "a block size that is no power of two");
	memcpy(image + 65536 + 48, saved, 4);

	/* Still reads the volume once it is put back. */
	result = mount_copy(&medium, 512U, &volume);
	check(result == ZBL_UFS_OK, "mount after the damage is put back");

	/* Refuses an indirect block pointer past the volume (the kernel's single indirect). */
	result = zbl_ufs_lookup(&volume, "/boot/vmunix", &file);
	check(result == ZBL_UFS_OK, "find the kernel again");
	file.inode[208 + 7] = 0x7f;
	work.indirect_number[0] = 0;
	result = zbl_ufs_read(&volume, &file, 12U * volume.block_size, saved, 8);
	check(result == ZBL_UFS_BAD_BLOCK, "an indirect pointer past the volume");

	/* Reports the verdict. */
	free(image);
	if (failures != 0) {
		printf("ufs-read-test: FAIL (%d)\n", failures);
		return 1;
	}
	printf("ufs-read-test: PASS\n");
	return 0;
}

/* Reads blocks of the image in memory. */
static int
medium_read(
	void *context,
	uint64_t block,
	uint32_t count,
	void *buffer)
{
	struct memory_medium *medium;
	uint64_t offset;
	uint64_t bytes;

	/* Refuses a read past the image's end. */
	medium = context;
	offset = block * medium->block_size;
	bytes = (uint64_t)count * medium->block_size;
	if (offset > medium->size || bytes > medium->size - offset)
		return 1;

	/* Copies the blocks out. */
	memcpy(buffer, medium->bytes + offset, (size_t)bytes);
	return 0;
}

/* Reads a whole file into memory. */
static uint8_t *
load_file(
	const char *path,
	uint64_t *size)
{
	FILE *stream;
	uint8_t *bytes;
	long length;

	/* Opens the file and measures it. */
	stream = fopen(path, "rb");
	if (stream == NULL) {
		perror(path);
		return NULL;
	}
	fseek(stream, 0, SEEK_END);
	length = ftell(stream);
	fseek(stream, 0, SEEK_SET);

	/* Reads it whole. */
	bytes = malloc(length > 0 ? (size_t)length : 1U);
	if (bytes == NULL) {
		fclose(stream);
		return NULL;
	}
	if (length > 0 && fread(bytes, 1, (size_t)length, stream) != (size_t)length) {
		fclose(stream);
		free(bytes);
		return NULL;
	}
	fclose(stream);
	*size = (uint64_t)length;
	return bytes;
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

/* Reads one file through the reader, in uneven pieces, and compares it with the tree. */
static void
compare_file(
	struct zbl_ufs_volume *volume,
	const char *tree,
	const char *path)
{
	struct zbl_ufs_file file;
	enum zbl_ufs_result result;
	char host_path[4096];
	uint8_t *expected;
	uint8_t *actual;
	uint64_t size;
	uint64_t offset;
	size_t piece;

	/* Reads the tree's copy. */
	snprintf(host_path, sizeof(host_path), "%s%s", tree, path);
	expected = load_file(host_path, &size);
	if (expected == NULL) {
		check(0, host_path);
		return;
	}

	/* Finds the file on the volume. */
	result = zbl_ufs_lookup(volume, path, &file);
	if (result != ZBL_UFS_OK) {
		printf("FAIL: %s: %s\n", path, zbl_ufs_result_name(result));
		failures++;
		free(expected);
		return;
	}
	check(file.size == size, path);

	/* Reads it in pieces of a prime size, so pieces cross block boundaries. */
	actual = malloc(size > 0 ? (size_t)size : 1U);
	if (actual == NULL) {
		check(0, "memory");
		free(expected);
		return;
	}
	for (offset = 0; offset < size; offset += piece) {
		piece = 100003U;
		if (piece > size - offset)
			piece = (size_t)(size - offset);
		result = zbl_ufs_read(volume, &file, offset, actual + offset, piece);
		if (result != ZBL_UFS_OK) {
			printf("FAIL: %s at %llu: %s\n", path, (unsigned long long)offset, zbl_ufs_result_name(result));
			failures++;
			break;
		}
	}
	if (size > 0)
		check(memcmp(expected, actual, (size_t)size) == 0, path);
	printf("ok: %s (%llu bytes)\n", path, (unsigned long long)size);
	free(expected);
	free(actual);
}

/* Mounts the image with a given medium block size. */
static enum zbl_ufs_result
mount_copy(
	struct memory_medium *medium,
	uint32_t block_size,
	struct zbl_ufs_volume *volume)
{
	struct zbl_ufs_device device;

	/* Describes the image as a partition of that block size. */
	medium->block_size = block_size;
	device.context = medium;
	device.block_size = block_size;
	device.block_count = medium->size / block_size;
	device.read = medium_read;
	return zbl_ufs_mount(volume, &device, &work);
}
