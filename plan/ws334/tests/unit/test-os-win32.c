/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the Windows helpers of the operating-system layer
 * (base/os-win32.c): the error tables, the UTF-8 and UTF-16 conversions and
 * the path conversion.  It runs on Windows only.
 *
 * The expected values come from the tables of base/os-win32.h, from the
 * Unicode Standard's definition of UTF-8 and UTF-16 (worked by hand or by
 * the reference decoder below, which is written differently from the
 * module), and from the Windows API.
 */

#include "check.h"

#include "base/os-win32.h"

#include <windows.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/*
 * One row of the error tables: a code of the system and the errno value
 * the layer maps it to.
 */
struct code_row {
	unsigned long code;
	int expected;
};

/*
 * The Windows error codes of the table in base/os-win32.h, with the numbers
 * written out (the values of <winerror.h>).
 */
static const struct code_row system_rows[] = {
	{ 0UL, 0 },
	{ 2UL, ENOENT }, { 3UL, ENOENT }, { 15UL, ENOENT }, { 53UL, ENOENT }, { 67UL, ENOENT }, { 126UL, ENOENT },
	{ 5UL, EACCES }, { 19UL, EACCES },
	{ 32UL, EBUSY }, { 33UL, EBUSY },
	{ 8UL, ENOMEM }, { 14UL, ENOMEM },
	{ 39UL, ENOSPC }, { 112UL, ENOSPC },
	{ 50UL, ENOTSUP }, { 120UL, ENOTSUP }, { 127UL, ENOTSUP },
	{ 80UL, EEXIST }, { 183UL, EEXIST },
	{ 87UL, EINVAL }, { 123UL, EINVAL }, { 131UL, EINVAL }, { 161UL, EINVAL },
	{ 111UL, ENAMETOOLONG }, { 206UL, ENAMETOOLONG },
	{ 122UL, ERANGE }, { 234UL, ERANGE },
	{ 145UL, ENOTEMPTY },
	{ 267UL, ENOTDIR },
	{ 258UL, ETIMEDOUT }, { 1460UL, ETIMEDOUT },
	{ 1113UL, EILSEQ }
};

/*
 * The Windows Sockets error codes of the table in base/os-win32.h, with the
 * numbers written out (the values of <winsock2.h>).
 */
static const struct code_row socket_rows[] = {
	{ 0UL, 0 },
	{ 10004UL, EINTR },
	{ 10013UL, EACCES },
	{ 10014UL, EINVAL }, { 10022UL, EINVAL }, { 10093UL, EINVAL },
	{ 10024UL, EMFILE },
	{ 10035UL, EAGAIN },
	{ 10036UL, EINPROGRESS },
	{ 10037UL, EALREADY },
	{ 10038UL, ENOTSOCK },
	{ 10040UL, EMSGSIZE },
	{ 10043UL, EAFNOSUPPORT }, { 10047UL, EAFNOSUPPORT },
	{ 10045UL, ENOTSUP },
	{ 10048UL, EADDRINUSE },
	{ 10049UL, EADDRNOTAVAIL },
	{ 10050UL, ENETDOWN },
	{ 10051UL, ENETUNREACH },
	{ 10053UL, ECONNABORTED },
	{ 10052UL, ECONNRESET }, { 10054UL, ECONNRESET },
	{ 10055UL, ENOBUFS },
	{ 10056UL, EISCONN },
	{ 10057UL, ENOTCONN },
	{ 10058UL, EPIPE },
	{ 10060UL, ETIMEDOUT },
	{ 10061UL, ECONNREFUSED },
	{ 10064UL, EHOSTUNREACH }, { 10065UL, EHOSTUNREACH },
	{ 8UL, ENOMEM }
};

/* A place the failure checks fill the outputs with, to see the function overwrite them. */
static wchar_t sentinel_wide[2];
static char sentinel_text[2];

/* Looks a code up in a table; a code that is not in it maps to EIO. */
static int
expected_for(
	const struct code_row *rows,
	size_t count,
	unsigned long code)
{
	size_t i;

	/* Searches the rows for the code. */
	for (i = 0; i < count; i++) {
		if (rows[i].code == code)
			return rows[i].expected;
	}

	/* Not in the table. */
	return EIO;
}

