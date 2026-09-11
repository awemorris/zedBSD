/*
 * WS005 p005 native /sbin/net Wi-Fi credential publication probe.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_UID ((uid_t)123)
#define TEST_GID ((gid_t)124)

#define NET_PATH "/sbin/net"
#define NETWORKD_SOCKET "/run/networkd.sock"
#define ROOT_TARGET "/etc/wifi.conf"
#define ROOT_LOCK "/etc/.wifi.conf.lock"
#define USER_HOME "/home/wifitest"
#define USER_TARGET USER_HOME "/.wifi.conf"
#define USER_LOCK USER_HOME "/.wifi.conf.lock"
#define FAKE_HOME "/tmp/fake-home"
#define FAKE_SENTINEL FAKE_HOME "/sentinel"
#define STATE_DIRECTORY "/q051-state"
#define STAGE_MARKER STATE_DIRECTORY "/stage1"
#define NETCONF_BASELINE STATE_DIRECTORY "/net.conf.before"
#define NETCONF_PATH "/etc/net.conf"

#define CAPTURE_LIMIT 65536U
#define FILE_LIMIT 65536U
#define SECRET_CAPACITY 32U
#define SECRET_COUNT 4U

enum persona {
	PERSONA_ROOT,
	PERSONA_SUDO_LIKE,
	PERSONA_USER_LIKE,
	PERSONA_ORDINARY
};

struct blob {
	unsigned char *bytes;
	size_t length;
};

static void
clear_bytes(void *memory, size_t length)
{
	volatile unsigned char *bytes = memory;

	while (length-- != 0U)
		*bytes++ = 0;
}

static int
failure(const char *stage)
{
	int saved = errno != 0 ? errno : EIO;

	printf("WS005-P005 FAIL stage=%s errno=%d\n", stage, saved);
	fflush(stdout);
	errno = saved;
	return 1;
}

static int
set_invalid(void)
{
	errno = EINVAL;
	return -1;
}

static void
blob_clear(struct blob *blob)
{
	if (blob->bytes != NULL) {
		clear_bytes(blob->bytes, blob->length);
		free(blob->bytes);
	}
	blob->bytes = NULL;
	blob->length = 0;
}

static int
read_file(const char *path, struct blob *result)
{
	unsigned char *bytes;
	size_t used = 0;
	ssize_t count;
	int descriptor, saved;

	result->bytes = NULL;
	result->length = 0;
	descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	bytes = malloc(FILE_LIMIT + 1U);
	if (bytes == NULL) {
		saved = errno;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}
	while (used <= FILE_LIMIT) {
		count = read(descriptor, bytes + used, FILE_LIMIT + 1U - used);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			goto failed;
		if (count == 0)
			break;
		used += (size_t)count;
	}
	if (used > FILE_LIMIT) {
		errno = EFBIG;
		goto failed;
	}
	if (close(descriptor) != 0) {
		descriptor = -1;
		goto failed;
	}
	result->bytes = bytes;
	result->length = used;
	return 0;

failed:
	saved = errno != 0 ? errno : EIO;
	if (descriptor >= 0)
		(void)close(descriptor);
	clear_bytes(bytes, FILE_LIMIT + 1U);
	free(bytes);
	errno = saved;
	return -1;
}

static int
write_all(int descriptor, const void *buffer, size_t length)
{
	const unsigned char *bytes = buffer;
	size_t offset = 0;

	while (offset < length) {
		ssize_t count = write(descriptor, bytes + offset,
		    length - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EIO;
			return -1;
		}
		offset += (size_t)count;
	}
	return 0;
}

static int
write_file(const char *path, const void *buffer, size_t length, mode_t mode)
{
	int descriptor, saved;

	descriptor = open(path,
	    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
	if (descriptor < 0)
		return -1;
	if (fchmod(descriptor, mode) != 0 ||
	    write_all(descriptor, buffer, length) != 0 ||
	    fsync(descriptor) != 0 || close(descriptor) != 0) {
		saved = errno != 0 ? errno : EIO;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}
	return 0;
}

static int
sync_directory(const char *path)
{
	int descriptor, result, saved;

	descriptor = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	result = fsync(descriptor);
	saved = errno;
	if (close(descriptor) != 0)
		return -1;
	if (result != 0) {
		errno = saved;
		return -1;
	}
	return 0;
}

static int
expect_absent(const char *path)
{
	struct stat status;

	errno = 0;
	if (lstat(path, &status) == -1 && errno == ENOENT)
		return 0;
	return set_invalid();
}

static int
expect_regular(const char *path, uid_t uid, gid_t gid,
	struct stat *result)
{
	struct stat status;

	if (lstat(path, &status) != 0)
		return -1;
	if (!S_ISREG(status.st_mode) || status.st_nlink != 1U ||
	    status.st_uid != uid || status.st_gid != gid ||
	    (status.st_mode & 07777U) != 0600U)
		return set_invalid();
	if (result != NULL)
		*result = status;
	return 0;
}

static int
same_inode(const struct stat *left, const struct stat *right)
{
	return left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

static int
expect_exact_bytes(const char *path, const void *expected, size_t length)
{
	struct blob actual;
	int result;

	if (read_file(path, &actual) != 0)
		return -1;
	result = actual.length == length &&
	    memcmp(actual.bytes, expected, length) == 0 ? 0 : -1;
	blob_clear(&actual);
	if (result != 0)
		errno = EINVAL;
	return result;
}

static int
expect_same_file(const char *path, const struct blob *expected)
{
	return expect_exact_bytes(path, expected->bytes, expected->length);
}

static void
make_secret(char output[SECRET_CAPACITY], unsigned ordinal)
{
	static const char prefix[] = "Q051-DUMMY-";
	static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
	size_t index, prefix_length = sizeof(prefix) - 1U;

	memcpy(output, prefix, prefix_length);
	for (index = 0; index < 12U; index++)
		output[prefix_length + index] =
		    alphabet[(ordinal * 7U + index * 5U + 3U) %
		    (sizeof(alphabet) - 1U)];
	output[prefix_length + 12U] = '\0';
}

static void
make_secrets(char secrets[SECRET_COUNT][SECRET_CAPACITY])
{
	unsigned index;

	for (index = 0; index < SECRET_COUNT; index++)
		make_secret(secrets[index], index);
}

static int
contains_secret(const unsigned char *bytes, size_t length,
	char secrets[SECRET_COUNT][SECRET_CAPACITY])
{
	unsigned index;

	for (index = 0; index < SECRET_COUNT; index++) {
		size_t offset, secret_length = strlen(secrets[index]);

		if (secret_length <= length) {
			for (offset = 0; offset <= length - secret_length; offset++)
				if (memcmp(bytes + offset, secrets[index],
				    secret_length) == 0)
					return 1;
		}
	}
	return 0;
}

static int
assume_persona(enum persona persona)
{
	uid_t real_uid = 0, effective_uid = 0;
	gid_t real_gid = 0, effective_gid = 0;

	if (setgroups(0U, NULL) != 0)
		return -1;
	switch (persona) {
	case PERSONA_ROOT:
		real_uid = 0;
		effective_uid = 0;
		real_gid = 0;
		effective_gid = 0;
		break;
	case PERSONA_SUDO_LIKE:
		real_uid = TEST_UID;
		effective_uid = 0;
		real_gid = TEST_GID;
		effective_gid = 0;
		break;
	case PERSONA_USER_LIKE:
		real_uid = 0;
		effective_uid = TEST_UID;
		real_gid = 0;
		effective_gid = TEST_GID;
		break;
	case PERSONA_ORDINARY:
		real_uid = TEST_UID;
		effective_uid = TEST_UID;
		real_gid = TEST_GID;
		effective_gid = TEST_GID;
		break;
	default:
		return set_invalid();
	}
	if (setregid(real_gid, effective_gid) != 0 ||
	    setreuid(real_uid, effective_uid) != 0)
		return -1;
	if (getuid() != real_uid || geteuid() != effective_uid ||
	    getgid() != real_gid || getegid() != effective_gid)
		return set_invalid();
	return 0;
}

static int
wait_child(pid_t child)
{
	int status;
	pid_t result;

	do {
		result = waitpid(child, &status, 0);
	} while (result < 0 && errno == EINTR);
	if (result != child)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return set_invalid();
	return 0;
}

static int
run_set_key(enum persona persona, const char *ssid, char *secret,
	int automatic, char secrets[SECRET_COUNT][SECRET_CAPACITY])
{
	static char home[] = "HOME=" FAKE_HOME;
	static char path[] = "PATH=/sbin:/bin:/usr/sbin:/usr/bin";
	char *environment[] = {home, path, NULL};
	char *arguments[7];
	unsigned char *capture;
	size_t used = 0;
	ssize_t count;
	int descriptors[2], saved, result = -1;
	pid_t child;

	arguments[0] = (char *)NET_PATH;
	arguments[1] = (char *)"wifi";
	arguments[2] = (char *)"set-key";
	arguments[3] = (char *)ssid;
	arguments[4] = secret;
	arguments[5] = automatic ? (char *)"auto" : NULL;
	arguments[6] = NULL;
	if (pipe(descriptors) != 0)
		return -1;
	child = fork();
	if (child < 0) {
		saved = errno;
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		errno = saved;
		return -1;
	}
	if (child == 0) {
		(void)close(descriptors[0]);
		if (dup2(descriptors[1], STDOUT_FILENO) < 0 ||
		    dup2(descriptors[1], STDERR_FILENO) < 0 ||
		    close(descriptors[1]) != 0 || assume_persona(persona) != 0)
			_exit(126);
		execve(NET_PATH, arguments, environment);
		_exit(127);
	}
	(void)close(descriptors[1]);
	capture = malloc(CAPTURE_LIMIT + 1U);
	if (capture == NULL) {
		saved = errno;
		(void)close(descriptors[0]);
		(void)wait_child(child);
		errno = saved;
		return -1;
	}
	while (used <= CAPTURE_LIMIT) {
		count = read(descriptors[0], capture + used,
		    CAPTURE_LIMIT + 1U - used);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			goto done;
		if (count == 0)
			break;
		used += (size_t)count;
	}
	if (used > CAPTURE_LIMIT) {
		errno = EOVERFLOW;
		goto done;
	}
	if (contains_secret(capture, used, secrets)) {
		errno = EACCES;
		goto done;
	}
	if (wait_child(child) != 0)
		goto done;
	child = -1;
	result = 0;

done:
	saved = errno;
	(void)close(descriptors[0]);
	if (child > 0)
		(void)wait_child(child);
	clear_bytes(capture, CAPTURE_LIMIT + 1U);
	free(capture);
	errno = saved;
	return result;
}

static int
expect_profile(const char *path, const char *ssid, const char *secret,
	int automatic)
{
	char expected[512];
	int length, result;

	length = snprintf(expected, sizeof(expected),
	    "wifi-conf 1\nnetwork \"%s\" wpa2-personal-ccmp \"%s\" %s\n",
	    ssid, secret, automatic ? "auto" : "manual");
	if (length < 0 || (size_t)length >= sizeof(expected))
		return set_invalid();
	result = expect_exact_bytes(path, expected, (size_t)length);
	clear_bytes(expected, sizeof(expected));
	return result;
}

static int
expect_no_temporary(const char *path)
{
	static const char prefix[] = ".wifi.conf.tmp.";
	DIR *directory;
	struct dirent *entry;
	int saved, result = 0;

	directory = opendir(path);
	if (directory == NULL)
		return -1;
	errno = 0;
	while ((entry = readdir(directory)) != NULL) {
		if (strncmp(entry->d_name, prefix, sizeof(prefix) - 1U) == 0) {
			errno = EINVAL;
			result = -1;
			break;
		}
	}
	if (entry == NULL && errno != 0)
		result = -1;
	saved = errno;
	if (closedir(directory) != 0 && result == 0)
		return -1;
	errno = saved;
	return result;
}

static int
make_user_home(void)
{
	int descriptor, saved;

	if (mkdir(USER_HOME, 0700U) != 0 ||
	    chown(USER_HOME, TEST_UID, TEST_GID) != 0 ||
	    chmod(USER_HOME, 0700U) != 0)
		return -1;
	descriptor = open(USER_HOME,
	    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	if (fchown(descriptor, TEST_UID, TEST_GID) != 0 ||
	    fchmod(descriptor, 0700U) != 0 || fsync(descriptor) != 0) {
		saved = errno;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}
	if (close(descriptor) != 0 || sync_directory("/home") != 0)
		return -1;
	return 0;
}

static int
make_fake_home(void)
{
	static const char sentinel[] = "unchanged\n";

	if (mkdir(FAKE_HOME, 0700U) != 0 ||
	    chown(FAKE_HOME, TEST_UID, TEST_GID) != 0 ||
	    chmod(FAKE_HOME, 0700U) != 0 ||
	    write_file(FAKE_SENTINEL, sentinel, sizeof(sentinel) - 1U,
	    0600U) != 0 || chown(FAKE_SENTINEL, TEST_UID, TEST_GID) != 0)
		return -1;
	return 0;
}

static int
validate_fake_home(void)
{
	static const char sentinel[] = "unchanged\n";
	DIR *directory;
	struct dirent *entry;
	struct stat status;
	int entries = 0, saved, result = 0;

	if (lstat(FAKE_HOME, &status) != 0 || !S_ISDIR(status.st_mode) ||
	    status.st_uid != TEST_UID || status.st_gid != TEST_GID ||
	    (status.st_mode & 07777U) != 0700U ||
	    expect_regular(FAKE_SENTINEL, TEST_UID, TEST_GID, NULL) != 0 ||
	    expect_exact_bytes(FAKE_SENTINEL, sentinel,
	    sizeof(sentinel) - 1U) != 0)
		return -1;
	directory = opendir(FAKE_HOME);
	if (directory == NULL)
		return -1;
	errno = 0;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		if (strcmp(entry->d_name, "sentinel") != 0) {
			errno = EINVAL;
			result = -1;
			break;
		}
		entries++;
	}
	if (entry == NULL && errno != 0)
		result = -1;
	if (result == 0 && entries != 1)
		result = set_invalid();
	saved = errno;
	if (closedir(directory) != 0 && result == 0)
		return -1;
	errno = saved;
	return result;
}

static int
validate_final(char secrets[SECRET_COUNT][SECRET_CAPACITY])
{
	if (expect_regular(ROOT_TARGET, 0, 0, NULL) != 0 ||
	    expect_regular(ROOT_LOCK, 0, 0, NULL) != 0 ||
	    expect_profile(ROOT_TARGET, "q051-root", secrets[1], 0) != 0 ||
	    expect_regular(USER_TARGET, TEST_UID, TEST_GID, NULL) != 0 ||
	    expect_regular(USER_LOCK, TEST_UID, TEST_GID, NULL) != 0 ||
	    expect_profile(USER_TARGET, "q051-user", secrets[3], 0) != 0 ||
	    expect_no_temporary("/etc") != 0 ||
	    expect_no_temporary(USER_HOME) != 0 ||
	    expect_absent(NETWORKD_SOCKET) != 0)
		return -1;
	return 0;
}

static int
run_stage_one(char secrets[SECRET_COUNT][SECRET_CAPACITY])
{
	static const char marker[] = "q051-stage1\n";
	struct blob netconf = {0};
	struct stat root_first, root_second, root_lock_first, root_lock_second;
	struct stat user_first, user_second, user_lock_first, user_lock_second;
	int saved;

	if (expect_absent(STATE_DIRECTORY) != 0 ||
	    expect_absent(ROOT_TARGET) != 0 || expect_absent(ROOT_LOCK) != 0 ||
	    expect_absent(USER_HOME) != 0 ||
	    expect_absent(NETWORKD_SOCKET) != 0 ||
	    read_file(NETCONF_PATH, &netconf) != 0)
		goto failed;
	if (mkdir(STATE_DIRECTORY, 0700U) != 0 ||
	    chmod(STATE_DIRECTORY, 0700U) != 0 || sync_directory("/") != 0 ||
	    make_user_home() != 0 || make_fake_home() != 0)
		goto failed;

	if (run_set_key(PERSONA_ROOT, "q051-root", secrets[0], 1,
	    secrets) != 0 ||
	    expect_regular(ROOT_TARGET, 0, 0, &root_first) != 0 ||
	    expect_regular(ROOT_LOCK, 0, 0, &root_lock_first) != 0 ||
	    expect_profile(ROOT_TARGET, "q051-root", secrets[0], 1) != 0 ||
	    expect_no_temporary("/etc") != 0 ||
	    run_set_key(PERSONA_SUDO_LIKE, "q051-root", secrets[1], 0,
	    secrets) != 0 ||
	    expect_regular(ROOT_TARGET, 0, 0, &root_second) != 0 ||
	    expect_regular(ROOT_LOCK, 0, 0, &root_lock_second) != 0 ||
	    same_inode(&root_first, &root_second) ||
	    !same_inode(&root_lock_first, &root_lock_second) ||
	    expect_profile(ROOT_TARGET, "q051-root", secrets[1], 0) != 0)
		goto failed;

	if (run_set_key(PERSONA_USER_LIKE, "q051-user", secrets[2], 1,
	    secrets) != 0 ||
	    expect_regular(USER_TARGET, TEST_UID, TEST_GID, &user_first) != 0 ||
	    expect_regular(USER_LOCK, TEST_UID, TEST_GID,
	    &user_lock_first) != 0 ||
	    expect_profile(USER_TARGET, "q051-user", secrets[2], 1) != 0 ||
	    expect_no_temporary(USER_HOME) != 0 ||
	    run_set_key(PERSONA_ORDINARY, "q051-user", secrets[3], 0,
	    secrets) != 0 ||
	    expect_regular(USER_TARGET, TEST_UID, TEST_GID, &user_second) != 0 ||
	    expect_regular(USER_LOCK, TEST_UID, TEST_GID,
	    &user_lock_second) != 0 || same_inode(&user_first, &user_second) ||
	    !same_inode(&user_lock_first, &user_lock_second) ||
	    expect_profile(USER_TARGET, "q051-user", secrets[3], 0) != 0)
		goto failed;

	if (validate_final(secrets) != 0 || validate_fake_home() != 0 ||
	    expect_same_file(NETCONF_PATH, &netconf) != 0 ||
	    write_file(NETCONF_BASELINE, netconf.bytes, netconf.length,
	    0600U) != 0 || sync_directory(STATE_DIRECTORY) != 0 ||
	    write_file(STAGE_MARKER, marker, sizeof(marker) - 1U, 0600U) != 0 ||
	    sync_directory(STATE_DIRECTORY) != 0)
		goto failed;
	blob_clear(&netconf);
	printf("WS005-P005 STAGE1 PASS\n");
	fflush(stdout);
	return 0;

failed:
	saved = errno != 0 ? errno : EIO;
	blob_clear(&netconf);
	errno = saved;
	return -1;
}

static int
run_stage_two(char secrets[SECRET_COUNT][SECRET_CAPACITY])
{
	static const char marker[] = "q051-stage1\n";
	struct blob netconf = {0};
	int saved;

	if (expect_regular(STAGE_MARKER, 0, 0, NULL) != 0 ||
	    expect_exact_bytes(STAGE_MARKER, marker, sizeof(marker) - 1U) != 0 ||
	    expect_regular(NETCONF_BASELINE, 0, 0, NULL) != 0 ||
	    read_file(NETCONF_BASELINE, &netconf) != 0 ||
	    validate_final(secrets) != 0 ||
	    expect_same_file(NETCONF_PATH, &netconf) != 0 ||
	    expect_absent(FAKE_HOME) != 0)
		goto failed;
	blob_clear(&netconf);
	printf("WS005-P005 PASS\n");
	fflush(stdout);
	return 0;

failed:
	saved = errno != 0 ? errno : EIO;
	blob_clear(&netconf);
	errno = saved;
	return -1;
}

int
main(void)
{
	char secrets[SECRET_COUNT][SECRET_CAPACITY];
	struct stat status;
	int result, saved;

	make_secrets(secrets);
	errno = 0;
	if (lstat(STAGE_MARKER, &status) == 0)
		result = run_stage_two(secrets);
	else if (errno == ENOENT)
		result = run_stage_one(secrets);
	else
		result = -1;
	saved = errno;
	clear_bytes(secrets, sizeof(secrets));
	if (result != 0) {
		errno = saved;
		return failure("native-credential-store");
	}
	return 0;
}
