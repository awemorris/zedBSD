/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library utmpx support.
 */

#include <utmpx.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t utmp_lock = PTHREAD_MUTEX_INITIALIZER;
static int utmp_fd = -1;
static off_t utmp_offset;
#if defined(ZEDBSD_DYNAMIC_LIBC)
static _Thread_local struct utmpx utmp_result;
#else
/*
 * The utmpx interfaces return implementation-owned storage and this file
 * serializes access with utmp_lock.  Static executables intentionally use the
 * shared object because their linker contract does not include PT_TLS.
 */
static struct utmpx utmp_result;
#endif

static int utmp_open(int writing);
static int utmp_record_read(off_t offset, struct utmpx *entry);
static int utmp_same_id(const struct utmpx *left, const struct utmpx *right);

/*
 * Implements the setutxent operation.
 */
void
setutxent(
	void)
{
	pthread_mutex_lock(&utmp_lock);
	utmp_offset = 0;
	pthread_mutex_unlock(&utmp_lock);
}

/*
 * Implements the getutxent operation.
 */
struct utmpx *
getutxent(
	void)
{
	int status;

	pthread_mutex_lock(&utmp_lock);

	/* Handles a failed utmp open operation. */
	if (utmp_open(0) != 0) {
		pthread_mutex_unlock(&utmp_lock);

		/* Reports that no result is available. */
		return NULL;
	}
	status = utmp_record_read(utmp_offset, &utmp_result);

	/* Checks the operation status. */
	if (status == 1)
		utmp_offset += (off_t)sizeof(utmp_result);
	pthread_mutex_unlock(&utmp_lock);

	/* Returns the computed result. */
	return status == 1 ? &utmp_result : NULL;
}

/*
 * Implements the endutxent operation.
 */
void
endutxent(
	void)
{
	pthread_mutex_lock(&utmp_lock);

	/* Handles the utmp fd condition. */
	if (utmp_fd >= 0)
		close(utmp_fd);
	utmp_fd = -1;
	utmp_offset = 0;
	pthread_mutex_unlock(&utmp_lock);
}

/*
 * Implements the getutxid operation.
 */
struct utmpx *
getutxid(
	const struct utmpx *key)
{
	struct utmpx *entry;

	/* Handles the key availability. */
	if (key == NULL) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}
	while ((entry = getutxent()) != NULL) {
		/* Handles the utmp same id condition. */
		if (utmp_same_id(entry, key))
			return entry;
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Implements the getutxline operation.
 */
struct utmpx *
getutxline(
	const struct utmpx *key)
{
	struct utmpx *entry;

	/* Handles the key availability. */
	if (key == NULL) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}
	while ((entry = getutxent()) != NULL) {
		/* Handles the entry condition. */
		if ((entry->ut_type == LOGIN_PROCESS ||
		     entry->ut_type == USER_PROCESS) &&
		    !strncmp(entry->ut_line, key->ut_line,
			     sizeof(entry->ut_line)))

			/* Returns the computed result. */
			return entry;
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Implements the pututxline operation.
 */
struct utmpx *
pututxline(
	const struct utmpx *entry)
{
	struct flock lock;
	struct utmpx current;
	off_t offset;
	int status;

	offset = 0;

	/* Handles the entry availability. */
	if (entry == NULL) {
		errno = EINVAL;

		/* Reports that no result is available. */
		return NULL;
	}
	pthread_mutex_lock(&utmp_lock);

	/* Handles a failed utmp open operation. */
	if (utmp_open(1) != 0)
		goto fail;
	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;

	/* Handles a failed fcntl operation. */
	if (fcntl(utmp_fd, F_SETLKW, &lock) != 0)
		goto fail;

	/* Process input until it is exhausted. */
	while ((status = utmp_record_read(offset, &current)) == 1) {
		/* Handles the utmp same id condition. */
		if (utmp_same_id(&current, entry))
			break;
		offset += (off_t)sizeof(current);
	}

	/* Handles a failed pwrite operation. */
	if (status < 0 || pwrite(utmp_fd, entry, sizeof(*entry), offset) !=
			      (ssize_t)sizeof(*entry)) {
		lock.l_type = F_UNLCK;
		(void)fcntl(utmp_fd, F_SETLK, &lock);
		goto fail;
	}
	(void)fsync(utmp_fd);
	lock.l_type = F_UNLCK;
	(void)fcntl(utmp_fd, F_SETLK, &lock);
	utmp_result = *entry;
	pthread_mutex_unlock(&utmp_lock);

	/* Returns the computed result. */
	return &utmp_result;
fail:
	pthread_mutex_unlock(&utmp_lock);

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the utmp open operation. */
static int
utmp_open(
	int writing)
{
	/* Handles the utmp fd condition. */
	if (utmp_fd >= 0)
		return 0;
	utmp_fd = open(_PATH_UTMP, writing ? O_RDWR | O_CREAT : O_RDONLY, 0644);
	utmp_offset = 0;

	/* Returns the computed result. */
	return utmp_fd < 0 ? -1 : 0;
}

/* Supports the utmp record read operation. */
static int
utmp_record_read(
	off_t offset,
	struct utmpx *entry)
{
	ssize_t count;

	count = pread(utmp_fd, entry, sizeof(*entry), offset);

	/* Checks the remaining item count. */
	if (count == 0)
		return 0;

	/* Checks the remaining item count. */
	if (count != (ssize_t)sizeof(*entry)) {
		/* Checks the remaining item count. */
		if (count >= 0)
			errno = EIO;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the utmp same id operation. */
static int
utmp_same_id(
	const struct utmpx *left,
	const struct utmpx *right)
{
	int function_result;

	/* Handles the left condition. */
	if (left->ut_type == BOOT_TIME || right->ut_type == BOOT_TIME)
		return left->ut_type == right->ut_type;

	/* Computes the function result. */
	function_result = !memcmp(left->ut_id, right->ut_id, sizeof(left->ut_id));

	/* Returns the computed result. */
	return function_result;
}
