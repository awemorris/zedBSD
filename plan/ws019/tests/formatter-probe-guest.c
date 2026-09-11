/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Observes real swap activation and persistence in the formatter QEMU cells.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zedbsd/system.h>

static int swap_state(const char *path, int expected);
static int persistence(const char *path, int writing);
static int create_file(const char *path, const char *size_text);
static int select_overlay(const char *path);
static int flip_byte(const char *path, const char *offset_text);

/*
 * Runs a bounded observation through the ordinary target interfaces.
 */
int
main(
	int argc,
	char **argv)
{
	int result;

	/* Selects one observation with an explicit target. */
	if (argc == 4 && strcmp(argv[1], "flip") == 0) {
		result = flip_byte(argv[2], argv[3]);
	} else if (argc == 3 && strcmp(argv[1], "select-overlay") == 0) {
		result = select_overlay(argv[2]);
	} else if (argc == 4 && strcmp(argv[1], "create") == 0) {
		result = create_file(argv[2], argv[3]);
	} else if (argc == 4 && strcmp(argv[1], "swap") == 0) {
		result = swap_state(argv[2], atoi(argv[3]));
	} else if (argc == 3 && strcmp(argv[1], "write") == 0) {
		result = persistence(argv[2], 1);
	} else if (argc == 3 && strcmp(argv[1], "read") == 0) {
		result = persistence(argv[2], 0);
	} else {
		return 2;
	}

	/* Reports successful observation only when every check passed. */
	if (result == 0)
		puts("formatter-probe PASS");
	return result;
}

/* Toggles one byte only in the two explicit disposable formatter files. */
static int
flip_byte(
	const char *path,
	const char *offset_text)
{
	struct stat status;
	char *end;
	unsigned long offset;
	unsigned char byte;
	ssize_t count;
	int fd;
	int error;

	/* Restricts the corruption helper to this fixture's generated files. */
	if (strcmp(path, "/q078/data.img") != 0 && strcmp(path, "/q078/swapfile") != 0)
		return 1;

	/* Parses a complete bounded byte offset. */
	errno = 0;
	offset = strtoul(offset_text, &end, 10);
	if (errno != 0 || end == offset_text || *end != '\0')
		return 1;

	/* Opens a regular non-symlink target and checks the exact mutation bound. */
	fd = open(path, O_RDWR | O_NOFOLLOW);
	if (fd < 0)
		return 1;
	error = fstat(fd, &status);
	if (error < 0 || !S_ISREG(status.st_mode) || offset >= (unsigned long)status.st_size) {
		close(fd);
		return 1;
	}

	/* Performs a reversible byte change and flushes it before verification. */
	count = pread(fd, &byte, 1, (off_t)offset);
	if (count != 1) {
		close(fd);
		return 1;
	}
	byte ^= 0x5a;
	count = pwrite(fd, &byte, 1, (off_t)offset);
	error = fsync(fd);
	if (count != 1 || error < 0) {
		close(fd);
		return 1;
	}

	/* Requires successful descriptor release before reporting mutation done. */
	error = close(fd);
	if (error < 0)
		return 1;
	return 0;
}

/* Selects the generated files in this cell's existing disposable boot config. */
static int
select_overlay(
	const char *path)
{
	static const char configuration[] =
		"kernel=vmunix\n"
		"boot1=PARTUUID=78190000-1111-4111-8111-111111111111\n"
		"overlay-root=rootfs.img\n"
		"overlay-data=boot1:data.img\n"
		"swap0=boot1:swapfile\n";
	struct stat status;
	char actual[sizeof(configuration)];
	ssize_t count;
	int fd;

	/* Requires the runner's exact mounted disposable configuration path. */
	if (strcmp(path, "/q078-boot/zedbsd.cfg") != 0)
		return 1;
	fd = open(path, O_RDWR | O_NOFOLLOW);
	if (fd < 0)
		return 1;

	/* Refuses an unexpected object before replacing its small contents. */
	if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
	    status.st_size <= 0 || status.st_size > 4096) {
		close(fd);
		return 1;
	}

	/* Writes, trims, and flushes the complete configuration before reboot. */
	count = pwrite(fd, configuration, sizeof(configuration) - 1, 0);
	if (count != (ssize_t)sizeof(configuration) - 1 ||
	    ftruncate(fd, sizeof(configuration) - 1) < 0 || fsync(fd) < 0) {
		close(fd);
		return 1;
	}

	/* Reads back the exact bytes and EOF through the ordinary file API. */
	count = pread(fd, actual, sizeof(actual), 0);
	if (count != (ssize_t)sizeof(configuration) - 1 ||
	    memcmp(actual, configuration, sizeof(configuration) - 1) != 0) {
		close(fd);
		return 1;
	}
	return close(fd) < 0 ? 1 : 0;
}

