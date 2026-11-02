/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the trust store and the path validation of cryc-chain.h.
 *
 * The certificates are made here, with derwriter.h and the ECDSA signer of
 * tlspeer.h, from RFC 5280 4.1 and 4.2: each test builds the chain it needs
 * (names, keys, validity, basicConstraints, keyUsage, extKeyUsage,
 * nameConstraints, unknown critical extensions, signature faults), and the
 * expected outcome of each is written by hand from RFC 5280 6.1 and from
 * the order of checks of cryc-chain.h: path building, the distrusted marks,
 * the signature, validity, name constraints, critical extensions and the
 * role of each certificate.  The store is checked for duplicates, the
 * limits, PEM text and files, the system's roots, and use from several
 * threads.  The x509-limbo cases whose ids begin rfc5280::, pathlen::,
 * webpki:: or pathological:: are the outside corpus; the cases where the
 * outcome differs from the case's expectation are listed with the reason.
 */

#include "check.h"
#include "vecfile.h"
#include "tlspeer.h"

#include "userland/base/libcrypto-compat/cryc-chain.h"
#include "userland/base/libcrypto-compat/cryc-der.h"
#include "userland/base/libcrypto-compat/cryc-pem.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

/* The room of one certificate this test builds, and how many may be alive at once. */
#define CERT_ROOM	2048U
#define CERT_POOL	400U

/* The keys of the tests, made on demand: indexes below 28 are P-256 keys, the rest P-384. */
#define KEYS_MAX	32U
#define KEYS_P384_FIRST	28U

/* The time the tests validate at (TLSPEER_PKI_TIME, 2026-07-01 00:00:00 UTC) and the UTCTime text of it. */
#define NOW		((int64_t)TLSPEER_PKI_TIME)
#define NOW_TEXT	"260701000000Z"

/* The most certificates of one x509-limbo case this test hands to a validation: the peer and 15 more. */
#define PEER_MAX	16U

/* The room of a peer table of a test, which may hold more than the validation accepts. */
#define PEER_ROOM	20U

/* The largest certificate of the x509-limbo cases is about 78 KB. */
#define LIMBO_MAX	131072U

/* The most certificates of one x509-limbo case: the peer, four trusted and 120 untrusted. */
#define LIMBO_CERTS	125U

/* The threads of the multi-thread test, and the certificates each adds. */
#define THREAD_COUNT	2U
#define THREAD_ADDS	150U

/* The values of struct spec's eku. */
#define EKU_NONE	0
#define EKU_SERVER	1
#define EKU_CLIENT	2
#define EKU_ANY		3
#define EKU_BOTH	4

/* The values of struct spec's sig_kind. */
#define SIG_NORMAL	0
#define SIG_SHA1	1
#define SIG_CORRUPT	2
#define SIG_RSA_ALG	3

/* The values of struct spec's key_kind. */
#define KEY_EC		0
#define KEY_RSA_1024	1
#define KEY_RSA_2048	2
#define KEY_UNKNOWN	3
#define KEY_RSA_2049	4
#define KEY_RSA_2047	5
#define KEY_RSA_HUGE	6

/* The values of struct spec's nc_mode. */
#define NC_NONE		0
#define NC_CRITICAL	1
#define NC_NONCRITICAL	2

/* The KeyUsage named bits (RFC 5280 4.2.1.3) as a mask of bit numbers. */
#define KU_DIGITAL_SIGNATURE	(1U << 0)
#define KU_KEY_ENCIPHERMENT	(1U << 2)
#define KU_KEY_CERT_SIGN	(1U << 5)
#define KU_CRL_SIGN		(1U << 6)

/* More object identifiers (the others are in tlspeer.h). */
#define OID_ECDSA_SHA1		"1.2.840.10045.4.1"
#define OID_RSA_ENCRYPTION	"1.2.840.113549.1.1.1"
#define OID_RSA_SHA256		"1.2.840.113549.1.1.11"
#define OID_ED25519		"1.3.101.112"
#define OID_NAME_CONSTRAINTS	"2.5.29.30"
#define OID_CERT_POLICIES	"2.5.29.32"
#define OID_CLIENT_AUTH		"1.3.6.1.5.5.7.3.2"
#define OID_ANY_EKU		"2.5.29.37.0"
#define OID_UNKNOWN_EXTENSION	"1.3.6.1.4.1.99999.1"

/*
 * What one certificate of a test is made of.  spec_init gives a version 3
 * certificate valid from 2026-01-01 to 2036-01-01 that is not a CA, with a
 * subjectKeyIdentifier and an authorityKeyIdentifier and nothing else; a
 * test sets what it wants different.
 *
 *   subject, issuer   commonName text of each Name.
 *   subject_key       index of the key whose public half is in the
 *                     certificate (see key_kind).
 *   issuer_key        index of the key that signs it.
 *   version           3, or 1 (no extensions at all).
 *   basic             0: no basicConstraints; 1: cA TRUE; 2: cA FALSE.
 *   path_len          pathLenConstraint, or -1 for none (basic is 1).
 *   has_ku, ku        keyUsage with these named bits.
 *   eku               EKU_ value.
 *   dns, address      subjectAltName entries; san_extra is more of its
 *                     GeneralNames, already encoded.
 *   nc_mode ...       nameConstraints and the GeneralSubtrees contents.
 *   unknown           1: an unknown critical extension; 2: not critical.
 *   policies          1: a critical certificatePolicies extension.
 *   ski, aki          1: the key identifiers; aki 2: a wrong one; aki 3:
 *                     the right one cut by a byte.
 *   sig_kind          SIG_ value.
 *   key_kind          KEY_ value: what SubjectPublicKeyInfo holds.
 */
struct spec {
	const char *subject;
	const char *issuer;
	unsigned subject_key;
	unsigned issuer_key;
	uint32_t serial;
	const char *not_before;
	const char *not_after;
	int version;
	int basic;
	int basic_critical;
	int path_len;
	int has_ku;
	unsigned ku;
	int eku;
	const char *dns;
	const uint8_t *address;
	size_t address_size;
	const uint8_t *san_extra;
	size_t san_extra_size;
	int nc_mode;
	const uint8_t *nc_permitted;
	size_t nc_permitted_size;
	const uint8_t *nc_excluded;
	size_t nc_excluded_size;
	int unknown;
	int policies;
	int ski;
	int aki;
	int sig_kind;
	int key_kind;
};

/*
 * A certificate made by a test: its DER, and its parse whose views point
 * into the DER (so the structure never moves while it is used).
 */
struct built {
	unsigned char der[CERT_ROOM];
	size_t size;
	struct cryc_x509 cert;
};

/* The keys made so far, and which are made. */
static struct tlspeer_key keys[KEYS_MAX];
static int keys_made[KEYS_MAX];

/* The pool of certificates and how many are used; a test function puts the count back when it ends. */
static struct built pool[CERT_POOL];
static unsigned pool_used;

/* The directory of the program, where the test writes its files. */
static char test_dir[1024];

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x2545f491U;

