/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The elliptic-curve operations TLS and the public interface use on
 * P-256 and P-384: key-pair generation, ECDH (the ECC CDH primitive) and
 * ECDSA signature verification.  Built on cryc-ecp.h.
 *
 * Keys and signatures here are fixed-size big-endian byte strings:
 * a private key is ec->size bytes, a public key is the uncompressed point
 * (1 + 2 * ec->size bytes), r and s are ec->size bytes each.  The DER
 * form of an ECDSA signature (Ecdsa-Sig-Value) is decoded by
 * cryc-sigver.h.
 *
 * Constant time.  The private key and the shared secret are secret.
 * Verification handles only public values.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_ECC_H
#define LIBCRYPTO_COMPAT_CRYC_ECC_H

#include "cryc-ecp.h"

/*
 * The most candidates key generation draws before it gives up (FIPS 186-5
 * A.2.2 loops until a candidate is in range; for P-256 and P-384 a
 * candidate is rejected with a probability below 2^-32, so 64 draws
 * failing means the random source is broken).
 */
#define CRYC_ECC_KEYGEN_TRIES	64U

/*
 * Makes a key pair by rejection sampling (FIPS 186-5 A.2.2): c is
 * bits(n) random bits; c > n - 2 is rejected; d = c + 1; Q = d * G.
 *
 * private_key receives ec->size bytes, public_key 1 + 2 * ec->size bytes
 * (uncompressed).  Returns 0, or EIO when random failed or
 * CRYC_ECC_KEYGEN_TRIES candidates were rejected (both outputs wiped).
 * Constant time in d (the comparison with n - 2 is a mask; the number of
 * draws depends only on rejected candidates, which are discarded).
 */
int cryc_ecc_keygen(const struct cryc_ecp *ec, uint8_t *private_key, uint8_t *public_key, cryc_random_fn random, void *random_context);

/*
 * Computes the ECDH shared secret (NIST SP 800-56A Rev. 3 5.7.1.2 with
 * cofactor 1; RFC 8446 7.4.2; RFC 8422 5.10): the x coordinate of d * Q.
 *
 * private_key is ec->size bytes, 1 <= d <= n - 1 (EINVAL otherwise);
 * peer is the peer's uncompressed public key of peer_size bytes, decoded
 * and validated with cryc_ecp_decode (its errors are returned); shared
 * receives ec->size bytes.  A product at infinity (impossible for a valid
 * point and d) returns EINVAL.  On any error shared is wiped.
 *
 * Returns 0, EINVAL or ENOTSUP.  Constant time in d.
 */
int cryc_ecc_ecdh(const struct cryc_ecp *ec, const uint8_t *private_key, const uint8_t *peer, size_t peer_size, uint8_t *shared);

/*
 * Verifies an ECDSA signature (FIPS 186-5 6.4.2) over a message whose
 * hash is given.
 *
 * public_key is the uncompressed point of public_key_size bytes (decoded
 * and validated with cryc_ecp_decode).  hash is hash_size bytes (any
 * size, 1 or more): step 3's E is its leftmost bits(n) bits (B.2.1), and
 * E mod n is formed with one conditional subtraction (cryc_bn_reduce_once
 * on the leftmost bits).  r and s are ec->size big-endian bytes each.
 *
 * Returns 0 when the signature verifies; EBADMSG when it does not,
 * including r or s outside [1, n - 1] (step 1) and R at infinity
 * (step 6); EINVAL or ENOTSUP for a public key cryc_ecp_decode refuses;
 * EINVAL for a hash_size of 0.  Variable time (public inputs).
 */
int cryc_ecdsa_verify(const struct cryc_ecp *ec, const uint8_t *public_key, size_t public_key_size, const uint8_t *hash, size_t hash_size, const uint8_t *r, const uint8_t *s);

#endif
