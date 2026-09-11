/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/quota.h>
#include <sys/snapshot.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/sysctl.h>
#include <zedbsd/writeback.h>
#include <unistd.h>

static unsigned checks;
static void require(int condition, const char *message);
static void mount_volume(const char *device, const char *path, int flags);
static void quota_request(struct quota_control *q, unsigned command, unsigned type);
static void verify(void);
static void quota_limits(void);
static void rename_setup(void);
static void rename_story(void);
static void rename_verify(void);
static void writeback_control(unsigned enabled)
{
	struct writeback_control request;
	memset(&request, 0, sizeof(request));
	request.version = WRITEBACK_REPORT_VERSION;
	request.enabled = enabled;
	strcpy(request.path, "/ufstest");
	require(sysctlbyname("vfs.writeback.control", NULL, NULL,
	    &request, sizeof(request)) == 0, "writeback policy control");
}

/* Exercises real mounted UFS features and repeats their persistent state checks. */
int
main(int argc, char **argv)
{
	struct quota_control q;
	struct snapshot_control snapshot;
	struct stat parent_before, parent_after, removed_state;
	char device[32];
	char buffer[64];
	int fd, deferred;
	unsigned type;

	/* Mount the disposable secondary device through the public syscall. */
	require(argc == 2, "mode argument");
	mount_volume("nvme0n1", "/ufstest", 0);
	if (strcmp(argv[1], "verify") == 0) {
		verify();
		require(unmount("/ufstest", 0) == 0, "verify unmount");
		printf("UFS FEATURES VERIFY PASS %u\n", checks);
		return 0;
	}
	deferred = strcmp(argv[1], "run-deferred") == 0;
	require(strcmp(argv[1], "run") == 0 || deferred, "known mode");
	if (deferred)
		writeback_control(1);

	/* Preserve file identity across link, rename and unlink with an open fd. */
	fd = open("/ufstest/original", O_CREAT | O_EXCL | O_RDWR, 0644);
	require(fd >= 0, "create file");
	require(write(fd, "before", 6) == 6, "initial content");
	require(link("/ufstest/original", "/ufstest/alias") == 0, "hard link");
	require(rename("/ufstest/original", "/ufstest/renamed") == 0, "rename");
	require(unlink("/ufstest/alias") == 0, "unlink alias");
	require(pread(fd, buffer, 6, 0) == 6 && memcmp(buffer, "before", 6) == 0,
	    "open identity survives namespace changes");

	/* Check create/replace/remove and short-buffer xattr contracts. */
	require(fsetxattr(fd, "user.ws024", "before", 6, XATTR_CREATE) == 0, "xattr create");
	errno = 0;
	require(fsetxattr(fd, "user.ws024", "bad", 3, XATTR_CREATE) == -1 && errno == EEXIST,
	    "duplicate xattr refusal");
	require(fgetxattr(fd, "user.ws024", NULL, 0) == 6, "xattr size");
	errno = 0;
	require(fgetxattr(fd, "user.ws024", buffer, 2) == -1 && errno == ERANGE,
	    "short xattr buffer");
	errno = 0;
	require(fsetxattr(fd, "user.toobig", buffer, 65537U, 0) == -1 && errno == EINVAL,
	    "oversized xattr rejected before copy");
	errno = 0;
	require(fsetxattr(fd, "user.", "x", 1, 0) == -1 && errno == EOPNOTSUPP,
	    "empty xattr name refused");
	require(fsetxattr(fd, "user.temporary", "x", 1, 0) == 0, "temporary xattr");
	require(fremovexattr(fd, "user.temporary") == 0, "remove xattr");
	require(fsync(fd) == 0 && close(fd) == 0, "sync initial file");

	/* Persist independent user and group limit configurations. */
	for (type = 0; type < 2; type++) {
		quota_request(&q, ZEDBSD_QUOTA_SET, type);
		q.id = 1234;
		q.block_soft = 12 + type;
		q.block_hard = 20 + type;
		q.inode_soft = 8 + type;
		q.inode_hard = 10 + type;
		require(quotactl("/ufstest", &q) == 0, "set quota");
		quota_request(&q, ZEDBSD_QUOTA_ENABLE, type);
		require(quotactl("/ufstest", &q) == 0, "enable quota");
	}

	quota_limits();
	rename_setup();

	/* Refuses a nonempty child, then captures an empty directory before removal. */
	require(stat("/ufstest", &parent_before) == 0, "parent before directory creation");
	require(mkdir("/ufstest/removed-directory", 0755) == 0, "create removable directory");
	fd = open("/ufstest/removed-directory/child", O_CREAT | O_EXCL | O_WRONLY, 0644);
	require(fd >= 0, "create child preventing rmdir");
	require(close(fd) == 0, "close child before rmdir refusal");
	errno = 0;
	require(rmdir("/ufstest/removed-directory") == -1 && errno == ENOTEMPTY, "nonempty rmdir refusal");
	require(unlink("/ufstest/removed-directory/child") == 0, "remove child before snapshot");

	/* Capture an old view and require the active snapshot to hold the mount. */
	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.size = sizeof(snapshot);
	snapshot.version = ZEDBSD_SNAPSHOT_VERSION;
	snapshot.command = ZEDBSD_SNAPSHOT_CREATE;
	require(snapshotctl("/ufstest", &snapshot) == 0, "create snapshot");
	require(snapshot.flags & ZEDBSD_SNAPSHOT_F_ACTIVE, "snapshot active");
	memcpy(device, snapshot.device, sizeof(snapshot.device));
	device[sizeof(snapshot.device)] = 0;
	errno = 0;
	require(unmount("/ufstest", 0) == -1 && errno == EBUSY, "snapshot prevents unmount");

	/* Removes the captured directory and restores the parent's original link count. */
	require(rmdir("/ufstest/removed-directory") == 0, "journal rmdir under snapshot");
	require(stat("/ufstest", &parent_after) == 0 && parent_after.st_nlink == parent_before.st_nlink,
	    "rmdir restores parent link count");
	errno = 0;
	require(stat("/ufstest/removed-directory", &removed_state) == -1 && errno == ENOENT,
	    "removed directory absent from live namespace");

	rename_story();

	/* Modify both data and attributes after the snapshot boundary. */
	fd = open("/ufstest/renamed", O_RDWR);
	require(fd >= 0, "open renamed file");
	require(pwrite(fd, "after!", 6, 0) == 6, "overwrite live data");
	require(fsetxattr(fd, "user.ws024", "after!", 6, XATTR_REPLACE) == 0, "replace live xattr");
	require(fsync(fd) == 0 && close(fd) == 0, "sync changed file");
	mount_volume(device, "/ufssnap", MNT_RDONLY);
	require(stat("/ufssnap/removed-directory", &removed_state) == 0 &&
	    S_ISDIR(removed_state.st_mode) && removed_state.st_nlink == 2,
	    "snapshot retains removed directory and link count");
	require(stat("/ufssnap/move-left/child", &removed_state) == 0, "snapshot retains moved source");
	require(stat("/ufssnap/move-right/child", &removed_state) == 0, "snapshot retains replaced directory");
	fd = open("/ufssnap/move-right/file", O_RDONLY);
	require(fd >= 0, "snapshot retired victim");
	require(fgetxattr(fd, "user.retirement", buffer, sizeof(buffer)) == 5 &&
	    memcmp(buffer, "owned", 5) == 0, "snapshot retains released xattr");
	require(close(fd) == 0, "close snapshot retired victim");
	fd = open("/ufssnap/renamed", O_RDONLY);
	require(fd >= 0, "snapshot file");
	require(read(fd, buffer, 6) == 6 && memcmp(buffer, "before", 6) == 0, "snapshot old data");
	require(fgetxattr(fd, "user.ws024", buffer, sizeof(buffer)) == 6 &&
	    memcmp(buffer, "before", 6) == 0, "snapshot old xattr");
	require(close(fd) == 0, "close snapshot file");
	require(unmount("/ufssnap", 0) == 0, "unmount snapshot view");
	snapshot.command = ZEDBSD_SNAPSHOT_DELETE;
	require(snapshotctl("/ufstest", &snapshot) == 0, "delete snapshot");
	if (deferred) {
		require(mkdir("/ufstest/policy-boundary", 0700) == 0, "pending policy directory");
		writeback_control(0);
		require(stat("/ufstest/policy-boundary", &removed_state) == 0, "policy off retains namespace");
		writeback_control(1);
		require(rmdir("/ufstest/policy-boundary") == 0, "pending unmount removal");
	}

	/* Reload the filesystem to exercise quota and xattr decoding from storage. */
	require(unmount("/ufstest", 0) == 0, "unmount live volume");
	mount_volume("nvme0n1", "/ufstest", 0);
	verify();
	require(unmount("/ufstest", 0) == 0, "final unmount");
	printf("UFS FEATURES RUN PASS %u\n", checks);
	return 0;
}

