/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes or sets the date and time (POSIX XCU date).
 *
 *	date [-u] [+format]
 *	date [-u] mmddhhmm[[cc]yy]
 *
 * The current time is written in the format given after +, whose
 * conversions are those of strftime(), or in the default format
 * "%a %b %e %H:%M:%S %Z %Y".  The E and O modifiers are accepted and, in
 * the POSIX locale, change nothing.  -u uses Coordinated Universal Time
 * instead of the TZ time zone.  A digits operand sets the system clock to
 * that local time (month, day, hour, minute, and optionally the century
 * and year; a two-digit year of 69 to 99 is in the 1900s), which only a
 * privileged user may do.
 *
 * GNU's extensions are taken too (ws045): -d date (--date) writes that
 * date instead of now (command_parse_date reads @SECONDS, ISO dates and
 * times, YYYYMMDD and relative items), -r file (--reference) the time the
 * file was last modified, -R (--rfc-email) the mail format and -I[spec]
 * (--iso-8601) ISO 8601; the format also takes %N (nanoseconds), %s
 * (seconds since the epoch) and %:z (the offset with a colon).  The long
 * options are GNU's, and options may follow operands unless
 * POSIXLY_CORRECT is set.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "include/kern/os-name.h"

/* The format without a + operand. */
#define DATE_DEFAULT_FORMAT "%a %b %e %H:%M:%S %Z %Y"

/* The largest output of one format. */
#define DATE_OUTPUT_SIZE 8192

/* The format of -R. */
#define DATE_MAIL_FORMAT "%a, %d %b %Y %H:%M:%S %z"

/* The codes of the long options that have no letter. */
#define OPTION_HELP 256
#define OPTION_VERSION 257

/*
 * The options written in full, read by the scan of the command line; a
 * long option shares its code with its letter.
 */
