/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the clocks and the local time zone of the
 * operating-system layer (base/os-time.h).  It runs on POSIX and on
 * Windows; argv[1] says which.
 *
 * The expected values come from the header's contracts, from the C library
 * and the Windows API used separately from the module, and from hand
 * calculation (the instants are counted from the calendar by hand).
 */

#include "check.h"

#include "base/os-time.h"
#include "base/os-zones.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Whether this run is the Windows one (argv[1] is "win32"). */
static int is_win32;

/* How many times the monotonic clock is read in a row. */
#define MONOTONIC_READS		100000

/* The time the test sleeps between two reads of the monotonic clock, in milliseconds. */
#define SLEEP_MS		100

/* A name buffer larger than any name the layer writes, filled with a mark so an untouched buffer shows. */
#define BUFFER_SIZE		(WB_OS_ZONE_NAME_MAX + 8U)

/* The instants that are on or outside the ends of the supported range. */
static const int64_t outside_below[] = { WB_OS_TIME_FIRST - 1, WB_OS_TIME_FIRST - 86400LL, INT64_MIN };
static const int64_t outside_above[] = { WB_OS_TIME_LAST + 1, WB_OS_TIME_LAST + 86400LL, INT64_MAX };

/* Sleeps for a number of milliseconds with the system's own call. */
static void
sleep_ms(
	unsigned milliseconds)
{
#ifdef _WIN32
	Sleep(milliseconds);
#else
	struct timespec request;

	/* Asks for the sleep, and sleeps on after an interruption by a signal. */
	request.tv_sec = milliseconds / 1000U;
	request.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	while (nanosleep(&request, &request) != 0 && errno == EINTR)
		;
#endif
}

/* Checks that the monotonic clock never goes back and follows real time. */
static void
test_monotonic(
	void)
{
	uint64_t before;
	uint64_t after;
	uint64_t now;
	uint64_t previous;
	uint64_t elapsed;
	int backwards;
	int fractions;
	int i;

	/* Reads the clock many times in a row; no read is below the one before it. */
	previous = wb_os_monotonic_ns();
	CHECK(previous != 0);
	backwards = 0;
	for (i = 0; i < MONOTONIC_READS; i++) {
		now = wb_os_monotonic_ns();
		if (now < previous)
			backwards++;
		previous = now;
	}
	CHECK_EQ(backwards, 0);

	/* Some reads carry a part of a second, so the clock is not rounded to whole seconds. */
	fractions = 0;
	for (i = 0; i < 100; i++) {
		if (wb_os_monotonic_ns() % 1000000000ULL != 0)
			fractions++;
	}
	CHECK(fractions > 0);

	/* A sleep of 100 ms shows as 90 ms to 2 s on the clock. */
	before = wb_os_monotonic_ns();
	sleep_ms(SLEEP_MS);
	after = wb_os_monotonic_ns();
	CHECK(after >= before);
	elapsed = after - before;
	CHECK(elapsed >= 90000000ULL);
	CHECK(elapsed < 2000000000ULL);

	/* A sleep of 1100 ms crosses a whole second on the clock and shows as 1 s to 3 s. */
	before = wb_os_monotonic_ns();
	sleep_ms(1100);
	after = wb_os_monotonic_ns();
	CHECK(after >= before);
	elapsed = after - before;
	CHECK(elapsed >= 1000000000ULL);
	CHECK(elapsed < 3000000000ULL);
}

