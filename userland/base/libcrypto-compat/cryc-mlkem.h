/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ML-KEM-768, the module-lattice key-encapsulation mechanism of FIPS 203
 * (August 2024), for the hybrid key exchange of the in-house ssh
 * (mlkem768x25519-sha256) through the public EVP_PKEY interface.
 *
 * Module split inside the .c files: cryc-mlkem-poly.c holds the
 * arithmetic of R_q and T_q (reductions, NTT, NTT^-1, MultiplyNTTs,
 * BaseCaseMultiply, Algorithms 9 to 12), ByteEncode and ByteDecode
 * (Algorithms 5 and 6), Compress and Decompress (4.2.1), SampleNTT and
 * SamplePolyCBD (Algorithms 7 and 8); cryc-mlkem.c holds K-PKE (5,
 * Algorithms 13 to 15) and ML-KEM (6 and 7, Algorithms 16 to 21).  The
 * polynomial functions are declared here because the unit tests check
 * them alone.
 *
 * Constant time.  The seeds d and z, the message m, the shared key K,
 * the decapsulation key and every polynomial derived from them are
 * secret: reductions modulo q use multiplication and masks (no % and no
 * / on a secret; a Barrett or Montgomery reduction derived by the
 * implementer), Compress uses multiplication and a shift, and the
 * re-encryption check of decapsulation (Algorithm 18 lines 9 to 11)
 * compares c and c' in constant time and selects K or K-bar with a mask
 * (implicit rejection: a wrong ciphertext gives a pseudo-random key, not
 * an error).  SampleNTT reads public seeds (rho) and may branch on its
 * rejection test.
 *
 * Input checks (7.2, 7.3): an encapsulation key whose 12-bit coefficients
 * are not all below q is refused (ByteEncode12(ByteDecode12(ek)) must
 * equal ek); a decapsulation key's hash check (7.3 item 3) is done on
 * import.  Sizes are checked by the callers (the public layer).
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_MLKEM_H
#define LIBCRYPTO_COMPAT_CRYC_MLKEM_H

#include "cryc-sha3.h"

/*
 * The section and algorithm numbers in the comments of this file are
 * those of FIPS 203.  The parameter set is ML-KEM-768 (Table 2: k = 3,
 * eta1 = 2, eta2 = 2, du = 10, dv = 4); q = 3329, n = 256.  The NTT's
 * zeta values (4.3, Appendix A) are data cited at their table in the .c
 * file.
 */

/* The sizes in bytes of ML-KEM-768 (Table 3). */
#define CRYC_MLKEM768_EK	1184U
#define CRYC_MLKEM768_DK	2400U
#define CRYC_MLKEM768_CT	1088U
#define CRYC_MLKEM_SHARED	32U
#define CRYC_MLKEM_SEED		32U

/* The modulus and the degree (2.3). */
#define CRYC_MLKEM_Q		3329
#define CRYC_MLKEM_N		256U

/*
 * A polynomial of R_q or T_q: 256 coefficients.  Between functions every
 * coefficient is in [0, q - 1] unless a function's comment says
 * otherwise.
 */
struct cryc_mlkem_poly {
	int16_t c[CRYC_MLKEM_N];
};

/* NTT(f) in place (Algorithm 9).  Constant time. */
void cryc_mlkem_ntt(struct cryc_mlkem_poly *f);

/* NTT^-1(f) in place (Algorithm 10).  Constant time. */
void cryc_mlkem_ntt_inverse(struct cryc_mlkem_poly *f);

/* h = f * g in T_q (MultiplyNTTs, Algorithm 11, with BaseCaseMultiply, Algorithm 12).  h may alias f or g.  Constant time. */
void cryc_mlkem_multiply_ntts(struct cryc_mlkem_poly *h, const struct cryc_mlkem_poly *f, const struct cryc_mlkem_poly *g);

/* Writes ByteEncode_d(f) (Algorithm 5), 32 * d bytes, for d from 1 to 12. */
void cryc_mlkem_byte_encode(uint8_t *out, const struct cryc_mlkem_poly *f, unsigned d);

