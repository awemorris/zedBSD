/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the ML-KEM polynomial functions: the
 * coefficients and bytes that stand for secrets (the secret vector, the
 * noise, the message polynomial) are marked secret, so memcheck reports
 * any branch or address that depends on their values (the bit width d,
 * the loop bounds and the seed of SampleNTT, which is public, may be
 * used freely).
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-mlkem.h"

/* Fills a polynomial with a fixed pattern of values in [0, q - 1]. */
static void
fill_poly(
	struct cryc_mlkem_poly *f,
	unsigned seed)
{
	unsigned index;

	/* A spread of values, including 0 and q - 1. */
	for (index = 0; index < CRYC_MLKEM_N; index++)
		f->c[index] = (int16_t)((seed + index * 1291U + (index >> 2) * 37U) % 3329U);
	f->c[0] = 0;
	f->c[1] = 3328;
}

/* Folds a public polynomial into one value, to keep the call from being dropped. */
static unsigned
fold(
	const struct cryc_mlkem_poly *f)
{
	unsigned value;
	unsigned index;

	/* Exclusive-or of the coefficients, rotated. */
	value = 0U;
	for (index = 0; index < CRYC_MLKEM_N; index++)
		value = ((value << 1) | (value >> 15)) ^ (unsigned)(uint16_t)f->c[index];
	return value & 0xffffU;
}

/* Runs the checks. */
int
main(void)
{
	struct cryc_mlkem_poly f;
	struct cryc_mlkem_poly g;
	struct cryc_mlkem_poly h;
	uint8_t bytes[384];
	uint8_t stream[192];
	unsigned total;
	unsigned d;
	unsigned index;
	int clean;

	total = 0U;

	/* NTT of a secret polynomial. */
	fill_poly(&f, 3U);
	CT_SECRET(&f, sizeof(f));
	cryc_mlkem_ntt(&f);
	CT_PUBLIC(&f, sizeof(f));
	total += fold(&f);

	/* NTT^-1 of a secret polynomial. */
	fill_poly(&f, 7U);
	CT_SECRET(&f, sizeof(f));
	cryc_mlkem_ntt_inverse(&f);
	CT_PUBLIC(&f, sizeof(f));
	total += fold(&f);

	/* The product of two secret polynomials, and of a secret one with itself in place. */
	fill_poly(&f, 11U);
	fill_poly(&g, 13U);
	CT_SECRET(&f, sizeof(f));
	CT_SECRET(&g, sizeof(g));
	cryc_mlkem_multiply_ntts(&h, &f, &g);
	CT_PUBLIC(&h, sizeof(h));
	total += fold(&h);
	cryc_mlkem_multiply_ntts(&f, &f, &f);
	CT_PUBLIC(&f, sizeof(f));
	total += fold(&f);
	CT_PUBLIC(&g, sizeof(g));

	/* Compress and Decompress of secret coefficients, for every width ML-KEM uses and the others in range. */
	for (d = 1U; d <= 11U; d++) {
		fill_poly(&f, 17U + d);
		CT_SECRET(&f, sizeof(f));
		cryc_mlkem_compress(&f, d);
		cryc_mlkem_decompress(&f, d);
		CT_PUBLIC(&f, sizeof(f));
		total += fold(&f);
	}

	/* ByteEncode of a secret polynomial and ByteDecode of secret bytes, for every width. */
	for (d = 1U; d <= 12U; d++) {
		fill_poly(&f, 19U + d);
		for (index = 0; index < CRYC_MLKEM_N; index++)
			f.c[index] = (int16_t)((unsigned)f.c[index] & ((1U << d) - 1U));
		memset(bytes, 0, sizeof(bytes));
		CT_SECRET(&f, sizeof(f));
		cryc_mlkem_byte_encode(bytes, &f, d);
		CT_PUBLIC(bytes, 32U * d);
		total += bytes[0] + bytes[32U * d - 1U];
		CT_PUBLIC(&f, sizeof(f));

		/* The decoded values and the clean flag derive from the secret bytes; both are made public here only to read them. */
		CT_SECRET(bytes, 32U * d);
		clean = cryc_mlkem_byte_decode(&g, bytes, d);
		CT_PUBLIC(&clean, sizeof(clean));
		CT_PUBLIC(&g, sizeof(g));
		CT_PUBLIC(bytes, 32U * d);
		total += fold(&g) + (unsigned)clean;
	}

	/* SamplePolyCBD of secret bytes for both values of eta. */
	for (d = 2U; d <= 3U; d++) {
		for (index = 0; index < sizeof(stream); index++)
			stream[index] = (uint8_t)(index * 29U + d * 7U + (index >> 3));
		CT_SECRET(stream, 64U * d);
		cryc_mlkem_sample_cbd(&f, stream, d);
		CT_PUBLIC(&f, sizeof(f));
		CT_PUBLIC(stream, 64U * d);
		total += fold(&f);
	}

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 100000000U);

	/* The result. */
	return check_finish("ct-mlkem-poly");
}