/* Checks wb_os_error_from_system against its table, for the table and for every code around it. */
static void
test_error_from_system(
	void)
{
	unsigned long code;
	size_t count;
	size_t i;

	count = sizeof(system_rows) / sizeof(system_rows[0]);

	/* Every code of the table maps to the value written for it. */
	for (i = 0; i < count; i++)
		CHECK_EQ(wb_os_error_from_system(system_rows[i].code), system_rows[i].expected);

	/* No code outside the table maps to anything but EIO, over the whole range the table lives in. */
	for (code = 0; code <= 2000UL; code++)
		CHECK_EQ(wb_os_error_from_system(code), expected_for(system_rows, count, code));

	/* The codes named by the task, and the largest value. */
	CHECK_EQ(wb_os_error_from_system(1UL), EIO);
	CHECK_EQ(wb_os_error_from_system(999UL), EIO);
	CHECK_EQ(wb_os_error_from_system(0xffffffffUL), EIO);
	CHECK_EQ(wb_os_error_from_system(0x80070005UL), EIO);
}

/* Checks wb_os_error_from_socket against its table, for the table and for every code around it. */
static void
test_error_from_socket(
	void)
{
	int code;
	size_t count;
	size_t i;

	count = sizeof(socket_rows) / sizeof(socket_rows[0]);

	/* Every code of the table maps to the value written for it. */
	for (i = 0; i < count; i++)
		CHECK_EQ(wb_os_error_from_socket((int)socket_rows[i].code), socket_rows[i].expected);

	/* No code outside the table maps to anything but EIO, over the sockets range and the small codes. */
	for (code = -300; code <= 300; code++)
		CHECK_EQ(wb_os_error_from_socket(code), expected_for(socket_rows, count, (unsigned long)code));
	for (code = 9990; code <= 10200; code++)
		CHECK_EQ(wb_os_error_from_socket(code), expected_for(socket_rows, count, (unsigned long)code));

	/* The codes named by the task. */
	CHECK_EQ(wb_os_error_from_socket(10000), EIO);
	CHECK_EQ(wb_os_error_from_socket(-1), EIO);
}

/* Converts text and checks the UTF-16 units against the expected ones. */
static void
expect_wide(
	const char *text,
	const unsigned short *units,
	size_t count)
{
	wchar_t *wide;
	size_t length;
	size_t i;
	int error;
	int same;

	wide = sentinel_wide;
	length = 99;
	error = wb_os_wide_from_utf8(text, &wide, &length);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;

	/* The length counts the units before the NUL. */
	CHECK_EQ(length, count);

	/* The units are the expected ones and a NUL follows them. */
	same = 1;
	for (i = 0; i < count; i++) {
		if ((unsigned short)wide[i] != units[i])
			same = 0;
	}
	CHECK(same);
	CHECK_EQ(wide[count], 0);
	free(wide);
}

/* Converts text that is not valid and checks the error and the failure state of the outputs. */
static void
expect_wide_error(
	const char *text,
	int expected)
{
	wchar_t *wide;
	size_t length;
	int error;

	wide = sentinel_wide;
	length = 99;
	error = wb_os_wide_from_utf8(text, &wide, &length);
	CHECK_EQ(error, expected);
	CHECK(wide == NULL);
	CHECK_EQ(length, 0);
}

/* The reference decoder: tells whether bytes are one well-formed UTF-8 sequence and gives its value. */
static int
reference_valid(
	const unsigned char *bytes,
	size_t count,
	unsigned long *value)
{
	unsigned long code;
	unsigned long minimum;
	size_t need;
	size_t i;

	/* The lead byte gives the length, the first bits and the smallest value that needs that length. */
	if (bytes[0] >= 0xc0 && bytes[0] <= 0xdf) {
		need = 2;
		code = bytes[0] & 0x1fUL;
		minimum = 0x80UL;
	} else if (bytes[0] >= 0xe0 && bytes[0] <= 0xef) {
		need = 3;
		code = bytes[0] & 0x0fUL;
		minimum = 0x800UL;
	} else if (bytes[0] >= 0xf0 && bytes[0] <= 0xf7) {
		need = 4;
		code = bytes[0] & 0x07UL;
		minimum = 0x10000UL;
	} else {
		return 0;
	}
	if (count != need)
		return 0;

	/* Every other byte is a continuation byte, 10xxxxxx. */
	for (i = 1; i < need; i++) {
		if ((bytes[i] & 0xc0) != 0x80)
			return 0;
		code = (code << 6) | (bytes[i] & 0x3fUL);
	}

	/* The value must need that many bytes, be no more than U+10FFFF and not be a surrogate. */
	if (code < minimum || code > 0x10ffffUL)
		return 0;
	if (code >= 0xd800UL && code <= 0xdfffUL)
		return 0;

	*value = code;
	return 1;
}

