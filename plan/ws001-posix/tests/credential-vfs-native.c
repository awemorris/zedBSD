/*
 * WS001 p015 effective-credential filesystem creation probe.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */

#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_UID ((uid_t)123)
#define TEST_GID ((gid_t)456)
#define PARENT_GID ((gid_t)222)

#define STATE_DIRECTORY "/p015-state"
#define STAGE_MARKER STATE_DIRECTORY "/stage1"
#define EXTERNAL_MOUNT "/p015-external"

static int
failure(const char *stage)
{
	printf("WS001-P015 FAIL stage=%s errno=%d\n", stage, errno);
	fflush(stdout);
	return 1;
}

static int
join_path(char *output, size_t capacity, const char *directory,
	  const char *name)
{
	int count = snprintf(output, capacity, "%s/%s", directory, name);

	if (count < 0 || (size_t)count >= capacity) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int
write_file(const char *path, const char *contents, mode_t mode)
{
	size_t length = strlen(contents), offset = 0;
	int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
	    mode);

	if (descriptor < 0)
		return -1;
	while (offset < length) {
		ssize_t count = write(descriptor, contents + offset,
		    length - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			int saved = count == 0 ? EIO : errno;
			(void)close(descriptor);
			errno = saved;
			return -1;
		}
		offset += (size_t)count;
	}
	return close(descriptor);
}

static int
read_word(const char *path, char *buffer, size_t capacity)
{
	ssize_t count;
	int descriptor;

	if (capacity < 2U) {
		errno = EINVAL;
		return -1;
	}
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	count = read(descriptor, buffer, capacity - 1U);
	if (count < 0) {
		int saved = errno;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}
	if (close(descriptor) != 0)
		return -1;
	buffer[count] = '\0';
	while (count > 0 && (buffer[count - 1] == '\n' ||
	    buffer[count - 1] == '\r'))
		buffer[--count] = '\0';
	return count > 0 ? 0 : -1;
}

static int
wait_child(pid_t child)
{
	int status;
	pid_t result;

	do {
		result = waitpid(child, &status, 0);
	} while (result < 0 && errno == EINTR);
	return result == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int
drop_credentials(void)
{
	return setgroups(0, NULL) == 0 && setgid(TEST_GID) == 0 &&
	    setuid(TEST_UID) == 0 ? 0 : -1;
}

static int
create_objects_child(const char *directory, int supports_hardlink,
	int supports_socket)
{
	struct sockaddr_un address;
	struct stat before, after;
	int directory_descriptor, descriptor, socket_descriptor;

	if (drop_credentials() != 0)
		return 10;
	(void)umask(0027U);
	directory_descriptor = open(directory,
	    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory_descriptor < 0)
		return 11;
	descriptor = openat(directory_descriptor, "regular",
	    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666U);
	if (descriptor < 0 || write(descriptor, "x", 1U) != 1 ||
	    close(descriptor) != 0)
		return 12;
	if (mkdirat(directory_descriptor, "directory", 0777U) != 0)
		return 13;
	if (mkfifoat(directory_descriptor, "fifo", 0666U) != 0)
		return 14;
	if (symlinkat("target", directory_descriptor, "symlink") != 0)
		return 15;
	if (fstatat(directory_descriptor, "regular", &before, 0) != 0)
		return 16;
	errno = 0;
	descriptor = openat(directory_descriptor, "regular",
	    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600U);
	if (descriptor >= 0 || errno != EEXIST)
		return 17;
	if (fstatat(directory_descriptor, "regular", &after, 0) != 0 ||
	    before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
	    before.st_mode != after.st_mode || before.st_uid != after.st_uid ||
	    before.st_gid != after.st_gid || before.st_nlink != after.st_nlink ||
	    before.st_size != after.st_size)
		return 18;
	errno = 0;
	if (supports_hardlink) {
		if (linkat(directory_descriptor, "regular", directory_descriptor,
		    "hardlink", 0) != 0)
			return 19;
	} else if (linkat(directory_descriptor, "regular", directory_descriptor,
	    "hardlink", 0) != -1 || errno != EOPNOTSUPP) {
		return 19;
	}
	if (close(directory_descriptor) != 0 || chdir(directory) != 0)
		return 20;
	socket_descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (socket_descriptor < 0)
		return 21;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, "socket");
	errno = 0;
	if (supports_socket) {
		if (bind(socket_descriptor, (struct sockaddr *)&address,
		    sizeof(address)) != 0 || close(socket_descriptor) != 0)
			return 22;
	} else {
		if (bind(socket_descriptor, (struct sockaddr *)&address,
		    sizeof(address)) != -1 || errno != EOPNOTSUPP) {
			(void)close(socket_descriptor);
			return 22;
		}
		if (close(socket_descriptor) != 0)
			return 22;
	}
	return 0;
}

