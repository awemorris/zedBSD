/*
 * WS005 NET-T21 secure wifi.conf publication fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#define _POSIX_C_SOURCE 200809L
#include "userland/base/net/wifi-store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s (errno=%d)\n", \
		    __FILE__, __LINE__, #expression, errno); \
		exit(1); \
	} \
} while (0)

static int directory_fd;
static uid_t owner_uid;
static gid_t owner_gid;
static const char *directory_path;

static void
secret_fill(char output[17], int seed)
{
	size_t index;

	for (index = 0; index < 16U; index++)
		output[index] = (char)('!' + (seed + (int)index * 17) % 90);
	output[16] = '\0';
}

static void
write_all(int descriptor, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	size_t offset = 0;

	while (offset < length) {
		ssize_t count = write(descriptor, bytes + offset, length - offset);

		CHECK(count > 0);
		offset += (size_t)count;
	}
}

static void
write_fixture(const char *name, const void *data, size_t length)
{
	int descriptor = openat(directory_fd, name,
	    O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);

	CHECK(descriptor >= 0);
	CHECK(fchmod(descriptor, 0600) == 0);
	CHECK(fchown(descriptor, owner_uid, owner_gid) == 0);
	write_all(descriptor, data, length);
	CHECK(fsync(descriptor) == 0);
	CHECK(close(descriptor) == 0);
}

static void
remove_name(const char *name)
{
	if (unlinkat(directory_fd, name, 0) != 0)
		CHECK(errno == ENOENT);
}

static void
clear_store(void)
{
	for (;;) {
		DIR *directory;
		struct dirent *entry;
		char name[256] = "";
		int scan = openat(directory_fd, ".", O_RDONLY | O_DIRECTORY |
		    O_CLOEXEC);

		CHECK(scan >= 0);
		directory = fdopendir(scan);
		CHECK(directory != NULL);
		while ((entry = readdir(directory)) != NULL)
			if (strcmp(entry->d_name, ".") != 0 &&
			    strcmp(entry->d_name, "..") != 0) {
				CHECK(strlen(entry->d_name) < sizeof(name));
				strcpy(name, entry->d_name);
				break;
			}
		CHECK(closedir(directory) == 0);
		if (name[0] == '\0')
			break;
		CHECK(unlinkat(directory_fd, name, 0) == 0);
	}
}

static void
assert_metadata(const char *name)
{
	struct stat status;

	CHECK(fstatat(directory_fd, name, &status, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(S_ISREG(status.st_mode));
	CHECK(status.st_nlink == 1);
	CHECK(status.st_uid == owner_uid && status.st_gid == owner_gid);
	CHECK((status.st_mode & 07777U) == 0600U);
}

static int
find_profile(const struct wifi_conf_model *model, const char *name)
{
	size_t index, length = strlen(name);

	for (index = 0; index < model->profile_count; index++)
		if (model->profiles[index].ssid_length == length &&
		    memcmp(model->profiles[index].ssid, name, length) == 0)
			return (int)index;
	return -1;
}

static void
load_model(struct wifi_conf_model *model)
{
	char error[WIFI_CONF_DIAGNOSTIC_MAX];

	wifi_conf_model_init(model);
	CHECK(wifi_store_load_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, model, error, sizeof(error)) == 0);
}

static void
assert_passphrase(const char *name, const char *passphrase, int automatic)
{
	struct wifi_conf_model model;
	int index;

	load_model(&model);
	index = find_profile(&model, name);
	CHECK(index >= 0);
	CHECK(model.profiles[index].passphrase_length == strlen(passphrase));
	CHECK(memcmp(model.profiles[index].passphrase, passphrase,
	    strlen(passphrase)) == 0);
	CHECK(model.profiles[index].automatic == automatic);
	wifi_conf_model_clear(&model);
}

static void
set_key(const char *name, const char *passphrase, int automatic)
{
	char error[WIFI_CONF_DIAGNOSTIC_MAX];

	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, name, strlen(name), passphrase, strlen(passphrase),
	    automatic, error, sizeof(error)) == 0);
}

static void
assert_no_temporary(void)
{
	DIR *directory;
	struct dirent *entry;
	int scan = openat(directory_fd, ".", O_RDONLY | O_DIRECTORY |
	    O_CLOEXEC);

	CHECK(scan >= 0);
	directory = fdopendir(scan);
	CHECK(directory != NULL);
	while ((entry = readdir(directory)) != NULL)
		CHECK(strncmp(entry->d_name, ".wifi.conf.tmp.", 15U) != 0);
	CHECK(closedir(directory) == 0);
}

static void
find_temporary(char *name, size_t capacity, struct stat *status)
{
	DIR *directory;
	struct dirent *entry;
	int found = 0;
	int scan = openat(directory_fd, ".", O_RDONLY | O_DIRECTORY |
	    O_CLOEXEC);

	CHECK(scan >= 0);
	directory = fdopendir(scan);
	CHECK(directory != NULL);
	while ((entry = readdir(directory)) != NULL) {
		if (strncmp(entry->d_name, ".wifi.conf.tmp.", 15U) != 0)
			continue;
		CHECK(!found);
		CHECK(strlen(entry->d_name) < capacity);
		strcpy(name, entry->d_name);
		CHECK(fstatat(directory_fd, name, status,
		    AT_SYMLINK_NOFOLLOW) == 0);
		found = 1;
	}
	CHECK(closedir(directory) == 0);
	CHECK(found);
}

static void
test_create_replace_append(void)
{
	struct wifi_conf_model model;
	char first[17], second[17], third[17];

	secret_fill(first, 1);
	secret_fill(second, 2);
	secret_fill(third, 3);
	clear_store();
	set_key("alpha", first, 1);
	assert_metadata("wifi.conf");
	assert_metadata(".wifi.conf.lock");
	assert_passphrase("alpha", first, 1);
	set_key("beta", second, 0);
	set_key("alpha", third, 0);
	load_model(&model);
	CHECK(model.profile_count == 2U);
	CHECK(find_profile(&model, "alpha") == 0);
	CHECK(find_profile(&model, "beta") == 1);
	wifi_conf_model_clear(&model);
	assert_passphrase("alpha", third, 0);
	assert_no_temporary();
	wifi_conf_explicit_clear(first, sizeof(first));
	wifi_conf_explicit_clear(second, sizeof(second));
	wifi_conf_explicit_clear(third, sizeof(third));
}

static void
test_invalid_existing_preserved(void)
{
	static const char invalid[] = "not-a-wifi-conf\n";
	char secret[17], error[WIFI_CONF_DIAGNOSTIC_MAX], bytes[32] = {0};
	int descriptor;

	secret_fill(secret, 7);
	clear_store();
	write_fixture("wifi.conf", invalid, sizeof(invalid) - 1U);
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, secret, strlen(secret), 0, error,
	    sizeof(error)) != 0);
	CHECK(strstr(error, secret) == NULL);
	descriptor = openat(directory_fd, "wifi.conf", O_RDONLY | O_NOFOLLOW);
	CHECK(descriptor >= 0);
	CHECK(read(descriptor, bytes, sizeof(bytes)) == (ssize_t)(sizeof(invalid) - 1U));
	CHECK(close(descriptor) == 0);
	CHECK(memcmp(bytes, invalid, sizeof(invalid) - 1U) == 0);
	assert_no_temporary();
	wifi_conf_explicit_clear(secret, sizeof(secret));
	wifi_conf_explicit_clear(bytes, sizeof(bytes));
}

static void
test_unsafe_objects(void)
{
	static const char valid[] =
	    "wifi-conf 1\nnetwork \"alpha\" wpa2-personal-ccmp \"12345678\" manual\n";
	char secret[17], error[WIFI_CONF_DIAGNOSTIC_MAX];

	secret_fill(secret, 9);
	clear_store();
	write_fixture("wifi.conf", valid, sizeof(valid) - 1U);
	CHECK(fchmodat(directory_fd, "wifi.conf", 0644, 0) == 0);
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, secret, strlen(secret), 0, error,
	    sizeof(error)) != 0);
	CHECK(fchmodat(directory_fd, "wifi.conf", 0600, 0) == 0);
	CHECK(linkat(directory_fd, "wifi.conf", directory_fd, "alias", 0) == 0);
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, secret, strlen(secret), 0, error,
	    sizeof(error)) != 0);
	remove_name("alias");
	remove_name("wifi.conf");
	write_fixture("victim", valid, sizeof(valid) - 1U);
	CHECK(symlinkat("victim", directory_fd, "wifi.conf") == 0);
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, secret, strlen(secret), 0, error,
	    sizeof(error)) != 0);
	remove_name("wifi.conf");
	remove_name(".wifi.conf.lock");
	CHECK(symlinkat("victim", directory_fd, ".wifi.conf.lock") == 0);
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, secret, strlen(secret), 0, error,
	    sizeof(error)) != 0);
	CHECK(strstr(error, secret) == NULL);
	remove_name(".wifi.conf.lock");
	remove_name("victim");
	wifi_conf_explicit_clear(secret, sizeof(secret));
}

static int
replace_target_after_read(int directory, const char *target)
{
	int saved;

	if (renameat(directory, target, directory, "reader-old") != 0)
		return -1;
	if (renameat(directory, "reader-new", directory, target) == 0)
		return 0;
	saved = errno;
	(void)renameat(directory, "reader-old", directory, target);
	errno = saved;
	return -1;
}

static void
test_reader_replaced_target(void)
{
	static const char replacement[] =
	    "wifi-conf 1\n"
	    "network \"replacement\" wpa2-personal-ccmp \"87654321\" manual\n";
	struct wifi_conf_model model;
	char secret[17], error[WIFI_CONF_DIAGNOSTIC_MAX];
	int index;

	secret_fill(secret, 52);
	clear_store();
	set_key("original", secret, 1);
	load_model(&model);
	write_fixture("reader-new", replacement, sizeof(replacement) - 1U);
	wifi_store_test_set_load_after_read_hook(replace_target_after_read);
	errno = 0;
	CHECK(wifi_store_load_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, &model, error, sizeof(error)) != 0);
	CHECK(errno == EBUSY);
	CHECK(strstr(error, secret) == NULL);
	CHECK(model.profile_count == 1U);
	index = find_profile(&model, "original");
	CHECK(index == 0);
	CHECK(model.profiles[index].passphrase_length == strlen(secret));
	CHECK(memcmp(model.profiles[index].passphrase, secret,
	    strlen(secret)) == 0);
	wifi_conf_model_clear(&model);
	assert_passphrase("replacement", "87654321", 0);
	remove_name("reader-old");
	assert_no_temporary();
	wifi_conf_explicit_clear(secret, sizeof(secret));
}

static void
test_invalid_automatic_and_truncation(void)
{
	char secret[17], error[WIFI_CONF_DIAGNOSTIC_MAX], small[24];
	size_t length;

	secret_fill(secret, 10);
	clear_store();
	errno = 0;
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, secret, strlen(secret), 2, error,
	    sizeof(error)) != 0);
	CHECK(errno == EINVAL);
	CHECK(strstr(error, "invalid update mode") != NULL);
	CHECK(strstr(error, secret) == NULL);
	errno = 0;
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, secret, strlen(secret), -1, small,
	    sizeof(small)) != 0);
	CHECK(errno == EINVAL);
	length = strlen(small);
	CHECK(length == sizeof(small) - 1U);
	CHECK(length >= strlen("[truncated]"));
	CHECK(strcmp(small + length - strlen("[truncated]"),
	    "[truncated]") == 0);
	wifi_conf_explicit_clear(secret, sizeof(secret));
}

static void
test_persistent_lock_survives_open_failure(void)
{
	struct stat before, after;
	char secret[17], error[WIFI_CONF_DIAGNOSTIC_MAX];

	secret_fill(secret, 14);
	clear_store();
	wifi_store_test_fail_once(WIFI_STORE_TEST_LOCK_ACQUIRE, EIO);
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, secret, strlen(secret), 0, error,
	    sizeof(error)) != 0);
	CHECK(fstatat(directory_fd, ".wifi.conf.lock", &before,
	    AT_SYMLINK_NOFOLLOW) == 0);
	assert_metadata(".wifi.conf.lock");
	set_key("alpha", secret, 0);
	CHECK(fstatat(directory_fd, ".wifi.conf.lock", &after,
	    AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(before.st_dev == after.st_dev && before.st_ino == after.st_ino);
	wifi_conf_explicit_clear(secret, sizeof(secret));
}

static void
test_cleanup_sanitizes_residual(void)
{
	struct stat status;
	char old_secret[17], new_secret[17];
	char error[WIFI_CONF_DIAGNOSTIC_MAX], temporary[NAME_MAX + 1U] = "";

	secret_fill(old_secret, 15);
	secret_fill(new_secret, 16);
	clear_store();
	set_key("alpha", old_secret, 0);
	wifi_store_test_fail_once(WIFI_STORE_TEST_TEMP_SYNC, EIO);
	wifi_store_test_fail_once(WIFI_STORE_TEST_CLEANUP, EACCES);
	errno = 0;
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, new_secret, strlen(new_secret), 1,
	    error, sizeof(error)) != 0);
	CHECK(errno == EIO);
	CHECK(strstr(error, "cleanup failed") != NULL);
	CHECK(strstr(error, "sanitized temporary remains") != NULL);
	CHECK(strstr(error, old_secret) == NULL);
	CHECK(strstr(error, new_secret) == NULL);
	find_temporary(temporary, sizeof(temporary), &status);
	CHECK(S_ISREG(status.st_mode));
	CHECK(status.st_size == 0);
	assert_passphrase("alpha", old_secret, 0);
	remove_name(temporary);
	assert_no_temporary();
	wifi_conf_explicit_clear(old_secret, sizeof(old_secret));
	wifi_conf_explicit_clear(new_secret, sizeof(new_secret));
}

static void
test_store_directory_policy(void)
{
	char path[PATH_MAX], linked[PATH_MAX];
	char secret[17], error[WIFI_CONF_DIAGNOSTIC_MAX];
	uid_t other_uid = owner_uid == 0 ? 1 : 0;
	int descriptor;

	secret_fill(secret, 18);
	clear_store();
	CHECK(fchmod(directory_fd, 0770) == 0);
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, secret, strlen(secret), 0, error,
	    sizeof(error)) != 0);
	CHECK(errno == EPERM);
	CHECK(fchmod(directory_fd, 0700) == 0);
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", other_uid,
	    owner_gid, "alpha", 5U, secret, strlen(secret), 0, error,
	    sizeof(error)) != 0);
	CHECK(errno == EPERM);
	CHECK(mkdirat(directory_fd, "safe-home", 0700) == 0);
	CHECK(snprintf(path, sizeof(path), "%s/safe-home", directory_path) > 0);
	descriptor = wifi_store_test_open_directory(path, owner_uid);
	CHECK(descriptor >= 0);
	CHECK(close(descriptor) == 0);
	CHECK(symlinkat("safe-home", directory_fd, "linked-home") == 0);
	CHECK(snprintf(linked, sizeof(linked), "%s/linked-home",
	    directory_path) > 0);
	CHECK(wifi_store_test_open_directory(linked, owner_uid) < 0);
	CHECK(fchmodat(directory_fd, "safe-home", 0770, 0) == 0);
	CHECK(wifi_store_test_open_directory(path, owner_uid) < 0);
	CHECK(errno == EPERM);
	remove_name("linked-home");
	CHECK(unlinkat(directory_fd, "safe-home", AT_REMOVEDIR) == 0);
	wifi_conf_explicit_clear(secret, sizeof(secret));
}

static void
test_failure_boundaries(void)
{
	static const enum wifi_store_test_stage stages[] = {
		WIFI_STORE_TEST_LOCK_OPEN, WIFI_STORE_TEST_LOCK_ACQUIRE,
		WIFI_STORE_TEST_TARGET_OPEN, WIFI_STORE_TEST_TARGET_READ,
		WIFI_STORE_TEST_PARSE, WIFI_STORE_TEST_TEMP_CREATE,
		WIFI_STORE_TEST_TEMP_WRITE, WIFI_STORE_TEST_TEMP_SYNC,
		WIFI_STORE_TEST_TEMP_CLOSE, WIFI_STORE_TEST_STAGE_OPEN,
		WIFI_STORE_TEST_STAGE_READ, WIFI_STORE_TEST_STAGE_VALIDATE,
		WIFI_STORE_TEST_RENAME, WIFI_STORE_TEST_DIRECTORY_SYNC,
		WIFI_STORE_TEST_UNLOCK
	};
	char old_secret[17], new_secret[17], error[WIFI_CONF_DIAGNOSTIC_MAX];
	size_t index;

	secret_fill(old_secret, 11);
	secret_fill(new_secret, 12);
	for (index = 0; index < sizeof(stages) / sizeof(stages[0]); index++) {
		clear_store();
		set_key("alpha", old_secret, 0);
		wifi_store_test_fail_once(stages[index], EIO);
		CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
		    owner_gid, "alpha", 5U, new_secret, strlen(new_secret), 1,
		    error, sizeof(error)) != 0);
		CHECK(strstr(error, old_secret) == NULL);
		CHECK(strstr(error, new_secret) == NULL);
		if (stages[index] == WIFI_STORE_TEST_DIRECTORY_SYNC ||
		    stages[index] == WIFI_STORE_TEST_UNLOCK)
			assert_passphrase("alpha", new_secret, 1);
		else
			assert_passphrase("alpha", old_secret, 0);
		assert_no_temporary();
	}
	wifi_conf_explicit_clear(old_secret, sizeof(old_secret));
	wifi_conf_explicit_clear(new_secret, sizeof(new_secret));
}

static void
test_concurrent_writers(void)
{
	struct wifi_conf_model model;
	char baseline[17];
	unsigned index;

	secret_fill(baseline, 20);
	clear_store();
	set_key("base", baseline, 0);
	for (index = 0; index < 8U; index++) {
		pid_t child = fork();

		CHECK(child >= 0);
		if (child == 0) {
			char name[16], secret[17], error[WIFI_CONF_DIAGNOSTIC_MAX];

			(void)snprintf(name, sizeof(name), "child-%u", index);
			secret_fill(secret, 30 + (int)index);
			_exit(wifi_store_set_key_at(directory_fd, "wifi.conf",
			    owner_uid, owner_gid, name, strlen(name), secret,
			    strlen(secret), index & 1U, error, sizeof(error)) == 0
			    ? 0 : 1);
		}
	}
	for (index = 0; index < 8U; index++) {
		int status;

		CHECK(wait(&status) > 0);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	load_model(&model);
	CHECK(model.profile_count == 9U);
	for (index = 0; index < 8U; index++) {
		char name[16];

		(void)snprintf(name, sizeof(name), "child-%u", index);
		CHECK(find_profile(&model, name) >= 0);
	}
	wifi_conf_model_clear(&model);
	assert_no_temporary();
	wifi_conf_explicit_clear(baseline, sizeof(baseline));
}

static long long
monotonic_milliseconds(void)
{
	struct timespec now;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000L;
}

static pid_t
start_lock_holder(unsigned milliseconds)
{
	int ready[2];
	pid_t child;

	CHECK(pipe(ready) == 0);
	child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		struct flock lock;
		struct timespec delay;
		char marker = 'R';
		int descriptor;

		(void)close(ready[0]);
		descriptor = openat(directory_fd, ".wifi.conf.lock",
		    O_RDWR | O_NOFOLLOW | O_CLOEXEC);
		if (descriptor < 0)
			_exit(2);
		memset(&lock, 0, sizeof(lock));
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		if (fcntl(descriptor, F_SETLK, &lock) != 0 ||
		    write(ready[1], &marker, 1U) != 1)
			_exit(3);
		delay.tv_sec = milliseconds / 1000U;
		delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
		(void)nanosleep(&delay, NULL);
		lock.l_type = F_UNLCK;
		if (fcntl(descriptor, F_SETLK, &lock) != 0 ||
		    close(descriptor) != 0)
			_exit(4);
		_exit(0);
	}
	CHECK(close(ready[1]) == 0);
	{
		char marker;

		CHECK(read(ready[0], &marker, 1U) == 1 && marker == 'R');
	}
	CHECK(close(ready[0]) == 0);
	return child;
}

static void
wait_holder(pid_t child)
{
	int status;

	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void
test_lock_deadline(void)
{
	char first[17], second[17], error[WIFI_CONF_DIAGNOSTIC_MAX];
	long long start, elapsed;
	pid_t child;

	secret_fill(first, 50);
	secret_fill(second, 51);
	clear_store();
	set_key("alpha", first, 0);
	child = start_lock_holder(4600U);
	start = monotonic_milliseconds();
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, second, strlen(second), 1, error,
	    sizeof(error)) == 0);
	elapsed = monotonic_milliseconds() - start;
	CHECK(elapsed >= 4000LL && elapsed < 5200LL);
	wait_holder(child);

	child = start_lock_holder(5500U);
	start = monotonic_milliseconds();
	errno = 0;
	CHECK(wifi_store_set_key_at(directory_fd, "wifi.conf", owner_uid,
	    owner_gid, "alpha", 5U, first, strlen(first), 0, error,
	    sizeof(error)) != 0);
	elapsed = monotonic_milliseconds() - start;
	CHECK(errno == ETIMEDOUT);
	CHECK(elapsed >= 4900LL && elapsed < 5300LL);
	CHECK(strstr(error, first) == NULL && strstr(error, second) == NULL);
	wait_holder(child);
	assert_passphrase("alpha", second, 1);
	wifi_conf_explicit_clear(first, sizeof(first));
	wifi_conf_explicit_clear(second, sizeof(second));
}

int
main(int argc, char **argv)
{
	CHECK(argc == 2);
	directory_fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
	    O_CLOEXEC);
	CHECK(directory_fd >= 0);
	owner_uid = geteuid();
	owner_gid = getegid();
	directory_path = argv[1];
	test_create_replace_append();
	test_invalid_existing_preserved();
	test_unsafe_objects();
	test_reader_replaced_target();
	test_invalid_automatic_and_truncation();
	test_persistent_lock_survives_open_failure();
	test_cleanup_sanitizes_residual();
	test_store_directory_policy();
	test_failure_boundaries();
	test_concurrent_writers();
	if (getenv("WIFI_STORE_SKIP_SLOW") == NULL)
		test_lock_deadline();
	clear_store();
	CHECK(close(directory_fd) == 0);
	puts("wifi-store test: PASS");
	return 0;
}
