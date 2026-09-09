/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "block-command.h"
#include "fat32-format.h"
#include "ufs-format.h"
#include <zedbsd/block.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
	unsigned digit;
	int native;
	int error;

	native = argc == 6 && strcmp(argv[1], "-t") == 0 &&
	    strcmp(argv[2], "ufs") == 0 && strcmp(argv[3], "--profile=native") == 0 &&
	    strcmp(argv[4], "--check-size") == 0;
	if (!native && !(argc == 5 && strcmp(argv[1], "-t") == 0 &&
	    strcmp(argv[2], "fat32") == 0 && strcmp(argv[3], "--check-size") == 0)) {
		fprintf(stderr, "usage: mkfs -t fat32 --check-size BYTES\n");
		fprintf(stderr, "       mkfs -t ufs --profile=native --check-size BYTES\n");
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
	if (bytes == 0 || bytes % 512U != 0) {
		fprintf(stderr, "mkfs: invalid 512-byte-sector size\n");
		return 1;
	}
	if (native) {
		format = "ufs-native";
		error = ufs_format_native_capacity(bytes, &capacity);
		free_bytes = capacity.free_bytes;
		free_inodes = capacity.free_inodes;
		unit = capacity.allocation_size;
	} else {
		format = "fat32";
		error = fat32_format_geometry(bytes / 512U, 512U, 0, 1, &geometry);
		unit = geometry.sectors_per_cluster * 512U;
		free_bytes = (uint64_t)(geometry.clusters > 0 ? geometry.clusters - 1U : 0) * unit;
		free_inodes = 0;
	}
	if (error != 0) {
		fprintf(stderr, "mkfs: capacity: %s\n", strerror(error));
		return 1;
	}
	/* FAT has no fixed inode table; zero is meaningful only for that profile. */
	if (printf("format\t1\t%s\t512\t%llu\t%llu\t%llu\t%u\n", format,
	    (unsigned long long)bytes, (unsigned long long)free_bytes,
	    (unsigned long long)free_inodes, unit) < 0 || fflush(stdout) != 0)
		return 1;
	return 0;
}

/* Formats only the description whose identity the operator confirms. */
int
mkfs_block_command(int argc, char **argv)
{
	struct zedbsd_block_info info;
	struct fat32_format_geometry geometry;
	struct stat status;
	const char *name;
	const char *format;
	char path[64], expected[80], answer[96];
	size_t length;
	int fd, error, started, native;
	uint64_t bytes;

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
	if (length == 0 || length >= ZEDBSD_BLOCK_NAME_MAX ||
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
	info.version = ZEDBSD_BLOCK_VERSION;
	info.struct_size = sizeof(info);
	if (ioctl(fd, BLKGETINFO, &info) < 0) {
		error = errno;
		goto out;
	}
	if (info.version != ZEDBSD_BLOCK_VERSION || info.struct_size != sizeof(info) ||
	    memchr(info.name, '\0', sizeof(info.name)) == NULL || strcmp(info.name, name) != 0) {
		error = EINVAL;
		goto out;
	}
	if (info.flags & ZEDBSD_BLOCK_READ_ONLY) {
		error = EROFS;
		goto out;
	}
	/* Current filesystem mount paths require 512-byte logical sectors. */
	if (info.sector_size != 512) {
		error = EOPNOTSUPP;
		goto out;
	}
	if (info.sector_count > UINT64_MAX / info.sector_size) {
		error = EOVERFLOW;
		goto out;
	}
	bytes = info.sector_count * info.sector_size;
	if (native)
		error = ufs_format_native_validate_size(bytes);
	else
		error = fat32_format_geometry(info.sector_count, info.sector_size,
		    info.parent_offset, info.device, &geometry);
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
		error = ufs_format_native_write(fd, bytes);
		if (error == 0)
			error = ufs_format_native_verify(fd, bytes);
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