static const struct command_long_option date_long_options[] = {
	{"date", COMMAND_VALUE_REQUIRED, 'd'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"iso-8601", COMMAND_VALUE_OPTIONAL, 'I'},
	{"reference", COMMAND_VALUE_REQUIRED, 'r'},
	{"rfc-email", COMMAND_VALUE_NONE, 'R'},
	{"universal", COMMAND_VALUE_NONE, 'u'},
	{"utc", COMMAND_VALUE_NONE, 'u'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{NULL, 0, 0}
};

/*
 * What the command line asks for: -u, the date of -d, the file of -r, and
 * the format of -R or -I.
 */
struct date_options {
	int utc;
	const char *date;
	const char *reference;
	const char *format;
};

static int read_options(int argc, char **argv, struct date_options *options);
static const char *iso_format(const char *spec);
static int find_moment(const struct date_options *options, struct timespec *moment);
static int write_date(const char *format, const struct timespec *moment);
static int expand_format(const char *format, const struct timespec *moment, const struct tm *broken, char *out, size_t size);
static int set_date(const char *operand);
static int read_digits(const char *text, int count);
static void strip_modifiers(const char *format, char *out, size_t size);
static void usage(void);

/*
 * Runs date.
 */
int
main(
	int argc,
	char **argv)
{
	struct date_options options;
	struct timespec moment;
	int count;
	int status;

	/* Reads the options; -u puts the time zone at UTC. */
	count = read_options(argc, argv, &options);
	if (options.utc)
		setenv("TZ", "UTC0", 1);

	/* Reads the time zone, now that -u may have set it. */
	tzset();

	/* At most one operand. */
	if (count > 1)
		usage();

	/* Anything but +format sets the clock, which -d and -r do not. */
	if (count == 1 && argv[1][0] != '+') {
		if (options.date != NULL || options.reference != NULL)
			usage();
		status = set_date(argv[1]);
		return status;
	}

	/* + and a format: that format. */
	if (count == 1)
		options.format = argv[1] + 1;

	/* The moment: now, -d's date or -r's file. */
	status = find_moment(&options, &moment);
	if (status != 0)
		return 1;

	/* Writes it. */
	status = write_date(options.format, &moment);
	return status;
}

/*
 * Reads the options; returns the number of operands, which are left in
 * argv from argv[1] on.
 */
static int
read_options(
	int argc,
	char **argv,
	struct date_options *options)
{
	struct command_options scan;
	int option;

	/* Now, in the default format. */
	memset(options, 0, sizeof(*options));
	options->format = DATE_DEFAULT_FORMAT;

	/* Reads each option, and the long forms. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "date";
	scan.letters = "ud:r:RI::";
	scan.names = date_long_options;
	command_options_start(&scan);
	for (;;) {
		option = command_options_next(&scan);
		if (option == COMMAND_OPTION_END)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'u':
			options->utc = 1;
			break;
		case 'd':
			options->date = scan.value;
			break;
		case 'r':
			options->reference = scan.value;
			break;
		case 'R':
			options->format = DATE_MAIL_FORMAT;
			break;
		case 'I':
			options->format = iso_format(scan.value);
			break;
		case OPTION_VERSION:
			printf("date (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
			break;
		default:
			usage();
			break;
		}
	}

	/* Reports how many operands there are. */
	return scan.operand_count;
}

/* Returns the format of -I[spec]: date, hours, minutes, seconds or ns. */
static const char *
iso_format(
	const char *spec)
{
	static const char *const specs[][2] = {
		{"date", "%Y-%m-%d"},
		{"hours", "%Y-%m-%dT%H%:z"},
		{"minutes", "%Y-%m-%dT%H:%M%:z"},
		{"seconds", "%Y-%m-%dT%H:%M:%S%:z"},
		{"ns", "%Y-%m-%dT%H:%M:%S,%N%:z"}
	};
	size_t length;
	size_t index;
	int differs;

	/* No spec is the date. */
	if (spec == NULL)
		return specs[0][1];

	/* The spec, or the beginning of one. */
	length = strlen(spec);
	for (index = 0; index < sizeof(specs) / sizeof(specs[0]); index++) {
		differs = strncmp(spec, specs[index][0], length);
		if (differs == 0)
			return specs[index][1];
	}

	/* Any other spec. */
	fprintf(stderr, "date: invalid argument '%s' for '--iso-8601'\n", spec);
	exit(1);
}

/*
 * Finds the moment to write: now, the date of -d, or the modification
 * time of -r's file.  Returns -1 after a message when there is none.
 */
static int
find_moment(
	const struct date_options *options,
	struct timespec *moment)
{
	struct timespec now;
	struct stat status_of_file;
	int status;
	int valid;

	/* Now. */
	status = clock_gettime(CLOCK_REALTIME, &now);
	if (status != 0) {
		fprintf(stderr, "date: cannot read the clock: %s\n", strerror(errno));
		return -1;
	}

	/* -r: the time the file was last modified. */
	if (options->reference != NULL) {
		status = stat(options->reference, &status_of_file);
		if (status != 0) {
			fprintf(stderr, "date: %s: %s\n", options->reference, strerror(errno));
			return -1;
		}

		/* The file's time. */
		*moment = status_of_file.st_mtim;
		return 0;
	}

	/* -d: the date given, in the time zone. */
	if (options->date != NULL) {
		valid = command_parse_date(options->date, &now, options->utc, moment);
		if (!valid) {
			fprintf(stderr, "date: invalid date '%s'\n", options->date);
			return -1;
		}

		/* The date given. */
		return 0;
	}

	/* Succeeded: now. */
	*moment = now;
	return 0;
}

/* Writes a moment in a format. */
static int
write_date(
	const char *format,
	const struct timespec *moment)
{
	char expanded[DATE_OUTPUT_SIZE];
	char stripped[DATE_OUTPUT_SIZE];
	char output[DATE_OUTPUT_SIZE];
	struct tm *broken;
	size_t length;
	int status;

	/* Breaks it down in the time zone. */
	broken = localtime(&moment->tv_sec);
	if (broken == NULL) {
		fprintf(stderr, "date: cannot convert the time: %s\n", strerror(errno));
		return 1;
	}

	/*
	 * Formats it; GNU's conversions are written out and the modifiers
	 * dropped first.  strftime() gives 0 both for an empty result and for
	 * one too long, so an empty format is told apart first.
	 */
	status = expand_format(format, moment, broken, expanded, sizeof(expanded));
	if (status != 0) {
		fprintf(stderr, "date: format too long\n");
		return 1;
	}

	/* Then the modifiers go. */
	strip_modifiers(expanded, stripped, sizeof(stripped));
	length = 0;
	if (stripped[0] != '\0') {
		length = strftime(output, sizeof(output), stripped, broken);
		if (length == 0)
			output[0] = '\0';
	}

	/* Terminates the text. */
	output[length] = '\0';

	/* Writes it and its newline. */
	printf("%s\n", output);
	fflush(stdout);
	status = ferror(stdout);
	if (status != 0) {
		fprintf(stderr, "date: write error\n");
		return 1;
	}

	/* Succeeded: the date was written. */
	return 0;
}

/*
 * Copies a format with GNU's %N (nanoseconds, nine digits), %s (seconds
 * since the epoch) and %:z (the offset as +hh:mm) written out, for
 * strftime() to do the rest.  Returns -1 when it does not fit.
 */
static int
expand_format(
	const char *format,
	const struct timespec *moment,
	const struct tm *broken,
	char *out,
	size_t size)
{
	char number[32];
	const char *cursor;
	size_t length;
	size_t used;
	size_t zone;

	/* Each character, and each conversion whole. */
	used = 0;
	for (cursor = format; *cursor != '\0'; cursor++) {
		/* %N, %s and %:z become their text; %% stays whole. */
		number[0] = '\0';
		length = 0;
		if (cursor[0] == '%' && cursor[1] == 'N') {
			snprintf(number, sizeof(number), "%09ld", (long)moment->tv_nsec);
		} else if (cursor[0] == '%' && cursor[1] == 's') {
			snprintf(number, sizeof(number), "%lld", (long long)moment->tv_sec);
		} else if (cursor[0] == '%' && cursor[1] == ':' && cursor[2] == 'z') {
			zone = strftime(number, sizeof(number) - 1, "%z", broken);
			if (zone == 5) {
				memmove(number + 4, number + 3, 3);
				number[3] = ':';
			}

			/* The conversion is one byte longer. */
			cursor++;
		} else if (cursor[0] == '%' && cursor[1] != '\0') {
			number[0] = cursor[0];
			number[1] = cursor[1];
			number[2] = '\0';
		} else {
			number[0] = cursor[0];
			number[1] = '\0';
			length = 1;
		}

		/* The text in the conversion's place; a conversion is two bytes. */
		if (length == 0) {
			length = strlen(number);
			cursor++;
		}

		/* It must fit with the end of the text. */
		if (used + length + 1 > size)
			return -1;
		memcpy(out + used, number, length);
		used += length;
	}

	/* Succeeded: the format, ended. */
	out[used] = '\0';
	return 0;
}

/*
 * Copies a format without the E and O modifiers, which the POSIX locale
 * does not use: %Ey is %y and %Od is %d.
 */
static void
strip_modifiers(
	const char *format,
	char *out,
	size_t size)
{
	size_t used;
	const char *cursor;

	/* Copies byte by byte, dropping a modifier after a %. */
	used = 0;
	for (cursor = format; *cursor != '\0' && used + 2 < size; cursor++) {
		out[used] = *cursor;
		used++;
		if (*cursor != '%')
			continue;

		/* The byte after % is copied, after any modifier. */
		cursor++;
		if (*cursor == 'E' || *cursor == 'O')
			cursor++;
		if (*cursor == '\0')
			break;
		out[used] = *cursor;
		used++;
	}

	/* Terminates the copy. */
	out[used] = '\0';
}

/*
 * Sets the system clock from mmddhhmm[[cc]yy] in local time.  The current
 * year is kept when none is given.
 */
static int
set_date(
	const char *operand)
{
	struct timespec now;
	struct tm *current;
	struct tm wanted;
	time_t when;
	size_t length;
	size_t index;
	int year;
	int day;
	int valid;
	int status;

	/* The operand is 8, 10 or 12 digits. */
	length = strlen(operand);
	if (length != 8 && length != 10 && length != 12) {
		fprintf(stderr, "date: invalid date: '%s'\n", operand);
		return 1;
	}

	/* The operand must be all digits. */
	for (index = 0; index < length; index++) {
		if (operand[index] < '0' || operand[index] > '9') {
			fprintf(stderr, "date: invalid date: '%s'\n", operand);
			return 1;
		}
	}

	/* The current year, for an operand without one. */
	status = clock_gettime(CLOCK_REALTIME, &now);
	if (status != 0) {
		fprintf(stderr, "date: cannot read the clock: %s\n", strerror(errno));
		return 1;
	}

	/* Breaks the current time down for its year. */
	current = localtime(&now.tv_sec);
	if (current == NULL) {
		fprintf(stderr, "date: cannot convert the time: %s\n", strerror(errno));
		return 1;
	}

	/* The fields of the new time. */
	memset(&wanted, 0, sizeof(wanted));
	wanted.tm_mon = read_digits(operand, 2) - 1;
	wanted.tm_mday = read_digits(operand + 2, 2);
	wanted.tm_hour = read_digits(operand + 4, 2);
	wanted.tm_min = read_digits(operand + 6, 2);
	year = current->tm_year + 1900;
	if (length == 12) {
		year = read_digits(operand + 8, 4);
	} else if (length == 10) {
		year = read_digits(operand + 8, 2);
		if (year >= 69)
			year += 1900;
		else
			year += 2000;
	}

	/* Keeps the year and lets mktime() find out about daylight saving. */
	wanted.tm_year = year - 1900;
	wanted.tm_isdst = -1;

	/* The fields must be in range; mktime() would carry them over. */
	day = wanted.tm_mday;
	valid = 1;
	if (wanted.tm_mon < 0 || wanted.tm_mon > 11)
		valid = 0;
	else if (day < 1 || day > 31)
		valid = 0;
	else if (wanted.tm_hour > 23 || wanted.tm_min > 59)
		valid = 0;
	if (!valid) {
		fprintf(stderr, "date: invalid date: '%s'\n", operand);
		return 1;
	}

	/* Converts it; a day past the end of the month moves and is refused. */
	when = mktime(&wanted);
	if (when == (time_t)-1 || wanted.tm_mday != day) {
		fprintf(stderr, "date: invalid date: '%s'\n", operand);
		return 1;
	}

	/* Sets the clock. */
	now.tv_sec = when;
	now.tv_nsec = 0;
	status = clock_settime(CLOCK_REALTIME, &now);
	if (status != 0) {
		fprintf(stderr, "date: cannot set the date: %s\n", strerror(errno));
		return 1;
	}

	/* Writes the new date. */
	status = write_date(DATE_DEFAULT_FORMAT, &now);
	if (status != 0)
		return 1;

	/* Succeeded: the clock is set. */
	return 0;
}

/* Reads a number of decimal digits. */
static int
read_digits(
	const char *text,
	int count)
{
	int value;
	int index;

	/* Accumulates the digits. */
	value = 0;
	for (index = 0; index < count; index++)
		value = value * 10 + (text[index] - '0');

	/* Reports the number. */
	return value;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX forms. */
	fprintf(stderr,
		"usage: date [-u] [-d date | -r file] [-R | -I[spec]] [+format]\n"
		"       date [-u] mmddhhmm[[cc]yy]\n");
	exit(1);
}
