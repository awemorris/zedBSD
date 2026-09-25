/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the kernel C runtime (src/kern/kcrt.c).
 *
 * kcrt.c is compiled apart with -ffreestanding -fno-builtin
 * -DKERN_KCRT_NATIVE -DKERN_KCRT_NO_STANDARD_NAMES, so the functions under
 * test are kcrt's own byte loops and not the host C library.  This file uses
 * the host C library as the oracle: every supported format is compared with
 * the host snprintf().
 */

#define KERN_KCRT_NATIVE 1
#define KERN_KCRT_NO_STANDARD_NAMES 1

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kern/kcrt.h>

static int failures;
static int checks;

static void check(int condition, const char *what, int line);
static void check_text(const char *expected, const char *actual, const char *what, int line);
static void test_memory(void);
static void test_strings(void);
static void test_format_against_host(void);
static void test_format_subset(void);
static int format_variable(char *buffer, size_t capacity, const char *format, ...);

#define CHECK(condition) check((condition) != 0, #condition, __LINE__)

/*
 * Runs every kcrt check and reports the result.
 */
int
main(
	void)
{
	/* Runs the groups of checks. */
	test_memory();
	test_strings();
	test_format_against_host();
	test_format_subset();

	/* Reports the result. */
	if (failures != 0) {
		printf("kcrt-test: FAIL (%d of %d checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("kcrt-test: PASS (%d checks)\n", checks);
	return 0;
}

/* Counts one check and reports it when it fails. */
static void
check(
	int condition,
	const char *what,
	int line)
{
	/* Counts the check. */
	checks++;

	/* Reports a failed check. */
	if (!condition) {
		failures++;
		printf("FAIL line %d: %s\n", line, what);
	}
}

/* Compares two texts and reports a difference. */
static void
check_text(
	const char *expected,
	const char *actual,
	const char *what,
	int line)
{
	int equal;

	/* Compares the texts. */
	equal = strcmp(expected, actual) == 0;

	/* Reports a difference with both texts. */
	checks++;
	if (!equal) {
		failures++;
		printf("FAIL line %d: %s: expected \"%s\", got \"%s\"\n", line, what, expected, actual);
	}
}

/* Checks the memory functions at their boundaries. */
static void
test_memory(
	void)
{
	unsigned char buffer[32];
	unsigned char other[32];
	void *result;
	int index;

	/* A copy of zero bytes touches nothing and reports the destination. */
	memset(buffer, 0xaa, sizeof(buffer));
	result = kern_memcpy(buffer, "xyz", 0);
	CHECK(result == buffer);
	CHECK(buffer[0] == 0xaa);

	/* A copy copies exactly the requested bytes. */
	result = kern_memcpy(buffer, "abcdef", 4);
	CHECK(result == buffer);
	CHECK(memcmp(buffer, "abcd", 4) == 0);
	CHECK(buffer[4] == 0xaa);

	/* A move handles an overlap in both directions. */
	memcpy(buffer, "0123456789", 10);
	result = kern_memmove(buffer + 2, buffer, 6);
	CHECK(result == buffer + 2);
	CHECK(memcmp(buffer, "0101234589", 10) == 0);
	memcpy(buffer, "0123456789", 10);
	kern_memmove(buffer, buffer + 3, 6);
	CHECK(memcmp(buffer, "3456786789", 10) == 0);
	kern_memmove(buffer, buffer, 10);
	CHECK(memcmp(buffer, "3456786789", 10) == 0);

	/* A fill stores the low byte of the value and reports the destination. */
	result = kern_memset(buffer, 0x1234, 5);
	CHECK(result == buffer);
	for (index = 0; index < 5; index++)
		CHECK(buffer[index] == 0x34);
	CHECK(buffer[5] == '8');
	result = kern_memset_explicit(buffer, 0, 3);
	CHECK(result == buffer);
	CHECK(buffer[0] == 0 && buffer[2] == 0 && buffer[3] == 0x34);

	/* A comparison orders by the first differing byte, unsigned. */
	memcpy(other, "abc\x80", 4);
	CHECK(kern_memcmp("abc\x01", other, 4) < 0);
	CHECK(kern_memcmp(other, "abc\x01", 4) > 0);
	CHECK(kern_memcmp("abc", "abd", 2) == 0);
	CHECK(kern_memcmp("abc", "xyz", 0) == 0);

	/* A search finds the first byte, zero included, within the count only. */
	memcpy(other, "ab\0cb", 5);
	CHECK(kern_memchr(other, 'b', 5) == other + 1);
	CHECK(kern_memchr(other, 0, 5) == other + 2);
	CHECK(kern_memchr(other, 'c', 3) == NULL);
	CHECK(kern_memchr(other, 'a' + 256, 5) == other);
	CHECK(kern_memchr(other, 'a', 0) == NULL);
}

/* Checks the string functions at their boundaries. */
static void
test_strings(
	void)
{
	char buffer[16];
	const char *text;

	/* Lengths stop at the terminator or the limit. */
	CHECK(kern_strlen("") == 0);
	CHECK(kern_strlen("abc") == 3);
	CHECK(kern_strnlen("abcdef", 3) == 3);
	CHECK(kern_strnlen("ab", 5) == 2);
	CHECK(kern_strnlen("abc", 0) == 0);

	/* Comparisons order by unsigned bytes and honour the limit. */
	CHECK(kern_strcmp("abc", "abc") == 0);
	CHECK(kern_strcmp("abc", "abd") < 0);
	CHECK(kern_strcmp("abc", "ab") > 0);
	CHECK(kern_strcmp("a\x80", "a\x01") > 0);
	CHECK(kern_strncmp("abcx", "abcy", 3) == 0);
	CHECK(kern_strncmp("abcx", "abcy", 4) < 0);
	CHECK(kern_strncmp("ab", "abc", 3) < 0);
	CHECK(kern_strncmp("x", "y", 0) == 0);

	/* strcpy() copies the terminator and strcat() appends. */
	memset(buffer, 'z', sizeof(buffer));
	CHECK(kern_strcpy(buffer, "abc") == buffer);
	CHECK(memcmp(buffer, "abc\0z", 5) == 0);
	CHECK(kern_strcat(buffer, "de") == buffer);
	CHECK(strcmp(buffer, "abcde") == 0);
	CHECK(buffer[6] == 'z');

	/* strncpy() fills the rest with zeros and does not terminate a long source. */
	memset(buffer, 'z', sizeof(buffer));
	CHECK(kern_strncpy(buffer, "ab", 6) == buffer);
	CHECK(memcmp(buffer, "ab\0\0\0\0z", 7) == 0);
	memset(buffer, 'z', sizeof(buffer));
	kern_strncpy(buffer, "abcdef", 4);
	CHECK(memcmp(buffer, "abcdz", 5) == 0);

	/* Character searches include the terminator. */
	text = "abcabc";
	CHECK(kern_strchr(text, 'b') == text + 1);
	CHECK(kern_strchr(text, 0) == text + 6);
	CHECK(kern_strchr(text, 'x') == NULL);
	CHECK(kern_strrchr(text, 'b') == text + 4);
	CHECK(kern_strrchr(text, 0) == text + 6);
	CHECK(kern_strrchr(text, 'x') == NULL);
	CHECK(kern_strrchr("", 0) != NULL);

	/* Substring searches, the empty needle included. */
	CHECK(kern_strstr(text, "ca") == text + 2);
	CHECK(kern_strstr(text, "bc") == text + 1);
	CHECK(kern_strstr(text, "abcabc") == text);
	CHECK(kern_strstr(text, "abcabcd") == NULL);
	CHECK(kern_strstr(text, "") == text);
	CHECK(kern_strstr("", "") != NULL);
	CHECK(kern_strstr("", "a") == NULL);
	text = "aab";
	CHECK(kern_strstr(text, "ab") == text + 1);
}

/*
 * One supported format case, rendered by kcrt and by the host.
 */
#define FORMAT_CASE(capacity, ...) do { \
	char expected_text[128]; \
	char actual_text[128]; \
	int expected_length; \
	int actual_length; \
	memset(expected_text, '#', sizeof(expected_text)); \
	memset(actual_text, '#', sizeof(actual_text)); \
	expected_length = snprintf(expected_text, (capacity), __VA_ARGS__); \
	actual_length = kern_snprintf(actual_text, (capacity), __VA_ARGS__); \
	check(expected_length == actual_length, "length of " #__VA_ARGS__, __LINE__); \
	check(memcmp(expected_text, actual_text, sizeof(expected_text)) == 0, "bytes of " #__VA_ARGS__, __LINE__); \
	if (memcmp(expected_text, actual_text, sizeof(expected_text)) != 0) \
		printf("  host \"%.60s\" kcrt \"%.60s\"\n", expected_text, actual_text); \
} while (0)

/* Compares every supported conversion with the host C library. */
static void
test_format_against_host(
	void)
{
	size_t size;

	size = 12345;

	/* Plain conversions. */
	FORMAT_CASE(128, "%d %i %u %x %X %c %s %%", -42, 17, 4000000000U, 0xbeefU, 0xbeefU, 'q', "text");
	FORMAT_CASE(128, "%ld %lu %lx %lld %llu %llx", -5L, 6UL, 0xabcUL, -7LL, 18446744073709551615ULL, 0x123456789abcdefULL);
	FORMAT_CASE(128, "%d %d %lld", -2147483647 - 1, 2147483647, -9223372036854775807LL - 1);

	/* Flags, widths and lengths in the subset. */
	FORMAT_CASE(128, "%zu %zx %zd", size, size, (ptrdiff_t)-77);
	FORMAT_CASE(128, "%#x %#X %#x %#08x %#8x", 0xabU, 0xabU, 0U, 0x1fU, 0x1fU);
	FORMAT_CASE(128, "%016llx %08x %8x %3u %03d %05d %5d", 0xfeedULL, 0x42U, 0x42U, 12345U, 7, -42, -42);
	FORMAT_CASE(128, "%hhu %hhd %hu %hd %hhx", 300, 200, 70000, 70000, 0x1ff);
	FORMAT_CASE(128, "[%5s] [%1s] [%3c] [%s]", "ab", "abc", 'z', "");

	/* Truncation keeps the terminator and reports the whole length. */
	FORMAT_CASE(5, "%s-%u", "abcdef", 123U);
	FORMAT_CASE(1, "%s", "abc");
	FORMAT_CASE(4, "%08x", 0x12U);
}

/* Formats with a format the compiler does not check. */
static int
format_variable(
	char *buffer,
	size_t capacity,
	const char *format,
	...)
{
	va_list arguments;
	int length;

	/* Formats the variable arguments with kcrt. */
	va_start(arguments, format);
	length = kern_vsnprintf(buffer, capacity, format, arguments);
	va_end(arguments);

	/* Reports what kern_vsnprintf() reported. */
	return length;
}

/* Checks the behaviour that differs from C: specifications outside the subset. */
static void
test_format_subset(
	void)
{
	char buffer[64];
	const char *format;
	int length;

	/* A pointer shows every hexadecimal digit after 0x. */
	kern_snprintf(buffer, sizeof(buffer), "%p", (void *)(uintptr_t)0x1234abcdU);
	check_text(sizeof(void *) == 8 ? "0x000000001234abcd" : "0x1234abcd", buffer, "%p", __LINE__);

	/* A null string shows as a marker. */
	format = "%s|%3s";
	format_variable(buffer, sizeof(buffer), format, (const char *)NULL, "x");
	check_text("(null)|  x", buffer, format, __LINE__);

	/* A capacity of zero writes nothing and reports the whole length. */
	length = kern_snprintf(NULL, 0, "%s%u", "abc", 12U);
	CHECK(length == 5);

	/* A null format is the empty text. */
	memset(buffer, '#', sizeof(buffer));
	format = NULL;
	length = format_variable(buffer, sizeof(buffer), format);
	CHECK(length == 0);
	CHECK(buffer[0] == '\0');

	/* The - flag is outside the subset; its argument is consumed. */
	format = "%-5d|%s";
	length = format_variable(buffer, sizeof(buffer), format, 42, "ok");
	check_text("%d|ok", buffer, format, __LINE__);
	CHECK(length == 5);

	/* A precision with * consumes the int and the string. */
	format = "%.*s|%d";
	format_variable(buffer, sizeof(buffer), format, 3, "abcdef", 7);
	check_text("%s|7", buffer, format, __LINE__);

	/* A * width consumes its int and the value. */
	format = "%*d|%u";
	format_variable(buffer, sizeof(buffer), format, 4, 99, 5U);
	check_text("%d|5", buffer, format, __LINE__);

	/* The o conversion is outside the subset; its argument is consumed. */
	format = "%o|%u";
	format_variable(buffer, sizeof(buffer), format, 8U, 9U);
	check_text("%o|9", buffer, format, __LINE__);

	/* A long long argument of an unsupported conversion is consumed whole. */
	format = "%llo|%u";
	format_variable(buffer, sizeof(buffer), format, 0x100000000ULL, 3U);
	check_text("%o|3", buffer, format, __LINE__);

	/* The + and space flags are outside the subset. */
	format = "%+d|% d|%u";
	format_variable(buffer, sizeof(buffer), format, 1, 2, 3U);
	check_text("%d|%d|3", buffer, format, __LINE__);

	/* The n conversion consumes its pointer and stores nothing. */
	format = "a%nb%u";
	length = 77;
	format_variable(buffer, sizeof(buffer), format, &length, 4U);
	check_text("a%nb4", buffer, format, __LINE__);
	CHECK(length == 77);

	/* A wide string or character is shown, its argument consumed. */
	format = "%ls|%lc|%u";
	format_variable(buffer, sizeof(buffer), format, (void *)buffer, 65, 6U);
	check_text("%s|%c|6", buffer, format, __LINE__);

	/* An unknown conversion consumes nothing. */
	format = "%y%d";
	format_variable(buffer, sizeof(buffer), format, 5);
	check_text("%y5", buffer, format, __LINE__);

	/* A percent sign at the end stands alone. */
	format = "abc%";
	length = format_variable(buffer, sizeof(buffer), format);
	check_text("abc%", buffer, format, __LINE__);
	CHECK(length == 4);

	/* A floating-point conversion is shown and fetches no double. */
	format = "%f|%u";
	format_variable(buffer, sizeof(buffer), format, 11U);
	check_text("%f|11", buffer, format, __LINE__);

	/* A huge width is cut to the limit rather than overflowing. */
	format = "%99999999999999999999u";
	length = format_variable(NULL, 0, format, 1U);
	CHECK(length == 4096);
}
