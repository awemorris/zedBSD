/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of AES and its counter mode: the key and the
 * plaintext are marked secret, so memcheck reports any branch or address
 * that depends on their bytes (the sizes and the key size are public).
 * The counter block is public, as it is in GCM (it comes from the IV): the
 * compiler may use it as the induction variable of the loop that makes the
 * counter blocks, which would make a loop test depend on its value.  The
 * outputs are made public only after the call.
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-aes.h"

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
		out[index] = (uint8_t)(seed + index * 29U + (index >> 2));
}

/* Folds a public output into one value, to keep the call from being dropped. */
static unsigned
fold(
	const uint8_t *bytes,
	size_t size)
{
	unsigned value;
	size_t index;

	/* Exclusive-or of the bytes, rotated. */
	value = 0;
	for (index = 0; index < size; index++)
		value = ((value << 1) | (value >> 7)) ^ bytes[index];
	return value & 0xffU;
}

/* Runs one key size. */
static unsigned
run_key_size(
	size_t key_size,
	unsigned seed)
{
	struct cryc_aes aes;
	uint8_t key[32];
	uint8_t counter[16];
	uint8_t plain[150];
	uint8_t out[150];
	unsigned total;
	int error;

	total = 0;

	/* The key expansion of a secret key. */
	fill(key, key_size, seed);
	CT_SECRET(key, key_size);
	error = cryc_aes_init(&aes, key, key_size);
	CHECK(error == 0);
	CT_PUBLIC(key, key_size);

	/* The schedule is secret: it is used below only as the cipher uses it. */

	/* The cipher of secret blocks, five of them (a group of four and one). */
	fill(plain, 80U, seed + 1U);
	CT_SECRET(plain, 80U);
	cryc_aes_encrypt_blocks(&aes, plain, out, 5U);
	CT_PUBLIC(out, 80U);
	total += fold(out, 80U);
	CT_PUBLIC(plain, 80U);

	/* The counter mode with a secret message, a partial last block, and a counter that wraps. */
	fill(counter, sizeof(counter), seed + 2U);
	counter[12] = 0xffU;
	counter[13] = 0xffU;
	counter[14] = 0xffU;
	counter[15] = 0xfeU;
	fill(plain, sizeof(plain), seed + 3U);
	CT_SECRET(plain, sizeof(plain));
	cryc_aes_ctr32_xor(&aes, counter, plain, out, 133U);
	CT_PUBLIC(out, 133U);
	total += fold(out, 133U);
	total += fold(counter, sizeof(counter));
	CT_PUBLIC(plain, sizeof(plain));

	/* The key is wiped. */
	cryc_aes_clear(&aes);
	return total;
}

/* Runs the checks. */
int
main(void)
{
	unsigned total;

	/* The three key sizes. */
	total = run_key_size(16U, 3U);
	total += run_key_size(24U, 7U);
	total += run_key_size(32U, 11U);

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);

	/* The result. */
	return check_finish("ct-aes");
}
