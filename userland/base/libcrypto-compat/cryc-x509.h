/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parsing of X.509 certificates (RFC 5280, May 2008, section 4), of
 * the public keys in them (RFC 3279 2.3.1, RFC 5480 2, RFC 8017 A.1.1)
 * and of signature AlgorithmIdentifiers (RFC 4055 3 and 5, RFC 5758 3.2,
 * RFC 8017 A.2).
 *
 * A parsed certificate is a set of views into the DER bytes it was parsed
 * from; nothing is copied.  The owner of the bytes (the trust store, the
 * received chain, a public X509 object) keeps them alive and unchanged
 * while the parsed form is used.
 *
 * Parsing checks the syntax strictly and records what path validation
 * needs; it does not decide trust.  A certificate whose syntax is valid
 * but which uses a key or a signature algorithm outside libssl-compat's
 * range parses successfully with CRYC_KEY_UNSUPPORTED or
 * CRYC_SIG_UNSUPPORTED, so that a CA bundle with such roots still loads;
 * validation refuses to use them.
 *
 * Extensions (RFC 5280 4.2).  Processed: basicConstraints, keyUsage,
 * extKeyUsage, subjectAltName, nameConstraints, subjectKeyIdentifier,
 * authorityKeyIdentifier.  Recognized and not processed even when marked
 * critical: certificatePolicies, policyMappings, policyConstraints,
 * inhibitAnyPolicy (libssl-compat does no policy processing, RFC 5280
 * 6.1 with user-initial-policy-set anyPolicy and no explicit policy
 * required; the same choice as the common web PKI verifiers).  Any other
 * extension is ignored when not critical; a critical one sets
 * CRYC_X509_UNKNOWN_CRITICAL, which validation refuses (6.1.4 (o)).  A
 * repeated extension is EINVAL (4.2).  A processed extension whose value
 * does not parse is EINVAL.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_X509_H
#define LIBCRYPTO_COMPAT_CRYC_X509_H

#include "cryc-ecp.h"
#include "cryc-hash.h"
#include "cryc-rsa.h"

/* The largest serial number accepted in bytes (RFC 5280 4.1.2.2: at most 20 octets, plus a sign octet). */
#define CRYC_X509_SERIAL_MAX	21U

/* The kinds of public key. */
enum cryc_key_type {
	CRYC_KEY_NONE = 0,
	CRYC_KEY_UNSUPPORTED = 1,
	CRYC_KEY_RSA = 2,
	CRYC_KEY_EC = 3
};

/*
 * The signature algorithms, of certificates and of TLS.  RSA-PSS uses
 * MGF1 with the same hash.
 */
enum cryc_sig_alg {
	CRYC_SIG_NONE = 0,
	CRYC_SIG_UNSUPPORTED = 1,
	CRYC_SIG_RSA_PKCS1_SHA256 = 2,
	CRYC_SIG_RSA_PKCS1_SHA384 = 3,
	CRYC_SIG_RSA_PKCS1_SHA512 = 4,
	CRYC_SIG_RSA_PSS_SHA256 = 5,
	CRYC_SIG_RSA_PSS_SHA384 = 6,
	CRYC_SIG_RSA_PSS_SHA512 = 7,
	CRYC_SIG_ECDSA_SHA256 = 8,
	CRYC_SIG_ECDSA_SHA384 = 9,
	CRYC_SIG_ECDSA_SHA512 = 10
};

/* The bits of flags in struct cryc_x509. */
#define CRYC_X509_HAS_BASIC_CONSTRAINTS	0x0001U
#define CRYC_X509_CA			0x0002U
#define CRYC_X509_HAS_PATH_LEN		0x0004U
#define CRYC_X509_HAS_KEY_USAGE		0x0008U
#define CRYC_X509_HAS_EXT_KEY_USAGE	0x0010U
#define CRYC_X509_HAS_SAN		0x0020U
#define CRYC_X509_HAS_NAME_CONSTRAINTS	0x0040U
#define CRYC_X509_SELF_ISSUED		0x0080U
#define CRYC_X509_UNKNOWN_CRITICAL	0x0100U
#define CRYC_X509_SAN_CRITICAL		0x0200U

/*
 * The keyUsage bits (RFC 5280 4.2.1.3), as bit i of key_usage for
 * KeyUsage bit i.
 */
