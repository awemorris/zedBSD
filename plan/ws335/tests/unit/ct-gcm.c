/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of AES-GCM: the key and the plaintext are
 * marked secret, and so are the operands of the multiplication of
 * GF(2^128) (the hash subkey and the GHASH state), so memcheck reports any
 * branch or address that depends on their bytes.  The lengths, the IV, the
 * additional data and the ciphertext are public, as they are on the wire.
 * Opening is checked with a good tag and with a forged one: the result and
 * the plaintext are made public only after the call, since the return value
 * tells the caller whether the tag was good.
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-gcm.h"

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

/* Runs one key size and one IV size. */
static unsigned
run_case(
	size_t key_size,
	size_t iv_size,
	unsigned seed)
{
	struct cryc_gcm gcm;
	uint8_t key[32];
	uint8_t iv[16];
	uint8_t aad[37];
	uint8_t plain[133];
	uint8_t cipher[133];
	uint8_t back[133];
	uint8_t tag[16];
	uint8_t forged[16];
	uint8_t x[16];
	uint8_t y[16];
	uint8_t product[16];
	unsigned total;
	int error;

	total = 0;

	/* The multiplication of two secret blocks, and the same in place. */
	fill(x, sizeof(x), seed);
	fill(y, sizeof(y), seed + 5U);
	CT_SECRET(x, sizeof(x));
	CT_SECRET(y, sizeof(y));
	cryc_gcm_multiply(product, x, y);
	CT_PUBLIC(product, sizeof(product));
	total += fold(product, sizeof(product));
	CT_PUBLIC(x, sizeof(x));
	CT_PUBLIC(y, sizeof(y));

	/* GHASH of public data with a secret subkey, from a secret state. */
	fill(x, sizeof(x), seed + 1U);
	fill(y, sizeof(y), seed + 2U);
	CT_SECRET(x, sizeof(x));
	CT_SECRET(y, sizeof(y));
	cryc_gcm_ghash(x, y, plain, 37U);
	CT_PUBLIC(x, sizeof(x));
	total += fold(x, sizeof(x));
	CT_PUBLIC(y, sizeof(y));

	/* The key expansion and H of a secret key. */
	fill(key, key_size, seed + 3U);
	CT_SECRET(key, key_size);
	error = cryc_gcm_init(&gcm, key, key_size);
	CHECK(error == 0);
	CT_PUBLIC(key, key_size);

	/* Seal a secret plaintext under public IV and additional data. */
	fill(iv, iv_size, seed + 4U);
	fill(aad, sizeof(aad), seed + 6U);
	fill(plain, sizeof(plain), seed + 7U);
	CT_SECRET(plain, sizeof(plain));
	error = cryc_gcm_seal(&gcm, iv, iv_size, aad, sizeof(aad), plain, sizeof(plain), cipher, tag);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == 0);
	CT_PUBLIC(cipher, sizeof(cipher));
	CT_PUBLIC(tag, sizeof(tag));
	total += fold(cipher, sizeof(cipher));
	total += fold(tag, sizeof(tag));
	CT_PUBLIC(plain, sizeof(plain));

	/* Open a good message with the secret key: the plaintext and the verdict are public afterwards. */
	error = cryc_gcm_open(&gcm, iv, iv_size, aad, sizeof(aad), cipher, sizeof(cipher), back, tag);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == 0);
	CT_PUBLIC(back, sizeof(back));
	total += fold(back, sizeof(back));

	/* Open a forged message: the same code runs, the verdict is EBADMSG and out is zero. */
	memcpy(forged, tag, sizeof(forged));
	forged[3] ^= 0x04;
	error = cryc_gcm_open(&gcm, iv, iv_size, aad, sizeof(aad), cipher, sizeof(cipher), back, forged);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == EBADMSG);
	CT_PUBLIC(back, sizeof(back));
	total += fold(back, sizeof(back));

	/* The key is wiped. */
	cryc_gcm_clear(&gcm);
	return total;
}

/* Runs the checks. */
int
main(void)
{
	unsigned total;

	/* The three key sizes, with a 96-bit IV and a 128-bit one (the GHASH form of J0). */
	total = run_case(16U, 12U, 3U);
	total += run_case(24U, 12U, 7U);
	total += run_case(32U, 12U, 11U);
	total += run_case(16U, 16U, 13U);
	total += run_case(32U, 16U, 17U);

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);

	/* The result. */
	return check_finish("ct-gcm");
}
