/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clocks and the local time zone of the operating-system layer on POSIX systems.
 */

#include "base/os-time.h"
#include "base/base.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* How many nanoseconds make one second. */
#define OS_NANOSECONDS_PER_SECOND	1000000000LL

/* The name given when the system supplies no abbreviation for the zone. */
#define OS_ZONE_NAME_DEFAULT		"UTC"

/* The bits of a UTF-8 byte that tell a continuation byte from the first byte of a character. */
#define OS_UTF8_CONTINUATION_MASK	0xC0U

/* The value those bits have in a continuation byte. */
#define OS_UTF8_CONTINUATION_BITS	0x80U

/* Forward declarations. */
static int64_t os_time_clamp(int64_t seconds);
static int os_time_break_down(int64_t seconds, struct tm *broken);
static size_t os_time_name_length(const char *name);

/*
 * Reports a monotonic time in nanoseconds.
 *
 * It never goes back and does not follow changes of the wall clock; its
 * zero is arbitrary but fixed for the process.  It is 0 when the system
 * has no monotonic clock.
 */
uint64_t
wb_os_monotonic_ns(
	void)
{
	struct timespec now;
	uint64_t nanoseconds;
	int status;

	/* Reads the system's monotonic clock. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0;

	/* Joins the seconds and the nanoseconds into one count of nanoseconds. */
	nanoseconds = (uint64_t)now.tv_sec * (uint64_t)OS_NANOSECONDS_PER_SECOND;
	nanoseconds += (uint64_t)now.tv_nsec;

	/* Succeeded: the count since the clock's arbitrary zero. */
	return nanoseconds;
}

/*
 * Reports the wall clock in nanoseconds since 1970-01-01 00:00 UTC.
 *
 * A time too far from 1970 for 64 bits is the nearest value that fits; it is
 * 0 when the system has no wall clock.
 */
int64_t
wb_os_wall_ns(
	void)
{
	struct timespec now;
	int64_t nanoseconds;
	int64_t limit;
	int status;

	/* Reads the system's wall clock. */
	status = clock_gettime(CLOCK_REALTIME, &now);
	if (status != 0)
		return 0;

	/* Keeps a time that is too late for 64 bits of nanoseconds at the latest one. */
	limit = INT64_MAX / OS_NANOSECONDS_PER_SECOND - 1;
	if ((int64_t)now.tv_sec > limit)
		return INT64_MAX;

	/* Keeps a time that is too early for 64 bits of nanoseconds at the earliest one. */
	limit = INT64_MIN / OS_NANOSECONDS_PER_SECOND + 1;
	if ((int64_t)now.tv_sec < limit)
		return INT64_MIN;

	/* Joins the seconds and the nanoseconds into one count of nanoseconds. */
	nanoseconds = (int64_t)now.tv_sec * OS_NANOSECONDS_PER_SECOND;
	nanoseconds += (int64_t)now.tv_nsec;

	/* Succeeded: the count since 1970. */
	return nanoseconds;
}

/*
 * Reads the system's local time zone again.
 *
 * The system keeps the zone until it is told to look again, so the call
 * makes later conversions follow a changed zone setting.
 */
void
wb_os_time_zone_start(
	void)
{
	/* Makes the C library read the zone setting again. */
	tzset();
}

/*
 * Reports the local time's offset from UTC at an instant.
 *
 * The offset is in seconds east of UTC and includes daylight saving when it
 * applies; an instant outside the supported years is answered with the
 * offset of the nearest end.
 */
int
wb_os_local_offset(
	int64_t seconds,
	long *offset)
{
	struct tm broken;
	int error;

	/* Refuses a call that names no place for the offset. */
	if (offset == NULL)
		return EINVAL;

	/* Leaves the output in its failure state until the conversion has worked. */
	*offset = 0;

	/* Splits the instant into the local calendar time, which carries the offset. */
	error = os_time_break_down(seconds, &broken);
	if (error != 0)
		return error;

	/* Hands the offset east of UTC to the caller. */
	*offset = (long)broken.tm_gmtoff;

	/* Succeeded: the caller has the offset. */
	return 0;
}

/*
 * Writes the name of the local zone at an instant.
 *
 * The name is the system's abbreviation of the zone, cut to the longest
 * length the layer promises at a character boundary; it is "UTC" when the
 * system gives none.
 */
int
wb_os_zone_name(
	int64_t seconds,
	char *buffer,
	size_t size)
{
	struct tm broken;
	const char *name;
	size_t length;
	int error;

	/* Refuses a call that names no place for the name. */
	if (buffer == NULL || size == 0)
		return EINVAL;

	/* Leaves the output empty until the name has been written. */
	buffer[0] = '\0';

	/* Splits the instant into the local calendar time, which carries the zone's abbreviation. */
	error = os_time_break_down(seconds, &broken);
	if (error != 0)
		return error;

	/* Takes the abbreviation, or the default name when the system gives none. */
	name = broken.tm_zone;
	if (name == NULL || name[0] == '\0')
		name = OS_ZONE_NAME_DEFAULT;

	/* Measures the part of the name the layer promises to keep. */
	length = os_time_name_length(name);

	/* Refuses a buffer that cannot hold the name and its terminator. */
	if (length >= size)
		return ERANGE;

	/* Copies the name and ends it. */
	memcpy(buffer, name, length);
	buffer[length] = '\0';

	/* Succeeded: the buffer holds the name. */
	return 0;
}

/*
 * Brings an instant into the range the layer answers for.
 *
 * Everything before the first supported instant becomes it, and everything
 * after the last becomes that.
 */
static int64_t
os_time_clamp(
	int64_t seconds)
{
	/* Answers for the first supported instant when the instant is earlier. */
	if (seconds < WB_OS_TIME_FIRST)
		return WB_OS_TIME_FIRST;

	/* Answers for the last supported instant when the instant is later. */
	if (seconds > WB_OS_TIME_LAST)
		return WB_OS_TIME_LAST;

	/* The instant is already inside the range. */
	return seconds;
}

/*
 * Splits an instant into the calendar time of the local zone.
 *
 * It returns EIO when the system's time type cannot hold the instant or the
 * system cannot convert it.
 */
static int
os_time_break_down(
	int64_t seconds,
	struct tm *broken)
{
	struct tm *converted;
	time_t moment;
	int64_t clamped;

	/* Brings the instant into the supported range. */
	clamped = os_time_clamp(seconds);

	/* Turns the instant into the system's time type and checks that nothing was lost. */
	moment = (time_t)clamped;
	if ((int64_t)moment != clamped)
		return EIO;

	/* Converts the instant to the local calendar time in the caller's own structure. */
	converted = localtime_r(&moment, broken);
	if (converted == NULL)
		return EIO;

	/* Succeeded: the structure holds the local time. */
	return 0;
}

/*
 * Measures how many bytes of a zone name the layer keeps.
 *
 * A name longer than the promised length is cut so that no character of
 * UTF-8 is split.
 */
static size_t
os_time_name_length(
	const char *name)
{
	size_t length;

	/* Measures the name, stopping at the promised length. */
	length = 0;
	while (length < WB_OS_ZONE_NAME_MAX && name[length] != '\0')
		length++;

	/* Keeps the whole name when it ends within the promised length. */
	if (name[length] == '\0')
		return length;

	/* Backs up to the start of the character the cut would have split. */
	while (length > 0 &&
	    ((unsigned char)name[length] & OS_UTF8_CONTINUATION_MASK) == OS_UTF8_CONTINUATION_BITS)
		length--;

	/* The longest whole characters that fit. */
	return length;
}
