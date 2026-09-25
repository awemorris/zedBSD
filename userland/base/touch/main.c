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
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The options: which times change, and to what. */
struct options {
	int access_only;
	int modification_only;
	int no_create;
	struct timespec times[2];
};

static int read_options(int argc, char **argv, struct options *options);
static const char *option_argument(int argc, char **argv, int *index, const char *rest);
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
	int first;
	int index;
	int error;
	int failed;

	/* The options, with the times now by default, and a file at least. */
	memset(&options, 0, sizeof(options));
	options.times[0].tv_nsec = UTIME_NOW;
	options.times[1].tv_nsec = UTIME_NOW;
	first = read_options(argc, argv, &options);
	if (first >= argc)
		usage();

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

/* Reads the options; returns the index of the first file. */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	const char *word;
	const char *letter;
	const char *argument;
	int index;
	int valid;

	/* Each word that starts with - and is not - alone; -- ends them. */
	for (index = 1; index < argc; index++) {
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* Each letter; -r, -t and -d take the rest or the next word. */
		for (letter = word + 1; *letter != '\0'; letter++) {
			switch (*letter) {
			case 'a':
				options->access_only = 1;
				continue;
			case 'm':
				options->modification_only = 1;
				continue;
			case 'c':
				options->no_create = 1;
				continue;
			case 'r':
				argument = option_argument(argc, argv, &index, letter + 1);
				reference_times(argument, options);
				break;
			case 't':
				argument = option_argument(argc, argv, &index, letter + 1);
				valid = parse_time(argument, options);
				if (!valid)
					invalid_date();
				break;
			case 'd':
				argument = option_argument(argc, argv, &index, letter + 1);
				valid = parse_date_time(argument, options);
				if (!valid)
					invalid_date();
				break;
			default:
				usage();
				break;
			}

			/* The argument took the rest of the word. */
			break;
		}
	}

	/* Succeeded: the first file. */
	return index;
}

/* Returns an option's argument: the rest of the word, or the next word. */
static const char *
option_argument(
	int argc,
	char **argv,
	int *index,
	const char *rest)
{
	/* The rest of the word. */
	if (*rest != '\0')
		return rest;

	/* The next word, which must be there. */
	if (*index + 1 >= argc)
		usage();
	(*index)++;

	/* Succeeded. */
	return argv[*index];
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
