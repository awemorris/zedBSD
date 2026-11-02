/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parsing of X.509 certificates, public keys and signature algorithms
 * (RFC 5280).
 */

#include "cryc-x509.h"

#include "cryc-der.h"

#include <string.h>

/*
 * The most extensions one certificate may carry.  A certificate is parsed
 * from untrusted bytes and a repeated extension is refused by comparing
 * every OID with the ones before it, so the count is bounded to keep that
 * comparison cheap whatever the size of the certificate.  Real certificates
 * carry fewer than twenty.
 */
#define X509_EXTENSION_MAX	64U

/* The largest pathLenConstraint accepted (RFC 5280 4.2.1.9 leaves it open; 255 is far beyond any real chain). */
#define X509_PATH_LEN_MAX	255U

/* The largest RSASSA-PSS saltLength accepted: the size of the largest hash, SHA-512 (RFC 4055 3.1 leaves the length open). */
#define X509_PSS_SALT_MAX	64U

/* The saltLength of RSASSA-PSS-params when the field is absent (RFC 4055 3.1: DEFAULT 20). */
#define X509_PSS_SALT_DEFAULT	20U

/* The trailerField value that stands for the trailer octet 0xbc (RFC 4055 3.1). */
#define X509_PSS_TRAILER_BC	1U

/* The largest version value, v3 (RFC 5280 4.1.2.1: Version ::= INTEGER { v1(0), v2(1), v3(2) }). */
#define X509_VERSION_V3		2U

/* The most bits a keyUsage value may have: digitalSignature to decipherOnly (RFC 5280 4.2.1.3). */
#define X509_KEY_USAGE_BITS	9U

/*
 * The explicit context-specific tags of the structures read: [0] version,
 * [3] extensions in tbsCertificate (RFC 5280 4.1), the four fields of the
 * RSASSA-PSS parameters (RFC 4055 3.1), the two subtree lists of
 * nameConstraints (RFC 5280 4.2.1.10), authorityCertIssuer (4.2.1.1).
 */
#define X509_TAG_EXPLICIT_0	(CRYC_DER_CONTEXT | CRYC_DER_CONSTRUCTED | 0U)
#define X509_TAG_EXPLICIT_1	(CRYC_DER_CONTEXT | CRYC_DER_CONSTRUCTED | 1U)
#define X509_TAG_EXPLICIT_2	(CRYC_DER_CONTEXT | CRYC_DER_CONSTRUCTED | 2U)
#define X509_TAG_EXPLICIT_3	(CRYC_DER_CONTEXT | CRYC_DER_CONSTRUCTED | 3U)

/*
 * The implicit (primitive) context-specific tags of the fields read: the
 * unique identifiers [1] and [2] of tbsCertificate (RFC 5280 4.1), the
 * minimum of a GeneralSubtree (4.2.1.10), the keyIdentifier [0] and the
 * serial number [2] of authorityKeyIdentifier (4.2.1.1).
 */
#define X509_TAG_IMPLICIT_0	(CRYC_DER_CONTEXT | 0U)
#define X509_TAG_IMPLICIT_1	(CRYC_DER_CONTEXT | 1U)
#define X509_TAG_IMPLICIT_2	(CRYC_DER_CONTEXT | 2U)

/* The class bits of an identifier octet (X.690 8.1.2.2); CRYC_DER_CONTEXT is the context-specific class. */
#define X509_TAG_CLASS_MASK	0xc0U

/* The first octet of an uncompressed point and of the two compressed forms (SEC 1 version 2.0 2.3.3). */
#define X509_POINT_UNCOMPRESSED	0x04U
#define X509_POINT_COMPRESSED_2	0x02U
#define X509_POINT_COMPRESSED_3	0x03U

/* The byte length of a coordinate of P-256 and of P-384 (NIST SP 800-186 3.2.1.3 and 3.2.1.4). */
#define X509_FIELD_SIZE_P256	32U
#define X509_FIELD_SIZE_P384	48U

/*
 * One object identifier and the value it stands for.  A table of these
 * ends with an entry whose oid is NULL.
 */
struct x509_oid_entry {
	const uint8_t *oid;
	size_t size;
	unsigned value;
};

/*
 * One AlgorithmIdentifier taken apart (RFC 5280 4.1.1.2): its OID, and its
 * parameters when the encoding has them.  The views are into the bytes the
 * identifier was read from.
 */
struct x509_algorithm {
	struct cryc_der_element oid;
	struct cryc_der_element parameters;
	int has_parameters;
};

/*
 * The extensions this parser tells apart.  X509_EXT_IGNORED is a
 * recognized extension that is not processed (the policy extensions),
 * X509_EXT_UNKNOWN is any other.
 */
enum x509_extension_id {
	X509_EXT_UNKNOWN = 0,
	X509_EXT_IGNORED = 1,
	X509_EXT_BASIC_CONSTRAINTS = 2,
	X509_EXT_KEY_USAGE = 3,
	X509_EXT_EXT_KEY_USAGE = 4,
	X509_EXT_SUBJECT_ALT_NAME = 5,
	X509_EXT_NAME_CONSTRAINTS = 6,
	X509_EXT_SUBJECT_KEY_ID = 7,
	X509_EXT_AUTHORITY_KEY_ID = 8
};

/*
 * The object identifiers read, as the contents octets of their DER
 * encodings (X.690 8.19: the first two arcs as 40 * a + b, every arc in
 * base 128 with the top bit set on all but the last octet).
 *
 * Values referenced: RFC 8017 (November 2016) Appendix A.2.4:
 * sha256WithRSAEncryption 1.2.840.113549.1.1.11, sha384WithRSAEncryption
 * 1.2.840.113549.1.1.12, sha512WithRSAEncryption 1.2.840.113549.1.1.13;
 * Appendix A.1: rsaEncryption 1.2.840.113549.1.1.1; Appendix A.2.3:
 * id-RSASSA-PSS 1.2.840.113549.1.1.10; Appendix B.2.1: id-mgf1
 * 1.2.840.113549.1.1.8.  RFC 4055 (June 2005) 2.1: id-sha256
 * 2.16.840.1.101.3.4.2.1, id-sha384 2.16.840.1.101.3.4.2.2, id-sha512
 * 2.16.840.1.101.3.4.2.3.
 */
