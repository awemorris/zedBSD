/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Independent syscall oracle for the read-only formatter frontend. */
#include "userland/base/common/format-file.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
static int step, fail_step, change_step, field, close_fail, live;
static struct stat identity;
static void require(int condition);
static int event(int expected);
static void status_at(struct stat *status);
static int validate(uint64_t bytes);
static int observed_verify(int fd, uint64_t bytes);
static int forbidden_write(int fd, uint64_t bytes);
static void reset(void);
static void run(int expected);

int mkfs_block_command(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	require(0);
	return 2;
}

#define main pristine_mkfs_main
#define ufs_format_validate_size validate
#define ufs_format_write forbidden_write
#define ufs_format_verify observed_verify
#define ufs_format_feature_write forbidden_write
#define ufs_format_feature_verify observed_verify
#define ufs_format_pristine observed_verify
#define ufs_format_feature_pristine observed_verify
#include "userland/base/mkfs/main.c"
#undef main
#define main pristine_swap_main
#define swap_format_validate_size validate
#define swap_format_write forbidden_write
#define swap_format_verify observed_verify
#define swap_format_pristine observed_verify
#include "userland/base/mkswap/main.c"
#undef main

/* Fail immediately on a protocol violation, independently of production branches. */
static void require(int condition)
{
	checks++;
	if (!condition) {
		fprintf(stderr, "pristine frontend failed check %u step %d\n", checks, step);
		exit(1);
	}
}

/* Advance only through the specified public read-only protocol. */
static int event(int expected)
{
	step++;
	require(step == expected);
	if (step == fail_step) {
		errno = EIO;
		return -1;
	}
	return 0;
}

/* Substitute one identity field at an independently selected checkpoint. */
static void status_at(struct stat *status)
{
	*status = identity;
	if (step != change_step)
		return;
	switch (field) {
	case 0: status->st_dev++; break;
	case 1: status->st_ino++; break;
	case 2: status->st_size++; break;
	case 3: status->st_nlink++; break;
	case 4: status->st_mode = S_IFIFO | 0600; break;
	case 5: status->st_mode = S_IFLNK | 0600; break;
	case 6: status->st_mode = S_IFCHR | 0600; break;
	case 7: status->st_mode = S_IFDIR | 0600; break;
	}
}

int ff_test_lstat(const char *path, struct stat *status)
{
	int error;
	require(strcmp(path, "/staging/image") == 0);
	error = event(step == 0 ? 1 : 7);
	if (error < 0)
		return error;
	status_at(status);
	return 0;
}

int ff_test_fstat(int fd, struct stat *status)
{
	int error;
	require(fd == 47 && live);
	error = event(step == 3 ? 4 : 6);
	if (error < 0)
		return error;
	status_at(status);
	return 0;
}

int ff_test_open(const char *path, int flags, ...)
{
	int error;
	require(strcmp(path, "/staging/image") == 0 && !live);
	require(flags == (O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
	error = event(3);
	if (error < 0)
		return error;
	live = 1;
	return 47;
}

int ff_test_close(int fd)
{
	require(fd == 47 && live);
	live = 0;
	if (fail_step != 0 || change_step != 0)
		step = 7;
	if (event(8) < 0)
		return -1;
	if (close_fail) {
		errno = EBADF;
		return -1;
	}
	return 0;
}

int ff_test_ioctl(int fd, unsigned long request, ...)
{
	(void)fd;
	(void)request;
	require(0);
	return -1;
}

int ff_test_fsync(int fd)
{
	(void)fd;
	require(0);
	return -1;
}

static int validate(uint64_t bytes)
{
	require(bytes == 16384);
	if (event(2) < 0)
		return EIO;
	return 0;
}

static int observed_verify(int fd, uint64_t bytes)
{
	require(fd == 47 && live && bytes == 16384);
	if (event(5) < 0)
		return EIO;
	return 0;
}

static int forbidden_write(int fd, uint64_t bytes)
{
	(void)fd;
	(void)bytes;
	require(0);
	return EIO;
}

static void reset(void)
{
	step = fail_step = change_step = field = close_fail = live = 0;
	memset(&identity, 0, sizeof(identity));
	identity.st_mode = S_IFREG | 0400;
	identity.st_dev = 8;
	identity.st_ino = 12;
	identity.st_nlink = 1;
	identity.st_size = 16384;
}

static void run(int expected)
{
	uint64_t bytes;
	int error;
	bytes = 123;
	error = format_file_verify("/staging/image", validate, observed_verify, &bytes);
	require(error == expected && !live);
	require(bytes == (expected == 0 ? 16384 : 123));
}

int main(void)
{
	static const int checkpoints[] = { 4, 6, 7 };
	char *mkfs[] = { "mkfs", "-t", "ufs", "--verify-pristine", "/staging/image", NULL };
	char *feature[] = { "mkfs", "-t", "ufs", "--verify-pristine", "--profile=journal-snapshot", "/staging/image", NULL };
	char *swap[] = { "mkswap", "--verify-pristine", "/staging/image", NULL };
	int index, property, error;
	reset();
	run(0);
	require(step == 8);
	for (index = 1; index <= 8; index++) {
		reset();
		fail_step = index;
		run(EIO);
	}
	for (index = 0; index < 3; index++) {
		for (property = 0; property < 8; property++) {
			reset();
			change_step = checkpoints[index];
			field = property;
			run(EBUSY);
		}
	}
	for (property = 4; property < 8; property++) {
		reset();
		change_step = 1;
		field = property;
		run(EINVAL);
		require(step == 1);
	}
	reset();
	close_fail = 1;
	run(EBADF);
	reset();
	close_fail = 1;
	fail_step = 5;
	run(EIO);
	reset();
	error = pristine_mkfs_main(5, mkfs);
	require(error == 0 && step == 8);
	reset();
	error = pristine_mkfs_main(6, feature);
	require(error == 0 && step == 8);
	reset();
	error = pristine_swap_main(3, swap);
	require(error == 0 && step == 8);
	reset();
	feature[4] = "--verify-pristine";
	error = pristine_mkfs_main(6, feature);
	require(error == 2 && step == 0);
	printf("Pristine frontend: %u checks PASS\n", checks);
	return 0;
}