/* Checks a converted sequence of the exhaustive sweeps against the reference decoder. */
static void
sweep_one(
	const unsigned char *bytes,
	size_t count,
	unsigned *failures)
{
	char text[8];
	wchar_t *wide;
	size_t length;
	unsigned long value;
	int valid;
	int error;

	memcpy(text, bytes, count);
	text[count] = '\0';
	valid = reference_valid(bytes, count, &value);
	wide = sentinel_wide;
	error = wb_os_wide_from_utf8(text, &wide, &length);

	/* A sequence the reference accepts converts to the units of its value, and one it rejects is EILSEQ. */
	if (valid) {
		if (error != 0) {
			(*failures)++;
			return;
		}
		if (value < 0x10000UL) {
			if (length != 1 || (unsigned short)wide[0] != (unsigned short)value)
				(*failures)++;
		} else {
			if (length != 2 || (unsigned short)wide[0] != (unsigned short)(0xd800UL + ((value - 0x10000UL) >> 10)) ||
			    (unsigned short)wide[1] != (unsigned short)(0xdc00UL + ((value - 0x10000UL) & 0x3ffUL)))
				(*failures)++;
		}
		free(wide);
	} else {
		if (error != EILSEQ || wide != NULL || length != 0)
			(*failures)++;
	}
}

/* Sweeps the two-, three- and four-byte sequences and every truncation against the reference decoder. */
static void
test_utf8_sweeps(
	void)
{
	static const unsigned char interesting[] = { 0x01, 0x7f, 0x80, 0x9f, 0xa0, 0xbf, 0xc0, 0x8f, 0x90, 0xff };
	unsigned char bytes[4];
	unsigned failures;
	unsigned lead;
	unsigned second;
	unsigned third;
	size_t a;
	size_t b;

	/* Every two-byte shape: each lead from 0x80 up, each second byte except NUL. */
	failures = 0;
	for (lead = 0xc0; lead <= 0xff; lead++) {
		for (second = 1; second <= 0xff; second++) {
			bytes[0] = (unsigned char)lead;
			bytes[1] = (unsigned char)second;
			if (lead <= 0xdf)
				sweep_one(bytes, 2, &failures);
		}
	}
	CHECK_EQ(failures, 0);

	/* Every three-byte shape: leads E0 to EF with every second and third byte except NUL. */
	failures = 0;
	for (lead = 0xe0; lead <= 0xef; lead++) {
		for (second = 1; second <= 0xff; second++) {
			for (third = 1; third <= 0xff; third++) {
				bytes[0] = (unsigned char)lead;
				bytes[1] = (unsigned char)second;
				bytes[2] = (unsigned char)third;
				sweep_one(bytes, 3, &failures);
			}
		}
	}
	CHECK_EQ(failures, 0);

	/* Every four-byte shape: leads F0 to F7, every second byte, and the edge values for the others. */
	failures = 0;
	for (lead = 0xf0; lead <= 0xf7; lead++) {
		for (second = 1; second <= 0xff; second++) {
			for (a = 0; a < sizeof(interesting); a++) {
				for (b = 0; b < sizeof(interesting); b++) {
					bytes[0] = (unsigned char)lead;
					bytes[1] = (unsigned char)second;
					bytes[2] = interesting[a];
					bytes[3] = interesting[b];
					sweep_one(bytes, 4, &failures);
				}
			}
		}
	}
	CHECK_EQ(failures, 0);

	/* Every lead byte alone, and every lead with only valid continuation bytes up to a cut, then the NUL. */
	failures = 0;
	for (lead = 0x80; lead <= 0xff; lead++) {
		char text[4];
		wchar_t *wide;
		size_t length;
		int error;

		text[0] = (char)lead;
		text[1] = '\0';
		wide = sentinel_wide;
		error = wb_os_wide_from_utf8(text, &wide, &length);
		if (error != EILSEQ || wide != NULL || length != 0)
			failures++;

		text[1] = (char)0x80;
		text[2] = '\0';
		if (lead >= 0xe0 && lead <= 0xf4 && lead != 0xe0 && lead != 0xf0 && lead != 0xed && lead != 0xf4) {
			wide = sentinel_wide;
			error = wb_os_wide_from_utf8(text, &wide, &length);
			if (error != EILSEQ || wide != NULL || length != 0)
				failures++;
		}
	}
	CHECK_EQ(failures, 0);
}