static const uint8_t oid_sha256_with_rsa[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
static const uint8_t oid_sha384_with_rsa[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
static const uint8_t oid_sha512_with_rsa[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d };
static const uint8_t oid_rsa_encryption[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
static const uint8_t oid_rsassa_pss[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0a };
static const uint8_t oid_mgf1[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x08 };
static const uint8_t oid_sha256[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };
static const uint8_t oid_sha384[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02 };
static const uint8_t oid_sha512[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03 };

/*
 * Values referenced: RFC 5758 (January 2010) 3.2: ecdsa-with-SHA256
 * 1.2.840.10045.4.3.2, ecdsa-with-SHA384 1.2.840.10045.4.3.3,
 * ecdsa-with-SHA512 1.2.840.10045.4.3.4.  RFC 5480 (March 2009) 2.1.1:
 * id-ecPublicKey 1.2.840.10045.2.1; 2.1.1.1: secp256r1
 * 1.2.840.10045.3.1.7 and secp384r1 1.3.132.0.34.
 */
static const uint8_t oid_ecdsa_sha256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
static const uint8_t oid_ecdsa_sha384[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03 };
static const uint8_t oid_ecdsa_sha512[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04 };
static const uint8_t oid_ec_public_key[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
static const uint8_t oid_secp256r1[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
static const uint8_t oid_secp384r1[] = { 0x2b, 0x81, 0x04, 0x00, 0x22 };

/*
 * Values referenced: RFC 5280 (May 2008) Appendix A.2: id-ce is 2.5.29;
 * id-ce-subjectKeyIdentifier 14, id-ce-keyUsage 15, id-ce-subjectAltName
 * 17, id-ce-basicConstraints 19, id-ce-nameConstraints 30,
 * id-ce-certificatePolicies 32, id-ce-policyMappings 33,
 * id-ce-authorityKeyIdentifier 35, id-ce-policyConstraints 36,
 * id-ce-extKeyUsage 37, id-ce-inhibitAnyPolicy 54.
 */
static const uint8_t oid_ce_subject_key_id[] = { 0x55, 0x1d, 0x0e };
static const uint8_t oid_ce_key_usage[] = { 0x55, 0x1d, 0x0f };
static const uint8_t oid_ce_subject_alt_name[] = { 0x55, 0x1d, 0x11 };
static const uint8_t oid_ce_basic_constraints[] = { 0x55, 0x1d, 0x13 };
static const uint8_t oid_ce_name_constraints[] = { 0x55, 0x1d, 0x1e };
static const uint8_t oid_ce_certificate_policies[] = { 0x55, 0x1d, 0x20 };
static const uint8_t oid_ce_policy_mappings[] = { 0x55, 0x1d, 0x21 };
static const uint8_t oid_ce_authority_key_id[] = { 0x55, 0x1d, 0x23 };
static const uint8_t oid_ce_policy_constraints[] = { 0x55, 0x1d, 0x24 };
static const uint8_t oid_ce_ext_key_usage[] = { 0x55, 0x1d, 0x25 };
static const uint8_t oid_ce_inhibit_any_policy[] = { 0x55, 0x1d, 0x36 };

/*
 * Values referenced: RFC 5280 (May 2008) Appendix A.2: id-kp is
 * 1.3.6.1.5.5.7.3 (id-pkix 1.3.6.1.5.5.7, id-kp is id-pkix 3);
 * id-kp-serverAuth id-kp 1, id-kp-clientAuth id-kp 2;
 * anyExtendedKeyUsage 2.5.29.37.0.
 */
static const uint8_t oid_kp_server_auth[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };
static const uint8_t oid_kp_client_auth[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x02 };
static const uint8_t oid_any_ext_key_usage[] = { 0x55, 0x1d, 0x25, 0x00 };

/* The signature algorithms of sha*WithRSAEncryption, each with the algorithm it names. */
static const struct x509_oid_entry x509_pkcs1_signatures[] = {
	{ oid_sha256_with_rsa, sizeof(oid_sha256_with_rsa), CRYC_SIG_RSA_PKCS1_SHA256 },
	{ oid_sha384_with_rsa, sizeof(oid_sha384_with_rsa), CRYC_SIG_RSA_PKCS1_SHA384 },
	{ oid_sha512_with_rsa, sizeof(oid_sha512_with_rsa), CRYC_SIG_RSA_PKCS1_SHA512 },
	{ NULL, 0, 0 }
};

/* The signature algorithms of ecdsa-with-SHA2, each with the algorithm it names. */
static const struct x509_oid_entry x509_ecdsa_signatures[] = {
	{ oid_ecdsa_sha256, sizeof(oid_ecdsa_sha256), CRYC_SIG_ECDSA_SHA256 },
	{ oid_ecdsa_sha384, sizeof(oid_ecdsa_sha384), CRYC_SIG_ECDSA_SHA384 },
	{ oid_ecdsa_sha512, sizeof(oid_ecdsa_sha512), CRYC_SIG_ECDSA_SHA512 },
	{ NULL, 0, 0 }
};

/* The hash algorithms RSASSA-PSS may name, each with the hash it stands for. */
static const struct x509_oid_entry x509_hashes[] = {
	{ oid_sha256, sizeof(oid_sha256), CRYC_HASH_SHA256 },
	{ oid_sha384, sizeof(oid_sha384), CRYC_HASH_SHA384 },
	{ oid_sha512, sizeof(oid_sha512), CRYC_HASH_SHA512 },
	{ NULL, 0, 0 }
};

/* The named curves a public key may use, each with the curve it stands for. */
static const struct x509_oid_entry x509_curves[] = {
	{ oid_secp256r1, sizeof(oid_secp256r1), CRYC_CURVE_P256 },
	{ oid_secp384r1, sizeof(oid_secp384r1), CRYC_CURVE_P384 },
	{ NULL, 0, 0 }
};

/* The extensions recognized, each with its kind. */
static const struct x509_oid_entry x509_extensions[] = {
	{ oid_ce_basic_constraints, sizeof(oid_ce_basic_constraints), X509_EXT_BASIC_CONSTRAINTS },
	{ oid_ce_key_usage, sizeof(oid_ce_key_usage), X509_EXT_KEY_USAGE },
	{ oid_ce_ext_key_usage, sizeof(oid_ce_ext_key_usage), X509_EXT_EXT_KEY_USAGE },
	{ oid_ce_subject_alt_name, sizeof(oid_ce_subject_alt_name), X509_EXT_SUBJECT_ALT_NAME },
	{ oid_ce_name_constraints, sizeof(oid_ce_name_constraints), X509_EXT_NAME_CONSTRAINTS },
	{ oid_ce_subject_key_id, sizeof(oid_ce_subject_key_id), X509_EXT_SUBJECT_KEY_ID },
	{ oid_ce_authority_key_id, sizeof(oid_ce_authority_key_id), X509_EXT_AUTHORITY_KEY_ID },
	{ oid_ce_certificate_policies, sizeof(oid_ce_certificate_policies), X509_EXT_IGNORED },
	{ oid_ce_policy_mappings, sizeof(oid_ce_policy_mappings), X509_EXT_IGNORED },
	{ oid_ce_policy_constraints, sizeof(oid_ce_policy_constraints), X509_EXT_IGNORED },
	{ oid_ce_inhibit_any_policy, sizeof(oid_ce_inhibit_any_policy), X509_EXT_IGNORED },
	{ NULL, 0, 0 }
};

/* The extended key usage purposes recognized, each with the bit it sets. */
static const struct x509_oid_entry x509_purposes[] = {
	{ oid_kp_server_auth, sizeof(oid_kp_server_auth), CRYC_EKU_SERVER_AUTH },
	{ oid_kp_client_auth, sizeof(oid_kp_client_auth), CRYC_EKU_CLIENT_AUTH },
	{ oid_any_ext_key_usage, sizeof(oid_any_ext_key_usage), CRYC_EKU_ANY },
	{ NULL, 0, 0 }
};

/* Forward declarations. */
static int x509_oid_equal(const struct cryc_der_element *oid, const uint8_t *expected, size_t expected_size);
static int x509_oid_lookup(const struct x509_oid_entry *table, const struct cryc_der_element *oid, unsigned *value);
static int x509_is_null(const struct cryc_der_element *element);
static int x509_read_algorithm(const struct cryc_der_element *sequence, struct x509_algorithm *algorithm);
static int x509_read_hash(const struct cryc_der_element *sequence, enum cryc_hash_id *hash);
static int x509_read_pss_field(struct cryc_der *body, unsigned tag, struct cryc_der_element *content, int *present);
static int x509_pss_parameters(const struct x509_algorithm *algorithm, struct cryc_sig_params *params);
static int x509_signature_from_algorithm(const struct x509_algorithm *algorithm, struct cryc_sig_params *params);
static enum cryc_sig_alg x509_pss_algorithm(enum cryc_hash_id hash);
static int x509_rsa_public_key(const uint8_t *bytes, size_t size, struct cryc_rsa_key *key);
static int x509_ec_public_key(const struct x509_algorithm *algorithm, const uint8_t *bytes, size_t size, unsigned unused, struct cryc_pubkey *key);
static int x509_check_name(const struct cryc_der_element *name, int allow_empty);
static int x509_check_serial(const struct cryc_der_element *serial);
static int x509_parse_certificate(struct cryc_x509 *cert, const uint8_t *der, size_t size);
static int x509_parse_tbs(struct cryc_x509 *cert, const struct cryc_der_element *tbs, const struct cryc_der_element *outer_algorithm);
static int x509_read_unique_id(struct cryc_x509 *cert, struct cryc_der *body, unsigned tag);
static int x509_read_extensions(struct cryc_x509 *cert, const struct cryc_der_element *wrapper);
static int x509_apply_extension(struct cryc_x509 *cert, const struct cryc_der_element *oid, int critical, const struct cryc_der_element *value);
static int x509_open_value(const struct cryc_der_element *value, unsigned tag, struct cryc_der_element *element);
static int x509_ext_basic_constraints(struct cryc_x509 *cert, const struct cryc_der_element *value);
static int x509_ext_key_usage(struct cryc_x509 *cert, const struct cryc_der_element *value);
static int x509_ext_ext_key_usage(struct cryc_x509 *cert, const struct cryc_der_element *value);
static int x509_ext_subject_alt_name(struct cryc_x509 *cert, const struct cryc_der_element *value);
static int x509_ext_name_constraints(struct cryc_x509 *cert, const struct cryc_der_element *value);
static int x509_check_subtrees(const struct cryc_der_element *subtrees);
static int x509_ext_subject_key_id(struct cryc_x509 *cert, const struct cryc_der_element *value);
static int x509_ext_authority_key_id(struct cryc_x509 *cert, const struct cryc_der_element *value);

/*
 * Parses an AlgorithmIdentifier encoding (the whole SEQUENCE) naming a
 * signature algorithm.
 *
 * Only the algorithms of the header are named; everything else that is
 * well formed answers CRYC_SIG_UNSUPPORTED, so a certificate signed with
 * SHA-1, say, still parses.
 */
int
cryc_x509_signature_algorithm(
	const uint8_t *encoding,
	size_t size,
	struct cryc_sig_params *params)
{
	struct cryc_der reader;
	struct cryc_der_element sequence;
	struct x509_algorithm algorithm;
	int error;

	/* The caller sees no algorithm after a failed read. */
	memset(params, 0, sizeof(*params));

	/* Reads the whole encoding as one SEQUENCE. */
	cryc_der_init(&reader, encoding, size);
	error = cryc_der_expect(&reader, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return EINVAL;

	/* Refuses bytes after the SEQUENCE: the encoding is exactly one identifier. */
	if (reader.size != 0)
		return EINVAL;

	/* Takes the identifier apart into its OID and parameters. */
	error = x509_read_algorithm(&sequence, &algorithm);
	if (error != 0)
		return EINVAL;

	/* Names the algorithm the OID and parameters stand for. */
	error = x509_signature_from_algorithm(&algorithm, params);
	if (error != 0) {
		memset(params, 0, sizeof(*params));
		return EINVAL;
	}

	/* Succeeded: params names the algorithm or says it is unsupported. */
	return 0;
}

/*
 * Parses a SubjectPublicKeyInfo encoding (the whole SEQUENCE).
 *
 * Values referenced: RFC 5280 (May 2008) 4.1: SubjectPublicKeyInfo ::=
 * SEQUENCE { algorithm AlgorithmIdentifier, subjectPublicKey BIT STRING }.
 */
int
cryc_x509_public_key(
	const uint8_t *encoding,
	size_t size,
	struct cryc_pubkey *key)
{
	struct cryc_der reader;
	struct cryc_der inner;
	struct cryc_der_element sequence;
	struct cryc_der_element algorithm_sequence;
	struct cryc_der_element bits;
	struct x509_algorithm algorithm;
	const uint8_t *bytes;
	size_t bytes_size;
	unsigned unused;
	int is_rsa;
	int is_ec;
	int is_null;
	int error;

	/* The caller sees no key after a failed read. */
	memset(key, 0, sizeof(*key));

	/* Reads the whole encoding as one SEQUENCE. */
	cryc_der_init(&reader, encoding, size);
	error = cryc_der_expect(&reader, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return EINVAL;

	/* Refuses bytes after the SEQUENCE. */
	if (reader.size != 0)
		return EINVAL;

	/* Enters the SEQUENCE: the algorithm, then the key bits. */
	error = cryc_der_enter(&sequence, &inner);
	if (error != 0)
		return EINVAL;

	/* Reads the algorithm identifier. */
	error = cryc_der_expect(&inner, CRYC_DER_SEQUENCE, &algorithm_sequence);
	if (error != 0)
		return EINVAL;
	error = x509_read_algorithm(&algorithm_sequence, &algorithm);
	if (error != 0)
		return EINVAL;

	/* Reads the key bits and requires nothing after them. */
	error = cryc_der_expect(&inner, CRYC_DER_BIT_STRING, &bits);
	if (error != 0)
		return EINVAL;
	error = cryc_der_bit_string(&bits, &bytes, &bytes_size, &unused);
	if (error != 0)
		return EINVAL;
	if (inner.size != 0)
		return EINVAL;

	/* Tells the two kinds of key this library uses from every other. */
	is_rsa = x509_oid_equal(&algorithm.oid, oid_rsa_encryption, sizeof(oid_rsa_encryption));
	is_ec = x509_oid_equal(&algorithm.oid, oid_ec_public_key, sizeof(oid_ec_public_key));

	/*
	 * An RSA key: NULL parameters (RFC 3279 2.3.1), and an RSAPublicKey
	 * that fills the BIT STRING in whole bytes (RFC 8017 A.1.1).
	 */
	if (is_rsa) {
		if (!algorithm.has_parameters)
			return EINVAL;
		is_null = x509_is_null(&algorithm.parameters);
		if (!is_null)
			return EINVAL;
		if (unused != 0)
			return EINVAL;
		error = x509_rsa_public_key(bytes, bytes_size, &key->rsa);
		if (error != 0) {
			memset(key, 0, sizeof(*key));
			return EINVAL;
		}
		key->type = CRYC_KEY_RSA;

		/* Succeeded: an RSA key with n and e as views. */
		return 0;
	}

	/* An EC key: a named curve, then the point (RFC 5480 2.1.1, 2.2). */
	if (is_ec) {
		error = x509_ec_public_key(&algorithm, bytes, bytes_size, unused, key);
		if (error != 0) {
			memset(key, 0, sizeof(*key));
			return EINVAL;
		}

		/* Succeeded: an EC key, or one recorded as unsupported. */
		return 0;
	}

	/* Any other algorithm (RSASSA-PSS keys, Ed25519, ...) is well formed and outside the range. */
	key->type = CRYC_KEY_UNSUPPORTED;

	/* Succeeded: the key is recorded as unsupported. */
	return 0;
}

/*
 * Parses a certificate (RFC 5280 4.1) from size DER bytes.
 */
int
cryc_x509_parse(
	struct cryc_x509 *cert,
	const uint8_t *der,
	size_t size)
{
	int error;

	/* Parses into cert; a failure leaves nothing of it. */
	memset(cert, 0, sizeof(*cert));
	error = x509_parse_certificate(cert, der, size);
	if (error != 0) {
		memset(cert, 0, sizeof(*cert));
		return EINVAL;
	}

	/* Succeeded: cert holds views into der. */
	return 0;
}

/*
 * Returns the hash of a signature algorithm (SHA-256 for
 * CRYC_SIG_RSA_PKCS1_SHA256 and so on), or CRYC_HASH_NONE for
 * CRYC_SIG_NONE and CRYC_SIG_UNSUPPORTED.
 */
enum cryc_hash_id
cryc_sig_hash(
	enum cryc_sig_alg alg)
{
	/* Each algorithm names the hash it is built on. */
	switch (alg) {
	case CRYC_SIG_RSA_PKCS1_SHA256:
	case CRYC_SIG_RSA_PSS_SHA256:
	case CRYC_SIG_ECDSA_SHA256:
		return CRYC_HASH_SHA256;
	case CRYC_SIG_RSA_PKCS1_SHA384:
	case CRYC_SIG_RSA_PSS_SHA384:
	case CRYC_SIG_ECDSA_SHA384:
		return CRYC_HASH_SHA384;
	case CRYC_SIG_RSA_PKCS1_SHA512:
	case CRYC_SIG_RSA_PSS_SHA512:
	case CRYC_SIG_ECDSA_SHA512:
		return CRYC_HASH_SHA512;
	case CRYC_SIG_NONE:
	case CRYC_SIG_UNSUPPORTED:
	default:
		break;
	}

	/* No hash: the algorithm is none or outside the range. */
	return CRYC_HASH_NONE;
}

/*
 * Returns 1 when the OID has exactly the expected contents octets, else 0.
 */
static int
x509_oid_equal(
	const struct cryc_der_element *oid,
	const uint8_t *expected,
	size_t expected_size)
{
	int different;

	/* A different length cannot be the same OID. */
	if (oid->length != expected_size)
		return 0;

	/* The same length is the same OID when every octet matches. */
	different = memcmp(oid->contents, expected, expected_size);
	if (different != 0)
		return 0;

	/* Every octet matched. */
	return 1;
}

/*
 * Finds an OID in a table that ends with a NULL entry; returns 1 and the
 * entry's value when it is there, else 0 with *value 0.
 */
static int
x509_oid_lookup(
	const struct x509_oid_entry *table,
	const struct cryc_der_element *oid,
	unsigned *value)
{
	size_t index;
	int same;

	/* The caller sees zero when the OID is not in the table. */
	*value = 0;

	/* Compares the OID with every entry in turn. */
	for (index = 0; table[index].oid != NULL; index++) {
		same = x509_oid_equal(oid, table[index].oid, table[index].size);
		if (same) {
			*value = table[index].value;
			return 1;
		}
	}

	/* The OID is not in the table. */
	return 0;
}

/*
 * Returns 1 when the element is an ASN.1 NULL (tag 5, length 0), else 0.
 */
static int
x509_is_null(
	const struct cryc_der_element *element)
{
	/* NULL has the NULL tag. */
	if (element->tag != CRYC_DER_NULL)
		return 0;

	/* NULL has no contents. */
	if (element->length != 0)
		return 0;

	/* The element is NULL. */
	return 1;
}

/*
 * Takes an AlgorithmIdentifier SEQUENCE apart (RFC 5280 4.1.1.2:
 * SEQUENCE { algorithm OBJECT IDENTIFIER, parameters ANY DEFINED BY
 * algorithm OPTIONAL }); the parameters are one element when present.
 */
static int
x509_read_algorithm(
	const struct cryc_der_element *sequence,
	struct x509_algorithm *algorithm)
{
	struct cryc_der inner;
	int error;

	/* Nothing is known yet. */
	memset(algorithm, 0, sizeof(*algorithm));

	/* Enters the SEQUENCE. */
	error = cryc_der_enter(sequence, &inner);
	if (error != 0)
		return EINVAL;

	/* Reads the OID and checks that it is well formed. */
	error = cryc_der_expect(&inner, CRYC_DER_OID, &algorithm->oid);
	if (error != 0)
		return EINVAL;
	error = cryc_der_oid(&algorithm->oid);
	if (error != 0)
		return EINVAL;

	/* Reads the parameters when there are any. */
	if (inner.size != 0) {
		error = cryc_der_next(&inner, &algorithm->parameters);
		if (error != 0)
			return EINVAL;
		algorithm->has_parameters = 1;
	}

	/* Refuses anything after the parameters. */
	if (inner.size != 0)
		return EINVAL;

	/* Succeeded: the OID and the parameters are views. */
	return 0;
}

/*
 * Reads a hash AlgorithmIdentifier of RSASSA-PSS-params (RFC 4055 2.1): a
 * hash of the table gets NULL or no parameters (both are legal, 2.1), any
 * other hash gives CRYC_HASH_NONE.
 */
static int
x509_read_hash(
	const struct cryc_der_element *sequence,
	enum cryc_hash_id *hash)
{
	struct x509_algorithm algorithm;
	unsigned value;
	int found;
	int is_null;
	int error;

	/* No hash is known yet. */
	*hash = CRYC_HASH_NONE;

	/* Takes the identifier apart. */
	error = x509_read_algorithm(sequence, &algorithm);
	if (error != 0)
		return EINVAL;

	/* Looks the OID up among the hashes this library names. */
	found = x509_oid_lookup(x509_hashes, &algorithm.oid, &value);
	if (!found)
		return 0;

	/* A known hash takes NULL parameters or none. */
	if (algorithm.has_parameters) {
		is_null = x509_is_null(&algorithm.parameters);
		if (!is_null)
			return EINVAL;
	}

	/* Publishes the hash. */
	*hash = (enum cryc_hash_id)value;

	/* Succeeded: the hash is known or CRYC_HASH_NONE. */
	return 0;
}

/*
 * Reads one optional explicitly tagged field of RSASSA-PSS-params: when the
 * field is there, *content is the single element inside the tag.
 */
static int
x509_read_pss_field(
	struct cryc_der *body,
	unsigned tag,
	struct cryc_der_element *content,
	int *present)
{
	struct cryc_der_element field;
	struct cryc_der wrapped;
	int error;

	/* The field may be absent. */
	memset(content, 0, sizeof(*content));
	error = cryc_der_optional(body, tag, &field, present);
	if (error != 0)
		return EINVAL;
	if (!*present)
		return 0;

	/* The explicit tag holds exactly one element. */
	error = cryc_der_enter(&field, &wrapped);
	if (error != 0)
		return EINVAL;
	error = cryc_der_next(&wrapped, content);
	if (error != 0)
		return EINVAL;
	if (wrapped.size != 0)
		return EINVAL;

	/* Succeeded: the field is absent or content is its element. */
	return 0;
}

/*
 * Reads the parameters of an RSASSA-PSS signature AlgorithmIdentifier
 * (RFC 4055 3.1) and names the algorithm when its hash is SHA-256, 384 or
 * 512, its mask generation is MGF1 with the same hash, its trailerField is
 * 1 and its saltLength is at most 64; any other well-formed combination is
 * CRYC_SIG_UNSUPPORTED.
 *
 * Values referenced: RFC 4055 (June 2005) 3.1: RSASSA-PSS-params ::=
 * SEQUENCE { hashAlgorithm [0] DEFAULT sha1Identifier, maskGenAlgorithm
 * [1] DEFAULT mgf1SHA1Identifier, saltLength [2] INTEGER DEFAULT 20,
 * trailerField [3] INTEGER DEFAULT 1 }.
 */
static int
x509_pss_parameters(
	const struct x509_algorithm *algorithm,
	struct cryc_sig_params *params)
{
	struct cryc_der body;
	struct cryc_der_element content;
	struct x509_algorithm mask_algorithm;
	enum cryc_hash_id hash;
	enum cryc_hash_id mask_hash;
	uint32_t salt_size;
	uint32_t trailer;
	int salt_too_large;
	int trailer_unsupported;
	int mask_is_mgf1;
	int present;
	int error;

	/* RSASSA-PSS names no algorithm without its parameters (RFC 4055 3.1: they MUST be present). */
	params->alg = CRYC_SIG_UNSUPPORTED;
	params->salt_size = 0;
	if (!algorithm->has_parameters)
		return 0;

	/* The parameters are a SEQUENCE. */
	if (algorithm->parameters.tag != CRYC_DER_SEQUENCE)
		return EINVAL;
	error = cryc_der_enter(&algorithm->parameters, &body);
	if (error != 0)
		return EINVAL;

	/* The defaults stand for every field that is absent: SHA-1, MGF1 with SHA-1, 20, 1. */
	hash = CRYC_HASH_NONE;
	mask_hash = CRYC_HASH_NONE;
	mask_is_mgf1 = 1;
	salt_size = X509_PSS_SALT_DEFAULT;
	salt_too_large = 0;
	trailer = X509_PSS_TRAILER_BC;
	trailer_unsupported = 0;

	/* The hash algorithm [0]; absent means SHA-1, which is outside the range. */
	error = x509_read_pss_field(&body, X509_TAG_EXPLICIT_0, &content, &present);
	if (error != 0)
		return EINVAL;
	if (present) {
		if (content.tag != CRYC_DER_SEQUENCE)
			return EINVAL;
		error = x509_read_hash(&content, &hash);
		if (error != 0)
			return EINVAL;
	}

	/* The mask generation algorithm [1]; absent means MGF1 with SHA-1. */
	error = x509_read_pss_field(&body, X509_TAG_EXPLICIT_1, &content, &present);
	if (error != 0)
		return EINVAL;
	if (present) {
		if (content.tag != CRYC_DER_SEQUENCE)
			return EINVAL;
		error = x509_read_algorithm(&content, &mask_algorithm);
		if (error != 0)
			return EINVAL;

		/* MGF1 carries the hash it uses as its parameters (RFC 4055 2.2: MUST). */
		mask_is_mgf1 = x509_oid_equal(&mask_algorithm.oid, oid_mgf1, sizeof(oid_mgf1));
		if (mask_is_mgf1) {
			if (!mask_algorithm.has_parameters)
				return EINVAL;
			if (mask_algorithm.parameters.tag != CRYC_DER_SEQUENCE)
				return EINVAL;
			error = x509_read_hash(&mask_algorithm.parameters, &mask_hash);
			if (error != 0)
				return EINVAL;
		}
	}

	/* The salt length [2]. */
	error = x509_read_pss_field(&body, X509_TAG_EXPLICIT_2, &content, &present);
	if (error != 0)
		return EINVAL;
	if (present) {
		if (content.tag != CRYC_DER_INTEGER)
			return EINVAL;
		error = cryc_der_small_integer(&content, X509_PSS_SALT_MAX, &salt_size);
		if (error == ERANGE) {
			salt_too_large = 1;
		} else if (error != 0) {
			return EINVAL;
		}
	}

	/* The trailer field [3]; only 1 (the octet 0xbc) is defined. */
	error = x509_read_pss_field(&body, X509_TAG_EXPLICIT_3, &content, &present);
	if (error != 0)
		return EINVAL;
	if (present) {
		if (content.tag != CRYC_DER_INTEGER)
			return EINVAL;
		error = cryc_der_small_integer(&content, 0xffffffffU, &trailer);
		if (error == ERANGE) {
			trailer_unsupported = 1;
		} else if (error != 0) {
			return EINVAL;
		}
		if (trailer != X509_PSS_TRAILER_BC)
			trailer_unsupported = 1;
	}

	/* Refuses anything after the four fields. */
	if (body.size != 0)
		return EINVAL;

	/* The shape is valid; the algorithm is named only for the combinations in the range. */
	if (hash == CRYC_HASH_NONE)
		return 0;
	if (!mask_is_mgf1)
		return 0;
	if (mask_hash != hash)
		return 0;
	if (salt_too_large)
		return 0;
	if (trailer_unsupported)
		return 0;

	/* Publishes the algorithm with the salt length the identifier names. */
	params->alg = x509_pss_algorithm(hash);
	params->salt_size = salt_size;

	/* Succeeded: the algorithm is named. */
	return 0;
}

/*
 * Names the signature algorithm of an AlgorithmIdentifier: PKCS #1 and
 * ECDSA by OID and parameters, RSASSA-PSS by its parameters, any other
 * well-formed identifier as unsupported.
 */
static int
x509_signature_from_algorithm(
	const struct x509_algorithm *algorithm,
	struct cryc_sig_params *params)
{
	unsigned value;
	int found;
	int is_pss;
	int is_null;
	int error;

	/* Any identifier not named below is outside the range. */
	params->alg = CRYC_SIG_UNSUPPORTED;
	params->salt_size = 0;

	/* sha*WithRSAEncryption: NULL parameters, or none (RFC 4055 5). */
	found = x509_oid_lookup(x509_pkcs1_signatures, &algorithm->oid, &value);
	if (found) {
		if (algorithm->has_parameters) {
			is_null = x509_is_null(&algorithm->parameters);
			if (!is_null)
				return EINVAL;
		}
		params->alg = (enum cryc_sig_alg)value;

		/* Succeeded: a PKCS #1 signature algorithm. */
		return 0;
	}

	/* ecdsa-with-SHA2: no parameters at all (RFC 5758 3.2). */
	found = x509_oid_lookup(x509_ecdsa_signatures, &algorithm->oid, &value);
	if (found) {
		if (algorithm->has_parameters)
			return EINVAL;
		params->alg = (enum cryc_sig_alg)value;

		/* Succeeded: an ECDSA signature algorithm. */
		return 0;
	}

	/* RSASSA-PSS: named by its parameters. */
	is_pss = x509_oid_equal(&algorithm->oid, oid_rsassa_pss, sizeof(oid_rsassa_pss));
	if (is_pss) {
		error = x509_pss_parameters(algorithm, params);
		if (error != 0)
			return error;

		/* Succeeded: a PSS algorithm, or one recorded as unsupported. */
		return 0;
	}

	/* Succeeded: a well-formed identifier outside the range stays unsupported. */
	return 0;
}

/*
 * Returns the RSASSA-PSS algorithm that uses the hash (SHA-256, 384 or
 * 512 only).
 */
static enum cryc_sig_alg
x509_pss_algorithm(
	enum cryc_hash_id hash)
{
	/* Each hash has its own PSS algorithm. */
	switch (hash) {
	case CRYC_HASH_SHA256:
		return CRYC_SIG_RSA_PSS_SHA256;
	case CRYC_HASH_SHA384:
		return CRYC_SIG_RSA_PSS_SHA384;
	case CRYC_HASH_SHA512:
		return CRYC_SIG_RSA_PSS_SHA512;
	case CRYC_HASH_NONE:
	default:
		break;
	}

	/* No PSS algorithm for another hash. */
	return CRYC_SIG_UNSUPPORTED;
}

/*
 * Reads an RSAPublicKey (RFC 8017 A.1.1: SEQUENCE { modulus INTEGER,
 * publicExponent INTEGER }), both positive, into views of their
 * magnitudes.
 */
static int
x509_rsa_public_key(
	const uint8_t *bytes,
	size_t size,
	struct cryc_rsa_key *key)
{
	struct cryc_der reader;
	struct cryc_der inner;
	struct cryc_der_element sequence;
	struct cryc_der_element integer;
	int error;

	/* Reads the whole bit string contents as one SEQUENCE. */
	cryc_der_init(&reader, bytes, size);
	error = cryc_der_expect(&reader, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return EINVAL;
	if (reader.size != 0)
		return EINVAL;
	error = cryc_der_enter(&sequence, &inner);
	if (error != 0)
		return EINVAL;

	/* The modulus: a positive INTEGER. */
	error = cryc_der_expect(&inner, CRYC_DER_INTEGER, &integer);
	if (error != 0)
		return EINVAL;
	error = cryc_der_integer(&integer, &key->n, &key->n_size);
	if (error != 0)
		return EINVAL;
	if (key->n_size == 1 && key->n[0] == 0)
		return EINVAL;

	/* The public exponent: a positive INTEGER. */
	error = cryc_der_expect(&inner, CRYC_DER_INTEGER, &integer);
	if (error != 0)
		return EINVAL;
	error = cryc_der_integer(&integer, &key->e, &key->e_size);
	if (error != 0)
		return EINVAL;
	if (key->e_size == 1 && key->e[0] == 0)
		return EINVAL;

	/* Refuses anything after the exponent. */
	if (inner.size != 0)
		return EINVAL;

	/* Succeeded: n and e are views into the key bytes. */
	return 0;
}

/*
 * Reads the parameters and the point of an id-ecPublicKey key (RFC 5480
 * 2.1.1, 2.2): a key on secp256r1 or secp384r1 with an uncompressed point
 * is CRYC_KEY_EC; a valid key outside that (another curve, a parameter
 * that is not a named curve, a compressed point) is CRYC_KEY_UNSUPPORTED.
 */
static int
x509_ec_public_key(
	const struct x509_algorithm *algorithm,
	const uint8_t *bytes,
	size_t size,
	unsigned unused,
	struct cryc_pubkey *key)
{
	unsigned value;
	size_t field_size;
	int found;
	int error;

	/* The parameters name the curve; anything but an OID is outside the range. */
	if (!algorithm->has_parameters) {
		key->type = CRYC_KEY_UNSUPPORTED;
		return 0;
	}
	if (algorithm->parameters.tag != CRYC_DER_OID) {
		key->type = CRYC_KEY_UNSUPPORTED;
		return 0;
	}
	error = cryc_der_oid(&algorithm->parameters);
	if (error != 0)
		return EINVAL;

	/* A curve this library does not use is outside the range. */
	found = x509_oid_lookup(x509_curves, &algorithm->parameters, &value);
	if (!found) {
		key->type = CRYC_KEY_UNSUPPORTED;
		return 0;
	}

	/* The point is a whole number of bytes. */
	if (unused != 0)
		return EINVAL;

	/* The size of a coordinate of the curve. */
	field_size = X509_FIELD_SIZE_P256;
	if (value == CRYC_CURVE_P384)
		field_size = X509_FIELD_SIZE_P384;

	/* A compressed point is valid but not used. */
	if (size == 1 + field_size) {
		if (bytes[0] == X509_POINT_COMPRESSED_2 || bytes[0] == X509_POINT_COMPRESSED_3) {
			key->type = CRYC_KEY_UNSUPPORTED;
			return 0;
		}
	}

	/* An uncompressed point is 0x04 and both coordinates. */
	if (size != 1 + 2 * field_size)
		return EINVAL;
	if (bytes[0] != X509_POINT_UNCOMPRESSED)
		return EINVAL;

	/* Publishes the key. */
	key->type = CRYC_KEY_EC;
	key->curve = (enum cryc_curve_id)value;
	key->point = bytes;
	key->point_size = size;

	/* Succeeded: the point is a view into the key bytes. */
	return 0;
}

/*
 * Checks the structure of a Name (RFC 5280 4.1.2.4: a SEQUENCE of
 * RelativeDistinguishedNames, each a non-empty SET of SEQUENCE { type
 * OBJECT IDENTIFIER, value ANY }); an empty Name is refused unless
 * allowed.
 */
static int
x509_check_name(
	const struct cryc_der_element *name,
	int allow_empty)
{
	struct cryc_der names;
	struct cryc_der set;
	struct cryc_der attribute;
	struct cryc_der_element rdn;
	struct cryc_der_element pair;
	struct cryc_der_element type;
	struct cryc_der_element value;
	int error;

	/* Enters the Name's SEQUENCE. */
	error = cryc_der_enter(name, &names);
	if (error != 0)
		return EINVAL;

	/* An empty Name is only for the subject of a certificate whose subjectAltName is critical. */
	if (names.size == 0) {
		if (allow_empty)
			return 0;
		return EINVAL;
	}

	/* Checks every relative distinguished name. */
	while (names.size != 0) {
		error = cryc_der_expect(&names, CRYC_DER_SET, &rdn);
		if (error != 0)
			return EINVAL;
		error = cryc_der_enter(&rdn, &set);
		if (error != 0)
			return EINVAL;

		/* An RDN has at least one attribute. */
		if (set.size == 0)
			return EINVAL;

		/* Checks every attribute: an OID and one value. */
		while (set.size != 0) {
			error = cryc_der_expect(&set, CRYC_DER_SEQUENCE, &pair);
			if (error != 0)
				return EINVAL;
			error = cryc_der_enter(&pair, &attribute);
			if (error != 0)
				return EINVAL;
			error = cryc_der_expect(&attribute, CRYC_DER_OID, &type);
			if (error != 0)
				return EINVAL;
			error = cryc_der_oid(&type);
			if (error != 0)
				return EINVAL;
			error = cryc_der_next(&attribute, &value);
			if (error != 0)
				return EINVAL;
			if (attribute.size != 0)
				return EINVAL;
		}
	}

	/* Succeeded: the Name is well formed. */
	return 0;
}

/*
 * Checks a serialNumber (RFC 5280 4.1.2.2): an INTEGER of 1 to
 * CRYC_X509_SERIAL_MAX octets in the minimal two's complement form
 * (X.690 8.3.2); a negative or zero value is accepted.
 */
static int
x509_check_serial(
	const struct cryc_der_element *serial)
{
	const uint8_t *contents;

	/* A serial number has at least one octet and at most the bound. */
	if (serial->length == 0)
		return EINVAL;
	if (serial->length > CRYC_X509_SERIAL_MAX)
		return EINVAL;

	/* A redundant leading 0x00 or 0xff octet is a non-minimal encoding (8.3.2). */
	contents = serial->contents;
	if (serial->length > 1) {
		if (contents[0] == 0x00 && (contents[1] & 0x80U) == 0)
			return EINVAL;
		if (contents[0] == 0xff && (contents[1] & 0x80U) != 0)
			return EINVAL;
	}

	/* Succeeded: the serial number is within range and minimal. */
	return 0;
}

/*
 * Parses the Certificate SEQUENCE (RFC 5280 4.1): tbsCertificate,
 * signatureAlgorithm, signatureValue, and nothing after.
 */
static int
x509_parse_certificate(
	struct cryc_x509 *cert,
	const uint8_t *der,
	size_t size)
{
	struct cryc_der reader;
	struct cryc_der inner;
	struct cryc_der_element certificate;
	struct cryc_der_element tbs;
	struct cryc_der_element outer_algorithm;
	struct cryc_der_element bits;
	const uint8_t *signature;
	size_t signature_size;
	unsigned unused;
	int error;

	/* The Certificate is the whole input: one SEQUENCE and nothing after. */
	cryc_der_init(&reader, der, size);
	error = cryc_der_expect(&reader, CRYC_DER_SEQUENCE, &certificate);
	if (error != 0)
		return EINVAL;
	if (reader.size != 0)
		return EINVAL;
	cert->der = certificate.encoding;
	cert->der_size = certificate.encoding_size;

	/* The three fields of the Certificate. */
	error = cryc_der_enter(&certificate, &inner);
	if (error != 0)
		return EINVAL;
	error = cryc_der_expect(&inner, CRYC_DER_SEQUENCE, &tbs);
	if (error != 0)
		return EINVAL;
	error = cryc_der_expect(&inner, CRYC_DER_SEQUENCE, &outer_algorithm);
	if (error != 0)
		return EINVAL;
	error = cryc_der_expect(&inner, CRYC_DER_BIT_STRING, &bits);
	if (error != 0)
		return EINVAL;
	if (inner.size != 0)
		return EINVAL;

	/* The signed bytes are the whole tbsCertificate encoding. */
	cert->tbs = tbs.encoding;
	cert->tbs_size = tbs.encoding_size;

	/* The signature algorithm (the very same bytes must open the tbsCertificate, checked there). */
	error = cryc_x509_signature_algorithm(outer_algorithm.encoding, outer_algorithm.encoding_size, &cert->signature_alg);
	if (error != 0)
		return EINVAL;

	/* The signature value: whole bytes. */
	error = cryc_der_bit_string(&bits, &signature, &signature_size, &unused);
	if (error != 0)
		return EINVAL;
	if (unused != 0)
		return EINVAL;
	cert->signature = signature;
	cert->signature_size = signature_size;

	/* The tbsCertificate fields. */
	error = x509_parse_tbs(cert, &tbs, &outer_algorithm);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the certificate is parsed. */
	return 0;
}

/*
 * Parses the tbsCertificate (RFC 5280 4.1.2) into cert.
 */
static int
x509_parse_tbs(
	struct cryc_x509 *cert,
	const struct cryc_der_element *tbs,
	const struct cryc_der_element *outer_algorithm)
{
	struct cryc_der body;
	struct cryc_der wrapped;
	struct cryc_der validity;
	struct cryc_der_element field;
	struct cryc_der_element serial;
	struct cryc_der_element inner_algorithm;
	struct cryc_der_element issuer;
	struct cryc_der_element validity_sequence;
	struct cryc_der_element time;
	struct cryc_der_element subject;
	struct cryc_der_element spki;
	struct cryc_der_element extensions;
	struct cryc_sig_params inner_params;
	uint32_t version_value;
	int present;
	int different;
	int error;

	/* Enters the tbsCertificate SEQUENCE. */
	error = cryc_der_enter(tbs, &body);
	if (error != 0)
		return EINVAL;

	/* The version [0] EXPLICIT: absent is v1, an explicit v1 is accepted. */
	cert->version = 1;
	error = cryc_der_optional(&body, X509_TAG_EXPLICIT_0, &field, &present);
	if (error != 0)
		return EINVAL;
	if (present) {
		error = cryc_der_enter(&field, &wrapped);
		if (error != 0)
			return EINVAL;
		error = cryc_der_expect(&wrapped, CRYC_DER_INTEGER, &field);
		if (error != 0)
			return EINVAL;
		if (wrapped.size != 0)
			return EINVAL;
		error = cryc_der_small_integer(&field, X509_VERSION_V3, &version_value);
		if (error != 0)
			return EINVAL;
		cert->version = version_value + 1;
	}

	/* The serial number. */
	error = cryc_der_expect(&body, CRYC_DER_INTEGER, &serial);
	if (error != 0)
		return EINVAL;
	error = x509_check_serial(&serial);
	if (error != 0)
		return EINVAL;
	cert->serial = serial.contents;
	cert->serial_size = serial.length;

	/* The signature algorithm: the very bytes of signatureAlgorithm (4.1.1.2). */
	error = cryc_der_expect(&body, CRYC_DER_SEQUENCE, &inner_algorithm);
	if (error != 0)
		return EINVAL;
	error = cryc_x509_signature_algorithm(inner_algorithm.encoding, inner_algorithm.encoding_size, &inner_params);
	if (error != 0)
		return EINVAL;
	if (inner_algorithm.encoding_size != outer_algorithm->encoding_size)
		return EINVAL;
	different = memcmp(inner_algorithm.encoding, outer_algorithm->encoding, outer_algorithm->encoding_size);
	if (different != 0)
		return EINVAL;

	/* The issuer: a non-empty Name (4.1.2.4). */
	error = cryc_der_expect(&body, CRYC_DER_SEQUENCE, &issuer);
	if (error != 0)
		return EINVAL;
	error = x509_check_name(&issuer, 0);
	if (error != 0)
		return EINVAL;
	cert->issuer = issuer.encoding;
	cert->issuer_size = issuer.encoding_size;

	/* The validity: notBefore and notAfter, each a UTCTime or a GeneralizedTime. */
	error = cryc_der_expect(&body, CRYC_DER_SEQUENCE, &validity_sequence);
	if (error != 0)
		return EINVAL;
	error = cryc_der_enter(&validity_sequence, &validity);
	if (error != 0)
		return EINVAL;
	error = cryc_der_next(&validity, &time);
	if (error != 0)
		return EINVAL;
	error = cryc_der_time(&time, &cert->not_before);
	if (error != 0)
		return EINVAL;
	error = cryc_der_next(&validity, &time);
	if (error != 0)
		return EINVAL;
	error = cryc_der_time(&time, &cert->not_after);
	if (error != 0)
		return EINVAL;
	if (validity.size != 0)
		return EINVAL;

	/*
	 * The subject: a Name, which may be empty.  RFC 5280 4.1.2.6 asks the
	 * issuing CA to mark the subjectAltName critical then; that is a rule
	 * for the CA, not for path validation, and it is not enforced here
	 * (host names come from the subjectAltName alone).
	 */
	error = cryc_der_expect(&body, CRYC_DER_SEQUENCE, &subject);
	if (error != 0)
		return EINVAL;
	error = x509_check_name(&subject, 1);
	if (error != 0)
		return EINVAL;
	cert->subject = subject.encoding;
	cert->subject_size = subject.encoding_size;

	/* Self-issued: the two Names are the same bytes. */
	if (cert->issuer_size == cert->subject_size) {
		different = memcmp(cert->issuer, cert->subject, cert->subject_size);
		if (different == 0)
			cert->flags |= CRYC_X509_SELF_ISSUED;
	}

	/* The subject public key. */
	error = cryc_der_expect(&body, CRYC_DER_SEQUENCE, &spki);
	if (error != 0)
		return EINVAL;
	cert->spki = spki.encoding;
	cert->spki_size = spki.encoding_size;
	error = cryc_x509_public_key(spki.encoding, spki.encoding_size, &cert->key);
	if (error != 0)
		return EINVAL;

	/* The unique identifiers [1] and [2] (v2 and v3 only, skipped). */
	error = x509_read_unique_id(cert, &body, X509_TAG_IMPLICIT_1);
	if (error != 0)
		return EINVAL;
	error = x509_read_unique_id(cert, &body, X509_TAG_IMPLICIT_2);
	if (error != 0)
		return EINVAL;

	/* The extensions [3] EXPLICIT (v3 only). */
	error = cryc_der_optional(&body, X509_TAG_EXPLICIT_3, &extensions, &present);
	if (error != 0)
		return EINVAL;
	if (present) {
		if (cert->version != 3)
			return EINVAL;
		error = x509_read_extensions(cert, &extensions);
		if (error != 0)
			return EINVAL;
	}

	/* Refuses anything after the extensions. */
	if (body.size != 0)
		return EINVAL;

	/* Succeeded: every field of the tbsCertificate was read. */
	return 0;
}

/*
 * Skips an optional unique identifier of tbsCertificate (a BIT STRING
 * under the implicit tag): RFC 5280 4.1.2.8 allows it in v2 and v3 only.
 */
static int
x509_read_unique_id(
	struct cryc_x509 *cert,
	struct cryc_der *body,
	unsigned tag)
{
	struct cryc_der_element unique_id;
	const uint8_t *bytes;
	size_t bytes_size;
	unsigned unused;
	int present;
	int error;

	/* The identifier may be absent. */
	error = cryc_der_optional(body, tag, &unique_id, &present);
	if (error != 0)
		return EINVAL;
	if (!present)
		return 0;

	/* A v1 certificate has none. */
	if (cert->version < 2)
		return EINVAL;

	/* The contents are a BIT STRING's. */
	error = cryc_der_bit_string(&unique_id, &bytes, &bytes_size, &unused);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the identifier is well formed and skipped. */
	return 0;
}

/*
 * Reads the Extensions of a tbsCertificate (RFC 5280 4.1.2.9): one or
 * more Extension ::= SEQUENCE { extnID OBJECT IDENTIFIER, critical
 * BOOLEAN DEFAULT FALSE, extnValue OCTET STRING }, each OID at most once
 * (4.2).
 */
static int
x509_read_extensions(
	struct cryc_x509 *cert,
	const struct cryc_der_element *wrapper)
{
	struct cryc_der wrapped;
	struct cryc_der list;
	struct cryc_der item;
	struct cryc_der_element sequence;
	struct cryc_der_element extension;
	struct cryc_der_element oid;
	struct cryc_der_element critical_element;
	struct cryc_der_element value;
	struct cryc_der_element seen[X509_EXTENSION_MAX];
	size_t count;
	size_t index;
	int critical;
	int present;
	int same;
	int error;

	/* The explicit tag holds one SEQUENCE OF Extension. */
	error = cryc_der_enter(wrapper, &wrapped);
	if (error != 0)
		return EINVAL;
	error = cryc_der_expect(&wrapped, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return EINVAL;
	if (wrapped.size != 0)
		return EINVAL;
	error = cryc_der_enter(&sequence, &list);
	if (error != 0)
		return EINVAL;

	/* The list has at least one extension (4.1.2.9). */
	if (list.size == 0)
		return EINVAL;

	/* Reads every extension. */
	count = 0;
	while (list.size != 0) {
		/* The count stays within the bound the duplicate check relies on. */
		if (count >= X509_EXTENSION_MAX)
			return EINVAL;

		/* One Extension: the OID, the optional critical flag, the value. */
		error = cryc_der_expect(&list, CRYC_DER_SEQUENCE, &extension);
		if (error != 0)
			return EINVAL;
		error = cryc_der_enter(&extension, &item);
		if (error != 0)
			return EINVAL;
		error = cryc_der_expect(&item, CRYC_DER_OID, &oid);
		if (error != 0)
			return EINVAL;
		error = cryc_der_oid(&oid);
		if (error != 0)
			return EINVAL;
		error = cryc_der_optional(&item, CRYC_DER_BOOLEAN, &critical_element, &present);
		if (error != 0)
			return EINVAL;

		/* The flag is TRUE when present: FALSE is the DEFAULT and DER leaves it out (X.690 11.5). */
		critical = 0;
		if (present) {
			error = cryc_der_boolean(&critical_element, &critical);
			if (error != 0)
				return EINVAL;
			if (!critical)
				return EINVAL;
		}
		error = cryc_der_expect(&item, CRYC_DER_OCTET_STRING, &value);
		if (error != 0)
			return EINVAL;
		if (item.size != 0)
			return EINVAL;

		/* An extension appears at most once (4.2). */
		for (index = 0; index < count; index++) {
			same = x509_oid_equal(&oid, seen[index].contents, seen[index].length);
			if (same)
				return EINVAL;
		}
		seen[count] = oid;
		count++;

		/* Records what the extension says. */
		error = x509_apply_extension(cert, &oid, critical, &value);
		if (error != 0)
			return EINVAL;
	}

	/* Succeeded: every extension was read. */
	return 0;
}

/*
 * Applies one extension to cert by its OID: the processed ones record
 * their value, the recognized policy extensions are passed over, and an
 * unknown critical one is flagged (RFC 5280 4.2: a verifier refuses a
 * certificate with a critical extension it does not process).
 */
static int
x509_apply_extension(
	struct cryc_x509 *cert,
	const struct cryc_der_element *oid,
	int critical,
	const struct cryc_der_element *value)
{
	unsigned kind;
	int found;
	int error;

	/* Finds out which extension it is; an OID not in the table is unknown. */
	found = x509_oid_lookup(x509_extensions, oid, &kind);
	if (!found)
		kind = X509_EXT_UNKNOWN;

	/* Reads the value by the kind of extension. */
	error = 0;
	switch (kind) {
	case X509_EXT_BASIC_CONSTRAINTS:
		error = x509_ext_basic_constraints(cert, value);
		break;
	case X509_EXT_KEY_USAGE:
		error = x509_ext_key_usage(cert, value);
		break;
	case X509_EXT_EXT_KEY_USAGE:
		error = x509_ext_ext_key_usage(cert, value);
		break;
	case X509_EXT_SUBJECT_ALT_NAME:
		error = x509_ext_subject_alt_name(cert, value);
		if (error == 0 && critical)
			cert->flags |= CRYC_X509_SAN_CRITICAL;
		break;
	case X509_EXT_NAME_CONSTRAINTS:
		/*
		 * RFC 5280 4.2.1.10 asks CAs to mark nameConstraints critical;
		 * a reader that refused the others could not load a root whose
		 * constraints are not marked, so the constraints are processed
		 * whatever the flag says.
		 */
		error = x509_ext_name_constraints(cert, value);
		break;
	case X509_EXT_SUBJECT_KEY_ID:
		error = x509_ext_subject_key_id(cert, value);
		break;
	case X509_EXT_AUTHORITY_KEY_ID:
		error = x509_ext_authority_key_id(cert, value);
		break;
	case X509_EXT_IGNORED:
		/* The policy extensions are not processed, whatever their critical flag. */
		break;
	default:
		/* An unprocessed extension matters only when it is critical. */
		if (critical)
			cert->flags |= CRYC_X509_UNKNOWN_CRITICAL;
		break;
	}

	/* Refuses a value that does not parse. */
	if (error != 0)
		return EINVAL;

	/* Succeeded: the extension was applied. */
	return 0;
}

/*
 * Opens the OCTET STRING that holds an extension's value: its contents
 * must be exactly one element with the given tag.
 */
static int
x509_open_value(
	const struct cryc_der_element *value,
	unsigned tag,
	struct cryc_der_element *element)
{
	struct cryc_der reader;
	int error;

	/* The contents are one DER element. */
	cryc_der_init(&reader, value->contents, value->length);
	error = cryc_der_expect(&reader, tag, element);
	if (error != 0)
		return EINVAL;

	/* Nothing may follow it. */
	if (reader.size != 0)
		return EINVAL;

	/* Succeeded: element is the value's one element. */
	return 0;
}

/*
 * Reads basicConstraints (RFC 5280 4.2.1.9: SEQUENCE { cA BOOLEAN DEFAULT
 * FALSE, pathLenConstraint INTEGER (0..MAX) OPTIONAL }).
 */
static int
x509_ext_basic_constraints(
	struct cryc_x509 *cert,
	const struct cryc_der_element *value)
{
	struct cryc_der inner;
	struct cryc_der_element sequence;
	struct cryc_der_element field;
	uint32_t path_len;
	int is_ca;
	int has_path_len;
	int present;
	int error;

	/* The value is one SEQUENCE. */
	error = x509_open_value(value, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return EINVAL;
	error = cryc_der_enter(&sequence, &inner);
	if (error != 0)
		return EINVAL;

	/* cA: TRUE when present (FALSE is the DEFAULT and is left out). */
	is_ca = 0;
	error = cryc_der_optional(&inner, CRYC_DER_BOOLEAN, &field, &present);
	if (error != 0)
		return EINVAL;
	if (present) {
		error = cryc_der_boolean(&field, &is_ca);
		if (error != 0)
			return EINVAL;
		if (!is_ca)
			return EINVAL;
	}

	/* pathLenConstraint: 0 to 255. */
	path_len = 0;
	error = cryc_der_optional(&inner, CRYC_DER_INTEGER, &field, &has_path_len);
	if (error != 0)
		return EINVAL;
	if (has_path_len) {
		error = cryc_der_small_integer(&field, X509_PATH_LEN_MAX, &path_len);
		if (error != 0)
			return EINVAL;
	}

	/* Refuses anything after the two fields. */
	if (inner.size != 0)
		return EINVAL;

	/* A path length constrains a CA only (4.2.1.9). */
	if (has_path_len && !is_ca)
		return EINVAL;

	/* Records the constraints. */
	cert->flags |= CRYC_X509_HAS_BASIC_CONSTRAINTS;
	if (is_ca)
		cert->flags |= CRYC_X509_CA;
	if (has_path_len) {
		cert->flags |= CRYC_X509_HAS_PATH_LEN;
		cert->path_len = path_len;
	}

	/* Succeeded: basicConstraints was recorded. */
	return 0;
}

/*
 * Reads keyUsage (RFC 5280 4.2.1.3: a BIT STRING of at most 9 bits, at
 * least one of them set).
 */
static int
x509_ext_key_usage(
	struct cryc_x509 *cert,
	const struct cryc_der_element *value)
{
	struct cryc_der_element element;
	const uint8_t *bytes;
	size_t size;
	size_t byte_index;
	unsigned unused;
	unsigned bit_index;
	uint32_t usage;
	uint32_t total_bits;
	int error;

	/* The value is one BIT STRING. */
	error = x509_open_value(value, CRYC_DER_BIT_STRING, &element);
	if (error != 0)
		return EINVAL;
	error = cryc_der_bit_string(&element, &bytes, &size, &unused);
	if (error != 0)
		return EINVAL;

	/* At least one byte and at most nine bits. */
	if (size == 0)
		return EINVAL;
	if (size > 2)
		return EINVAL;
	total_bits = (uint32_t)size * 8U - unused;
	if (total_bits > X509_KEY_USAGE_BITS)
		return EINVAL;

	/* KeyUsage bit i is the top bit of byte 0 for i = 0 and counts down from there, so byte b holds bits 8b to 8b + 7. */
	usage = 0;
	for (byte_index = 0; byte_index < size; byte_index++) {
		for (bit_index = 0; bit_index < 8U; bit_index++) {
			if ((bytes[byte_index] & (0x80U >> bit_index)) != 0)
				usage |= (uint32_t)1 << (byte_index * 8U + bit_index);
		}
	}

	/* At least one bit is set (4.2.1.3). */
	if (usage == 0)
		return EINVAL;

	/* Records the usage. */
	cert->flags |= CRYC_X509_HAS_KEY_USAGE;
	cert->key_usage = usage;

	/* Succeeded: keyUsage was recorded. */
	return 0;
}

/*
 * Reads extKeyUsage (RFC 5280 4.2.1.12: SEQUENCE SIZE (1..MAX) OF
 * KeyPurposeId).
 */
static int
x509_ext_ext_key_usage(
	struct cryc_x509 *cert,
	const struct cryc_der_element *value)
{
	struct cryc_der inner;
	struct cryc_der_element sequence;
	struct cryc_der_element purpose;
	unsigned bit;
	uint32_t usage;
	int found;
	int error;

	/* The value is one SEQUENCE with at least one purpose. */
	error = x509_open_value(value, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return EINVAL;
	error = cryc_der_enter(&sequence, &inner);
	if (error != 0)
		return EINVAL;
	if (inner.size == 0)
		return EINVAL;

	/* Records each purpose: the ones named, or other. */
	usage = 0;
	while (inner.size != 0) {
		error = cryc_der_expect(&inner, CRYC_DER_OID, &purpose);
		if (error != 0)
			return EINVAL;
		error = cryc_der_oid(&purpose);
		if (error != 0)
			return EINVAL;
		found = x509_oid_lookup(x509_purposes, &purpose, &bit);
		if (found) {
			usage |= bit;
		} else {
			usage |= CRYC_EKU_OTHER;
		}
	}

	/* Records the purposes. */
	cert->flags |= CRYC_X509_HAS_EXT_KEY_USAGE;
	cert->ext_key_usage = usage;

	/* Succeeded: extKeyUsage was recorded. */
	return 0;
}

/*
 * Reads subjectAltName (RFC 5280 4.2.1.6: GeneralNames, a SEQUENCE SIZE
 * (1..MAX) OF GeneralName).  Each GeneralName must be a well-formed
 * element; their meaning is checked when a name is matched.
 */
static int
x509_ext_subject_alt_name(
	struct cryc_x509 *cert,
	const struct cryc_der_element *value)
{
	struct cryc_der inner;
	struct cryc_der_element sequence;
	struct cryc_der_element name;
	int error;

	/* The value is one SEQUENCE with at least one name. */
	error = x509_open_value(value, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return EINVAL;
	error = cryc_der_enter(&sequence, &inner);
	if (error != 0)
		return EINVAL;
	if (inner.size == 0)
		return EINVAL;

	/* Every name is a well-formed element. */
	while (inner.size != 0) {
		error = cryc_der_next(&inner, &name);
		if (error != 0)
			return EINVAL;
	}

	/* Records the names. */
	cert->flags |= CRYC_X509_HAS_SAN;
	cert->san = sequence.contents;
	cert->san_size = sequence.length;

	/* Succeeded: subjectAltName was recorded. */
	return 0;
}

/*
 * Reads nameConstraints (RFC 5280 4.2.1.10: SEQUENCE { permittedSubtrees
 * [0] GeneralSubtrees OPTIONAL, excludedSubtrees [1] GeneralSubtrees
 * OPTIONAL }, at least one present).
 */
static int
x509_ext_name_constraints(
	struct cryc_x509 *cert,
	const struct cryc_der_element *value)
{
	struct cryc_der inner;
	struct cryc_der_element sequence;
	struct cryc_der_element permitted;
	struct cryc_der_element excluded;
	int has_permitted;
	int has_excluded;
	int error;

	/* The value is one SEQUENCE. */
	error = x509_open_value(value, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return EINVAL;
	error = cryc_der_enter(&sequence, &inner);
	if (error != 0)
		return EINVAL;

	/* The permitted subtrees [0] and the excluded subtrees [1], in this order. */
	error = cryc_der_optional(&inner, X509_TAG_EXPLICIT_0, &permitted, &has_permitted);
	if (error != 0)
		return EINVAL;
	error = cryc_der_optional(&inner, X509_TAG_EXPLICIT_1, &excluded, &has_excluded);
	if (error != 0)
		return EINVAL;
	if (inner.size != 0)
		return EINVAL;

	/* At least one of the two is present. */
	if (!has_permitted && !has_excluded)
		return EINVAL;

	/* Checks each list of subtrees. */
	if (has_permitted) {
		error = x509_check_subtrees(&permitted);
		if (error != 0)
			return EINVAL;
	}
	if (has_excluded) {
		error = x509_check_subtrees(&excluded);
		if (error != 0)
			return EINVAL;
	}

	/* Records the lists: views of the GeneralSubtree sequences. */
	cert->flags |= CRYC_X509_HAS_NAME_CONSTRAINTS;
	if (has_permitted) {
		cert->permitted = permitted.contents;
		cert->permitted_size = permitted.length;
	}
	if (has_excluded) {
		cert->excluded = excluded.contents;
		cert->excluded_size = excluded.length;
	}

	/* Succeeded: nameConstraints was recorded. */
	return 0;
}

/*
 * Checks GeneralSubtrees (RFC 5280 4.2.1.10: SIZE (1..MAX) OF
 * GeneralSubtree ::= SEQUENCE { base GeneralName, minimum [0] BaseDistance
 * DEFAULT 0, maximum [1] BaseDistance OPTIONAL }): each base is a
 * context-specific element, a present minimum is 0 and a maximum is
 * refused (the profile of 4.2.1.10).
 */
static int
x509_check_subtrees(
	const struct cryc_der_element *subtrees)
{
	struct cryc_der list;
	struct cryc_der item;
	struct cryc_der_element subtree;
	struct cryc_der_element base;
	struct cryc_der_element minimum;
	uint32_t distance;
	int present;
	int error;

	/* The list has at least one subtree. */
	error = cryc_der_enter(subtrees, &list);
	if (error != 0)
		return EINVAL;
	if (list.size == 0)
		return EINVAL;

	/* Checks every subtree. */
	while (list.size != 0) {
		error = cryc_der_expect(&list, CRYC_DER_SEQUENCE, &subtree);
		if (error != 0)
			return EINVAL;
		error = cryc_der_enter(&subtree, &item);
		if (error != 0)
			return EINVAL;

		/* The base is a GeneralName: a context-specific tag. */
		error = cryc_der_next(&item, &base);
		if (error != 0)
			return EINVAL;
		if ((base.tag & X509_TAG_CLASS_MASK) != CRYC_DER_CONTEXT)
			return EINVAL;

		/* A minimum, when present, is 0. */
		error = cryc_der_optional(&item, X509_TAG_IMPLICIT_0, &minimum, &present);
		if (error != 0)
			return EINVAL;
		if (present) {
			error = cryc_der_small_integer(&minimum, 0, &distance);
			if (error != 0)
				return EINVAL;
		}

		/* A maximum, or anything else, is refused. */
		if (item.size != 0)
			return EINVAL;
	}

	/* Succeeded: every subtree is well formed. */
	return 0;
}

/*
 * Reads subjectKeyIdentifier (RFC 5280 4.2.1.2: an OCTET STRING).
 */
static int
x509_ext_subject_key_id(
	struct cryc_x509 *cert,
	const struct cryc_der_element *value)
{
	struct cryc_der_element element;
	int error;

	/* The value is one OCTET STRING. */
	error = x509_open_value(value, CRYC_DER_OCTET_STRING, &element);
	if (error != 0)
		return EINVAL;

	/* Records the identifier. */
	cert->ski = element.contents;
	cert->ski_size = element.length;

	/* Succeeded: subjectKeyIdentifier was recorded. */
	return 0;
}

/*
 * Reads authorityKeyIdentifier (RFC 5280 4.2.1.1: SEQUENCE { keyIdentifier
 * [0] OCTET STRING OPTIONAL, authorityCertIssuer [1] GeneralNames
 * OPTIONAL, authorityCertSerialNumber [2] INTEGER OPTIONAL }); only the
 * key identifier is kept.
 */
static int
x509_ext_authority_key_id(
	struct cryc_x509 *cert,
	const struct cryc_der_element *value)
{
	struct cryc_der inner;
	struct cryc_der names;
	struct cryc_der_element sequence;
	struct cryc_der_element key_id;
	struct cryc_der_element issuer;
	struct cryc_der_element serial;
	struct cryc_der_element name;
	int has_key_id;
	int has_issuer;
	int has_serial;
	int error;

	/* The value is one SEQUENCE. */
	error = x509_open_value(value, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return EINVAL;
	error = cryc_der_enter(&sequence, &inner);
	if (error != 0)
		return EINVAL;

	/* The three optional fields, in order. */
	error = cryc_der_optional(&inner, X509_TAG_IMPLICIT_0, &key_id, &has_key_id);
	if (error != 0)
		return EINVAL;
	error = cryc_der_optional(&inner, X509_TAG_EXPLICIT_1, &issuer, &has_issuer);
	if (error != 0)
		return EINVAL;
	error = cryc_der_optional(&inner, X509_TAG_IMPLICIT_2, &serial, &has_serial);
	if (error != 0)
		return EINVAL;
	if (inner.size != 0)
		return EINVAL;

	/* The issuer names are well-formed elements. */
	if (has_issuer) {
		error = cryc_der_enter(&issuer, &names);
		if (error != 0)
			return EINVAL;
		while (names.size != 0) {
			error = cryc_der_next(&names, &name);
			if (error != 0)
				return EINVAL;
		}
	}

	/* The serial number is an INTEGER's contents: at least one octet. */
	if (has_serial) {
		if (serial.length == 0)
			return EINVAL;
	}

	/* Records the key identifier when there is one. */
	if (has_key_id) {
		cert->aki = key_id.contents;
		cert->aki_size = key_id.length;
	}

	/* Succeeded: authorityKeyIdentifier was recorded. */
	return 0;
}
