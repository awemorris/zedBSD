/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * When a photo was taken (ws157-p002): a JPEG's APP1 block holds EXIF, a
 * TIFF header and its directories.  The first directory (IFD0) may point
 * to the EXIF directory, whose DateTimeOriginal (0x9003) is when the
 * picture was taken; without it, IFD0's DateTime (0x0132) is when it was
 * last changed.  Both are "YYYY:MM:DD HH:MM:SS" in the camera's own time.
 *
 * The dates are kept as seconds counted as if the calendar's time were
 * UTC (ph_time), so the days are as written, whatever the zone.
 */

#include "photos.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* The tags read: the EXIF directory's pointer, the dates. */
#define EXIF_TAG_POINTER	0x8769U
#define EXIF_TAG_ORIGINAL	0x9003U
#define EXIF_TAG_DATE		0x0132U

/* The TIFF types: ASCII, LONG. */
#define EXIF_TYPE_ASCII		2U
#define EXIF_TYPE_LONG		4U

/* The bytes of a date: "YYYY:MM:DD HH:MM:SS" and its NUL. */
#define EXIF_DATE_SIZE		20U

/* How much of a file is read for its EXIF (APP1 is at most 64 KiB, near the start). */
#define EXIF_READ		(128U * 1024U)

/* The JPEG markers: start of image, APP1, start of scan; the seconds of a day. */
#define EXIF_SOI		0xd8U
#define EXIF_APP1		0xe1U
#define EXIF_SOS		0xdaU
#define EXIF_DAY		86400

/*
 * The TIFF data of an EXIF block: its bytes from the header, their count,
 * and the byte order.
 */
struct exif_tiff {
	const unsigned char *data;
	size_t size;
	int big_endian;
};

static int exif_entry(const struct exif_tiff *tiff, unsigned long directory, unsigned tag, unsigned *type, unsigned long *count, unsigned long *value);
static int exif_parse_date(const struct exif_tiff *tiff, unsigned long count, unsigned long offset, ph_time *taken);
static int exif_digits(const unsigned char *text, size_t count, int *number);
static unsigned exif_read16(const unsigned char *data, int big_endian);
static unsigned long exif_read32(const unsigned char *data, int big_endian);
static int64_t exif_days(int year, int month, int day);

/*
 * Reads when the picture was taken from an APP1 block (starting at its
 * "Exif" header).  Returns 0 with the time, or ENOENT when the block has
 * no date it can read.
 */
int
ph_exif_date(
	const unsigned char *data,
	size_t size,
	ph_time *taken)
{
	struct exif_tiff tiff;
	unsigned long directory;
	unsigned long count;
	unsigned long value;
	unsigned magic;
	unsigned type;
	int found;
	int status;

	/* "Exif", two zeros, then a TIFF header. */
	if (size < 14U)
		return ENOENT;
	status = memcmp(data, "Exif\0\0", 6U);
	if (status != 0)
		return ENOENT;
	tiff.data = data + 6;
	tiff.size = size - 6U;

	/* The byte order, then 42. */
	if (tiff.data[0] == 'I' && tiff.data[1] == 'I')
		tiff.big_endian = 0;
	else if (tiff.data[0] == 'M' && tiff.data[1] == 'M')
		tiff.big_endian = 1;
	else
		return ENOENT;
	magic = exif_read16(tiff.data + 2, tiff.big_endian);
	if (magic != 42U)
		return ENOENT;
	directory = exif_read32(tiff.data + 4, tiff.big_endian);

	/* The EXIF directory's DateTimeOriginal. */
	found = exif_entry(&tiff, directory, EXIF_TAG_POINTER, &type, &count, &value);
	if (found && type == EXIF_TYPE_LONG) {
		found = exif_entry(&tiff, value, EXIF_TAG_ORIGINAL, &type, &count, &value);
		if (found && type == EXIF_TYPE_ASCII) {
			status = exif_parse_date(&tiff, count, value, taken);
			if (status == 0)
				return 0;
		}
	}

	/* Else IFD0's DateTime. */
	found = exif_entry(&tiff, directory, EXIF_TAG_DATE, &type, &count, &value);
	if (found && type == EXIF_TYPE_ASCII)
		return exif_parse_date(&tiff, count, value, taken);

	/* No date. */
	return ENOENT;
}

