/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Ed25519 signatures (RFC 8032, January 2017, section 5.1; pure Ed25519,
 * not Ed25519ctx or Ed25519ph): key generation from a 32-byte seed,
 * signing and verification.
 *
 * The TLS client does not use Ed25519 (no server of the web PKI signs
 * with it, and the client signs nothing); the in-house ssh and sshd use it
 * for host and user keys through the public EVP_PKEY interface
 * (EVP_PKEY_ED25519).  It is built on cryc-f25519.h and cryc-sha2.h
 * (SHA-512); the point arithmetic is in extended twisted Edwards
 * coordinates (RFC 8032 5.1.4).
 *
 * Constant time: the seed, the private scalar, the nonce r and the
 * signing arithmetic are secret (the fixed-base multiplication reads its
 * table by scanning every entry and selecting with masks).  Verification
 * handles only public values.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_ED25519_H
#define LIBCRYPTO_COMPAT_CRYC_ED25519_H

#include "cryc-f25519.h"
#include "cryc-sha2.h"

/* The sizes in bytes (RFC 8032 5.1.5, 5.1.6). */
#define CRYC_ED25519_SEED	32U
#define CRYC_ED25519_PUBLIC	32U
#define CRYC_ED25519_SIGNATURE	64U

/*
 * Computes the public key of a 32-byte private key (seed) (5.1.5).
 *
 * Returns 0.
 */
int cryc_ed25519_public(uint8_t *public_key, const uint8_t *seed);

/*
 * Signs message (message_size bytes, may be 0) with the private key seed
 * and its public key (5.1.6), writing 64 bytes to signature.
 *
 * public_key must be the seed's public key (the caller keeps them
 * together; a mismatched pair gives a signature that does not verify,
 * and leaks nothing more).  Deterministic (the nonce is derived from the
 * seed's hash and the message, 5.1.6 step 2); needs no random source.
 * Returns 0.
 */
int cryc_ed25519_sign(uint8_t *signature, const uint8_t *message, size_t message_size, const uint8_t *seed, const uint8_t *public_key);

/*
 * Verifies a signature (5.1.7) with the cofactorless equation [S]B = R +
 * [k]A' of step 3, rejecting S not less than L (step 1) and a public key
 * or R that does not decode (5.1.3).
 *
 * Returns 0 when it verifies; EBADMSG otherwise.
 */
int cryc_ed25519_verify(const uint8_t *signature, const uint8_t *message, size_t message_size, const uint8_t *public_key);

#endif
