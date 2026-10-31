/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clocks and the local time zone of the operating-system layer on Windows.
 */

#include "base/os-time.h"
#include "base/os-zones.h"
#include "base/os-win32.h"
#include "base/base.h"

#include <windows.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* How many nanoseconds make one second. */
#define OS_NANOSECONDS_PER_SECOND	1000000000ULL

/* How many FILETIME units (hundreds of nanoseconds) make one second. */
#define OS_FILETIME_PER_SECOND		10000000LL

/* How many nanoseconds one FILETIME unit lasts. */
#define OS_NANOSECONDS_PER_FILETIME	100LL

/* The FILETIME of 1970-01-01 00:00 UTC, which counts from 1601-01-01 00:00 UTC. */
#define OS_FILETIME_UNIX_EPOCH		116444736000000000LL

/* The seconds in a day, the step taken inward when an instant at the edge of the range cannot be converted. */
#define OS_SECONDS_PER_DAY		86400LL

/* How many seconds one minute of the zone's bias is. */
#define OS_SECONDS_PER_MINUTE		60L

/* The name given when the system and the table supply none. */
#define OS_ZONE_NAME_DEFAULT		"UTC"

/* The bits of a UTF-8 byte that tell a continuation byte from the first byte of a character. */
#define OS_UTF8_CONTINUATION_MASK	0xC0U

/* The value those bits have in a continuation byte. */
#define OS_UTF8_CONTINUATION_BITS	0x80U

/* The bytes a zone key needs as UTF-8: 127 UTF-16 units of at most three bytes each, and the NUL. */
#define OS_KEY_BYTES			384U

/* Forward declarations. */
static int64_t os_time_clamp(int64_t seconds);
static int os_time_offset_in_zone(DYNAMIC_TIME_ZONE_INFORMATION *zone, int64_t seconds, long *offset);
static int os_time_convert_once(DYNAMIC_TIME_ZONE_INFORMATION *zone, int64_t seconds, long *offset);
static int os_time_filetime_from_system(const SYSTEMTIME *time, int64_t *ticks);
static size_t os_time_wide_length(const wchar_t *text, size_t capacity);
static int os_time_utf8_name(const wchar_t *text, size_t capacity, char *name, size_t size, size_t *length);
static size_t os_time_cut_length(const char *name, size_t length, size_t limit);

/*
 * Reports a monotonic time in nanoseconds.
 *
 * It never goes back and does not follow changes of the wall clock; its
 * zero is arbitrary but fixed for the process.  It is 0 when the system has
 * no performance counter.
 *
 * The counter is scaled by its frequency in two parts, the whole seconds
 * and the remainder, so that no product overflows 64 bits.
 */
uint64_t
wb_os_monotonic_ns(
	void)
{
	LARGE_INTEGER counter;
	LARGE_INTEGER frequency;
	uint64_t ticks;
	uint64_t rate;
	uint64_t seconds;
	uint64_t remainder;
	uint64_t nanoseconds;
	BOOL succeeded;

	/* Reads how many counts the counter makes in a second. */
	succeeded = QueryPerformanceFrequency(&frequency);
	if (!succeeded)
		return 0;

	/* Refuses a counter that does not count. */
	if (frequency.QuadPart <= 0)
		return 0;

	/* Reads the counter. */
	succeeded = QueryPerformanceCounter(&counter);
	if (!succeeded)
		return 0;

	/* Refuses a counter that reads below its zero. */
	if (counter.QuadPart < 0)
		return 0;

	/* Splits the counter into the whole seconds it has run and the counts left over. */
	ticks = (uint64_t)counter.QuadPart;
	rate = (uint64_t)frequency.QuadPart;
	seconds = ticks / rate;
	remainder = ticks % rate;

	/*
	 * Scales the remainder down together with the rate until its product with a
	 * second of nanoseconds fits in 64 bits (a counter ticking faster than
	 * about 18 GHz loses its lowest bits, a rate no real counter has).
	 */
	while (remainder > UINT64_MAX / OS_NANOSECONDS_PER_SECOND) {
		remainder >>= 1;
		rate >>= 1;
	}

	/* Joins the whole seconds and the scaled remainder into nanoseconds. */
	nanoseconds = seconds * OS_NANOSECONDS_PER_SECOND;
	nanoseconds += remainder * OS_NANOSECONDS_PER_SECOND / rate;

	/* Succeeded: the count since the counter's arbitrary zero. */
	return nanoseconds;
}

