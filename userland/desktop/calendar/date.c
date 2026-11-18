/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calendar's days (WS155 p000; calendar.h): the Gregorian months, the day
 * of the week, and moving by months.
 */

#include "calendar.h"

/* The months' and the days' names. */
static const char *const date_months[] = {
	"January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December"
};
static const char *const date_weekdays[] = {
	"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

/*
 * Reports how many days a month of a year has.
 */
int
cal_days_in_month(
	int year,
	int month)
{
	static const int lengths[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int leap;

	/* A leap year: every fourth year, but not a century's unless it is a fourth one. */
	leap = 0;
	if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
		leap = 1;

	/* February of a leap year. */
	if (month == 2 && leap)
		return 29;

	/* A month not known has none. */
	if (month < 1 || month > 12)
		return 0;

	/* The month's length. */
	return lengths[month - 1];
}

/*
 * Reports the day of the week of a day: 0 for Sunday to 6 for Saturday
 * (Sakamoto's way).
 */
int
cal_weekday(
	const struct cal_date *date)
{
	static const int shifts[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
	int year;

	/* January and February count as the year before's. */
	year = date->year;
	if (date->month < 3)
		year--;

	/* The day of the week. */
	return (year + year / 4 - year / 100 + year / 400 + shifts[date->month - 1] + date->day) % 7;
}

/*
 * Moves a day by a number of months, keeping its day of the month within
 * the month it lands in.
 */
void
cal_add_months(
	struct cal_date *date,
	int months)
{
	int index;
	int length;

	/* The month counted from year 0, moved, and back. */
	index = date->year * 12 + (date->month - 1) + months;
	date->year = index / 12;
	date->month = index % 12 + 1;

	/* A day past the month's end goes to its last day. */
	length = cal_days_in_month(date->year, date->month);
	if (date->day > length)
		date->day = length;
}

/*
 * Moves a day by a number of days (back when it is negative).
 */
void
cal_add_days(
	struct cal_date *date,
	int days)
{
	int length;

	/* Forward, a month's end at a time. */
	date->day += days;
	length = cal_days_in_month(date->year, date->month);
	while (date->day > length) {
		/* The rest of this month, and on into the next (a new year after December). */
		date->day -= length;
		date->month++;
		if (date->month > 12) {
			date->month = 1;
			date->year++;
		}

		/* The next month's length. */
		length = cal_days_in_month(date->year, date->month);
	}

	/* Back, a month's start at a time. */
	while (date->day < 1) {
		/* Into the month before (the year before's December after January). */
		date->month--;
		if (date->month < 1) {
			date->month = 12;
			date->year--;
		}

		/* Its days added. */
		date->day += cal_days_in_month(date->year, date->month);
	}
}

/*
 * Reports whether two days are the same one.
 */
int
cal_same_day(
	const struct cal_date *a,
	const struct cal_date *b)
{
	/* The year, the month and the day. */
	if (a->year != b->year ||
	    a->month != b->month ||
	    a->day != b->day)
		return 0;

	/* The same. */
	return 1;
}

/*
 * Reports how many months after a first one a day's month is.
 */
int
cal_month_index(
	const struct cal_date *first,
	const struct cal_date *date)
{
	/* The months between. */
	return (date->year - first->year) * 12 + (date->month - first->month);
}

/*
 * Reports a month's name (an empty one for a month not known).
 */
const char *
cal_month_name(
	int month)
{
	/* A month not known. */
	if (month < 1 || month > 12)
		return "";

	/* Its name. */
	return date_months[month - 1];
}

/*
 * Reports a day of the week's name (0 for Sunday).
 */
const char *
cal_weekday_name(
	int weekday)
{
	/* A day not known. */
	if (weekday < 0 || weekday > 6)
		return "";

	/* Its name. */
	return date_weekdays[weekday];
}