/* Stops with the exact failing syscall context. */
static void
require(int condition, const char *message)
{
	checks++;
	if (!condition) {
		printf("UFS FEATURES FAIL %u %s errno=%d\n", checks, message, errno);
		exit(1);
	}
}

/* Mounts a disposable block device at an explicit test directory. */
static void
mount_volume(const char *device, const char *path, int flags)
{
	struct mount_args args;

	/* Create the mountpoint and pass the versioned device identity. */
	if (mkdir(path, 0755) != 0)
		require(errno == EEXIST, "mkdir mountpoint");
	memset(&args, 0, sizeof(args));
	args.size = sizeof(args);
	args.version = ZEDBSD_MOUNT_ARGS_VERSION;
	require(strlen(device) < sizeof(args.fspec), "device name length");
	strcpy(args.fspec, device);
	require(mount("ufs", path, flags, &args) == 0, "mount UFS");
}

/* Initializes one versioned quota operation. */
static void
quota_request(struct quota_control *q, unsigned command, unsigned type)
{
	memset(q, 0, sizeof(*q));
	q->size = sizeof(*q);
	q->version = ZEDBSD_QUOTA_VERSION;
	q->command = command;
	q->type = type;
}

/* Checks data, xattrs and both quota classes after remount or reboot. */
static void
verify(void)
{
	struct quota_control q;
	struct stat removed_state;
	char buffer[64];
	int fd;
	unsigned type;

	rename_verify();
	/* Keeps the committed directory removal across remount and reboot. */
	errno = 0;
	require(stat("/ufstest/removed-directory", &removed_state) == -1 && errno == ENOENT,
	    "directory removal persists");

	/* Check persisted file identity, bytes and the replacement attribute. */
	fd = open("/ufstest/renamed", O_RDONLY);
	require(fd >= 0, "verify file");
	require(read(fd, buffer, 6) == 6 && memcmp(buffer, "after!", 6) == 0, "persisted data");
	require(fgetxattr(fd, "user.ws024", buffer, sizeof(buffer)) == 6 &&
	    memcmp(buffer, "after!", 6) == 0, "persisted xattr");
	require(close(fd) == 0, "verify close");

	/* Reload each independent quota configuration from system.zedbsd.quota. */
	for (type = 0; type < 2; type++) {
		quota_request(&q, ZEDBSD_QUOTA_GET, type);
		q.id = 1234;
		require(quotactl("/ufstest", &q) == 0, "get persisted quota");
		require(q.block_soft == 12 + type && q.block_hard == 20 + type &&
		    q.inode_soft == 8 + type && q.inode_hard == 10 + type,
		    "persistent quota limits");
		require(q.flags & ZEDBSD_QUOTA_F_ENABLED, "persistent quota enabled");
		require(q.blocks == 0 && q.inodes == 0, "released quota accounting survives remount");
	}
}