/* Returns the next pseudo-random 32-bit value. */
static uint32_t
rng_next(void)
{
	/* Marsaglia's xorshift with the shifts 13, 17 and 5. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* Returns the key of an index, making it on first use from a label. */
static const struct tlspeer_key *
key_get(
	unsigned index)
{
	char label[32];
	enum cryc_curve_id curve;
	int error;

	/* Makes the key when it is first asked for. */
	if (keys_made[index] == 0) {
		sprintf(label, "chain test key %u", index);
		curve = CRYC_CURVE_P256;
		if (index >= KEYS_P384_FIRST)
			curve = CRYC_CURVE_P384;
		error = tlspeer_key_from_label(&keys[index], curve, label);
		CHECK(error == 0);
		keys_made[index] = 1;
	}
	return &keys[index];
}

/* Starts a spec: version 3, valid 2026 to 2036, not a CA, key identifiers, signed by key issuer_key. */
static void
spec_init(
	struct spec *spec,
	const char *subject,
	unsigned subject_key,
	const char *issuer,
	unsigned issuer_key)
{
	/* Everything absent, then the defaults. */
	memset(spec, 0, sizeof(*spec));
	spec->subject = subject;
	spec->issuer = issuer;
	spec->subject_key = subject_key;
	spec->issuer_key = issuer_key;
	spec->serial = 1;
	spec->not_before = TLSPEER_NOT_BEFORE;
	spec->not_after = TLSPEER_NOT_AFTER;
	spec->version = 3;
	spec->basic = 2;
	spec->basic_critical = 1;
	spec->path_len = -1;
	spec->ski = 1;
	spec->aki = 1;
	spec->has_ku = 1;
	spec->ku = KU_DIGITAL_SIGNATURE;
}

/* Makes the spec a CA: basicConstraints cA TRUE and keyUsage keyCertSign and cRLSign. */
static void
spec_ca(
	struct spec *spec)
{
	/* The CA's extensions. */
	spec->basic = 1;
	spec->has_ku = 1;
	spec->ku = KU_KEY_CERT_SIGN | KU_CRL_SIGN;
}

/* Writes one extension with a SEQUENCE value built by the caller into the list. */
static void
put_extension(
	struct der_writer *list,
	const char *oid,
	int critical,
	const struct der_writer *value)
{
	/* The tool writes the extnID, the critical flag and the OCTET STRING. */
	tlspeer_der_extension(list, oid, critical, value);
}

/* Writes the keyUsage BIT STRING of the named bits in mask (X.690 8.6 and 11.2.2: trailing zero bits dropped). */
static void
put_key_usage_bits(
	struct der_writer *value,
	unsigned mask)
{
	unsigned char bits[3];
	unsigned highest;
	unsigned bit;
	size_t bytes;

	/* The highest named bit decides the length. */
	highest = 0;
	for (bit = 0; bit < 9U; bit++) {
		if ((mask & (1U << bit)) != 0U)
			highest = bit;
	}
	bytes = highest / 8U + 1U;
	memset(bits, 0, sizeof(bits));
	for (bit = 0; bit < 9U; bit++) {
		if ((mask & (1U << bit)) != 0U)
			bits[1U + bit / 8U] = (unsigned char)(bits[1U + bit / 8U] | (0x80U >> (bit % 8U)));
	}

	/* The first octet counts the unused bits of the last. */
	bits[0] = (unsigned char)(7U - highest % 8U);
	der_put(value, 0x03U, bits, 1U + bytes);
}

/* Writes the SubjectPublicKeyInfo of a key kind. */
static void
put_spki(
	struct der_writer *writer,
	const struct spec *spec)
{
	unsigned char algorithm_bytes[64];
	unsigned char key_bytes[1200];
	unsigned char spki_bytes[1300];
	unsigned char modulus[1100];
	static const unsigned char exponent[3] = { 0x01, 0x00, 0x01 };
	unsigned char bits[1300];
	static const unsigned char null_value[1] = { 0 };
	struct der_writer algorithm;
	struct der_writer rsa;
	struct der_writer spki;
	size_t modulus_size;
	size_t index;

	/* An EC key is the tool's own encoding. */
	if (spec->key_kind == KEY_EC) {
		tlspeer_der_spki(writer, key_get(spec->subject_key));
		return;
	}

	/* An unknown algorithm: an Ed25519 key, which the parser reads as unsupported. */
	if (spec->key_kind == KEY_UNKNOWN) {
		der_init(&algorithm, algorithm_bytes, sizeof(algorithm_bytes));
		der_oid(&algorithm, OID_ED25519);
		memset(bits, 0, 33);
		bits[1] = 0x42;
		der_init(&spki, spki_bytes, sizeof(spki_bytes));
		der_wrap(&spki, 0x30U, &algorithm);
		der_put(&spki, 0x03U, bits, 33);
		der_wrap(writer, 0x30U, &spki);
		return;
	}

	/* An RSA key of a made-up modulus (odd, top bit set) of the wanted length; it only has to parse. */
	modulus_size = 128;
	if (spec->key_kind == KEY_RSA_2048)
		modulus_size = 256;
	if (spec->key_kind == KEY_RSA_2049)
		modulus_size = 257;
	if (spec->key_kind == KEY_RSA_2047)
		modulus_size = 256;
	if (spec->key_kind == KEY_RSA_HUGE)
		modulus_size = 1025;
	for (index = 0; index < modulus_size; index++)
		modulus[index] = (unsigned char)(0xc3U ^ (index * 7U));
	modulus[0] = 0xc3;
	if (spec->key_kind == KEY_RSA_2049)
		modulus[0] = 0x01;
	if (spec->key_kind == KEY_RSA_2047)
		modulus[0] = 0x7f;
	modulus[modulus_size - 1U] = (unsigned char)(modulus[modulus_size - 1U] | 1U);
	der_init(&rsa, key_bytes, sizeof(key_bytes));
	der_unsigned(&rsa, modulus, modulus_size);
	der_unsigned(&rsa, exponent, sizeof(exponent));
	der_init(&spki, spki_bytes, sizeof(spki_bytes));
	der_wrap(&spki, 0x30U, &rsa);
	bits[0] = 0;
	memcpy(bits + 1, spki.data, spki.used);
	modulus_size = spki.used;
	der_init(&algorithm, algorithm_bytes, sizeof(algorithm_bytes));
	der_oid(&algorithm, OID_RSA_ENCRYPTION);
	der_put(&algorithm, 0x05U, null_value, 0);
	der_init(&spki, spki_bytes, sizeof(spki_bytes));
	der_wrap(&spki, 0x30U, &algorithm);
	der_put(&spki, 0x03U, bits, 1U + modulus_size);
	der_wrap(writer, 0x30U, &spki);
}

/* Writes the extensions of a certificate; returns 0 or ENOBUFS. */
static int
put_extensions(
	struct der_writer *writer,
	const struct spec *spec)
{
	static const unsigned char true_value[1] = { 0xff };
	unsigned char list_bytes[1000];
	unsigned char value_bytes[400];
	unsigned char inner_bytes[400];
	unsigned char outer_bytes[1020];
	unsigned char extra_bytes[400];
	uint8_t identifier[20];
	struct der_writer list;
	struct der_writer value;
	struct der_writer inner;
	struct der_writer outer;
	struct der_writer extra;
	unsigned char length_octet;
	int error;

	/* Extensions ::= SEQUENCE OF Extension (4.1). */
	der_init(&list, list_bytes, sizeof(list_bytes));

	/* basicConstraints: cA TRUE (with an optional pathLenConstraint) or the empty SEQUENCE (4.2.1.9). */
	if (spec->basic != 0) {
		der_init(&inner, inner_bytes, sizeof(inner_bytes));
		if (spec->basic == 1)
			der_put(&inner, 0x01U, true_value, 1);
		if (spec->path_len >= 0) {
			length_octet = (unsigned char)spec->path_len;
			der_put(&inner, 0x02U, &length_octet, 1);
		}
		der_init(&value, value_bytes, sizeof(value_bytes));
		der_wrap(&value, 0x30U, &inner);
		put_extension(&list, TLSPEER_OID_BASIC_CONSTRAINTS, spec->basic_critical, &value);
	}

	/* keyUsage (4.2.1.3). */
	if (spec->has_ku != 0) {
		der_init(&value, value_bytes, sizeof(value_bytes));
		put_key_usage_bits(&value, spec->ku);
		put_extension(&list, TLSPEER_OID_KEY_USAGE, 1, &value);
	}

	/* extKeyUsage (4.2.1.12). */
	if (spec->eku != EKU_NONE) {
		der_init(&inner, inner_bytes, sizeof(inner_bytes));
		if (spec->eku == EKU_SERVER || spec->eku == EKU_BOTH)
			der_oid(&inner, TLSPEER_OID_SERVER_AUTH);
		if (spec->eku == EKU_CLIENT || spec->eku == EKU_BOTH)
			der_oid(&inner, OID_CLIENT_AUTH);
		if (spec->eku == EKU_ANY)
			der_oid(&inner, OID_ANY_EKU);
		der_init(&value, value_bytes, sizeof(value_bytes));
		der_wrap(&value, 0x30U, &inner);
		put_extension(&list, TLSPEER_OID_EXT_KEY_USAGE, 0, &value);
	}

	/* subjectAltName (4.2.1.6). */
	if (spec->dns != NULL || spec->address != NULL || spec->san_extra_size != 0U) {
		der_init(&inner, inner_bytes, sizeof(inner_bytes));
		if (spec->dns != NULL)
			der_put(&inner, 0x82U, (const unsigned char *)spec->dns, strlen(spec->dns));
		if (spec->address != NULL)
			der_put(&inner, 0x87U, spec->address, spec->address_size);
		if (spec->san_extra_size != 0U)
			der_raw(&inner, spec->san_extra, spec->san_extra_size);
		der_init(&value, value_bytes, sizeof(value_bytes));
		der_wrap(&value, 0x30U, &inner);
		put_extension(&list, TLSPEER_OID_SUBJECT_ALT_NAME, 0, &value);
	}

	/* nameConstraints: permitted [0] and excluded [1], each the GeneralSubtrees (4.2.1.10). */
	if (spec->nc_mode != NC_NONE) {
		der_init(&inner, inner_bytes, sizeof(inner_bytes));
		if (spec->nc_permitted_size != 0U)
			der_put(&inner, 0xa0U, spec->nc_permitted, spec->nc_permitted_size);
		if (spec->nc_excluded_size != 0U)
			der_put(&inner, 0xa1U, spec->nc_excluded, spec->nc_excluded_size);
		der_init(&value, value_bytes, sizeof(value_bytes));
		der_wrap(&value, 0x30U, &inner);
		put_extension(&list, OID_NAME_CONSTRAINTS, spec->nc_mode == NC_CRITICAL, &value);
	}

	/* A critical certificatePolicies (recognized, not processed): one policy, anyPolicy (4.2.1.4). */
	if (spec->policies != 0) {
		der_init(&extra, extra_bytes, sizeof(extra_bytes));
		der_oid(&extra, "2.5.29.32.0");
		der_init(&inner, inner_bytes, sizeof(inner_bytes));
		der_wrap(&inner, 0x30U, &extra);
		der_init(&value, value_bytes, sizeof(value_bytes));
		der_wrap(&value, 0x30U, &inner);
		put_extension(&list, OID_CERT_POLICIES, 1, &value);
	}

	/* An extension nobody here knows, critical or not (4.2: a critical one must be refused). */
	if (spec->unknown != 0) {
		der_init(&value, value_bytes, sizeof(value_bytes));
		der_put(&value, 0x05U, true_value, 0);
		put_extension(&list, OID_UNKNOWN_EXTENSION, spec->unknown == 1, &value);
	}

	/* subjectKeyIdentifier (4.2.1.2). */
	if (spec->ski != 0) {
		error = tlspeer_key_id(key_get(spec->subject_key), identifier);
		CHECK(error == 0);
		der_init(&value, value_bytes, sizeof(value_bytes));
		der_put(&value, 0x04U, identifier, sizeof(identifier));
		put_extension(&list, TLSPEER_OID_SUBJECT_KEY_ID, 0, &value);
	}

	/* authorityKeyIdentifier { keyIdentifier [0] } (4.2.1.1); aki 2 names another key. */
	if (spec->aki != 0) {
		error = tlspeer_key_id(key_get(spec->aki == 2 ? 30U : spec->issuer_key), identifier);
		CHECK(error == 0);
		der_init(&inner, inner_bytes, sizeof(inner_bytes));
		der_put(&inner, 0x80U, identifier, spec->aki == 3 ? sizeof(identifier) - 1U : sizeof(identifier));
		der_init(&value, value_bytes, sizeof(value_bytes));
		der_wrap(&value, 0x30U, &inner);
		put_extension(&list, TLSPEER_OID_AUTHORITY_KEY_ID, 0, &value);
	}

	/* [3] EXPLICIT Extensions. */
	der_init(&outer, outer_bytes, sizeof(outer_bytes));
	der_wrap(&outer, 0x30U, &list);
	der_wrap(writer, 0xa3U, &outer);
	if (writer->error != 0)
		return ENOBUFS;
	return 0;
}

/* Writes the AlgorithmIdentifier of a signature (sig_kind picks the algorithm; key size picks the hash). */
static void
put_sigalg(
	struct der_writer *writer,
	const struct spec *spec)
{
	unsigned char inner_bytes[32];
	static const unsigned char null_value[1] = { 0 };
	struct der_writer inner;
	const struct tlspeer_key *key;

	/* The algorithm of the kind. */
	key = key_get(spec->issuer_key);
	der_init(&inner, inner_bytes, sizeof(inner_bytes));
	if (spec->sig_kind == SIG_SHA1) {
		der_oid(&inner, OID_ECDSA_SHA1);
	} else if (spec->sig_kind == SIG_RSA_ALG) {
		der_oid(&inner, OID_RSA_SHA256);
		der_put(&inner, 0x05U, null_value, 0);
	} else if (key->ec.size == 48U) {
		der_oid(&inner, TLSPEER_OID_ECDSA_SHA384);
	} else {
		der_oid(&inner, TLSPEER_OID_ECDSA_SHA256);
	}
	der_wrap(writer, 0x30U, &inner);
}

/*
 * Makes the certificate a spec describes, parses it, and returns it from
 * the pool.  The parse must succeed (a certificate that must not parse is
 * not made with this).
 */
static struct built *
make(
	const struct spec *spec)
{
	static const unsigned char version3[1] = { 0x02 };
	unsigned char version_bytes[8];
	unsigned char validity_bytes[40];
	unsigned char tbs_bytes[CERT_ROOM];
	unsigned char body_bytes[CERT_ROOM];
	unsigned char cert_bytes[CERT_ROOM];
	unsigned char signature_bytes[400];
	uint8_t serial[4];
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t signature[300];
	size_t signature_size;
	struct der_writer version;
	struct der_writer validity;
	struct der_writer tbs;
	struct der_writer body;
	struct der_writer cert;
	struct der_writer final;
	struct built *built;
	enum cryc_hash_id hash;
	const struct tlspeer_key *signer;
	int error;

	/* A certificate from the pool. */
	CHECK(pool_used < CERT_POOL);
	built = &pool[pool_used++];

	/* version [0] EXPLICIT (version 3 only: version 1 is the default and is not written) and the serial number. */
	der_init(&tbs, tbs_bytes, sizeof(tbs_bytes));
	if (spec->version == 3) {
		der_init(&version, version_bytes, sizeof(version_bytes));
		der_put(&version, 0x02U, version3, 1);
		der_wrap(&tbs, 0xa0U, &version);
	}
	serial[0] = (uint8_t)(spec->serial >> 24);
	serial[1] = (uint8_t)(spec->serial >> 16);
	serial[2] = (uint8_t)(spec->serial >> 8);
	serial[3] = (uint8_t)spec->serial;
	der_unsigned(&tbs, serial, sizeof(serial));

	/* signature, issuer, validity, subject, subjectPublicKeyInfo. */
	put_sigalg(&tbs, spec);
	tlspeer_der_name(&tbs, spec->issuer);
	der_init(&validity, validity_bytes, sizeof(validity_bytes));
	der_put(&validity, 0x17U, (const unsigned char *)spec->not_before, strlen(spec->not_before));
	der_put(&validity, 0x17U, (const unsigned char *)spec->not_after, strlen(spec->not_after));
	der_wrap(&tbs, 0x30U, &validity);
	tlspeer_der_name(&tbs, spec->subject);
	put_spki(&tbs, spec);

	/* The extensions of a version 3 certificate. */
	if (spec->version == 3) {
		error = put_extensions(&tbs, spec);
		CHECK(error == 0);
	}
	der_init(&body, body_bytes, sizeof(body_bytes));
	der_wrap(&body, 0x30U, &tbs);
	CHECK(body.error == 0);

	/* The signature over the tbsCertificate by the issuer's key; an RSA algorithm gets made-up bytes. */
	signer = key_get(spec->issuer_key);
	hash = CRYC_HASH_SHA256;
	if (signer->ec.size == 48U)
		hash = CRYC_HASH_SHA384;
	error = cryc_hash_digest(hash, body.data, body.used, digest);
	CHECK(error == 0);
	signature[0] = 0;
	if (spec->sig_kind == SIG_RSA_ALG) {
		memset(signature + 1, 0x5a, 256);
		signature[1] = 0x11;
		signature_size = 256;
	} else {
		error = tlspeer_ecdsa_sign_der(signer, digest, cryc_hash_size(hash), signature + 1, sizeof(signature) - 1U, &signature_size);
		CHECK(error == 0);
	}
	if (spec->sig_kind == SIG_CORRUPT)
		signature[signature_size] = (uint8_t)(signature[signature_size] ^ 0x01U);

	/* Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue }. */
	der_init(&cert, cert_bytes, sizeof(cert_bytes));
	der_raw(&cert, body.data, body.used);
	put_sigalg(&cert, spec);
	der_init(&final, signature_bytes, sizeof(signature_bytes));
	der_put(&final, 0x03U, signature, 1U + signature_size);
	der_raw(&cert, final.data, final.used);
	der_init(&final, built->der, sizeof(built->der));
	der_wrap(&final, 0x30U, &cert);
	CHECK(cert.error == 0 && final.error == 0);
	built->size = final.used;

	/* The parse, whose views point into the DER of the pool entry. */
	memset(&built->cert, 0, sizeof(built->cert));
	error = cryc_x509_parse(&built->cert, built->der, built->size);
	CHECK(error == 0);
	return built;
}

/* A self-signed root CA called name with key index key. */
static struct built *
make_root(
	const char *name,
	unsigned key)
{
	struct spec spec;

	/* A CA whose issuer is itself. */
	spec_init(&spec, name, key, name, key);
	spec_ca(&spec);
	spec.aki = 0;
	return make(&spec);
}

/* A CA called name (key index key) issued by issuer (key index issuer_key). */
static struct built *
make_ca(
	const char *name,
	unsigned key,
	const char *issuer,
	unsigned issuer_key)
{
	struct spec spec;

	/* A CA below another. */
	spec_init(&spec, name, key, issuer, issuer_key);
	spec_ca(&spec);
	return make(&spec);
}

/* A TLS server certificate for host (a subjectAltName dNSName) issued by issuer. */
static struct built *
make_leaf(
	const char *host,
	unsigned key,
	const char *issuer,
	unsigned issuer_key)
{
	struct spec spec;

	/* A leaf with serverAuth. */
	spec_init(&spec, host, key, issuer, issuer_key);
	spec.dns = host;
	spec.eku = EKU_SERVER;
	return make(&spec);
}

/* The default parameters: validate at NOW, no name, the default depth. */
static void
params_init(
	struct cryc_verify_params *params)
{
	/* Only the time is set. */
	memset(params, 0, sizeof(*params));
	params->has_time = 1;
	params->time = NOW;
}

/*
 * The peer table of the last validation run(): the result's chain points
 * into it (and into the store), so it must outlive the call; it is
 * overwritten by the next run.
 */
static struct cryc_x509 run_peer[PEER_ROOM];

/* Runs one validation of the certificates (a list of built pointers, leaf first); the result is in *result. */
static void
run(
	struct cryc_store *store,
	struct built **list,
	size_t count,
	const struct cryc_verify_params *params,
	struct cryc_verify_result *result)
{
	size_t index;
	int error;

	/* The peer table the validation takes is a copy of the parses (their views stay in the pool). */
	CHECK(count <= PEER_ROOM);
	for (index = 0; index < count; index++)
		run_peer[index] = list[index]->cert;
	error = cryc_chain_verify(store, run_peer, count, params, result);
	CHECK(error == 0);
}

/* Reports whether a result is what is wanted, with a line to find the case. */
static void
expect(
	const struct cryc_verify_result *result,
	enum cryc_verify_code code,
	size_t depth,
	int line)
{
	int good;

	/* The code and the depth together. */
	good = result->code == code && result->depth == depth;
	if (!check_one(good, __FILE__, line, "verify result")) {
		printf("  line %d: got code %d depth %u, wanted code %d depth %u\n", line, (int)result->code, (unsigned)result->depth, (int)code, (unsigned)depth);
	}
}

#define EXPECT(result, code, depth) expect(&(result), (code), (depth), __LINE__)

/* A store with the given certificates added (made fresh each time; the test frees it). */
static void
store_with(
	struct cryc_store *store,
	struct built **list,
	size_t count)
{
	size_t index;
	int error;

	/* Makes the store and adds each certificate. */
	error = cryc_store_init(store);
	CHECK(error == 0);
	for (index = 0; index < count; index++) {
		error = cryc_store_add_der(store, list[index]->der, list[index]->size);
		CHECK(error == 0);
	}
}

/* Makes a PEM block of a DER with a label into out (RFC 7468 2: 64 characters per line); returns the length. */
static size_t
pem_encode(
	const char *label,
	const unsigned char *der,
	size_t size,
	char *out)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t used;
	size_t index;
	size_t column;
	unsigned long group;
	unsigned count;
	unsigned digit;

	/* The BEGIN line. */
	used = (size_t)sprintf(out, "-----BEGIN %s-----\n", label);

	/* The base64 body: each 3 bytes make 4 characters (RFC 4648 4), 64 characters to a line. */
	column = 0;
	for (index = 0; index < size; index += 3U) {
		count = 3;
		if (size - index < 3U)
			count = (unsigned)(size - index);
		group = (unsigned long)der[index] << 16;
		if (count > 1U)
			group |= (unsigned long)der[index + 1U] << 8;
		if (count > 2U)
			group |= (unsigned long)der[index + 2U];
		for (digit = 0; digit < 4U; digit++) {
			if (digit > count)
				out[used++] = '=';
			else
				out[used++] = alphabet[(group >> (18U - 6U * digit)) & 0x3fU];
			column++;
			if (column == 64U) {
				out[used++] = '\n';
				column = 0;
			}
		}
	}
	if (column != 0U)
		out[used++] = '\n';

	/* The END line. */
	used += (size_t)sprintf(out + used, "-----END %s-----\n", label);
	return used;
}

/* Writes a file in the test directory in binary mode; returns 1 on success. */
static int
write_file(
	const char *name,
	const unsigned char *bytes,
	size_t size,
	char *path_out)
{
	FILE *file;
	size_t put;

	/* The path is the directory, a slash and the name. */
	sprintf(path_out, "%s/%s", test_dir, name);
	file = fopen(path_out, "wb");
	if (file == NULL)
		return 0;
	put = size == 0U ? 0U : fwrite(bytes, 1, size, file);
	if (fclose(file) != 0)
		return 0;
	return put == size;
}

