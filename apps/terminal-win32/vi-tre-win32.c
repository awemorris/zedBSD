/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * UTF-8 mbtowc implementation for TRE regular expressions on Windows.
 */

#include "apps/terminal-win32/vi-tre-win32.h"

#include <stdint.h>

/*
 * Decodes one UTF-8 character from s (up to n bytes) into *pwc.
 *
 * Code points above the Basic Multilingual Plane are mapped to U+FFFD so
 * that they fit in Windows' 16-bit wchar_t while preserving the exact UTF-8
 * byte count returned to the caller.
 */
int
vi_tre_mbtowc(
	wchar_t *pwc,
	const char *s,
	size_t n)
{
	const unsigned char *bytes;
	uint32_t code;
	size_t needed;
	size_t index;

	/* Null pointer queries whether the encoding is stateful (UTF-8 is not). */
	if (s == NULL)
		return 0;
	if (n == 0U)
		return -1;

	bytes = (const unsigned char *)s;

	/* NUL byte terminates and returns 0 per ISO C mbtowc. */
	if (bytes[0] == 0U) {
		if (pwc != NULL)
			*pwc = 0;
		return 0;
	}

	/* Single-byte ASCII. */
	if (bytes[0] < 0x80U) {
		if (pwc != NULL)
			*pwc = (wchar_t)bytes[0];
		return 1;
	}

	/* Determines the expected sequence length from the leading byte. */
	if (bytes[0] >= 0xc2U && bytes[0] <= 0xdfU) {
		needed = 2U;
		code = bytes[0] & 0x1fU;
	} else if (bytes[0] >= 0xe0U && bytes[0] <= 0xefU) {
		needed = 3U;
		code = bytes[0] & 0x0fU;
	} else if (bytes[0] >= 0xf0U && bytes[0] <= 0xf4U) {
		needed = 4U;
		code = bytes[0] & 0x07U;
	} else {
		return -1;
	}

	if (n < needed)
		return -1;

	/* Reads and validates each continuation byte. */
	for (index = 1U; index < needed; index++) {
		if ((bytes[index] & 0xc0U) != 0x80U)
			return -1;
		code = (code << 6U) | (bytes[index] & 0x3fU);
	}

	/* Rejects overlong forms, UTF-16 surrogates, and out-of-range values. */
	if ((needed == 3U && code < 0x800U) ||
	    (needed == 4U && code < 0x10000U) ||
	    (code >= 0xd800U && code <= 0xdfffU) ||
	    code > 0x10ffffU)
		return -1;

	if (pwc != NULL) {
		if (code > 0xffffU)
			*pwc = (wchar_t)0xfffdU;
		else
			*pwc = (wchar_t)code;
	}

	return (int)needed;
}
