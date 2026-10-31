/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the scanf family (src/libc/scan.c).
 *
 * The file is compiled together with scan.c, every public name of the
 * family renamed (-Dsscanf=zed_sscanf and so on), over the host's own
 * stdio, so the calls below reach the zedBSD scanner and its streams are
 * the host's.  It checks that a stream keeps what a conversion did not use,
 * that white space skips new-lines, and the conversions one by one.
 */

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* How many checks failed. */
static int failures;

static void check(int condition, const char *what);
static FILE *stream_of(const char *text);

/*
 * Runs every check and reports.
 */
int
main(void)
{
	char long_line[6000];
	char word[64];
	char other[64];
	wchar_t wide[16];
	unsigned int width;
	unsigned int height;
	unsigned int depth;
	unsigned char small;
	long long big;
	size_t size;
	double number;
	long double precise;
	float single;
	void *pointer;
	FILE *file;
	int first;
	int second;
	int count;
	int used;
	char letter;

	/* The PPM header the gpu-selftest reads: three lines, one fscanf. */
	file = stream_of("P6\n320 240\n255\nX");
	count = fscanf(file, "P6 %u %u %u", &width, &height, &depth);
	check(count == 3, "fscanf reads across new-lines");
	check(width == 320 && height == 240 && depth == 255, "the PPM header's values");
	check(fgetc(file) == '\n', "the new-line after the last item stays unread");
	check(fgetc(file) == 'X', "the stream goes on after it");
	fclose(file);

	/* What a conversion did not use stays in the stream. */
	file = stream_of("12abc 7\n8\n");
	check(fscanf(file, "%d", &first) == 1 && first == 12, "an integer before letters");
	check(fgetc(file) == 'a', "the letters stay in the stream");
	check(fscanf(file, "%*s %d", &first) == 1 && first == 7, "the rest of the line");
	check(fscanf(file, "%d", &second) == 1 && second == 8, "the next line");
	check(fscanf(file, "%d", &second) == EOF, "EOF at the end");
	fclose(file);

	/* A line longer than any line buffer. */
	memset(long_line, 'a', sizeof(long_line) - 8U);
	strcpy(long_line + sizeof(long_line) - 8U, " 77\n");
	file = stream_of(long_line);
	check(fscanf(file, "%*s %d", &first) == 1 && first == 77, "an item after a 5992-byte word");
	fclose(file);

	/* A literal that does not match stays unread. */
	file = stream_of("1,2;3");
	check(fscanf(file, "%d,%d,%d", &first, &second, &count) == 2, "a literal mismatch stops the call");
	check(fgetc(file) == ';', "the mismatched character stays unread");
	fclose(file);

	/* An empty input and a matching failure. */
	check(sscanf("", "%d", &first) == EOF, "EOF for an empty string");
	check(sscanf("   ", "%d", &first) == EOF, "EOF for white space only");
	check(sscanf("x", "%d", &first) == 0, "0 for a letter");
	check(sscanf("100ergs", "%f", &single) == 0, "100ergs does not match %f (ISO C's example)");

	/* Integers. */
	check(sscanf("-42", "%d", &first) == 1 && first == -42, "%d negative");
	check(sscanf("0x1f", "%x", &width) == 1 && width == 31U, "%x with 0x");
	check(sscanf("1f", "%x", &width) == 1 && width == 31U, "%x without 0x");
	check(sscanf("017", "%i", &first) == 1 && first == 15, "%i octal");
	check(sscanf("0x10", "%i", &first) == 1 && first == 16, "%i hexadecimal");
	check(sscanf("0", "%i", &first) == 1 && first == 0, "%i zero");
	check(sscanf("-1", "%u", &width) == 1 && width == UINT32_MAX, "%u negative");
	check(sscanf("300", "%hhu", &small) == 1 && small == 44U, "%hhu wraps");
	check(sscanf("9000000000", "%lld", &big) == 1 && big == 9000000000LL, "%lld");
	check(sscanf("123", "%zu", &size) == 1 && size == 123U, "%zu");
	check(sscanf("12345", "%3d%d", &first, &second) == 2 && first == 123 && second == 45, "a width splits an integer");
	check(sscanf("0x", "%x", &width) == 0, "0x without digits does not match");

	/* Floating numbers. */
	check(sscanf("3.5", "%f", &single) == 1 && single == 3.5f, "%f");
	check(sscanf("1e3", "%lf", &number) == 1 && number == 1000.0, "%lf with an exponent");
	check(sscanf("-0.25", "%Lf", &precise) == 1 && precise == -0.25L, "%Lf");
	check(sscanf("0x1p4", "%lf", &number) == 1 && number == 16.0, "%lf hexadecimal");
	check(sscanf("inf", "%lf", &number) == 1 && isinf(number), "inf");
	check(sscanf("-Infinity", "%lf", &number) == 1 && isinf(number) && number < 0.0, "-Infinity");
	check(sscanf("nan(x1)", "%lf", &number) == 1 && isnan(number), "nan with characters");
	check(sscanf("2.5e", "%lf", &number) == 0, "an exponent without digits does not match");
	check(sscanf(".5,", "%lf%c", &number, &letter) == 2 && number == 0.5 && letter == ',', "a leading point");

	/* Strings, characters and sets. */
	check(sscanf("  hello world", "%s %s", word, other) == 2, "two words");
	check(strcmp(word, "hello") == 0 && strcmp(other, "world") == 0, "the words' text");
	check(sscanf("abcdef", "%3s%s", word, other) == 2 && strcmp(word, "abc") == 0, "a width splits a word");
	check(sscanf(" x", "%c", &letter) == 1 && letter == ' ', "%c takes white space");
	check(sscanf(" x", " %c", &letter) == 1 && letter == 'x', "' %c' skips it");
	check(sscanf("abc123", "%[a-z]%d", word, &first) == 2 && strcmp(word, "abc") == 0 && first == 123, "%[a-z]");
	check(sscanf("key=value", "%[^=]=%s", word, other) == 2 && strcmp(word, "key") == 0, "%[^=]");
	check(sscanf("]]-x", "%[]-]", word) == 1 && strcmp(word, "]]-") == 0, "%[]-]");
	check(sscanf("123", "%[a-z]", word) == 0, "an empty %[ run does not match");
	check(sscanf("ab cd", "%ls", wide) == 1 && wcscmp(wide, L"ab") == 0, "%ls");
	check(sscanf("xy", "%2lc", wide) == 1 && wide[0] == L'x' && wide[1] == L'y', "%2lc");

	/* %n, %%, %p and suppression. */
	check(sscanf("ab  12", "%s%n %d", word, &used, &first) == 2 && used == 2, "%n counts the characters used");
	check(sscanf("50%", "%d%%", &first) == 1 && first == 50, "%%");
	check(sscanf("0x1234", "%p", &pointer) == 1 && pointer == (void *)(uintptr_t)0x1234, "%p");
	check(sscanf("1 2", "%*d %d", &first) == 1 && first == 2, "a suppressed item is not counted");

	/* The report. */
	if (failures != 0) {
		printf("scanf host test: %d FAILED\n", failures);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("scanf host test: PASS\n");
	return 0;
}

/* Counts and names a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A passed check says nothing. */
	if (condition)
		return;

	/* A failed one is named and counted. */
	printf("FAIL: %s\n", what);
	failures++;
}

/* Returns a stream that reads a text, from a temporary file. */
static FILE *
stream_of(
	const char *text)
{
	FILE *file;

	/* The text in a temporary file, read from its start. */
	file = tmpfile();
	if (file == NULL) {
		perror("tmpfile");
		exit(2);
	}

	/* The text, and the stream back at its start. */
	fputs(text, file);
	rewind(file);

	/* Succeeded: the stream reads the text. */
	return file;
}