#define CRYC_KU_DIGITAL_SIGNATURE	0x0001U
#define CRYC_KU_KEY_ENCIPHERMENT	0x0004U
#define CRYC_KU_KEY_AGREEMENT		0x0010U
#define CRYC_KU_KEY_CERT_SIGN		0x0020U
#define CRYC_KU_CRL_SIGN		0x0040U

/* The extKeyUsage purposes recorded (RFC 5280 4.2.1.12). */
#define CRYC_EKU_SERVER_AUTH		0x0001U
#define CRYC_EKU_CLIENT_AUTH		0x0002U
#define CRYC_EKU_ANY			0x0004U
#define CRYC_EKU_OTHER			0x0008U

/*
 * A signature algorithm with its parameters: alg, and for RSA-PSS the
 * salt length its AlgorithmIdentifier names (RFC 4055 3.1).
 */
struct cryc_sig_params {
	enum cryc_sig_alg alg;
	size_t salt_size;
};

/*
 * A public key: its type; for CRYC_KEY_RSA the key's n and e (views);
 * for CRYC_KEY_EC its curve and the uncompressed point (a view, 1 + 2 *
 * field size bytes, not yet validated on the curve).
 */
struct cryc_pubkey {
	enum cryc_key_type type;
	struct cryc_rsa_key rsa;
	enum cryc_curve_id curve;
	const uint8_t *point;
	size_t point_size;
};

/*
 * A parsed certificate (RFC 5280 4.1).  Every pointer is a view into
 * the bytes given to cryc_x509_parse.
 *
 *   der, der_size            The whole Certificate.
 *   tbs, tbs_size            The whole tbsCertificate encoding (signed).
 *   signature_alg            Certificate.signatureAlgorithm; parsing
 *                            requires tbsCertificate.signature to be the
 *                            same bytes (4.1.1.2).
 *   signature, signature_size  The signatureValue BIT STRING's bytes
 *                            (unused bits must be 0).
 *   version                  1, 2 or 3.
 *   serial, serial_size      The serialNumber INTEGER's contents (sign
 *                            octet included; a negative or zero serial is
 *                            accepted, as 4.1.2.2 asks receivers to be
 *                            tolerant).
 *   issuer, subject          The whole Name encodings (SEQUENCE).
 *   not_before, not_after    The validity, seconds since 1970.
 *   spki, spki_size          The whole SubjectPublicKeyInfo encoding.
 *   key                      The parsed key.
 *   flags                    CRYC_X509_ bits.
 *   path_len                 pathLenConstraint when CRYC_X509_HAS_PATH_LEN.
 *   key_usage                CRYC_KU_ bits when CRYC_X509_HAS_KEY_USAGE.
 *   ext_key_usage            CRYC_EKU_ bits when CRYC_X509_HAS_EXT_KEY_USAGE.
 *   san, san_size            The GeneralNames SEQUENCE's contents when
 *                            CRYC_X509_HAS_SAN.
 *   permitted, excluded      The GeneralSubtrees contents of
 *                            nameConstraints (either may be empty) when
 *                            CRYC_X509_HAS_NAME_CONSTRAINTS.
 *   ski                      subjectKeyIdentifier's OCTET STRING contents.
 *   aki                      authorityKeyIdentifier's keyIdentifier.
 *
 * Absent views are NULL with size 0.
 */
struct cryc_x509 {
	const uint8_t *der;
	size_t der_size;
	const uint8_t *tbs;
	size_t tbs_size;
	struct cryc_sig_params signature_alg;
	const uint8_t *signature;
	size_t signature_size;
	uint32_t version;
	const uint8_t *serial;
	size_t serial_size;
	const uint8_t *issuer;
	size_t issuer_size;
	const uint8_t *subject;
	size_t subject_size;
	int64_t not_before;
	int64_t not_after;
	const uint8_t *spki;
	size_t spki_size;
	struct cryc_pubkey key;
	uint32_t flags;
	uint32_t path_len;
	uint32_t key_usage;
	uint32_t ext_key_usage;
	const uint8_t *san;
	size_t san_size;
	const uint8_t *permitted;
	size_t permitted_size;
	const uint8_t *excluded;
	size_t excluded_size;
	const uint8_t *ski;
	size_t ski_size;
	const uint8_t *aki;
	size_t aki_size;
};

