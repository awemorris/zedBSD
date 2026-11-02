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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

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
	unsigned blocks;
	int error;

	text = malloc(size != 0U ? size : 1U);
	decoded = malloc(size + 1U);
	if (text == NULL || decoded == NULL) {
		free(text);
		free(decoded);
		return 0;
	}
	if (size != 0U)
		memcpy(text, data, size);

	/* Every block, skipping broken ones as a bundle loader does, until none is left (at most 4096 rounds). */
	position = 0;
	for (blocks = 0; blocks < 4096U; blocks++) {
		der = NULL;
		error = cryc_pem_next_certificate(text, size, &position, &der, &der_size);
		if (error == 0) {
			free(der);
			continue;
		}
		if (error != EINVAL)
			break;
	}

	/* The base64 decoder over the whole text. */
	(void)cryc_base64_decode(text, size, decoded, size + 1U, &written);

	/* The store's PEM path. */
	if (cryc_store_init(&store) == 0) {
		(void)cryc_store_add_pem(&store, text, size, &added);
		cryc_store_free(&store);
	}
	free(text);
	free(decoded);
	return 0;
}