static int
expect_object(const char *directory, const char *name, mode_t type,
	mode_t permissions, uid_t uid, gid_t gid, struct stat *result)
{
	char path[256];
	struct stat status;

	if (join_path(path, sizeof(path), directory, name) != 0 ||
	    lstat(path, &status) != 0)
		return -1;
	if ((status.st_mode & S_IFMT) != type ||
	    (status.st_mode & 07777U) != permissions || status.st_uid != uid ||
	    status.st_gid != gid) {
		errno = EINVAL;
		return -1;
	}
	if (result != NULL)
		*result = status;
	return 0;
}

static int
validate_objects(const char *directory, gid_t gid, int setgid_parent,
	int supports_hardlink, int supports_socket)
{
	char path[256];
	struct stat regular, hardlink;
	mode_t directory_mode = 0750U | (setgid_parent ? S_ISGID : 0U);

	if (expect_object(directory, "regular", S_IFREG, 0640U, TEST_UID,
	    gid, &regular) != 0 ||
	    expect_object(directory, "directory", S_IFDIR, directory_mode,
	    TEST_UID, gid, NULL) != 0 ||
	    expect_object(directory, "fifo", S_IFIFO, 0640U, TEST_UID, gid,
	    NULL) != 0 ||
	    expect_object(directory, "symlink", S_IFLNK, 0777U, TEST_UID,
	    gid, NULL) != 0)
		return -1;
	if (supports_hardlink) {
		if (expect_object(directory, "hardlink", S_IFREG, 0640U,
		    TEST_UID, gid, &hardlink) != 0 ||
		    regular.st_dev != hardlink.st_dev ||
		    regular.st_ino != hardlink.st_ino || regular.st_nlink != 2U ||
		    hardlink.st_nlink != 2U) {
			errno = EINVAL;
			return -1;
		}
	} else {
		if (regular.st_nlink != 1U)
			return -1;
		if (join_path(path, sizeof(path), directory, "hardlink") != 0)
			return -1;
		errno = 0;
		if (lstat(path, &hardlink) != -1 || errno != ENOENT)
			return -1;
	}
	if (supports_socket) {
		if (expect_object(directory, "socket", S_IFSOCK, 0750U,
		    TEST_UID, gid, NULL) != 0)
			return -1;
	} else {
		if (join_path(path, sizeof(path), directory, "socket") != 0)
			return -1;
		errno = 0;
		if (lstat(path, &hardlink) != -1 || errno != ENOENT)
			return -1;
	}
	return 0;
}

static int
create_object_set(const char *directory, uid_t owner, gid_t group,
	mode_t mode, gid_t expected_group, int setgid_parent,
	int supports_hardlink, int supports_socket)
{
	pid_t child;

	if (mkdir(directory, 0700U) != 0 || chown(directory, owner, group) != 0 ||
	    chmod(directory, mode) != 0)
		return -1;
	child = fork();
	if (child < 0)
		return -1;
	if (child == 0)
		_exit(create_objects_child(directory, supports_hardlink,
		    supports_socket));
	if (wait_child(child) != 0) {
		errno = EIO;
		return -1;
	}
	return validate_objects(directory, expected_group, setgid_parent,
	    supports_hardlink, supports_socket);
}