/* Makes a certificate for the store tests (a root, key 0) and returns it. */
static struct built *
store_cert(
	const char *name,
	uint32_t serial)
{
	struct spec spec;

	/* A self-signed CA of the name and serial. */
	spec_init(&spec, name, 0, name, 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.serial = serial;
	return make(&spec);
}

/* The basic behaviour of the store: init, add, duplicates, copies, stable entries, free. */
static void
test_store_basics(void)
{
	struct cryc_store store;
	struct built *first;
	struct built *other;
	struct cryc_store_entry *entries[24];
	unsigned char copy[CERT_ROOM];
	unsigned char garbage[40];
	uint8_t digest[CRYC_SHA256_SIZE];
	unsigned mark;
	unsigned index;
	char name[32];
	int error;

	mark = pool_used;

	/* Init needs a store; the store starts empty. */
	CHECK(cryc_store_init(NULL) == EINVAL);
	CHECK(cryc_store_init(&store) == 0);
	CHECK(store.count == 0U);
	CHECK(store.distrusted_count == 0U);
	CHECK(store.lock.impl != NULL);

	/* A certificate is copied: the entry's DER is not the caller's, equal to it, parsed over the copy. */
	first = store_cert("Store Root", 1);
	memcpy(copy, first->der, first->size);
	CHECK(cryc_store_add_der(&store, copy, first->size) == 0);
	CHECK(store.count == 1U);
	memset(copy, 0, sizeof(copy));
	CHECK(store.entries[0]->der_size == first->size);
	CHECK_BYTES(store.entries[0]->der, first->der, first->size);
	CHECK(store.entries[0]->der != copy);
	CHECK(store.entries[0]->cert.der == store.entries[0]->der);
	CHECK(store.entries[0]->cert.der_size == first->size);
	CHECK(store.entries[0]->cert.subject_size == first->cert.subject_size);

	/* The digest is the SHA-256 of the DER. */
	error = cryc_hash_digest(CRYC_HASH_SHA256, first->der, first->size, digest);
	CHECK(error == 0);
	CHECK_BYTES(store.entries[0]->digest, digest, CRYC_SHA256_SIZE);

	/* The same certificate again is not added and is not an error; a different one is added. */
	CHECK(cryc_store_add_der(&store, first->der, first->size) == 0);
	CHECK(store.count == 1U);
	other = store_cert("Store Root", 2);
	CHECK(cryc_store_add_der(&store, other->der, other->size) == 0);
	CHECK(store.count == 2U);

	/* What is not a certificate is EINVAL and changes nothing: garbage, a prefix, a trailing byte, NULL, size 0. */
	memset(garbage, 0x30, sizeof(garbage));
	CHECK(cryc_store_add_der(&store, garbage, sizeof(garbage)) == EINVAL);
	CHECK(cryc_store_add_der(&store, first->der, first->size - 1U) == EINVAL);
	memcpy(copy, first->der, first->size);
	copy[first->size] = 0;
	CHECK(cryc_store_add_der(&store, copy, first->size + 1U) == EINVAL);
	CHECK(cryc_store_add_der(&store, NULL, first->size) == EINVAL);
	CHECK(cryc_store_add_der(&store, first->der, 0) == EINVAL);
	CHECK(cryc_store_add_der(NULL, first->der, first->size) == EINVAL);
	CHECK(store.count == 2U);

	/* Entries keep their address when the array grows, and are kept in the order of adding. */
	entries[0] = store.entries[0];
	entries[1] = store.entries[1];
	for (index = 2; index < 24U; index++) {
		sprintf(name, "Store Root %u", index);
		other = store_cert(name, 100U + index);
		CHECK(cryc_store_add_der(&store, other->der, other->size) == 0);
		entries[index] = store.entries[index];
	}
	CHECK(store.count == 24U);
	CHECK(store.capacity >= 24U);
	for (index = 0; index < 24U; index++)
		CHECK(store.entries[index] == entries[index]);
	CHECK(store.entries[0]->cert.serial_size == first->cert.serial_size);
	CHECK(memcmp(store.entries[0]->cert.serial, first->cert.serial, first->cert.serial_size) == 0);

	/* Freeing is safe twice and leaves the structure zero. */
	cryc_store_free(&store);
	CHECK(store.entries == NULL);
	CHECK(store.count == 0U);
	CHECK(store.lock.impl == NULL);
	cryc_store_free(&store);
	cryc_store_free(NULL);

	pool_used = mark;
}

/*
 * The order n of P-256 and of P-384, big-endian.
 * Values referenced: FIPS 186-5 / SP 800-186 3.2.1.3 and 3.2.1.4.
 */
static const char p256_order_hex[] = "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551";
static const char p384_order_hex[] = "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973";

/*
 * Makes from an ECDSA-signed certificate the same certificate with the
 * other valid signature (r, n - s), re-encoded: the tbsCertificate and
 * the algorithm are the same bytes, the DER differs.  curve is the
 * issuer key's.  Returns the new certificate, parsed, or NULL.
 */
static struct built *
malleate(
	const struct built *original,
	enum cryc_curve_id curve)
{
	unsigned char order[CRYC_ECP_MAX_BYTES];
	unsigned char s_value[CRYC_ECP_MAX_BYTES];
	unsigned char flipped[CRYC_ECP_MAX_BYTES];
	unsigned char sig_bytes[160];
	unsigned char bits_bytes[200];
	unsigned char body_bytes[CERT_ROOM];
	struct der_writer sig;
	struct der_writer bits;
	struct der_writer body;
	struct der_writer outer;
	struct cryc_der reader;
	struct cryc_der inner;
	struct cryc_der_element element;
	struct cryc_der_element tbs;
	struct cryc_der_element alg;
	struct cryc_der_element bit_string;
	struct cryc_der_element r_element;
	struct cryc_der_element s_element;
	const uint8_t *signature;
	const uint8_t *r_bytes;
	const uint8_t *s_bytes;
	size_t signature_size;
	size_t r_size;
	size_t s_size;
	size_t size;
	size_t index;
	unsigned unused;
	unsigned borrow;
	unsigned difference;
	struct built *made;

	/* The three parts of the outer SEQUENCE. */
	size = curve == CRYC_CURVE_P384 ? 48U : 32U;
	CHECK(hex_decode(curve == CRYC_CURVE_P384 ? p384_order_hex : p256_order_hex, order, size) == size);
	cryc_der_init(&reader, original->der, original->size);
	if (cryc_der_expect(&reader, CRYC_DER_SEQUENCE, &element) != 0 || cryc_der_enter(&element, &inner) != 0)
		return NULL;
	if (cryc_der_next(&inner, &tbs) != 0 || cryc_der_next(&inner, &alg) != 0 || cryc_der_next(&inner, &bit_string) != 0)
		return NULL;
	if (cryc_der_bit_string(&bit_string, &signature, &signature_size, &unused) != 0)
		return NULL;

	/* r and s of the Ecdsa-Sig-Value, and n - s (s < n, so no borrow out). */
	cryc_der_init(&reader, signature, signature_size);
	if (cryc_der_expect(&reader, CRYC_DER_SEQUENCE, &element) != 0 || cryc_der_enter(&element, &inner) != 0)
		return NULL;
	if (cryc_der_next(&inner, &r_element) != 0 || cryc_der_next(&inner, &s_element) != 0)
		return NULL;
	if (cryc_der_integer(&r_element, &r_bytes, &r_size) != 0 || cryc_der_integer(&s_element, &s_bytes, &s_size) != 0)
		return NULL;
	if (s_size > size)
		return NULL;
	memset(s_value, 0, sizeof(s_value));
	memcpy(s_value + size - s_size, s_bytes, s_size);
	borrow = 0;
	for (index = size; index-- > 0U;) {
		difference = (unsigned)order[index] - (unsigned)s_value[index] - borrow;
		flipped[index] = (unsigned char)difference;
		borrow = (difference >> 8) & 1U;
	}

	/* The new signature, BIT STRING, and certificate. */
	for (index = 0; index + 1U < size && flipped[index] == 0U; index++)
		;
	der_init(&sig, sig_bytes, sizeof(sig_bytes));
	der_unsigned(&sig, r_bytes, r_size);
	der_unsigned(&sig, flipped + index, size - index);
	der_init(&bits, bits_bytes, sizeof(bits_bytes));
	bits_bytes[0] = 0;
	bits.used = 1;
	der_put(&bits, 0x30U, sig.data, sig.used);
	der_init(&body, body_bytes, sizeof(body_bytes));
	der_raw(&body, tbs.encoding, tbs.encoding_size);
	der_raw(&body, alg.encoding, alg.encoding_size);
	der_put(&body, 0x03U, bits.data, bits.used);
	CHECK(pool_used < CERT_POOL);
	made = &pool[pool_used++];
	der_init(&outer, made->der, sizeof(made->der));
	der_wrap(&outer, 0x30U, &body);
	if (sig.error || bits.error || body.error || outer.error)
		return NULL;
	made->size = outer.used;
	if (cryc_x509_parse(&made->cert, made->der, made->size) != 0)
		return NULL;
	return made;
}

/* The distrusted marks: kept by SHA-256 only, no duplicates, errors, not in the entries. */
static void
test_store_distrusted(void)
{
	struct cryc_store store;
	struct built *first;
	unsigned char garbage[16];
	uint8_t digest[CRYC_SHA256_SIZE];
	unsigned mark;
	int error;

	mark = pool_used;
	CHECK(cryc_store_init(&store) == 0);
	CHECK(store.distrusted == NULL);
	CHECK(store.distrusted_capacity == 0U);

	/* Arguments: no store, no bytes, size 0. */
	first = store_cert("Distrust Root", 1);
	CHECK(cryc_store_add_distrusted(NULL, first->der, first->size) == EINVAL);
	CHECK(cryc_store_add_distrusted(&store, NULL, first->size) == EINVAL);
	CHECK(cryc_store_add_distrusted(&store, first->der, 0) == EINVAL);
	CHECK(store.distrusted_count == 0U);

	/* A certificate that parses is marked by the digest of its tbsCertificate; the entries are not touched. */
	CHECK(cryc_store_add_distrusted(&store, first->der, first->size) == 0);
	CHECK(store.distrusted_count == 1U);
	CHECK(store.count == 0U);
	error = cryc_hash_digest(CRYC_HASH_SHA256, first->cert.tbs, first->cert.tbs_size, digest);
	CHECK(error == 0);
	CHECK_BYTES(store.distrusted[0], digest, CRYC_SHA256_SIZE);

	/* The same value again is not marked again; bytes that do not parse are marked too (one byte is enough). */
	CHECK(cryc_store_add_distrusted(&store, first->der, first->size) == 0);
	CHECK(store.distrusted_count == 1U);
	memset(garbage, 0x30, sizeof(garbage));
	CHECK(cryc_store_add_distrusted(&store, garbage, sizeof(garbage)) == 0);
	CHECK(cryc_store_add_distrusted(&store, garbage, 1) == 0);
	CHECK(cryc_store_add_distrusted(&store, garbage, sizeof(garbage)) == 0);
	CHECK(store.distrusted_count == 3U);

	/* A mark does not remove an entry that is added after it. */
	CHECK(cryc_store_add_der(&store, first->der, first->size) == 0);
	CHECK(store.count == 1U);
	CHECK(store.distrusted_count == 3U);

	cryc_store_free(&store);
	pool_used = mark;
}

/* The limits: CRYC_STORE_MAX entries and CRYC_STORE_MAX marks, and a duplicate at the limit is not an error. */
static void
test_store_limits(void)
{
	struct cryc_store store;
	struct built *template_cert;
	unsigned char *copy;
	unsigned char mark_bytes[8];
	size_t serial_offset;
	size_t size;
	unsigned index;
	unsigned mark;
	int error;

	mark = pool_used;
	CHECK(cryc_store_init(&store) == 0);

	/* The certificates differ only in their serial number (not signed over again: the store does not verify). */
	template_cert = store_cert("Limit Root", 0x01000000UL);
	size = template_cert->size;
	copy = malloc(size);
	CHECK(copy != NULL);
	if (copy == NULL) {
		cryc_store_free(&store);
		return;
	}
	memcpy(copy, template_cert->der, size);
	serial_offset = (size_t)(template_cert->cert.serial - template_cert->der);
	CHECK(template_cert->cert.serial_size == 4U);

	/* CRYC_STORE_MAX distinct certificates are accepted. */
	for (index = 0; index < CRYC_STORE_MAX; index++) {
		copy[serial_offset + 2U] = (unsigned char)(index >> 8);
		copy[serial_offset + 3U] = (unsigned char)index;
		error = cryc_store_add_der(&store, copy, size);
		CHECK(error == 0);
	}
	CHECK(store.count == CRYC_STORE_MAX);

	/* One more is ENOBUFS and changes nothing; one already in the store is still 0. */
	copy[serial_offset + 2U] = 0x7f;
	copy[serial_offset + 3U] = 0x7f;
	CHECK(cryc_store_add_der(&store, copy, size) == ENOBUFS);
	CHECK(store.count == CRYC_STORE_MAX);
	copy[serial_offset + 2U] = 0;
	copy[serial_offset + 3U] = 5;
	CHECK(cryc_store_add_der(&store, copy, size) == 0);
	CHECK(store.count == CRYC_STORE_MAX);
	free(copy);
	cryc_store_free(&store);
	pool_used = mark;

	/* The marks: 4096 distinct values, then ENOBUFS; one that is already marked is still 0. */
	CHECK(cryc_store_init(&store) == 0);
	for (index = 0; index < CRYC_STORE_MAX; index++) {
		mark_bytes[0] = (unsigned char)index;
		mark_bytes[1] = (unsigned char)(index >> 8);
		mark_bytes[2] = 7;
		error = cryc_store_add_distrusted(&store, mark_bytes, 3);
		if (error != 0)
			CHECK(error == 0);
	}
	CHECK(store.distrusted_count == CRYC_STORE_MAX);
	mark_bytes[0] = 0;
	mark_bytes[1] = 0xff;
	mark_bytes[2] = 8;
	CHECK(cryc_store_add_distrusted(&store, mark_bytes, 3) == ENOBUFS);
	CHECK(store.distrusted_count == CRYC_STORE_MAX);
	mark_bytes[0] = 5;
	mark_bytes[1] = 0;
	mark_bytes[2] = 7;
	CHECK(cryc_store_add_distrusted(&store, mark_bytes, 3) == 0);
	CHECK(store.distrusted_count == CRYC_STORE_MAX);
	cryc_store_free(&store);
}

/* PEM text: several blocks with text between, bad blocks skipped, the codes of the header. */
static void
test_store_pem(void)
{
	struct cryc_store store;
	struct built *first;
	struct built *second;
	struct built *third;
	char *text;
	size_t used;
	size_t added;
	unsigned char junk[40];
	unsigned mark;
	int error;

	mark = pool_used;
	text = malloc(20000);
	CHECK(text != NULL);
	if (text == NULL)
		return;
	CHECK(cryc_store_init(&store) == 0);
	first = store_cert("Pem Root", 1);
	second = store_cert("Pem Root", 2);
	third = store_cert("Pem Root", 3);

	/* Three blocks with explanatory text before, between and after them. */
	used = (size_t)sprintf(text, "comment line\n");
	used += pem_encode("CERTIFICATE", first->der, first->size, text + used);
	used += (size_t)sprintf(text + used, "between\n");
	used += pem_encode("CERTIFICATE", second->der, second->size, text + used);
	used += pem_encode("CERTIFICATE", third->der, third->size, text + used);
	used += (size_t)sprintf(text + used, "trailing text\n");
	added = 99;
	error = cryc_store_add_pem(&store, text, used, &added);
	CHECK(error == 0);
	CHECK(added == 3U);
	CHECK(store.count == 3U);
	CHECK_BYTES(store.entries[1]->der, second->der, second->size);

	/* The same text again adds nothing and is not an error; added may be NULL. */
	added = 99;
	CHECK(cryc_store_add_pem(&store, text, used, &added) == 0);
	CHECK(added == 0U);
	CHECK(cryc_store_add_pem(&store, text, used, NULL) == 0);
	CHECK(store.count == 3U);

	/* A block that is not a certificate and a block that does not decode are EINVAL, and the good block is kept. */
	cryc_store_free(&store);
	CHECK(cryc_store_init(&store) == 0);
	memset(junk, 0x30, sizeof(junk));
	used = pem_encode("CERTIFICATE", junk, sizeof(junk), text);
	used += pem_encode("CERTIFICATE", first->der, first->size, text + used);
	used += (size_t)sprintf(text + used, "-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n");
	used += pem_encode("CERTIFICATE", second->der, second->size, text + used);
	added = 99;
	error = cryc_store_add_pem(&store, text, used, &added);
	CHECK(error == EINVAL);
	CHECK(added == 2U);
	CHECK(store.count == 2U);

	/* A BEGIN line without its END line is EINVAL; the blocks before it are kept. */
	cryc_store_free(&store);
	CHECK(cryc_store_init(&store) == 0);
	used = pem_encode("CERTIFICATE", first->der, first->size, text);
	used += (size_t)sprintf(text + used, "-----BEGIN CERTIFICATE-----\nAAAA\n");
	added = 99;
	error = cryc_store_add_pem(&store, text, used, &added);
	CHECK(error == EINVAL);
	CHECK(added == 1U);

	/* Text with no certificate block, a block of another label, and no text at all are ENOENT. */
	added = 99;
	CHECK(cryc_store_add_pem(&store, "nothing to see\n", 15, &added) == ENOENT);
	CHECK(added == 0U);
	used = pem_encode("TRUSTED CERTIFICATE", first->der, first->size, text);
	CHECK(cryc_store_add_pem(&store, text, used, &added) == ENOENT);
	CHECK(cryc_store_add_pem(&store, "", 0, &added) == ENOENT);
	CHECK(cryc_store_add_pem(&store, NULL, 0, &added) == ENOENT);

	/* Arguments: no store, text missing with a size; the count is zero whatever it held before. */
	added = 99;
	CHECK(cryc_store_add_pem(NULL, "x", 1, &added) == EINVAL);
	CHECK(added == 0U);
	added = 99;
	CHECK(cryc_store_add_pem(&store, NULL, 5, &added) == EINVAL);
	CHECK(added == 0U);
	cryc_store_free(&store);

	/* A block that decodes but is not a certificate is EINVAL on its own, and a good block after it is kept. */
	CHECK(cryc_store_init(&store) == 0);
	used = pem_encode("CERTIFICATE", junk, sizeof(junk), text);
	added = 99;
	CHECK(cryc_store_add_pem(&store, text, used, &added) == EINVAL);
	CHECK(added == 0U);
	CHECK(store.count == 0U);
	used += pem_encode("CERTIFICATE", first->der, first->size, text + used);
	CHECK(cryc_store_add_pem(&store, text, used, &added) == EINVAL);
	CHECK(added == 1U);
	CHECK(store.count == 1U);
	cryc_store_free(&store);

	/* A store with room for one more takes the first certificate of a text and then gives ENOBUFS: the count of the first is reported. */
	CHECK(cryc_store_init(&store) == 0);
	{
		struct built *big;
		unsigned char *copy;
		size_t serial_offset;
		unsigned index;

		big = store_cert("Pem Big", 0x01000000UL);
		copy = malloc(big->size);
		CHECK(copy != NULL);
		if (copy != NULL) {
			memcpy(copy, big->der, big->size);
			serial_offset = (size_t)(big->cert.serial - big->der);
			for (index = 0; index < CRYC_STORE_MAX - 1U; index++) {
				copy[serial_offset + 2U] = (unsigned char)(index >> 8);
				copy[serial_offset + 3U] = (unsigned char)index;
				CHECK(cryc_store_add_der(&store, copy, big->size) == 0);
			}
			copy[serial_offset + 2U] = 0x7e;
			copy[serial_offset + 3U] = 0x7e;
			used = pem_encode("CERTIFICATE", copy, big->size, text);
			copy[serial_offset + 3U] = 0x7f;
			used += pem_encode("CERTIFICATE", copy, big->size, text + used);
			added = 99;
			CHECK(cryc_store_add_pem(&store, text, used, &added) == ENOBUFS);
			CHECK(added == 1U);
			CHECK(store.count == CRYC_STORE_MAX);
			free(copy);
		}
	}
	cryc_store_free(&store);
	free(text);
	pool_used = mark;
}

/* Files: a PEM file, a bad block, a missing file, an empty file, and the arguments. */
static void
test_store_file(void)
{
	struct cryc_store store;
	struct built *first;
	struct built *second;
	char text[CERT_ROOM * 3];
	char path[1200];
	char missing[1200];
	size_t used;
	unsigned mark;
	int error;

	mark = pool_used;
	CHECK(cryc_store_init(&store) == 0);
	first = store_cert("File Root", 1);
	second = store_cert("File Root", 2);

	/* A file with two certificates, read as the library reads a bundle. */
	used = pem_encode("CERTIFICATE", first->der, first->size, text);
	used += pem_encode("CERTIFICATE", second->der, second->size, text + used);
	CHECK(write_file("chain-test-bundle.pem", (const unsigned char *)text, used, path));
	error = cryc_store_add_file(&store, path);
	CHECK(error == 0);
	CHECK(store.count == 2U);

	/* The same file again changes nothing. */
	CHECK(cryc_store_add_file(&store, path) == 0);
	CHECK(store.count == 2U);

	/* A missing file is ENOENT and an empty one has no block. */
	sprintf(missing, "%s/chain-test-missing.pem", test_dir);
	CHECK(cryc_store_add_file(&store, missing) == ENOENT);
	CHECK(write_file("chain-test-empty.pem", (const unsigned char *)text, 0, path));
	CHECK(cryc_store_add_file(&store, path) == ENOENT);

	/* A bad block in a file is EINVAL and the good one stays. */
	cryc_store_free(&store);
	CHECK(cryc_store_init(&store) == 0);
	used = (size_t)sprintf(text, "-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n");
	used += pem_encode("CERTIFICATE", first->der, first->size, text + used);
	CHECK(write_file("chain-test-bad.pem", (const unsigned char *)text, used, path));
	CHECK(cryc_store_add_file(&store, path) == EINVAL);
	CHECK(store.count == 1U);

	/* Arguments. */
	CHECK(cryc_store_add_file(NULL, path) == EINVAL);
	CHECK(cryc_store_add_file(&store, NULL) == EINVAL);
	cryc_store_free(&store);
	pool_used = mark;
}

/* The system's roots against the real source: the call succeeds, adds what the source has, and is repeatable. */
static void
test_store_system(void)
{
	struct cryc_store store;
	size_t before;
	int error;
#ifndef _WIN32
	FILE *file;
#endif

	CHECK(cryc_store_init(&store) == 0);
	CHECK(cryc_store_add_system(NULL) == EINVAL);

	/* The call succeeds (a missing default file is not an error). */
	error = cryc_store_add_system(&store);
	CHECK(error == 0);

	/* Windows: Wine's ROOT store holds the host's authorities, 100 or more. */
#ifdef _WIN32
	CHECK(store.count >= 100U);
#else
	/* POSIX: the file's certificates when the host has it, else nothing. */
	file = fopen(CRYC_OS_DEFAULT_CA_FILE, "rb");
	if (file != NULL) {
		fclose(file);
		CHECK(store.count > 0U);
	} else {
		CHECK(store.count == 0U);
	}
#endif

	/* A second call adds nothing new. */
	before = store.count;
	CHECK(cryc_store_add_system(&store) == 0);
	CHECK(store.count == before);
	cryc_store_free(&store);
}

/* The state of the multi-thread test. */
struct thread_job {
	struct cryc_store *store;
	const unsigned char *der;
	size_t size;
	size_t serial_offset;
	unsigned base;
	int errors;
#ifdef _WIN32
	HANDLE thread;
#else
	pthread_t thread;
#endif
};

#ifdef _WIN32
#define THREAD_RETURN	DWORD
#define THREAD_CALL	WINAPI
#define THREAD_RESULT	0
#else
#define THREAD_RETURN	void *
#define THREAD_CALL
#define THREAD_RESULT	NULL
#endif

/* One thread: adds THREAD_ADDS certificates that differ in their serial number. */
static THREAD_RETURN THREAD_CALL
thread_adder(
	void *argument)
{
	struct thread_job *job;
	unsigned char *copy;
	unsigned index;
	int error;

	/* A private copy whose serial number is patched. */
	job = argument;
	copy = malloc(job->size);
	if (copy == NULL) {
		job->errors++;
		return THREAD_RESULT;
	}
	memcpy(copy, job->der, job->size);
	for (index = 0; index < THREAD_ADDS; index++) {
		copy[job->serial_offset + 1U] = (unsigned char)(job->base + index);
		copy[job->serial_offset + 2U] = (unsigned char)((job->base + index) >> 8);
		error = cryc_store_add_der(job->store, copy, job->size);
		if (error != 0)
			job->errors++;
	}
	free(copy);
	return THREAD_RESULT;
}

/* Several threads add certificates while this thread validates and calls cryc_store_add_system: nothing hangs, nothing is lost. */
static void
test_store_threads(void)
{
	struct cryc_store store;
	struct thread_job jobs[THREAD_COUNT];
	struct built *root;
	struct built *inter;
	struct built *leaf;
	struct built *list[2];
	struct built *template_cert;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	size_t system_count;
	unsigned index;
	unsigned round;
	unsigned started;
	unsigned mark;

	mark = pool_used;
	root = make_root("Thread Root", 0);
	inter = make_ca("Thread Inter", 1, "Thread Root", 0);
	leaf = make_leaf("thread.test", 3, "Thread Inter", 1);
	template_cert = store_cert("Thread Filler", 0x01000000UL);
	CHECK(cryc_store_init(&store) == 0);
	CHECK(cryc_store_add_der(&store, root->der, root->size) == 0);

	/* The threads start. */
	started = 0;
	for (index = 0; index < THREAD_COUNT; index++) {
		jobs[index].store = &store;
		jobs[index].der = template_cert->der;
		jobs[index].size = template_cert->size;
		jobs[index].serial_offset = (size_t)(template_cert->cert.serial - template_cert->der);
		jobs[index].base = index * 1000U;
		jobs[index].errors = 0;
#ifdef _WIN32
		jobs[index].thread = CreateThread(NULL, 0, thread_adder, &jobs[index], 0, NULL);
		if (jobs[index].thread != NULL)
			started++;
#else
		if (pthread_create(&jobs[index].thread, NULL, thread_adder, &jobs[index]) == 0)
			started++;
#endif
	}
	CHECK(started == THREAD_COUNT);

	/* Meanwhile: validations that must keep working, and the system's roots. */
	params_init(&params);
	list[0] = leaf;
	list[1] = inter;
	for (round = 0; round < 10U; round++) {
		run(&store, list, 2, &params, &result);
		EXPECT(result, CRYC_VERIFY_OK, 0);
		CHECK(result.chain_count == 3U);
	}
	CHECK(cryc_store_add_system(&store) == 0);

	/* The threads end. */
	for (index = 0; index < THREAD_COUNT; index++) {
		if (index >= started)
			break;
#ifdef _WIN32
		WaitForSingleObject(jobs[index].thread, INFINITE);
		CloseHandle(jobs[index].thread);
#else
		pthread_join(jobs[index].thread, NULL);
#endif
		CHECK(jobs[index].errors == 0);
	}

	/* Every added certificate is there (the root, the 2 x THREAD_ADDS, and the system's); the system's count is what a repeat adds. */
	system_count = store.count - 1U - 2U * THREAD_ADDS;
	CHECK(store.count >= 1U + 2U * THREAD_ADDS);
	CHECK(cryc_store_add_system(&store) == 0);
	CHECK(store.count == 1U + 2U * THREAD_ADDS + system_count);
	cryc_store_free(&store);
	pool_used = mark;
}

/* Writes one GeneralSubtree (RFC 5280 4.2.1.10: base, minimum 0 and maximum absent) with a base of the given tag. */
static size_t
subtree(
	unsigned char *out,
	size_t room,
	unsigned tag,
	const unsigned char *base,
	size_t size)
{
	unsigned char inner_bytes[300];
	struct der_writer inner;
	struct der_writer outer;

	/* SEQUENCE { GeneralName }. */
	der_init(&inner, inner_bytes, sizeof(inner_bytes));
	der_put(&inner, tag, base, size);
	der_init(&outer, out, room);
	der_wrap(&outer, 0x30U, &inner);
	CHECK(outer.error == 0);
	return outer.used;
}

/* A root, an intermediate and a leaf for "a.test" with the store holding the root. */
struct standard {
	struct built *root;
	struct built *inter;
	struct built *leaf;
	struct built *list[2];
	struct cryc_store store;
};

/* Makes the standard chain: Root (key 0) signs Inter (key 1) signs the leaf (key 3). */
static void
standard_make(
	struct standard *chain)
{
	chain->root = make_root("Root", 0);
	chain->inter = make_ca("Inter", 1, "Root", 0);
	chain->leaf = make_leaf("a.test", 3, "Inter", 1);
	chain->list[0] = chain->leaf;
	chain->list[1] = chain->inter;
	store_with(&chain->store, &chain->root, 1);
}

/* Checks that result.chain holds the DER of the given certificates (peer) in order, the last being the store's entry for anchor. */
static void
expect_chain(
	const struct cryc_verify_result *result,
	struct built **peer,
	size_t peer_count,
	const struct cryc_store *store,
	size_t anchor_index,
	int line)
{
	size_t index;
	int good;

	/* The peer's certificates first, then the anchor, which is the store's own copy. */
	good = result->chain_count == peer_count + 1U;
	if (good != 0) {
		for (index = 0; index < peer_count; index++) {
			if (result->chain[index]->der != peer[index]->der)
				good = 0;
		}
		if (result->chain[peer_count]->der != store->entries[anchor_index]->der)
			good = 0;
	}
	if (!check_one(good, __FILE__, line, "chain contents"))
		printf("  line %d: chain_count %u\n", line, (unsigned)result->chain_count);
}

#define EXPECT_CHAIN(result, peer, count, store, anchor) expect_chain(&(result), (peer), (count), &(store), (anchor), __LINE__)

/* Arguments, limits of the call, and what is written whatever happens. */
static void
test_verify_arguments(void)
{
	struct standard chain;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct cryc_x509 peer[PEER_ROOM];
	struct built *many[PEER_ROOM];
	struct built *list[4];
	struct spec spec;
	unsigned mark;
	unsigned index;
	int error;

	mark = pool_used;
	standard_make(&chain);
	params_init(&params);

	/* Nothing sent: no peer certificate, nothing in the chain, whatever the result held before. */
	memset(&result, 0xaa, sizeof(result));
	error = cryc_chain_verify(&chain.store, NULL, 0, &params, &result);
	CHECK(error == 0);
	EXPECT(result, CRYC_VERIFY_NO_PEER_CERTIFICATE, 0);
	CHECK(result.chain_count == 0U);
	memset(&result, 0xaa, sizeof(result));
	peer[0] = chain.leaf->cert;
	error = cryc_chain_verify(&chain.store, peer, 0, &params, &result);
	CHECK(error == 0);
	EXPECT(result, CRYC_VERIFY_NO_PEER_CERTIFICATE, 0);

	/* More than CRYC_CHAIN_MAX_PEER is refused; exactly that many is fine (the leaf, the intermediate and 14 repeats of it). */
	many[0] = chain.leaf;
	for (index = 1; index < PEER_ROOM; index++)
		many[index] = chain.inter;
	run(&chain.store, many, PEER_MAX, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 3U);
	memset(&result, 0xaa, sizeof(result));
	for (index = 0; index < PEER_MAX + 1U; index++)
		peer[index] = many[index]->cert;
	error = cryc_chain_verify(&chain.store, peer, PEER_MAX + 1U, &params, &result);
	CHECK(error == 0);
	EXPECT(result, CRYC_VERIFY_MALFORMED, 0);
	CHECK(result.chain_count == 0U);

	/* Missing pointers are EINVAL and still leave a result when there is one. */
	peer[0] = chain.leaf->cert;
	memset(&result, 0xaa, sizeof(result));
	CHECK(cryc_chain_verify(NULL, peer, 1, &params, &result) == EINVAL);
	CHECK(result.code == CRYC_VERIFY_MALFORMED);
	memset(&result, 0xaa, sizeof(result));
	CHECK(cryc_chain_verify(&chain.store, peer, 1, NULL, &result) == EINVAL);
	CHECK(result.code == CRYC_VERIFY_MALFORMED);
	memset(&result, 0xaa, sizeof(result));
	CHECK(cryc_chain_verify(&chain.store, NULL, 1, &params, &result) == EINVAL);
	CHECK(result.code == CRYC_VERIFY_MALFORMED);
	CHECK(cryc_chain_verify(&chain.store, peer, 1, &params, NULL) == EINVAL);

	/* The depth: 0 is the largest; 1 and above the largest are refused; 2 to the largest are used. */
	params.max_depth = 1;
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_MALFORMED, 0);
	params.max_depth = CRYC_CHAIN_MAX_DEPTH + 1U;
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_MALFORMED, 0);
	params.max_depth = (size_t)-1;
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_MALFORMED, 0);
	params.max_depth = 3;
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	params.max_depth = CRYC_CHAIN_MAX_DEPTH;
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	params.max_depth = 2;
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_CHAIN_TOO_LONG, 2);
	params.max_depth = 0;

	/* A name and an address together are refused. */
	params.has_host = 1;
	strcpy(params.host, "a.test");
	params.ip_size = 4;
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_MALFORMED, 0);
	params.has_host = 0;
	params.ip_size = 0;

	/* Without a given time the clock is used: a certificate valid from 2026 to 2036 is valid now, one that ended in 2001 is not. */
	params.has_time = 0;
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec_init(&spec, "old.test", 3, "Inter", 1);
	spec.dns = "old.test";
	spec.not_before = "000101000000Z";
	spec.not_after = "010101000000Z";
	list[0] = make(&spec);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_EXPIRED, 0);

	cryc_store_free(&chain.store);
	pool_used = mark;
}

