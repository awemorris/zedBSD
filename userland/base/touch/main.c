/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Changes file access and modification times (POSIX XCU touch).
 *
 *	touch [-acm] [-r ref_file | -t time | -d date_time] file...
 *
 * The times are now, those of ref_file (-r), a local time
 * [[CC]YY]MMDDhhmm[.SS] (-t), or YYYY-MM-DDThh:mm:SS[.frac][Z] (-d, a
 * space may stand for the T, and Z means UTC).  -a changes only the access
 * time and -m only the modification time; neither changes both.  A missing
 * file is made, unless -c is given.
 *
 * GNU's extensions: -d takes the other dates GNU's touch does (@SECONDS,
 * a date without a time, relative items; see command_parse_date), the
 * long options (--reference, --date, --no-create, --time=atime|mtime), and
 * options after operands (unless POSIXLY_CORRECT is set).
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* The codes of the long options that have no letter. */
#define OPTION_TIME	256
#define OPTION_HELP	257
#define OPTION_VERSION	258

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option touch_long_options[] = {
	{"date", COMMAND_VALUE_REQUIRED, 'd'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"no-create", COMMAND_VALUE_NONE, 'c'},
	{"no-dereference", COMMAND_VALUE_NONE, 'h'},
	{"reference", COMMAND_VALUE_REQUIRED, 'r'},
	{"time", COMMAND_VALUE_REQUIRED, OPTION_TIME},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{NULL, 0, 0}
};

/* The options: which times change, and to what. */
struct options {
	int access_only;
	int modification_only;
	int no_create;
	struct timespec times[2];
};

static int read_options(int argc, char **argv, struct options *options);
static void apply_time_word(const char *word, struct options *options);
static void gnu_date(const char *text, struct options *options);
static void reference_times(const char *path, struct options *options);
static int parse_time(const char *text, struct options *options);
static int parse_date_time(const char *text, struct options *options);
static void invalid_date(void);
static int digits(const char *text, size_t count, int *value);
static int touch_file(const struct options *options, const char *path);
static void usage(void);

/*
 * Runs touch.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	int count;
	int first;
	int index;
	int error;
	int failed;

	/* The options, with the times now by default, and a file at least. */
	memset(&options, 0, sizeof(options));
	options.times[0].tv_nsec = UTIME_NOW;
	options.times[1].tv_nsec = UTIME_NOW;
	count = read_options(argc, argv, &options);
	if (count == 0)
		usage();
	first = 1;
	argc = first + count;

	/* -a leaves the modification time, and -m the access time. */
	if (options.access_only && !options.modification_only)
		options.times[1].tv_nsec = UTIME_OMIT;
	if (options.modification_only && !options.access_only)
		options.times[0].tv_nsec = UTIME_OMIT;

	/* Each file. */
	failed = 0;
	for (index = first; index < argc; index++) {
		error = touch_file(&options, argv[index]);
		if (error != 0)
			failed = 1;
	}

	/* Some file could not be touched. */
	if (failed)
		return 1;

	/* Succeeded. */
	return 0;
}

/*
 * Reads the options; returns the number of files, which are left in argv
 * from argv[1] on.
 */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	struct command_options scan;
	int valid;
	int code;

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "touch";
	scan.letters = "acfhmr:t:d:";
	scan.names = touch_long_options;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;

		/* The option of its code. */
		switch (code) {
		case 'a':
			options->access_only = 1;
			break;
		case 'm':
			options->modification_only = 1;
			break;
		case 'c':
			options->no_create = 1;
			break;
		case 'f':
		case 'h':
			/* Nothing to force; links are followed. */
			break;
		case 'r':
			reference_times(scan.value, options);
			break;
		case 't':
			valid = parse_time(scan.value, options);
			if (!valid)
				invalid_date();
			break;
		case 'd':
			/* POSIX's form, or one of GNU's. */
			valid = parse_date_time(scan.value, options);
			if (!valid)
				gnu_date(scan.value, options);
			break;
		case OPTION_TIME:
			apply_time_word(scan.value, options);
			break;
		case OPTION_VERSION:
			printf("touch (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
		default:
			usage();
			break;
		}
	}

	/* Succeeded: the files follow argv[0]. */
	return scan.operand_count;
}

