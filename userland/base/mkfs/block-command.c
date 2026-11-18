/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "block-command.h"
#include "fat32-format.h"
#include "ufs-format.h"
#include <uapi/block.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static int random_volume_id(uint32_t *volume_id);
static int parse_sector_size(const char *text, uint32_t *sector_size);

/* Queries the production geometry before a future partition even exists. */
int
mkfs_capacity_command(int argc, char **argv)
{
	struct ufs_format_capacity capacity;
	struct fat32_format_geometry geometry;
	const char *number;
	const char *format;
	uint64_t bytes;
	uint64_t free_bytes;
	uint64_t free_inodes;
	uint32_t unit;
	uint32_t sector_size;
	unsigned digit;
	int native;
	int fat;
	int sized;
	int error;
	int has_option;
	int is_t;
	int is_ufs;
	int is_fat32;
	int is_native;
	int is_check;
	int printed;
	int flushed;

	/*
	 * Takes the optional --sector-size=N before --check-size: the
	 * geometry of a disk with 4096-byte sectors differs.
	 */
	sector_size = 512U;
	sized = 0;
	has_option = 1;
	if (argc >= 5)
		has_option = strncmp(argv[argc - 3], "--sector-size=", 14);
	if (argc >= 5 && has_option == 0) {
		error = parse_sector_size(argv[argc - 3] + 14, &sector_size);
		if (error != 0)
			return 2;
		sized = 1;
	}

	/* Accepts the two profiles' exact command lines. */
	is_t = 1;
	is_ufs = 1;
	is_fat32 = 1;
	is_native = 1;
	is_check = 1;
	if (argc >= 5) {
		is_t = strcmp(argv[1], "-t");
		is_ufs = strcmp(argv[2], "ufs");
		is_fat32 = strcmp(argv[2], "fat32");
		is_check = strcmp(argv[argc - 2], "--check-size");
	}
	if (argc >= 4)
		is_native = strcmp(argv[3], "--profile=native");
	native = 0;
	if (argc == 6 + sized && is_t == 0 && is_ufs == 0 && is_native == 0 && is_check == 0)
		native = 1;
	fat = 0;
	if (argc == 5 + sized && is_t == 0 && is_fat32 == 0 && is_check == 0)
		fat = 1;
	if (!native && !fat) {
		fprintf(stderr, "usage: mkfs -t fat32 [--sector-size=512|4096] --check-size BYTES\n");
		fprintf(stderr, "       mkfs -t ufs --profile=native [--sector-size=512|4096] --check-size BYTES\n");
		return 2;
	}
	number = argv[argc - 1];
	bytes = 0;
	if (*number == '\0')
		return 2;
	while (*number != '\0') {
		if (*number < '0' || *number > '9')
			return 2;
		digit = (unsigned)(*number++ - '0');
		if (bytes > (UINT64_MAX - digit) / 10U)
			return 2;
		bytes = bytes * 10U + digit;
	}
	if (bytes == 0 || bytes % sector_size != 0) {
		fprintf(stderr, "mkfs: the size is not a whole number of %u-byte sectors\n", sector_size);
		return 1;
	}

	/* A 4096-byte disk takes the native UFS profile of 4 KiB fragments. */
	if (native) {
		format = "ufs-native";
		error = ufs_format_native_capacity(bytes, sector_size, &capacity);
		free_bytes = capacity.free_bytes;
		free_inodes = capacity.free_inodes;
		unit = capacity.allocation_size;
	} else {
		format = "fat32";
		error = fat32_format_geometry(bytes / sector_size, sector_size, 0, 1, &geometry);
		unit = geometry.sectors_per_cluster * sector_size;
		free_bytes = (uint64_t)(geometry.clusters > 0 ? geometry.clusters - 1U : 0) * unit;
		free_inodes = 0;
	}
	if (error != 0) {
		fprintf(stderr, "mkfs: capacity: %s\n", strerror(error));
		return 1;
	}

	/* FAT has no fixed inode table; zero is meaningful only for that profile. */
	printed = printf("format\t1\t%s\t%u\t%llu\t%llu\t%llu\t%u\n", format,
	    sector_size, (unsigned long long)bytes, (unsigned long long)free_bytes,
	    (unsigned long long)free_inodes, unit);
	if (printed < 0)
		return 1;
	flushed = fflush(stdout);
	if (flushed != 0)
		return 1;

	/* Succeeded: the capacity line is printed. */
	return 0;
}