/* Converts every code point to UTF-16 and back, and compares with the definition of UTF-16. */
static void
test_utf8_round_trip(
	void)
{
	unsigned long code;
	unsigned failures;
	unsigned char encoded[5];
	size_t size;
	wchar_t *wide;
	char *back;
	size_t length;
	size_t back_length;
	int error;
	size_t expected_units;

	failures = 0;
	for (code = 1; code <= 0x10ffffUL; code++) {
		if (code >= 0xd800UL && code <= 0xdfffUL)
			continue;

		/* The canonical UTF-8 of the code point, worked by hand. */
		if (code < 0x80UL) {
			encoded[0] = (unsigned char)code;
			size = 1;
		} else if (code < 0x800UL) {
			encoded[0] = (unsigned char)(0xc0UL | (code >> 6));
			encoded[1] = (unsigned char)(0x80UL | (code & 0x3fUL));
			size = 2;
		} else if (code < 0x10000UL) {
			encoded[0] = (unsigned char)(0xe0UL | (code >> 12));
			encoded[1] = (unsigned char)(0x80UL | ((code >> 6) & 0x3fUL));
			encoded[2] = (unsigned char)(0x80UL | (code & 0x3fUL));
			size = 3;
		} else {
			encoded[0] = (unsigned char)(0xf0UL | (code >> 18));
			encoded[1] = (unsigned char)(0x80UL | ((code >> 12) & 0x3fUL));
			encoded[2] = (unsigned char)(0x80UL | ((code >> 6) & 0x3fUL));
			encoded[3] = (unsigned char)(0x80UL | (code & 0x3fUL));
			size = 4;
		}
		encoded[size] = 0;
		expected_units = code >= 0x10000UL ? 2 : 1;

		/* To UTF-16: one unit, or a surrogate pair from the definition. */
		wide = NULL;
		error = wb_os_wide_from_utf8((const char *)encoded, &wide, &length);
		if (error != 0 || length != expected_units) {
			failures++;
			continue;
		}
		if (expected_units == 1) {
			if ((unsigned short)wide[0] != (unsigned short)code)
				failures++;
		} else {
			if ((unsigned short)wide[0] != (unsigned short)(0xd800UL + ((code - 0x10000UL) >> 10)) ||
			    (unsigned short)wide[1] != (unsigned short)(0xdc00UL + ((code - 0x10000UL) & 0x3ffUL)))
				failures++;
		}

		/* And back, by the count and by the NUL. */
		back = NULL;
		error = wb_os_utf8_from_wide(wide, length, &back, &back_length);
		if (error != 0 || back_length != size || memcmp(back, encoded, size + 1) != 0)
			failures++;
		free(back);
		back = NULL;
		error = wb_os_utf8_from_wide(wide, (size_t)-1, &back, &back_length);
		if (error != 0 || back_length != size || memcmp(back, encoded, size + 1) != 0)
			failures++;
		free(back);
		free(wide);
	}
	CHECK_EQ(failures, 0);
}

