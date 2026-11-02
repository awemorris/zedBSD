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

/* The libFuzzer entry point, run once for every generated input. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/*
 * Runs the ML-KEM operations on the input.
 *
 * Always reports 0, because a refusal is not a failure.
 */
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
	size_t ciphertext_size;
	size_t candidate_size;
	int status;
	EVP_PKEY *key;

	/* The fixed key pair, made once from fixed seeds. */
	if (!ready) {
		for (index = 0; index < sizeof(seed); index++)
			seed[index] = (uint8_t)index;
		cryc_mlkem768_keygen_internal(ek, dk, seed, seed);
		ready = 1;
	}

	/* Decapsulation of the input as a ciphertext, zero-padded or cut to size. */
	ciphertext_size = size;
	if (ciphertext_size > sizeof(ciphertext))
		ciphertext_size = sizeof(ciphertext);
	memset(ciphertext, 0, sizeof(ciphertext));
	memcpy(ciphertext, data, ciphertext_size);
	cryc_mlkem768_decaps(shared, ciphertext, dk);

	/* The input as an encapsulation key: checked, and used when it passes. */
	candidate_size = size;
	if (candidate_size > sizeof(candidate))
		candidate_size = sizeof(candidate);
	memset(candidate, 0, sizeof(candidate));
	memcpy(candidate, data, candidate_size);
	status = cryc_mlkem768_check_ek(candidate);
	if (status == 0)
		cryc_mlkem768_encaps_internal(shared, ciphertext, candidate, seed);

	/* The public import of the raw key, of any length. */
	key = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, data, size);
	EVP_PKEY_free(key);

	/* The input never fails the run on its own. */
	return 0;
}