/* Enforces hard limits and returns charges after deleting the owned file. */
static void
quota_limits(void)
{
	static char block[8192];
	struct quota_control q;
	unsigned index;
	unsigned type;
	ssize_t count;
	int fd;
	int refused;

	/* Transfer a newly created file into the configured quota identities. */
	fd = open("/ufstest/limited", O_CREAT | O_EXCL | O_RDWR, 0644);
	require(fd >= 0, "create quota-owned file");
	require(fchown(fd, 1234, 1234) == 0, "transfer quota ownership");
	memset(block, 0x5a, sizeof(block));
	refused = 0;

	/* Include indirect metadata in the charge and stop at the hard limit. */
	for (index = 0; index < 24; index++) {
		errno = 0;
		count = write(fd, block, sizeof(block));
		if (count < 0) {
			require(errno == EDQUOT, "hard block limit returns EDQUOT");
			refused = 1;
			break;
		}
		require(count == sizeof(block), "complete quota-bounded block");
	}
	require(refused && index > 0, "quota accepts initial blocks then refuses growth");

	/* Inspect both accounting classes before freeing all data and metadata. */
	for (type = 0; type < 2; type++) {
		quota_request(&q, ZEDBSD_QUOTA_GET, type);
		q.id = 1234;
		require(quotactl("/ufstest", &q) == 0, "get charged quota");
		require(q.blocks <= q.block_hard && q.blocks > 0 && q.inodes == 1,
		    "quota charge remains bounded after refusal");
	}
	require(close(fd) == 0, "close quota-owned file");
	require(unlink("/ufstest/limited") == 0, "release quota-owned file");
}