/* Applies --time=WORD: atime, access or use is -a; mtime or modify is -m. */
static void
apply_time_word(
	const char *word,
	struct options *options)
{
	static const char *const access_words[] = {"atime", "access", "use"};
	static const char *const modify_words[] = {"mtime", "modify"};
	size_t index;
	int differs;

	/* A word of the access time. */
	for (index = 0; index < 3U; index++) {
		differs = strcmp(word, access_words[index]);
		if (differs == 0) {
			options->access_only = 1;
			return;
		}
	}

	/* A word of the modification time. */
	for (index = 0; index < 2U; index++) {
		differs = strcmp(word, modify_words[index]);
		if (differs == 0) {
			options->modification_only = 1;
			return;
		}
	}

	/* Any other word. */
	fprintf(stderr, "touch: invalid argument '%s' for '--time'\n", word);
	exit(1);
}

/* Reads -d's date in one of GNU's forms, ending touch when it is none. */
static void
gnu_date(
	const char *text,
	struct options *options)
{
	struct timespec now;
	struct timespec moment;
	int result;
	int valid;

	/* Now, which the date is relative to. */
	result = clock_gettime(CLOCK_REALTIME, &now);
	if (result != 0)
		invalid_date();

	/* The date. */
	valid = command_parse_date(text, &now, 0, &moment);
	if (!valid)
		invalid_date();

	/* Succeeded: both times. */
	options->times[0] = moment;
	options->times[1] = moment;
}

/* Takes both times from a reference file (-r). */
static void
reference_times(
	const char *path,
	struct options *options)
{
	struct stat status;
	int error;

	/* The file's times. */
	error = stat(path, &status);
	if (error != 0) {
		fprintf(stderr, "touch: %s: %s\n", path, strerror(errno));
		exit(1);
	}

	/* Succeeded. */
	options->times[0] = status.st_atim;
	options->times[1] = status.st_mtim;
}

/* Parses -t [[CC]YY]MMDDhhmm[.SS], a local time; returns 0 when invalid. */
static int
parse_time(
	const char *text,
	struct options *options)
{
	struct tm moment;
	struct tm *now;
	const char *point;
	size_t length;
	time_t seconds;
	int year;
	int century;
	int value;
	int valid;

	/* The digits before the point, and the seconds after it. */
	memset(&moment, 0, sizeof(moment));
	point = strchr(text, '.');
	length = strlen(text);
	if (point != NULL) {
		length = (size_t)(point - text);
		valid = digits(point + 1, 2, &moment.tm_sec);
		if (!valid || point[3] != '\0')
			return 0;
	}

	/* The year: none (this year), YY, or CCYY. */
	year = -1;
	if (length == 12) {
		valid = digits(text, 4, &year);
		if (!valid)
			return 0;
		text += 4;
	} else if (length == 10) {
		valid = digits(text, 2, &value);
		if (!valid)
			return 0;
		century = 1900;
		if (value < 69)
			century = 2000;
		year = century + value;
		text += 2;
	} else if (length != 8) {
		return 0;
	}

	/* The month, day, hour and minute. */
	valid = digits(text, 2, &moment.tm_mon);
	if (!valid)
		return 0;
	valid = digits(text + 2, 2, &moment.tm_mday);
	if (!valid)
		return 0;
	valid = digits(text + 4, 2, &moment.tm_hour);
	if (!valid)
		return 0;
	valid = digits(text + 6, 2, &moment.tm_min);
	if (!valid)
		return 0;

	/* This year when none is given. */
	if (year < 0) {
		seconds = time(NULL);
		now = localtime(&seconds);
		year = now->tm_year + 1900;
	}

