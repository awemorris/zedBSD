/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD cal userland command.
 */

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int parse_number(const char *text, int minimum, int maximum, int *result);
static void print_month(int month, int year);
static int month_days(int month, int year);
static int leap_year(int year, int gregorian);
static void month_name(int month, int year, char *result, size_t capacity);
static void weekday_header(void);
static int weekday(int year, int month, int day);
static long julian_day_number(int year, int month, int day, int gregorian);

/*
 * Runs the cal command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	time_t now;
	struct tm *current;
	int month;
	int year;
	int index;

	(void)setlocale(LC_ALL, "");

	month = 0;
	year = 0;
	index = 1;

	/* Validates the command-line arguments. */
	if (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
		/* Validates the command-line arguments. */
		if (argv[index][1] == '-' && argv[index][2] == '\0')
			index++;
		else {
			fprintf(stderr, "usage: cal [[month] year]\n");

			/* Reports operation failure. */
			return 1;
		}
	}

	/* Validates the command-line arguments. */
	if (argc - index == 0) {
				now = time(NULL);
				current = localtime(&now);

		/* Handles the current availability. */
		if (current == NULL) {
			fprintf(stderr, "cal: cannot determine current date\n");

			/* Reports operation failure. */
			return 1;
		}
		month = current->tm_mon + 1;
		year = current->tm_year + 1900;
	} else if (argc - index == 1) {
		/* Validates the command-line arguments. */
		if (!parse_number(argv[index], 1, 9999, &year)) {
			fprintf(stderr, "cal: invalid year: %s\n", argv[index]);

			/* Reports operation failure. */
			return 1;
		}
	} else if (argc - index == 2) {
		/* Validates the command-line arguments. */
		if (!parse_number(argv[index], 1, 12, &month) ||
		    !parse_number(argv[index + 1], 1, 9999, &year)) {
			fprintf(stderr, "cal: invalid month or year\n");

			/* Reports operation failure. */
			return 1;
		}
	} else {
		fprintf(stderr, "usage: cal [[month] year]\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the month condition. */
	if (month != 0) {
		print_month(month, year);
	} else {
		/* Process each element required by the operation. */
		for (month = 1; month <= 12; month++) {
			/* Handles the month condition. */
			if (month != 1)
				putchar('\n');
			print_month(month, year);
		}
	}

	/* Computes the function result. */
	function_result = ferror(stdout) ? 1 : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the parse number operation. */
static int
parse_number(
	const char *text,
	int minimum,
	int maximum,
	int *result)
{
	char *end;
	long value;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0')
		return 0;
	errno = 0;
	value = strtol(text, &end, 10);

	/* Handles the reported system error. */
	if (errno != 0 || *end != '\0' || value < minimum || value > maximum)
		return 0;
	*result = (int)value;
	/* Reports operation failure. */
	return 1;
}

/* Supports the print month operation. */
static void
print_month(
	int month,
	int year)
{
	char name[64];
	int column;
	int day;
	int limit;

	limit = month_days(month, year);

	month_name(month, year, name, sizeof(name));
	printf("     %s %d\n", name, year);
	weekday_header();

	/* Process each element required by the operation. */
	column = weekday(year, month, 1);
	for (day = 0; day < column; day++)
		printf("   ");

	/* Process each element required by the operation. */
	for (day = 1; day <= limit; day++) {
		/* Handles the year condition. */
		if (year == 1752 && month == 9 && day == 3)
			day = 14;
		printf("%2d", day);
		column++;

		/* Handles the column condition. */
		if (column == 7) {
			putchar('\n');
			column = 0;
		} else if (day != limit) {
			putchar(' ');
		}
	}

	/* Handles the column condition. */
	if (column != 0)
		putchar('\n');
}

/* Supports the month days operation. */
static int
month_days(
	int month,
	int year)
{
	static const unsigned char days[] = {
	    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};
	int gregorian;

	gregorian = year > 1752 || (year == 1752 && month >= 9);

	/* Handles a failed leap year operation. */
	if (month == 2 && leap_year(year, gregorian))
		return 29;

	/* Returns the computed result. */
	return days[month - 1];
}

/* Supports the leap year operation. */
static int
leap_year(
	int year,
	int gregorian)
{
	/* Handles the gregorian condition. */
	if (!gregorian)
		return year % 4 == 0;

	/* Returns the computed result. */
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/* Supports the month name operation. */
static void
month_name(
	int month,
	int year,
	char *result,
	size_t capacity)
{
	struct tm value = {0};

	value.tm_year = year - 1900;
	value.tm_mon = month - 1;
	value.tm_mday = 1;

	/* Handles a failed strftime operation. */
	if (strftime(result, capacity, "%B", &value) == 0)
		(void)snprintf(result, capacity, "%d", month);
}

/* Supports the weekday header operation. */
static void
weekday_header(
	void)
{
	struct tm value = {0};
	char name[16];
	int day;

	/* 2023-01-01 was a Sunday. */
	for (day = 1; day <= 7; day++) {
		value.tm_year = 123;
		value.tm_mon = 0;
		value.tm_mday = day;
		value.tm_wday = day - 1;

		/* Handles a failed strftime operation. */
		if (strftime(name, sizeof(name), "%a", &value) == 0)
			(void)snprintf(name, sizeof(name), "D%d", day);
		printf("%.2s%s", name, day == 7 ? "\n" : " ");
	}
}

/* Supports the weekday operation. */
static int
weekday(
	int year,
	int month,
	int day)
{
	int function_result;
	int gregorian;

	gregorian = year > 1752 ||
	    (year == 1752 && (month > 9 || (month == 9 && day >= 14)));

	/* Computes the function result. */
	function_result = (int)((julian_day_number(year, month, day, gregorian) + 1) % 7);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the julian day number operation. */
static long
julian_day_number(
	int year,
	int month,
	int day,
	int gregorian)
{
	long a;
	long y;
	long m;
	long result;

	a = (14 - month) / 12;
	y = (long)year + 4800 - a;
	m = month + 12 * a - 3;
	result = day + (153 * m + 2) / 5 + 365 * y + y / 4;

	/* Handles the gregorian condition. */
	if (gregorian)
		result += -y / 100 + y / 400 - 32045;
	else
		result -= 32083;

	/* Returns the computed result. */
	return result;
}
