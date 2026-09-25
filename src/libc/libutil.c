/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD utility library.
 *
 * What is here is not part of the C library: nothing in the standard asks
 * for it, and a program that does not want it should not carry it.  It is
 * built from the same tree as libc so that the two cannot drift apart.
 *
 * Written from what each interface is defined to do, not from another
 * system's source.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <util.h>

/* The prefixes, in the order their powers increase. */
static const char scale_prefix[] = "BKMGTPE";

/*
 * Implements the fmt scaled operation.
 *
 * Writes a number the way a person reads one: the largest prefix whose
 * value the number reaches, and one decimal place when that leaves room for
 * it.  A number below a thousand is written as it is, with no prefix at
 * all, because rounding it would lose more than it saved.
 */
int
fmt_scaled(
	long long number,
	char *out)
{
	long long value;
	long long fraction;
	unsigned prefix;
	int negative;

	/* Handles the out availability. */
	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}
	negative = number < 0;

	/*
	 * The most negative value has no positive counterpart, so it is
	 * stepped in before it is made positive.
	 */
	if (number == LLONG_MIN) {
		value = LLONG_MAX;
	} else {
		value = negative ? -number : number;
	}

	/* A small number is written out, since a prefix would only round it. */
	if (value < 1024) {
		(void)snprintf(out, FMT_SCALED_STRSIZE, "%s%lldB",
			       negative ? "-" : "", value);

		/* Reports successful completion. */
		return 0;
	}

	/* Steps up until what is left is under a thousand and twenty-four. */
	prefix = 0;
	fraction = 0;
	while (value >= 1024 && prefix + 1U < sizeof(scale_prefix) - 1U) {
		fraction = value % 1024;
		value /= 1024;
		prefix++;
	}

	/* Handles a number too large for the prefixes there are. */
	if (value >= 1024) {
		errno = ERANGE;

		/* Reports operation failure. */
		return -1;
	}

	/*
	 * One decimal place, but only while the whole part is a single
	 * digit: "1.5G" says more than "15G" would leave room to say.
	 */
	if (value < 10) {
		(void)snprintf(out, FMT_SCALED_STRSIZE, "%s%lld.%lld%c",
			       negative ? "-" : "", value,
			       (fraction * 10 + 512) / 1024 % 10,
			       scale_prefix[prefix]);
	} else {
		(void)snprintf(out, FMT_SCALED_STRSIZE, "%s%lld%c",
			       negative ? "-" : "", value,
			       scale_prefix[prefix]);
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the scan scaled operation.
 *
 * Reads back what fmt_scaled writes, and also what a person would type: a
 * number, optionally a decimal part, and optionally a prefix.  A prefix
 * this does not know is refused rather than ignored, because ignoring one
 * would turn a gigabyte into a byte without saying so.
 */
int
scan_scaled(
	char *text,
	long long *result)
{
	const char *at;
	const char *found;
	long long whole;
	long long fraction;
	long long divisor;
	long long multiplier;
	int negative;

	/* Handles the arguments availability. */
	if (text == NULL || result == NULL) {
		errno = EINVAL;
		return -1;
	}
	at = text;
	while (isspace((unsigned char)*at))
		at++;
	negative = 0;
	if (*at == '-' || *at == '+') {
		negative = *at == '-';
		at++;
	}

	/* Rejects text that begins with no digit at all. */
	if (!isdigit((unsigned char)*at)) {
		errno = EINVAL;
		return -1;
	}

	/* Reads the whole part, refusing one that will not fit. */
	whole = 0;
	while (isdigit((unsigned char)*at)) {
		if (whole > (LLONG_MAX - (*at - '0')) / 10) {
			errno = ERANGE;
			return -1;
		}
		whole = whole * 10 + (*at - '0');
		at++;
	}

	/* Reads the decimal part, keeping only what can matter. */
	fraction = 0;
	divisor = 1;
	if (*at == '.') {
		at++;
		while (isdigit((unsigned char)*at)) {
			if (divisor <= 1000000) {
				fraction = fraction * 10 + (*at - '0');
				divisor *= 10;
			}
			at++;
		}
	}

	/* A prefix multiplies; one that is not known is an error. */
	multiplier = 1;
	if (*at != '\0' && !isspace((unsigned char)*at)) {
		found = strchr(scale_prefix, toupper((unsigned char)*at));
		if (found == NULL) {
			errno = EINVAL;
			return -1;
		}

		/* Process each remaining element. */
		while (found != scale_prefix) {
			if (multiplier > LLONG_MAX / 1024) {
				errno = ERANGE;
				return -1;
			}
			multiplier *= 1024;
			found--;
		}
		at++;
	}

	/* Rejects anything after the prefix but the end of the text. */
	while (isspace((unsigned char)*at))
		at++;
	if (*at != '\0') {
		errno = EINVAL;
		return -1;
	}

	/* Handles a product that will not fit. */
	if (whole > LLONG_MAX / multiplier) {
		errno = ERANGE;
		return -1;
	}
	whole *= multiplier;
	whole += fraction * multiplier / divisor;
	*result = negative ? -whole : whole;

	/* Reports successful completion. */
	return 0;
}
