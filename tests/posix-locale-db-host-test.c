/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int host_errno;
static const void *thread_locale;

int *
__libc_errno_location(void)
{
	return &host_errno;
}

const void *
__pthread_locale_exchange(const void *locale, int setting)
{
	const void *previous = thread_locale;

	if (setting)
		thread_locale = locale;
	return previous;
}

int
main(int argc, char **argv)
{
	struct lconv *values;
	locale_t mixed;

	if (argc != 2 || setenv("LOCPATH", argv[1], 1) != 0)
		return 2;
	if (setlocale(LC_ALL, "zed-test") == NULL) {
		fprintf(stderr, "setlocale failed\n");
		return 1;
	}
	if (MB_CUR_MAX != 4U || strcmp(nl_langinfo(CODESET), "UTF-8") != 0 ||
	    strcmp(nl_langinfo(D_FMT), "%Y-%m-%d") != 0 ||
	    strcmp(nl_langinfo(YESEXPR), "^[jJyY]") != 0) {
		fprintf(stderr,
			"locale values: mb=%zu codeset=%s d_fmt=%s yes=%s\n",
			MB_CUR_MAX, nl_langinfo(CODESET), nl_langinfo(D_FMT),
			nl_langinfo(YESEXPR));
		return 1;
	}
	values = localeconv();
	if (strcmp(values->decimal_point, ",") != 0 ||
	    strcmp(values->currency_symbol, "Z$") != 0 ||
	    values->grouping[0] != 3 || values->grouping[1] != 3 ||
	    values->frac_digits != 2) {
		fprintf(stderr,
			"lconv values: decimal=%s currency=%s grouping=%d,%d "
			"frac=%d\n",
			values->decimal_point, values->currency_symbol,
			(unsigned char)values->grouping[0],
			(unsigned char)values->grouping[1],
			values->frac_digits);
		return 1;
	}
	if (setlocale(LC_ALL, "C") == NULL)
		return 1;
	mixed = newlocale(LC_NUMERIC_MASK, "zed-test", NULL);
	if (mixed == NULL || uselocale(mixed) == NULL || MB_CUR_MAX != 1U ||
	    strcmp(localeconv()->decimal_point, ",") != 0 ||
	    strcmp(nl_langinfo(CODESET), "US-ASCII") != 0) {
		fprintf(
		    stderr,
		    "mixed locale: object=%p mb=%zu decimal=%s codeset=%s\n",
		    (void *)mixed, MB_CUR_MAX, localeconv()->decimal_point,
		    nl_langinfo(CODESET));
		return 1;
	}
	(void)uselocale(LC_GLOBAL_LOCALE);
	freelocale(mixed);
	puts("zedBSD POSIX locale artifact host test: PASS");
	return 0;
}
