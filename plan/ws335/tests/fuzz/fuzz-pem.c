/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libFuzzer target: PEM and base64.  The input is a PEM text: every
 * CERTIFICATE block is taken with cryc_pem_next_certificate (each DER is
 * freed), the whole text is decoded as base64, and the same text goes
 * into a trust store (cryc_store_add_pem).
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "userland/base/libcrypto-compat/cryc-chain.h"
#include "userland/base/libcrypto-compat/cryc-pem.h"

/* The libFuzzer entry point, run once for every generated input. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/*
 * Runs the PEM, base64 and trust store paths on the input as a PEM text.
 *
 * Always reports 0, because a refusal is not a failure.
 */
int
LLVMFuzzerTestOneInput(
	const uint8_t *data,
	size_t size)
{
	struct cryc_store store;
	char *text;
	uint8_t *der;
	uint8_t *decoded;
	size_t position;
	size_t der_size;
	size_t written;
	size_t added;
	size_t text_size;
	unsigned blocks;
	int error;

	/* Makes a copy of exactly size bytes, so a read past the end is caught, and the base64 output buffer. */
	text_size = size;
	if (text_size == 0U)
		text_size = 1U;
	text = malloc(text_size);
	decoded = malloc(size + 1U);

	/* Gives up when either allocation failed. */
	if (text == NULL || decoded == NULL) {
		free(text);
		free(decoded);
		return 0;
	}

	/* Fills the copy with the input. */
	if (size != 0U)
		memcpy(text, data, size);

	/* Every block, skipping broken ones as a bundle loader does, until none is left (at most 4096 rounds). */
	position = 0;
	for (blocks = 0; blocks < 4096U; blocks++) {
		der = NULL;
		error = cryc_pem_next_certificate(text, size, &position, &der, &der_size);

		/* Frees the certificate of a good block and goes on to the next. */
		if (error == 0) {
			free(der);
			continue;
		}

		/* Stops at the end of the text or at a failure other than a broken block. */
		if (error != EINVAL)
			break;
	}

	/* The base64 decoder over the whole text. */
	(void)cryc_base64_decode(text, size, decoded, size + 1U, &written);

	/* The store's PEM path. */
	error = cryc_store_init(&store);
	if (error == 0) {
		(void)cryc_store_add_pem(&store, text, size, &added);
		cryc_store_free(&store);
	}

	/* Releases the copies. */
	free(text);
	free(decoded);

	/* The input never fails the run on its own. */
	return 0;
}
