/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clocks and the local time zone of the operating-system layer.
 *
 * os-time-posix.c and os-time-win32.c implement it; the conventions are
 * those of os.h.  The Windows names of the zones come from os-zones.h.
 */

#ifndef KEILAND_BROWSER_OS_TIME_H
#define KEILAND_BROWSER_OS_TIME_H

#include "base/os.h"

#include <stddef.h>
#include <stdint.h>

/*
 * The earliest and latest instants, in seconds since 1970-01-01 00:00 UTC,
 * at which the local offset and the zone's name are asked of the system.
 * Windows' SYSTEMTIME covers 1601 to 30827; the range is the years 1601 to
 * 9999 on both systems so they agree.  Outside it the functions answer for
 * the nearest end (the offset of 1601 holds before it, that of 9999 after).
 */
#define WB_OS_TIME_FIRST	(-11644473600LL)	/* 1601-01-01 00:00:00 UTC */
#define WB_OS_TIME_LAST		(253402300799LL)	/* 9999-12-31 23:59:59 UTC */

/* The longest zone name wb_os_zone_name writes, in bytes without the NUL. */
#define WB_OS_ZONE_NAME_MAX	63U

/*
 * Reports a monotonic time in nanoseconds: it never goes back and does not
 * follow changes of the wall clock; its zero is arbitrary (but fixed for
 * the process).  POSIX: clock_gettime(CLOCK_MONOTONIC).  Windows:
 * QueryPerformanceCounter scaled by QueryPerformanceFrequency without
 * overflow (the quotient and the remainder scaled apart).
 */
uint64_t wb_os_monotonic_ns(void);

/*
 * Reports the wall clock: nanoseconds since 1970-01-01 00:00 UTC (negative
 * before; leap seconds are not counted).  POSIX:
 * clock_gettime(CLOCK_REALTIME).  Windows: GetSystemTimePreciseAsFileTime
 * minus 116444736000000000 hundred-nanosecond units, times 100.
 */
int64_t wb_os_wall_ns(void);

/*
 * Reads the system's local time zone again (the engine calls it once when
 * the Date built-ins are installed).  POSIX: tzset.  Windows: nothing (the
 * Windows calls below read the current zone each time).
 */
void wb_os_time_zone_start(void);

/*
 * Reports the local time's offset from UTC at an instant.
 *
 * seconds: the instant, seconds since 1970-01-01 00:00 UTC (clamped to
 *          WB_OS_TIME_FIRST..WB_OS_TIME_LAST).
 * offset:  receives the offset in seconds east of UTC (local = UTC +
 *          offset), daylight saving included when it applies at that
 *          instant; 0 on failure.
 * Returns 0, EINVAL (offset NULL), EIO (the system could not convert).
 * POSIX: localtime_r's tm_gmtoff.  Windows: SystemTimeToTzSpecificLocalTimeEx
 * with the current dynamic zone (GetDynamicTimeZoneInformation) on the
 * instant, the difference of the two SYSTEMTIMEs in seconds.
 */
int wb_os_local_offset(int64_t seconds, long *offset);

/*
 * Writes the name of the local zone at an instant, as Date's toString
 * shows it inside its parentheses.
 *
 * seconds: as wb_os_local_offset.
 * buffer:  size bytes; receives NUL-terminated UTF-8 of at most
 *          WB_OS_ZONE_NAME_MAX bytes.
 * POSIX: localtime_r's tm_zone (an abbreviation such as "JST"; the Date
 * code maps the common ones to long names); "UTC" when the system gives
 * none.  Windows: the zone's key (DYNAMIC_TIME_ZONE_INFORMATION's
 * TimeZoneKeyName) looked up with wb_os_zone_find; its daylight name when
 * the offset at the instant is the zone's daylight offset
 * (-(Bias + DaylightBias) minutes, with DaylightBias not 0), its standard
 * name otherwise; for a key the table does not know, the system's own
 * StandardName or DaylightName in the same way (converted to UTF-8, cut at
 * a character boundary to fit); "UTC" when all of that is empty.
 * Returns 0, EINVAL (buffer NULL or size 0), ERANGE (size cannot hold the
 * name: buffer then holds the empty string), EIO.
 */
int wb_os_zone_name(int64_t seconds, char *buffer, size_t size);

#endif