/* Paths that are valid: shapes of the peer's list, the store holding more than a root, and a trusted self-signed server. */
static void
test_verify_ok(void)
{
	struct standard chain;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *list[8];
	struct built *other_root;
	struct built *self_signed;
	struct built *inter2;
	struct built *leaf;
	struct built *both[2];
	struct spec spec;
	unsigned mark;

	mark = pool_used;
	standard_make(&chain);
	params_init(&params);

	/* The ordinary path: leaf, the intermediate the server sent, the store's root. */
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	EXPECT_CHAIN(result, chain.list, 2, chain.store, 0);

	/* The leaf alone, when the store has the intermediate (an anchor need not be self-signed). */
	both[0] = chain.inter;
	cryc_store_free(&chain.store);
	store_with(&chain.store, both, 1);
	run(&chain.store, chain.list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	EXPECT_CHAIN(result, chain.list, 1, chain.store, 0);

	/* The server's list in another order, with an unrelated root, and a repeated leaf and intermediate. */
	cryc_store_free(&chain.store);
	store_with(&chain.store, &chain.root, 1);
	other_root = make_root("Other Root", 9);
	list[0] = chain.leaf;
	list[1] = other_root;
	list[2] = chain.inter;
	list[3] = chain.inter;
	list[4] = chain.leaf;
	run(&chain.store, list, 5, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 3U);
	CHECK(result.chain[1]->der == chain.inter->der);

	/* The root the server sent too is the same as the store's: the store's copy ends the path. */
	list[0] = chain.leaf;
	list[1] = chain.inter;
	list[2] = chain.root;
	run(&chain.store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 3U);
	CHECK(result.chain[2]->der == chain.store.entries[0]->der);

	/* A leaf signed directly by the root. */
	leaf = make_leaf("direct.test", 4, "Root", 0);
	list[0] = leaf;
	run(&chain.store, list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 2U);

	/* Two intermediates, one with a P-384 key that signs with SHA-384 below it. */
	inter2 = make_ca("Inter 2", 28, "Inter", 1);
	leaf = make_leaf("deep.test", 5, "Inter 2", 28);
	list[0] = leaf;
	list[1] = inter2;
	list[2] = chain.inter;
	run(&chain.store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 4U);

	/* Two anchors that both make a valid path: the first path found is the answer and a later one does not replace it. */
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.serial = 9;
	other_root = make(&spec);
	both[0] = chain.root;
	both[1] = other_root;
	cryc_store_free(&chain.store);
	store_with(&chain.store, both, 2);
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain[2]->der == chain.store.entries[0]->der);
	cryc_store_free(&chain.store);
	store_with(&chain.store, &chain.root, 1);

	/* A trusted self-signed server is its own anchor (a one-certificate path) and passes the leaf's checks. */
	spec_init(&spec, "self.test", 6, "self.test", 6);
	spec.dns = "self.test";
	spec.eku = EKU_SERVER;
	spec.aki = 0;
	self_signed = make(&spec);
	list[0] = self_signed;
	cryc_store_free(&chain.store);
	store_with(&chain.store, list, 1);
	run(&chain.store, list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 1U);
	CHECK(result.chain[0]->der == chain.store.entries[0]->der);

	/* Not in the store, the same certificate is a self-signed leaf. */
	cryc_store_free(&chain.store);
	store_with(&chain.store, &chain.root, 1);
	run(&chain.store, list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_DEPTH_ZERO_SELF_SIGNED, 0);
	CHECK(result.chain_count == 1U);

	/* An empty store has no anchor for anything. */
	cryc_store_free(&chain.store);
	CHECK(cryc_store_init(&chain.store) == 0);
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNABLE_TO_GET_ISSUER, 1);
	CHECK(result.chain_count == 2U);
	run(&chain.store, list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_DEPTH_ZERO_SELF_SIGNED, 0);

	cryc_store_free(&chain.store);
	pool_used = mark;
}

/* Key identifiers: a certificate whose AKI is not the candidate's SKI is not its issuer; one side missing does not exclude. */
static void
test_verify_identifiers(void)
{
	struct standard chain;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *list[3];
	struct built *no_ski;
	struct built *wrong;
	struct spec spec;
	unsigned mark;

	mark = pool_used;
	standard_make(&chain);
	params_init(&params);

	/* A leaf naming another key as its authority finds no issuer, though the issuer's name matches. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.aki = 2;
	wrong = make(&spec);
	list[0] = wrong;
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNABLE_TO_GET_ISSUER, 0);
	CHECK(result.chain_count == 1U);

	/* A key identifier that is the right one cut by a byte is a different identifier. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.aki = 3;
	list[0] = make(&spec);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNABLE_TO_GET_ISSUER, 0);

	/* An intermediate without a subjectKeyIdentifier is a candidate whatever the leaf's AKI says. */
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.ski = 0;
	no_ski = make(&spec);
	list[0] = chain.leaf;
	list[1] = no_ski;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* A leaf without an AKI is matched by name alone, with the intermediate's SKI present. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.aki = 0;
	list[0] = make(&spec);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* The issuer's name is compared as RFC 5280 7.1 asks (case and spacing folded): a certificate that names its issuer in capitals finds it. */
	spec_init(&spec, "a.test", 3, "INTER", 1);
	spec.dns = "a.test";
	spec.aki = 0;
	list[0] = make(&spec);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	cryc_store_free(&chain.store);
	pool_used = mark;
}

