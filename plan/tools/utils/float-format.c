/*
 * ws043-p012: compares the floating conversions of zedBSD's libc
 * (src/libc/format.c, built for the host with its functions renamed
 * zed_snprintf and zed_vsnprintf) with the host's (glibc).
 *
 *   cc -std=c11 -D_GNU_SOURCE -Dvsnprintf=zed_vsnprintf -Dsnprintf=zed_snprintf \
 *       -c src/libc/format.c -o format.o
 *   cc -std=c11 -D_GNU_SOURCE plan/tools/utils/float-format.c format.o -o float-format
 *   ./float-format [COUNT]
 *
 * Prints each difference and the totals; exits 1 when there is any.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int zed_snprintf(char *buffer, size_t size, const char *format, ...);

/* The formats every value is written with. */
static const char *formats[] = {
	"%f", "%.0f", "%.1f", "%.2f", "%.10f", "%.17f", "%.20f", "%.40f",
	"%e", "%.0e", "%.1e", "%.3e", "%.16e", "%.20e", "%.30e",
	"%g", "%.0g", "%.1g", "%.3g", "%.6g", "%.10g", "%.17g", "%.25g",
	"%G", "%E", "%F", "%#g", "%#.0f", "%#.0e", "%#.3g",
	"%+f", "% e", "%12.3f", "%-12.3e|", "%012.4g", "%+015.2f", "%-+10g|",
	"%.300f", "%.1100f", "%.0e", "%.750e",
	NULL
};

/* The number of values compared, that differed, and known glibc differences. */
static unsigned long compared;
static unsigned long differed;
static unsigned long known;

/*
 * glibc drops the zeros of %#g when rounding carries into a new exponent
 * ("1.e+06" for 999999.5), though # keeps trailing zeros (C11 7.21.6.1);
 * ours keeps them ("1.00000e+06").
 */
static int
known_glibc_difference(const char *format, const char *ours, const char *theirs)
{
	if (strchr(format, '#') == NULL || strchr(format, 'g') == NULL)
		return 0;
	if (strstr(theirs, "1.e") == NULL || strstr(ours, "1.0") == NULL)
		return 0;
	return 1;
}

/* Compares one value in every format. */
static void
compare(double value)
{
	static char ours[12000];
	static char theirs[12000];
	const char **format;
	int our_length;
	int their_length;

	for (format = formats; *format != NULL; format++) {
		our_length = zed_snprintf(ours, sizeof(ours), *format, value);
		their_length = snprintf(theirs, sizeof(theirs), *format, value);
		compared++;
		if (our_length == their_length && strcmp(ours, theirs) == 0)
			continue;
		if (known_glibc_difference(*format, ours, theirs)) {
			known++;
			continue;
		}
		differed++;
		if (differed <= 40)
			printf("DIFF %-8s %a\n  ours   %.200s\n  theirs %.200s\n", *format, value, ours, theirs);
	}
}

/* Returns a random 64-bit number. */
static uint64_t
random64(void)
{
	uint64_t value;

	value = (uint64_t)rand() << 62;
	value ^= (uint64_t)rand() << 31;
	value ^= (uint64_t)rand();
	return value;
}

int
main(int argc, char **argv)
{
	static const double fixed[] = {
		0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 2.5, 0.125, 0.05, 0.15, 0.25, 0.35,
		9.5, 99.5, 0.95, 0.995, 9.999999, 123456.789, 1e-5, 1e-4, 1e-300,
		1e300, 1e308, 1.7976931348623157e308, 4.9e-324, 2.2250738585072014e-308,
		9007199254740992.0, 9007199254740993.0, 18446744073709551616.0,
		2147483648.0, 1e20, 1e21, 1e22, 1e23, 0.1, 0.2, 0.3, 1.0 / 3.0,
		2.0 / 3.0, 3.14159265358979, 2.718281828459045, 5e-324, 999999.5,
		0.00001234, 123456789012345678.0,
	};
	union { double value; uint64_t bits; } shape;
	unsigned long count;
	unsigned long index;
	int power;

	/* The values chosen by hand, and their negatives. */
	for (index = 0; index < sizeof(fixed) / sizeof(fixed[0]); index++) {
		compare(fixed[index]);
		compare(-fixed[index]);
	}

	/* Every power of two, and the halves and ties near integers. */
	for (power = -1074; power <= 1023; power++)
		compare(ldexp(1.0, power));
	for (index = 0; index < 2000; index++)
		compare((double)index + 0.5);
	for (index = 0; index < 2000; index++)
		compare((double)index / 1000.0);

	/* Random bit patterns (finite ones), and infinity and not-a-number. */
	count = 20000;
	if (argc > 1)
		count = strtoul(argv[1], NULL, 10);
	srand(12345);
	for (index = 0; index < count; index++) {
		shape.bits = random64();
		if (((shape.bits >> 52) & 0x7ffU) == 0x7ffU)
			continue;
		compare(shape.value);
	}
	compare(INFINITY);
	compare(-INFINITY);
	compare(NAN);

	printf("compared %lu, differed %lu, known glibc differences %lu\n", compared, differed, known);
	return differed != 0;
}
