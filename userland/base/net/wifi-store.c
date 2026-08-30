/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200809L
#include "userland/base/net/wifi-store.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WIFI_STORE_ROOT_DIRECTORY "/etc"
#define WIFI_STORE_ROOT_TARGET "wifi.conf"
#define WIFI_STORE_USER_TARGET ".wifi.conf"
#define WIFI_STORE_LOCK_TARGET ".wifi.conf.lock"
#define WIFI_STORE_PASSWD_BUFFER 2048U
#define WIFI_STORE_PASSWD_FILE_MAX 65536U
#define WIFI_STORE_PASSWD_LINE_MAX 1024U
#define WIFI_STORE_TEMP_ATTEMPTS 128U
#define WIFI_STORE_LOCK_TIMEOUT_NS 5000000000ULL
#define WIFI_STORE_LOCK_POLL_NS 10000000L

#ifdef WIFI_STORE_TESTING
static unsigned char wifi_store_failure_pending[WIFI_STORE_TEST_UNLOCK + 1U];
static int wifi_store_failure_errno[WIFI_STORE_TEST_UNLOCK + 1U];

void
wifi_store_test_fail_once(enum wifi_store_test_stage stage, int error)
{
	if (stage > WIFI_STORE_TEST_NONE && stage <= WIFI_STORE_TEST_UNLOCK) {
		wifi_store_failure_pending[stage] = 1U;
		wifi_store_failure_errno[stage] = error;
	}
}

static int
test_failure(enum wifi_store_test_stage stage)
{
	if (stage <= WIFI_STORE_TEST_NONE || stage > WIFI_STORE_TEST_UNLOCK ||
	    wifi_store_failure_pending[stage] == 0)
		return 0;
	wifi_store_failure_pending[stage] = 0;
	errno = wifi_store_failure_errno[stage] != 0 ?
	    wifi_store_failure_errno[stage] : EIO;
	return -1;
}
#define WIFI_STORE_FAIL(stage) test_failure(stage)
#else
#define WIFI_STORE_FAIL(stage) 0
#endif

struct selected_store {
	int directory;
	uid_t uid;
	gid_t gid;
	const char *target;
};

static int
copy_diagnostic(char *error, size_t capacity, const char *message)
{
	static const char marker[] = "[truncated]";
	size_t length, prefix;

	if (error == NULL || capacity == 0)
		return 0;
	length = strlen(message);
	if (length < capacity) {
		memcpy(error, message, length + 1U);
		return 0;
	}
	if (capacity > sizeof(marker)) {
		prefix = capacity - sizeof(marker);
		memcpy(error, message, prefix);
		memcpy(error + prefix, marker, sizeof(marker));
	} else {
		/* A buffer this small cannot hold the complete marker. */
		memcpy(error, marker, capacity - 1U);
		error[capacity - 1U] = '\0';
	}
	return 0;
}

static int
store_error(char *error, size_t capacity, int number, const char *stage,
	    int durability_uncertain)
{
	char message[WIFI_CONF_DIAGNOSTIC_MAX];

	if (durability_uncertain)
		(void)snprintf(message, sizeof(message),
		    "wifi.conf: %s failed after publication; durability is uncertain: %s",
		    stage, strerror(number));
	else
		(void)snprintf(message, sizeof(message), "wifi.conf: %s: %s",
		    stage, strerror(number));
	(void)copy_diagnostic(error, capacity, message);
	wifi_conf_explicit_clear(message, sizeof(message));
	errno = number;
	return -1;
}

static int
store_cleanup_error(char *error, size_t capacity, int original_error,
		    int cleanup_error, int sanitized)
{
	char message[WIFI_CONF_DIAGNOSTIC_MAX];

	(void)snprintf(message, sizeof(message),
	    "wifi.conf: update failed: %s; cleanup failed (%s); %s temporary remains",
	    strerror(original_error), strerror(cleanup_error),
	    sanitized ? "sanitized" : "credential-bearing");
	(void)copy_diagnostic(error, capacity, message);
	wifi_conf_explicit_clear(message, sizeof(message));
	errno = original_error;
	return -1;
}