/* Creates a fixture through the same primitives available to the installer. */
static int
create_file(
	const char *path,
	const char *size_text)
{
	char *end;
	struct stat status;
	unsigned long size;
	mode_t previous_mask;
	int fd;
	int open_error;

	/* Limits this observer to the small disposable fixture sizes. */
	size = strtoul(size_text, &end, 10);
	if (*end != '\0' || size > 67108864UL)
		return 1;

	/* Uses the mode representable on a FAT volume without POSIX metadata. */
	previous_mask = umask(0);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0755);
	open_error = errno;
	umask(previous_mask);

	/* Refuses a failed creation without changing an existing destination. */
	if (fd < 0) {
		errno = open_error;
		perror("formatter-probe create open");
		return 1;
	}

	/* Allocates the caller-sized file before invoking any formatter. */
	if (ftruncate(fd, (off_t)size) < 0) {
		perror("formatter-probe create ftruncate");
		close(fd);
		return 1;
	}

	/* Flushes allocation metadata before the formatter collects extents. */
	if (fsync(fd) < 0) {
		perror("formatter-probe create fsync");
		close(fd);
		return 1;
	}

	/* Verifies that the new regular file retained its caller-selected size. */
	if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
	    status.st_size != (off_t)size) {
		close(fd);
		return 1;
	}

	/* Returns only after closing the fixture's creation descriptor. */
	if (close(fd) < 0) {
		perror("formatter-probe create close");
		return 1;
	}
	return 0;
}

/* Checks the selected source through the production swap query. */
static int
swap_state(
	const char *path,
	int expected)
{
	struct system_swap_source_info info;
	unsigned index;
	int active;
	int fd;

	/* Opens the diagnostic endpoint without mutation privileges. */
	fd = open("/dev/system", O_RDONLY);
	if (fd < 0)
		return 1;

	/* Finds the exact source and verifies its production slot count. */
	active = 0;
	for (index = 0; index < ZEDBSD_SYSTEM_SWAP_SOURCE_COUNT; index++) {
		memset(&info, 0, sizeof(info));
		info.version = ZEDBSD_SYSTEM_SWAP_VERSION;
		info.struct_size = sizeof(info);
		info.source_id = index;
		if (ioctl(fd, ZEDBSD_SYSTEM_GET_SWAP_SOURCE, &info) < 0) {
			close(fd);
			return 1;
		}

		/* Records only the source named by this test. */
		if (strcmp(info.source, path) == 0 &&
		    info.state == ZEDBSD_SYSTEM_SWAP_STATE_ACTIVE) {
			printf("formatter-swap %s version=%u slots=%u\n",
				path, info.header_version, info.total_pages);
			if (info.header_version != 2 || info.total_pages != 16383) {
				close(fd);
				return 1;
			}
			active++;
		}
	}

	/* Checks both activation and clean deactivation. */
	if (close(fd) < 0 || active != expected)
		return 1;
	return 0;
}

/* Writes or verifies an exact payload in the generated overlay upper. */
static int
persistence(
	const char *path,
	int writing)
{
	static const char payload[] = "q129 generated UFS overlay persistence\n";
	char buffer[sizeof(payload)];
	ssize_t count;
	int fd;
	int flags;

	/* Opens through the ordinary overlay path and file operations. */
	flags = writing ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY;
	fd = open(path, flags, 0644);
	if (fd < 0)
		return 1;

	/* Writes durably or verifies an exact payload including EOF. */
	if (writing) {
		count = write(fd, payload, sizeof(payload) - 1);
		if (count != (ssize_t)sizeof(payload) - 1 || fsync(fd) < 0) {
			close(fd);
			return 1;
		}
	} else {
		count = read(fd, buffer, sizeof(buffer));
		if (count != (ssize_t)sizeof(payload) - 1 ||
		    memcmp(buffer, payload, sizeof(payload) - 1) != 0) {
			close(fd);
			return 1;
		}
	}

	/* Completes the observation after closing the backing description. */
	if (close(fd) < 0)
		return 1;
	return 0;
}
