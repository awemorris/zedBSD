/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the UEFI loader's FAT boot-sector check on disks of
 * 4096-byte blocks.
 *
 *     fat-bpb-4k-test FILE
 *
 * Writes a 512 MiB FAT32 volume with mkfs's formatter into FILE for each
 * sector size and gives its boot sector to zbl_uefi_fat_bpb_parse on media
 * of each block size, expecting what the kernel FAT driver takes: a BPB
 * sector of 512 through 4096 bytes that is at least the medium's block, on
 * media of 512 through 4096-byte blocks.
 */

#include "bootloader/uefi/volume-discovery.h"
#include "fat32-format.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VOLUME_BYTES (UINT64_C(512) * 1024U * 1024U)

static int format_volume(const char *path, uint32_t sector_size, uint8_t *boot);
static void expect(const uint8_t *boot, uint32_t block, enum zbl_uefi_fat_result wanted, const char *what);

/* How many checks failed. */
static int failures;

/*
 * Runs the checks for the two sector sizes mkfs writes.
 */
int
main(
	int argc,
	char **argv)
{
	struct zbl_uefi_fat_info info;
	uint8_t boot[4096];
	enum zbl_uefi_fat_result result;

	/* Takes the scratch file. */
	if (argc != 2) {
		fprintf(stderr, "usage: fat-bpb-4k-test FILE\n");
		return 2;
	}

	/* A 4096-byte-sector volume on 4096-, 2048- and 512-byte media blocks. */
	if (format_volume(argv[1], 4096U, boot) != 0)
		return 1;
	expect(boot, 4096U, ZBL_UEFI_FAT_OK, "4096 BPB on 4096 media");
	expect(boot, 2048U, ZBL_UEFI_FAT_OK, "4096 BPB on 2048 media");
	expect(boot, 512U, ZBL_UEFI_FAT_OK, "4096 BPB on 512 media");
	expect(boot, 8192U, ZBL_UEFI_FAT_UNSUPPORTED_BLOCK_SIZE, "8192 media");
	result = zbl_uefi_fat_bpb_parse(boot, sizeof(boot), 4096U, VOLUME_BYTES / 4096U, &info);
	if (result != ZBL_UEFI_FAT_OK || info.type != ZBL_UEFI_FAT32 ||
	    info.bytes_per_sector != 4096U || info.volume_serial != 0x12345678U) {
		printf("FAIL: the 4096 volume's description\n");
		failures++;
	}

	/* The volume claims more sectors than a smaller medium holds. */
	result = zbl_uefi_fat_bpb_parse(boot, sizeof(boot), 4096U, VOLUME_BYTES / 4096U - 1U, &info);
	if (result != ZBL_UEFI_FAT_MALFORMED) {
		printf("FAIL: a 4096 volume past its medium\n");
		failures++;
	}

	/* A 512-byte-sector volume is refused on 4096-byte media, as by the kernel. */
	if (format_volume(argv[1], 512U, boot) != 0)
		return 1;
	expect(boot, 512U, ZBL_UEFI_FAT_OK, "512 BPB on 512 media");
	expect(boot, 4096U, ZBL_UEFI_FAT_MALFORMED, "512 BPB on 4096 media");

	/* Reports the verdict. */
	if (failures != 0) {
		printf("fat-bpb-4k-test: FAIL (%d)\n", failures);
		return 1;
	}
	printf("fat-bpb-4k-test: PASS\n");
	return 0;
}

/* Writes one volume and reads its first sector back. */
static int
format_volume(
	const char *path,
	uint32_t sector_size,
	uint8_t *boot)
{
	struct fat32_format_geometry geometry;
	int fd;
	int error;

	/* Formats a sparse file of the volume's size. */
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || ftruncate(fd, (off_t)VOLUME_BYTES) != 0) {
		perror(path);
		return 1;
	}
	error = fat32_format_geometry(VOLUME_BYTES / sector_size, sector_size, 0, 0x12345678U, &geometry);
	if (error == 0)
		error = fat32_format_write(fd, &geometry);
	memset(boot, 0, 4096U);
	if (error == 0 && pread(fd, boot, sector_size, 0) != (ssize_t)sector_size)
		error = 1;
	close(fd);
	if (error != 0) {
		printf("FAIL: format %u: %d\n", sector_size, error);
		return 1;
	}
	return 0;
}

/* Checks the result for one medium's block size. */
static void
expect(
	const uint8_t *boot,
	uint32_t block,
	enum zbl_uefi_fat_result wanted,
	const char *what)
{
	struct zbl_uefi_fat_info info;
	enum zbl_uefi_fat_result result;

	/* Gives the parser the medium's block size and block count. */
	result = zbl_uefi_fat_bpb_parse(boot, 4096U, block, VOLUME_BYTES / block, &info);
	if (result != wanted) {
		printf("FAIL: %s: %s\n", what, zbl_uefi_fat_result_name(result));
		failures++;
	}
}
