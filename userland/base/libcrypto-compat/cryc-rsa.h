/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * RSA signature verification: RSASSA-PKCS1-v1_5 and RSASSA-PSS with
 * MGF1, the RSA signatures of certificates and of TLS handshakes.
 *
 * Only the public-key operation is here: libssl-compat never holds an RSA
 * private key (no client certificates, no RSA key exchange), so every
 * value is public and the code may branch on it.  Still, padding is
 * checked by building the expected encoding and comparing it whole
 * (PKCS1-v1_5, 8.2.2 step 4 "compare"), never by parsing the decoded
 * block, which closes the class of forgeries that come from lax parsing of
 * the DigestInfo.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_RSA_H
#define LIBCRYPTO_COMPAT_CRYC_RSA_H

#include "cryc-bn.h"
#include "cryc-hash.h"

/*
 * The section numbers in the comments of this file are those of RFC 8017
 * (PKCS #1 v2.2, November 2016).
 */

/*
 * The smallest modulus accepted, in bits.  RFC 9325 4.5 (and the CA/B
 * Forum's baseline) make 2048 the floor; a smaller key is refused as
 * ENOTSUP rather than verified.
 */
#define CRYC_RSA_MIN_BITS	2048U

/*
 * An RSA public key (n, e), borrowed from a parsed SubjectPublicKeyInfo
 * (cryc-sigver.h): the big-endian bytes of n and of e as they appear in
 * the RSAPublicKey (A.1.1) with the DER sign byte removed.  The owner of
 * the certificate keeps the bytes alive while the key is used.
 */
struct cryc_rsa_key {
	const uint8_t *n;
	size_t n_size;
	const uint8_t *e;
	size_t e_size;
};

/*
 * Verifies an RSASSA-PKCS1-v1_5 signature (8.2.2) with hash hash_id over
 * a message whose hash is given.
 *
 * key: n of CRYC_RSA_MIN_BITS to CRYC_BN_MAX_BITS bits (ENOTSUP outside),
 * odd (EINVAL otherwise); e odd, at least 3 and at most 2^32 - 1 (EINVAL
 * otherwise).  hash is hash_size bytes, which must equal
 * cryc_hash_size(hash_id) (EINVAL otherwise); hash_id is SHA-256, SHA-384
 * or SHA-512 (ENOTSUP for others).  signature is
 * signature_size bytes, which must equal the byte length of n (8.2.2
 * step 1; EBADMSG otherwise).
 *
 * Steps: s as an integer must be less than n (RSAVP1, 5.2.2; EBADMSG
 * otherwise); m = s^e mod n; EM = I2OSP(m, k); EM' = EMSA-PKCS1-v1_5-
 * ENCODE(hash) (9.2, with the DigestInfo prefixes of 9.2 note 1, cited as
 * data at their table in the .c file, and only the form with the NULL
 * parameters); EM must equal EM' byte for byte.
 *
 * Returns 0, EBADMSG, EINVAL or ENOTSUP.
 */
int cryc_rsa_pkcs1_verify(const struct cryc_rsa_key *key, enum cryc_hash_id hash_id, const uint8_t *hash, size_t hash_size, const uint8_t *signature, size_t signature_size);

/*
 * Verifies an RSASSA-PSS signature (8.1.2, EMSA-PSS-VERIFY of 9.1.2) with
 * hash hash_id, MGF1 (B.2.1) with the same hash, and a salt of salt_size
 * bytes.
 *
 * Arguments and key checks as cryc_rsa_pkcs1_verify; salt_size is the
 * salt length the caller requires (TLS 1.3 4.2.3 and RFC 8446's
 * rsa_pss_rsae_* schemes require the hash length; an X.509 RSASSA-PSS
 * AlgorithmIdentifier names its own, RFC 4055 3.1).  The decoded salt
 * length must equal salt_size exactly (9.1.2 step 10's padding check
 * fixes it).  emBits = modBits - 1 (8.1.2 step 2).
 *
 * Returns 0, EBADMSG, EINVAL or ENOTSUP.
 */
int cryc_rsa_pss_verify(const struct cryc_rsa_key *key, enum cryc_hash_id hash_id, const uint8_t *hash, size_t hash_size, size_t salt_size, const uint8_t *signature, size_t signature_size);

#endif
