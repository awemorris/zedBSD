/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland command support.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * A date being read by command_parse_date: where the reading is, and the
 * items found so far.  The relative items add up; the others are set once.
 */
struct command_date {
	const char *cursor;

	/* The date, the time and the zone, when given. */
	int have_date;
	int year;
	int month;
	int day;
	int have_time;
	int hour;
	int minute;
	int second;
	long nanoseconds;
	int have_zone;
	long zone_offset;

	/* today: the time is midnight. */
	int midnight;

	/* The moves: seconds, days, months and years. */
	time_t relative_seconds;
	int relative_days;
	int relative_months;
	int relative_years;
};

extern char **environ;

static int options_letter(struct command_options *scan);
static int options_long(struct command_options *scan, const char *text);
static int options_find_long(const struct command_options *scan, const char *name, size_t length);
static void options_operand(struct command_options *scan, char *argument);
static void options_rest(struct command_options *scan);
static int date_epoch(struct command_date *date, struct timespec *moment);
static int date_item(struct command_date *date);
static int date_iso_date(struct command_date *date);
static int date_time(struct command_date *date);
static int date_signed(struct command_date *date);
static int date_move(struct command_date *date, long long count);
static int date_word(struct command_date *date);
static size_t date_word_text(struct command_date *date, char *word, size_t size);
static int date_number(struct command_date *date, long long *value, size_t *digits);
static long date_fraction(struct command_date *date);
static void date_skip_blanks(struct command_date *date);

/*
 * Starts a scan of the command line for command_options_next.
 *
 * The caller has filled in the arguments, the letters, the long options and
 * whether -NUM is an option.
 */
void
command_options_start(
	struct command_options *scan)
{
	const char *posix;

	/* Nothing read yet; the operands gather from argv[1] on. */
	scan->index = 1;
	scan->cursor = NULL;
	scan->value = NULL;
	scan->number[0] = '\0';
	scan->operands = scan->argv + 1;
	scan->operand_count = 0;

	/* POSIXLY_CORRECT keeps the POSIX order: options come first. */
	posix = getenv("POSIXLY_CORRECT");
	scan->permute = 1;
	if (posix != NULL)
		scan->permute = 0;
}

/*
 * Reports the next option of a scan.
 *
 * Returns the option's letter or code, with its value in scan->value;
 * COMMAND_OPTION_NUMBER for -NUM; COMMAND_OPTION_ERROR after a message for
 * an option that is unknown, ambiguous or missing its value; and
 * COMMAND_OPTION_END when the options are over, when scan->operands holds
 * the operands.
 */
int
command_options_next(
	struct command_options *scan)
{
	char *argument;
	int differs;
	int code;

	/* The rest of a run of letters, such as the b of -ab. */
	scan->value = NULL;
	if (scan->cursor != NULL && *scan->cursor != '\0') {
		code = options_letter(scan);
		return code;
	}

	/* The run is over. */
	scan->cursor = NULL;

	/* Each argument until one is an option. */
	for (;;) {
		/* The end of the arguments ends the options. */
		if (scan->index >= scan->argc)
			return COMMAND_OPTION_END;
		argument = scan->argv[scan->index];

		/* -- ends the options; everything after it is an operand. */
		differs = strcmp(argument, "--");
		if (differs == 0) {
			scan->index++;
			options_rest(scan);
			return COMMAND_OPTION_END;
		}

		/* An option: -letters or --name. */
		if (argument[0] == '-' && argument[1] != '\0')
			break;

		/* An operand ends the options in the POSIX order. */
		if (!scan->permute) {
			options_rest(scan);
			return COMMAND_OPTION_END;
		}

		/* In the GNU order it is set aside and the scan goes on. */
		options_operand(scan, argument);
		scan->index++;
	}

	/* A long option. */
	scan->index++;
	if (argument[1] == '-') {
		code = options_long(scan, argument + 2);
		return code;
	}

	/* Succeeded: the first of a run of letters. */
	scan->cursor = argument + 1;
	code = options_letter(scan);
	return code;
}

/*
 * Writes an entire buffer to a descriptor.
 */