static int
permission_denial_child(const char *directory)
{
	char path[256];
	int descriptor;

	if (drop_credentials() != 0 ||
	    join_path(path, sizeof(path), directory, "must-not-exist") != 0)
		return 30;
	errno = 0;
	descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600U);
	if (descriptor >= 0 || errno != EACCES)
		return 31;
	errno = 0;
	if (lstat(path, &(struct stat){0}) != -1 || errno != ENOENT)
		return 32;
	return 0;
}

static int
run_suite(const char *base, int supports_hardlink, int supports_socket)
{
	char ordinary[256], inherited[256], denied[256];
	pid_t child;

	if (mkdir(base, 0755U) != 0 ||
	    join_path(ordinary, sizeof(ordinary), base, "ordinary") != 0 ||
	    join_path(inherited, sizeof(inherited), base, "setgid") != 0 ||
	    join_path(denied, sizeof(denied), base, "denied") != 0 ||
	    create_object_set(ordinary, TEST_UID, TEST_GID, 0770U, TEST_GID,
	    0, supports_hardlink, supports_socket) != 0 ||
	    create_object_set(inherited, TEST_UID, PARENT_GID,
	    S_ISGID | 0770U, PARENT_GID, 1,
	    supports_hardlink, supports_socket) != 0 ||
	    mkdir(denied, 0555U) != 0 || chown(denied, 0, 0) != 0 ||
	    chmod(denied, 0555U) != 0)
		return -1;
	child = fork();
	if (child < 0)
		return -1;
	if (child == 0)
		_exit(permission_denial_child(denied));
	if (wait_child(child) != 0) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int
validate_suite(const char *base, int supports_hardlink, int supports_socket)
{
	char ordinary[256], inherited[256], denied_path[256];
	struct stat status;

	if (join_path(ordinary, sizeof(ordinary), base, "ordinary") != 0 ||
	    join_path(inherited, sizeof(inherited), base, "setgid") != 0 ||
	    join_path(denied_path, sizeof(denied_path), base,
	    "denied/must-not-exist") != 0 ||
	    validate_objects(ordinary, TEST_GID, 0,
	    supports_hardlink, supports_socket) != 0 ||
	    validate_objects(inherited, PARENT_GID, 1,
	    supports_hardlink, supports_socket) != 0)
		return -1;
	errno = 0;
	if (lstat(denied_path, &status) != -1 || errno != ENOENT)
		return -1;
	return 0;
}

static int
create_direct_nonroot(const char *path, mode_t mode)
{
	pid_t child = fork();

	if (child < 0)
		return -1;
	if (child == 0) {
		int descriptor;

		if (drop_credentials() != 0)
			_exit(40);
		(void)umask(0077U);
		descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
		    mode);
		if (descriptor < 0 || close(descriptor) != 0)
			_exit(41);
		_exit(0);
	}
	if (wait_child(child) != 0) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int
mount_external(const char *wanted_backend, char fspec[32])
{
	static const char *const candidates[] = {"sdb", "sdc", "sdd", "sda"};
	struct mount_args arguments;
	char marker[32];
	unsigned index;

	if (mkdir(EXTERNAL_MOUNT, 0755U) != 0 && errno != EEXIST)
		return -1;
	for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]);
	    index++) {
		memset(&arguments, 0, sizeof(arguments));
		arguments.size = sizeof(arguments);
		arguments.version = ZEDBSD_MOUNT_ARGS_VERSION;
		strcpy(arguments.fspec, candidates[index]);
		if (mount("auto", EXTERNAL_MOUNT, 0, &arguments) != 0)
			continue;
		if (read_word(EXTERNAL_MOUNT "/.p015-backend", marker,
		    sizeof(marker)) == 0 && strcmp(marker, wanted_backend) == 0) {
			strcpy(fspec, candidates[index]);
			return 0;
		}
		(void)unmount(EXTERNAL_MOUNT, 0);
	}
	errno = ENODEV;
	return -1;
}

static int
remount_external(const char *fspec)
{
	struct mount_args arguments;

	memset(&arguments, 0, sizeof(arguments));
	arguments.size = sizeof(arguments);
	arguments.version = ZEDBSD_MOUNT_ARGS_VERSION;
	strcpy(arguments.fspec, fspec);
	return mount("auto", EXTERNAL_MOUNT, 0, &arguments);
}