/*
 * Reports the wall clock in nanoseconds since 1970-01-01 00:00 UTC.
 *
 * A time too far from 1970 for 64 bits is the nearest value that fits.
 */
int64_t
wb_os_wall_ns(
	void)
{
	FILETIME now;
	ULARGE_INTEGER units;
	int64_t ticks;
	int64_t limit;

	/* Reads the system clock at its finest resolution. */
	GetSystemTimePreciseAsFileTime(&now);

	/* Joins the two halves of the time into one count of 100 ns units since 1601. */
	units.LowPart = now.dwLowDateTime;
	units.HighPart = now.dwHighDateTime;

	/* Keeps a time that does not fit a signed count at the latest one. */
	if (units.QuadPart > (ULONGLONG)INT64_MAX)
		return INT64_MAX;

	/* Moves the zero of the count from 1601 to 1970. */
	ticks = (int64_t)units.QuadPart;
	ticks -= OS_FILETIME_UNIX_EPOCH;

	/* Keeps a time too late for nanoseconds in 64 bits at the latest one. */
	limit = INT64_MAX / OS_NANOSECONDS_PER_FILETIME;
	if (ticks > limit)
		return INT64_MAX;

	/* Keeps a time too early for nanoseconds in 64 bits at the earliest one. */
	limit = INT64_MIN / OS_NANOSECONDS_PER_FILETIME;
	if (ticks < limit)
		return INT64_MIN;

	/* Succeeded: the count of nanoseconds since 1970. */
	return ticks * OS_NANOSECONDS_PER_FILETIME;
}

/*
 * Reads the system's local time zone again.
 *
 * Nothing is kept between calls on Windows, so there is nothing to do.
 */
void
wb_os_time_zone_start(
	void)
{
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
	DYNAMIC_TIME_ZONE_INFORMATION zone;
	DWORD kind;
	int error;

	/* Refuses a call that names no place for the offset. */
	if (offset == NULL)
		return EINVAL;

	/* Leaves the output in its failure state until the conversion has worked. */
	*offset = 0;

	/* Reads the zone the system is set to now. */
	kind = GetDynamicTimeZoneInformation(&zone);
	if (kind == TIME_ZONE_ID_INVALID)
		return EIO;

	/* Finds the offset the zone has at the instant. */
	error = os_time_offset_in_zone(&zone, seconds, offset);
	if (error != 0)
		return error;

	/* Succeeded: the caller has the offset. */
	return 0;
}

/*
 * Writes the name of the local zone at an instant.
 *
 * The name comes from the table of English names when it knows the zone's
 * key, and from the system's own names otherwise; daylight saving chooses
 * between the standard and the daylight name.
 */