int
command_write_all(
	int descriptor,
	const void *data,
	size_t length)
{
	const unsigned char *bytes;
	ssize_t count;

	bytes = data;

	/* Write every byte, retrying interrupted system calls. */
	while (length != 0) {
		count = write(descriptor, bytes, length);

		/* Checks the remaining item count. */
		if (count < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;

			/* Reports operation failure. */
			return -1;
		}

		/* Checks the remaining item count. */
		if (count == 0) {
			errno = EIO;

			/* Reports operation failure. */
			return -1;
		}

		bytes += count;
		length -= (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Copies all available bytes between two descriptors.
 */
int
command_copy_fd(
	int input,
	int output)
{
	unsigned char buffer[4096];
	ssize_t count;
	int result;

	/* Copy chunks until the input reaches end of file. */
	for (;;) {
		count = read(input, buffer, sizeof(buffer));

		/* Checks the remaining item count. */
		if (count == 0)
			return 0;

		/* Checks the remaining item count. */
		if (count < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;

			/* Reports operation failure. */
			return -1;
		}

		result = command_write_all(output, buffer, (size_t)count);

		/* Checks the operation result. */
		if (result != 0)
			return -1;
	}
}

/*
 * Parses an unsigned decimal integer.
 */
int
command_parse_ull(
	const char *text,
	unsigned long long *result)
{
	char *end;
	unsigned long long value;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0' || *text == '-') {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	errno = 0;
	value = strtoull(text, &end, 10);

	/* Handles the reported system error. */
	if (errno != 0 || *end != '\0') {
		/* Handles the reported system error. */
		if (errno == 0)
			errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	*result = value;

	/* Reports successful completion. */
	return 0;
}

/*
 * Parses an unsigned octal file mode.
 */
int
command_parse_mode(
	const char *text,
	unsigned *result)
{
	char *end;
	unsigned long value;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0' || *text == '-') {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	errno = 0;
	value = strtoul(text, &end, 8);

	/* Handles the reported system error. */
	if (errno != 0 || *end != '\0' || value > 07777UL) {
		/* Handles the reported system error. */
		if (errno == 0)
			errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	*result = (unsigned)value;

	/* Reports successful completion. */
	return 0;
}

/*
 * Reports a command error using the saved errno value.
 */
void
command_error(
	const char *command,
	const char *operand)
{
	int saved;

	saved = errno;

	/* Handles the operand availability. */
	if (operand != NULL) {
		fprintf(
			stderr,
			"%s: %s: %s\n",
			command,
			operand,
			strerror(saved));
	} else {
		fprintf(stderr, "%s: %s\n", command, strerror(saved));
	}
}

/*
 * Reads one arbitrarily sized line from a stream.
 */
long
command_read_line(
	FILE *stream,
	char **line,
	size_t *capacity)
{
	size_t next;
	char *grown;
	size_t length;
	int c;

	length = 0;

	/* Handles the line availability. */
	if (*line == NULL || *capacity < 2) {
		*capacity = 128;
		*line = malloc(*capacity);
		/* Handles the line availability. */
		if (*line == NULL)
			return -1;
	}

	/* Collect characters through a newline or end of file. */
	for (;;) {
		c = fgetc(stream);

		/* Handles the end-of-file condition. */
		if (c == EOF)
			break;

		/* Checks the current data length. */
		if (length + 1 >= *capacity) {
			/* Handles the capacity condition. */
			if (*capacity > SIZE_MAX / 2)
				next = SIZE_MAX;
			else
				next = *capacity * 2;

			/* Handles the next condition. */
			if (next <= *capacity) {
				errno = EOVERFLOW;

				/* Reports operation failure. */
				return -1;
			}

			grown = realloc(*line, next);

			/* Handles the grown availability. */
			if (grown == NULL)
				return -1;

			*line = grown;
			*capacity = next;
		}

		(*line)[length++] = (char)c;

		/* Classifies the current input character. */
		if (c == '\n')
			break;
	}

	/* Handles the end-of-file condition. */
	if (length == 0 && c == EOF) {
		/* Handles an operation failure. */
		if (ferror(stream))
			return -1;

		/* Reports successful completion. */
		return 0;
	}

	(*line)[length] = '\0';

	/* Returns the computed result. */
	return (long)length;
}

/*
 * Executes a command using the current search path.
 */
int
command_exec(
	const char *name,
	char *const argv[])
{
	int function_result;
	char path[512];
	const char *search;
	const char *position;
	const char *end;
	size_t name_length;
	size_t directory_length;

	/* Handles a failed strchr operation. */
	if (strchr(name, '/') != NULL) {
		/* Obtains the execve result. */
		function_result = execve(name, argv, environ);

		/* Returns the computed result. */
		return function_result;
	}

	search = getenv("PATH");

	/* Handles the search availability. */
	if (search == NULL || *search == '\0')
		search = "/bin:/usr/bin";

	name_length = strlen(name);
	position = search;

	/* Try every search-path component in order. */
	for (;;) {
		end = strchr(position, ':');

		/* Handles the end availability. */
		if (end == NULL)
			directory_length = strlen(position);
		else
			directory_length = (size_t)(end - position);

		/* Handles the directory length condition. */
		if (directory_length + name_length + 2 < sizeof(path)) {
			/* Handles the directory length condition. */
			if (directory_length != 0) {
				memcpy(path, position, directory_length);
			} else {
				path[0] = '.';
				directory_length = 1;
			}

			path[directory_length] = '/';
			strcpy(path + directory_length + 1, name);
			execve(path, argv, environ);

			/* Handles the reported system error. */
			if (errno != ENOENT && errno != ENOTDIR)
				return -1;
		}

		/* Handles the end availability. */
		if (end == NULL)
			break;

		position = end + 1;
	}

	errno = ENOENT;

	/* Reports operation failure. */
	return -1;
}

/*
 * Reads a date the way GNU's date -d and touch -d take them.
 *
 * The forms are those scripts use: @SECONDS[.fraction]; a date written
 * YYYY-MM-DD or YYYYMMDD; a time hh:mm[:ss[.fraction]] after the date, a T
 * or blanks; a zone of Z, UTC, GMT or +hhmm/-hhmm; the words now, today,
 * yesterday and tomorrow; and relative items "[+|-]N unit [ago]" (second,
 * minute, hour, day, week, fortnight, month or year, with or without an s).
 * What is not given comes from now; a date without a time is midnight.  A
 * date without a zone is local time, or UTC when utc is set.  Returns 0
 * when the text is not such a date.
 */
int
command_parse_date(
	const char *text,
	const struct timespec *now,
	int utc,
	struct timespec *moment)
{
	struct command_date date;
	struct tm fields;
	time_t seconds;
	int valid;

	/* @SECONDS is the moment itself. */
	memset(&date, 0, sizeof(date));
	date.cursor = text;
	date_skip_blanks(&date);
	if (*date.cursor == '@') {
		date.cursor++;
		valid = date_epoch(&date, moment);
		return valid;
	}

	/* The items, in any order, each at most once. */
	for (;;) {
		date_skip_blanks(&date);
		if (*date.cursor == '\0')
			break;
		valid = date_item(&date);
		if (!valid)
			return 0;
	}

	/* The fields of now, in the zone the date is read in. */
	seconds = now->tv_sec;
	if (utc || date.have_zone)
		gmtime_r(&seconds, &fields);
	else
		localtime_r(&seconds, &fields);

	/* The date, and midnight when it has no time. */
	if (date.have_date) {
		fields.tm_year = date.year - 1900;
		fields.tm_mon = date.month - 1;
		fields.tm_mday = date.day;
		fields.tm_hour = 0;
		fields.tm_min = 0;
		fields.tm_sec = 0;
	}

	/* A named day at midnight, or a date, has no fraction of a second. */
	moment->tv_nsec = now->tv_nsec;
	if (date.have_date || date.have_time || date.midnight)
		moment->tv_nsec = 0;

	/* today: midnight. */
	if (date.midnight) {
		fields.tm_hour = 0;
		fields.tm_min = 0;
		fields.tm_sec = 0;
	}

	/* The time given. */
	if (date.have_time) {
		fields.tm_hour = date.hour;
		fields.tm_min = date.minute;
		fields.tm_sec = date.second;
		moment->tv_nsec = date.nanoseconds;
	}

	/* Months and years move the fields; the rest moves the seconds. */
	fields.tm_year += date.relative_years;
	fields.tm_mon += date.relative_months;
	fields.tm_mday += date.relative_days;
	fields.tm_isdst = -1;

	/* The fields as seconds: UTC, a zone's offset, or local time. */
	if (utc || date.have_zone)
		seconds = timegm(&fields);
	else
		seconds = mktime(&fields);
	if (date.have_zone)
		seconds -= date.zone_offset;

	/* Succeeded: the moment, with the relative seconds. */
	moment->tv_sec = seconds + date.relative_seconds;
	return 1;
}

/* Reads @SECONDS[.fraction]: the seconds since the epoch. */
static int
date_epoch(
	struct command_date *date,
	struct timespec *moment)
{
	long long value;
	int negative;
	int valid;

	/* The sign. */
	negative = 0;
	if (*date->cursor == '-' || *date->cursor == '+') {
		if (*date->cursor == '-')
			negative = 1;
		date->cursor++;
	}

	/* The seconds. */
	valid = date_number(date, &value, NULL);
	if (!valid)
		return 0;

	/* A fraction of a second. */
	moment->tv_nsec = 0;
	if (*date->cursor == '.' || *date->cursor == ',') {
		date->cursor++;
		moment->tv_nsec = date_fraction(date);
	}

	/* Only blanks may follow. */
	date_skip_blanks(date);
	if (*date->cursor != '\0')
		return 0;

	/* Succeeded. */
	moment->tv_sec = (time_t)value;
	if (negative)
		moment->tv_sec = -(time_t)value;
	return 1;
}

/* Reads one item of a date: a date, a time, a zone, a word or a move. */
static int
date_item(
	struct command_date *date)
{
	const char *start;
	long long value;
	size_t digits;
	int valid;

	/* T between a date and its time. */
	if (*date->cursor == 'T' && date->have_date && !date->have_time) {
		date->cursor++;
		valid = date_time(date);
		return valid;
	}

	/* A word: the named days, a zone, or a unit. */
	if ((*date->cursor >= 'a' && *date->cursor <= 'z') ||
	    (*date->cursor >= 'A' && *date->cursor <= 'Z')) {
		valid = date_word(date);
		return valid;
	}

	/* A signed number: a zone after a time, or a move. */
	if (*date->cursor == '+' || *date->cursor == '-') {
		valid = date_signed(date);
		return valid;
	}

	/* A number: a date, a time, or a move. */
	start = date->cursor;
	valid = date_number(date, &value, &digits);
	if (!valid)
		return 0;

	/* YYYY-MM-DD. */
	if (*date->cursor == '-' && !date->have_date) {
		date->cursor = start;
		valid = date_iso_date(date);
		return valid;
	}

	/* hh:mm. */
	if (*date->cursor == ':' && !date->have_time) {
		date->cursor = start;
		valid = date_time(date);
		return valid;
	}

	/* YYYYMMDD. */
	if (digits == 8U && !date->have_date) {
		date->year = (int)(value / 10000);
		date->month = (int)(value / 100 % 100);
		date->day = (int)(value % 100);
		date->have_date = 1;
		return 1;
	}

	/* Succeeded: a number before a unit. */
	valid = date_move(date, value);
	return valid;
}

/* Reads YYYY-MM-DD. */
static int
date_iso_date(
	struct command_date *date)
{
	long long year;
	long long month;
	long long day;
	int valid;

	/* The year, the month and the day, with a - between them. */
	valid = date_number(date, &year, NULL);
	if (!valid || *date->cursor != '-')
		return 0;
	date->cursor++;
	valid = date_number(date, &month, NULL);
	if (!valid || *date->cursor != '-')
		return 0;
	date->cursor++;
	valid = date_number(date, &day, NULL);
	if (!valid)
		return 0;

	/* A month and a day that exist. */
	if (month < 1 || month > 12 || day < 1 || day > 31)
		return 0;

	/* Succeeded. */
	date->year = (int)year;
	date->month = (int)month;
	date->day = (int)day;
	date->have_date = 1;
	return 1;
}

/* Reads hh:mm[:ss[.fraction]]. */
static int
date_time(
	struct command_date *date)
{
	long long hour;
	long long minute;
	long long second;
	int valid;

	/* The hour and the minute. */
	valid = date_number(date, &hour, NULL);
	if (!valid || *date->cursor != ':')
		return 0;
	date->cursor++;
	valid = date_number(date, &minute, NULL);
	if (!valid)
		return 0;

	/* The seconds, and a fraction of one. */
	second = 0;
	date->nanoseconds = 0;
	if (*date->cursor == ':') {
		date->cursor++;
		valid = date_number(date, &second, NULL);
		if (!valid)
			return 0;
		if (*date->cursor == '.' || *date->cursor == ',') {
			date->cursor++;
			date->nanoseconds = date_fraction(date);
		}
	}

	/* A time of the day. */
	if (hour > 24 || minute > 59 || second > 60)
		return 0;

	/* Succeeded. */
	date->hour = (int)hour;
	date->minute = (int)minute;
	date->second = (int)second;
	date->have_time = 1;
	return 1;
}

/*
 * Reads a number with a sign: a zone (+hhmm, +hh:mm) right after a time,
 * or else a move of so many units.
 */
static int
date_signed(
	struct command_date *date)
{
	long long value;
	long long hours;
	long long minutes;
	size_t digits;
	int negative;
	int valid;

	/* The sign and the number. */
	negative = 0;
	if (*date->cursor == '-')
		negative = 1;
	date->cursor++;
	valid = date_number(date, &value, &digits);
	if (!valid)
		return 0;

	/* A zone after a time: hhmm, hh, or hh:mm. */
	if (date->have_time && !date->have_zone) {
		hours = value;
		minutes = 0;
		if (digits == 4U) {
			hours = value / 100;
			minutes = value % 100;
		} else if (*date->cursor == ':') {
			date->cursor++;
			valid = date_number(date, &minutes, NULL);
			if (!valid)
				return 0;
		}

		/* The offset east of UTC, in seconds. */
		date->zone_offset = (long)(hours * 3600 + minutes * 60);
		if (negative)
			date->zone_offset = -date->zone_offset;
		date->have_zone = 1;
		return 1;
	}

	/* Succeeded: a move of so many units. */
	if (negative)
		value = -value;
	valid = date_move(date, value);
	return valid;
}

/* Reads the unit after a number of a move, and an ago after it. */
static int
date_move(
	struct command_date *date,
	long long count)
{
	static const struct {
		const char *name;
		long seconds;
		int kind;
	} units[] = {
		{"second", 1L, 0}, {"sec", 1L, 0},
		{"minute", 60L, 0}, {"min", 60L, 0},
		{"hour", 3600L, 0},
		{"day", 0L, 1},
		{"week", 0L, 2},
		{"fortnight", 0L, 3},
		{"month", 0L, 4},
		{"year", 0L, 5},
		{NULL, 0L, 0}
	};
	char word[16];
	size_t length;
	size_t index;
	int differs;

	/* The unit's word, without an s at its end. */
	date_skip_blanks(date);
	length = date_word_text(date, word, sizeof(word));
	if (length == 0)
		return 0;
	if (length > 1U && word[length - 1U] == 's')
		word[length - 1U] = '\0';

	/* ago turns the move back. */
	date_skip_blanks(date);
	differs = strncmp(date->cursor, "ago", 3);
	if (differs == 0) {
		date->cursor += 3;
		count = -count;
	}

	/* The unit. */
	for (index = 0; units[index].name != NULL; index++) {
		differs = strcmp(word, units[index].name);
		if (differs != 0)
			continue;

		/* The move, in seconds, days, months or years. */
		switch (units[index].kind) {
		case 0:
			date->relative_seconds += (time_t)(count * units[index].seconds);
			break;
		case 1:
			date->relative_days += (int)count;
			break;
		case 2:
			date->relative_days += (int)(count * 7);
			break;
		case 3:
			date->relative_days += (int)(count * 14);
			break;
		case 4:
			date->relative_months += (int)count;
			break;
		default:
			date->relative_years += (int)count;
			break;
		}

		/* Succeeded. */
		return 1;
	}

	/* A word that is no unit. */
	return 0;
}

/* Reads a word: the named days, a zone name, or a unit with a count of 1. */
static int
date_word(
	struct command_date *date)
{
	char word[16];
	size_t length;
	int differs;
	int valid;

	/* The word, in lower case. */
	length = date_word_text(date, word, sizeof(word));
	if (length == 0)
		return 0;

	/* now changes nothing. */
	differs = strcmp(word, "now");
	if (differs == 0)
		return 1;

	/* today is midnight. */
	differs = strcmp(word, "today");
	if (differs == 0) {
		date->midnight = 1;
		return 1;
	}

	/* yesterday is a day before, at this time. */
	differs = strcmp(word, "yesterday");
	if (differs == 0) {
		date->relative_days -= 1;
		return 1;
	}

	/* tomorrow is a day after. */
	differs = strcmp(word, "tomorrow");
	if (differs == 0) {
		date->relative_days += 1;
		return 1;
	}

	/* Z, UTC and GMT are the zone of offset 0. */
	differs = strcmp(word, "z");
	if (differs != 0)
		differs = strcmp(word, "utc");
	if (differs != 0)
		differs = strcmp(word, "gmt");
	if (differs == 0) {
		date->zone_offset = 0;
		date->have_zone = 1;
		return 1;
	}

	/* Succeeded: a unit alone, as in "next day", counts 1. */
	date->cursor -= length;
	valid = date_move(date, 1);
	return valid;
}

/* Copies the letters at the cursor, in lower case.  Returns how many. */
static size_t
date_word_text(
	struct command_date *date,
	char *word,
	size_t size)
{
	size_t length;
	char value;

	/* Each letter, while there is room. */
	length = 0;
	for (;;) {
		value = *date->cursor;
		if (value >= 'A' && value <= 'Z')
			value = (char)(value - 'A' + 'a');
		if (value < 'a' || value > 'z')
			break;
		if (length + 1U >= size)
			return 0;
		word[length] = value;
		length++;
		date->cursor++;
	}

	/* Succeeded: the word, ended. */
	word[length] = '\0';
	return length;
}

/* Reads decimal digits.  Returns 0 when there is none. */
static int
date_number(
	struct command_date *date,
	long long *value,
	size_t *digits)
{
	size_t count;

	/* Each digit. */
	*value = 0;
	count = 0;
	while (*date->cursor >= '0' && *date->cursor <= '9') {
		*value = *value * 10 + (long long)(*date->cursor - '0');
		date->cursor++;
		count++;
	}

	/* How many there were. */
	if (digits != NULL)
		*digits = count;
	if (count == 0)
		return 0;

	/* Succeeded. */
	return 1;
}

/* Reads the digits of a fraction of a second, as nanoseconds. */
static long
date_fraction(
	struct command_date *date)
{
	long nanoseconds;
	long scale;

	/* Each digit, the first nine counting. */
	nanoseconds = 0;
	scale = 100000000L;
	while (*date->cursor >= '0' && *date->cursor <= '9') {
		nanoseconds += (long)(*date->cursor - '0') * scale;
		scale /= 10;
		date->cursor++;
	}

	/* Succeeded. */
	return nanoseconds;
}

/* Skips blanks and commas between the items of a date. */
static void
date_skip_blanks(
	struct command_date *date)
{
	/* Each blank. */
	while (*date->cursor == ' ' || *date->cursor == '\t' ||
	       *date->cursor == ',')
		date->cursor++;
}

/* Reads one letter of a run, and its value when it takes one. */
static int
options_letter(
	struct command_options *scan)
{
	const char *found;
	size_t length;
	int letter;

	/* The letter, and the run after it. */
	letter = (unsigned char)*scan->cursor;
	scan->cursor++;

	/* -NUM: the digits that follow make up the number. */
	if (scan->numbers && letter >= '0' && letter <= '9') {
		scan->number[0] = (char)letter;
		length = 1;
		while (*scan->cursor >= '0' && *scan->cursor <= '9' &&
		       length + 1U < sizeof(scan->number)) {
			scan->number[length] = *scan->cursor;
			length++;
			scan->cursor++;
		}

		/* Succeeded: the number as the value. */
		scan->number[length] = '\0';
		scan->value = scan->number;
		return COMMAND_OPTION_NUMBER;
	}

	/* A letter the program does not take. */
	found = NULL;
	if (letter != ':')
		found = strchr(scan->letters, letter);
	if (found == NULL) {
		fprintf(stderr, "%s: invalid option -- '%c'\n", scan->program,
			letter);
		scan->cursor = NULL;
		return COMMAND_OPTION_ERROR;
	}

	/* A letter without a value. */
	if (found[1] != ':')
		return letter;

	/* A value attached to the letter, as in -escript or -i.bak. */
	if (*scan->cursor != '\0') {
		scan->value = scan->cursor;
		scan->cursor = NULL;
		return letter;
	}

	/* An optional value is only ever attached. */
	scan->cursor = NULL;
	if (found[2] == ':')
		return letter;

	/* A required value is the next argument, which must exist. */
	if (scan->index >= scan->argc) {
		fprintf(stderr, "%s: option requires an argument -- '%c'\n",
			scan->program, letter);
		return COMMAND_OPTION_ERROR;
	}

	/* Succeeded: the next argument is the value. */
	scan->value = scan->argv[scan->index];
	scan->index++;
	return letter;
}

/* Reads a long option (the text after --) and its value. */
static int
options_long(
	struct command_options *scan,
	const char *text)
{
	const struct command_long_option *option;
	const char *equals;
	size_t length;
	int found;

	/* The name, up to an = that starts the value. */
	equals = strchr(text, '=');
	length = strlen(text);
	if (equals != NULL)
		length = (size_t)(equals - text);

	/* The option the name, or a unique beginning of it, stands for. */
	found = options_find_long(scan, text, length);
	if (found < 0)
		return COMMAND_OPTION_ERROR;
	option = &scan->names[found];

	/* An option without a value refuses one. */
	if (option->value == COMMAND_VALUE_NONE) {
		if (equals != NULL) {
			fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
				scan->program, option->name);
			return COMMAND_OPTION_ERROR;
		}

		/* Succeeded: the option alone. */
		return option->code;
	}

	/* A value after the =. */
	if (equals != NULL) {
		scan->value = equals + 1;
		return option->code;
	}

	/* An optional value is only ever written after an =. */
	if (option->value == COMMAND_VALUE_OPTIONAL)
		return option->code;

	/* A required value is the next argument, which must exist. */
	if (scan->index >= scan->argc) {
		fprintf(stderr, "%s: option '--%s' requires an argument\n",
			scan->program, option->name);
		return COMMAND_OPTION_ERROR;
	}

	/* Succeeded: the next argument is the value. */
	scan->value = scan->argv[scan->index];
	scan->index++;
	return option->code;
}

/*
 * Finds the long option a name selects: the one of that name, or else the
 * only one it begins (options that differ only in name, such as --color
 * and --colour, count as one).  Returns -1 after a message.
 */
static int
options_find_long(
	const struct command_options *scan,
	const char *name,
	size_t length)
{
	const struct command_long_option *option;
	const struct command_long_option *first;
	int candidate;
	int ambiguous;
	int differs;
	int index;

	/* Each option the name begins. */
	candidate = -1;
	ambiguous = 0;
	for (index = 0; scan->names != NULL && scan->names[index].name != NULL;
	     index++) {
		/* An option the name does not begin. */
		option = &scan->names[index];
		differs = strncmp(option->name, name, length);
		if (differs != 0)
			continue;

		/* The name in full is the option whatever else it begins. */
		if (option->name[length] == '\0')
			return index;

		/* The first option it begins. */
		if (candidate < 0) {
			candidate = index;
			continue;
		}

		/* A second one that is really another option. */
		first = &scan->names[candidate];
		if (first->code != option->code || first->value != option->value)
			ambiguous = 1;
	}

	/* A beginning of two different options chooses neither. */
	if (ambiguous) {
		fprintf(stderr, "%s: option '--%.*s' is ambiguous\n",
			scan->program, (int)length, name);
		return -1;
	}

	/* A name that begins no option. */
	if (candidate < 0) {
		fprintf(stderr, "%s: unrecognized option '--%.*s'\n",
			scan->program, (int)length, name);
		return -1;
	}

	/* Succeeded: the only option the name begins. */
	return candidate;
}

/*
 * Sets an operand aside.  Every argument before scan->index has been read,
 * so the slot it goes into is free.
 */
static void
options_operand(
	struct command_options *scan,
	char *argument)
{
	/* The next operand slot. */
	scan->argv[1 + scan->operand_count] = argument;
	scan->operand_count++;
}

/* Sets every argument that is left aside as an operand. */
static void
options_rest(
	struct command_options *scan)
{
	/* Each argument in order. */
	while (scan->index < scan->argc) {
		options_operand(scan, scan->argv[scan->index]);
		scan->index++;
	}
}