/* Validity periods at their edges, in every position of the path, and the order of the checks (anchor first). */
static void
test_verify_validity(void)
{
	struct standard chain;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *list[2];
	struct built *roots[1];
	struct spec spec;
	unsigned mark;

	mark = pool_used;
	standard_make(&chain);
	params_init(&params);

	/* The leaf valid from exactly the time of validation: valid at it, not yet valid one second before. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.not_before = NOW_TEXT;
	list[0] = make(&spec);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	params.time = NOW - 1;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_NOT_YET_VALID, 0);

	/* The leaf valid until exactly the time: valid at it, expired one second after. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.not_after = NOW_TEXT;
	list[0] = make(&spec);
	params.time = NOW;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	params.time = NOW + 1;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_EXPIRED, 0);
	params.time = NOW;

	/* An expired intermediate is reported at its position; a not yet valid one too. */
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.not_after = "260101000001Z";
	list[0] = chain.leaf;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_EXPIRED, 1);
	spec.not_after = TLSPEER_NOT_AFTER;
	spec.not_before = "360101000000Z";
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_NOT_YET_VALID, 1);

	/* The trust anchor's period is checked too, at its position. */
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.not_after = "260101000001Z";
	roots[0] = make(&spec);
	cryc_store_free(&chain.store);
	store_with(&chain.store, roots, 1);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_EXPIRED, 2);
	spec.not_after = TLSPEER_NOT_AFTER;
	spec.not_before = "360101000000Z";
	roots[0] = make(&spec);
	cryc_store_free(&chain.store);
	store_with(&chain.store, roots, 1);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_NOT_YET_VALID, 2);

	/* With an expired anchor and an expired leaf, the anchor's depth is reported: the check runs from the anchor down. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.not_after = "260101000001Z";
	list[0] = make(&spec);
	cryc_store_free(&chain.store);
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.not_after = "260101000001Z";
	roots[0] = make(&spec);
	store_with(&chain.store, roots, 1);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_EXPIRED, 2);

	/* The intermediate before the leaf. */
	cryc_store_free(&chain.store);
	store_with(&chain.store, &chain.root, 1);
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.not_after = "260101000001Z";
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_EXPIRED, 1);

	cryc_store_free(&chain.store);
	pool_used = mark;
}

/* Signatures: faults at each position, the algorithms and keys outside the range, the anchor's own signature not checked. */
static void
test_verify_signatures(void)
{
	struct standard chain;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *list[2];
	struct built *roots[1];
	struct spec spec;
	unsigned mark;

	mark = pool_used;
	standard_make(&chain);
	params_init(&params);

	/* A leaf signature with a flipped bit is a signature failure at depth 0. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.sig_kind = SIG_CORRUPT;
	list[0] = make(&spec);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_SIGNATURE_FAILURE, 0);
	CHECK(result.chain_count == 3U);

	/* An intermediate's signature fault is at depth 1. */
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.sig_kind = SIG_CORRUPT;
	list[0] = chain.leaf;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_SIGNATURE_FAILURE, 1);

	/* A leaf signed by another key than the intermediate's, found by name alone, fails its signature. */
	spec_init(&spec, "a.test", 3, "Inter", 5);
	spec.dns = "a.test";
	spec.aki = 0;
	list[0] = make(&spec);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_SIGNATURE_FAILURE, 0);

	/* A signature algorithm outside the range (ECDSA with SHA-1): unsupported, at the certificate it signs. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.sig_kind = SIG_SHA1;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNSUPPORTED_ALGORITHM, 0);
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.sig_kind = SIG_SHA1;
	list[0] = chain.leaf;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNSUPPORTED_ALGORITHM, 1);

	/* The anchor's own signature is not checked: a root signed with SHA-1, or by nobody, is still an anchor. */
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.sig_kind = SIG_SHA1;
	roots[0] = make(&spec);
	cryc_store_free(&chain.store);
	store_with(&chain.store, roots, 1);
	list[0] = chain.leaf;
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec.sig_kind = SIG_CORRUPT;
	roots[0] = make(&spec);
	cryc_store_free(&chain.store);
	store_with(&chain.store, roots, 1);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	cryc_store_free(&chain.store);
	store_with(&chain.store, &chain.root, 1);

	/* An RSA algorithm under an EC issuer key cannot verify: a signature failure. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.sig_kind = SIG_RSA_ALG;
	list[0] = make(&spec);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_SIGNATURE_FAILURE, 0);

	/* Under an RSA issuer key: of 1024 bits too small, of 2048 bits a signature that does not verify. */
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.key_kind = KEY_RSA_1024;
	list[1] = make(&spec);
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.sig_kind = SIG_RSA_ALG;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_KEY_TOO_SMALL, 0);
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.key_kind = KEY_RSA_2048;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_SIGNATURE_FAILURE, 0);

	/* An issuer key of 2047 bits is just too small; one of 8200 bits is beyond the range, which is not a matter of size. */
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.key_kind = KEY_RSA_2047;
	list[1] = make(&spec);
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.sig_kind = SIG_RSA_ALG;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_KEY_TOO_SMALL, 0);
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.key_kind = KEY_RSA_HUGE;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNSUPPORTED_ALGORITHM, 0);

	/* An issuer key of a kind outside the range (Ed25519) makes the signature unsupported. */
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.key_kind = KEY_UNKNOWN;
	list[1] = make(&spec);
	list[0] = chain.leaf;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNSUPPORTED_ALGORITHM, 0);

	/* The leaf's own key: RSA of 1024 bits is too small, of 2048 and 2049 bits is used, an unknown kind is unsupported. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.key_kind = KEY_RSA_1024;
	list[0] = make(&spec);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_KEY_TOO_SMALL, 0);
	spec.key_kind = KEY_RSA_2048;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec.key_kind = KEY_RSA_2049;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec.key_kind = KEY_RSA_2047;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_KEY_TOO_SMALL, 0);
	spec.key_kind = KEY_UNKNOWN;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNSUPPORTED_ALGORITHM, 0);

	cryc_store_free(&chain.store);
	pool_used = mark;
}

/* What each role asks: an intermediate is a v3 CA that may sign and serve; the leaf may serve and sign. */
static void
test_verify_roles(void)
{
	struct standard chain;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *list[2];
	struct spec spec;
	unsigned mark;

	mark = pool_used;
	standard_make(&chain);
	params_init(&params);
	list[0] = chain.leaf;

	/* An intermediate without basicConstraints, with cA FALSE, or of version 1: not a CA. */
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.basic = 0;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_INVALID_CA, 1);
	spec_ca(&spec);
	spec.basic = 2;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_INVALID_CA, 1);
	spec_ca(&spec);
	spec.version = 1;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_INVALID_CA, 1);

	/* A non-critical basicConstraints still counts (the critical flag is not asked of an intermediate). */
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.basic_critical = 0;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* An intermediate's keyUsage: without keyCertSign refused, with it (alone or with others) or absent accepted. */
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.ku = KU_DIGITAL_SIGNATURE | KU_CRL_SIGN;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_KEY_USAGE, 1);
	spec.ku = KU_KEY_CERT_SIGN;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec.ku = KU_KEY_CERT_SIGN | KU_DIGITAL_SIGNATURE;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec.has_ku = 0;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* An intermediate's extKeyUsage: client only refused; server, any, both and absent accepted. */
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.eku = EKU_CLIENT;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_INVALID_PURPOSE, 1);
	spec.eku = EKU_SERVER;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec.eku = EKU_ANY;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec.eku = EKU_BOTH;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* The leaf's extKeyUsage: client only refused; server, any, both and absent accepted. */
	list[1] = chain.inter;
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.eku = EKU_CLIENT;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_INVALID_PURPOSE, 0);
	spec.eku = EKU_ANY;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec.eku = EKU_BOTH;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec.eku = EKU_NONE;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* The leaf's keyUsage must allow digital signatures; absent is fine; keyEncipherment alone is not. */
	spec.ku = KU_KEY_ENCIPHERMENT;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_KEY_USAGE, 0);
	spec.ku = KU_KEY_ENCIPHERMENT | KU_DIGITAL_SIGNATURE;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec.has_ku = 0;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* A leaf that is itself a CA (cA TRUE, with digitalSignature) is not refused for that. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.basic = 1;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* A version 1 leaf has no extensions and no key usage to refuse. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.version = 1;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	cryc_store_free(&chain.store);
	pool_used = mark;
}

/* pathLenConstraint: counted in non-self-issued intermediates below, not in the leaf, self-issued ones skipped. */
static void
test_verify_pathlen(void)
{
	struct cryc_store store;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *root;
	struct built *list[4];
	struct built *roots[1];
	struct spec spec;
	unsigned mark;

	mark = pool_used;
	params_init(&params);
	root = make_root("Root", 0);
	roots[0] = root;
	store_with(&store, roots, 1);

	/* One intermediate with pathLenConstraint 0 under the root: the leaf below it is not counted. */
	spec_init(&spec, "Inter 1", 1, "Root", 0);
	spec_ca(&spec);
	spec.path_len = 0;
	list[1] = make(&spec);
	list[0] = make_leaf("a.test", 3, "Inter 1", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* A second intermediate below it exceeds 0, reported at the constraining intermediate (depth 2); with 1 it is fine. */
	list[2] = list[1];
	list[1] = make_ca("Inter 2", 2, "Inter 1", 1);
	list[0] = make_leaf("a.test", 3, "Inter 2", 2);
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_PATH_LENGTH_EXCEEDED, 2);
	spec.path_len = 1;
	list[2] = make(&spec);
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* Three below a constraint of 1 exceed it; of 2 do not. */
	list[3] = list[2];
	list[2] = make_ca("Inter 2", 2, "Inter 1", 1);
	list[1] = make_ca("Inter 3", 4, "Inter 2", 2);
	list[0] = make_leaf("a.test", 3, "Inter 3", 4);
	run(&store, list, 4, &params, &result);
	EXPECT(result, CRYC_VERIFY_PATH_LENGTH_EXCEEDED, 3);
	spec.path_len = 2;
	list[3] = make(&spec);
	run(&store, list, 4, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* A self-issued intermediate (the same name, a new key, signed by the old key) is not counted: constraint 0 still holds. */
	spec_init(&spec, "Inter 1", 1, "Root", 0);
	spec_ca(&spec);
	spec.path_len = 0;
	list[2] = make(&spec);
	spec_init(&spec, "Inter 1", 2, "Inter 1", 1);
	spec_ca(&spec);
	list[1] = make(&spec);
	list[0] = make_leaf("a.test", 3, "Inter 1", 2);
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 4U);

	/* Self-issued means the same name in the sense of 7.1, not only the same bytes: a new key whose issuer is written in capitals is not counted either. */
	spec_init(&spec, "Inter 1", 1, "Root", 0);
	spec_ca(&spec);
	spec.path_len = 0;
	list[2] = make(&spec);
	spec_init(&spec, "Inter 1", 2, "INTER 1", 1);
	spec_ca(&spec);
	list[1] = make(&spec);
	CHECK((list[1]->cert.flags & CRYC_X509_SELF_ISSUED) == 0U);
	list[0] = make_leaf("a.test", 3, "Inter 1", 2);
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 4U);

	/* The root's own pathLenConstraint does not limit the path (the anchor is taken as it is). */
	cryc_store_free(&store);
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.path_len = 0;
	roots[0] = make(&spec);
	store_with(&store, roots, 1);
	list[1] = make_ca("Inter 1", 1, "Root", 0);
	list[0] = make_leaf("a.test", 3, "Inter 1", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	cryc_store_free(&store);
	pool_used = mark;
}

