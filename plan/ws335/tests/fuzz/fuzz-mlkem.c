/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libFuzzer target: ML-KEM-768 (cryc-mlkem.h) with input from outside.
 * The input's first 1088 bytes (zero-padded) are a ciphertext decapsulated
 * with a fixed key pair (FIPS 203 Algorithm 18: a ciphertext that does not
 * re-encrypt gives the implicit rejection, never an error); the input's
 * first 1184 bytes are checked as an encapsulation key (FIPS 203 7.2) and,
 * when they pass, encapsulated to; and the input as it is goes into the
 * public EVP_PKEY_new_raw_public_key_ex("ML-KEM-768").
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <compat/crypto/evp.h>

#include "userland/base/libcrypto-compat/cryc-mlkem.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(
	const uint8_t *data,
	size_t size)
{
	static uint8_t ek[CRYC_MLKEM768_EK];
	static uint8_t dk[CRYC_MLKEM768_DK];
	static int ready;
	uint8_t ciphertext[CRYC_MLKEM768_CT];
	uint8_t candidate[CRYC_MLKEM768_EK];
	uint8_t shared[32];
	uint8_t seed[32];
	size_t index;
	EVP_PKEY *key;

	/* The fixed key pair, made once from fixed seeds. */
	if (!ready) {
		for (index = 0; index < sizeof(seed); index++)
			seed[index] = (uint8_t)index;
		cryc_mlkem768_keygen_internal(ek, dk, seed, seed);
		ready = 1;
	}

	/* Decapsulation of the input as a ciphertext. */
	memset(ciphertext, 0, sizeof(ciphertext));
	memcpy(ciphertext, data, size < sizeof(ciphertext) ? size : sizeof(ciphertext));
	cryc_mlkem768_decaps(shared, ciphertext, dk);

	/* The input as an encapsulation key: checked, and used when it passes. */
	memset(candidate, 0, sizeof(candidate));
	memcpy(candidate, data, size < sizeof(candidate) ? size : sizeof(candidate));
	if (cryc_mlkem768_check_ek(candidate) == 0)
		cryc_mlkem768_encaps_internal(shared, ciphertext, candidate, seed);

	/* The public import of the raw key, of any length. */
	key = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, data, size);
	EVP_PKEY_free(key);
	return 0;
}