/* Creates both parents and replacement victims before the snapshot boundary. */
static void
rename_setup(void)
{
	int fd;

	require(mkdir("/ufstest/move-left", 0755) == 0, "rename left parent");
	require(mkdir("/ufstest/move-right", 0755) == 0, "rename right parent");
	require(mkdir("/ufstest/move-left/child", 0755) == 0, "rename source directory");
	require(mkdir("/ufstest/move-right/child", 0755) == 0, "rename replacement directory");
	fd = open("/ufstest/move-left/file", O_CREAT | O_EXCL | O_WRONLY, 0644);
	require(fd >= 0, "rename source file");
	require(write(fd, "source", 6) == 6, "rename source content");
	require(close(fd) == 0, "close rename source");
	fd = open("/ufstest/move-right/file", O_CREAT | O_EXCL | O_WRONLY, 0644);
	require(fd >= 0, "rename replacement file");
	require(write(fd, "victim", 6) == 6, "rename victim content");
	require(fsetxattr(fd, "user.retirement", "owned", 5, 0) == 0, "victim xattr before final release");
	require(close(fd) == 0, "close rename victim");
}

/* Replaces directories and files while retaining old open-file identity. */
static void
rename_story(void)
{
	struct stat before, after;
	char buffer[6];
	int fd;

	require(stat("/ufstest/move-left/child/..", &before) == 0, "cache old dotdot");
	require(rename("/ufstest/move-left/child", "/ufstest/move-right/child") == 0,
	    "cross-parent directory replacement");
	require(stat("/ufstest/move-right/child/..", &after) == 0 && after.st_ino != before.st_ino,
	    "moved directory has new cached parent");
	fd = open("/ufstest/move-right/file", O_RDONLY);
	require(fd >= 0, "open replacement victim");
	require(rename("/ufstest/move-left/file", "/ufstest/move-right/file") == 0,
	    "cross-parent file replacement");
	require(read(fd, buffer, sizeof(buffer)) == 6 && memcmp(buffer, "victim", 6) == 0,
	    "replaced open inode retains content");
	require(close(fd) == 0, "close replaced inode");
	require(rename("/ufstest/move-right/file", "/ufstest/move-right/final") == 0,
	    "same-parent rename after replacement");
	rename_verify();
}

/* Checks the complete rename result again after remount and reboot. */
static void
rename_verify(void)
{
	struct stat left, right, parent;
	char buffer[6];
	int fd;

	require(stat("/ufstest/move-left", &left) == 0 && left.st_nlink == 2,
	    "rename old parent link count");
	require(stat("/ufstest/move-right", &right) == 0 && right.st_nlink == 3,
	    "rename new parent link count");
	require(stat("/ufstest/move-right/child/..", &parent) == 0 && parent.st_ino == right.st_ino,
	    "rename persistent dotdot");
	errno = 0;
	require(stat("/ufstest/move-left/child", &parent) == -1 && errno == ENOENT,
	    "rename old directory absent");
	errno = 0;
	require(stat("/ufstest/move-left/file", &parent) == -1 && errno == ENOENT,
	    "rename old file absent");
	fd = open("/ufstest/move-right/final", O_RDONLY);
	require(fd >= 0, "rename final name persists");
	require(read(fd, buffer, sizeof(buffer)) == 6 && memcmp(buffer, "source", 6) == 0,
	    "rename replacement content persists");
	require(close(fd) == 0, "close rename verification");
}