static int
status_regular(const struct stat *status, uid_t uid, gid_t gid)
{
	if (!S_ISREG(status->st_mode) || status->st_nlink != 1 ||
	    status->st_uid != uid || status->st_gid != gid ||
	    (status->st_mode & 07777U) != 0600U) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

static int
descriptor_regular(int descriptor, uid_t uid, gid_t gid,
		   struct stat *result)
{
	struct stat status;

	if (fstat(descriptor, &status) != 0)
		return -1;
	if (status_regular(&status, uid, gid) != 0)
		return -1;
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
named_inode_matches(int directory, const char *name,
		    const struct stat *expected, uid_t uid, gid_t gid)
{
	struct stat status;

	if (fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) != 0)
		return -1;
	if (status_regular(&status, uid, gid) != 0)
		return -1;
	if (!same_inode(&status, expected)) {
		errno = EBUSY;
		return -1;
	}
	return 0;
}

static int
descriptor_store_directory(int descriptor, uid_t uid,
			   struct stat *result)
{
	struct stat status;

	if (fstat(descriptor, &status) != 0)
		return -1;
	if (!S_ISDIR(status.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}
	if (status.st_uid != uid || (status.st_mode & 0022U) != 0) {
		errno = EPERM;
		return -1;
	}
	if (result != NULL)
		*result = status;
	return 0;
}

static uint64_t
timespec_nanoseconds(const struct timespec *value)
{
	return (uint64_t)value->tv_sec * 1000000000ULL +
	       (uint64_t)value->tv_nsec;
}

static int
acquire_record_lock(int descriptor, short type)
{
	struct flock lock;
	struct timespec now, delay;
	uint64_t deadline;
	int first_attempt = 1;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return -1;
	deadline = timespec_nanoseconds(&now) + WIFI_STORE_LOCK_TIMEOUT_NS;
	memset(&lock, 0, sizeof(lock));
	lock.l_type = type;
	lock.l_whence = SEEK_SET;
	delay.tv_sec = 0;
	delay.tv_nsec = WIFI_STORE_LOCK_POLL_NS;
	for (;;) {
		/* The initial attempt is immediate; every retry is deadline-gated. */
		if (!first_attempt) {
			if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
				return -1;
			if (timespec_nanoseconds(&now) >= deadline) {
				errno = ETIMEDOUT;
				return -1;
			}
		}
		first_attempt = 0;
		if (fcntl(descriptor, F_SETLK, &lock) == 0)
			return 0;
		if (errno != EACCES && errno != EAGAIN)
			return -1;
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
			return -1;
		if (timespec_nanoseconds(&now) >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		{
			uint64_t remaining = deadline - timespec_nanoseconds(&now);

			delay.tv_sec = 0;
			delay.tv_nsec = remaining < (uint64_t)WIFI_STORE_LOCK_POLL_NS
			    ? (long)remaining : WIFI_STORE_LOCK_POLL_NS;
		}
		if (nanosleep(&delay, NULL) != 0)
			return -1;
	}
}

static int
release_record_lock(int descriptor)
{
	struct flock lock;

	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_UNLCK;
	lock.l_whence = SEEK_SET;
	return fcntl(descriptor, F_SETLK, &lock);
}

static int
open_lock(int directory, uid_t uid, gid_t gid, short type)
{
	struct stat descriptor_status;
	int descriptor, created = 0, saved;

	if (WIFI_STORE_FAIL(WIFI_STORE_TEST_LOCK_OPEN) != 0)
		return -1;
	descriptor = openat(directory, WIFI_STORE_LOCK_TARGET,
	    O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (descriptor >= 0)
		created = 1;
	else if (errno == EEXIST)
		descriptor = openat(directory, WIFI_STORE_LOCK_TARGET,
		    O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	if (created && (fchmod(descriptor, 0600) != 0 ||
	    fchown(descriptor, uid, gid) != 0)) {
		saved = errno;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}
	if (descriptor_regular(descriptor, uid, gid, NULL) != 0) {
		saved = errno;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}
	if (WIFI_STORE_FAIL(WIFI_STORE_TEST_LOCK_ACQUIRE) != 0 ||
	    acquire_record_lock(descriptor, type) != 0) {
		saved = errno;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}
	if (descriptor_regular(descriptor, uid, gid, &descriptor_status) != 0 ||
	    named_inode_matches(directory, WIFI_STORE_LOCK_TARGET,
	    &descriptor_status, uid, gid) != 0) {
		saved = errno;
		(void)release_record_lock(descriptor);
		(void)close(descriptor);
		errno = saved;
		return -1;
	}
	return descriptor;
}

static int
read_checked_file(int directory, const char *name, uid_t uid, gid_t gid,
		  unsigned char **result, size_t *result_length,
		  struct stat *result_status,
		  enum wifi_store_test_stage open_stage,
		  enum wifi_store_test_stage read_stage)
{
	struct stat status;
	unsigned char *buffer = NULL;
	size_t offset = 0, capacity;
	int descriptor = -1, saved;
	ssize_t count;

	(void)open_stage;
	(void)read_stage;
	*result = NULL;
	*result_length = 0;
	if (WIFI_STORE_FAIL(open_stage) != 0)
		return -1;
	descriptor = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	if (descriptor_regular(descriptor, uid, gid, &status) != 0)
		goto failed;
	if (status.st_size < 0 || (uint64_t)status.st_size > WIFI_CONF_FILE_MAX) {
		errno = EFBIG;
		goto failed;
	}
	capacity = (size_t)status.st_size;
	buffer = malloc(capacity + 1U);
	if (buffer == NULL)
		goto failed;
	if (WIFI_STORE_FAIL(read_stage) != 0)
		goto failed;
	while (offset < capacity) {
		count = read(descriptor, buffer + offset, capacity - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EIO;
			goto failed;
		}
		offset += (size_t)count;
	}
	count = read(descriptor, buffer + capacity, 1U);
	if (count < 0 && errno == EINTR)
		count = read(descriptor, buffer + capacity, 1U);
	if (count != 0) {
		if (count > 0)
			errno = EFBIG;
		goto failed;
	}
	if (close(descriptor) != 0)
		goto failed_closed;
	descriptor = -1;
	*result = buffer;
	*result_length = capacity;
	if (result_status != NULL)
		*result_status = status;
	return 0;

failed:
	saved = errno;
	(void)close(descriptor);
	errno = saved;
failed_closed:
	saved = errno;
	if (buffer != NULL) {
		wifi_conf_explicit_clear(buffer, capacity + 1U);
		free(buffer);
	}
	errno = saved;
	return -1;
}

static int
write_all(int descriptor, const unsigned char *bytes, size_t length)
{
	size_t offset = 0;

	while (offset < length) {
		ssize_t count = write(descriptor, bytes + offset, length - offset);

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
open_unique_temporary(int directory, uid_t uid, gid_t gid, char *name,
		      size_t capacity, struct stat *result_status)
{
	struct timespec now;
	unsigned attempt;
	int descriptor, saved;
	unsigned long nonce = 0;

	if (WIFI_STORE_FAIL(WIFI_STORE_TEST_TEMP_CREATE) != 0)
		return -1;
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
		nonce = (unsigned long)now.tv_nsec;
	for (attempt = 0; attempt < WIFI_STORE_TEMP_ATTEMPTS; attempt++) {
		if (snprintf(name, capacity, ".wifi.conf.tmp.%ld.%lu.%u",
		    (long)getpid(), nonce, attempt) >= (int)capacity) {
			errno = ENAMETOOLONG;
			return -1;
		}
		descriptor = openat(directory, name,
		    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (descriptor < 0 && errno == EEXIST)
			continue;
		if (descriptor < 0)
			return -1;
		if (fchmod(descriptor, 0600) == 0 &&
		    fchown(descriptor, uid, gid) == 0 &&
		    descriptor_regular(descriptor, uid, gid, result_status) == 0)
			return descriptor;
		saved = errno;
		(void)close(descriptor);
		(void)unlinkat(directory, name, 0);
		errno = saved;
		return -1;
	}
	errno = EEXIST;
	return -1;
}

static int
unlink_temporary(int directory, const char *name,
		 const struct stat *expected, uid_t uid, gid_t gid,
		 int force_failure)
{
	if (named_inode_matches(directory, name, expected, uid, gid) != 0)
		return -1;
#ifdef WIFI_STORE_TESTING
	if (force_failure)
		/* Exercise a real failing unlinkat call without deleting the file. */
		return unlinkat(directory, name, AT_REMOVEDIR);
#else
	(void)force_failure;
#endif
	return unlinkat(directory, name, 0);
}

static int
sanitize_temporary(int directory, const char *name,
		   const struct stat *expected, uid_t uid, gid_t gid)
{
	struct stat status;
	int descriptor, result = -1, saved;

	descriptor = openat(directory, name,
	    O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	if (descriptor_regular(descriptor, uid, gid, &status) != 0)
		goto done;
	if (!same_inode(&status, expected)) {
		errno = EBUSY;
		goto done;
	}
	if (ftruncate(descriptor, 0) != 0 || fsync(descriptor) != 0)
		goto done;
	result = 0;

done:
	saved = errno;
	if (close(descriptor) != 0 && result == 0) {
		saved = errno;
		result = -1;
	}
	errno = saved;
	return result;
}

static int
remove_temporary(int directory, const char *name,
		 const struct stat *expected, uid_t uid, gid_t gid,
		 int *sanitized)
{
	unsigned attempt;
	int force_failure = 0, saved;

	*sanitized = 0;
	if (WIFI_STORE_FAIL(WIFI_STORE_TEST_CLEANUP) != 0)
		force_failure = 1;
	if (unlink_temporary(directory, name, expected, uid, gid,
	    force_failure) == 0 || errno == ENOENT)
		return 0;
	/* Once removal fails, erase and synchronize the credential bytes. */
	if (sanitize_temporary(directory, name, expected, uid, gid) == 0)
		*sanitized = 1;
	else {
		saved = errno;
		errno = saved;
		return -1;
	}
	for (attempt = 0; attempt < 3U; attempt++) {
		if (unlink_temporary(directory, name, expected, uid, gid,
		    force_failure) == 0 || errno == ENOENT)
			return 0;
		if (errno == EINTR)
			continue;
	}
	return -1;
}

static int
finish_lock(int descriptor, int failed, int *saved_errno, char *error,
	    size_t error_capacity)
{
	int unlock_error = 0, close_error = 0;

	if (descriptor < 0)
		return failed ? -1 : 0;
	if (release_record_lock(descriptor) != 0 ||
	    WIFI_STORE_FAIL(WIFI_STORE_TEST_UNLOCK) != 0)
		unlock_error = errno;
	if (close(descriptor) != 0)
		close_error = errno;
	if (!failed && (unlock_error != 0 || close_error != 0)) {
		*saved_errno = unlock_error != 0 ? unlock_error : close_error;
		return store_error(error, error_capacity, *saved_errno,
		    unlock_error != 0 ? "unlock" : "close lock", 0);
	}
	if (failed)
		errno = *saved_errno != 0 ? *saved_errno : EIO;
	return failed ? -1 : 0;
}

int
wifi_store_load_at(int directory, const char *target, uid_t uid, gid_t gid,
		   struct wifi_conf_model *model, char *error,
		   size_t error_capacity)
{
	struct wifi_conf_model loaded;
	unsigned char *input = NULL;
	size_t input_length = 0;
	int lock_descriptor = -1, failed = 0, saved = 0, finish_result;

	wifi_conf_model_init(&loaded);
	if (error != NULL && error_capacity != 0)
		error[0] = '\0';
	if (target == NULL || model == NULL ||
	    descriptor_store_directory(directory, uid, NULL) != 0)
		return store_error(error, error_capacity,
		    errno != 0 ? errno : EINVAL, "invalid load request", 0);
	lock_descriptor = open_lock(directory, uid, gid, F_RDLCK);
	if (lock_descriptor < 0)
		return store_error(error, error_capacity, errno, "open or lock", 0);
	if (read_checked_file(directory, target, uid, gid, &input,
	    &input_length, NULL, WIFI_STORE_TEST_TARGET_OPEN,
	    WIFI_STORE_TEST_TARGET_READ) != 0) {
		failed = 1;
		saved = errno;
	} else if (WIFI_STORE_FAIL(WIFI_STORE_TEST_PARSE) != 0 ||
	    wifi_conf_parse(input, input_length, &loaded, error,
	    error_capacity) != 0) {
		failed = 1;
		saved = errno;
	}
	if (input != NULL) {
		wifi_conf_explicit_clear(input, input_length + 1U);
		free(input);
	}
	if (failed && (error == NULL || error_capacity == 0 || error[0] == '\0'))
		(void)store_error(error, error_capacity, saved,
		    "read credential store", 0);
	finish_result = finish_lock(lock_descriptor, failed, &saved, error,
	    error_capacity);
	if (finish_result == 0) {
		wifi_conf_model_clear(model);
		*model = loaded;
		wifi_conf_model_clear(&loaded);
		return 0;
	}
	wifi_conf_model_clear(&loaded);
	return -1;
}

int
wifi_store_set_key_at(int directory, const char *target, uid_t uid, gid_t gid,
		      const void *ssid, size_t ssid_length,
		      const void *passphrase, size_t passphrase_length,
		      int automatic, char *error, size_t error_capacity)
{
	struct wifi_conf_model *model = NULL, staged_model;
	struct stat target_status, staged_status, temporary_status;
	unsigned char *input = NULL, *output = NULL, *staged = NULL;
	size_t input_length = 0, output_length = 0, staged_length = 0;
	char temporary[NAME_MAX + 1U] = "";
	int lock_descriptor = -1, output_descriptor = -1;
	int target_existed = 0, temporary_exists = 0, renamed = 0;
	int failed = 0, saved = 0;
	int cleanup_failed = 0, cleanup_error = 0, cleanup_sanitized = 0;

	wifi_conf_model_init(&staged_model);
	if (error != NULL && error_capacity != 0)
		error[0] = '\0';
	if (target == NULL ||
	    descriptor_store_directory(directory, uid, NULL) != 0) {
		saved = errno != 0 ? errno : EINVAL;
		return store_error(error, error_capacity, saved,
		    "invalid update request", 0);
	}
	if (wifi_conf_validate_profile(ssid, ssid_length, passphrase,
	    passphrase_length, error, error_capacity) != 0)
		return -1;
	if (automatic != 0 && automatic != 1) {
		errno = EINVAL;
		return store_error(error, error_capacity, errno,
		    "invalid update mode", 0);
	}
	lock_descriptor = open_lock(directory, uid, gid, F_WRLCK);
	if (lock_descriptor < 0)
		return store_error(error, error_capacity, errno, "open or lock", 0);
	model = malloc(sizeof(*model));
	output = malloc(WIFI_CONF_FILE_MAX + 1U);
	if (model == NULL || output == NULL) {
		failed = 1;
		saved = ENOMEM;
		goto done;
	}
	wifi_conf_model_init(model);
	if (read_checked_file(directory, target, uid, gid, &input,
	    &input_length, &target_status, WIFI_STORE_TEST_TARGET_OPEN,
	    WIFI_STORE_TEST_TARGET_READ) != 0) {
		if (errno != ENOENT) {
			failed = 1;
			saved = errno;
			goto done;
		}
	} else {
		target_existed = 1;
		if (WIFI_STORE_FAIL(WIFI_STORE_TEST_PARSE) != 0 ||
		    wifi_conf_parse(input, input_length, model, error,
		    error_capacity) != 0) {
			failed = 1;
			saved = errno;
			goto done;
		}
	}
	if (wifi_conf_set_key(model, ssid, ssid_length, passphrase,
	    passphrase_length, automatic, error, error_capacity) != 0 ||
	    wifi_conf_serialize(model, output, WIFI_CONF_FILE_MAX + 1U,
	    &output_length, error, error_capacity) != 0) {
		failed = 1;
		saved = errno;
		goto done;
	}
	output_descriptor = open_unique_temporary(directory, uid, gid, temporary,
	    sizeof(temporary), &temporary_status);
	if (output_descriptor < 0) {
		failed = 1;
		saved = errno;
		goto done;
	}
	temporary_exists = 1;
	if (WIFI_STORE_FAIL(WIFI_STORE_TEST_TEMP_WRITE) != 0 ||
	    write_all(output_descriptor, output, output_length) != 0) {
		failed = 1;
		saved = errno;
		goto done;
	}
	if (WIFI_STORE_FAIL(WIFI_STORE_TEST_TEMP_SYNC) != 0 ||
	    fsync(output_descriptor) != 0) {
		failed = 1;
		saved = errno;
		goto done;
	}
	if (close(output_descriptor) != 0) {
		failed = 1;
		saved = errno;
		output_descriptor = -1;
		goto done;
	}
	output_descriptor = -1;
	if (WIFI_STORE_FAIL(WIFI_STORE_TEST_TEMP_CLOSE) != 0) {
		failed = 1;
		saved = errno;
		goto done;
	}
	if (read_checked_file(directory, temporary, uid, gid, &staged,
	    &staged_length, &staged_status, WIFI_STORE_TEST_STAGE_OPEN,
	    WIFI_STORE_TEST_STAGE_READ) != 0) {
		failed = 1;
		saved = errno;
		goto done;
	}
	if (staged_length != output_length ||
	    memcmp(staged, output, output_length) != 0) {
		errno = EIO;
		failed = 1;
		saved = errno;
		goto done;
	}
	if (WIFI_STORE_FAIL(WIFI_STORE_TEST_STAGE_VALIDATE) != 0 ||
	    wifi_conf_parse(staged, staged_length, &staged_model, error,
	    error_capacity) != 0) {
		failed = 1;
		saved = errno;
		goto done;
	}
	if (named_inode_matches(directory, temporary, &staged_status, uid,
	    gid) != 0) {
		failed = 1;
		saved = errno;
		goto done;
	}
	if (target_existed) {
		if (named_inode_matches(directory, target, &target_status, uid,
		    gid) != 0) {
			failed = 1;
			saved = errno;
			goto done;
		}
	} else {
		struct stat unexpected;

		if (fstatat(directory, target, &unexpected,
		    AT_SYMLINK_NOFOLLOW) == 0) {
			errno = EBUSY;
			failed = 1;
			saved = errno;
			goto done;
		}
		if (errno != ENOENT) {
			failed = 1;
			saved = errno;
			goto done;
		}
	}
	{
		struct stat lock_status;

		if (descriptor_regular(lock_descriptor, uid, gid,
		    &lock_status) != 0 ||
		    named_inode_matches(directory, WIFI_STORE_LOCK_TARGET,
		    &lock_status, uid, gid) != 0) {
			failed = 1;
			saved = errno;
			goto done;
		}
	}
	if (WIFI_STORE_FAIL(WIFI_STORE_TEST_RENAME) != 0 ||
	    renameat(directory, temporary, directory, target) != 0) {
		failed = 1;
		saved = errno;
		goto done;
	}
	temporary_exists = 0;
	renamed = 1;
	if (WIFI_STORE_FAIL(WIFI_STORE_TEST_DIRECTORY_SYNC) != 0 ||
	    fsync(directory) != 0) {
		failed = 1;
		saved = errno;
		goto done;
	}

done:
	if (output_descriptor >= 0) {
		int close_result = close(output_descriptor);

		if (!failed && close_result != 0) {
			failed = 1;
			saved = errno;
		}
	}
	if (temporary_exists) {
		int cleanup_result = remove_temporary(directory, temporary,
		    &temporary_status, uid, gid, &cleanup_sanitized);

		if (cleanup_result != 0) {
			cleanup_failed = 1;
			cleanup_error = errno;
			if (!failed) {
				failed = 1;
				saved = cleanup_error;
			}
		}
	}
	if (input != NULL) {
		wifi_conf_explicit_clear(input, input_length + 1U);
		free(input);
	}
	if (staged != NULL) {
		wifi_conf_explicit_clear(staged, staged_length + 1U);
		free(staged);
	}
	if (output != NULL) {
		wifi_conf_explicit_clear(output, WIFI_CONF_FILE_MAX + 1U);
		free(output);
	}
	if (model != NULL) {
		wifi_conf_model_clear(model);
		free(model);
	}
	wifi_conf_model_clear(&staged_model);
	if (cleanup_failed)
		(void)store_cleanup_error(error, error_capacity,
		    saved != 0 ? saved : EIO,
		    cleanup_error != 0 ? cleanup_error : EIO,
		    cleanup_sanitized);
	else if (failed &&
	    (error == NULL || error_capacity == 0 || error[0] == '\0'))
		(void)store_error(error, error_capacity,
		    saved != 0 ? saved : EIO,
		    renamed ? "synchronize credential directory" :
		    "publish credential store", renamed);
	return finish_lock(lock_descriptor, failed, &saved, error,
	    error_capacity);
}

static int
decimal_id(const unsigned char *text, size_t length, unsigned long *result)
{
	unsigned long value = 0;
	size_t index;

	if (length == 0)
		return -1;
	for (index = 0; index < length; index++) {
		unsigned digit;

		if (text[index] < '0' || text[index] > '9')
			return -1;
		digit = (unsigned)(text[index] - '0');
		if (value > (ULONG_MAX - digit) / 10UL)
			return -1;
		value = value * 10UL + digit;
	}
	*result = value;
	return 0;
}

static int
strict_passwd_unique(uid_t uid, const struct passwd *selected)
{
	unsigned char *bytes = NULL;
	size_t used = 0, line_start = 0, matches = 0;
	int descriptor = -1, saved = 0, result = -1;

	descriptor = open("/etc/passwd", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	bytes = malloc(WIFI_STORE_PASSWD_FILE_MAX + 1U);
	if (bytes == NULL)
		goto done;
	while (used <= WIFI_STORE_PASSWD_FILE_MAX) {
		ssize_t count = read(descriptor, bytes + used,
		    WIFI_STORE_PASSWD_FILE_MAX + 1U - used);

		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			goto done;
		if (count == 0)
			break;
		used += (size_t)count;
	}
	if (used > WIFI_STORE_PASSWD_FILE_MAX) {
		errno = EFBIG;
		goto done;
	}
	while (line_start < used) {
		size_t field_start[7], field_length[7];
		size_t cursor, line_end, field = 0, start;
		unsigned long record_uid, record_gid;

		for (line_end = line_start; line_end < used &&
		     bytes[line_end] != '\n'; line_end++) {
			if (bytes[line_end] == '\0' || bytes[line_end] == '\r') {
				errno = EINVAL;
				goto done;
			}
		}
		if (line_end == used ||
		    line_end - line_start + 1U > WIFI_STORE_PASSWD_LINE_MAX) {
			errno = line_end == used ? EINVAL : EOVERFLOW;
			goto done;
		}
		if (line_end == line_start || bytes[line_start] == '#') {
			line_start = line_end + 1U;
			continue;
		}
		start = line_start;
		for (cursor = line_start; cursor <= line_end; cursor++) {
			if (cursor != line_end && bytes[cursor] != ':')
				continue;
			if (field == 7U) {
				errno = EINVAL;
				goto done;
			}
			field_start[field] = start;
			field_length[field] = cursor - start;
			field++;
			start = cursor + 1U;
		}
		if (field != 7U || field_length[0] == 0 ||
		    decimal_id(bytes + field_start[2], field_length[2],
		    &record_uid) != 0 ||
		    decimal_id(bytes + field_start[3], field_length[3],
		    &record_gid) != 0 || record_uid > UINT_MAX ||
		    record_gid > UINT_MAX) {
			errno = EINVAL;
			goto done;
		}
		if ((uid_t)record_uid == uid) {
			size_t selected_name = strlen(selected->pw_name);
			size_t selected_home = strlen(selected->pw_dir);

			matches++;
			if (selected->pw_gid != (gid_t)record_gid ||
			    field_length[0] != selected_name ||
			    memcmp(bytes + field_start[0], selected->pw_name,
			    selected_name) != 0 || field_length[5] == 0 ||
			    bytes[field_start[5]] != '/' ||
			    field_length[5] != selected_home ||
			    memcmp(bytes + field_start[5], selected->pw_dir,
			    selected_home) != 0) {
				errno = EINVAL;
				goto done;
			}
		}
		line_start = line_end + 1U;
	}
	if (matches != 1U) {
		errno = matches == 0 ? ENOENT : EEXIST;
		goto done;
	}
	result = 0;

done:
	saved = errno;
	(void)close(descriptor);
	if (bytes != NULL) {
		wifi_conf_explicit_clear(bytes, WIFI_STORE_PASSWD_FILE_MAX + 1U);
		free(bytes);
	}
	errno = saved;
	return result;
}

static int
passwd_store(uid_t uid, char *directory, size_t capacity, gid_t *gid)
{
	struct passwd entry, *found;
	char buffer[WIFI_STORE_PASSWD_BUFFER];
	size_t length;
	int result;

	result = getpwuid_r(uid, &entry, buffer, sizeof(buffer), &found);
	if (result != 0) {
		errno = result;
		return -1;
	}
	if (found == NULL) {
		errno = ENOENT;
		return -1;
	}
	if (found->pw_uid != uid || found->pw_name == NULL ||
	    found->pw_name[0] == '\0' || found->pw_dir == NULL ||
	    found->pw_dir[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	length = strnlen(found->pw_dir, capacity);
	if (length == 0 || length == capacity) {
		errno = length == 0 ? EINVAL : ENAMETOOLONG;
		return -1;
	}
	memcpy(directory, found->pw_dir, length + 1U);
	*gid = found->pw_gid;
	if (strict_passwd_unique(uid, found) != 0)
		return -1;
	return 0;
}

static int
open_store_directory(const char *path, uid_t uid)
{
	char copy[PATH_MAX];
	char *component, *separator;
	struct stat descriptor_status, named_status;
	size_t length, index;
	int current = -1, next = -1, saved;

	if (path == NULL || path[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	length = strnlen(path, sizeof(copy));
	if (length <= 1U || length == sizeof(copy) || path[length - 1U] == '/') {
		errno = length == sizeof(copy) ? ENAMETOOLONG : EINVAL;
		return -1;
	}
	for (index = 1U; index < length; index++)
		if ((path[index] == '/' && path[index - 1U] == '/') ||
		    (path[index] == '.' &&
		    (index == 1U || path[index - 1U] == '/') &&
		    (index + 1U == length || path[index + 1U] == '/' ||
		    (path[index + 1U] == '.' &&
		    (index + 2U == length || path[index + 2U] == '/'))))) {
			errno = EINVAL;
			return -1;
		}
	(void)snprintf(copy, sizeof(copy), "%s", path);
	current = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (current < 0)
		goto failed;
	component = copy + 1;
	for (;;) {
		separator = strchr(component, '/');
		if (separator != NULL)
			*separator = '\0';
		next = openat(current, component,
		    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (next < 0)
			goto failed;
		if (fstat(next, &descriptor_status) != 0)
			goto failed;
		if (!S_ISDIR(descriptor_status.st_mode)) {
			errno = ENOTDIR;
			goto failed;
		}
		if (separator == NULL) {
			if (descriptor_store_directory(next, uid,
			    &descriptor_status) != 0 ||
			    fstatat(current, component, &named_status,
			    AT_SYMLINK_NOFOLLOW) != 0)
				goto failed;
			if (!S_ISDIR(named_status.st_mode) ||
			    !same_inode(&descriptor_status, &named_status)) {
				errno = EBUSY;
				goto failed;
			}
		}
		if (close(current) != 0) {
			saved = errno;
			current = -1;
			errno = saved;
			goto failed;
		}
		current = next;
		next = -1;
		if (separator == NULL)
			break;
		component = separator + 1;
	}
	wifi_conf_explicit_clear(copy, sizeof(copy));
	return current;

failed:
	saved = errno != 0 ? errno : EIO;
	if (next >= 0)
		(void)close(next);
	if (current >= 0)
		(void)close(current);
	wifi_conf_explicit_clear(copy, sizeof(copy));
	errno = saved;
	return -1;
}

#ifdef WIFI_STORE_TESTING
int
wifi_store_test_open_directory(const char *path, uid_t uid)
{
	return open_store_directory(path, uid);
}
#endif

static int
select_store(struct selected_store *selected)
{
	char directory[PATH_MAX] = "";
	uid_t uid = geteuid();

	memset(selected, 0, sizeof(*selected));
	selected->directory = -1;
	selected->uid = uid;
	if (uid == 0) {
		selected->gid = 0;
		selected->target = WIFI_STORE_ROOT_TARGET;
		strcpy(directory, WIFI_STORE_ROOT_DIRECTORY);
	} else {
		if (passwd_store(uid, directory, sizeof(directory),
		    &selected->gid) != 0)
			return -1;
		selected->target = WIFI_STORE_USER_TARGET;
	}
	selected->directory = open_store_directory(directory, selected->uid);
	wifi_conf_explicit_clear(directory, sizeof(directory));
	if (selected->directory < 0)
		return -1;
	return 0;
}

int
wifi_store_set_key_for_effective_user(const char *ssid,
				      const char *passphrase, int automatic,
				      char *error, size_t error_capacity)
{
	struct selected_store selected;
	size_t ssid_length, passphrase_length;
	int result, saved;

	if (ssid == NULL || passphrase == NULL) {
		errno = EINVAL;
		return store_error(error, error_capacity, errno,
		    "invalid command fields", 0);
	}
	ssid_length = strnlen(ssid, WIFI_CONF_SSID_MAX + 1U);
	passphrase_length = strnlen(passphrase,
	    WIFI_CONF_PASSPHRASE_MAX + 1U);
	if (automatic != 0 && automatic != 1) {
		errno = EINVAL;
		return store_error(error, error_capacity, errno,
		    "invalid update mode", 0);
	}
	if (wifi_conf_validate_profile(ssid, ssid_length, passphrase,
	    passphrase_length, error, error_capacity) != 0)
		return -1;
	if (select_store(&selected) != 0)
		return store_error(error, error_capacity, errno,
		    "select credential store", 0);
	result = wifi_store_set_key_at(selected.directory, selected.target,
	    selected.uid, selected.gid, ssid, ssid_length, passphrase,
	    passphrase_length, automatic, error, error_capacity);
	saved = errno;
	if (close(selected.directory) != 0 && result == 0) {
		saved = errno;
		result = store_error(error, error_capacity, saved,
		    "close credential directory", 0);
	}
	errno = saved;
	return result;
}

int
wifi_store_load_for_effective_user(struct wifi_conf_model *model, char *error,
				   size_t error_capacity)
{
	struct selected_store selected;
	int result, saved;

	if (model == NULL) {
		errno = EINVAL;
		return store_error(error, error_capacity, errno,
		    "invalid load request", 0);
	}
	if (select_store(&selected) != 0)
		return store_error(error, error_capacity, errno,
		    "select credential store", 0);
	result = wifi_store_load_at(selected.directory, selected.target,
	    selected.uid, selected.gid, model, error, error_capacity);
	saved = errno;
	if (close(selected.directory) != 0 && result == 0) {
		saved = errno;
		wifi_conf_model_clear(model);
		result = store_error(error, error_capacity, saved,
		    "close credential directory", 0);
	}
	errno = saved;
	return result;
}