/* Checks the wall clock against the C library's time. */
static void
test_wall(
	void)
{
	int64_t first;
	int64_t second;
	int64_t library_seconds;
	int64_t library_before;
	int fractions;
	int i;
	int64_t difference;

	/* The wall clock in seconds is within 2 s of the C library's clock. */
	library_before = (int64_t)time(NULL);
	first = wb_os_wall_ns();
	library_seconds = (int64_t)time(NULL);
	difference = first / 1000000000LL - library_seconds;
	CHECK(difference > -2 && difference < 2);

	/* It lies between the C library's whole seconds read before and after it (10 ms allowed for the library's coarser clock). */
	CHECK(first >= library_before * 1000000000LL - 10000000LL);
	CHECK(first < (library_seconds + 1) * 1000000000LL + 10000000LL);

	/* Two reads in a row do not go back (the test does not change the clock). */
	second = wb_os_wall_ns();
	CHECK(second >= first);

	/* Some reads carry a part of a second, so the clock is not rounded to whole seconds. */
	fractions = 0;
	for (i = 0; i < 100; i++) {
		if (wb_os_wall_ns() % 1000000000LL != 0)
			fractions++;
	}
	CHECK(fractions > 0);

	/* The clock is after 2020 (1577836800 is 2020-01-01 00:00 UTC), so it has the 1970 zero. */
	CHECK(first > 1577836800LL * 1000000000LL);
}

/* Checks the failures common to both systems for the offset and the name. */
static void
test_errors(
	void)
{
	char buffer[BUFFER_SIZE];
	char exact[BUFFER_SIZE];
	long offset;
	size_t length;
	size_t size;
	int error;

	/* A NULL place for the offset is refused. */
	error = wb_os_local_offset(0, NULL);
	CHECK_EQ(error, EINVAL);

	/* A NULL buffer is refused, whatever the size. */
	error = wb_os_zone_name(0, NULL, 16);
	CHECK_EQ(error, EINVAL);
	error = wb_os_zone_name(0, NULL, 0);
	CHECK_EQ(error, EINVAL);

	/* A size of 0 is refused and the buffer is not written. */
	memset(buffer, 'X', sizeof(buffer));
	error = wb_os_zone_name(0, buffer, 0);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(buffer[0], 'X');

	/* A good call fills the offset (the sentinel proves the write). */
	offset = 123456789L;
	error = wb_os_local_offset(0, &offset);
	CHECK_EQ(error, 0);
	CHECK(offset != 123456789L);

	/* The name fits in a buffer of the promised size and is not empty. */
	memset(exact, 'X', sizeof(exact));
	error = wb_os_zone_name(0, exact, WB_OS_ZONE_NAME_MAX + 1U);
	CHECK_EQ(error, 0);
	length = strlen(exact);
	CHECK(length > 0);
	CHECK(length <= WB_OS_ZONE_NAME_MAX);

	/* A size one more than the name's length holds it exactly. */
	memset(buffer, 'X', sizeof(buffer));
	error = wb_os_zone_name(0, buffer, length + 1U);
	CHECK_EQ(error, 0);
	CHECK(strcmp(buffer, exact) == 0);

	/* A size equal to the length or smaller cannot hold the name: ERANGE and the empty string. */
	for (size = 1; size <= length; size++) {
		memset(buffer, 'X', sizeof(buffer));
		error = wb_os_zone_name(0, buffer, size);
		CHECK_EQ(error, ERANGE);
		CHECK_EQ(buffer[0], 0);
	}
}

#ifndef _WIN32

/*
 * A zone given by a POSIX TZ string and what it must report at an instant.
 *
 * The instants are counted by hand: 2024-07-01 12:00 UTC is 1719835200 and
 * 2024-01-15 12:00 UTC is 1705320000; the United States rules switch at
 * 2024-03-10 07:00 UTC (1710054000) and 2024-11-03 06:00 UTC (1730613600).
 */
struct zone_case {
	/* The TZ string. */
	const char *zone;

	/* The instant asked about. */
	int64_t seconds;

	/* The offset east of UTC the zone has then. */
	long offset;

	/* The abbreviation the zone has then. */
	const char *name;
};