/*
 * Parses an AlgorithmIdentifier encoding (the whole SEQUENCE) naming a
 * signature algorithm.
 *
 * Recognized (OIDs cited as data at their table in the .c file):
 * sha256/384/512WithRSAEncryption with NULL parameters (RFC 8017 A.2.4;
 * absent parameters are also accepted, as RFC 4055 5 permits readers
 * to); RSASSA-PSS (RFC 4055 3.1) whose hashAlgorithm is SHA-256/384/512,
 * maskGenAlgorithm MGF1 with the same hash, trailerField 1, and
 * saltLength at most 64 (other PSS parameters are CRYC_SIG_UNSUPPORTED;
 * absent parameters, meaning SHA-1, are CRYC_SIG_UNSUPPORTED);
 * ecdsa-with-SHA256/384/512 with absent parameters (RFC 5758 3.2).  Any
 * other well-formed AlgorithmIdentifier (SHA-1, MD5, Ed25519, ...) is
 * CRYC_SIG_UNSUPPORTED.
 *
 * Returns 0, or EINVAL for a malformed encoding (also: NULL parameters
 * on ECDSA, a PSS parameter structure that does not parse).
 */
int cryc_x509_signature_algorithm(const uint8_t *encoding, size_t size, struct cryc_sig_params *params);

/*
 * Parses a SubjectPublicKeyInfo encoding (the whole SEQUENCE).
 *
 * rsaEncryption with NULL parameters (RFC 3279 2.3.1): RSAPublicKey
 * (RFC 8017 A.1.1), n and e positive INTEGERs, giving CRYC_KEY_RSA (the
 * size limits are checked when the key is used, not here).
 * id-ecPublicKey with namedCurve secp256r1 or secp384r1 (RFC 5480 2.1.1),
 * giving CRYC_KEY_EC with the BIT STRING's bytes as the point.  Any other
 * algorithm or curve (RSASSA-PSS keys, explicit curve parameters,
 * P-521, Ed25519) is CRYC_KEY_UNSUPPORTED.
 *
 * Returns 0 or EINVAL (malformed).
 */
int cryc_x509_public_key(const uint8_t *encoding, size_t size, struct cryc_pubkey *key);

/*
 * Parses a certificate (RFC 5280 4.1) from size DER bytes.
 *
 * Checks: the outer structure and every field of tbsCertificate in
 * order; version 1, 2 or 3 (an explicit v1 is accepted); issuerUniqueID
 * and subjectUniqueID only in v2 and v3 (skipped); extensions only in v3,
 * at least one when present (4.1.2.9); tbsCertificate.signature equal to
 * signatureAlgorithm; no bytes after the Certificate; the extensions as
 * the file comment says.  Sets CRYC_X509_SELF_ISSUED when the issuer and
 * subject Name encodings are equal byte for byte, which is how a CA
 * writes a self-issued certificate (the comparison of RFC 5280 7.1 used
 * for chaining is cryc_name_equal, done during path building).
 *
 * The extension values (OIDs and syntax cited as data in the .c file):
 * basicConstraints (4.2.1.9; cA DEFAULT FALSE, pathLenConstraint 0 to
 * 255, a pathLenConstraint with cA false is EINVAL); keyUsage (4.2.1.3; a
 * BIT STRING of at most 9 bits with at least one set); extKeyUsage
 * (4.2.1.12; one or more OIDs); subjectAltName (4.2.1.6; a non-empty
 * GeneralNames whose contents are checked by cryc-name.h when used);
 * nameConstraints (4.2.1.10; must be critical, at least one of the two
 * subtrees, minimum 0 and no maximum, else EINVAL); subjectKeyIdentifier
 * (4.2.1.2); authorityKeyIdentifier (4.2.1.1; only keyIdentifier is kept).
 *
 * Returns 0, or EINVAL for anything malformed (*cert is then zeroed).
 */
int cryc_x509_parse(struct cryc_x509 *cert, const uint8_t *der, size_t size);

/*
 * Returns the hash of a signature algorithm (SHA-256 for
 * CRYC_SIG_RSA_PKCS1_SHA256 and so on), or CRYC_HASH_NONE for
 * CRYC_SIG_NONE and CRYC_SIG_UNSUPPORTED.
 */
enum cryc_hash_id cryc_sig_hash(enum cryc_sig_alg alg);

#endif
