/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The polynomial arithmetic, encodings and sampling of ML-KEM (FIPS 203
 * 4).
 */

#include "cryc-mlkem.h"

/*
 * NTT(f) in place (Algorithm 9).
 */
void
cryc_mlkem_ntt(
	struct cryc_mlkem_poly *f)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(f);
}

/*
 * NTT^-1(f) in place (Algorithm 10).
 */
void
cryc_mlkem_ntt_inverse(
	struct cryc_mlkem_poly *f)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(f);
}

/*
 * h = f * g in T_q (MultiplyNTTs, Algorithm 11, with BaseCaseMultiply,
 * Algorithm 12).
 */
void
cryc_mlkem_multiply_ntts(
	struct cryc_mlkem_poly *h,
	const struct cryc_mlkem_poly *f,
	const struct cryc_mlkem_poly *g)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(h);
	UNUSED_PARAMETER(f);
	UNUSED_PARAMETER(g);
}

/*
 * Writes ByteEncode_d(f) (Algorithm 5), 32 * d bytes, for d from 1 to 12.
 */
void
cryc_mlkem_byte_encode(
	uint8_t *out,
	const struct cryc_mlkem_poly *f,
	unsigned d)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(f);
	UNUSED_PARAMETER(d);
}

/*
 * Reads ByteDecode_d (Algorithm 6) from 32 * d bytes.
 */
int
cryc_mlkem_byte_decode(
	struct cryc_mlkem_poly *f,
	const uint8_t *in,
	unsigned d)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(f);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(d);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Compress_d of every coefficient in place (4.2.1, equation (4.7)), d
 * below 12.
 */
void
cryc_mlkem_compress(
	struct cryc_mlkem_poly *f,
	unsigned d)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(f);
	UNUSED_PARAMETER(d);
}

/*
 * Decompress_d of every coefficient in place (4.2.1, equation (4.8)), d
 * below 12.
 */
void
cryc_mlkem_decompress(
	struct cryc_mlkem_poly *f,
	unsigned d)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(f);
	UNUSED_PARAMETER(d);
}

/*
 * SampleNTT (Algorithm 7) from a 34-byte input rho || j || i, using
 * SHAKE128 as the XOF.
 */
void
cryc_mlkem_sample_ntt(
	struct cryc_mlkem_poly *f,
	const uint8_t *seed34)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(f);
	UNUSED_PARAMETER(seed34);
}

/*
 * SamplePolyCBD_eta (Algorithm 8) from 64 * eta bytes, eta 2 (ML-KEM-768's
 * eta1 and eta2).
 */
void
cryc_mlkem_sample_cbd(
	struct cryc_mlkem_poly *f,
	const uint8_t *bytes,
	unsigned eta)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(f);
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(eta);
}