/*
 * Reads when a JPEG file's picture was taken: its markers are walked to
 * the APP1 block that holds EXIF.  Returns 0 with the time, ENOENT when
 * the file has none, or an errno value of the file's.
 */
int
ph_exif_file_date(
	const char *path,
	ph_time *taken)
{
	int descriptor;
	int error;

	/* Opens one source and uses the descriptor parser shared by received media. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;
	error = ph_exif_descriptor_date(descriptor, taken);
	close(descriptor);
	if (error != 0)
		return error;

	/* Succeeded: the original photo's EXIF date is available. */
	return 0;
}

/*
 * Reads a JPEG's EXIF date from an independent descriptor without moving its offset.
 */
int
ph_exif_descriptor_date(
	int descriptor,
	ph_time *taken)
{
	static unsigned char data[EXIF_READ];
	ssize_t got;
	size_t size;
	size_t at;
	size_t length;
	int status;

	/* The start of the file. */
	got = pread(descriptor, data, sizeof(data), 0);
	if (got < 4)
		return ENOENT;
	size = (size_t)got;

	/* A JPEG starts with SOI. */
	if (data[0] != 0xffU || data[1] != EXIF_SOI)
		return ENOENT;

	/* Each marker with its length, up to the scan. */
	at = 2;
	while (at + 4U <= size) {
		if (data[at] != 0xffU)
			return ENOENT;
		if (data[at + 1U] == EXIF_SOS)
			return ENOENT;
		length = ((size_t)data[at + 2U] << 8) | data[at + 3U];
		if (length < 2U || at + 2U + length > size)
			return ENOENT;

		/* APP1 with "Exif". */
		if (data[at + 1U] == EXIF_APP1) {
			status = ph_exif_date(data + at + 4U, length - 2U, taken);
			if (status == 0)
				return 0;
		}

		/* The next marker. */
		at += 2U + length;
	}

	/* No EXIF in the part read. */
	return ENOENT;
}

/*
 * Makes a time of a calendar's date and time (counted as if UTC).
 */
ph_time
ph_time_make(
	int year,
	int month,
	int day,
	int hour,
	int minute,
	int second)
{
	int64_t days;

	/* The days, then the seconds of the day. */
	days = exif_days(year, month, day);
	return days * EXIF_DAY + (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
}

/*
 * Splits a time into its year, month (1 to 12) and day.
 */
void
ph_time_split(
	ph_time when,
	int *year,
	int *month,
	int *day)
{
	int64_t days;
	int64_t era;
	int64_t of_era;
	int64_t year_of_era;
	int64_t day_of_year;
	int64_t shifted;
	int64_t civil;

	/* The days since 1970, whole even before it. */
	days = when / EXIF_DAY;
	if (when % EXIF_DAY < 0)
		days--;

	/* The civil date of a day count (eras of 400 years from March 1st, year 0). */
	days += 719468;
	era = days / 146097;
	if (days < 0 && days % 146097 != 0)
		era--;
	of_era = days - era * 146097;
	year_of_era = (of_era - of_era / 1460 + of_era / 36524 - of_era / 146096) / 365;
	day_of_year = of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	shifted = (5 * day_of_year + 2) / 153;
	*day = (int)(day_of_year - (153 * shifted + 2) / 5 + 1);
	civil = shifted + 3;
	if (shifted >= 10)
		civil = shifted - 9;
	*month = (int)civil;
	*year = (int)(year_of_era + era * 400);
	if (civil <= 2)
		*year += 1;
}

/*
 * Finds a tag in a directory: 1 with its type, count and value (or the
 * offset of its value), 0 when the directory does not hold it or cannot
 * be read.
 */
static int
exif_entry(
	const struct exif_tiff *tiff,
	unsigned long directory,
	unsigned tag,
	unsigned *type,
	unsigned long *count,
	unsigned long *value)
{
	unsigned long offset;
	unsigned entries;
	unsigned index;
	unsigned found;

	/* The directory's count. */
	if (tiff->size < 2U || directory > tiff->size - 2U)
		return 0;
	entries = exif_read16(tiff->data + directory, tiff->big_endian);

	/* Each entry, twelve bytes. */
	for (index = 0; index < entries; index++) {
		offset = directory + 2U + (unsigned long)index * 12U;
		if (tiff->size < 12U || offset > tiff->size - 12U)
			return 0;
		found = exif_read16(tiff->data + offset, tiff->big_endian);
		if (found != tag)
			continue;

		/* The tag: its type, count and value. */
		*type = exif_read16(tiff->data + offset + 2U, tiff->big_endian);
		*count = exif_read32(tiff->data + offset + 4U, tiff->big_endian);
		*value = exif_read32(tiff->data + offset + 8U, tiff->big_endian);
		return 1;
	}

	/* Not there. */
	return 0;
}

/* Reads "YYYY:MM:DD HH:MM:SS" at an offset; 0 with the time, or ENOENT. */
static int
exif_parse_date(
	const struct exif_tiff *tiff,
	unsigned long count,
	unsigned long offset,
	ph_time *taken)
{
	const unsigned char *text;
	int fields[6];
	int status;
	int index;
	static const unsigned char starts[6] = { 0, 5, 8, 11, 14, 17 };
	static const unsigned char widths[6] = { 4, 2, 2, 2, 2, 2 };

	/* Twenty bytes (more than four: stored at the offset). */
	if (count < EXIF_DATE_SIZE - 1U || tiff->size < EXIF_DATE_SIZE || offset > tiff->size - (EXIF_DATE_SIZE - 1U))
		return ENOENT;
	text = tiff->data + offset;

	/* Each number. */
	for (index = 0; index < 6; index++) {
		status = exif_digits(text + starts[index], widths[index], &fields[index]);
		if (status != 0)
			return ENOENT;
	}

	/* A date a calendar has (a camera without a clock writes zeros). */
	if (fields[0] < 1900 || fields[1] < 1 || fields[1] > 12 || fields[2] < 1 || fields[2] > 31 || fields[3] > 23 || fields[4] > 59 || fields[5] > 60)
		return ENOENT;
	*taken = ph_time_make(fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]);
	return 0;
}