/*
 * Reads ByteDecode_d (Algorithm 6) from 32 * d bytes.  For d = 12 the
 * values are reduced modulo q (Algorithm 6's m = q for d = 12); the
 * return value is 1 when every value read was already below q (the
 * encapsulation key check of 7.2), else 0.
 */
int cryc_mlkem_byte_decode(struct cryc_mlkem_poly *f, const uint8_t *in, unsigned d);

/* Compress_d of every coefficient in place (4.2.1, equation (4.7)), d below 12. */
void cryc_mlkem_compress(struct cryc_mlkem_poly *f, unsigned d);

/* Decompress_d of every coefficient in place (4.2.1, equation (4.8)), d below 12. */
void cryc_mlkem_decompress(struct cryc_mlkem_poly *f, unsigned d);

/*
 * SampleNTT (Algorithm 7) from a 34-byte input rho || j || i, using
 * SHAKE128 as the XOF.  Rejection sampling on public data.
 */
void cryc_mlkem_sample_ntt(struct cryc_mlkem_poly *f, const uint8_t *seed34);

/* SamplePolyCBD_eta (Algorithm 8) from 64 * eta bytes, eta 2 (ML-KEM-768's eta1 and eta2).  Constant time. */
void cryc_mlkem_sample_cbd(struct cryc_mlkem_poly *f, const uint8_t *bytes, unsigned eta);

/*
 * ML-KEM.KeyGen_internal (Algorithm 16) from the seeds d and z (32
 * bytes each): writes ek (1184 bytes) and dk (2400 bytes).  Constant
 * time in d and z.
 */
void cryc_mlkem768_keygen_internal(uint8_t *ek, uint8_t *dk, const uint8_t *d, const uint8_t *z);

/*
 * ML-KEM.KeyGen (Algorithm 19): d and z from random, then
 * KeyGen_internal.  Returns 0, or EIO when random failed (outputs wiped).
 */
int cryc_mlkem768_keygen(uint8_t *ek, uint8_t *dk, cryc_random_fn random, void *random_context);

/*
 * Checks an encapsulation key (7.2 "modulus check": every 12-bit value
 * below q).  Returns 0 or EINVAL.  Variable time (public).
 */
int cryc_mlkem768_check_ek(const uint8_t *ek);

/*
 * Checks a decapsulation key (7.3 "hash check": H(ek part) equals the
 * stored hash).  Returns 0 or EINVAL.  Constant time in dk.
 */
int cryc_mlkem768_check_dk(const uint8_t *dk);

/*
 * ML-KEM.Encaps_internal (Algorithm 17) with the message m (32 bytes):
 * writes the shared key (32 bytes) and the ciphertext (1088 bytes).  ek
 * must have passed cryc_mlkem768_check_ek.  Constant time in m.
 */
void cryc_mlkem768_encaps_internal(uint8_t *shared, uint8_t *ciphertext, const uint8_t *ek, const uint8_t *m);

/*
 * ML-KEM.Encaps (Algorithm 20): m from random, then Encaps_internal.
 * Returns 0, EINVAL (ek fails the check) or EIO (random failed; outputs
 * wiped).
 */
int cryc_mlkem768_encaps(uint8_t *shared, uint8_t *ciphertext, const uint8_t *ek, cryc_random_fn random, void *random_context);

/*
 * ML-KEM.Decaps (Algorithm 21 with Decaps_internal, Algorithm 18):
 * writes the shared key (32 bytes) for a 1088-byte ciphertext and a
 * 2400-byte dk that passed cryc_mlkem768_check_dk.  Never fails: a
 * ciphertext that does not re-encrypt to itself gives the implicit
 * rejection key J(z || c).  Constant time in dk and in whether c is
 * valid.
 */
void cryc_mlkem768_decaps(uint8_t *shared, const uint8_t *ciphertext, const uint8_t *dk);

#endif