/* Checks wb_os_wide_from_utf8. */
static void
test_wide_from_utf8(
	void)
{
	static const unsigned short ascii[] = { 'W', 'i', 'n', '3', '2' };
	static const unsigned short two_byte[] = { 0x00e9 };
	static const unsigned short three_byte[] = { 0x3042 };
	static const unsigned short four_byte[] = { 0xd83d, 0xde00 };
	static const unsigned short mixed[] = { 'a', 0x00e9, 0x3042, 0xd83d, 0xde00, 'z' };
	static const unsigned short last_bmp[] = { 0xffff };
	static const unsigned short before_surrogates[] = { 0xd7ff };
	static const unsigned short after_surrogates[] = { 0xe000 };
	static const unsigned short highest[] = { 0xdbff, 0xdfff };
	static const unsigned short first_supplementary[] = { 0xd800, 0xdc00 };
	wchar_t *wide;
	size_t length;
	unsigned char *long_text;
	int error;
	int converted;
	wchar_t oracle[16];
	const char *mixed_text;

	/* Text of each length class, and the samples of the task. */
	expect_wide("Win32", ascii, 5);
	expect_wide("\xc3\xa9", two_byte, 1);
	expect_wide("\xe3\x81\x82", three_byte, 1);
	expect_wide("\xf0\x9f\x98\x80", four_byte, 2);
	expect_wide("a\xc3\xa9\xe3\x81\x82\xf0\x9f\x98\x80z", mixed, 6);
	expect_wide("\xef\xbf\xbf", last_bmp, 1);
	expect_wide("\xed\x9f\xbf", before_surrogates, 1);
	expect_wide("\xee\x80\x80", after_surrogates, 1);
	expect_wide("\xf4\x8f\xbf\xbf", highest, 2);
	expect_wide("\xf0\x90\x80\x80", first_supplementary, 2);

	/* The empty string: length 0 and a NUL. */
	wide = NULL;
	length = 99;
	error = wb_os_wide_from_utf8("", &wide, &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 0);
	if (wide != NULL) {
		CHECK_EQ(wide[0], 0);
		free(wide);
	}

	/* The length output is optional. */
	wide = NULL;
	error = wb_os_wide_from_utf8("abc", &wide, NULL);
	CHECK_EQ(error, 0);
	if (wide != NULL) {
		CHECK(wcscmp(wide, L"abc") == 0);
		free(wide);
	}

	/* The Windows conversion with invalid bytes refused gives the same units for a valid sample. */
	mixed_text = "a\xc3\xa9\xe3\x81\x82\xf0\x9f\x98\x80z";
	converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mixed_text, -1, oracle, 16);
	CHECK_EQ(converted, 7);
	wide = NULL;
	error = wb_os_wide_from_utf8(mixed_text, &wide, &length);
	CHECK_EQ(error, 0);
	if (wide != NULL) {
		CHECK(wcscmp(wide, oracle) == 0);
		free(wide);
	}

	/* The invalid forms of the task: overlong, surrogates, above U+10FFFF, truncated, lone continuation, F5 to FF. */
	expect_wide_error("\xc0\x80", EILSEQ);
	expect_wide_error("\xe0\x80\x80", EILSEQ);
	expect_wide_error("\xf0\x80\x80\x80", EILSEQ);
	expect_wide_error("\xed\xa0\x80", EILSEQ);
	expect_wide_error("\xed\xbf\xbf", EILSEQ);
	expect_wide_error("\xf4\x90\x80\x80", EILSEQ);
	expect_wide_error("\xe3\x81", EILSEQ);
	expect_wide_error("\x80", EILSEQ);
	expect_wide_error("\xbf", EILSEQ);
	expect_wide_error("\xc1\xbf", EILSEQ);
	expect_wide_error("\xf5\x80\x80\x80", EILSEQ);
	expect_wide_error("\xf8\x88\x80\x80\x80", EILSEQ);
	expect_wide_error("\xfe", EILSEQ);
	expect_wide_error("\xff", EILSEQ);
	expect_wide_error("ab\xe3\x81", EILSEQ);
	expect_wide_error("\xf0\x9f\x98", EILSEQ);

	/* The arguments. */
	wide = sentinel_wide;
	length = 99;
	error = wb_os_wide_from_utf8(NULL, &wide, &length);
	CHECK_EQ(error, EINVAL);
	CHECK(wide == NULL);
	CHECK_EQ(length, 0);
	length = 99;
	error = wb_os_wide_from_utf8("a", NULL, &length);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(length, 0);

	/* The longest accepted length succeeds and one byte more is ENAMETOOLONG. */
	long_text = (unsigned char *)malloc(WB_OS_PATH_MAX + 2U);
	CHECK(long_text != NULL);
	if (long_text == NULL)
		return;
	memset(long_text, 'a', WB_OS_PATH_MAX + 1U);
	long_text[WB_OS_PATH_MAX + 1U] = '\0';
	long_text[WB_OS_PATH_MAX] = '\0';
	wide = NULL;
	error = wb_os_wide_from_utf8((const char *)long_text, &wide, &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, WB_OS_PATH_MAX);
	free(wide);
	long_text[WB_OS_PATH_MAX] = 'a';
	wide = sentinel_wide;
	length = 99;
	error = wb_os_wide_from_utf8((const char *)long_text, &wide, &length);
	CHECK_EQ(error, ENAMETOOLONG);
	CHECK(wide == NULL);
	CHECK_EQ(length, 0);
	free(long_text);
}