/* The zones and instants the POSIX test asks about. */
static const struct zone_case zone_cases[] = {
	{ "UTC0", 0, 0, "UTC" },
	{ "UTC0", 1719835200LL, 0, "UTC" },
	{ "JST-9", 0, 32400, "JST" },
	{ "JST-9", 1719835200LL, 32400, "JST" },
	{ "IST-5:30", 1705320000LL, 19800, "IST" },
	{ "<+0545>-5:45", 1705320000LL, 20700, "+0545" },
	{ "EST5EDT,M3.2.0,M11.1.0", 1719835200LL, -14400, "EDT" },
	{ "EST5EDT,M3.2.0,M11.1.0", 1705320000LL, -18000, "EST" },
	{ "EST5EDT,M3.2.0,M11.1.0", 1710053999LL, -18000, "EST" },
	{ "EST5EDT,M3.2.0,M11.1.0", 1710054000LL, -14400, "EDT" },
	{ "EST5EDT,M3.2.0,M11.1.0", 1730613599LL, -14400, "EDT" },
	{ "EST5EDT,M3.2.0,M11.1.0", 1730613600LL, -18000, "EST" },
	{ "AEST-10AEDT,M10.1.0,M4.1.0/3", 1705320000LL, 39600, "AEDT" },
	{ "AEST-10AEDT,M10.1.0,M4.1.0/3", 1719835200LL, 36000, "AEST" }
};

/* Sets the zone the way the engine does: the environment first, then the layer is told to read it again. */
static void
use_zone(
	const char *zone)
{
	/* Changes the environment variable that names the zone. */
	setenv("TZ", zone, 1);

	/* Makes the layer read it. */
	wb_os_time_zone_start();
}

/* Checks the offset and the abbreviation for the table of zones. */
static void
test_posix_zones(
	void)
{
	char buffer[BUFFER_SIZE];
	long offset;
	size_t i;
	int error;

	/* Every case gives the offset and the name worked out by hand. */
	for (i = 0; i < sizeof(zone_cases) / sizeof(zone_cases[0]); i++) {
		use_zone(zone_cases[i].zone);

		offset = 1;
		error = wb_os_local_offset(zone_cases[i].seconds, &offset);
		CHECK_EQ(error, 0);
		CHECK_EQ(offset, zone_cases[i].offset);

		memset(buffer, 'X', sizeof(buffer));
		error = wb_os_zone_name(zone_cases[i].seconds, buffer, sizeof(buffer));
		CHECK_EQ(error, 0);
		CHECK(strcmp(buffer, zone_cases[i].name) == 0);
	}
}

/* Checks that an instant outside the range is answered like the nearest end. */
static void
test_posix_clamp(
	void)
{
	static const char *const zones[] = { "JST-9", "EST5EDT,M3.2.0,M11.1.0", "AEST-10AEDT,M10.1.0,M4.1.0/3" };
	char end_name[BUFFER_SIZE];
	char name[BUFFER_SIZE];
	long end_offset;
	long offset;
	size_t z;
	size_t i;
	int error;

	/* Every zone answers before the first instant as at the first, and after the last as at the last. */
	for (z = 0; z < sizeof(zones) / sizeof(zones[0]); z++) {
		use_zone(zones[z]);

		error = wb_os_local_offset(WB_OS_TIME_FIRST, &end_offset);
		CHECK_EQ(error, 0);
		error = wb_os_zone_name(WB_OS_TIME_FIRST, end_name, sizeof(end_name));
		CHECK_EQ(error, 0);
		for (i = 0; i < sizeof(outside_below) / sizeof(outside_below[0]); i++) {
			error = wb_os_local_offset(outside_below[i], &offset);
			CHECK_EQ(error, 0);
			CHECK_EQ(offset, end_offset);
			error = wb_os_zone_name(outside_below[i], name, sizeof(name));
			CHECK_EQ(error, 0);
			CHECK(strcmp(name, end_name) == 0);
		}

		error = wb_os_local_offset(WB_OS_TIME_LAST, &end_offset);
		CHECK_EQ(error, 0);
		error = wb_os_zone_name(WB_OS_TIME_LAST, end_name, sizeof(end_name));
		CHECK_EQ(error, 0);
		for (i = 0; i < sizeof(outside_above) / sizeof(outside_above[0]); i++) {
			error = wb_os_local_offset(outside_above[i], &offset);
			CHECK_EQ(error, 0);
			CHECK_EQ(offset, end_offset);
			error = wb_os_zone_name(outside_above[i], name, sizeof(name));
			CHECK_EQ(error, 0);
			CHECK(strcmp(name, end_name) == 0);
		}
	}

	/* The ends themselves have the offsets worked out by hand: January in 1601 and December in 9999. */
	use_zone("JST-9");
	error = wb_os_local_offset(WB_OS_TIME_FIRST, &offset);
	CHECK_EQ(error, 0);
	CHECK_EQ(offset, 32400);
	error = wb_os_local_offset(WB_OS_TIME_LAST, &offset);
	CHECK_EQ(error, 0);
	CHECK_EQ(offset, 32400);
	use_zone("EST5EDT,M3.2.0,M11.1.0");
	error = wb_os_local_offset(WB_OS_TIME_FIRST, &offset);
	CHECK_EQ(error, 0);
	CHECK_EQ(offset, -18000);
	error = wb_os_local_offset(WB_OS_TIME_LAST, &offset);
	CHECK_EQ(error, 0);
	CHECK_EQ(offset, -18000);
}

