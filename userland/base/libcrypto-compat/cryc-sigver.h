/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The verification of a signature by a public key, whatever the
 * algorithm: of a certificate by its issuer, of a TLS 1.3
 * CertificateVerify and of a TLS 1.2 ServerKeyExchange by the server's
 * key.  It joins cryc-x509.h (the keys and the algorithms) to cryc-rsa.h
 * and cryc-ecc.h (the primitives).
 *
 * Every value is public; the functions may branch.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_SIGVER_H
#define LIBCRYPTO_COMPAT_CRYC_SIGVER_H

#include "cryc-ecc.h"
#include "cryc-x509.h"

/*
 * Decodes an ECDSA signature in its DER form, Ecdsa-Sig-Value (RFC 5480
 * 2.2 via RFC 3279 2.2.3; the same form TLS uses, RFC 8446 4.2.3 and RFC
 * 8422 5.4): SEQUENCE { r INTEGER, s INTEGER }, nothing after it.
 *
 * r and s are written as size big-endian bytes each (left-padded with
 * zeros).  Returns 0, or EBADMSG when the encoding is not strict DER, a
 * value is negative or zero, or a value has more than size significant
 * bytes (a malformed signature never verifies; EBADMSG keeps the
 * caller's mapping to an alert simple).
 */
int cryc_ecdsa_signature_decode(const uint8_t *der, size_t der_size, size_t size, uint8_t *r, uint8_t *s);

/*
 * Verifies signature over message with key under params.
 *
 * The message is hashed here with the algorithm's hash (a whole
 * tbsCertificate, or a TLS signed content).  The key must suit the
 * algorithm: RSA for RSA-PKCS1 and RSA-PSS (with key->rsa's size limits
 * of cryc-rsa.h); EC for ECDSA, any of P-256 and P-384 with any of the
 * ECDSA hashes (X.509 allows the combinations; TLS 1.3's restriction of
 * the curve to the scheme is checked by the caller, libssl-compat's
 * handshake).  PSS
 * uses params->salt_size.
 *
 * Returns 0 when it verifies; EBADMSG when it does not; EINVAL when the
 * key does not suit the algorithm or is malformed (an EC point not on the
 * curve); ENOTSUP for CRYC_SIG_UNSUPPORTED, CRYC_KEY_UNSUPPORTED, or an
 * RSA key outside the size limits.
 */
int cryc_signature_verify(const struct cryc_pubkey *key, const struct cryc_sig_params *params, const uint8_t *message, size_t message_size, const uint8_t *signature, size_t signature_size);

#endif
