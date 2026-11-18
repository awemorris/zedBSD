/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reports free disk space (POSIX XCU df).
 *
 *	df [-k] [-P|-t] [file...]
 *
 * Without operands df writes a line for every mounted file system, in
 * the order of the kernel's mount table; with operands, one for the file
 * system each file is on, or that a mounted device special file holds.
 * The figures are in 512-byte units, or 1024-byte units with -k:
 *
 *	Filesystem 512-blocks Used Available Capacity Mounted on
 *	<name> <total> <used> <available> <percent>% <mount point>
 *
 * Used is the total less the free space; the capacity is used over used
 * plus available, rounded up.  The output is the -P format with or
 * without -P; -t (XSI: include the total space) changes nothing, since
 * the total is always written.  The name is the device (/dev/...) of a
 * file system on one, else its source or type.
 *
 * df exits with 0, or 1 when some file system could not be reported.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <uapi/mountinfo.h>

/*
 * A mounted file system: what df names it and where it is mounted, and
 * the device number of its mount point.
 */
struct df_mount {
	char name[KERN_MOUNT_INFO_PATH_MAX + 8];
	char target[KERN_MOUNT_INFO_PATH_MAX];
	dev_t device;
	int device_known;
};

static struct df_mount *load_mounts(size_t *count);
static const struct df_mount *find_mount(const struct df_mount *mounts, size_t count, const char *path);
static const struct df_mount *find_device(const struct df_mount *mounts, size_t count, const struct stat *status);
static int report(const char *name, const char *figures_path, const char *mount_point, unsigned long long unit);
static int units(unsigned long long blocks, unsigned long long fragment, unsigned long long unit, unsigned long long *converted);
static unsigned capacity(unsigned long long used, unsigned long long available);
static void usage(void);

/*
 * Runs df.
 */
int
main(
	int argc,
	char **argv)
{
	const struct df_mount *mount;
	struct df_mount *mounts;
	unsigned long long unit;
	size_t count;
	size_t index;
	int option;
	int portable;
	int total;
	int failed;
	int status;
	int index_operand;

	/* Reads the options. */
	unit = 512;
	portable = 0;
	total = 0;
	for (;;) {
		option = getopt(argc, argv, "kPt");
		if (option == -1)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'k':
			unit = 1024;
			break;
		case 'P':
			portable = 1;
			break;
		case 't':
			total = 1;
			break;
		default:
			usage();
			break;
		}
	}

	/* -P and -t are alternatives. */
	if (portable && total)
		usage();

	/* The mounted file systems. */
	mounts = load_mounts(&count);
	if (mounts == NULL && optind == argc)
		return 1;

	/* The heading. */
	printf("Filesystem %llu-blocks Used Available Capacity Mounted on\n", unit);

	/* Every file system, without operands; a table not read is a failure. */
	failed = 0;
	if (mounts == NULL)
		failed = 1;
	if (optind == argc) {
		for (index = 0; index < count; index++) {
			status = report(mounts[index].name, mounts[index].target, mounts[index].target, unit);
			if (status != 0)
				failed = 1;
		}
	}

	/* The file system of each operand. */
	for (index_operand = optind; index_operand < argc; index_operand++) {
		mount = find_mount(mounts, count, argv[index_operand]);
		if (mount == NULL) {
			failed = 1;
			continue;
		}

		/* Reports it with the figures of the file's file system. */
		status = report(mount->name, argv[index_operand], mount->target, unit);
		if (status != 0)
			failed = 1;
	}

	/* A failed write is a failure too. */
	status = fflush(stdout);
	if (status != 0) {
		command_error("df", "standard output");
		failed = 1;
	}

	/* Frees the table. */
	free(mounts);
	if (failed)
		return 1;

	/* Succeeded: every file system was reported. */
	return 0;
}

/*
 * Reads the kernel's mount table.  Returns the file systems, or NULL
 * with a diagnostic when the table cannot be read.
 */