	/* Succeeded: the local time. */
	moment.tm_year = year - 1900;
	moment.tm_mon -= 1;
	moment.tm_isdst = -1;
	seconds = mktime(&moment);
	options->times[0].tv_sec = seconds;
	options->times[0].tv_nsec = 0;
	options->times[1] = options->times[0];
	return 1;
}

/* Parses -d YYYY-MM-DDThh:mm:SS[.frac][Z]; returns 0 when invalid. */
static int
parse_date_time(
	const char *text,
	struct options *options)
{
	struct tm moment;
	const char *cursor;
	time_t seconds;
	long nanoseconds;
	long scale;
	int year;
	int valid;
	int utc;

	/* YYYY-MM-DD, then T or a space, then hh:mm:SS. */
	memset(&moment, 0, sizeof(moment));
	valid = digits(text, 4, &year);
	if (!valid || text[4] != '-')
		return 0;
	valid = digits(text + 5, 2, &moment.tm_mon);
	if (!valid || text[7] != '-')
		return 0;
	valid = digits(text + 8, 2, &moment.tm_mday);
	if (!valid || (text[10] != 'T' && text[10] != ' '))
		return 0;
	valid = digits(text + 11, 2, &moment.tm_hour);
	if (!valid || text[13] != ':')
		return 0;
	valid = digits(text + 14, 2, &moment.tm_min);
	if (!valid || text[16] != ':')
		return 0;
	valid = digits(text + 17, 2, &moment.tm_sec);
	if (!valid)
		return 0;

	/* A fraction of a second, after a point or a comma. */
	cursor = text + 19;
	nanoseconds = 0;
	if (*cursor == '.' || *cursor == ',') {
		cursor++;
		scale = 100000000L;
		while (*cursor >= '0' && *cursor <= '9') {
			nanoseconds += (long)(*cursor - '0') * scale;
			scale /= 10;
			cursor++;
		}
	}

	/* Z is UTC; anything else after it is an error. */
	utc = 0;
	if (*cursor == 'Z') {
		utc = 1;
		cursor++;
	}

	/* Nothing may follow. */
	if (*cursor != '\0')
		return 0;

	/* Succeeded: UTC or the local time. */
	moment.tm_year = year - 1900;
	moment.tm_mon -= 1;
	moment.tm_isdst = -1;
	if (utc)
		seconds = timegm(&moment);
	else
		seconds = mktime(&moment);
	options->times[0].tv_sec = seconds;
	options->times[0].tv_nsec = nanoseconds;
	options->times[1] = options->times[0];
	return 1;
}

/* Reports a time that cannot be read and ends touch. */
static void
invalid_date(
	void)
{
	/* The error. */
	fprintf(stderr, "touch: invalid date format\n");
	exit(1);
}

/* Reads a number of decimal digits; returns 0 when they are not all there. */
static int
digits(
	const char *text,
	size_t count,
	int *value)
{
	size_t index;

	/* Each digit. */
	*value = 0;
	for (index = 0; index < count; index++) {
		if (text[index] < '0' || text[index] > '9')
			return 0;
		*value = *value * 10 + (text[index] - '0');
	}

	/* Succeeded. */
	return 1;
}

/* Touches a file: made when missing (unless -c), then given the times. */
static int
touch_file(
	const struct options *options,
	const char *path)
{
	int descriptor;
	int error;
	int missing;

	/* A missing file is made, or with -c left alone. */
	error = access(path, F_OK);
	missing = 0;
	if (error != 0 && errno == ENOENT)
		missing = 1;
	if (missing && options->no_create)
		return 0;
	if (missing) {
		descriptor = open(path, O_WRONLY | O_CREAT, 0666);
		if (descriptor < 0) {
			fprintf(stderr, "touch: %s: %s\n", path, strerror(errno));
			return -1;
		}

		/* The new file is closed; its times are set below. */
		close(descriptor);
	}

	/* The times. */
	error = utimensat(AT_FDCWD, path, options->times, 0);
	if (error != 0) {
		fprintf(stderr, "touch: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Reports the usage and ends touch. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: touch [-acm] [-r ref_file | -t time | -d date_time] file...\n");
	exit(1);
}
