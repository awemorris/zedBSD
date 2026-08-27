/*
 * WS012 SVC-T002 stable-lock and failure-atomic rc.conf persistence fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#define _POSIX_C_SOURCE 200809L

#include "userland/base/service/rcconf.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum failure_mode {
	FAILURE_NONE,
	FAILURE_WRITE,
	FAILURE_FSYNC,
	FAILURE_RENAME,
	WRITE_EINTR_ONCE,
	WRITE_SHORT_ONCE,
};

static enum failure_mode failure_mode;
static unsigned unlink_calls;

ssize_t
rcconf_test_write(int descriptor, const void *buffer, size_t length)
{
	if (failure_mode == FAILURE_WRITE) {
		errno = EIO;
		return -1;
	}
	if (failure_mode == WRITE_EINTR_ONCE) {
		failure_mode = FAILURE_NONE;
		errno = EINTR;
		return -1;
	}
	if (failure_mode == WRITE_SHORT_ONCE && length > 1U) {
		failure_mode = FAILURE_NONE;
		return write(descriptor, buffer, length / 2U);
	}
	return write(descriptor, buffer, length);
}

int
rcconf_test_fsync(int descriptor)
{
	if (failure_mode == FAILURE_FSYNC) {
		errno = EIO;
		return -1;
	}
	return fsync(descriptor);
}

int
rcconf_test_rename(const char *old_path, const char *new_path)
{
	if (failure_mode == FAILURE_RENAME) {
		errno = EIO;
		return -1;
	}
	return rename(old_path, new_path);
}

int
rcconf_test_unlink(const char *path)
{
	unlink_calls++;
	return unlink(path);
}

static void
require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "SVC-T002: %s (errno=%d)\n", message, errno);
		exit(1);
	}
}

static void
write_initial(const char *path)
{
	struct rcconf_model model;
	FILE *stream;

	rcconf_model_init(&model);
	strcpy(model.hostname, "zedbsd");
	require(rcconf_model_set_enabled(&model, "cron", 0) == 0 &&
		    rcconf_model_set_enabled(&model, "ntpdate", 0) == 0,
		"construct initial model");
	stream = fopen(path, "w");
	require(stream != NULL && rcconf_write(stream, &model) == 0 &&
		    fflush(stream) == 0 && fsync(fileno(stream)) == 0 &&
		    fclose(stream) == 0,
		"write initial configuration");
}

static unsigned char *
read_file(const char *path, size_t *length)
{
	FILE *stream = fopen(path, "rb");
	unsigned char *data;
	long size;

	require(stream != NULL && fseek(stream, 0, SEEK_END) == 0,
		"open authoritative file");
	size = ftell(stream);
	require(size >= 0 && fseek(stream, 0, SEEK_SET) == 0,
		"measure authoritative file");
	data = malloc((size_t)size + 1U);
	require(data != NULL, "allocate authoritative snapshot");
	require(fread(data, 1, (size_t)size, stream) == (size_t)size &&
		    fclose(stream) == 0,
		"read authoritative snapshot");
	data[size] = '\0';
	*length = (size_t)size;
	return data;
}

static void
require_no_temporary(const char *directory)
{
	DIR *stream = opendir(directory);
	struct dirent *entry;

	require(stream != NULL, "open fixture directory");
	while ((entry = readdir(stream)) != NULL) {
		if (strncmp(entry->d_name, "rc.conf.tmp.", 12) == 0) {
			fprintf(stderr, "SVC-T002: leaked temporary %s\n",
				entry->d_name);
			exit(1);
		}
	}
	require(closedir(stream) == 0, "close fixture directory");
}

static void
read_exact(int descriptor, void *buffer, size_t length)
{
	unsigned char *cursor = buffer;
	size_t offset = 0;

	while (offset < length) {
		ssize_t result =
		    read(descriptor, cursor + offset, length - offset);

		require(result > 0, "read child synchronization record");
		offset += (size_t)result;
	}
}

static void
child_writer(const char *path, const char *service, int lock_descriptor,
	     int ready_descriptor, int result_descriptor)
{
	char record;
	int result;

	(void)close(lock_descriptor);
	record = 'R';
	if (write(ready_descriptor, &record, 1) != 1)
		_exit(120);
	result = rcconf_set_enabled(path, service, 1);
	record = result == 0 ? '0' : '1';
	if (write(result_descriptor, &record, 1) != 1)
		_exit(121);
	_exit(result == 0 ? 0 : 122);
}

static void
test_lock_and_two_writers(const char *path, const char *lock_path)
{
	struct flock lock;
	struct stat config_before, config_after, lock_before, lock_after;
	struct rcconf_model model;
	struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
	int ready[2], result[2], lock_descriptor, flags, status;
	pid_t first, second;
	char records[2];
	int enabled;

	lock_descriptor = open(lock_path, O_RDWR | O_CREAT, 0600);
	require(lock_descriptor >= 0, "open stable lock");
	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	require(fcntl(lock_descriptor, F_SETLK, &lock) == 0,
		"take parent stable lock");
	require(stat(path, &config_before) == 0 &&
		    fstat(lock_descriptor, &lock_before) == 0,
		"stat initial lock/configuration");
	require(pipe(ready) == 0 && pipe(result) == 0,
		"create writer synchronization pipes");
	first = fork();
	require(first >= 0, "fork first writer");
	if (first == 0) {
		close(ready[0]);
		close(result[0]);
		child_writer(path, "cron", lock_descriptor, ready[1],
			     result[1]);
	}
	second = fork();
	require(second >= 0, "fork second writer");
	if (second == 0) {
		close(ready[0]);
		close(result[0]);
		child_writer(path, "ntpdate", lock_descriptor, ready[1],
			     result[1]);
	}
	close(ready[1]);
	close(result[1]);
	read_exact(ready[0], records, sizeof(records));
	require(records[0] == 'R' && records[1] == 'R',
		"writers reached lock acquisition");
	flags = fcntl(result[0], F_GETFL);
	require(flags >= 0 &&
		    fcntl(result[0], F_SETFL, flags | O_NONBLOCK) == 0,
		"make result pipe nonblocking");
	(void)nanosleep(&delay, NULL);
	errno = 0;
	require(read(result[0], records, 1) < 0 && errno == EAGAIN,
		"writer bypassed stable lock");
	lock.l_type = F_UNLCK;
	require(fcntl(lock_descriptor, F_SETLK, &lock) == 0,
		"release parent stable lock");
	require(fcntl(result[0], F_SETFL, flags) == 0,
		"restore blocking result pipe");
	read_exact(result[0], records, sizeof(records));
	require(records[0] == '0' && records[1] == '0',
		"concurrent writer failed");
	require(waitpid(first, &status, 0) == first && WIFEXITED(status) &&
		    WEXITSTATUS(status) == 0,
		"first writer exit");
	require(waitpid(second, &status, 0) == second && WIFEXITED(status) &&
		    WEXITSTATUS(status) == 0,
		"second writer exit");
	close(ready[0]);
	close(result[0]);
	close(lock_descriptor);

	require(rcconf_load(path, &model) == 0 &&
		    rcconf_service_enabled(&model, "cron", &enabled) == 0 &&
		    enabled == 1 &&
		    rcconf_service_enabled(&model, "ntpdate", &enabled) == 0 &&
		    enabled == 1,
		"two writers lost a nonconflicting update");
	require(stat(path, &config_after) == 0 &&
		    stat(lock_path, &lock_after) == 0,
		"stat final lock/configuration");
	require(config_before.st_ino != config_after.st_ino,
		"configuration was not replaced by rename");
	require(lock_before.st_dev == lock_after.st_dev &&
		    lock_before.st_ino == lock_after.st_ino,
		"stable lock inode changed across rename");
	require((config_after.st_mode & 0777) == 0644 &&
		    (lock_after.st_mode & 0777) == 0600,
		"configuration or lock mode is incorrect");
}

static void
test_failure_preservation(const char *directory, const char *path,
			  enum failure_mode mode, const char *name)
{
	unsigned char *before, *after;
	size_t before_length, after_length;
	unsigned before_unlink = unlink_calls;

	before = read_file(path, &before_length);
	failure_mode = mode;
	errno = 0;
	require(rcconf_set_enabled(path, name, 1) != 0 && errno == EIO,
		"injected persistence failure was not reported");
	failure_mode = FAILURE_NONE;
	after = read_file(path, &after_length);
	require(before_length == after_length &&
		    memcmp(before, after, before_length) == 0,
		"failure changed the authoritative file");
	require(unlink_calls == before_unlink + 1U,
		"owned temporary was not cleaned after failure");
	require_no_temporary(directory);
	free(after);
	free(before);
}

int
main(void)
{
	char directory[] = "/tmp/zedbsd-rcconf-persist-XXXXXX";
	char path[512], lock_path[520];
	struct rcconf_model model;
	int enabled;

	require(mkdtemp(directory) != NULL, "create persistence directory");
	require(snprintf(path, sizeof(path), "%s/rc.conf", directory) <
			(int)sizeof(path) &&
		    snprintf(lock_path, sizeof(lock_path), "%s.lock", path) <
			(int)sizeof(lock_path),
		"format persistence paths");
	write_initial(path);
	test_lock_and_two_writers(path, lock_path);
	test_failure_preservation(directory, path, FAILURE_WRITE,
				  "write-failed");
	test_failure_preservation(directory, path, FAILURE_FSYNC,
				  "sync-failed");
	test_failure_preservation(directory, path, FAILURE_RENAME,
				  "rename-failed");

	failure_mode = WRITE_EINTR_ONCE;
	require(rcconf_set_enabled(path, "eintr-retry", 1) == 0 &&
		    failure_mode == FAILURE_NONE,
		"interrupted write was not retried");
	failure_mode = WRITE_SHORT_ONCE;
	require(rcconf_set_enabled(path, "short-write", 1) == 0 &&
		    failure_mode == FAILURE_NONE,
		"short write was not completed");
	require(
	    rcconf_load(path, &model) == 0 &&
		rcconf_service_enabled(&model, "eintr-retry", &enabled) == 0 &&
		enabled == 1 &&
		rcconf_service_enabled(&model, "short-write", &enabled) == 0 &&
		enabled == 1,
	    "successful retry updates missing");
	require_no_temporary(directory);
	require(unlink(path) == 0 && unlink(lock_path) == 0 &&
		    rmdir(directory) == 0,
		"remove persistence fixtures");
	puts("WS012 rc.conf persistence: PASS");
	return 0;
}