/* Name constraints at the anchor, at an intermediate, critical or not, self-issued skip, unevaluated forms. */
static void
test_verify_names(void)
{
	struct cryc_store store;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *roots[1];
	struct built *list[3];
	struct spec spec;
	unsigned char permitted[200];
	unsigned char excluded[200];
	unsigned char extra[100];
	unsigned char subtree_bytes[100];
	size_t permitted_size;
	size_t excluded_size;
	size_t extra_size;
	unsigned mark;
	static const unsigned char ten_net[8] = { 10, 0, 0, 0, 255, 0, 0, 0 };
	static const unsigned char ten_host[4] = { 10, 1, 2, 3 };
	static const unsigned char other_host[4] = { 192, 0, 2, 1 };

	mark = pool_used;
	params_init(&params);

	/* A root that permits only example.com: the leaf below must be inside it. */
	permitted_size = subtree(permitted, sizeof(permitted), 0x82U, (const unsigned char *)"example.com", 11);
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.nc_mode = NC_CRITICAL;
	spec.nc_permitted = permitted;
	spec.nc_permitted_size = permitted_size;
	roots[0] = make(&spec);
	store_with(&store, roots, 1);
	list[1] = make_ca("Inter", 1, "Root", 0);
	list[0] = make_leaf("www.example.com", 3, "Inter", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	list[0] = make_leaf("example.com", 3, "Inter", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	list[0] = make_leaf("evil.org", 3, "Inter", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION, 0);
	list[0] = make_leaf("notexample.com", 3, "Inter", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION, 0);
	cryc_store_free(&store);

	/* The same constraint not marked critical is applied just the same. */
	spec.nc_mode = NC_NONCRITICAL;
	roots[0] = make(&spec);
	store_with(&store, roots, 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION, 0);
	list[0] = make_leaf("www.example.com", 3, "Inter", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	cryc_store_free(&store);

	/* An excluded subtree: names in it are refused, others allowed. */
	excluded_size = subtree(excluded, sizeof(excluded), 0x82U, (const unsigned char *)"bad.example.com", 15);
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.nc_mode = NC_CRITICAL;
	spec.nc_excluded = excluded;
	spec.nc_excluded_size = excluded_size;
	roots[0] = make(&spec);
	store_with(&store, roots, 1);
	list[0] = make_leaf("x.bad.example.com", 3, "Inter", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION, 0);
	list[0] = make_leaf("ok.example.com", 3, "Inter", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	cryc_store_free(&store);

	/* Constraints of an intermediate apply to the leaf, and to a lower intermediate's names (reported at the lower certificate). */
	roots[0] = make_root("Root", 0);
	store_with(&store, roots, 1);
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.nc_mode = NC_CRITICAL;
	spec.nc_permitted = permitted;
	spec.nc_permitted_size = permitted_size;
	list[1] = make(&spec);
	list[0] = make_leaf("evil.org", 3, "Inter", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION, 0);
	list[0] = make_leaf("a.example.com", 3, "Inter", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec_init(&spec, "Inter 2", 2, "Inter", 1);
	spec_ca(&spec);
	spec.dns = "evil.org";
	list[2] = list[1];
	list[1] = make(&spec);
	list[0] = make_leaf("a.example.com", 3, "Inter 2", 2);
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION, 1);

	/* A self-issued intermediate is not checked (6.1.3 (b), (c)) though its names are outside; a plain one is. */
	spec_init(&spec, "Inter", 2, "Inter", 1);
	spec_ca(&spec);
	spec.dns = "evil.org";
	list[1] = make(&spec);
	list[0] = make_leaf("a.example.com", 3, "Inter", 2);
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	cryc_store_free(&store);

	/* A leaf that is self-issued (a re-keyed certificate of the anchor's own name) is checked: only self-issued intermediates are not. */
	permitted_size = subtree(permitted, sizeof(permitted), 0x82U, (const unsigned char *)"example.com", 11);
	spec_init(&spec, "Self", 12, "Self", 12);
	spec_ca(&spec);
	spec.aki = 0;
	spec.nc_mode = NC_CRITICAL;
	spec.nc_permitted = permitted;
	spec.nc_permitted_size = permitted_size;
	roots[0] = make(&spec);
	store_with(&store, roots, 1);
	spec_init(&spec, "Self", 13, "Self", 12);
	spec.dns = "evil.org";
	list[0] = make(&spec);
	run(&store, list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION, 0);
	spec.dns = "www.example.com";
	list[0] = make(&spec);
	run(&store, list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	cryc_store_free(&store);

	/* An iPAddress constraint: inside 10.0.0.0/8 passes, outside is refused. */
	permitted_size = subtree(permitted, sizeof(permitted), 0x87U, ten_net, sizeof(ten_net));
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.nc_mode = NC_CRITICAL;
	spec.nc_permitted = permitted;
	spec.nc_permitted_size = permitted_size;
	roots[0] = make(&spec);
	store_with(&store, roots, 1);
	list[1] = make_ca("Inter", 1, "Root", 0);
	spec_init(&spec, "ip.test", 3, "Inter", 1);
	spec.address = ten_host;
	spec.address_size = sizeof(ten_host);
	list[0] = make(&spec);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec.address = other_host;
	spec.address_size = sizeof(other_host);
	list[0] = make(&spec);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION, 0);
	cryc_store_free(&store);

	/* A form the check cannot evaluate: constrained and used is unsupported; constrained and unused is ignored. */
	permitted_size = subtree(permitted, sizeof(permitted), 0x86U, (const unsigned char *)"example.org", 11);
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.nc_mode = NC_CRITICAL;
	spec.nc_permitted = permitted;
	spec.nc_permitted_size = permitted_size;
	roots[0] = make(&spec);
	store_with(&store, roots, 1);
	extra_size = subtree(subtree_bytes, sizeof(subtree_bytes), 0x86U, (const unsigned char *)"http://x.example.org/", 21);
	CHECK(extra_size > 2U);
	memset(extra, 0, sizeof(extra));
	extra_size = 0;
	{
		struct der_writer writer;

		der_init(&writer, extra, sizeof(extra));
		der_put(&writer, 0x86U, (const unsigned char *)"http://x.example.org/", 21);
		extra_size = writer.used;
	}
	spec_init(&spec, "uri.test", 3, "Inter", 1);
	spec.dns = "uri.test";
	spec.san_extra = extra;
	spec.san_extra_size = extra_size;
	list[0] = make(&spec);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNSUPPORTED_NAME_CONSTRAINT, 0);
	list[0] = make_leaf("plain.test", 3, "Inter", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	cryc_store_free(&store);

	/* A constraint that does not parse as a name constraint (a trailing dot) is a malformed certificate. */
	permitted_size = subtree(permitted, sizeof(permitted), 0x82U, (const unsigned char *)"example.com.", 12);
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.nc_mode = NC_CRITICAL;
	spec.nc_permitted = permitted;
	spec.nc_permitted_size = permitted_size;
	roots[0] = make(&spec);
	store_with(&store, roots, 1);
	list[0] = make_leaf("www.example.com", 3, "Inter", 1);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_MALFORMED, 1);
	cryc_store_free(&store);

	/* A leaf's own nameConstraints constrain nothing (only the certificates above apply). */
	roots[0] = make_root("Root", 0);
	store_with(&store, roots, 1);
	permitted_size = subtree(permitted, sizeof(permitted), 0x82U, (const unsigned char *)"example.com", 11);
	spec_init(&spec, "evil.org", 3, "Inter", 1);
	spec.dns = "evil.org";
	spec.nc_mode = NC_CRITICAL;
	spec.nc_permitted = permitted;
	spec.nc_permitted_size = permitted_size;
	list[0] = make(&spec);
	list[1] = make_ca("Inter", 1, "Root", 0);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	cryc_store_free(&store);

	pool_used = mark;
}

/* Critical extensions, certificatePolicies, and the position of the unknown critical one. */
static void
test_verify_extensions(void)
{
	struct standard chain;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *list[2];
	struct built *roots[1];
	struct spec spec;
	unsigned mark;

	mark = pool_used;
	standard_make(&chain);
	params_init(&params);

	/* An unknown critical extension refuses the leaf and the intermediate, at their positions. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.unknown = 1;
	list[0] = make(&spec);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNHANDLED_CRITICAL_EXTENSION, 0);
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.unknown = 1;
	list[0] = chain.leaf;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNHANDLED_CRITICAL_EXTENSION, 1);

	/* An unknown extension that is not critical is ignored. */
	spec.unknown = 2;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* An anchor with an unknown critical extension is refused at its depth; with an unknown extension that is not critical it is an anchor. */
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.unknown = 1;
	roots[0] = make(&spec);
	cryc_store_free(&chain.store);
	store_with(&chain.store, roots, 1);
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNHANDLED_CRITICAL_EXTENSION, 2);
	spec.unknown = 2;
	roots[0] = make(&spec);
	cryc_store_free(&chain.store);
	store_with(&chain.store, roots, 1);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* A critical certificatePolicies is recognized and not processed, on the leaf and the intermediate. */
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.policies = 1;
	list[0] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.policies = 1;
	list[0] = chain.leaf;
	list[1] = make(&spec);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	cryc_store_free(&chain.store);
	pool_used = mark;
}

/* Makes a chain of count intermediates below "Root" and a leaf for them: list[0] is the leaf, list[1] the lowest intermediate, list[count] the highest. */
static void
make_deep_chain(
	struct built **list,
	unsigned count)
{
	char names[12][16];
	unsigned index;

	/* The names and keys: intermediate i is "CA i" with key i, issued by the one before it. */
	for (index = 1; index <= count; index++)
		sprintf(names[index], "CA %u", index);
	for (index = 1; index <= count; index++) {
		if (index == 1U)
			list[count + 1U - index] = make_ca(names[index], index, "Root", 0);
		else
			list[count + 1U - index] = make_ca(names[index], index, names[index - 1U], index - 1U);
	}
	list[0] = make_leaf("a.test", 20, names[count], count);
}

/* The depth limit: chains of 10 and 11 certificates, and a too-long path is only the answer when no valid one exists. */
static void
test_verify_depth(void)
{
	struct cryc_store store;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *roots[2];
	struct built *list[13];
	struct built *alternative;
	unsigned mark;

	mark = pool_used;
	params_init(&params);
	roots[0] = make_root("Root", 0);
	store_with(&store, roots, 1);

	/* A chain with 8 intermediates has 10 certificates with the root: accepted with the default and the largest depth, too long for 9. */
	make_deep_chain(list, 8);
	run(&store, list, 9, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 10U);
	params.max_depth = CRYC_CHAIN_MAX_DEPTH;
	run(&store, list, 9, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	params.max_depth = 9;
	run(&store, list, 9, &params, &result);
	EXPECT(result, CRYC_VERIFY_CHAIN_TOO_LONG, 9);
	params.max_depth = 0;

	/* With 9 intermediates the path has 11 certificates: too long, at depth 10, with the default and the largest depth. */
	make_deep_chain(list, 9);
	run(&store, list, 10, &params, &result);
	EXPECT(result, CRYC_VERIFY_CHAIN_TOO_LONG, 10);
	params.max_depth = CRYC_CHAIN_MAX_DEPTH;
	run(&store, list, 10, &params, &result);
	EXPECT(result, CRYC_VERIFY_CHAIN_TOO_LONG, 10);
	params.max_depth = 0;

	/* A short valid path found after the too-long one wins: the server also sent a twin of the top intermediate issued by another root. */
	roots[1] = make_root("Alt Root", 15);
	alternative = make_ca("CA 9", 9, "Alt Root", 15);
	list[10] = alternative;
	cryc_store_free(&store);
	store_with(&store, roots, 2);
	run(&store, list, 11, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 3U);

	cryc_store_free(&store);
	pool_used = mark;
}

/* Matching the name or address of the server against the leaf after a valid path. */
static void
test_verify_host(void)
{
	struct standard chain;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *list[2];
	struct spec spec;
	static const unsigned char address[4] = { 192, 0, 2, 1 };
	static const unsigned char other[4] = { 192, 0, 2, 2 };
	unsigned mark;

	mark = pool_used;
	standard_make(&chain);
	params_init(&params);

	/* A name that matches the dNSName of the leaf, in another case and with a trailing dot too. */
	params.has_host = 1;
	strcpy(params.host, "a.test");
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	strcpy(params.host, "A.TEST.");
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);

	/* A name that does not match: the code and the validated chain are both reported. */
	strcpy(params.host, "b.test");
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_HOSTNAME_MISMATCH, 0);
	EXPECT_CHAIN(result, chain.list, 2, chain.store, 0);

	/* A name that is not a host name matches nothing. */
	strcpy(params.host, "a..test");
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_HOSTNAME_MISMATCH, 0);

	/* An address: the leaf's iPAddress equal to it matches; another address, or one of a wrong length, does not. */
	params.has_host = 0;
	spec_init(&spec, "ip.test", 3, "Inter", 1);
	spec.address = address;
	spec.address_size = sizeof(address);
	list[0] = make(&spec);
	list[1] = chain.inter;
	params.ip_size = 4;
	memcpy(params.ip, address, 4);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	memcpy(params.ip, other, 4);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_IP_ADDRESS_MISMATCH, 0);
	EXPECT_CHAIN(result, list, 2, chain.store, 0);
	memcpy(params.ip, address, 4);
	params.ip_size = 5;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_IP_ADDRESS_MISMATCH, 0);
	params.ip_size = 0;

	/* The name is matched only when the path is valid: a bad path keeps its own code. */
	params.has_host = 1;
	strcpy(params.host, "b.test");
	params.time = (int64_t)NOW + 631152000;
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_EXPIRED, 2);

	cryc_store_free(&chain.store);
	pool_used = mark;
}

/* Makes a self-issued junk certificate called name signed by its own key (a look-alike for the search tests). */
static struct built *
make_junk(
	const char *name,
	unsigned key)
{
	struct spec spec;

	/* A CA certificate whose issuer and subject are the same name; no key identifiers, so only the name ties it to others. */
	spec_init(&spec, name, key, name, key);
	spec_ca(&spec);
	spec.aki = 0;
	return make(&spec);
}

/*
 * A model of the search over look-alikes, written from the header: certificates 1 to count are all named "J" and
 * issued by "J" (so each is a candidate issuer of every other and of the leaf, which is certificate 0), the search
 * goes depth first, tries the candidates in the order the server sent them, skips one already on the path, makes one
 * signature verification for each pair (child, issuer) it has not made, extends a path only while it has fewer
 * certificates than the limit, and stops when it needs verification number CRYC_CHAIN_MAX_WORK + 1 or when it would
 * expand node number MODEL_MAX_NODES + 1 (the library's own bound on the nodes of one search, 1024: a search over
 * look-alikes whose pairs are all verified already still has many paths to walk).
 */
#define MODEL_MAX_NODES	1024U
static unsigned model_path[CRYC_CHAIN_MAX_DEPTH + 1U];
static unsigned model_length;
static unsigned model_work;
static unsigned model_nodes;
static unsigned model_count;
static unsigned model_depth;
static int model_stopped;
static unsigned char model_done[PEER_ROOM][PEER_ROOM];

/* Extends the model's path by each candidate of its last certificate. */
static void
model_extend(void)
{
	unsigned top;
	unsigned candidate;
	unsigned position;
	int on_path;

	/* Each expansion is a node; the one beyond the bound stops the search at the depth of the path. */
	model_nodes++;
	if (model_nodes > MODEL_MAX_NODES) {
		model_stopped = 1;
		model_depth = model_length - 1U;
		return;
	}

	/* Peers may be added while the path is shorter than the largest depth. */
	top = model_path[model_length - 1U];
	if (model_length >= CRYC_CHAIN_MAX_DEPTH)
		return;
	for (candidate = 1; candidate <= model_count; candidate++) {
		on_path = 0;
		for (position = 0; position < model_length; position++) {
			if (model_path[position] == candidate)
				on_path = 1;
		}
		if (on_path != 0)
			continue;

		/* A pair not verified yet costs one verification; the one beyond the bound stops the search. */
		if (model_done[top][candidate] == 0U) {
			if (model_work >= CRYC_CHAIN_MAX_WORK) {
				model_stopped = 1;
				model_depth = model_length - 1U;
				return;
			}
			model_work++;
			model_done[top][candidate] = 1;
		}
		model_path[model_length++] = candidate;
		model_extend();
		model_length--;
		if (model_stopped != 0)
			return;
	}
}

/* Runs the model for count look-alikes: model_stopped tells whether a bound stopped the search, model_depth where. */
static void
model_run(
	unsigned count)
{
	/* The path starts with the leaf. */
	memset(model_done, 0, sizeof(model_done));
	model_path[0] = 0;
	model_length = 1;
	model_work = 0;
	model_nodes = 0;
	model_count = count;
	model_stopped = 0;
	model_depth = 0;
	model_extend();
}

/* Path building: backtracking to another candidate, the code of the first complete path, dead ends, cycles, the work bound. */
static void
test_verify_search(void)
{
	struct cryc_store store;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *roots[2];
	struct built *list[17];
	struct built *junk[16];
	struct built *leaf;
	struct built *expired;
	struct built *not_yet;
	struct built *valid;
	struct built *root_a;
	struct built *root_b;
	struct spec spec;
	unsigned index;
	unsigned count;
	unsigned mark;

	mark = pool_used;
	params_init(&params);

	/* Cross-signed intermediates (the same name and key, two issuers): the path through the one that is not valid is left for the other. */
	root_a = make_root("Root A", 0);
	root_b = make_root("Root B", 6);
	roots[0] = root_a;
	roots[1] = root_b;
	store_with(&store, roots, 2);
	spec_init(&spec, "Inter", 1, "Root A", 0);
	spec_ca(&spec);
	spec.not_after = "260101000001Z";
	expired = make(&spec);
	spec_init(&spec, "Inter", 1, "Root B", 6);
	spec_ca(&spec);
	valid = make(&spec);
	spec.not_before = "360101000000Z";
	not_yet = make(&spec);
	leaf = make_leaf("a.test", 3, "Inter", 1);
	list[0] = leaf;
	list[1] = expired;
	list[2] = valid;
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 3U);
	CHECK(result.chain[1]->der == valid->der);
	CHECK(result.chain[2]->der == store.entries[1]->der);
	list[1] = valid;
	list[2] = expired;
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain[1]->der == valid->der);

	/* No path valid: the first complete path's code, in the order the server sent them. */
	list[1] = expired;
	list[2] = not_yet;
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_EXPIRED, 1);
	CHECK(result.chain_count == 3U);
	CHECK(result.chain[1]->der == expired->der);
	list[1] = not_yet;
	list[2] = expired;
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_NOT_YET_VALID, 1);
	CHECK(result.chain[1]->der == not_yet->der);
	cryc_store_free(&store);

	/* The store's candidates are tried before the server's: a trusted intermediate is used though the server's copy is bad. */
	roots[0] = make_root("Root", 0);
	roots[1] = make_ca("Inter", 1, "Root", 0);
	store_with(&store, roots, 2);
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.sig_kind = SIG_CORRUPT;
	list[0] = leaf;
	list[1] = make(&spec);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain_count == 2U);
	cryc_store_free(&store);

	/* A root the server sent that the store does not have is a self-signed certificate in the chain, at its position. */
	roots[0] = make_root("Other Root", 9);
	store_with(&store, roots, 1);
	list[0] = make_leaf("a.test", 3, "Inter", 1);
	list[1] = make_ca("Inter", 1, "Root", 0);
	list[2] = make_root("Root", 0);
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_SELF_SIGNED_IN_CHAIN, 2);
	CHECK(result.chain_count == 3U);
	cryc_store_free(&store);

	/* A store root with the same name but another key is a candidate for a certificate that names no key identifier (the root the server sent, the intermediate without one), and its signature then fails. */
	roots[0] = make_root("Root", 7);
	store_with(&store, roots, 1);
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_SIGNATURE_FAILURE, 2);
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	list[1] = make(&spec);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_SIGNATURE_FAILURE, 1);
	CHECK(result.chain_count == 3U);
	cryc_store_free(&store);

	/* An issuer that is not there: the longest partial path with the position where it stopped. */
	roots[0] = make_root("Other Root", 9);
	store_with(&store, roots, 1);
	list[0] = make_leaf("a.test", 3, "Inter", 1);
	list[1] = make_ca("Inter", 1, "Root", 0);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNABLE_TO_GET_ISSUER, 1);
	CHECK(result.chain_count == 2U);
	run(&store, list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNABLE_TO_GET_ISSUER, 0);
	CHECK(result.chain_count == 1U);
	cryc_store_free(&store);

	/* A cycle (A issued by B, B issued by A) ends: the search does not loop. */
	roots[0] = make_root("Other Root", 9);
	store_with(&store, roots, 1);
	list[0] = make_leaf("a.test", 3, "A", 10);
	list[1] = make_ca("A", 10, "B", 11);
	list[2] = make_ca("B", 11, "A", 10);
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNABLE_TO_GET_ISSUER, 2);
	CHECK(result.chain_count == 3U);
	cryc_store_free(&store);

	/* Two candidates for an issuer that both lead nowhere: the partial path reported is the first found. */
	roots[0] = make_root("Other Root", 9);
	store_with(&store, roots, 1);
	spec_init(&spec, "Mid", 1, "Nowhere", 14);
	spec_ca(&spec);
	list[1] = make(&spec);
	spec.serial = 2;
	list[2] = make(&spec);
	list[0] = make_leaf("a.test", 3, "Mid", 1);
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNABLE_TO_GET_ISSUER, 1);
	CHECK(result.chain_count == 2U);
	CHECK(result.chain[1]->der == list[1]->der);

	/* A root the server sent right after the leaf, unknown to the store: self-signed in the chain at depth 1. */
	list[0] = make_leaf("a.test", 3, "Root", 0);
	list[1] = make_root("Root", 0);
	run(&store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_SELF_SIGNED_IN_CHAIN, 1);
	CHECK(result.chain_count == 2U);
	cryc_store_free(&store);

	/* A self-issued certificate that does have a candidate issuer is not a dead end of the self-signed kind: the failure is the missing issuer of the candidate. */
	spec_init(&spec, "S", 1, "S", 1);
	spec_ca(&spec);
	spec.aki = 0;
	list[1] = make(&spec);
	spec_init(&spec, "S", 14, "Missing", 15);
	spec_ca(&spec);
	spec.aki = 0;
	list[2] = make(&spec);
	spec_init(&spec, "a.test", 3, "S", 1);
	spec.dns = "a.test";
	spec.aki = 0;
	list[0] = make(&spec);
	run(&store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNABLE_TO_GET_ISSUER, 2);
	CHECK(result.chain_count == 3U);

	/* A hostile chain of look-alikes with no way to an anchor: the work bound ends it with WORK_EXCEEDED and the longest partial path. */
	roots[0] = make_root("Root", 0);
	store_with(&store, roots, 1);
	spec_init(&spec, "a.test", 1, "J", 1);
	spec.dns = "a.test";
	spec.aki = 0;
	list[0] = make(&spec);
	for (index = 0; index < 15U; index++) {
		junk[index] = make_junk("J", 8U + index);
		list[1U + index] = junk[index];
	}
	/* With 1 to 15 look-alikes: a bound stops the search where the model says (with 9 or more the pairs alone are more than CRYC_CHAIN_MAX_WORK, with 6 or more the paths alone are more than the nodes allowed), and with fewer the search ends with every look-alike on the path, the last one self-signed in the chain. */
	for (count = 1; count <= 15U; count++) {
		model_run(count);
		run(&store, list, count + 1U, &params, &result);
		CHECK(result.chain[0]->der == list[0]->der);
		if (model_stopped != 0) {
			EXPECT(result, CRYC_VERIFY_WORK_EXCEEDED, model_depth);
			CHECK(result.chain_count == (count + 1U < CRYC_CHAIN_MAX_DEPTH ? count + 1U : CRYC_CHAIN_MAX_DEPTH));
		} else {
			EXPECT(result, CRYC_VERIFY_SELF_SIGNED_IN_CHAIN, count);
			CHECK(result.chain_count == count + 1U);
		}
		if (count == 5U)
			CHECK(model_stopped == 0);
		if (count == 6U)
			CHECK(model_stopped != 0);
	}
	cryc_store_free(&store);

	/* Exactly CRYC_CHAIN_MAX_WORK verifications are allowed: 8 look-alikes of one name below the leaf and 7 of another below those need 8 + 8 * 7 and no more, and end in a missing issuer, not in the bound. */
	roots[0] = make_root("Root", 0);
	store_with(&store, roots, 1);
	spec_init(&spec, "a.test", 1, "J", 1);
	spec.dns = "a.test";
	spec.aki = 0;
	list[0] = make(&spec);
	for (index = 0; index < 8U; index++) {
		spec_init(&spec, "J", 8U + index, "I", 16);
		spec_ca(&spec);
		spec.aki = 0;
		list[1U + index] = make(&spec);
	}
	for (index = 0; index < 7U; index++) {
		spec_init(&spec, "I", 16U + index, "X", 23);
		spec_ca(&spec);
		spec.aki = 0;
		list[9U + index] = make(&spec);
	}
	run(&store, list, 16, &params, &result);
	EXPECT(result, CRYC_VERIFY_UNABLE_TO_GET_ISSUER, 2);
	CHECK(result.chain_count == 3U);
	cryc_store_free(&store);

	/* A complete path found first keeps its own code over the work bound that ends the search later. */
	roots[0] = make_root("Root", 0);
	store_with(&store, roots, 1);
	spec_init(&spec, "J", 1, "Root", 0);
	spec_ca(&spec);
	spec.not_after = "260101000001Z";
	list[1] = make(&spec);
	for (index = 0; index < 13U; index++)
		list[2U + index] = junk[index];
	run(&store, list, 15, &params, &result);
	EXPECT(result, CRYC_VERIFY_EXPIRED, 1);
	CHECK(result.chain_count == 3U);
	CHECK(result.chain[1]->der == list[1]->der);
	cryc_store_free(&store);

	pool_used = mark;
}

