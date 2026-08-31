/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
utmp_open(int writing)
{
	if (utmp_fd >= 0)
		return 0;
	utmp_fd = open(_PATH_UTMP, writing ? O_RDWR | O_CREAT : O_RDONLY, 0644);
	utmp_offset = 0;
	return utmp_fd < 0 ? -1 : 0;
}

static int
utmp_record_read(off_t offset, struct utmpx *entry)
{
	ssize_t count = pread(utmp_fd, entry, sizeof(*entry), offset);
	if (count == 0)
		return 0;
	if (count != (ssize_t)sizeof(*entry)) {
		if (count >= 0)
			errno = EIO;
		return -1;
	}
	return 1;
}

static int
utmp_same_id(const struct utmpx *left, const struct utmpx *right)
{
	if (left->ut_type == BOOT_TIME || right->ut_type == BOOT_TIME)
		return left->ut_type == right->ut_type;
	return !memcmp(left->ut_id, right->ut_id, sizeof(left->ut_id));
}

void
setutxent(void)
{
	pthread_mutex_lock(&utmp_lock);
	utmp_offset = 0;
	pthread_mutex_unlock(&utmp_lock);
}

struct utmpx *
getutxent(void)
{
	int status;
	pthread_mutex_lock(&utmp_lock);
	if (utmp_open(0) != 0) {
		pthread_mutex_unlock(&utmp_lock);
		return NULL;
	}
	status = utmp_record_read(utmp_offset, &utmp_result);
	if (status == 1)
		utmp_offset += (off_t)sizeof(utmp_result);
	pthread_mutex_unlock(&utmp_lock);
	return status == 1 ? &utmp_result : NULL;
}

void
endutxent(void)
{
	pthread_mutex_lock(&utmp_lock);
	if (utmp_fd >= 0)
		close(utmp_fd);
	utmp_fd = -1;
	utmp_offset = 0;
	pthread_mutex_unlock(&utmp_lock);
}

struct utmpx *
getutxid(const struct utmpx *key)
{
	struct utmpx *entry;
	if (key == NULL) {
		errno = EINVAL;
		return NULL;
	}
	while ((entry = getutxent()) != NULL)
		if (utmp_same_id(entry, key))
			return entry;
	return NULL;
}

struct utmpx *
getutxline(const struct utmpx *key)
{
	struct utmpx *entry;
	if (key == NULL) {
		errno = EINVAL;
		return NULL;
	}
	while ((entry = getutxent()) != NULL)
		if ((entry->ut_type == LOGIN_PROCESS ||
		     entry->ut_type == USER_PROCESS) &&
		    !strncmp(entry->ut_line, key->ut_line,
			     sizeof(entry->ut_line)))
			return entry;
	return NULL;
}

struct utmpx *
pututxline(const struct utmpx *entry)
{
	struct flock lock;
	struct utmpx current;
	off_t offset = 0;
	int status;

	if (entry == NULL) {
		errno = EINVAL;
		return NULL;
	}
	pthread_mutex_lock(&utmp_lock);
	if (utmp_open(1) != 0)
		goto fail;
	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	if (fcntl(utmp_fd, F_SETLKW, &lock) != 0)
		goto fail;
	while ((status = utmp_record_read(offset, &current)) == 1) {
		if (utmp_same_id(&current, entry))
			break;
		offset += (off_t)sizeof(current);
	}
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
	return &utmp_result;
fail:
	pthread_mutex_unlock(&utmp_lock);
	return NULL;
}
