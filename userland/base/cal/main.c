/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int
parse_number(const char *text, int minimum, int maximum, int *result)
{
	char *end;
	long value;

	if (text == NULL || *text == '\0')
		return 0;
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || *end != '\0' || value < minimum || value > maximum)
		return 0;
	*result = (int)value;
	return 1;
}

static int
leap_year(int year, int gregorian)
{
	if (!gregorian)
		return year % 4 == 0;
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int
month_days(int month, int year)
{
	static const unsigned char days[] = {
	    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};
	int gregorian = year > 1752 || (year == 1752 && month >= 9);

	if (month == 2 && leap_year(year, gregorian))
		return 29;
	return days[month - 1];
}

static long
julian_day_number(int year, int month, int day, int gregorian)
{
	long a = (14 - month) / 12;
	long y = (long)year + 4800 - a;
	long m = month + 12 * a - 3;
	long result = day + (153 * m + 2) / 5 + 365 * y + y / 4;

	if (gregorian)
		result += -y / 100 + y / 400 - 32045;
	else
		result -= 32083;
	return result;
}

static int
weekday(int year, int month, int day)
{
	int gregorian =
	    year > 1752 ||
	    (year == 1752 && (month > 9 || (month == 9 && day >= 14)));
	return (int)((julian_day_number(year, month, day, gregorian) + 1) % 7);
}

static void
month_name(int month, int year, char *result, size_t capacity)
{
	struct tm value = {0};

	value.tm_year = year - 1900;
	value.tm_mon = month - 1;
	value.tm_mday = 1;
	if (strftime(result, capacity, "%B", &value) == 0)
		(void)snprintf(result, capacity, "%d", month);
}

static void
weekday_header(void)
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
		if (strftime(name, sizeof(name), "%a", &value) == 0)
			(void)snprintf(name, sizeof(name), "D%d", day);
		printf("%.2s%s", name, day == 7 ? "\n" : " ");
	}
}

static void
print_month(int month, int year)
{
	char name[64];
	int column;
	int day;
	int limit = month_days(month, year);

	month_name(month, year, name, sizeof(name));
	printf("     %s %d\n", name, year);
	weekday_header();
	column = weekday(year, month, 1);
	for (day = 0; day < column; day++)
		printf("   ");
	for (day = 1; day <= limit; day++) {
		if (year == 1752 && month == 9 && day == 3)
			day = 14;
		printf("%2d", day);
		column++;
		if (column == 7) {
			putchar('\n');
			column = 0;
		} else if (day != limit) {
			putchar(' ');
		}
	}
	if (column != 0)
		putchar('\n');
}

int
main(int argc, char **argv)
{
	int month = 0;
	int year = 0;
	int index = 1;

	(void)setlocale(LC_ALL, "");
	if (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
		if (argv[index][1] == '-' && argv[index][2] == '\0')
			index++;
		else {
			fprintf(stderr, "usage: cal [[month] year]\n");
			return 1;
		}
	}
	if (argc - index == 0) {
		time_t now = time(NULL);
		struct tm *current = localtime(&now);
		if (current == NULL) {
			fprintf(stderr, "cal: cannot determine current date\n");
			return 1;
		}
		month = current->tm_mon + 1;
		year = current->tm_year + 1900;
	} else if (argc - index == 1) {
		if (!parse_number(argv[index], 1, 9999, &year)) {
			fprintf(stderr, "cal: invalid year: %s\n", argv[index]);
			return 1;
		}
	} else if (argc - index == 2) {
		if (!parse_number(argv[index], 1, 12, &month) ||
		    !parse_number(argv[index + 1], 1, 9999, &year)) {
			fprintf(stderr, "cal: invalid month or year\n");
			return 1;
		}
	} else {
		fprintf(stderr, "usage: cal [[month] year]\n");
		return 1;
	}
	if (month != 0) {
		print_month(month, year);
	} else {
		for (month = 1; month <= 12; month++) {
			if (month != 1)
				putchar('\n');
			print_month(month, year);
		}
	}
	return ferror(stdout) ? 1 : 0;
}