/* The distrusted marks: the lowest depth, other paths, other anchors, order of adding. */
static void
test_verify_distrusted(void)
{
	struct standard chain;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct built *list[17];
	struct built *junk[16];
	struct built *roots[2];
	struct built *self_signed;
	struct built *inter_a;
	struct built *inter_b;
	struct built *root_again;
	struct built *expired_leaf;
	struct built *malleated;
	struct spec spec;
	unsigned char garbage[8];
	unsigned index;
	unsigned mark;

	mark = pool_used;
	standard_make(&chain);
	params_init(&params);

	/* A mark on the leaf rejects at depth 0; the path is reported; a certificate that is not on the path changes nothing. */
	memset(garbage, 0x30, sizeof(garbage));
	CHECK(cryc_store_add_distrusted(&chain.store, garbage, sizeof(garbage)) == 0);
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(cryc_store_add_distrusted(&chain.store, chain.leaf->der, chain.leaf->size) == 0);
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 0);
	CHECK(result.chain_count == 3U);
	cryc_store_free(&chain.store);

	/* A mark on the intermediate: depth 1. */
	store_with(&chain.store, &chain.root, 1);
	CHECK(cryc_store_add_distrusted(&chain.store, chain.inter->der, chain.inter->size) == 0);
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 1);

	/* The same intermediate with its other valid ECDSA signature (r, n - s): another DER, the same tbsCertificate, still rejected. */
	malleated = malleate(chain.inter, chain.root->cert.key.curve);
	CHECK(malleated != NULL);
	if (malleated != NULL) {
		CHECK(malleated->size != chain.inter->size || memcmp(malleated->der, chain.inter->der, chain.inter->size) != 0);
		list[0] = chain.leaf;
		list[1] = malleated;
		run(&chain.store, list, 2, &params, &result);
		EXPECT(result, CRYC_VERIFY_REJECTED, 1);
	}
	cryc_store_free(&chain.store);

	/* Without the mark the re-encoded intermediate is a valid link (the signature verifies). */
	if (malleated != NULL) {
		store_with(&chain.store, &chain.root, 1);
		run(&chain.store, list, 2, &params, &result);
		EXPECT(result, CRYC_VERIFY_OK, 0);
		cryc_store_free(&chain.store);
	}

	/* A mark on the anchor in the store: depth 2. */
	store_with(&chain.store, &chain.root, 1);
	CHECK(cryc_store_add_distrusted(&chain.store, chain.root->der, chain.root->size) == 0);
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 2);

	/* Marks on the intermediate and the anchor: the lowest; on the leaf and the anchor: the leaf. */
	CHECK(cryc_store_add_distrusted(&chain.store, chain.inter->der, chain.inter->size) == 0);
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 1);
	CHECK(cryc_store_add_distrusted(&chain.store, chain.leaf->der, chain.leaf->size) == 0);
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 0);
	cryc_store_free(&chain.store);

	/* The mark wins over the other checks of the path: an expired leaf marked is rejected, not expired. */
	store_with(&chain.store, &chain.root, 1);
	spec_init(&spec, "a.test", 3, "Inter", 1);
	spec.dns = "a.test";
	spec.not_after = "260101000001Z";
	expired_leaf = make(&spec);
	list[0] = expired_leaf;
	list[1] = chain.inter;
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_EXPIRED, 0);
	CHECK(cryc_store_add_distrusted(&chain.store, expired_leaf->der, expired_leaf->size) == 0);
	run(&chain.store, list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 0);
	cryc_store_free(&chain.store);

	/* A mark added before the certificate is added to the store still rejects it, and adding it again does not undo the mark. */
	CHECK(cryc_store_init(&chain.store) == 0);
	CHECK(cryc_store_add_distrusted(&chain.store, chain.root->der, chain.root->size) == 0);
	CHECK(cryc_store_add_der(&chain.store, chain.root->der, chain.root->size) == 0);
	CHECK(cryc_store_add_der(&chain.store, chain.root->der, chain.root->size) == 0);
	CHECK(chain.store.count == 1U);
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 2);
	cryc_store_free(&chain.store);

	/* An intermediate in the store as the anchor, marked. */
	roots[0] = chain.inter;
	store_with(&chain.store, roots, 1);
	run(&chain.store, chain.list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(cryc_store_add_distrusted(&chain.store, chain.inter->der, chain.inter->size) == 0);
	run(&chain.store, chain.list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 1);
	cryc_store_free(&chain.store);

	/* A trusted self-signed server that is marked is rejected at depth 0. */
	spec_init(&spec, "self.test", 6, "self.test", 6);
	spec.dns = "self.test";
	spec.aki = 0;
	self_signed = make(&spec);
	list[0] = self_signed;
	store_with(&chain.store, list, 1);
	run(&chain.store, list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(cryc_store_add_distrusted(&chain.store, self_signed->der, self_signed->size) == 0);
	run(&chain.store, list, 1, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 0);
	CHECK(result.chain_count == 1U);
	cryc_store_free(&chain.store);

	/* Two paths: one through a marked intermediate, one through its unmarked twin (the same name and key, another serial): the other path passes. */
	spec_init(&spec, "Inter", 1, "Root", 0);
	spec_ca(&spec);
	inter_a = make(&spec);
	spec.serial = 2;
	inter_b = make(&spec);
	store_with(&chain.store, &chain.root, 1);
	CHECK(cryc_store_add_distrusted(&chain.store, inter_a->der, inter_a->size) == 0);
	list[0] = chain.leaf;
	list[1] = inter_a;
	list[2] = inter_b;
	run(&chain.store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain[1]->der == inter_b->der);
	list[1] = inter_b;
	list[2] = inter_a;
	run(&chain.store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain[1]->der == inter_b->der);

	/* With both twins marked the code is the first path's: depth 1. */
	CHECK(cryc_store_add_distrusted(&chain.store, inter_b->der, inter_b->size) == 0);
	run(&chain.store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 1);
	cryc_store_free(&chain.store);

	/* The first complete path's code stands: a rejected path first, then one that is expired. */
	spec.not_after = "260101000001Z";
	spec.serial = 3;
	inter_b = make(&spec);
	store_with(&chain.store, &chain.root, 1);
	CHECK(cryc_store_add_distrusted(&chain.store, inter_a->der, inter_a->size) == 0);
	list[1] = inter_a;
	list[2] = inter_b;
	run(&chain.store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 1);
	list[1] = inter_b;
	list[2] = inter_a;
	run(&chain.store, list, 3, &params, &result);
	EXPECT(result, CRYC_VERIFY_EXPIRED, 1);
	cryc_store_free(&chain.store);

	/* Two anchors for the same name and key (a root and its re-issue): the marked one is skipped for the other. */
	root_again = make_root("Root", 0);
	spec_init(&spec, "Root", 0, "Root", 0);
	spec_ca(&spec);
	spec.aki = 0;
	spec.serial = 9;
	root_again = make(&spec);
	roots[0] = chain.root;
	roots[1] = root_again;
	store_with(&chain.store, roots, 2);
	CHECK(cryc_store_add_distrusted(&chain.store, chain.root->der, chain.root->size) == 0);
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_OK, 0);
	CHECK(result.chain[2]->der == chain.store.entries[1]->der);
	CHECK(cryc_store_add_distrusted(&chain.store, root_again->der, root_again->size) == 0);
	run(&chain.store, chain.list, 2, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 2);
	cryc_store_free(&chain.store);

	/* A rejected path is found without work, so a work bound that stops the search after it does not change the answer. */
	store_with(&chain.store, &chain.root, 1);
	spec_init(&spec, "a.test", 1, "J", 1);
	spec.dns = "a.test";
	spec.aki = 0;
	list[0] = make(&spec);
	spec_init(&spec, "J", 1, "Root", 0);
	spec_ca(&spec);
	list[1] = make(&spec);
	CHECK(cryc_store_add_distrusted(&chain.store, list[1]->der, list[1]->size) == 0);
	for (index = 0; index < 13U; index++) {
		junk[index] = make_junk("J", 8U + index);
		list[2U + index] = junk[index];
	}
	run(&chain.store, list, 15, &params, &result);
	EXPECT(result, CRYC_VERIFY_REJECTED, 1);
	CHECK(result.chain_count == 3U);
	cryc_store_free(&chain.store);

	pool_used = mark;
}

/* The largest code a result may hold. */
#define CODE_LAST	((int)CRYC_VERIFY_REJECTED)

/* Damaged and shuffled input: no crash, a code of the enumeration, and a chain that is what a valid result promises. */
static void
test_verify_random(void)
{
	struct standard chain;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct cryc_x509 peer[PEER_ROOM];
	struct built *others[8];
	struct built *clone;
	struct built *list[PEER_ROOM];
	struct built *roots[3];
	struct cryc_store store;
	unsigned long damaged_parsed;
	unsigned long ok_count;
	size_t count;
	size_t position;
	unsigned index;
	unsigned round;
	unsigned mark;
	int error;

	mark = pool_used;
	standard_make(&chain);
	params_init(&params);
	clone = &pool[pool_used++];

	/* One byte of the leaf or of the intermediate changed: when it still parses it is never a valid path (everything is signed or checked). */
	damaged_parsed = 0;
	for (round = 0; round < 600U; round++) {
		list[0] = chain.leaf;
		list[1] = chain.inter;
		index = rng_next() % 2U;
		clone->size = list[index]->size;
		memcpy(clone->der, list[index]->der, clone->size);
		position = rng_next() % clone->size;
		clone->der[position] = (unsigned char)(clone->der[position] ^ (1U << (rng_next() % 8U)));
		error = cryc_x509_parse(&clone->cert, clone->der, clone->size);
		if (error != 0)
			continue;
		damaged_parsed++;
		list[index] = clone;
		run(&chain.store, list, 2, &params, &result);
		CHECK((int)result.code >= 0 && (int)result.code <= CODE_LAST);
		CHECK(result.code != CRYC_VERIFY_OK);
	}
	CHECK(damaged_parsed > 20UL);
	cryc_store_free(&chain.store);

	/* Random subsets and orders of certificates and random stores: any outcome in range, and a valid one is well formed. */
	others[0] = chain.leaf;
	others[1] = chain.inter;
	others[2] = chain.root;
	others[3] = make_ca("Inter", 1, "Root", 0);
	others[4] = make_root("Root", 7);
	others[5] = make_leaf("b.test", 4, "Inter", 1);
	others[6] = make_junk("Inter", 5);
	others[7] = make_ca("Inter 2", 2, "Inter", 1);
	ok_count = 0;
	for (round = 0; round < 1500U; round++) {
		count = 1U + rng_next() % 9U;
		for (index = 0; index < count; index++) {
			list[index] = others[rng_next() % 8U];
			peer[index] = list[index]->cert;
		}
		roots[0] = others[2];
		roots[1] = others[4];
		roots[2] = others[1];
		error = cryc_store_init(&store);
		CHECK(error == 0);
		for (index = 0; index < 3U; index++) {
			if ((rng_next() & 1U) != 0U)
				cryc_store_add_der(&store, roots[index]->der, roots[index]->size);
		}
		if ((rng_next() & 7U) == 0U)
			cryc_store_add_distrusted(&store, others[rng_next() % 8U]->der, 10);
		if ((rng_next() & 7U) == 0U) {
			position = rng_next() % 8U;
			cryc_store_add_distrusted(&store, others[position]->der, others[position]->size);
		}
		params.max_depth = rng_next() % 6U;
		if (params.max_depth == 1U)
			params.max_depth = 2;
		error = cryc_chain_verify(&store, peer, count, &params, &result);
		CHECK(error == 0);
		CHECK((int)result.code >= 0 && (int)result.code <= CODE_LAST);
		CHECK(result.chain_count <= CRYC_CHAIN_MAX_DEPTH);
		if (result.code == CRYC_VERIFY_OK) {
			ok_count++;
			CHECK(result.chain_count >= 1U);
			CHECK(result.chain[0]->der == peer[0].der);
			CHECK(result.depth == 0U);
		}
		cryc_store_free(&store);
	}
	CHECK(ok_count > 50UL);
	params.max_depth = 0;

	pool_used = mark;
}

/* The real bundle of root certificates (WS335_CACERT, as test-pem.c reads it): all load, each as a trusted server, each marked. */
static void
test_bundle(void)
{
	struct cryc_store store;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct cryc_x509 peer[1];
	const char *path;
	size_t index;
	size_t added;
	unsigned long kinds[CODE_LAST + 1];
	int error;

	/* The bundle must be readable: a missing file is a failure, not a pass. */
	path = getenv("WS335_CACERT");
	if (path == NULL)
		path = "/home/awe/zedBSD-claude1/build/distfiles/cacert-2026-08-13.pem";
	CHECK(cryc_store_init(&store) == 0);
	error = cryc_store_add_file(&store, path);
	CHECK(error == 0);
	CHECK(store.count > 100U);
	if (error != 0) {
		cryc_store_free(&store);
		return;
	}
	params_init(&params);

	/* Loading it again adds nothing. */
	added = store.count;
	CHECK(cryc_store_add_file(&store, path) == 0);
	CHECK(store.count == added);

	/* Each root alone as the peer: a one-certificate path, with a code the leaf's checks can give and never a work, depth or reject code. */
	memset(kinds, 0, sizeof(kinds));
	for (index = 0; index < store.count; index++) {
		peer[0] = store.entries[index]->cert;
		error = cryc_chain_verify(&store, peer, 1, &params, &result);
		CHECK(error == 0);
		CHECK((int)result.code >= 0 && (int)result.code <= CODE_LAST);
		kinds[result.code]++;
		CHECK(result.code != CRYC_VERIFY_REJECTED);
		CHECK(result.code != CRYC_VERIFY_WORK_EXCEEDED);
		CHECK(result.code != CRYC_VERIFY_CHAIN_TOO_LONG);
		CHECK(result.code != CRYC_VERIFY_MALFORMED);
		CHECK(result.chain_count >= 1U);
	}
	printf("bundle: %u roots; as a server: %lu valid, %lu key usage, %lu expired, %lu other\n", (unsigned)store.count, kinds[CRYC_VERIFY_OK], kinds[CRYC_VERIFY_KEY_USAGE], kinds[CRYC_VERIFY_EXPIRED], (unsigned long)store.count - kinds[CRYC_VERIFY_OK] - kinds[CRYC_VERIFY_KEY_USAGE] - kinds[CRYC_VERIFY_EXPIRED]);

	/* With every root marked distrusted every one is rejected at depth 0. */
	for (index = 0; index < store.count; index++) {
		error = cryc_store_add_distrusted(&store, store.entries[index]->der, store.entries[index]->der_size);
		CHECK(error == 0);
	}
	CHECK(store.distrusted_count == store.count);
	for (index = 0; index < store.count; index++) {
		peer[0] = store.entries[index]->cert;
		error = cryc_chain_verify(&store, peer, 1, &params, &result);
		CHECK(error == 0);
		CHECK(result.code == CRYC_VERIFY_REJECTED);
		CHECK(result.depth == 0U);
	}
	cryc_store_free(&store);
}

