/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of ChaCha20, Poly1305 and the AEAD: keys,
 * nonces, plaintexts, additional data and the Poly1305 key are marked
 * secret, so memcheck reports any branch or address that depends on their
 * bytes (the sizes are public and may be used freely).  The result of a
 * tag check (the return value of open, and the plaintext written under
 * the mask) is made public only after the call, as a caller does.
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-chacha.h"

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
	uint8_t key[32];
	uint8_t nonce[12];
	uint8_t nonce8[8];
	uint8_t aad[40];
	uint8_t plain[200];
	uint8_t cipher[200];
	uint8_t out[200];
	uint8_t block[64];
	uint8_t tag[16];
	uint8_t bad_tag[16];
	struct cryc_poly1305 mac;
	unsigned total;
	int error;

	total = 0;

	/* The block function of a secret key and nonce. */
	fill(key, sizeof(key), 3U);
	fill(nonce, sizeof(nonce), 7U);
	CT_SECRET(key, sizeof(key));
	CT_SECRET(nonce, sizeof(nonce));
	cryc_chacha20_block(key, 0xfffffffeU, nonce, block);
	CT_PUBLIC(block, sizeof(block));
	total += fold(block, sizeof(block));

	/* The cipher over several blocks and a partial one, in the 32-bit form. */
	fill(plain, sizeof(plain), 11U);
	CT_SECRET(plain, sizeof(plain));
	error = cryc_chacha20_xor(key, 1U, nonce, plain, out, 150U);
	CHECK(error == 0);
	CT_PUBLIC(out, 150U);
	total += fold(out, 150U);
	CT_PUBLIC(plain, sizeof(plain));

	/* The cipher in the 64-bit form with a counter across 2^32. */
	fill(nonce8, sizeof(nonce8), 21U);
	CT_SECRET(nonce8, sizeof(nonce8));
	fill(plain, sizeof(plain), 29U);
	CT_SECRET(plain, sizeof(plain));
	error = cryc_chacha20_xor64(key, CRYC_U64(1U, 0xffffffffU), nonce8, plain, out, 130U);
	CHECK(error == 0);
	CT_PUBLIC(out, 130U);
	total += fold(out, 130U);
	CT_PUBLIC(plain, sizeof(plain));
	CT_PUBLIC(nonce8, sizeof(nonce8));

	/* Poly1305 of a secret key and message, in pieces across block boundaries. */
	fill(block, 32U, 37U);
	CT_SECRET(block, 32U);
	fill(plain, sizeof(plain), 41U);
	CT_SECRET(plain, sizeof(plain));
	cryc_poly1305_init(&mac, block);
	cryc_poly1305_update(&mac, plain, 5U);
	cryc_poly1305_update(&mac, plain + 5, 40U);
	cryc_poly1305_update(&mac, plain + 45, 16U);
	cryc_poly1305_update(&mac, plain + 61, 1U);
	cryc_poly1305_final(&mac, tag);
	CT_PUBLIC(tag, sizeof(tag));
	total += fold(tag, sizeof(tag));
	CT_PUBLIC(plain, sizeof(plain));
	CT_PUBLIC(block, 32U);

	/* The AEAD seal of a secret message with secret additional data. */
	fill(key, sizeof(key), 43U);
	fill(nonce, sizeof(nonce), 47U);
	fill(aad, sizeof(aad), 53U);
	fill(plain, sizeof(plain), 59U);
	CT_SECRET(key, sizeof(key));
	CT_SECRET(nonce, sizeof(nonce));
	CT_SECRET(aad, sizeof(aad));
	CT_SECRET(plain, sizeof(plain));
	error = cryc_chacha20_poly1305_seal(key, nonce, aad, sizeof(aad), plain, 133U, cipher, tag);
	CHECK(error == 0);
	CT_PUBLIC(cipher, 133U);
	CT_PUBLIC(tag, sizeof(tag));
	total += fold(cipher, 133U);
	total += fold(tag, sizeof(tag));

	/* The open of a good tag: the return value is public after the call, the plaintext stays secret. */
	error = cryc_chacha20_poly1305_open(key, nonce, aad, sizeof(aad), cipher, 133U, out, tag);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == 0);
	CT_PUBLIC(out, 133U);
	total += fold(out, 133U);

	/* The open of a bad tag: the tag is compared with a mask, not a branch, and the output is zero. */
	memcpy(bad_tag, tag, sizeof(tag));
	bad_tag[3] = (uint8_t)(bad_tag[3] ^ 0x10U);
	error = cryc_chacha20_poly1305_open(key, nonce, aad, sizeof(aad), cipher, 133U, out, bad_tag);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == EBADMSG);
	CT_PUBLIC(out, 133U);
	total += fold(out, 133U);

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);

	/* The result. */
	return check_finish("ct-chacha");
}