int
wb_os_zone_name(
	int64_t seconds,
	char *buffer,
	size_t size)
{
	DYNAMIC_TIME_ZONE_INFORMATION zone;
	const struct wb_os_zone_names *names;
	const char *chosen;
	const wchar_t *first;
	const wchar_t *second;
	char key[OS_KEY_BYTES];
	char name[WB_OS_ZONE_NAME_MAX + 1U];
	size_t key_length;
	size_t length;
	long offset;
	long daylight_offset;
	int daylight;
	int error;
	DWORD kind;

	/* Refuses a call that names no place for the name. */
	if (buffer == NULL || size == 0)
		return EINVAL;

	/* Leaves the output empty until the name has been written. */
	buffer[0] = '\0';

	/* Reads the zone the system is set to now. */
	kind = GetDynamicTimeZoneInformation(&zone);
	if (kind == TIME_ZONE_ID_INVALID)
		return EIO;

	/* Finds the offset the zone has at the instant. */
	error = os_time_offset_in_zone(&zone, seconds, &offset);
	if (error != 0)
		return error;

	/*
	 * Decides whether the instant is in daylight saving: the offset is the
	 * zone's daylight offset and the zone has a daylight bias at all.
	 */
	daylight = 0;
	daylight_offset = -((long)zone.Bias + (long)zone.DaylightBias) * OS_SECONDS_PER_MINUTE;
	if (zone.DaylightBias != 0 && offset == daylight_offset)
		daylight = 1;

	/* Takes the zone's key as UTF-8; a key that does not convert is a key the table does not know. */
	name[0] = '\0';
	key[0] = '\0';
	error = os_time_utf8_name(zone.TimeZoneKeyName, sizeof(zone.TimeZoneKeyName) / sizeof(zone.TimeZoneKeyName[0]),
	    key, sizeof(key), &key_length);
	if (error == ENOMEM)
		return error;
	if (error != 0)
		key[0] = '\0';

	/* Looks the key up in the table of English names. */
	names = wb_os_zone_find(key);
	if (names != NULL) {
		/* Takes the daylight name in daylight saving when the zone has one, the standard name otherwise. */
		chosen = names->standard;
		if (daylight && names->daylight != NULL)
			chosen = names->daylight;

		/* Copies the chosen name, cut to the promised length. */
		length = os_time_cut_length(chosen, strlen(chosen), WB_OS_ZONE_NAME_MAX);
		memcpy(name, chosen, length);
		name[length] = '\0';
	} else {
		/* Chooses the system's own name that suits the instant, and the other one as its fallback. */
		first = zone.StandardName;
		second = zone.DaylightName;
		if (daylight) {
			first = zone.DaylightName;
			second = zone.StandardName;
		}

		/* Takes the first name, or the second when the first is empty or does not convert. */
		error = os_time_utf8_name(first, sizeof(zone.StandardName) / sizeof(zone.StandardName[0]),
		    name, sizeof(name), &length);
		if (error == ENOMEM)
			return error;
		if (error != 0 || length == 0) {
			error = os_time_utf8_name(second, sizeof(zone.StandardName) / sizeof(zone.StandardName[0]),
			    name, sizeof(name), &length);
			if (error == ENOMEM)
				return error;
			if (error != 0)
				name[0] = '\0';
		}
	}

	/* Falls back to the default name when everything above was empty. */
	if (name[0] == '\0')
		memcpy(name, OS_ZONE_NAME_DEFAULT, sizeof(OS_ZONE_NAME_DEFAULT));

	/* Refuses a buffer that cannot hold the name and its terminator. */
	length = strlen(name);
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
 * Finds the offset a zone has at an instant.
 *
 * At the very first day of the range a zone west of UTC puts the local time
 * before the year 1601, which the system cannot show; the offset is then
 * taken one day further in, where the offset is the same.
 */
static int
os_time_offset_in_zone(
	DYNAMIC_TIME_ZONE_INFORMATION *zone,
	int64_t seconds,
	long *offset)
{
	int64_t clamped;
	int error;

	/* Brings the instant into the supported range. */
	clamped = os_time_clamp(seconds);

	/* Converts the instant itself. */
	error = os_time_convert_once(zone, clamped, offset);
	if (error == 0)
		return 0;

	/* Gives up on a failure anywhere but in the first day, where the local time lies before 1601. */
	if (clamped >= WB_OS_TIME_FIRST + OS_SECONDS_PER_DAY)
		return error;

	/* Converts the same instant one day later in the range. */
	error = os_time_convert_once(zone, WB_OS_TIME_FIRST + OS_SECONDS_PER_DAY, offset);
	if (error != 0)
		return error;

	/* Succeeded: the caller has the offset. */
	return 0;
}

/*
 * Converts one instant of the supported range to local time and measures the offset.
 *
 * The zone's rules are asked for the year of the instant and applied with
 * the plain conversion; the conversion that takes the dynamic zone itself
 * does the same, but not every system the layer runs on implements it.
 * The offset is the local time minus the UTC time, both as FILETIMEs.
 */
static int
os_time_convert_once(
	DYNAMIC_TIME_ZONE_INFORMATION *zone,
	int64_t seconds,
	long *offset)
{
	FILETIME instant;
	SYSTEMTIME utc;
	SYSTEMTIME local;
	TIME_ZONE_INFORMATION year_rules;
	ULARGE_INTEGER units;
	int64_t utc_ticks;
	int64_t local_ticks;
	BOOL succeeded;
	int error;

	/* Expresses the instant as the count of 100 ns units since 1601 that FILETIME uses. */
	units.QuadPart = (ULONGLONG)((seconds * OS_FILETIME_PER_SECOND) + OS_FILETIME_UNIX_EPOCH);
	instant.dwLowDateTime = units.LowPart;
	instant.dwHighDateTime = units.HighPart;

	/* Splits the instant into a calendar date and time in UTC. */
	succeeded = FileTimeToSystemTime(&instant, &utc);
	if (!succeeded)
		return EIO;

	/* Asks for the zone's rules of the instant's own year, which may differ from this year's. */
	succeeded = GetTimeZoneInformationForYear(utc.wYear, zone, &year_rules);
	if (!succeeded)
		return EIO;

	/* Converts the UTC time to the zone's local time by those rules. */
	succeeded = SystemTimeToTzSpecificLocalTime(&year_rules, &utc, &local);
	if (!succeeded)
		return EIO;

	/* Measures both calendar times on the same scale. */
	error = os_time_filetime_from_system(&utc, &utc_ticks);
	if (error != 0)
		return error;
	error = os_time_filetime_from_system(&local, &local_ticks);
	if (error != 0)
		return error;

	/* Turns the difference of the two scales into seconds, east of UTC. */
	*offset = (long)((local_ticks - utc_ticks) / OS_FILETIME_PER_SECOND);

	/* Succeeded: the offset is measured. */
	return 0;
}

/*
 * Turns a calendar date and time into a count of 100 ns units.
 *
 * The count is relative to 1601, so two of them can be subtracted.
 */
static int
os_time_filetime_from_system(
	const SYSTEMTIME *time,
	int64_t *ticks)
{
	FILETIME converted;
	ULARGE_INTEGER units;
	BOOL succeeded;

	/* Joins the calendar fields into a FILETIME. */
	succeeded = SystemTimeToFileTime(time, &converted);
	if (!succeeded)
		return EIO;

	/* Joins the two halves into one count. */
	units.LowPart = converted.dwLowDateTime;
	units.HighPart = converted.dwHighDateTime;
	*ticks = (int64_t)units.QuadPart;

	/* Succeeded: the caller has the count. */
	return 0;
}

/*
 * Measures a wide string inside a fixed array.
 *
 * It is the number of units before the first NUL, or the capacity when the
 * array holds no NUL, so a damaged array is never read past its end.
 */
static size_t
os_time_wide_length(
	const wchar_t *text,
	size_t capacity)
{
	size_t length;

	/* Counts the units up to the first NUL or the end of the array. */
	length = 0;
	while (length < capacity && text[length] != 0)
		length++;

	/* The number of units to convert. */
	return length;
}

/*
 * Converts a wide string in a fixed array to UTF-8 inside a buffer.
 *
 * The name is cut at a character boundary to fit the buffer.  It returns
 * EILSEQ for text that is not valid UTF-16, ENOMEM when memory ran out.
 */
static int
os_time_utf8_name(
	const wchar_t *text,
	size_t capacity,
	char *name,
	size_t size,
	size_t *length)
{
	char *utf8;
	size_t count;
	size_t used;
	int error;

	/* Measures the units that are really there. */
	count = os_time_wide_length(text, capacity);

	/* Treats an empty string as the empty name without converting. */
	if (count == 0) {
		name[0] = '\0';
		*length = 0;
		return 0;
	}

	/* Converts the units to a new UTF-8 string. */
	error = wb_os_utf8_from_wide(text, count, &utf8, &used);
	if (error != 0)
		return error;

	/* Cuts the string to the buffer at a character boundary. */
	used = os_time_cut_length(utf8, used, size - 1U);

	/* Copies the kept part and ends it. */
	memcpy(name, utf8, used);
	name[used] = '\0';
	*length = used;

	/* Releases the converted string. */
	free(utf8);

	/* Succeeded: the buffer holds the name. */
	return 0;
}

/*
 * Finds where to cut a UTF-8 string so that no character is split.
 *
 * The cut is at most the given length and at most the limit.
 */
static size_t
os_time_cut_length(
	const char *name,
	size_t length,
	size_t limit)
{
	/* Keeps within the limit. */
	if (length > limit)
		length = limit;

	/* Backs up to the start of the character that the cut would have split. */
	while (length > 0 &&
	    name[length] != '\0' &&
	    ((unsigned char)name[length] & OS_UTF8_CONTINUATION_MASK) == OS_UTF8_CONTINUATION_BITS)
		length--;

	/* The longest whole characters that fit. */
	return length;
}