/* Converts units to UTF-8 and checks the bytes. */
static void
expect_text(
	const wchar_t *wide,
	size_t count,
	const char *expected,
	size_t expected_length)
{
	char *text;
	size_t length;
	int error;

	text = NULL;
	length = 99;
	error = wb_os_utf8_from_wide(wide, count, &text, &length);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	CHECK_EQ(length, expected_length);
	CHECK(memcmp(text, expected, expected_length + 1U) == 0);
	free(text);
}

/* Converts units that are not text and checks the error and the failure state of the outputs. */
static void
expect_text_error(
	const wchar_t *wide,
	size_t count,
	int expected)
{
	char *text;
	size_t length;
	int error;

	text = sentinel_text;
	length = 99;
	error = wb_os_utf8_from_wide(wide, count, &text, &length);
	CHECK_EQ(error, expected);
	CHECK(text == NULL);
	CHECK_EQ(length, 0);
}

/* Checks wb_os_utf8_from_wide. */
static void
test_utf8_from_wide(
	void)
{
	static const wchar_t mixed[] = { L'a', 0x00e9, 0x3042, 0xd83d, 0xde00, L'z', 0 };
	static const wchar_t high_alone[] = { L'a', 0xd83d, 0 };
	static const wchar_t high_then_char[] = { 0xd83d, L'a', 0 };
	static const wchar_t low_alone[] = { 0xde00, 0 };
	static const wchar_t low_then_high[] = { 0xde00, 0xd83d, 0 };
	static const wchar_t two_highs[] = { 0xd83d, 0xd83d, 0xde00, 0 };
	static const wchar_t with_nul[] = { L'a', 0, L'b', 0 };
	static const wchar_t cut_pair[] = { 0xd83d, 0xde00, 0 };
	wchar_t *big;
	char *text;
	size_t length;
	size_t i;
	int error;

	/* A terminated string stops at its NUL; a count stops at the count, not at the NUL after it. */
	expect_text(mixed, (size_t)-1, "a\xc3\xa9\xe3\x81\x82\xf0\x9f\x98\x80z", 11);
	expect_text(mixed, 6, "a\xc3\xa9\xe3\x81\x82\xf0\x9f\x98\x80z", 11);
	expect_text(mixed, 3, "a\xc3\xa9\xe3\x81\x82", 6);
	expect_text(mixed, 0, "", 0);
	expect_text(L"", (size_t)-1, "", 0);

	/* The length output is optional. */
	text = NULL;
	error = wb_os_utf8_from_wide(L"abc", (size_t)-1, &text, NULL);
	CHECK_EQ(error, 0);
	if (text != NULL) {
		CHECK(strcmp(text, "abc") == 0);
		free(text);
	}

	/* Units that are not text. */
	expect_text_error(high_alone, (size_t)-1, EILSEQ);
	expect_text_error(high_alone, 2, EILSEQ);
	expect_text_error(high_then_char, (size_t)-1, EILSEQ);
	expect_text_error(low_alone, (size_t)-1, EILSEQ);
	expect_text_error(low_then_high, (size_t)-1, EILSEQ);
	expect_text_error(two_highs, (size_t)-1, EILSEQ);
	expect_text_error(with_nul, 3, EILSEQ);
	expect_text_error(with_nul, 2, EILSEQ);
	expect_text_error(cut_pair, 1, EILSEQ);
	expect_text_error(cut_pair + 1, 1, EILSEQ);

	/* A count that covers the whole pair converts it. */
	expect_text(cut_pair, 2, "\xf0\x9f\x98\x80", 4);

	/* The arguments. */
	text = sentinel_text;
	length = 99;
	error = wb_os_utf8_from_wide(NULL, 1, &text, &length);
	CHECK_EQ(error, EINVAL);
	CHECK(text == NULL);
	CHECK_EQ(length, 0);
	length = 99;
	error = wb_os_utf8_from_wide(L"a", 1, NULL, &length);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(length, 0);

	/* The longest accepted result (32767 three-byte characters) succeeds and one more is ENAMETOOLONG. */
	big = (wchar_t *)malloc(32770U * sizeof(wchar_t));
	CHECK(big != NULL);
	if (big == NULL)
		return;
	for (i = 0; i < 32768U; i++)
		big[i] = 0x3042;
	big[32768U] = 0;
	text = NULL;
	error = wb_os_utf8_from_wide(big, 32767U, &text, &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, WB_OS_PATH_MAX);
	free(text);
	big[32767U] = 0;
	text = NULL;
	error = wb_os_utf8_from_wide(big, (size_t)-1, &text, &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, WB_OS_PATH_MAX);
	free(text);
	big[32767U] = 0x3042;
	text = sentinel_text;
	length = 99;
	error = wb_os_utf8_from_wide(big, 32768U, &text, &length);
	CHECK_EQ(error, ENAMETOOLONG);
	CHECK(text == NULL);
	CHECK_EQ(length, 0);
	text = sentinel_text;
	error = wb_os_utf8_from_wide(big, (size_t)-1, &text, &length);
	CHECK_EQ(error, ENAMETOOLONG);
	CHECK(text == NULL);
	free(big);
}