/*
 * Checks the names the C library gives for odd zones against the C library
 * itself: the name is the library's abbreviation cut to 63 bytes, and "UTC"
 * when the library gives none.
 */
static void
test_posix_odd_names(
	void)
{
	/* Abbreviations of 62, 63, 64 and 80 letters in the quoted form of the TZ string, and two zones the library cannot read. */
	static const char *const zones[] = {
		"<ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHI>-1",
		"<ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJ>-1",
		"<ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJK>-1",
		"<ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUV>-1",
		"XX",
		"<>-1"
	};
	char expected[BUFFER_SIZE + 64U];
	char buffer[BUFFER_SIZE];
	struct tm library_time;
	time_t moment;
	size_t length;
	size_t i;
	int error;

	/* Every zone gives what the library's own abbreviation gives, cut to 63 bytes. */
	for (i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
		use_zone(zones[i]);

		moment = 0;
		memset(&library_time, 0, sizeof(library_time));
		localtime_r(&moment, &library_time);
		if (library_time.tm_zone == NULL || library_time.tm_zone[0] == '\0')
			snprintf(expected, sizeof(expected), "%s", "UTC");
		else
			snprintf(expected, sizeof(expected), "%s", library_time.tm_zone);
		length = strlen(expected);
		if (length > WB_OS_ZONE_NAME_MAX)
			expected[WB_OS_ZONE_NAME_MAX] = '\0';

		memset(buffer, 'X', sizeof(buffer));
		error = wb_os_zone_name(0, buffer, sizeof(buffer));
		CHECK_EQ(error, 0);
		CHECK(strcmp(buffer, expected) == 0);
	}

	/* The C library on this system reads at least the 62-letter abbreviation, which proves the cut test ran on a long name. */
	use_zone(zones[1]);
	memset(buffer, 'X', sizeof(buffer));
	error = wb_os_zone_name(0, buffer, sizeof(buffer));
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(buffer), 63);
}

/* Runs the POSIX checks. */
static void
test_posix(
	void)
{
	test_posix_zones();
	test_posix_clamp();
	test_posix_odd_names();
}

#else

/*
 * The instants of the Windows offset test: noon UTC on January 15 and July
 * 15 of every third year from 2000 to 2027, counted by hand.
 */
static const int64_t windows_instants[] = {
	947937600LL, 963662400LL, 1042632000LL, 1058270400LL, 1137326400LL, 1152964800LL, 1232020800LL,
	1247659200LL, 1326628800LL, 1342353600LL, 1421323200LL, 1436961600LL, 1516017600LL, 1531656000LL,
	1610712000LL, 1626350400LL, 1705320000LL, 1721044800LL, 1800014400LL, 1815652800LL
};

/*
 * The seconds just before and at the 2024 changes of daylight saving of the
 * United States, the European Union and Sydney (counted by hand: 2024-03-10
 * 07:00, 2024-11-03 06:00, 2024-03-31 01:00, 2024-10-27 01:00, 2024-04-06
 * 16:00 and 2024-10-05 16:00 UTC), so a zone with such rules is tested at
 * the edge.  A zone without them gives the same offset at all of these.
 */