static struct df_mount *
load_mounts(
	size_t *count)
{
	const struct kern_mount_info *entry;
	struct kern_mount_query *query;
	struct df_mount *mounts;
	struct stat status;
	size_t index;
	int descriptor;
	int result;
	int error;

	/* Asks /dev/system for the table. */
	*count = 0;
	query = calloc(1, sizeof(*query) + KERN_MOUNT_INFO_MAX * sizeof(query->entries[0]));
	if (query == NULL) {
		fprintf(stderr, "df: out of memory\n");
		return NULL;
	}

	/* Asks for up to the most entries the kernel keeps. */
	query->version = KERN_MOUNT_INFO_VERSION;
	query->struct_size = sizeof(*query);
	query->capacity = KERN_MOUNT_INFO_MAX;
	descriptor = open("/dev/system", O_RDONLY);
	if (descriptor < 0) {
		command_error("df", "/dev/system");
		free(query);
		return NULL;
	}

	/* Reads the table. */
	result = ioctl(descriptor, KERN_SYSTEM_GET_MOUNTS, query);
	error = errno;
	close(descriptor);
	if (result < 0) {
		errno = error;
		command_error("df", "mount table");
		free(query);
		return NULL;
	}

	/* Names each file system and notes the device of its mount point. */
	mounts = calloc(query->count + 1, sizeof(*mounts));
	if (mounts == NULL) {
		fprintf(stderr, "df: out of memory\n");
		free(query);
		return NULL;
	}

	/* Each file system. */
	for (index = 0; index < query->count; index++) {
		entry = &query->entries[index];
		if (entry->device != 0 && (entry->kind & KERN_MOUNT_INFO_BIND) == 0)
			snprintf(mounts[index].name, sizeof(mounts[index].name), "/dev/%.*s", (int)sizeof(entry->source), entry->source);
		else if (entry->source[0] != '\0')
			snprintf(mounts[index].name, sizeof(mounts[index].name), "%.*s", (int)sizeof(entry->source), entry->source);
		else
			snprintf(mounts[index].name, sizeof(mounts[index].name), "%.*s", (int)sizeof(entry->type), entry->type);
		snprintf(mounts[index].target, sizeof(mounts[index].target), "%.*s", (int)sizeof(entry->target), entry->target);
		result = stat(mounts[index].target, &status);
		if (result == 0) {
			mounts[index].device = status.st_dev;
			mounts[index].device_known = 1;
		}
	}

	/* Succeeded: the table. */
	*count = query->count;
	free(query);
	return mounts;
}

/*
 * Finds the file system a file is on: the last mounted one whose mount
 * point is on the same device, or for a device special file the file
 * system it holds.  Returns NULL with a diagnostic for a file that cannot
 * be examined; a file on no known file system is reported alone.
 */
static const struct df_mount *
find_mount(
	const struct df_mount *mounts,
	size_t count,
	const char *path)
{
	static struct df_mount alone;
	const struct df_mount *found;
	struct stat status;
	size_t index;
	int result;
	int special;

	/* The file must exist. */
	result = stat(path, &status);
	if (result != 0) {
		command_error("df", path);
		return NULL;
	}

	/* A device special file stands for the file system it holds. */
	special = S_ISBLK(status.st_mode) || S_ISCHR(status.st_mode);
	if (special) {
		found = find_device(mounts, count, &status);
		if (found != NULL)
			return found;
	}

	/* The last mount on the file's device is the one over it. */
	found = NULL;
	for (index = 0; index < count; index++) {
		if (mounts[index].device_known && mounts[index].device == status.st_dev)
			found = &mounts[index];
	}

	/* A file on none of them. */
	if (found != NULL)
		return found;

	/* Without a mount table the file is reported by itself. */
	memset(&alone, 0, sizeof(alone));
	snprintf(alone.name, sizeof(alone.name), "-");
	snprintf(alone.target, sizeof(alone.target), "%s", path);
	return &alone;
}