static int
fat_nonroot_rejection_child(void)
{
	struct sockaddr_un address;
	struct stat status;
	const char *const paths[] = {
	    EXTERNAL_MOUNT "/writable/user-file",
	    EXTERNAL_MOUNT "/writable/user-directory",
	    EXTERNAL_MOUNT "/writable/user-fifo",
	    EXTERNAL_MOUNT "/writable/user-symlink",
	    EXTERNAL_MOUNT "/writable/user-socket"};
	int descriptor, socket_descriptor;
	unsigned index;

	if (drop_credentials() != 0)
		return 50;
	(void)umask(0);
	errno = 0;
	descriptor = open(paths[0], O_WRONLY | O_CREAT | O_EXCL, 0755U);
	if (descriptor >= 0 || errno != EOPNOTSUPP)
		return 51;
	errno = 0;
	if (mkdir(paths[1], 0755U) != -1 || errno != EOPNOTSUPP)
		return 52;
	errno = 0;
	if (mkfifo(paths[2], 0600U) != -1 || errno != EOPNOTSUPP)
		return 53;
	errno = 0;
	if (symlink("target", paths[3]) != -1 || errno != EOPNOTSUPP)
		return 54;
	socket_descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (socket_descriptor < 0)
		return 55;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, paths[4]);
	errno = 0;
	if (bind(socket_descriptor, (struct sockaddr *)&address,
	    sizeof(address)) != -1 || errno != EOPNOTSUPP) {
		(void)close(socket_descriptor);
		return 56;
	}
	(void)close(socket_descriptor);
	for (index = 0; index < sizeof(paths) / sizeof(paths[0]); index++) {
		errno = 0;
		if (lstat(paths[index], &status) != -1 || errno != ENOENT)
			return 57;
	}
	return 0;
}

static int
run_fat(void)
{
	char fspec[32];
	struct stat status;
	pid_t child;
	int descriptor;

	if (mount_external("fat", fspec) != 0)
		return failure("fat-mount");
	(void)umask(0);
	descriptor = open(EXTERNAL_MOUNT "/root-file",
	    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0755U);
	if (descriptor < 0 || close(descriptor) != 0 ||
	    mkdir(EXTERNAL_MOUNT "/root-directory", 0755U) != 0 ||
	    expect_object(EXTERNAL_MOUNT, "root-file", S_IFREG, 0755U, 0, 0,
	    NULL) != 0 ||
	    expect_object(EXTERNAL_MOUNT, "root-directory", S_IFDIR, 0755U,
	    0, 0, NULL) != 0)
		return failure("fat-representable-create");
	child = fork();
	if (child < 0)
		return failure("fat-fork");
	if (child == 0)
		_exit(fat_nonroot_rejection_child());
	if (wait_child(child) != 0)
		return failure("fat-nonroot-rejection");
	if (unmount(EXTERNAL_MOUNT, 0) != 0 || remount_external(fspec) != 0 ||
	    expect_object(EXTERNAL_MOUNT, "root-file", S_IFREG, 0755U, 0, 0,
	    NULL) != 0 ||
	    expect_object(EXTERNAL_MOUNT, "root-directory", S_IFDIR, 0755U,
	    0, 0, NULL) != 0)
		return failure("fat-remount");
	errno = 0;
	if (lstat(EXTERNAL_MOUNT "/writable/user-file", &status) != -1 ||
	    errno != ENOENT)
		return failure("fat-residue");
	printf("WS001-P015 FAT PASS\n");
	fflush(stdout);
	return 0;
}

