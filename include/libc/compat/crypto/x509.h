/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libcrypto-compat's certificates and their validation, with OpenSSL's
 * interface: the X509 object, the trust store, the verification
 * parameters, and the verification result codes (X509_V_*).
 *
 * The subset: making a certificate from DER and adding it to a store,
 * the host and address a connection must match, and the text of a
 * verification result.
 */

#ifndef KERN_COMPAT_CRYPTO_X509_H
#define KERN_COMPAT_CRYPTO_X509_H

#include <compat/crypto/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The verification results this library reports (a subset of OpenSSL's;
 * SSL_get_verify_result returns one of them).
 * Values referenced: OpenSSL 3.5.8 include/openssl/x509_vfy.h.in
 * (X509_V_OK, X509_V_ERR_*).
 */
#define X509_V_OK					0
#define X509_V_ERR_UNSPECIFIED				1
#define X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT		2
#define X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY	6
#define X509_V_ERR_CERT_SIGNATURE_FAILURE		7
#define X509_V_ERR_CERT_NOT_YET_VALID			9
#define X509_V_ERR_CERT_HAS_EXPIRED			10
#define X509_V_ERR_OUT_OF_MEM				17
#define X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT		18
#define X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN		19
#define X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY	20
#define X509_V_ERR_CERT_CHAIN_TOO_LONG			22
#define X509_V_ERR_PATH_LENGTH_EXCEEDED			25
#define X509_V_ERR_INVALID_PURPOSE			26
#define X509_V_ERR_KEYUSAGE_NO_CERTSIGN			32
#define X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION		34
#define X509_V_ERR_INVALID_CA				79
#define X509_V_ERR_KEYUSAGE_NO_DIGITAL_SIGNATURE	39
#define X509_V_ERR_PERMITTED_VIOLATION			47
#define X509_V_ERR_EXCLUDED_VIOLATION			48
#define X509_V_ERR_UNSUPPORTED_NAME_SYNTAX		53
#define X509_V_ERR_HOSTNAME_MISMATCH			62
#define X509_V_ERR_IP_ADDRESS_MISMATCH			64
#define X509_V_ERR_EE_KEY_TOO_SMALL			66
#define X509_V_ERR_UNSUPPORTED_SIGNATURE_ALGORITHM	76

/*
 * Makes a certificate from DER (OpenSSL's d2i convention): reads one
 * Certificate from the first bytes of *in (at most length bytes; bytes
 * after the Certificate's own DER length are left unread, not an error),
 * advances *in past it and returns a new X509 with one reference, stored
 * also in *out when out is not NULL (an existing *out is freed first).
 * On an error returns NULL with an error queued, leaves *in unchanged,
 * and when out is not NULL frees *out and sets it to NULL (d2i_X509.pod
 * of OpenSSL 3): malformed DER, a certificate libcrypto-compat cannot
 * parse, or a length that is not positive.
 */
X509 *d2i_X509(X509 **out, const unsigned char **in, long length);

/* Drops one reference to a certificate and frees it at the last; NULL is ignored. */
void X509_free(X509 *certificate);

/*
 * Adds a certificate to a store as a trust anchor (the store takes its
 * own copy; the caller still frees certificate).  Returns 1, or 0 on
 * failure with an error queued.  Adding a certificate already present
 * succeeds.
 */
int X509_STORE_add_cert(X509_STORE *store, X509 *certificate);

/*
 * Sets the host name a validation must match (RFC 9525 DNS-ID; no
 * common-name fallback).  name is length bytes, or a C string when length
 * is 0; NULL clears the setting.  Returns 1, or 0 for a name that is not
 * a valid host name.
 */
int X509_VERIFY_PARAM_set1_host(X509_VERIFY_PARAM *param, const char *name, size_t length);

/*
 * Sets the IP address a validation must match, from its text: a dotted
 * IPv4 address or an IPv6 address (RFC 4291 2.2 forms, without brackets
 * or zone).  Returns 1, or 0 for a text that is not an address.
 */
int X509_VERIFY_PARAM_set1_ip_asc(X509_VERIFY_PARAM *param, const char *address);

/*
 * Returns a constant English text for a verification result (an
 * unknown value gives a generic text, never NULL).
 */
const char *X509_verify_cert_error_string(long result);

#ifdef __cplusplus
}
#endif

#endif
