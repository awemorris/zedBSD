/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libFuzzer target: the DER reader (cryc-der.h).  The input is read as a
 * sequence of elements; each constructed one is entered (up to 32 levels)
 * and each primitive one is read with every typed decoder (integer, small
 * integer, boolean, bit string, OID, time).  A decoder's refusal is fine;
 * a read outside the input or an undefined operation is a sanitizer stop.
 */

#include <stddef.h>
#include <stdint.h>

#include "userland/base/libcrypto-compat/cryc-der.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Reads every element of der at depth (the constructed ones recursively). */
static void
walk(
	struct cryc_der *der,
	unsigned depth)
{
	struct cryc_der_element element;
	struct cryc_der inner;
	const uint8_t *bytes;
	size_t length;
	uint32_t small;
	int64_t seconds;
	unsigned unused;
	int value;

	while (!cryc_der_empty(der)) {
		if (cryc_der_next(der, &element) != 0)
			return;
		if ((element.tag & 0x20U) != 0U) {
			if (depth < 32U && cryc_der_enter(&element, &inner) == 0)
				walk(&inner, depth + 1U);
			continue;
		}
		(void)cryc_der_integer(&element, &bytes, &length);
		(void)cryc_der_small_integer(&element, 0xffffffffUL, &small);
		(void)cryc_der_boolean(&element, &value);
		(void)cryc_der_bit_string(&element, &bytes, &length, &unused);
		(void)cryc_der_oid(&element);
		(void)cryc_der_time(&element, &seconds);
	}
}

int
LLVMFuzzerTestOneInput(
	const uint8_t *data,
	size_t size)
{
	struct cryc_der der;

	cryc_der_init(&der, size != 0U ? data : NULL, size);
	walk(&der, 0);
	return 0;
}