/* x509-limbo
 */

/* Parses a text IP address (IPv4 dotted, or IPv6 with groups and one "::") into out; returns 4, 16, or 0 when it is not one. */
static size_t
parse_ip(
	const char *text,
	unsigned char *out)
{
	unsigned parts[8];
	unsigned head;
	unsigned tail;
	unsigned value;
	unsigned digits;
	unsigned index;
	unsigned total;
	const char *at;
	int double_colon;
	unsigned groups[8];
	unsigned count;

	/* IPv4: four decimal numbers separated by dots. */
	if (strchr(text, ':') == NULL) {
		at = text;
		for (index = 0; index < 4U; index++) {
			value = 0;
			digits = 0;
			while (*at >= '0' && *at <= '9') {
				value = value * 10U + (unsigned)(*at - '0');
				digits++;
				at++;
				if (value > 255U || digits > 3U)
					return 0;
			}
			if (digits == 0U)
				return 0;
			out[index] = (unsigned char)value;
			if (index < 3U) {
				if (*at != '.')
					return 0;
				at++;
			}
		}
		if (*at != '\0')
			return 0;
		return 4;
	}

	/* IPv6: groups of up to four hex digits, with at most one "::" standing for zero groups. */
	head = 0;
	tail = 0;
	double_colon = 0;
	count = 0;
	at = text;
	if (at[0] == ':' && at[1] == ':') {
		double_colon = 1;
		at += 2;
	} else if (at[0] == ':') {
		return 0;
	}
	while (*at != '\0') {
		value = 0;
		digits = 0;
		while ((*at >= '0' && *at <= '9') || (*at >= 'a' && *at <= 'f') || (*at >= 'A' && *at <= 'F')) {
			if (*at <= '9')
				value = value * 16U + (unsigned)(*at - '0');
			else if (*at >= 'a')
				value = value * 16U + (unsigned)(*at - 'a' + 10);
			else
				value = value * 16U + (unsigned)(*at - 'A' + 10);
			digits++;
			at++;
		}
		if (digits == 0U || digits > 4U || count >= 8U)
			return 0;
		groups[count++] = value;
		if (double_colon)
			tail++;
		else
			head++;
		if (*at == ':') {
			if (at[1] == ':') {
				if (double_colon)
					return 0;
				double_colon = 1;
				at += 2;
			} else {
				at++;
				if (*at == '\0')
					return 0;
			}
		} else if (*at != '\0') {
			return 0;
		}
	}
	total = head + tail;
	if (double_colon && total > 7U)
		return 0;
	if (!double_colon && total != 8U)
		return 0;

	/* The groups go in order with zeros between the head and the tail. */
	for (index = 0; index < 8U; index++)
		parts[index] = 0;
	for (index = 0; index < head; index++)
		parts[index] = groups[index];
	for (index = 0; index < tail; index++)
		parts[8U - tail + index] = groups[head + index];
	for (index = 0; index < 8U; index++) {
		out[index * 2U] = (unsigned char)(parts[index] >> 8);
		out[index * 2U + 1U] = (unsigned char)(parts[index] & 0xffU);
	}
	return 16;
}

/*
 * The x509-limbo cases whose outcome here differs from the case's
 * expectation, with the reason.  A case in this table must differ; a case
 * that differs and is not in it is a failure.  The reasons are those of
 * test-limbo.c, which runs every case of the corpus:
 *
 *   issuance        a rule RFC 5280 sets for issuing CAs, not checked by
 *                   path validation (6.1).
 *   cabf            a rule of the CA/Browser Forum Baseline Requirements.
 *   anchor          a fault of the trust anchor's own contents, which are
 *                   taken as they are (6.1.1 (d)).
 *   name            a name rule cryc-name.h defines otherwise.
 *   key-usage       a leaf whose keyUsage lacks digitalSignature is
 *                   refused (the header); the case accepts it.
 *   depth           the case does not count self-issued intermediates.
 *   nc-noncritical  nameConstraints that are not critical are processed.
 *   nc-leaf         the leaf's own nameConstraints constrain nothing.
 */
struct limbo_exception {
	const char *id;
	const char *reason;
	int seen;
};

static struct limbo_exception limbo_exceptions[] = {
	{ "pathlen::validation-ignores-pathlen-in-leaf", "key-usage", 0 },
	{ "pathlen::max-chain-depth-1-self-issued", "depth", 0 },
	{ "rfc5280::aki::critical-aki", "issuance", 0 },
	{ "rfc5280::aki::leaf-missing-aki", "issuance", 0 },
	{ "rfc5280::aki::intermediate-missing-aki", "issuance", 0 },
	{ "rfc5280::aki::cross-signed-root-missing-aki", "issuance", 0 },
	{ "rfc5280::nc::permitted-dns-match-noncritical", "nc-noncritical", 0 },
	{ "rfc5280::nc::invalid-dnsname-leading-period", "name", 0 },
	{ "rfc5280::nc::not-allowed-in-ee-noncritical", "nc-leaf", 0 },
	{ "rfc5280::nc::not-allowed-in-ee-critical", "nc-leaf", 0 },
	{ "rfc5280::pc::ica-noncritical-pc", "issuance", 0 },
	{ "rfc5280::san::noncritical-with-empty-subject", "issuance", 0 },
	{ "rfc5280::san::underscore-dns", "name", 0 },
	{ "rfc5280::serial::zero", "issuance", 0 },
	{ "rfc5280::ski::root-missing-ski", "issuance", 0 },
	{ "rfc5280::ski::intermediate-missing-ski", "issuance", 0 },
	{ "rfc5280::root-missing-basic-constraints", "anchor", 0 },
	{ "rfc5280::root-non-critical-basic-constraints", "issuance", 0 },
	{ "rfc5280::leaf-ku-keycertsign", "issuance", 0 },
	{ "rfc5280::ca-as-leaf", "key-usage", 0 },
	{ "webpki::aki::root-with-aki-missing-keyidentifier", "cabf", 0 },
	{ "webpki::aki::root-with-aki-authoritycertissuer", "cabf", 0 },
	{ "webpki::aki::root-with-aki-authoritycertserialnumber", "cabf", 0 },
	{ "webpki::aki::root-with-aki-all-fields", "cabf", 0 },
	{ "webpki::aki::root-with-aki-ski-mismatch", "cabf", 0 },
	{ "webpki::cn::ipv4-hex-mismatch", "cabf", 0 },
	{ "webpki::cn::ipv4-leading-zeros-mismatch", "cabf", 0 },
	{ "webpki::cn::ipv6-uppercase-mismatch", "cabf", 0 },
	{ "webpki::cn::ipv6-uncompressed-mismatch", "cabf", 0 },
	{ "webpki::cn::ipv6-non-rfc5952-mismatch", "cabf", 0 },
	{ "webpki::cn::punycode-not-in-san", "cabf", 0 },
	{ "webpki::cn::utf8-vs-punycode-mismatch", "cabf", 0 },
	{ "webpki::cn::not-in-san", "cabf", 0 },
	{ "webpki::cn::case-mismatch", "cabf", 0 },
	{ "webpki::eku::ee-anyeku", "cabf", 0 },
	{ "webpki::eku::ee-critical-eku", "cabf", 0 },
	{ "webpki::eku::ee-without-eku", "cabf", 0 },
	{ "webpki::eku::root-has-eku", "cabf", 0 },
	{ "webpki::san::public-suffix-multi-label-wildcard-san", "name", 0 },
	{ "webpki::san::public-suffix-private-namespace-wildcard-san", "name", 0 },
	{ "webpki::san::san-critical-with-nonempty-subject", "cabf", 0 },
	{ "webpki::malformed-aia", "cabf", 0 },
	{ "webpki::forbidden-rsa-not-divisible-by-8-in-root", "cabf", 0 },
	{ "webpki::forbidden-rsa-key-not-divisible-by-8-in-leaf", "cabf", 0 },
	{ "webpki::ee-basicconstraints-ca", "cabf", 0 },
	{ NULL, NULL, 0 }
};

/* The counts of the limbo run. */
static unsigned long limbo_cases;
static unsigned long limbo_agree;
static unsigned long limbo_explained;
static unsigned long limbo_unexplained;
static unsigned long limbo_success;
static unsigned long limbo_success_ok;
static unsigned long limbo_refused_unparsed;
static unsigned long limbo_truncated;

/* Reads decimal seconds since 1970 in 64 bits (long is 32 bits on Windows). */
static int64_t
parse_time(
	const char *text)
{
	int64_t value;

	/* Digits only. */
	value = 0;
	while (*text >= '0' && *text <= '9') {
		value = value * 10 + (int64_t)(*text - '0');
		text++;
	}
	return value;
}

/* Runs one case: the trusted certificates are the store, the peer and the untrusted intermediates (up to 15) the server's list. */
static void
limbo_run(
	const struct vec_file *file,
	unsigned char *buffer)
{
	struct cryc_store store;
	struct cryc_verify_params params;
	struct cryc_verify_result result;
	struct cryc_x509 peer[PEER_MAX];
	unsigned char *der[PEER_MAX];
	const char *id;
	const char *expected;
	const char *time_text;
	const char *kind;
	const char *features;
	char key[32];
	size_t size;
	size_t peer_count;
	size_t depth;
	unsigned index;
	unsigned entry;
	int success;
	int ok;
	int explained;
	int error;

	id = vec_get(file, "id");
	expected = vec_get(file, "expected");
	success = strcmp(expected, "SUCCESS") == 0;
	limbo_cases++;
	if (success)
		limbo_success++;

	/* The trusted certificates are the store; one that does not parse is left out (it cannot be an anchor). */
	error = cryc_store_init(&store);
	CHECK(error == 0);
	for (index = 0; index < 4U; index++) {
		sprintf(key, "trusted.%u", index);
		size = vec_hex(file, key, buffer, LIMBO_MAX);
		if (size == (size_t)-1)
			continue;
		cryc_store_add_der(&store, buffer, size);
	}

	/* The peer, and the untrusted certificates as the server's list (a real server sends at most 16 certificates, so a longer list is cut). */
	peer_count = 0;
	memset(der, 0, sizeof(der));
	size = vec_hex(file, "peer", buffer, LIMBO_MAX);
	if (size == (size_t)-1) {
		CHECK(0);
		cryc_store_free(&store);
		return;
	}
	der[0] = malloc(size);
	CHECK(der[0] != NULL);
	if (der[0] == NULL) {
		cryc_store_free(&store);
		return;
	}
	memcpy(der[0], buffer, size);
	error = cryc_x509_parse(&peer[0], der[0], size);
	if (error != 0) {
		/* A leaf that does not parse is refused before any validation: it counts as FAILURE. */
		limbo_refused_unparsed++;
		ok = 0;
		if (ok == success) {
			limbo_agree++;
		} else {
			limbo_unexplained++;
			printf("limbo: %s: unparsed leaf, expected %s\n", id, expected);
		}
		free(der[0]);
		cryc_store_free(&store);
		return;
	}
	peer_count = 1;
	entry = 1;
	for (index = 0; index < 120U; index++) {
		sprintf(key, "untrusted.%u", index);
		size = vec_hex(file, key, buffer, LIMBO_MAX);
		if (size == (size_t)-1)
			continue;
		if (peer_count >= PEER_MAX) {
			limbo_truncated++;
			break;
		}
		der[entry] = malloc(size);
		CHECK(der[entry] != NULL);
		if (der[entry] == NULL)
			break;
		memcpy(der[entry], buffer, size);
		error = cryc_x509_parse(&peer[peer_count], der[entry], size);
		if (error == 0)
			peer_count++;
		entry++;
	}

	/* The parameters: the case's time (else the clock), name or address, and depth (limbo counts the intermediates, this counts all). */
	memset(&params, 0, sizeof(params));
	time_text = vec_get(file, "time");
	if (time_text != NULL && time_text[0] != '\0') {
		params.has_time = 1;
		params.time = parse_time(time_text);
	}
	kind = vec_get(file, "peer_kind");
	if (kind != NULL && strcmp(kind, "DNS") == 0) {
		params.has_host = 1;
		strncpy(params.host, vec_get(file, "peer_value"), CRYC_NAME_HOST_MAX);
	} else if (kind != NULL) {
		params.ip_size = parse_ip(vec_get(file, "peer_value"), params.ip);
	}
	depth = (size_t)atol(vec_get(file, "max_depth"));
	features = vec_get(file, "features");
	if (depth != 0U)
		params.max_depth = depth + 2U;
	else if (features != NULL && strstr(features, "max-chain-depth") != NULL)
		params.max_depth = 2;

	/* Validates, and compares with the case's expectation. */
	error = cryc_chain_verify(&store, peer, peer_count, &params, &result);
	CHECK(error == 0);
	CHECK((int)result.code >= 0 && (int)result.code <= CODE_LAST);
	ok = result.code == CRYC_VERIFY_OK;
	if (ok != 0 && success != 0)
		limbo_success_ok++;
	if (ok == success) {
		limbo_agree++;
	} else {
		explained = 0;
		for (index = 0; limbo_exceptions[index].id != NULL; index++) {
			if (strcmp(limbo_exceptions[index].id, id) == 0) {
				limbo_exceptions[index].seen++;
				explained = 1;
			}
		}
		if (explained != 0) {
			limbo_explained++;
		} else {
			limbo_unexplained++;
			printf("limbo: %s: expected %s, got code %d depth %u\n", id, expected, (int)result.code, (unsigned)result.depth);
		}
	}

	/* A valid result is well formed. */
	if (ok != 0) {
		CHECK(result.chain_count >= 1U);
		CHECK(result.chain[0]->der == peer[0].der);
	}
	for (index = 0; index < PEER_MAX; index++)
		free(der[index]);
	cryc_store_free(&store);
}

/* Runs the cases with the ids rfc5280::, pathlen::, webpki:: and pathological::. */
static void
test_limbo(void)
{
	struct vec_file file;
	unsigned char *buffer;
	const char *id;
	unsigned index;

	buffer = malloc(LIMBO_MAX);
	CHECK(buffer != NULL);
	if (buffer == NULL)
		return;
	if (vec_open(&file, "limbo") != 0) {
		CHECK(0);
		free(buffer);
		return;
	}
	while (vec_next(&file)) {
		id = vec_get(&file, "id");
		if (strncmp(id, "rfc5280::", 9) != 0 && strncmp(id, "pathlen::", 9) != 0 && strncmp(id, "webpki::", 8) != 0 && strncmp(id, "pathological::", 14) != 0)
			continue;
		limbo_run(&file, buffer);
	}
	vec_close(&file);
	free(buffer);

	/* Every listed case must really differ (a stale entry is a failure), and no other may. */
	for (index = 0; limbo_exceptions[index].id != NULL; index++) {
		if (limbo_exceptions[index].seen != 1)
			printf("limbo: listed case did not differ once: %s\n", limbo_exceptions[index].id);
		CHECK(limbo_exceptions[index].seen == 1);
	}
	printf("limbo: %lu cases (%lu SUCCESS); %lu agree with the case, %lu differ with a listed reason, %lu unexplained; %lu SUCCESS cases valid; %lu unparsed leaves; %lu lists cut at 16\n", limbo_cases, limbo_success, limbo_agree, limbo_explained, limbo_unexplained, limbo_success_ok, limbo_refused_unparsed, limbo_truncated);
	CHECK(limbo_unexplained == 0UL);
	CHECK(limbo_cases == 179UL);
}

int
main(
	int argc,
	char **argv)
{
	size_t length;

	/* The directory of the program is where the test's files go; Windows runs in the directory of its own. */
	setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef _WIN32
	UNUSED_PARAMETER(argc);
	UNUSED_PARAMETER(argv);
	strcpy(test_dir, ".");
#else
	UNUSED_PARAMETER(argc);
	length = strlen(argv[0]);
	while (length > 0U && argv[0][length - 1U] != '/')
		length--;
	if (length > 1U && length < sizeof(test_dir)) {
		memcpy(test_dir, argv[0], length - 1U);
		test_dir[length - 1U] = '\0';
	} else {
		strcpy(test_dir, ".");
	}
#endif
	UNUSED_PARAMETER(length);

	test_store_basics();
	test_store_distrusted();
	test_store_limits();
	test_store_pem();
	test_store_file();
	test_store_system();
	test_store_threads();
	test_verify_arguments();
	test_verify_ok();
	test_verify_identifiers();
	test_verify_validity();
	test_verify_signatures();
	test_verify_roles();
	test_verify_pathlen();
	test_verify_names();
	test_verify_extensions();
	test_verify_depth();
	test_verify_host();
	test_verify_search();
	test_verify_distrusted();
	test_verify_random();
	test_bundle();
	test_limbo();
	return check_finish("test-chain");
}