static const int64_t transition_instants[] = {
	1710053999LL, 1710054000LL, 1730613599LL, 1730613600LL, 1711846799LL, 1711846800LL,
	1729990799LL, 1729990800LL, 1712419199LL, 1712419200LL, 1728143999LL, 1728144000LL
};

/* Narrows UTF-16 to UTF-8 with the system's own conversion (not the module's). */
static void
narrow(
	const wchar_t *wide,
	char *text,
	size_t size)
{
	int count;

	/* Converts, and gives the empty string when the conversion fails. */
	count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, (int)size, NULL, NULL);
	if (count <= 0)
		text[0] = '\0';
}

/* Converts a FILETIME to a count of seconds and 100 ns units since 1601 as one number. */
static int64_t
filetime_ticks(
	const SYSTEMTIME *time)
{
	FILETIME converted;
	ULARGE_INTEGER units;
	BOOL succeeded;

	/* Joins the calendar fields. */
	succeeded = SystemTimeToFileTime(time, &converted);
	CHECK(succeeded);
	units.LowPart = converted.dwLowDateTime;
	units.HighPart = converted.dwHighDateTime;
	return (int64_t)units.QuadPart;
}

/* Tells whether a year is a leap year of the Gregorian calendar. */
static int
leap_year(
	int year)
{
	if (year % 400 == 0)
		return 1;
	if (year % 100 == 0)
		return 0;
	if (year % 4 == 0)
		return 1;
	return 0;
}

/* Gives the number of days of a month (1 to 12) of a year. */
static int
month_days(
	int year,
	int month)
{
	static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	if (month == 2 && leap_year(year))
		return 29;
	return days[month - 1];
}

/* Counts the days from 1970-01-01 to a date of 1970 or later, by walking the calendar. */
static int64_t
days_since_1970(
	int year,
	int month,
	int day)
{
	int64_t days;
	int y;
	int m;

	days = 0;
	for (y = 1970; y < year; y++)
		days += leap_year(y) ? 366 : 365;
	for (m = 1; m < month; m++)
		days += month_days(year, m);
	days += day - 1;
	return days;
}

/*
 * Gives the local wall-clock time, in seconds counted as if it were UTC from
 * 1970, at which a transition rule of a TIME_ZONE_INFORMATION falls in a year.
 *
 * A rule with a year is an absolute date; one without is "the n-th weekday
 * of the month", with 5 meaning the last.
 */
static int64_t
transition_seconds(
	const SYSTEMTIME *rule,
	int year)
{
	int64_t days;
	int weekday;
	int day;

	if (rule->wYear != 0) {
		day = rule->wDay;
	} else {
		/* 1970-01-01 was a Thursday (4, counting Sunday as 0). */
		days = days_since_1970(year, rule->wMonth, 1);
		weekday = (int)((4 + days) % 7);
		day = 1 + (rule->wDayOfWeek - weekday + 7) % 7;
		day += 7 * (rule->wDay - 1);
		while (day > month_days(year, rule->wMonth))
			day -= 7;
	}
	days = days_since_1970(year, rule->wMonth, day);
	return days * 86400LL + rule->wHour * 3600LL + rule->wMinute * 60LL + rule->wSecond;
}

/* Works out the offset of an instant from the year's rules by hand, without the system's conversion. */
static long
rule_offset(
	const TIME_ZONE_INFORMATION *rules,
	int year,
	int64_t seconds)
{
	int64_t standard_local;
	int64_t daylight_local;
	int64_t start;
	int64_t end;
	long standard_offset;
	long daylight_offset;
	int in_daylight;

	standard_offset = -(rules->Bias + rules->StandardBias) * 60L;
	daylight_offset = -(rules->Bias + rules->DaylightBias) * 60L;

	/* A zone without a transition keeps the standard offset all year. */
	if (rules->StandardDate.wMonth == 0 || rules->DaylightDate.wMonth == 0)
		return standard_offset;

	/* The daylight time starts in standard local time and ends in daylight local time. */
	start = transition_seconds(&rules->DaylightDate, year);
	end = transition_seconds(&rules->StandardDate, year);
	standard_local = seconds + standard_offset;
	daylight_local = seconds + daylight_offset;
	if (start < end)
		in_daylight = standard_local >= start && daylight_local < end;
	else
		in_daylight = standard_local >= start || daylight_local < end;
	return in_daylight ? daylight_offset : standard_offset;
}

