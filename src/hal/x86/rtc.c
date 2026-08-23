/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "rtc.h"

#define CMOS_SECOND   0x00U
#define CMOS_MINUTE   0x02U
#define CMOS_HOUR     0x04U
#define CMOS_DAY      0x07U
#define CMOS_MONTH    0x08U
#define CMOS_YEAR     0x09U
#define CMOS_STATUS_A 0x0aU
#define CMOS_STATUS_B 0x0bU
#define CMOS_UIP      0x80U
#define CMOS_BINARY   0x04U
#define CMOS_24_HOUR  0x02U
#define CMOS_PM       0x80U

#define CMOS_UIP_POLLS 100000U
#define CMOS_SNAPSHOT_RETRIES 8U

struct rtc_snapshot {
	uint8_t second, minute, hour, day, month, year, status_b;
};

static int
snapshot_equal(const struct rtc_snapshot *a, const struct rtc_snapshot *b)
{
	return a->second == b->second && a->minute == b->minute &&
	    a->hour == b->hour && a->day == b->day && a->month == b->month &&
	    a->year == b->year && a->status_b == b->status_b;
}

static int
snapshot_read(x86_cmos_read_fn read, void *context, struct rtc_snapshot *out)
{
	unsigned poll;

	for (poll = 0; poll < CMOS_UIP_POLLS; poll++)
		if ((read(CMOS_STATUS_A, context) & CMOS_UIP) == 0)
			break;
	if (poll == CMOS_UIP_POLLS)
		return 0;
	out->second = read(CMOS_SECOND, context);
	out->minute = read(CMOS_MINUTE, context);
	out->hour = read(CMOS_HOUR, context);
	out->day = read(CMOS_DAY, context);
	out->month = read(CMOS_MONTH, context);
	out->year = read(CMOS_YEAR, context);
	out->status_b = read(CMOS_STATUS_B, context);
	return (read(CMOS_STATUS_A, context) & CMOS_UIP) == 0;
}

static int
bcd_value(uint8_t value, unsigned *result)
{
	unsigned high = (value >> 4) & 0x0fU;
	unsigned low = value & 0x0fU;

	if (high > 9U || low > 9U)
		return 0;
	*result = high * 10U + low;
	return 1;
}

static int
field_value(uint8_t value, int binary, unsigned *result)
{
	if (binary) {
		*result = value;
		return 1;
	}
	return bcd_value(value, result);
}

static int
leap_year(unsigned year)
{
	return (year % 4U) == 0U &&
	    ((year % 100U) != 0U || (year % 400U) == 0U);
}

static unsigned
month_days(unsigned year, unsigned month)
{
	static const uint8_t days[] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};

	if (month == 0 || month > 12)
		return 0;
	return month == 2 && leap_year(year) ? 29U : days[month - 1U];
}

static int
snapshot_seconds(const struct rtc_snapshot *snapshot, uint64_t *result)
{
	unsigned second, minute, hour, day, month, year, current;
	uint64_t days = 0;
	int binary = (snapshot->status_b & CMOS_BINARY) != 0;
	int pm = (snapshot->hour & CMOS_PM) != 0;
	uint8_t raw_hour = snapshot->hour & ~CMOS_PM;

	if (!field_value(snapshot->second, binary, &second) ||
	    !field_value(snapshot->minute, binary, &minute) ||
	    !field_value(raw_hour, binary, &hour) ||
	    !field_value(snapshot->day, binary, &day) ||
	    !field_value(snapshot->month, binary, &month) ||
	    !field_value(snapshot->year, binary, &year))
		return 0;
	year += 2000U; /* PC/AT HAL intentionally supports 2000..2099. */
	if ((snapshot->status_b & CMOS_24_HOUR) == 0) {
		if (hour == 0 || hour > 12)
			return 0;
		if (hour == 12)
			hour = 0;
		if (pm)
			hour += 12;
	} else if (pm) {
		return 0;
	}
	if (second > 59 || minute > 59 || hour > 23 ||
	    day == 0 || day > month_days(year, month))
		return 0;
	for (current = 1970; current < year; current++)
		days += leap_year(current) ? 366U : 365U;
	for (current = 1; current < month; current++)
		days += month_days(year, current);
	days += day - 1U;
	*result = days * 86400U + hour * 3600U + minute * 60U + second;
	return 1;
}

bool
x86_cmos_rtc_read(x86_cmos_read_fn read, void *context, uint64_t *unix_seconds)
{
	struct rtc_snapshot first, second;
	unsigned retry;

	if (read == NULL || unix_seconds == NULL)
		return false;
	for (retry = 0; retry < CMOS_SNAPSHOT_RETRIES; retry++) {
		if (!snapshot_read(read, context, &first) ||
		    !snapshot_read(read, context, &second))
			continue;
		if (snapshot_equal(&first, &second))
			return snapshot_seconds(&second, unix_seconds) != 0;
	}
	return false;
}
