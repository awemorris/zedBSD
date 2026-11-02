/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of Ed25519: the seed is secret, so memcheck
 * reports any branch or address that depends on it in the public key
 * derivation and in signing (the nonce, the scalar multiplications, the
 * table reads and the scalar arithmetic modulo the group order).  The
 * message and the outputs (the public key and the signature) are public;
 * the outputs are made public only after the call, as a caller does when
 * it sends them.  Verification handles public values only and is not
 * marked.
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-ed25519.h"

/* Fills size bytes at out with a fixed pattern. */
static void fill(uint8_t *out, size_t size, unsigned seed);

/* Fills size bytes at out with a fixed pattern. */
static void
fill(
	uint8_t *out,
	size_t size,
	unsigned seed)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(seed + index * 31U + (index >> 3));
}

/* Runs the checks. */
int
main(void)
{
	uint8_t seed[32];
	uint8_t public_key[32];
	uint8_t signature[64];
	uint8_t message[200];
	unsigned int round;
	size_t size;
	int error;

	/* Public key and signature for several seeds and message lengths. */
	for (round = 0; round < 4U; round++) {
		fill(seed, sizeof(seed), 3U + round * 40U);
		fill(message, sizeof(message), 11U);

		/* The message length is public: empty in the first round, then 60 bytes longer each round. */
		size = 0U;
		if (round != 0U)
			size = round * 60U;

		/* The public key of a secret seed. */
		CT_SECRET(seed, sizeof(seed));
		error = cryc_ed25519_public(public_key, seed);
		CT_PUBLIC(&error, sizeof(error));
		CHECK(error == 0);
		CT_PUBLIC(public_key, sizeof(public_key));
		CT_PUBLIC(seed, sizeof(seed));

		/* A signature under the secret seed. */
		CT_SECRET(seed, sizeof(seed));
		error = cryc_ed25519_sign(signature, message, size, seed);
		CT_PUBLIC(&error, sizeof(error));
		CHECK(error == 0);
		CT_PUBLIC(signature, sizeof(signature));
		CT_PUBLIC(seed, sizeof(seed));

		/* The result is usable: it verifies. */
		CHECK(cryc_ed25519_verify(signature, message, size, public_key) == 0);
	}

	/* The result. */
	return check_finish("ct-ed25519");
}