/* Finds an offset by the system's rules of the instant's own year, not the current year's. */
static long
expected_offset(
	DYNAMIC_TIME_ZONE_INFORMATION *dynamic,
	int64_t seconds)
{
	FILETIME instant;
	SYSTEMTIME utc;
	SYSTEMTIME local;
	TIME_ZONE_INFORMATION year_rules;
	ULARGE_INTEGER units;
	BOOL succeeded;

	/* The instant as a FILETIME: 100 ns units from 1601, which is 11644473600 s before 1970. */
	units.QuadPart = (ULONGLONG)((seconds + 11644473600LL) * 10000000LL);
	instant.dwLowDateTime = units.LowPart;
	instant.dwHighDateTime = units.HighPart;
	succeeded = FileTimeToSystemTime(&instant, &utc);
	CHECK(succeeded);

	/* The zone's rules for that year, and the local time by them. */
	succeeded = GetTimeZoneInformationForYear(utc.wYear, dynamic, &year_rules);
	CHECK(succeeded);
	succeeded = SystemTimeToTzSpecificLocalTime(&year_rules, &utc, &local);
	CHECK(succeeded);

	/* The rules worked out by hand agree with the system's conversion. */
	CHECK_EQ(rule_offset(&year_rules, utc.wYear, seconds), (filetime_ticks(&local) - filetime_ticks(&utc)) / 10000000LL);

	return (long)((filetime_ticks(&local) - filetime_ticks(&utc)) / 10000000LL);
}

/* Checks the offsets against the system's per-year rules. */
static void
test_windows_offsets(
	void)
{
	DYNAMIC_TIME_ZONE_INFORMATION dynamic;
	long offset;
	long outer;
	size_t i;
	int error;
	DWORD kind;

	/* Reads the zone the test runs in. */
	kind = GetDynamicTimeZoneInformation(&dynamic);
	CHECK(kind != TIME_ZONE_ID_INVALID);

	/* Every instant has the offset the per-year rules give. */
	for (i = 0; i < sizeof(windows_instants) / sizeof(windows_instants[0]); i++) {
		offset = 1;
		error = wb_os_local_offset(windows_instants[i], &offset);
		CHECK_EQ(error, 0);
		CHECK_EQ(offset, expected_offset(&dynamic, windows_instants[i]));
	}

	/* The instants at the changes of daylight saving have the offset the per-year rules give. */
	for (i = 0; i < sizeof(transition_instants) / sizeof(transition_instants[0]); i++) {
		offset = 1;
		error = wb_os_local_offset(transition_instants[i], &offset);
		CHECK_EQ(error, 0);
		CHECK_EQ(offset, expected_offset(&dynamic, transition_instants[i]));
	}

	/* The ends of the range answer like the days next to them (the end's own local time may leave the calendar). */
	error = wb_os_local_offset(WB_OS_TIME_FIRST, &offset);
	CHECK_EQ(error, 0);
	CHECK_EQ(offset, expected_offset(&dynamic, WB_OS_TIME_FIRST + 86400LL));
	error = wb_os_local_offset(WB_OS_TIME_LAST, &offset);
	CHECK_EQ(error, 0);
	CHECK_EQ(offset, expected_offset(&dynamic, WB_OS_TIME_LAST - 86400LL));

	/* The instants outside answer like the nearest end. */
	error = wb_os_local_offset(WB_OS_TIME_FIRST, &offset);
	CHECK_EQ(error, 0);
	for (i = 0; i < sizeof(outside_below) / sizeof(outside_below[0]); i++) {
		outer = 1;
		error = wb_os_local_offset(outside_below[i], &outer);
		CHECK_EQ(error, 0);
		CHECK_EQ(outer, offset);
	}
	error = wb_os_local_offset(WB_OS_TIME_LAST, &offset);
	CHECK_EQ(error, 0);
	for (i = 0; i < sizeof(outside_above) / sizeof(outside_above[0]); i++) {
		outer = 1;
		error = wb_os_local_offset(outside_above[i], &outer);
		CHECK_EQ(error, 0);
		CHECK_EQ(outer, offset);
	}
}