/* Formats only the description whose identity the operator confirms. */
int
mkfs_block_command(int argc, char **argv)
{
	struct kern_block_info info;
	struct fat32_format_geometry geometry;
	struct stat status;
	const char *name;
	const char *format;
	char path[64], expected[80], answer[96];
	size_t length;
	int fd, error, started, native;
	uint64_t bytes;
	uint32_t volume_id;

	native = argc == 5 && strcmp(argv[1], "-t") == 0 &&
	    strcmp(argv[2], "ufs") == 0 && strcmp(argv[3], "--profile=native") == 0;
	if (!native && (argc != 4 || strcmp(argv[1], "-t") != 0 ||
	    strcmp(argv[2], "fat32") != 0)) {
		fprintf(stderr, "usage: mkfs -t fat32 DEVICE\n");
		fprintf(stderr, "       mkfs -t ufs --profile=native DEVICE\n");
		return 2;
	}
	format = native ? "ufs native" : "fat32";
	name = argv[argc - 1];
	if (strncmp(name, "/dev/", 5) == 0)
		name += 5;
	length = strlen(name);
	if (length == 0 || length >= KERN_BLOCK_NAME_MAX ||
	    strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != length) {
		fprintf(stderr, "mkfs: expected canonical block device name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/dev/%s", name);
	started = 0;
	error = 0;
	fd = open(path, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		error = errno;
		goto out;
	}
	if (fstat(fd, &status) < 0) {
		error = errno;
		goto out;
	}
	if (!S_ISBLK(status.st_mode)) {
		error = EINVAL;
		goto out;
	}
	memset(&info, 0, sizeof(info));
	info.version = KERN_BLOCK_VERSION;
	info.struct_size = sizeof(info);
	if (ioctl(fd, BLKGETINFO, &info) < 0) {
		error = errno;
		goto out;
	}
	if (info.version != KERN_BLOCK_VERSION || info.struct_size != sizeof(info) ||
	    memchr(info.name, '\0', sizeof(info.name)) == NULL || strcmp(info.name, name) != 0) {
		error = EINVAL;
		goto out;
	}
	if (info.flags & KERN_BLOCK_READ_ONLY) {
		error = EROFS;
		goto out;
	}
	/*
	 * FAT32 and native UFS are written for 512-byte and 4096-byte
	 * logical sectors; UFS takes 4 KiB fragments on the latter.
	 */
	if (info.sector_size != 512 && info.sector_size != 4096) {
		error = EOPNOTSUPP;
		goto out;
	}
	if (info.sector_count > UINT64_MAX / info.sector_size) {
		error = EOVERFLOW;
		goto out;
	}
	bytes = info.sector_count * info.sector_size;

	/*
	 * A FAT32 volume's serial is random: the loader and the kernel find
	 * the boot volume by it (boot0=UUID=), so it must not repeat the
	 * serial of a volume on another disk.
	 */
	volume_id = 0;
	if (!native) {
		error = random_volume_id(&volume_id);
		if (error != 0)
			goto out;
	}

	/* Checks the volume fits the format being written. */
	if (native)
		error = ufs_format_native_validate_size(bytes, info.sector_size);
	else
		error = fat32_format_geometry(info.sector_count, info.sector_size,
		    info.parent_offset, volume_id, &geometry);
	if (error != 0)
		goto out;
	if (ioctl(fd, BLKRESERVE, &info) < 0) {
		error = errno;
		goto out;
	}

	/* A redirected answer must still name the exact reviewed registration. */
	snprintf(expected, sizeof(expected), "FORMAT %s:%u", info.name, info.device);
	printf("Target %s registration=%u bytes=%llu\n", path, info.device,
	    (unsigned long long)info.sector_count * info.sector_size);
	printf("WARNING: Replace this volume's filesystem with %s. Existing files will be lost.\n", format);
	puts("Failure may leave a partially formatted volume. Data is not securely erased.");
	printf("Type %s to continue: ", expected);
	if (fflush(stdout) != 0 || ferror(stdout)) {
		error = EIO;
		goto out;
	}
	if (fgets(answer, sizeof(answer), stdin) == NULL) {
		error = EINVAL;
		goto out;
	}
	length = strlen(answer);
	if (length == 0 || answer[length - 1] != '\n') {
		error = EINVAL;
		goto out;
	}
	answer[--length] = '\0';
	if (length != 0 && answer[length - 1] == '\r')
		answer[--length] = '\0';
	if (strcmp(answer, expected) != 0) {
		error = EINVAL;
		goto out;
	}
	started = 1;
	if (native) {
		error = ufs_format_native_write(fd, bytes, info.sector_size);
		if (error == 0)
			error = ufs_format_native_verify(fd, bytes, info.sector_size);
	} else {
		error = fat32_format_write(fd, &geometry);
		if (error == 0)
			error = fat32_format_verify(fd, &geometry);
	}

out:
	/* Final close releases the reservation, including all failed operations. */
	if (fd >= 0 && close(fd) < 0 && error == 0)
		error = errno;
	if (error != 0) {
		fprintf(stderr, "mkfs: %s: %s%s\n", path, strerror(error),
		    started ? "; volume may be partially formatted" : "; no format writes attempted");
		return 1;
	}
	printf("mkfs: %s: %s initialized and verified (%llu bytes)\n", path, format,
	    (unsigned long long)info.sector_count * info.sector_size);
	if (fflush(stdout) != 0 || ferror(stdout))
		return 1;
	return 0;
}

/* Draws a nonzero FAT volume serial from the kernel's random source. */
static int
random_volume_id(
	uint32_t *volume_id)
{
	uint32_t value;
	int drawn;

	/* Draws until the serial is not zero, which the loader refuses. */
	value = 0;
	while (value == 0) {
		drawn = getentropy(&value, sizeof(value));
		if (drawn != 0)
			return errno;
	}

	/* Succeeded: the serial is random and nonzero. */
	*volume_id = value;
	return 0;
}

/* Reads a --sector-size value: 512 or 4096. */
static int
parse_sector_size(
	const char *text,
	uint32_t *sector_size)
{
	int is_512;
	int is_4096;

	/* Accepts the two logical sector sizes the formats are made for. */
	is_512 = strcmp(text, "512");
	if (is_512 == 0) {
		*sector_size = 512U;
		return 0;
	}
	is_4096 = strcmp(text, "4096");
	if (is_4096 == 0) {
		*sector_size = 4096U;
		return 0;
	}

	/* Refuses any other size. */
	return EINVAL;
}
