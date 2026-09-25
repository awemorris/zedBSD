/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library interfaces OpenBSD established.
 *
 * These are written from what each interface is defined to do, not from any
 * other system's source.  The names and shapes are shared on purpose, so
 * that software written against them compiles here unchanged.
 */

#include <sys/socket.h>
#include <uapi/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* Descriptors to consider when the system will not say how many there are. */
#define DESCRIPTOR_FALLBACK 1024

/* Characters a temporary name is built from. */
static const char name_alphabet[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

/*
 * Supports the descriptor limit operation.
 *
 * Reports one past the highest descriptor worth examining.
 */
static int
descriptor_limit(
	void)
{
	long limit;

	limit = sysconf(_SC_OPEN_MAX);

	/* Handles a system that declines to answer. */
	if (limit <= 0L || limit > (long)INT_MAX)
		return DESCRIPTOR_FALLBACK;

	/* Returns the computed result. */
	return (int)limit;
}

/*
 * Supports the temporary name operation.
 *
 * Replaces the six placeholder characters that precede the suffix and offers
 * the name to the caller's maker until one is taken.  A name is only ever
 * offered with an exclusive creation, so two callers cannot agree on one.
 */
static int
temporary_name(
	char *path,
	int suffix,
	int directory)
{
	size_t length;
	size_t at;
	unsigned attempt;
	unsigned index;
	uint32_t draw;
	int result;

	/* Rejects a template without the placeholder in the right place. */
	if (path == NULL || suffix < 0) {
		errno = EINVAL;
		return -1;
	}
	length = strlen(path);
	if (length < 6U + (size_t)suffix) {
		errno = EINVAL;
		return -1;
	}
	at = length - 6U - (size_t)suffix;
	if (memcmp(path + at, "XXXXXX", 6U) != 0) {
		errno = EINVAL;
		return -1;
	}

	/* Process each remaining element. */
	for (attempt = 0; attempt < TMP_MAX; attempt++) {
		draw = arc4random();
		for (index = 0; index < 6U; index++) {
			path[at + index] =
			    name_alphabet[draw % (sizeof(name_alphabet) - 1U)];
			draw = draw / (sizeof(name_alphabet) - 1U) ^
			       arc4random();
		}

		/* Takes the name, or tries another if it was already there. */
		if (directory)
			result = mkdir(path, 0700) == 0 ? 0 : -1;
		else
			result = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (result >= 0)
			return result;
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the mkstemps operation.
 */
int
mkstemps(
	char *path,
	int suffix)
{
	/* Returns the computed result. */
	return temporary_name(path, suffix, 0);
}

/*
 * Implements the mkdtemp operation.
 */
char *
mkdtemp(
	char *path)
{
	/* Handles a failed temporary name operation. */
	if (temporary_name(path, 0, 1) != 0)
		return NULL;

	/* Returns the computed result. */
	return path;
}

/*
 * Implements the freezero operation.
 *
 * Erases the allocation before releasing it, so that a secret does not
 * outlive its owner in memory the allocator hands to somebody else.
 */
void
freezero(
	void *memory,
	size_t size)
{
	/* Handles the memory availability. */
	if (memory == NULL)
		return;
	explicit_bzero(memory, size);
	free(memory);
}

/*
 * Implements the getpagesize operation.
 */
int
getpagesize(
	void)
{
	long size;

	size = sysconf(_SC_PAGESIZE);

	/* Returns the computed result. */
	return size > 0L && size <= (long)INT_MAX ? (int)size : 4096;
}

/*
 * Implements the getdtablesize operation.
 */
int
getdtablesize(
	void)
{
	long size;

	size = sysconf(_SC_OPEN_MAX);

	/* Returns the computed result. */
	return size > 0L && size <= (long)INT_MAX ? (int)size : 0;
}

/*
 * Implements the closefrom operation.
 *
 * Closes every descriptor at or above the one named, which is what a program
 * does before handing itself to something else: it cannot enumerate what it
 * inherited, so it closes the whole tail.
 */
int
closefrom(
	int lowest)
{
	int limit;
	int descriptor;

	/* Rejects a descriptor number that cannot exist. */
	if (lowest < 0) {
		errno = EBADF;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	limit = descriptor_limit();
	for (descriptor = lowest; descriptor < limit; descriptor++)
		(void)close(descriptor);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the getdtablecount operation.
 */
int
getdtablecount(
	void)
{
	int limit;
	int descriptor;
	int count;

	/* Process each remaining element. */
	count = 0;
	limit = descriptor_limit();
	for (descriptor = 0; descriptor < limit; descriptor++)
		if (fcntl(descriptor, F_GETFD) != -1)
			count++;

	/* Returns the computed result. */
	return count;
}

/*
 * Implements the getpeereid operation.
 *
 * Reports who the other end of a connected local socket was when it
 * connected, which is the only identity a receiver can trust: the peer
 * cannot change it afterwards.
 */
int
getpeereid(
	int socket,
	uid_t *uid,
	gid_t *gid)
{
	struct kern_peercred credentials;
	socklen_t length;

	length = sizeof(credentials);

	/* Handles a failed getsockopt operation. */
	if (getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials,
		       &length) != 0)
		return -1;

	/* Handles an answer that is not the identity asked for. */
	if (length != sizeof(credentials)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the uid availability. */
	if (uid != NULL)
		*uid = (uid_t)credentials.euid;

	/* Handles the gid availability. */
	if (gid != NULL)
		*gid = (gid_t)credentials.egid;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the daemon operation.
 *
 * Leaves the caller's session and terminal behind, so that the surviving
 * process is not killed with the login that started it and cannot read or
 * write the terminal by accident.
 */
int
daemon(
	int nochdir,
	int noclose)
{
	int null_device;

	/* Dispatch the selected outcome of the fork. */
	switch (fork()) {
	case -1:

		/* Reports operation failure. */
		return -1;
	case 0:
		break;
	default:

		/* The parent leaves without running any exit handler. */
		_exit(0);
	}

	/* Handles a failed setsid operation. */
	if (setsid() == -1)
		return -1;

	/* Releases the directory the caller was started in. */
	if (!nochdir)
		(void)chdir("/");

	/* Handles the noclose condition. */
	if (noclose)
		return 0;
	null_device = open("/dev/null", O_RDWR);

	/* Handles a failed open operation. */
	if (null_device == -1)
		return -1;
	(void)dup2(null_device, STDIN_FILENO);
	(void)dup2(null_device, STDOUT_FILENO);
	(void)dup2(null_device, STDERR_FILENO);

	/* Keeps only the three standard descriptors. */
	if (null_device > STDERR_FILENO)
		(void)close(null_device);

	/* Reports successful completion. */
	return 0;
}

/*
 * Supports the days from civil operation.
 *
 * Counts the days from 1970-01-01 to the given date, by moving the start of
 * the year to March so that the leap day falls at the end of a year and the
 * month lengths repeat on a five-month cycle.
 */
static int64_t
days_from_civil(
	int64_t year,
	unsigned month,
	unsigned day)
{
	int64_t era;
	unsigned year_of_era;
	unsigned day_of_year;
	unsigned day_of_era;

	/* Moves January and February to the end of the preceding year. */
	year -= month <= 2U ? 1 : 0;
	era = (year >= 0 ? year : year - 399) / 400;
	year_of_era = (unsigned)(year - era * 400);
	day_of_year = (153U * (month + (month > 2U ? -3U : 9U)) + 2U) / 5U +
		      day - 1U;
	day_of_era = year_of_era * 365U + year_of_era / 4U -
		     year_of_era / 100U + day_of_year;

	/* Returns the computed result, shifted from 0000-03-01 to the epoch. */
	return era * 146097 + (int64_t)day_of_era - 719468;
}

/*
 * Implements the timegm operation.
 *
 * Reads the broken-down time as UTC, which is what mktime will not do, and
 * writes the normalised form back the way mktime does.
 */
time_t
timegm(
	struct tm *fields)
{
	struct tm normalised;
	int64_t year;
	int64_t month;
	int64_t days;
	int64_t seconds;
	time_t result;

	/* Handles the fields availability. */
	if (fields == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return (time_t)-1;
	}

	/* Carries the month into the year, so the day count sees a real date. */
	year = (int64_t)fields->tm_year + 1900;
	month = fields->tm_mon;
	year += month / 12;
	month %= 12;
	if (month < 0) {
		month += 12;
		year--;
	}

	/* Counts from the first of the month, then adds the rest. */
	days = days_from_civil(year, (unsigned)month + 1U, 1U) +
	       ((int64_t)fields->tm_mday - 1);
	seconds = days * 86400 + (int64_t)fields->tm_hour * 3600 +
		  (int64_t)fields->tm_min * 60 + (int64_t)fields->tm_sec;
	result = (time_t)seconds;

	/* Handles a moment the result cannot hold. */
	if ((int64_t)result != seconds) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return (time_t)-1;
	}

	/* Writes back the normalised date, as this call is defined to do. */
	if (gmtime_r(&result, &normalised) != NULL)
		*fields = normalised;

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the getgrouplist operation.
 *
 * Collects the groups a user belongs to: the one the password file names,
 * and every group that lists the user among its members.
 */
int
getgrouplist(
	const char *name,
	gid_t basegid,
	gid_t *groups,
	int *ngroups)
{
	struct group *entry;
	int capacity;
	int count;
	int member;
	int index;
	int truncated;

	/* Handles the arguments availability. */
	if (name == NULL || ngroups == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	capacity = *ngroups;
	count = 0;
	truncated = 0;

	/* The password file's group comes first, and is always included. */
	if (count < capacity && groups != NULL)
		groups[count] = basegid;
	else
		truncated = 1;
	count++;

	/* Process each element required by the operation. */
	setgrent();
	while ((entry = getgrent()) != NULL) {
		/* Skips the group already counted. */
		if (entry->gr_gid == basegid)
			continue;

		/* Skips a group this user is not named in. */
		member = 0;
		for (index = 0; entry->gr_mem != NULL &&
		     entry->gr_mem[index] != NULL; index++) {
			if (strcmp(entry->gr_mem[index], name) == 0) {
				member = 1;
				break;
			}
		}
		if (!member)
			continue;

		/* Records the group, or notes that it did not fit. */
		if (count < capacity && groups != NULL)
			groups[count] = entry->gr_gid;
		else
			truncated = 1;
		count++;
	}
	endgrent();

	/* Reports how many there are, whether or not they were all stored. */
	*ngroups = count;

	/* Returns the computed result. */
	return truncated ? -1 : count;
}

/*
 * Implements the user from uid operation.
 *
 * Names the user, or reports the number as text when there is no such user.
 * A caller that would rather see nothing at all passes a true nouser.
 */
const char *
user_from_uid(
	uid_t uid,
	int nouser)
{
	static char fallback[24];
	struct passwd *entry;

	entry = getpwuid(uid);

	/* Handles the entry availability. */
	if (entry != NULL && entry->pw_name != NULL)
		return entry->pw_name;

	/* Handles a caller that wants no answer rather than a number. */
	if (nouser)
		return NULL;
	(void)snprintf(fallback, sizeof(fallback), "%lu",
		       (unsigned long)uid);

	/* Returns the computed result. */
	return fallback;
}

/*
 * Implements the group from gid operation.
 */
const char *
group_from_gid(
	gid_t gid,
	int nogroup)
{
	static char fallback[24];
	struct group *entry;

	entry = getgrgid(gid);

	/* Handles the entry availability. */
	if (entry != NULL && entry->gr_name != NULL)
		return entry->gr_name;

	/* Handles a caller that wants no answer rather than a number. */
	if (nogroup)
		return NULL;
	(void)snprintf(fallback, sizeof(fallback), "%lu",
		       (unsigned long)gid);

	/* Returns the computed result. */
	return fallback;
}

/*
 * Implements the getdelim operation.
 *
 * Reads up to and including the delimiter, growing the caller's buffer to
 * fit.  The buffer and its size are published on every path that changes
 * them, including a failure, so the caller always owns what was allocated.
 */
ssize_t
getdelim(
	char **line,
	size_t *capacity,
	int delimiter,
	FILE *stream)
{
	char *buffer;
	char *grown;
	size_t size;
	size_t wanted;
	size_t used;
	int byte;

	/* Handles the arguments availability. */
	if (line == NULL || capacity == NULL || stream == NULL ||
	    delimiter < 0 || delimiter > 255) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	buffer = *line;
	size = buffer != NULL ? *capacity : 0;
	used = 0;

	/* Continue while the operation condition remains true. */
	for (;;) {
		byte = fgetc(stream);

		/* Stops at the end of the stream. */
		if (byte == EOF)
			break;

		/* Keeps room for this byte and the terminator that follows. */
		if (used + 2U > size) {
			wanted = size < 120U ? 120U : size * 2U;
			grown = realloc(buffer, wanted);

			/* Handles a failed realloc operation. */
			if (grown == NULL) {
				*line = buffer;
				*capacity = size;

				/* Reports operation failure. */
				return -1;
			}
			buffer = grown;
			size = wanted;
		}
		buffer[used++] = (char)byte;

		/* Stops once the delimiter itself has been stored. */
		if (byte == delimiter)
			break;
	}
	*line = buffer;
	*capacity = size;

	/* Reports the end of the stream, having read nothing. */
	if (used == 0U)
		return -1;
	buffer[used] = '\0';

	/* Returns the computed result. */
	return (ssize_t)used;
}

/*
 * Implements the getline operation.
 */
ssize_t
getline(
	char **line,
	size_t *capacity,
	FILE *stream)
{
	/* Returns the computed result. */
	return getdelim(line, capacity, '\n', stream);
}
