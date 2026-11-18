/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * X25519, the Diffie-Hellman function on Curve25519 (RFC 7748), the key
 * exchange TLS names x25519 (RFC 8446 4.2.7 and 7.4.2; RFC 8422 for TLS
 * 1.2).
 *
 * Constant time.  The scalar and the result are secret.  The ladder of
 * section 5 is run for all 255 bits with conditional swaps done by masks
 * (cswap, section 5); the field arithmetic is cryc-f25519.h's (shared
 * with Ed25519), and the inversion is its fixed exponentiation to p - 2.
 * The
 * u-coordinate's top bit is masked off and non-canonical values are
 * accepted, as section 5 requires.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_X25519_H
#define LIBCRYPTO_COMPAT_CRYC_X25519_H

#include "cryc-f25519.h"

/*
 * The section numbers in the comments of this file are those of RFC 7748
 * (January 2016).
 */

/* The size in bytes of a scalar, a u-coordinate and a shared secret (5). */
#define CRYC_X25519_SIZE	32U

/*
 * Computes X25519(scalar, u) (section 5) into out (32 bytes).
 *
 * scalar is 32 bytes, decoded with decodeScalar25519 (the clamping of
 * section 5); u is 32 bytes, decoded with decodeUCoordinate.  out may not
 * overlap scalar.  When the result is all zeros (u was a point of small
 * order), out is still written and EINVAL is returned: RFC 8446 7.4.2 and
 * RFC 7748 6.1 require the caller to abort then.  The zero test is done
 * in constant time over the 32 bytes.
 *
 * Returns 0 or EINVAL.
 */
int cryc_x25519(uint8_t *out, const uint8_t *scalar, const uint8_t *u);

/*
 * Computes the public key X25519(scalar, 9) (section 6.1) into out.
 *
 * Constant time.  (The base point gives a non-zero result for every
 * clamped scalar, so there is no error.)
 */
void cryc_x25519_public(uint8_t *out, const uint8_t *scalar);

/*
 * Makes a key pair: 32 random bytes from random as the private scalar,
 * and its public key.
 *
 * Returns 0, or EIO when random failed (private_key and public_key are
 * then wiped).
 */
int cryc_x25519_keygen(uint8_t *private_key, uint8_t *public_key, cryc_random_fn random, void *random_context);

#endif
