/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes the ZEDSWAP2 header of a swap partition.
 *
 * The partition is named as /dev/NAME.  The command reserves it, shows its
 * identity and size, and writes only after the operator types the exact
 * FORMAT NAME:REGISTRATION phrase; then it rereads the header with the
 * kernel's parser.  Only the header page is written: the slots are not
 * cleared, which the kernel does not need.
 */

#include "swap-format.h"

#include <kern/swap.h>
#include <uapi/block.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* The characters a canonical block device name is made of. */
#define DEVICE_NAME_CHARACTERS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"

static int open_partition(const char *name, int *fd, struct kern_block_info *info);
static int confirm(const struct kern_block_info *info, uint64_t bytes);

/*
 * Formats the swap partition /dev/NAME and reports the outcome.
 *
 * Returns the exit status: 0 when the header is written and verified, 2
 * for a name that is not a canonical device, 1 for any other failure.
 */
int
swap_device_command(
	const char *operand)
{
	struct kern_block_info info;
	const char *name;
	const char *partial;
	uint64_t bytes;
	size_t length;
	size_t valid_length;
	int started;
	int synced;
	int closed;
	int error;
	int fd;

	/* Takes the canonical name after /dev/. */
	name = operand + 5;
	length = strlen(name);
	if (length == 0 || length >= KERN_BLOCK_NAME_MAX) {
		fprintf(stderr, "mkswap: expected /dev/ and a canonical block device name\n");
		return 2;
	}

	/* Refuses a name with a character no canonical name holds. */
	valid_length = strspn(name, DEVICE_NAME_CHARACTERS);
	if (valid_length != length) {
		fprintf(stderr, "mkswap: expected /dev/ and a canonical block device name\n");
		return 2;
	}

	/* Opens and reserves the partition, refusing anything but a partition. */
	fd = -1;
	started = 0;
	bytes = 0;
	error = open_partition(name, &fd, &info);
	if (error != 0)
		goto out;
	bytes = info.sector_count * info.sector_size;

	/* Refuses a size the header cannot describe, before any write. */
	error = swap_format_validate_partition(bytes);
	if (error != 0)
		goto out;

	/* Asks the operator to name the partition. */
	error = confirm(&info, bytes);
	if (error != 0)
		goto out;

	/* Writes the header page and makes it durable. */
	started = 1;
	error = swap_format_write_partition(fd, bytes);
	if (error != 0)
		goto out;
	synced = fsync(fd);
	if (synced < 0) {
		error = errno;
		goto out;
	}

	/* Rereads the header with the kernel's parser. */
	error = swap_format_verify_partition(fd, bytes);

out:
	/* Releases the reservation, keeping the first error. */
	if (fd >= 0) {
		closed = close(fd);
		if (closed < 0 && error == 0)
			error = errno;
	}

	/* Reports a failure and whether the partition may have changed. */
	if (error != 0) {
		partial = "; nothing written";
		if (started)
			partial = "; the header may be partly written";
		fprintf(stderr, "mkswap: /dev/%s: %s%s\n", name, strerror(error), partial);
		return 1;
	}

	/* Succeeded: the partition carries a verified header. */
	printf("mkswap: /dev/%s: ZEDSWAP2 initialized and verified (%llu slots)\n",
	    name, (unsigned long long)(bytes / SWAP_PAGE_SIZE - 1U));
	return 0;
}

/*
 * Opens a partition for writing and reserves it for this command.
 *
 * The reservation fails while the partition is mounted, swapped on or
 * held open by another administrator, and lasts until the descriptor is
 * closed.
 */
static int
open_partition(
	const char *name,
	int *fd,
	struct kern_block_info *info)
{
	struct stat status;
	char path[64];
	int is_block;
	int reserved;
	int queried;
	int stated;
	int different;
	void *terminator;

	/* Opens the device node without following a link. */
	snprintf(path, sizeof(path), "/dev/%s", name);
	*fd = open(path, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (*fd < 0)
		return errno;

	/* Refuses a node that is not a block device. */
	stated = fstat(*fd, &status);
	if (stated < 0)
		return errno;
	is_block = S_ISBLK(status.st_mode);
	if (!is_block)
		return EINVAL;

	/* Reads the device's identity and geometry. */
	memset(info, 0, sizeof(*info));
	info->version = KERN_BLOCK_VERSION;
	info->struct_size = sizeof(*info);
	queried = ioctl(*fd, BLKGETINFO, info);
	if (queried < 0)
		return errno;

	/* Refuses an answer of another version or for another name. */
	if (info->version != KERN_BLOCK_VERSION || info->struct_size != sizeof(*info))
		return EINVAL;
	terminator = memchr(info->name, '\0', sizeof(info->name));
	if (terminator == NULL)
		return EINVAL;
	different = strcmp(info->name, name);
	if (different != 0)
		return EINVAL;

	/* Refuses a whole disk: the kernel swaps only on partitions. */
	if ((info->flags & KERN_BLOCK_PARTITION) == 0)
		return EINVAL;

	/* Refuses a read-only device. */
	if ((info->flags & KERN_BLOCK_READ_ONLY) != 0)
		return EROFS;

	/* Refuses a sector that does not tile a swap page. */
	if (info->sector_size == 0 || SWAP_PAGE_SIZE % info->sector_size != 0)
		return EOPNOTSUPP;
	if (info->sector_count > UINT64_MAX / info->sector_size)
		return EOVERFLOW;

	/* Takes the partition for this command alone. */
	reserved = ioctl(*fd, BLKRESERVE, info);
	if (reserved < 0)
		return errno;

	/* Succeeded: the partition is open and reserved. */
	return 0;
}

/* Shows the partition and asks for the exact FORMAT phrase. */
static int
confirm(
	const struct kern_block_info *info,
	uint64_t bytes)
{
	char expected[80];
	char answer[96];
	size_t length;
	char *line;
	int different;
	int flushed;

	/* Shows the identity and what the command is about to do. */
	snprintf(expected, sizeof(expected), "FORMAT %s:%u", info->name, info->device);
	printf("Target /dev/%s registration=%u sector-size=%u bytes=%llu\n", info->name,
	    info->device, info->sector_size, (unsigned long long)bytes);
	printf("WARNING: Make this partition a swap area. Its contents will be lost.\n");
	printf("Type %s to continue: ", expected);
	flushed = fflush(stdout);
	if (flushed != 0)
		return EIO;

	/* Reads one complete answer line. */
	line = fgets(answer, sizeof(answer), stdin);
	if (line == NULL)
		return ECANCELED;
	length = strlen(answer);
	if (length == 0 || answer[length - 1] != '\n')
		return ECANCELED;

	/* Drops the line ending, a CR before it included. */
	answer[length - 1] = '\0';
	length--;
	if (length != 0 && answer[length - 1] == '\r')
		answer[length - 1] = '\0';

	/* Refuses any answer but the phrase itself. */
	different = strcmp(answer, expected);
	if (different != 0)
		return ECANCELED;

	/* Succeeded: the operator named this partition. */
	return 0;
}
