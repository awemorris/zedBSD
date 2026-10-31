/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the hash functions, HMAC and HKDF: keys,
 * messages and key material are marked secret, so memcheck reports any
 * branch or address that depends on their bytes (their lengths are
 * public and may be used freely).
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-hash.h"

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
		out[index] = (uint8_t)(seed + index * 13U + (index >> 3));
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

/* Runs the checks. */
int
main(void)
{
	static const enum cryc_hash_id ids[3] = { CRYC_HASH_SHA256, CRYC_HASH_SHA384, CRYC_HASH_SHA512 };
	static const size_t key_sizes[4] = { 0, 20, 128, 300 };
	uint8_t key[300];
	uint8_t data[300];
	uint8_t out[64];
	uint8_t okm[200];
	uint8_t second[64];
	struct cryc_hash hash;
	struct cryc_hmac hmac;
	size_t which;
	size_t sizes;
	size_t digest;
	unsigned total;

	/* Each hash. */
	total = 0;
	for (which = 0; which < 3U; which++) {
		digest = cryc_hash_size(ids[which]);

		/* The hash of a secret message of several lengths, in one call and in parts. */
		fill(data, sizeof(data), 5U);
		CT_SECRET(data, sizeof(data));
		CHECK(cryc_hash_init(&hash, ids[which]) == 0);
		cryc_hash_update(&hash, data, 100U);
		cryc_hash_update(&hash, data + 100, 200U);
		cryc_hash_final(&hash, out);
		CT_PUBLIC(out, digest);
		total += fold(out, digest);
		CHECK(cryc_hash_digest(ids[which], data, 129U, second) == 0);
		CT_PUBLIC(second, digest);
		total += fold(second, digest);
		CT_PUBLIC(data, sizeof(data));

		/* HMAC with a secret key (empty, short, a block, longer than a block) and a secret message. */
		for (sizes = 0; sizes < 4U; sizes++) {
			fill(key, sizeof(key), 77U);
			fill(data, sizeof(data), 9U);
			CT_SECRET(key, sizeof(key));
			CT_SECRET(data, sizeof(data));
			CHECK(cryc_hmac(ids[which], key, key_sizes[sizes], data, 150U, out) == 0);
			CT_PUBLIC(out, digest);
			total += fold(out, digest);

			CHECK(cryc_hmac_init(&hmac, ids[which], key, key_sizes[sizes]) == 0);
			cryc_hmac_update(&hmac, data, 33U);
			cryc_hmac_update(&hmac, data + 33, 200U);
			cryc_hmac_final(&hmac, second);
			CT_PUBLIC(second, digest);
			total += fold(second, digest);
			CT_PUBLIC(key, sizeof(key));
			CT_PUBLIC(data, sizeof(data));
		}

		/* HKDF with secret input keying material and a secret pseudorandom key. */
		fill(key, sizeof(key), 31U);
		fill(data, sizeof(data), 17U);
		CT_SECRET(key, sizeof(key));
		CT_SECRET(data, sizeof(data));
		CHECK(cryc_hkdf_extract(ids[which], data, 40U, key, 70U, out) == 0);
		CT_SECRET(out, digest);
		CHECK(cryc_hkdf_expand(ids[which], out, digest, data + 100, 30U, okm, sizeof(okm)) == 0);
		CT_PUBLIC(okm, sizeof(okm));
		CT_PUBLIC(out, digest);
		total += fold(okm, sizeof(okm));
		CT_PUBLIC(key, sizeof(key));
		CT_PUBLIC(data, sizeof(data));
	}

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);

	/* The result. */
	return check_finish("ct-hash");
}