static int
run_overlay_stage_one(void)
{
	char fspec[32], backend[32];

	/*
	 * Overlay hard links remain an explicit EOPNOTSUPP residual.  Pathname
	 * AF_UNIX sockets now use the same atomic publication contract as tmpfs
	 * and native UFS.
	 */
	if (mkdir(STATE_DIRECTORY, 0755U) != 0 ||
	    run_suite(STATE_DIRECTORY "/overlay", 0, 1) != 0 ||
	    create_direct_nonroot("/p015-lower-only/nonroot", 0600U) != 0 ||
	    expect_object("/p015-lower-only", "nonroot", S_IFREG, 0600U,
	    TEST_UID, TEST_GID, NULL) != 0 ||
	    run_suite("/tmp/ws001-p015", 1, 1) != 0)
		return failure("overlay-tmpfs-create");
	if (mount_external("ufs2", fspec) != 0 ||
	    read_word(EXTERNAL_MOUNT "/.p015-backend", backend,
	    sizeof(backend)) != 0 || strcmp(backend, "ufs2") != 0 ||
	    run_suite(EXTERNAL_MOUNT "/suite", 1, 1) != 0 ||
	    unmount(EXTERNAL_MOUNT, 0) != 0 || remount_external(fspec) != 0 ||
	    validate_suite(EXTERNAL_MOUNT "/suite", 1, 1) != 0 ||
	    unmount(EXTERNAL_MOUNT, 0) != 0)
		return failure("ufs2-remount");
	if (write_file(STAGE_MARKER, "overlay\n", 0600U) != 0)
		return failure("overlay-stage-marker");
	printf("WS001-P015 STAGE1 PASS scenario=overlay backend=ufs2 "
	    "overlay-hardlink=EOPNOTSUPP overlay-socket=PASS\n");
	fflush(stdout);
	return 0;
}

static int
run_overlay(void)
{
	struct stat status;

	if (lstat(STAGE_MARKER, &status) != 0) {
		if (errno != ENOENT)
			return failure("overlay-stage-probe");
		return run_overlay_stage_one();
	}
	if (validate_suite(STATE_DIRECTORY "/overlay", 0, 1) != 0 ||
	    expect_object("/p015-lower-only", "nonroot", S_IFREG, 0600U,
	    TEST_UID, TEST_GID, NULL) != 0)
		return failure("overlay-reboot-validate");
	printf("WS001-P015 PASS scenario=overlay\n");
	fflush(stdout);
	return 0;
}

static int
run_native_stage_one(void)
{
	if (mkdir(STATE_DIRECTORY, 0755U) != 0 ||
	    run_suite(STATE_DIRECTORY "/native", 1, 1) != 0 ||
	    mkdir("/home/p015", 0700U) != 0 ||
	    chown("/home/p015", TEST_UID, TEST_GID) != 0 ||
	    create_direct_nonroot("/home/p015/.wifi.conf", 0600U) != 0 ||
	    expect_object("/home/p015", ".wifi.conf", S_IFREG, 0600U,
	    TEST_UID, TEST_GID, NULL) != 0 ||
	    write_file(STAGE_MARKER, "native\n", 0600U) != 0)
		return failure("native-stage1");
	printf("WS001-P015 STAGE1 PASS scenario=native backend=ufs1\n");
	fflush(stdout);
	return 0;
}

static int
run_native(void)
{
	struct stat status;

	if (lstat(STAGE_MARKER, &status) != 0) {
		if (errno != ENOENT)
			return failure("native-stage-probe");
		return run_native_stage_one();
	}
	if (validate_suite(STATE_DIRECTORY "/native", 1, 1) != 0 ||
	    expect_object("/home/p015", ".wifi.conf", S_IFREG, 0600U,
	    TEST_UID, TEST_GID, NULL) != 0)
		return failure("native-reboot-validate");
	printf("WS001-P015 PASS scenario=native-ufs1\n");
	fflush(stdout);
	return 0;
}

int
main(void)
{
	char scenario[32];

	printf("WS001-P015 START\n");
	fflush(stdout);
	if (read_word("/etc/ws001-p015-scenario", scenario,
	    sizeof(scenario)) != 0)
		return failure("scenario");
	if (strcmp(scenario, "overlay") == 0)
		return run_overlay();
	if (strcmp(scenario, "native") == 0)
		return run_native();
	if (strcmp(scenario, "fat") == 0)
		return run_fat();
	errno = EINVAL;
	return failure("scenario-value");
}
