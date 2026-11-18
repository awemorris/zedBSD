/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check of a read-only root by init (BUG-312).
 *
 * The kernel replays the root's journal and mounts the root read-write.
 * It mounts the root read-only only when the volume refuses to be written
 * but can still be read; its log ("ufs:" and "vfs:" lines) says why.
 * Before anything else, init then runs /sbin/fsck -y on the root's device.
 * A volume fsck repaired is clean, and the machine is rebooted so that the
 * kernel mounts it read-write again, as FreeBSD's rc does after a repair of
 * the root.  When fsck finds nothing to repair, cannot repair the volume,
 * or is missing, the root stays read-only and the console says why and
 * what to do.
 */

#include "root-check.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#include <uapi/mountinfo.h>
#include <uapi/system.h>

/* The longest device path init builds for fsck: "/dev/" and a disk's name. */
#define ROOT_CHECK_DEVICE_MAX	(KERN_MOUNT_INFO_PATH_MAX + 8U)

static int root_check_find(char *device, size_t size, int *read_only);
static int root_check_run_fsck(const char *device);
static void root_check_stays_read_only(const char *device, const char *why);
static void root_check_reboot(void);

/*
 * Checks the root when the kernel mounted it read-only.
 *
 * A writable root, a root that is not UFS, or a mount table that cannot be
 * read is left alone.  After a repair the machine reboots and this does not
 * return.
 */
void
root_check(
	void)
{
	char device[ROOT_CHECK_DEVICE_MAX];
	int read_only;
	int status;
	int error;

	/* The root's device and whether it is read-only. */
	error = root_check_find(device, sizeof(device), &read_only);
	if (error != 0)
		return;
	if (!read_only)
		return;

	/* The console says what happens. */
	printf("init: the root file system on %s is mounted READ-ONLY: the kernel could not mount it read-write after a crash\n", device);
	printf("init: checking %s with %s -y\n", device, ROOT_CHECK_FSCK_PATH);
	(void)fflush(stdout);

	/* fsck repairs the volume, or says it cannot. */
	status = root_check_run_fsck(device);

	/* A repaired volume is mounted read-write by the next boot. */
	if (status == ROOT_CHECK_FSCK_REPAIRED) {
		printf("init: %s was repaired; rebooting so that the kernel mounts it read-write\n", device);
		(void)fflush(stdout);
		root_check_reboot();
		return;
	}

	/* A clean volume the kernel still refused, a volume fsck cannot repair, or no fsck at all. */
	if (status == ROOT_CHECK_FSCK_CLEAN) {
		root_check_stays_read_only(device, "fsck found nothing to repair, so the kernel refused it for another reason (an I/O error?)");
	} else if (status < 0) {
		root_check_stays_read_only(device, "fsck could not be run");
	} else {
		root_check_stays_read_only(device, "fsck could not repair it");
	}
}

/*
 * Finds the root in the kernel's mount table: the device fsck is given
 * ("/dev/" and the disk's name) and whether it is read-only.  Returns 0,
 * ENOENT for a root that is not a UFS volume on a disk, or the table's
 * error.
 */
static int
root_check_find(
	char *device,
	size_t size,
	int *read_only)
{
	struct kern_mount_query *query;
	const struct kern_mount_info *entry;
	uint32_t index;
	int descriptor;
	int found;
	int matches;
	int status;
	int error;

	/* The table, as mount lists it. */
	*read_only = 0;
	query = calloc(1, sizeof(*query) + KERN_MOUNT_INFO_MAX * sizeof(struct kern_mount_info));
	if (query == NULL)
		return ENOMEM;
	query->version = KERN_MOUNT_INFO_VERSION;
	query->struct_size = sizeof(*query);
	query->capacity = KERN_MOUNT_INFO_MAX;
	descriptor = open("/dev/system", O_RDONLY);
	if (descriptor < 0) {
		error = errno;
		free(query);
		return error;
	}

	/* The kernel fills it. */
	error = 0;
	status = ioctl(descriptor, KERN_SYSTEM_GET_MOUNTS, query);
	if (status < 0)
		error = errno;
	(void)close(descriptor);
	if (error != 0) {
		free(query);
		return error;
	}

	/* The root: mounted at "/", a UFS volume on a disk. */
	error = ENOENT;
	for (index = 0; index < query->count; index++) {
		entry = &query->entries[index];
		found = strcmp(entry->target, "/") == 0;
		if (!found)
			continue;
		matches = strcmp(entry->type, "ufs") == 0;
		if (!matches || entry->device == 0 || entry->source[0] == '\0')
			break;
		(void)snprintf(device, size, "/dev/%s", entry->source);
		if ((entry->flags & MNT_RDONLY) != 0)
			*read_only = 1;
		error = 0;
		break;
	}

	/* The table is no longer needed. */
	free(query);

	/* Reports a root that is not a UFS volume on a disk. */
	if (error != 0)
		return error;

	/* Succeeded: the root's device. */
	return 0;
}

/*
 * Runs fsck -y on a device and waits for it.  Returns its exit status, or
 * -1 when it could not be run or did not exit.
 */
static int
root_check_run_fsck(
	const char *device)
{
	char *arguments[4];
	pid_t child;
	pid_t waited;
	int status;
	int exited;
	int code;

	/* The child runs fsck. */
	child = fork();
	if (child < 0) {
		fprintf(stderr, "init: cannot start %s: %s\n", ROOT_CHECK_FSCK_PATH, strerror(errno));
		return -1;
	}

	/* The child runs fsck, or says why it could not. */
	if (child == 0) {
		arguments[0] = "fsck";
		arguments[1] = "-y";
		arguments[2] = (char *)device;
		arguments[3] = NULL;
		(void)execv(ROOT_CHECK_FSCK_PATH, arguments);
		fprintf(stderr, "init: cannot run %s: %s\n", ROOT_CHECK_FSCK_PATH, strerror(errno));
		_exit(127);
	}

	/* Waits for that child alone. */
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != child)
		return -1;
	exited = WIFEXITED(status);
	if (!exited)
		return -1;
	code = WEXITSTATUS(status);
	if (code == 127)
		return -1;

	/* Succeeded: fsck's answer. */
	return code;
}

/* Tells the console that the root stays read-only, why, and how to recover. */
static void
root_check_stays_read_only(
	const char *device,
	const char *why)
{
	/* Why, then what to do. */
	printf("init: the root file system stays READ-ONLY: %s\n", why);
	printf("init: to recover, log in as root and see why the kernel refused it: dmesg | grep -e ufs: -e vfs:\n");
	printf("init: then run %s -y %s and reboot; if it cannot be repaired, copy your files off and write the image again\n", ROOT_CHECK_FSCK_PATH, device);
	(void)fflush(stdout);
}

/*
 * Reboots the machine: fsck wrote its repairs to the device and made them
 * durable, and no service runs yet.  Returns only when the kernel refused,
 * and the boot goes on with the read-only root.
 */
static void
root_check_reboot(
	void)
{
	int descriptor;

	/* Writes anything left out, then asks the kernel to reboot. */
	sync();
	descriptor = open("/dev/system", O_RDONLY);
	if (descriptor >= 0) {
		(void)ioctl(descriptor, KERN_SYSTEM_REBOOT);
		(void)close(descriptor);
	}

	/* Succeeded in learning that the kernel did not reboot. */
	fprintf(stderr, "init: the reboot was refused: %s; the boot goes on with the root READ-ONLY\n", strerror(errno));
}