/* Converts a path and checks the wide result. */
static void
expect_path(
	const char *path,
	const wchar_t *expected)
{
	wchar_t *wide;
	int error;

	wide = NULL;
	error = wb_os_wide_path(path, &wide);
	CHECK_EQ(error, 0);
	if (error != 0 || wide == NULL)
		return;
	CHECK(wcscmp(wide, expected) == 0);
	if (wcscmp(wide, expected) != 0)
		wprintf(L"  got %ls\n  expected %ls\n", wide, expected);
	free(wide);
}

/* Converts a path that must be refused and checks the error and the output. */
static void
expect_path_error(
	const char *path,
	int expected)
{
	wchar_t *wide;
	int error;

	wide = sentinel_wide;
	error = wb_os_wide_path(path, &wide);
	CHECK_EQ(error, expected);
	CHECK(wide == NULL);
}

/* Makes a directory of the name below a parent with the layer's path conversion; returns the new path. */
static char *
make_directory(
	const char *parent,
	const char *name)
{
	char *path;
	wchar_t *wide;
	size_t size;
	int error;
	BOOL made;

	size = strlen(parent) + strlen(name) + 2U;
	path = (char *)malloc(size);
	if (path == NULL)
		return NULL;
	snprintf(path, size, "%s\\%s", parent, name);
	wide = NULL;
	error = wb_os_wide_path(path, &wide);
	CHECK_EQ(error, 0);
	if (error != 0) {
		free(path);
		return NULL;
	}
	made = CreateDirectoryW(wide, NULL);
	CHECK(made);
	free(wide);
	return path;
}

