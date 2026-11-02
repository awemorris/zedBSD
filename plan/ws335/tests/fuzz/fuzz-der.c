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

/* The libFuzzer entry point, run once for every generated input. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Reads every element of der at depth (the constructed ones recursively). */
static void walk(struct cryc_der *der, unsigned depth);

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
	int empty;
	int status;

	/* Reads the elements one after another until the input is used up. */
	for (;;) {
		/* Stops when no element is left. */
		empty = cryc_der_empty(der);
		if (empty)
			break;

		/* Reads the header of the next element. */
		status = cryc_der_next(der, &element);
		if (status != 0)
			return;

		/* A constructed element is entered and its contents are read in turn. */
		if ((element.tag & 0x20U) != 0U) {
			if (depth < 32U) {
				status = cryc_der_enter(&element, &inner);
				if (status == 0)
					walk(&inner, depth + 1U);
			}

			continue;
		}

		/* A primitive element is read with every typed decoder. */
		(void)cryc_der_integer(&element, &bytes, &length);
		(void)cryc_der_small_integer(&element, 0xffffffffUL, &small);
		(void)cryc_der_boolean(&element, &value);
		(void)cryc_der_bit_string(&element, &bytes, &length, &unused);
		(void)cryc_der_oid(&element);
		(void)cryc_der_time(&element, &seconds);
	}
}

/*
 * Reads the input as a sequence of DER elements.
 *
 * Always reports 0, because a decoder's refusal is not a failure.
 */
int
LLVMFuzzerTestOneInput(
	const uint8_t *data,
	size_t size)
{
	struct cryc_der der;
	const uint8_t *start;

	/* Takes no pointer for an empty input, so the reader sees a null one. */
	start = data;
	if (size == 0U)
		start = NULL;

	/* Starts the reader on the input. */
	cryc_der_init(&der, start, size);

	/* Reads every element, from the top level down. */
	walk(&der, 0);

	/* The input never fails the run on its own. */
	return 0;
}