/* Reads a number of decimal digits; 0, or ENOENT for a byte that is not one. */
static int
exif_digits(
	const unsigned char *text,
	size_t count,
	int *number)
{
	size_t index;

	/* Each digit. */
	*number = 0;
	for (index = 0; index < count; index++) {
		if (text[index] < '0' || text[index] > '9')
			return ENOENT;
		*number = *number * 10 + (text[index] - '0');
	}

	/* The number. */
	return 0;
}

/* Reads a 16-bit number in a byte order. */
static unsigned
exif_read16(
	const unsigned char *data,
	int big_endian)
{
	/* The most significant byte first, or last. */
	if (big_endian)
		return ((unsigned)data[0] << 8) | (unsigned)data[1];
	return ((unsigned)data[1] << 8) | (unsigned)data[0];
}

/* Reads a 32-bit number in a byte order. */
static unsigned long
exif_read32(
	const unsigned char *data,
	int big_endian)
{
	/* The most significant byte first, or last. */
	if (big_endian)
		return ((unsigned long)data[0] << 24) | ((unsigned long)data[1] << 16) | ((unsigned long)data[2] << 8) | (unsigned long)data[3];
	return ((unsigned long)data[3] << 24) | ((unsigned long)data[2] << 16) | ((unsigned long)data[1] << 8) | (unsigned long)data[0];
}

/* Counts the days from 1970-01-01 to a date (eras of 400 years from March 1st, year 0). */
static int64_t
exif_days(
	int year,
	int month,
	int day)
{
	int64_t era;
	int64_t of_era;
	int64_t day_of_year;
	int64_t shifted;

	/* January and February count with the year before. */
	if (month <= 2)
		year--;
	era = year / 400;
	if (year < 0 && year % 400 != 0)
		era--;
	of_era = year - era * 400;

	/* The day in the year from March, then in the era. */
	shifted = month - 3;
	if (month <= 2)
		shifted = month + 9;
	day_of_year = (153 * shifted + 2) / 5 + day - 1;
	return era * 146097 + (of_era * 365 + of_era / 4 - of_era / 100 + day_of_year) - 719468;
}