/* Checks wb_os_wide_path. */
static void
test_wide_path(
	void)
{
	wchar_t current[1024];
	char current_utf8[4096];
	char names[4][128];
	char *parent;
	char *next;
	char *file_path;
	char *huge;
	wchar_t *wide;
	DWORD got;
	DWORD attributes;
	HANDLE handle;
	size_t i;
	int error;
	int converted;

	/* Relative paths only have their separators turned, with no prefix. */
	expect_path("a/b\\c", L"a\\b\\c");
	expect_path("name", L"name");
	expect_path("../up/x", L"..\\up\\x");
	expect_path("./x", L".\\x");

	/* A path that depends on the current drive or directory is left relative. */
	expect_path("C:rel/x", L"C:rel\\x");
	expect_path("/rooted/x", L"\\rooted\\x");

	/* A drive path is made full and prefixed. */
	expect_path("C:/x/../y", L"\\\\?\\C:\\y");
	expect_path("C:\\a\\.\\b\\\\c", L"\\\\?\\C:\\a\\b\\c");
	expect_path("c:/Dir/File.txt", L"\\\\?\\c:\\Dir\\File.txt");
	expect_path("C:\\", L"\\\\?\\C:\\");

	/* A UNC path loses its two backslashes to "\\?\UNC\". */
	expect_path("\\\\srv\\share\\x", L"\\\\?\\UNC\\srv\\share\\x");
	expect_path("//srv/share/x/../y", L"\\\\?\\UNC\\srv\\share\\y");

	/* An extended-length path and a device path are left as they are, separators turned. */
	expect_path("\\\\?\\C:\\x", L"\\\\?\\C:\\x");
	expect_path("\\\\?\\UNC\\srv\\share\\x", L"\\\\?\\UNC\\srv\\share\\x");
	expect_path("\\\\.\\pipe\\name", L"\\\\.\\pipe\\name");
	expect_path("//?/C:/x", L"\\\\?\\C:\\x");

	/* Non-ASCII text goes through UTF-16. */
	expect_path("C:/\xe3\x81\x82/\xf0\x9f\x98\x80", L"\\\\?\\C:\\\x3042\\\xd83d\xde00");

	/* The refusals. */
	expect_path_error("", EINVAL);
	expect_path_error(NULL, EINVAL);
	expect_path_error("C:/\xc0\x80", EILSEQ);
	expect_path_error("rel/\xff", EILSEQ);
	error = wb_os_wide_path("a", NULL);
	CHECK_EQ(error, EINVAL);

	/* A drive path whose full form is past the limit of an extended path is ENAMETOOLONG. */
	huge = (char *)malloc(40000U);
	CHECK(huge != NULL);
	if (huge != NULL) {
		memset(huge, 'a', 39990U);
		memcpy(huge, "C:\\", 3);
		huge[39990U] = '\0';
		expect_path_error(huge, ENAMETOOLONG);
		free(huge);
	}

	/* A path of more than MAX_PATH characters is prefixed and the system makes a file with it. */
	got = GetCurrentDirectoryW(1024U, current);
	CHECK(got > 0 && got < 1024U);
	if (got == 0 || got >= 1024U)
		return;
	converted = WideCharToMultiByte(CP_UTF8, 0, current, -1, current_utf8, (int)sizeof(current_utf8), NULL, NULL);
	CHECK(converted > 0);
	if (converted <= 0)
		return;
	parent = (char *)malloc(strlen(current_utf8) + 1U);
	CHECK(parent != NULL);
	if (parent == NULL)
		return;
	strcpy(parent, current_utf8);
	for (i = 0; i < 4U; i++) {
		memset(names[i], 'a' + (int)i, 100U);
		names[i][100] = '\0';
		next = make_directory(parent, names[i]);
		free(parent);
		parent = next;
		if (parent == NULL)
			return;
	}
	file_path = (char *)malloc(strlen(parent) + 16U);
	CHECK(file_path != NULL);
	if (file_path == NULL) {
		free(parent);
		return;
	}
	snprintf(file_path, strlen(parent) + 16U, "%s/long-file.txt", parent);
	CHECK(strlen(file_path) > 300U);
	wide = NULL;
	error = wb_os_wide_path(file_path, &wide);
	CHECK_EQ(error, 0);
	if (error == 0) {
		CHECK(wcslen(wide) > 300U);
		CHECK(wcsncmp(wide, L"\\\\?\\", 4) == 0);
		handle = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(handle != INVALID_HANDLE_VALUE);
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
		attributes = GetFileAttributesW(wide);
		CHECK(attributes != INVALID_FILE_ATTRIBUTES);
		free(wide);
	}
	free(file_path);
	free(parent);
}

/* Runs the checks. */
int
main(
	int argc,
	char **argv)
{
	/* The harness passes the system's name; this test is for Windows only. */
	CHECK(argc == 2);
	if (argc == 2)
		CHECK(strcmp(argv[1], "win32") == 0);

	test_error_from_system();
	test_error_from_socket();
	test_wide_from_utf8();
	test_utf8_from_wide();
	test_utf8_sweeps();
	test_utf8_round_trip();
	test_wide_path();

	return check_finish("os-win32");
}