/*
 * Finds the file system mounted from a device special file, by the name
 * the file system is known by.
 */
static const struct df_mount *
find_device(
	const struct df_mount *mounts,
	size_t count,
	const struct stat *status)
{
	struct stat named;
	size_t index;
	int result;

	/* A name that is the same special file. */
	for (index = 0; index < count; index++) {
		result = stat(mounts[index].name, &named);
		if (result != 0)
			continue;
		if (named.st_rdev == status->st_rdev && named.st_mode == status->st_mode)
			return &mounts[index];
	}

	/* Not mounted. */
	return NULL;
}

/*
 * Writes the line of one file system, taking its figures from a path on
 * it.  Returns -1 with a diagnostic when they cannot be had.
 */
static int
report(
	const char *name,
	const char *figures_path,
	const char *mount_point,
	unsigned long long unit)
{
	struct statvfs figures;
	unsigned long long total;
	unsigned long long free_space;
	unsigned long long available;
	unsigned long long used;
	unsigned percent;
	int result;
	int error;

	/* The figures. */
	result = statvfs(figures_path, &figures);
	if (result != 0) {
		command_error("df", figures_path);
		return -1;
	}

	/* Counters that do not add up cannot be reported. */
	error = 0;
	if (figures.f_frsize == 0)
		error = EIO;
	if (figures.f_bfree > figures.f_blocks)
		error = EIO;
	if (figures.f_bavail > figures.f_bfree)
		error = EIO;
	if (error == 0)
		error = units(figures.f_blocks, figures.f_frsize, unit, &total);
	if (error == 0)
		error = units(figures.f_bfree, figures.f_frsize, unit, &free_space);
	if (error == 0)
		error = units(figures.f_bavail, figures.f_frsize, unit, &available);
	if (error != 0) {
		errno = error;
		command_error("df", figures_path);
		return -1;
	}

	/* Used is what is not free; the capacity is rounded up. */
	used = total - free_space;
	percent = capacity(used, available);
	printf("%-16s %10llu %10llu %10llu %7u%% %s\n", name, total, used, available, percent, mount_point);
	return 0;
}

/*
 * Converts blocks of one size into whole units of another without
 * overflowing a product whose quotient would fit.
 */
static int
units(
	unsigned long long blocks,
	unsigned long long fragment,
	unsigned long long unit,
	unsigned long long *converted)
{
	unsigned long long whole;
	unsigned long long remainder;
	unsigned long long high;
	unsigned long long low;
	unsigned long long tail;

	/* The unit is 512 or 1024, which bounds the remainder product. */
	whole = fragment / unit;
	remainder = fragment % unit;
	if (whole != 0 && blocks > ULLONG_MAX / whole)
		return EOVERFLOW;
	high = blocks * whole;
	low = (blocks / unit) * remainder;
	tail = ((blocks % unit) * remainder) / unit;
	if (low > ULLONG_MAX - high)
		return EOVERFLOW;
	high += low;
	if (tail > ULLONG_MAX - high)
		return EOVERFLOW;
	*converted = high + tail;

	/* Succeeded: the number of whole units. */
	return 0;
}

/* Finds the percentage in use, rounded up, without multiplying by 100. */
static unsigned
capacity(
	unsigned long long used,
	unsigned long long available)
{
	unsigned long long denominator;
	unsigned long long threshold;
	unsigned percent;

	/* Each threshold is floor(denominator * percent / 100), without wrapping. */
	denominator = used + available;
	for (percent = 0; percent < 100; percent++) {
		threshold = (denominator / 100) * percent;
		threshold += ((denominator % 100) * percent) / 100;
		if (used <= threshold)
			return percent;
	}

	/* What is left rounds up to 100 percent. */
	return 100;
}

/* Writes the usage message and exits. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: df [-k] [-P|-t] [file...]\n");
	exit(1);
}