/* Checks the names against the system's zone and the table. */
static void
test_windows_names(
	void)
{
	DYNAMIC_TIME_ZONE_INFORMATION dynamic;
	const struct wb_os_zone_names *names;
	char key[BUFFER_SIZE * 4U];
	char standard[BUFFER_SIZE * 4U];
	char daylight[BUFFER_SIZE * 4U];
	char name[BUFFER_SIZE];
	const char *wanted;
	int64_t instant;
	long offset;
	long daylight_offset;
	int is_daylight;
	size_t i;
	int error;
	DWORD kind;

	/* Reads the zone the test runs in, and its key and names as UTF-8. */
	kind = GetDynamicTimeZoneInformation(&dynamic);
	CHECK(kind != TIME_ZONE_ID_INVALID);
	narrow(dynamic.TimeZoneKeyName, key, sizeof(key));
	narrow(dynamic.StandardName, standard, sizeof(standard));
	narrow(dynamic.DaylightName, daylight, sizeof(daylight));
	names = wb_os_zone_find(key);
	daylight_offset = -((long)dynamic.Bias + (long)dynamic.DaylightBias) * 60L;
	printf("zone key \"%s\", table %s, standard \"%s\", daylight \"%s\", daylight bias %ld\n", key,
	    names != NULL ? "knows it" : "does not know it", standard, daylight, (long)dynamic.DaylightBias);

	/* Every instant has the standard or the daylight name, by the daylight offset of the per-year rules. */
	for (i = 0; i < sizeof(windows_instants) / sizeof(windows_instants[0]) + sizeof(transition_instants) / sizeof(transition_instants[0]); i++) {
		if (i < sizeof(windows_instants) / sizeof(windows_instants[0]))
			instant = windows_instants[i];
		else
			instant = transition_instants[i - sizeof(windows_instants) / sizeof(windows_instants[0])];
		offset = expected_offset(&dynamic, instant);
		is_daylight = dynamic.DaylightBias != 0 && offset == daylight_offset;

		memset(name, 'X', sizeof(name));
		error = wb_os_zone_name(instant, name, sizeof(name));
		CHECK_EQ(error, 0);
		if (names != NULL) {
			if (is_daylight && names->daylight != NULL)
				CHECK(strcmp(name, names->daylight) == 0);
			else
				CHECK(strcmp(name, names->standard) == 0);
		} else {
			wanted = is_daylight ? daylight : standard;
			if (wanted[0] == '\0')
				wanted = is_daylight ? standard : daylight;
			if (wanted[0] == '\0')
				wanted = "UTC";
			if (strlen(wanted) <= WB_OS_ZONE_NAME_MAX)
				CHECK(strcmp(name, wanted) == 0);
			else
				CHECK(strncmp(name, wanted, strlen(name)) == 0);
		}
	}
}

/* Runs the Windows checks. */
static void
test_windows(
	void)
{
	test_windows_offsets();
	test_windows_names();
}

#endif

/* Runs the checks. */
int
main(
	int argc,
	char **argv)
{
	/* The harness passes the system's name. */
	CHECK(argc == 2);
	if (argc != 2)
		return check_finish("os-time");
	is_win32 = strcmp(argv[1], "win32") == 0;
	CHECK(is_win32 || strcmp(argv[1], "posix") == 0);

	test_monotonic();
	test_wall();
	test_errors();
#ifdef _WIN32
	test_windows();
#else
	test_posix();
#endif

	return check_finish("os-time");
}
