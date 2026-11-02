/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the public X509, X509_STORE and X509_VERIFY_PARAM functions
 * (api-x509.c): d2i_X509 and its conventions, the certificate and store
 * objects with their reference counts, X509_STORE_add_cert,
 * X509_VERIFY_PARAM_set1_host and set1_ip_asc, the verdict-to-X509_V_
 * mapping and X509_verify_cert_error_string.
 *
 * The certificates are built here from RFC 5280 4.1 and 4.2.1.6 with the
 * DER writer of derwriter.h (the signature is a placeholder: nothing in
 * this module checks it); a certificate with its own name in a store is
 * its own trust anchor (cryc-chain.h), which lets the whole chain from a
 * public call to a verification verdict be run without a signer.
 * The expected error codes are worked out by hand from the layout of
 * RFC-style packing (library in the bits from 23: X509 11, ASN.1 13;
 * reasons as OpenSSL numbers them with their flag bits: 258 and 0xc0000
 * for a null parameter, 262 and 0x80000 for an invalid argument, 268 and
 * 0x80000 for unsupported, 168 for the wrong tag, 123 for a header too
 * long).  The expected IPv4 and IPv6 bytes are hand-computed from RFC
 * 4291 2.2; a reference reader of addresses written differently from the
 * library's (by splitting at the colons first) is compared with it on
 * random and mutated text.  The expected texts of the verification
 * results are those of OpenSSL's documentation of X509_V_*, typed here.
 * The ca-certificates bundle and the x509-limbo certificates are read
 * through d2i_X509 and compared with the parser's own verdict.
 */

#include "check.h"
#include "derwriter.h"
#include "vecfile.h"

#include <limits.h>

#include "userland/base/libcrypto-compat/cryc-api.h"
#include "userland/base/libcrypto-compat/cryc-der.h"
#include "userland/base/libcrypto-compat/cryc-os.h"
#include "userland/base/libcrypto-compat/cryc-pem.h"

#include <compat/crypto/err.h>
#include <compat/crypto/x509.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* Where the bundle is looked for when WS335_CACERT is not set. */
#define CACERT_DEFAULT	"/home/awe/zedBSD-claude1/build/distfiles/cacert-2026-08-13.pem"

/* The largest certificate of the x509-limbo cases is about 78 KB. */
#define LIMBO_MAX	131072U

/* The error codes (library << 23 | reason without flag bits), worked out by hand. */
#define CODE_NULL_PARAMETER	0x058c0102UL
#define CODE_INVALID_ARGUMENT	0x05880106UL
#define CODE_UNSUPPORTED	0x0588010cUL
#define CODE_WRONG_TAG		0x068000a8UL
#define CODE_HEADER_TOO_LONG	0x0680007bUL

/* The most certificates a store holds, and the host name limits (RFC 1035 2.3.4). */
#define STORE_MAX		4096U
#define HOST_MAX		253U

/* The threads of the concurrency check, and the certificates each adds. */
#define THREADS			4U
#define PER_THREAD		100U
#define SHARED_CERTS		50U
#define REF_ROUNDS		5000U

/* The most error codes the drain helper remembers. */
#define ERR_KEEP		32U

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x2545f491U;

/* The codes of the last drain_errors call, and how many there were. */
static unsigned long err_codes[ERR_KEEP];
static unsigned err_total;

/*
 * A buffer with a DER writer over it.
 */
struct blob {
	unsigned char *data;
	struct der_writer writer;
};

/*
 * What to build into a certificate: a serial, a name, the names it
 * carries, and the size of an extension with no meaning (to make it
 * large).
 */
struct cert_spec {
	unsigned serial;
	const char *common_name;
	const char *san_host;
	const unsigned char *san_ip;
	size_t san_ip_size;
	size_t padding;
	unsigned long exact_content;
};

/* What a thread of the concurrency check is given. */
struct thread_job {
	X509_STORE *store;
	X509 **own;
	X509 **shared;
	X509 *counted;
	unsigned ok;
};

/*
 * A small DER part: a buffer of its own and a writer over it.
 */
struct small {
	unsigned char data[2048];
	struct der_writer writer;
};

/* The generator point of P-256 (SEC 2, 2.4.2: the base point G), which is a real point the public key can carry. */
static const unsigned char p256_g[65] = {
	0x04,
	0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
	0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
	0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
	0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5
};

/* The documentation address 192.0.2.7 (RFC 5737) and 2001:db8::1 (RFC 3849). */
static const unsigned char doc_ipv4[4] = { 192, 0, 2, 7 };
static const unsigned char doc_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

/* Returns the next pseudo-random number. */
static uint32_t next_random(void);

/* Empties the calling thread's error queue, keeping the first codes; returns how many there were. */
static unsigned drain_errors(void);

/* Drains the queue and says whether it held exactly one code, equal to expected. */
static int one_error(unsigned long expected);

/* Drains the queue and says whether it was empty. */
static int no_error(void);

/* Starts a blob with room bytes. */
static void blob_init(struct blob *blob, size_t room);

/* Frees a blob's buffer. */
static void blob_free(struct blob *blob);

/* Starts a small part. */
static void small_init(struct small *part);

/* Builds one Certificate (RFC 5280 4.1) as a malloc'd buffer with the given amount of padding. */
static unsigned char *build_once(const struct cert_spec *spec, size_t padding, size_t *size);

/* Builds a certificate to the spec, adjusting the padding when an exact content size is asked for. */
static unsigned char *build_cert(const struct cert_spec *spec, size_t *size);

/* Builds a certificate of a serial with a host name in its subjectAltName. */
static unsigned char *simple_cert(unsigned serial, size_t *size);

/* Returns a copy of data in a buffer of exactly size bytes (so that a read past the end is seen by the sanitizer). */
static unsigned char *exact_copy(const unsigned char *data, size_t size);

/* Makes an X509 from DER with d2i_X509, which the test needs to succeed. */
static X509 *make_cert(const unsigned char *der, size_t size);

/* The verdicts and their X509_V_ codes. */
static void test_verdicts(void);

/* The texts of the verification results. */
static void test_error_strings(void);

/* A certificate through d2i_X509: the object, the pointer's advance, the copy. */
static void test_d2i_basic(void);

/* The out argument: reuse, replacement and the failure convention. */
static void test_d2i_out(void);

/* The error cases of d2i_X509, one by one. */
static void test_d2i_errors(void);

/* Hostile and random input: nothing is read beyond the buffer, and every call either fails with one error or succeeds with none. */
static void test_d2i_hostile(void);

/* Large certificates, the length forms, the largest the interface allows and one octet more. */
static void test_d2i_sizes(void);

/* cryc_x509_from_der. */
static void test_from_der(void);

/* The reference counts of certificates and stores. */
static void test_references(void);

/* X509_STORE_add_cert. */
static void test_store(void);

/* The store's size limit: the last certificate that fits, and one more. */
static void test_store_limit(void);

/* Checks the host name that X509_VERIFY_PARAM_set1_host recorded: the stored form and the flags. */
static int host_is(const X509_VERIFY_PARAM *param, const char *expected);

/* X509_VERIFY_PARAM_set1_host. */
static void test_host(void);

/* Checks an address text: it must be accepted, recorded as the given octets, and clear a host name set before. */
static void check_ip(const char *text, const char *hex);

/* IP addresses that must be accepted, with the octets worked out from RFC 4291 2.2. */
static void test_ip_valid(void);

/* IP address texts that must be refused. */
static void test_ip_invalid(void);

/* Reference reader of a decimal IPv4 address, written by splitting at the dots first (the library scans character by character). */
static int reference_v4(const char *begin, size_t length, unsigned char *out);

/* Reference reader of an IPv6 address, written by splitting the text at the "::" first. */
static int reference_v6(const char *text, unsigned char *out);

/* Random and mutated texts: the library and the reference reader must agree on accepting, and on the octets. */
static void test_ip_differential(void);

/* How a host name and an address replace each other, and how clearing works. */
static void test_param_interplay(void);

/* Runs a validation of a peer certificate in a store with the parameters, and returns the X509_V_ code. */
static long validate(X509_STORE *store, X509 *peer, X509_VERIFY_PARAM *param);

/* The public calls working together: a trusted self-signed server, the names, and the verdicts. */
static void test_validation(void);

/* The thread function of the concurrency check. */
#ifdef _WIN32
static DWORD WINAPI thread_entry(LPVOID argument);
#else
static void *thread_entry(void *argument);
#endif

/* What each thread does: adds its own certificates and the shared ones to the store, and takes and drops references to one certificate. */
static void thread_body(struct thread_job *job);

/* Several threads adding to one store and sharing one certificate. */
static void test_threads(void);

/* Reads a whole file into a NUL-terminated buffer; NULL when it cannot be read. */
static char *read_file(const char *path, size_t *size);

/* Reads every certificate of the ca-certificates bundle with d2i_X509 and puts them in a store. */
static void test_bundle(void);

/* Reads every certificate of the x509-limbo cases with d2i_X509: it accepts exactly what the parser accepts. */
static void test_limbo(void);

/* Returns the next pseudo-random number. */
static uint32_t
next_random(void)
{
	/* One xorshift32 step. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* Empties the calling thread's error queue, keeping the first codes; returns how many there were. */
static unsigned
drain_errors(void)
{
	unsigned long code;
	unsigned count;

	/* Takes every code out. */
	count = 0;
	code = ERR_get_error();
	while (code != 0UL) {
		if (count < ERR_KEEP)
			err_codes[count] = code;
		count++;
		code = ERR_get_error();
	}

	/* Remembers how many codes there were. */
	err_total = count;

	/* The number of codes that were queued. */
	return count;
}

/* Drains the queue and says whether it held exactly one code, equal to expected. */
static int
one_error(
	unsigned long expected)
{
	unsigned count;

	/* One code, the expected one. */
	count = drain_errors();
	if (count != 1U)
		return 0;
	if (err_codes[0] != expected)
		return 0;

	/* Succeeded: the queue held exactly the expected code. */
	return 1;
}

/* Drains the queue and says whether it was empty. */
static int
no_error(void)
{
	unsigned count;

	/* Nothing queued. */
	count = drain_errors();
	if (count != 0U)
		return 0;

	/* Succeeded: the queue was empty. */
	return 1;
}

/* Starts a blob with room bytes. */
static void
blob_init(
	struct blob *blob,
	size_t room)
{
	/* The buffer must exist: a test cannot go on without it. */
	blob->data = malloc(room);
	if (blob->data == NULL) {
		printf("FAIL no memory for a test buffer\n");
		exit(EXIT_FAILURE);
	}

	/* Starts the DER writer over the buffer. */
	der_init(&blob->writer, blob->data, room);
}

/* Frees a blob's buffer. */
static void
blob_free(
	struct blob *blob)
{
	/* The buffer goes. */
	free(blob->data);
	blob->data = NULL;
}

/* Starts a small part. */
static void
small_init(
	struct small *part)
{
	/* Empty. */
	der_init(&part->writer, part->data, sizeof(part->data));
}

/*
 * Builds one Certificate (RFC 5280 4.1) as a malloc'd buffer with the
 * given amount of padding: version 3, the serial, a P-256 key, the same
 * Name as issuer and subject (CN = the common name), validity 2020-01-01 to
 * 2999-12-31, a subjectAltName with a dNSName and an iPAddress when given,
 * and an unknown non-critical extension of padding octets when padding is
 * not 0.
 */
static unsigned char *
build_once(
	const struct cert_spec *spec,
	size_t padding,
	size_t *size)
{
	static const unsigned char version[] = { 0x02, 0x01, 0x02 };
	static const unsigned char not_before[] = "20200101000000Z";
	static const unsigned char not_after[] = "29991231235959Z";
	static const unsigned char signature[] = { 0x00, 0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01 };
	struct small algorithm_content;
	struct small algorithm;
	struct small attribute;
	struct small attribute_seq;
	struct small rdn;
	struct small name;
	struct small validity_content;
	struct small validity;
	struct small key_algorithm_content;
	struct small key_algorithm;
	struct small spki_content;
	struct small spki;
	struct small names;
	struct small names_seq;
	struct small san_content;
	struct small san_extension;
	struct blob pad_content;
	struct blob pad_extension;
	struct blob extension_list;
	struct blob extension_seq;
	struct blob tbs_content;
	struct blob tbs;
	struct blob certificate_content;
	struct blob certificate;
	unsigned char serial[4];
	unsigned char bits[66];
	unsigned char *zeros;
	unsigned char *result;
	size_t room;

	/* The room that holds the fixed parts and the padding. */
	room = padding + 4096U;

	/* The signature algorithm: ecdsa-with-SHA256 (1.2.840.10045.4.3.2) with no parameters. */
	small_init(&algorithm_content);
	der_oid(&algorithm_content.writer, "1.2.840.10045.4.3.2");
	small_init(&algorithm);
	der_wrap(&algorithm.writer, 0x30U, &algorithm_content.writer);

	/* The Name: one RDN (SET) of one AttributeTypeAndValue (SEQUENCE) with the CommonName (2.5.4.3) as a UTF8String. */
	small_init(&attribute);
	der_oid(&attribute.writer, "2.5.4.3");
	der_put(&attribute.writer, 0x0cU, (const unsigned char *)spec->common_name, strlen(spec->common_name));
	small_init(&attribute_seq);
	der_wrap(&attribute_seq.writer, 0x30U, &attribute.writer);
	small_init(&rdn);
	der_wrap(&rdn.writer, 0x31U, &attribute_seq.writer);
	small_init(&name);
	der_wrap(&name.writer, 0x30U, &rdn.writer);

	/* The validity. */
	small_init(&validity_content);
	der_put(&validity_content.writer, 0x18U, not_before, 15U);
	der_put(&validity_content.writer, 0x18U, not_after, 15U);
	small_init(&validity);
	der_wrap(&validity.writer, 0x30U, &validity_content.writer);

	/* The public key: id-ecPublicKey with secp256r1, and the BIT STRING of the uncompressed point of G. */
	small_init(&key_algorithm_content);
	der_oid(&key_algorithm_content.writer, "1.2.840.10045.2.1");
	der_oid(&key_algorithm_content.writer, "1.2.840.10045.3.1.7");
	small_init(&key_algorithm);
	der_wrap(&key_algorithm.writer, 0x30U, &key_algorithm_content.writer);
	small_init(&spki_content);
	der_raw(&spki_content.writer, key_algorithm.data, key_algorithm.writer.used);
	bits[0] = 0x00;
	memcpy(bits + 1, p256_g, sizeof(p256_g));
	der_put(&spki_content.writer, 0x03U, bits, sizeof(bits));
	small_init(&spki);
	der_wrap(&spki.writer, 0x30U, &spki_content.writer);

	/* The extensions: subjectAltName (2.5.29.17) with the names, then the unknown padding extension. */
	blob_init(&extension_list, room);
	if (spec->san_host != NULL || spec->san_ip != NULL) {
		small_init(&names);
		if (spec->san_host != NULL)
			der_put(&names.writer, 0x82U, (const unsigned char *)spec->san_host, strlen(spec->san_host));
		if (spec->san_ip != NULL)
			der_put(&names.writer, 0x87U, spec->san_ip, spec->san_ip_size);
		small_init(&names_seq);
		der_wrap(&names_seq.writer, 0x30U, &names.writer);
		small_init(&san_content);
		der_oid(&san_content.writer, "2.5.29.17");
		der_put(&san_content.writer, 0x04U, names_seq.data, names_seq.writer.used);
		small_init(&san_extension);
		der_wrap(&san_extension.writer, 0x30U, &san_content.writer);
		der_raw(&extension_list.writer, san_extension.data, san_extension.writer.used);
	}

	/* The unknown extension that carries the padding, when padding is wanted. */
	if (padding != 0U) {
		zeros = calloc(padding, 1U);
		if (zeros == NULL) {
			printf("FAIL no memory for a test buffer\n");
			exit(EXIT_FAILURE);
		}

		/* Builds the extension that holds the zero octets. */
		blob_init(&pad_content, room);
		der_oid(&pad_content.writer, "1.3.6.1.4.1.99999.1");
		der_put(&pad_content.writer, 0x04U, zeros, padding);
		free(zeros);
		blob_init(&pad_extension, room);
		der_wrap(&pad_extension.writer, 0x30U, &pad_content.writer);
		der_raw(&extension_list.writer, pad_extension.data, pad_extension.writer.used);
		blob_free(&pad_content);
		blob_free(&pad_extension);
	}

	/* tbsCertificate: [0] version, serial, algorithm, issuer, validity, subject, key, [3] extensions. */
	blob_init(&tbs_content, room + 4096U);
	der_put(&tbs_content.writer, 0xa0U, version, sizeof(version));
	serial[0] = (unsigned char)(spec->serial >> 24);
	serial[1] = (unsigned char)(spec->serial >> 16);
	serial[2] = (unsigned char)(spec->serial >> 8);
	serial[3] = (unsigned char)spec->serial;
	der_unsigned(&tbs_content.writer, serial, sizeof(serial));
	der_raw(&tbs_content.writer, algorithm.data, algorithm.writer.used);
	der_raw(&tbs_content.writer, name.data, name.writer.used);
	der_raw(&tbs_content.writer, validity.data, validity.writer.used);
	der_raw(&tbs_content.writer, name.data, name.writer.used);
	der_raw(&tbs_content.writer, spki.data, spki.writer.used);
	if (extension_list.writer.used != 0U) {
		blob_init(&extension_seq, room + 64U);
		der_wrap(&extension_seq.writer, 0x30U, &extension_list.writer);
		der_put(&tbs_content.writer, 0xa3U, extension_seq.data, extension_seq.writer.used);
		blob_free(&extension_seq);
	}

	/* Wraps the content into the tbsCertificate SEQUENCE. */
	blob_init(&tbs, room + 4096U + 16U);
	der_wrap(&tbs.writer, 0x30U, &tbs_content.writer);

	/* The Certificate: tbs, the algorithm, the signature BIT STRING. */
	blob_init(&certificate_content, room + 8192U);
	der_raw(&certificate_content.writer, tbs.data, tbs.writer.used);
	der_raw(&certificate_content.writer, algorithm.data, algorithm.writer.used);
	der_put(&certificate_content.writer, 0x03U, signature, sizeof(signature));
	blob_init(&certificate, room + 8192U + 16U);
	der_wrap(&certificate.writer, 0x30U, &certificate_content.writer);

	/* A writer error is a bug of the test. */
	if (certificate.writer.error) {
		printf("FAIL certificate builder ran out of room\n");
		exit(EXIT_FAILURE);
	}

	/* Copies the finished bytes out. */
	result = malloc(certificate.writer.used);
	if (result == NULL) {
		printf("FAIL no memory for a test buffer\n");
		exit(EXIT_FAILURE);
	}

	/* Stores the copy and its size. */
	memcpy(result, certificate.data, certificate.writer.used);
	*size = certificate.writer.used;

	/* Releases the buffers of the parts. */
	blob_free(&extension_list);
	blob_free(&tbs_content);
	blob_free(&tbs);
	blob_free(&certificate_content);
	blob_free(&certificate);

	/* The finished certificate, which the caller frees. */
	return result;
}

/*
 * Builds a certificate to the spec: when exact_content is not 0 the padding
 * is adjusted (a second build) so that the outer SEQUENCE's contents are
 * exactly that many octets.
 */
static unsigned char *
build_cert(
	const struct cert_spec *spec,
	size_t *size)
{
	unsigned char *first;
	unsigned char *second;
	size_t first_size;
	size_t second_size;
	size_t header;
	size_t content;
	size_t padding;

	/* The first build with the requested padding. */
	first = build_once(spec, spec->padding, &first_size);
	if (spec->exact_content == 0UL) {
		*size = first_size;
		return first;
	}

	/* The header of the outer SEQUENCE is the tag and the length octets; the contents are the rest. */
	header = 2U;
	if ((first[1] & 0x80U) != 0U)
		header = 2U + (size_t)(first[1] & 0x7fU);
	content = first_size - header;

	/* A second build whose padding makes up the difference (the length octets stay the same in count). */
	padding = spec->padding + ((size_t)spec->exact_content - content);
	free(first);
	second = build_once(spec, padding, &second_size);
	*size = second_size;
	return second;
}


/* Builds a certificate of a serial with a host name in its subjectAltName. */
static unsigned char *
simple_cert(
	unsigned serial,
	size_t *size)
{
	struct cert_spec spec;
	char name[32];

	/* A name that differs with the serial, so that two certificates differ in many bytes. */
	memset(&spec, 0, sizeof(spec));
	sprintf(name, "host%u.example", serial);
	spec.serial = serial;
	spec.common_name = name;
	spec.san_host = name;
	return build_cert(&spec, size);
}

/* Returns a copy of data in a buffer of exactly size bytes (so that a read past the end is seen by the sanitizer). */
static unsigned char *
exact_copy(
	const unsigned char *data,
	size_t size)
{
	unsigned char *copy;
	size_t room;

	/* A buffer of exactly the size (one byte for size 0). */
	room = 1U;
	if (size > 0U)
		room = size;
	copy = malloc(room);
	if (copy == NULL) {
		printf("FAIL no memory for a test buffer\n");
		exit(EXIT_FAILURE);
	}

	/* Copies the data when there is any. */
	if (size > 0U)
		memcpy(copy, data, size);
	return copy;
}

/* Makes an X509 from DER with d2i_X509, which the test needs to succeed. */
static X509 *
make_cert(
	const unsigned char *der,
	size_t size)
{
	const unsigned char *cursor;
	unsigned char *copy;
	X509 *certificate;

	/* Reads from an exact-size copy. */
	copy = exact_copy(der, size);
	cursor = copy;
	certificate = d2i_X509(NULL, &cursor, (long)size);
	free(copy);
	CHECK(certificate != NULL);
	drain_errors();
	return certificate;
}

/* The verdicts and their X509_V_ codes. */
static void
test_verdicts(void)
{
	/* The expected codes, in the order of the verdicts of cryc-chain.h, from the X509_V_ values of OpenSSL's x509_vfy.h. */
	static const long expected[] = {
		0L,	/* OK */
		1L,	/* NO_PEER_CERTIFICATE: unspecified */
		1L,	/* MALFORMED: unspecified */
		20L,	/* UNABLE_TO_GET_ISSUER: unable to get local issuer certificate */
		18L,	/* DEPTH_ZERO_SELF_SIGNED */
		19L,	/* SELF_SIGNED_IN_CHAIN */
		22L,	/* CHAIN_TOO_LONG */
		7L,	/* SIGNATURE_FAILURE */
		76L,	/* UNSUPPORTED_ALGORITHM */
		66L,	/* KEY_TOO_SMALL: EE key too small */
		9L,	/* NOT_YET_VALID */
		10L,	/* EXPIRED */
		79L,	/* INVALID_CA */
		25L,	/* PATH_LENGTH_EXCEEDED */
		32L,	/* KEY_USAGE: no certsign */
		26L,	/* INVALID_PURPOSE */
		34L,	/* UNHANDLED_CRITICAL_EXTENSION */
		47L,	/* NAME_CONSTRAINT_VIOLATION: permitted violation */
		53L,	/* UNSUPPORTED_NAME_CONSTRAINT: unsupported name syntax */
		62L,	/* HOSTNAME_MISMATCH */
		64L,	/* IP_ADDRESS_MISMATCH */
		1L,	/* WORK_EXCEEDED: unspecified */
		28L	/* REJECTED */
	};
	unsigned index;
	long code;

	/* Every verdict of the enumeration, from CRYC_VERIFY_OK (0) to CRYC_VERIFY_REJECTED (22). */
	CHECK((unsigned)CRYC_VERIFY_REJECTED == 22U);
	CHECK(sizeof(expected) / sizeof(expected[0]) == 23U);
	for (index = 0; index < 23U; index++) {
		code = cryc_verify_code_to_x509((enum cryc_verify_code)index);
		CHECK(code == expected[index]);
	}

	/* The named ones, written out so that a reordered enumeration is noticed. */
	CHECK(cryc_verify_code_to_x509(CRYC_VERIFY_OK) == 0L);
	CHECK(cryc_verify_code_to_x509(CRYC_VERIFY_REJECTED) == 28L);
	CHECK(cryc_verify_code_to_x509(CRYC_VERIFY_HOSTNAME_MISMATCH) == 62L);
	CHECK(cryc_verify_code_to_x509(CRYC_VERIFY_IP_ADDRESS_MISMATCH) == 64L);
	CHECK(cryc_verify_code_to_x509(CRYC_VERIFY_EXPIRED) == 10L);
	CHECK(cryc_verify_code_to_x509(CRYC_VERIFY_NOT_YET_VALID) == 9L);
	CHECK(cryc_verify_code_to_x509(CRYC_VERIFY_UNABLE_TO_GET_ISSUER) == 20L);

	/* A value outside the enumeration is the unspecified error. */
	CHECK(cryc_verify_code_to_x509((enum cryc_verify_code)23) == 1L);
	CHECK(cryc_verify_code_to_x509((enum cryc_verify_code)100) == 1L);
	CHECK(cryc_verify_code_to_x509((enum cryc_verify_code)(-1)) == 1L);
	CHECK(no_error());
}

/* The texts of the verification results. */
static void
test_error_strings(void)
{
	/* Each code of the subset and its text, typed from OpenSSL's documentation of X509_V_*. */
	static const struct {
		long code;
		const char *text;
	} table[] = {
		{ 0L, "ok" },
		{ 1L, "unspecified certificate verification error" },
		{ 2L, "unable to get issuer certificate" },
		{ 6L, "unable to decode issuer public key" },
		{ 7L, "certificate signature failure" },
		{ 9L, "certificate is not yet valid" },
		{ 10L, "certificate has expired" },
		{ 17L, "out of memory" },
		{ 18L, "self-signed certificate" },
		{ 19L, "self-signed certificate in certificate chain" },
		{ 20L, "unable to get local issuer certificate" },
		{ 22L, "certificate chain too long" },
		{ 25L, "path length constraint exceeded" },
		{ 26L, "unsuitable certificate purpose" },
		{ 28L, "certificate rejected" },
		{ 32L, "key usage does not include certificate signing" },
		{ 34L, "unhandled critical extension" },
		{ 39L, "key usage does not include digital signature" },
		{ 47L, "permitted subtree violation" },
		{ 48L, "excluded subtree violation" },
		{ 53L, "unsupported or invalid name syntax" },
		{ 62L, "hostname mismatch" },
		{ 64L, "IP address mismatch" },
		{ 66L, "EE certificate key too weak" },
		{ 76L, "Cannot find certificate signature algorithm" },
		{ 79L, "invalid CA certificate" }
	};
	static const char unknown[] = "unknown certificate verification error";
	const char *text;
	const char *again;
	unsigned index;
	unsigned other;
	long code;
	int known;

	/* Each text of the table, and the same pointer on a second call (a constant). */
	for (index = 0; index < sizeof(table) / sizeof(table[0]); index++) {
		text = X509_verify_cert_error_string(table[index].code);
		CHECK(text != NULL);
		if (text != NULL)
			CHECK(strcmp(text, table[index].text) == 0);
		again = X509_verify_cert_error_string(table[index].code);
		CHECK(text == again);
	}

	/* Every code of the header's list has a text of its own: the table has them all, and no two texts are alike. */
	CHECK(sizeof(table) / sizeof(table[0]) == 26U);
	for (index = 0; index < sizeof(table) / sizeof(table[0]); index++) {
		for (other = index + 1U; other < sizeof(table) / sizeof(table[0]); other++)
			CHECK(strcmp(table[index].text, table[other].text) != 0);
	}

	/* A code that is not in the table gets the generic text: never NULL; the codes from -2000 to 2000. */
	for (code = -2000L; code <= 2000L; code++) {
		text = X509_verify_cert_error_string(code);
		CHECK(text != NULL);
		if (text == NULL)
			continue;
		known = 0;
		for (index = 0; index < sizeof(table) / sizeof(table[0]); index++) {
			if (table[index].code == code)
				known = 1;
		}

		/* A code of the table has a text of its own; any other code gets the generic text. */
		if (known)
			CHECK(strcmp(text, unknown) != 0);
		else
			CHECK(strcmp(text, unknown) == 0);
	}

	/* The extremes, and a value that is a known code in its low 32 bits only (a long of 64 bits must not be cut). */
	CHECK(strcmp(X509_verify_cert_error_string(LONG_MAX), unknown) == 0);
	CHECK(strcmp(X509_verify_cert_error_string(LONG_MIN), unknown) == 0);
	if (sizeof(long) > 4U) {
		code = (long)CRYC_U64(1U, 0U);
		CHECK(strcmp(X509_verify_cert_error_string(code), unknown) == 0);
		code = (long)CRYC_U64(1U, 28U);
		CHECK(strcmp(X509_verify_cert_error_string(code), unknown) == 0);
		code = (long)CRYC_U64(0xffffffffU, 0xffffffffU);
		CHECK(strcmp(X509_verify_cert_error_string(code), unknown) == 0);
	}

	/* The verdict of the library is described by the matching text. */
	CHECK(strcmp(X509_verify_cert_error_string(cryc_verify_code_to_x509(CRYC_VERIFY_REJECTED)), "certificate rejected") == 0);
	CHECK(strcmp(X509_verify_cert_error_string(cryc_verify_code_to_x509(CRYC_VERIFY_HOSTNAME_MISMATCH)), "hostname mismatch") == 0);
	CHECK(no_error());
}

/* A certificate through d2i_X509: the object, the pointer's advance, the copy. */
static void
test_d2i_basic(void)
{
	unsigned char *der;
	unsigned char *input;
	unsigned char *saved;
	const unsigned char *cursor;
	size_t size;
	X509 *certificate;
	struct cryc_x509 parsed;

	/* Builds one certificate and gives it to d2i_X509 in a buffer of exactly its size. */
	der = simple_cert(7U, &size);
	input = exact_copy(der, size);
	saved = exact_copy(der, size);
	cursor = input;
	certificate = d2i_X509(NULL, &cursor, (long)size);
	CHECK(certificate != NULL);
	CHECK(no_error());
	if (certificate == NULL) {
		free(der);
		free(input);
		free(saved);
		return;
	}

	/* The pointer is past the whole certificate. */
	CHECK(cursor == input + size);

	/* One reference, a private copy of the DER, the parsed form inside the copy. */
	CHECK(certificate->references == 1L);
	CHECK(certificate->der != input);
	CHECK(certificate->der_size == size);
	CHECK_BYTES(certificate->der, saved, size);
	CHECK(certificate->cert.der == certificate->der);
	CHECK(certificate->cert.der_size == size);
	CHECK(certificate->cert.version == 3U);
	CHECK(certificate->cert.key.type == CRYC_KEY_EC);
	CHECK(certificate->cert.key.curve == CRYC_CURVE_P256);
	CHECK(certificate->cert.not_before == 1577836800);
	CHECK(certificate->cert.serial_size == 1U);
	CHECK((certificate->cert.flags & CRYC_X509_HAS_SAN) != 0U);
	CHECK((certificate->cert.flags & CRYC_X509_SELF_ISSUED) != 0U);

	/* The same bytes parsed directly give the same fields. */
	CHECK(cryc_x509_parse(&parsed, saved, size) == 0);
	CHECK(parsed.not_after == certificate->cert.not_after);
	CHECK(parsed.flags == certificate->cert.flags);
	CHECK(parsed.san_size == certificate->cert.san_size);

	/* The copy does not depend on the caller's buffer: overwriting the input changes nothing. */
	memset(input, 0xa5, size);
	CHECK_BYTES(certificate->der, saved, size);
	CHECK(certificate->cert.not_after == parsed.not_after);

	/* Freeing the certificate releases the copy (the leak check of the run sees a missing free). */
	X509_free(certificate);
	free(der);
	free(input);
	free(saved);
	CHECK(no_error());
}

/* The out argument: reuse, replacement and the failure convention. */
static void
test_d2i_out(void)
{
	unsigned char *first_der;
	unsigned char *second_der;
	unsigned char *input;
	const unsigned char *cursor;
	size_t first_size;
	size_t second_size;
	X509 *held;
	X509 *kept;
	X509 *result;

	/* Two different certificates. */
	first_der = simple_cert(1U, &first_size);
	second_der = simple_cert(2U, &second_size);

	/* A NULL *out: the result is stored in *out too. */
	held = NULL;
	input = exact_copy(first_der, first_size);
	cursor = input;
	result = d2i_X509(&held, &cursor, (long)first_size);
	CHECK(result != NULL);
	CHECK(held == result);
	CHECK(cursor == input + first_size);
	CHECK(no_error());
	free(input);

	/* An existing *out is freed and replaced (the old object is gone: the leak check sees it). */
	input = exact_copy(second_der, second_size);
	cursor = input;
	result = d2i_X509(&held, &cursor, (long)second_size);
	CHECK(result != NULL);
	CHECK(held == result);
	CHECK(held->der_size == second_size);
	CHECK_BYTES(held->der, second_der, second_size);
	CHECK(held->references == 1L);
	CHECK(no_error());
	free(input);

	/* A failure frees *out and sets it to NULL; the pointer does not move. */
	input = exact_copy(first_der, first_size);
	input[0] = 0x31;
	cursor = input;
	result = d2i_X509(&held, &cursor, (long)first_size);
	CHECK(result == NULL);
	CHECK(held == NULL);
	CHECK(cursor == input);
	CHECK(one_error(CODE_WRONG_TAG));
	free(input);

	/* A failure with out NULL does not need an out. */
	input = exact_copy(first_der, first_size);
	input[0] = 0x31;
	cursor = input;
	result = d2i_X509(NULL, &cursor, (long)first_size);
	CHECK(result == NULL);
	CHECK(cursor == input);
	CHECK(one_error(CODE_WRONG_TAG));
	free(input);

	/* A failure through an out holding a certificate with another reference leaves that reference alone. */
	input = exact_copy(first_der, first_size);
	cursor = input;
	held = d2i_X509(NULL, &cursor, (long)first_size);
	CHECK(held != NULL);
	free(input);
	kept = held;
	CHECK(cryc_os_atomic_add(&kept->references, 1L) == 2L);
	input = exact_copy(second_der, 3U);
	cursor = input;
	result = d2i_X509(&held, &cursor, 3L);
	CHECK(result == NULL);
	CHECK(held == NULL);
	CHECK(kept->references == 1L);
	CHECK_BYTES(kept->der, first_der, first_size);
	CHECK(one_error(CODE_HEADER_TOO_LONG));
	X509_free(kept);
	free(input);

	/* A success replacing an object that has another reference: that reference survives. */
	input = exact_copy(first_der, first_size);
	cursor = input;
	held = d2i_X509(NULL, &cursor, (long)first_size);
	free(input);
	kept = held;
	CHECK(cryc_os_atomic_add(&kept->references, 1L) == 2L);
	input = exact_copy(second_der, second_size);
	cursor = input;
	result = d2i_X509(&held, &cursor, (long)second_size);
	CHECK(result != NULL);
	CHECK(held == result);
	CHECK(kept->references == 1L);
	CHECK_BYTES(kept->der, first_der, first_size);
	CHECK(no_error());
	X509_free(kept);
	X509_free(held);
	free(input);

	/* Frees the certificate encodings. */
	free(first_der);
	free(second_der);
}

/* The error cases of d2i_X509, one by one. */
static void
test_d2i_errors(void)
{
	unsigned char *der;
	unsigned char *input;
	unsigned char pair[2];
	const unsigned char *cursor;
	const unsigned char *none;
	size_t size;
	size_t cut;
	X509 *held;
	X509 *result;
	static const long lengths[] = { 0L, -1L, -2L, LONG_MIN };

	/* Builds the certificate that is given to d2i_X509. */
	der = simple_cert(3U, &size);

	/* A length that is not positive: the null pointer is not read; one invalid-argument error, *in unchanged. */
	for (cut = 0; cut < sizeof(lengths) / sizeof(lengths[0]); cut++) {
		input = exact_copy(der, size);
		cursor = input;
		held = NULL;
		result = d2i_X509(&held, &cursor, lengths[cut]);
		CHECK(result == NULL);
		CHECK(held == NULL);
		CHECK(cursor == input);
		CHECK(one_error(CODE_INVALID_ARGUMENT));
		cursor = input;
		result = d2i_X509(NULL, &cursor, lengths[cut]);
		CHECK(result == NULL);
		CHECK(cursor == input);
		CHECK(one_error(CODE_INVALID_ARGUMENT));
		free(input);
	}

	/* A length that is not positive, and a SEQUENCE that is not a certificate, with *out holding a certificate: it is freed and cleared (the leak check of the run sees a missing free). */
	input = exact_copy(der, size);
	cursor = input;
	held = d2i_X509(NULL, &cursor, (long)size);
	CHECK(held != NULL);
	cursor = input;
	result = d2i_X509(&held, &cursor, 0L);
	CHECK(result == NULL);
	CHECK(held == NULL);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	cursor = input;
	held = d2i_X509(NULL, &cursor, (long)size);
	CHECK(held != NULL);
	pair[0] = 0x30;
	pair[1] = 0x00;
	none = pair;
	result = d2i_X509(&held, &none, 2L);
	CHECK(result == NULL);
	CHECK(held == NULL);
	CHECK(none == pair);
	CHECK(one_error(CODE_WRONG_TAG));
	cursor = input;
	held = d2i_X509(NULL, &cursor, (long)size);
	CHECK(held != NULL);
	pair[0] = 0x31;
	none = pair;
	result = d2i_X509(&held, &none, 2L);
	CHECK(result == NULL);
	CHECK(held == NULL);
	CHECK(one_error(CODE_WRONG_TAG));
	free(input);

	/* A missing input pointer, and a pointer to NULL: a null-parameter error; *out freed and cleared. */
	held = NULL;
	result = d2i_X509(&held, NULL, 10L);
	CHECK(result == NULL);
	CHECK(one_error(CODE_NULL_PARAMETER));
	result = d2i_X509(NULL, NULL, 10L);
	CHECK(result == NULL);
	CHECK(one_error(CODE_NULL_PARAMETER));
	none = NULL;
	result = d2i_X509(&held, &none, 10L);
	CHECK(result == NULL);
	CHECK(none == NULL);
	CHECK(held == NULL);
	CHECK(one_error(CODE_NULL_PARAMETER));
	input = exact_copy(der, size);
	cursor = input;
	held = d2i_X509(NULL, &cursor, (long)size);
	CHECK(held != NULL);
	none = NULL;
	result = d2i_X509(&held, &none, 10L);
	CHECK(result == NULL);
	CHECK(held == NULL);
	CHECK(one_error(CODE_NULL_PARAMETER));
	free(input);

	/* Every length from 1 to size - 1: the SEQUENCE does not fit (or the header is cut): a header-too-long error each time, nothing read beyond the length. */
	for (cut = 1; cut < size; cut++) {
		input = exact_copy(der, cut);
		cursor = input;
		held = NULL;
		result = d2i_X509(&held, &cursor, (long)cut);
		CHECK(result == NULL);
		CHECK(held == NULL);
		CHECK(cursor == input);
		CHECK(one_error(CODE_HEADER_TOO_LONG));
		free(input);
	}

	/* The first octet is not SEQUENCE: the wrong tag, for every other value of the first octet. */
	for (cut = 0; cut < 256U; cut++) {
		input = exact_copy(der, size);
		input[0] = (unsigned char)cut;
		cursor = input;
		result = d2i_X509(NULL, &cursor, (long)size);
		if (cut == 0x30U) {
			CHECK(result != NULL);
			X509_free(result);
			CHECK(no_error());
		} else {
			CHECK(result == NULL);
			CHECK(cursor == input);
			CHECK(one_error(CODE_WRONG_TAG));
		}

		/* Releases the changed copy. */
		free(input);
	}

	/* One octet only, and a SEQUENCE header whose length octets are missing. */
	pair[0] = 0x30;
	input = exact_copy(pair, 1U);
	cursor = input;
	CHECK(d2i_X509(NULL, &cursor, 1L) == NULL);
	CHECK(one_error(CODE_HEADER_TOO_LONG));
	free(input);
	pair[1] = 0x80;
	input = exact_copy(pair, 2U);
	cursor = input;
	CHECK(d2i_X509(NULL, &cursor, 2L) == NULL);
	CHECK(one_error(CODE_HEADER_TOO_LONG));
	free(input);

	/* A well-formed SEQUENCE that is not a certificate: the parse error (an empty one, and a SEQUENCE of one INTEGER). */
	pair[0] = 0x30;
	pair[1] = 0x00;
	input = exact_copy(pair, 2U);
	cursor = input;
	CHECK(d2i_X509(NULL, &cursor, 2L) == NULL);
	CHECK(cursor == input);
	CHECK(one_error(CODE_WRONG_TAG));
	free(input);

	/* The queue holds one error per failed call even after many failed calls without draining: it keeps the newest. */
	for (cut = 0; cut < 40U; cut++) {
		input = exact_copy(der, size);
		input[0] = 0x31;
		cursor = input;
		CHECK(d2i_X509(NULL, &cursor, (long)size) == NULL);
		free(input);
	}

	/* Many failed calls without draining keep only the newest codes of the queue. */
	CHECK(drain_errors() == 16U);

	/* Releases the certificate encoding. */
	free(der);
}

/* Hostile and random input: nothing is read beyond the buffer, and every call either fails with one error or succeeds with none. */
static void
test_d2i_hostile(void)
{
	unsigned char *der;
	unsigned char *input;
	unsigned char *random_bytes;
	const unsigned char *cursor;
	size_t size;
	size_t index;
	size_t round;
	size_t length;
	unsigned long failures;
	unsigned long successes;
	unsigned change;
	X509 *result;
	static const unsigned char xors[] = { 0xff, 0x01, 0x80 };

	/* Builds the certificate whose octets are changed. */
	der = simple_cert(11U, &size);
	failures = 0;
	successes = 0;

	/* Each octet changed in three ways: a failure is one error with the pointer unmoved; a success moves the pointer by the certificate's own length. */
	for (index = 0; index < size; index++) {
		for (change = 0; change < 4U; change++) {
			input = exact_copy(der, size);
			if (change < 3U)
				input[index] = (unsigned char)(input[index] ^ xors[change]);
			else
				input[index] = 0x00;
			cursor = input;
			result = d2i_X509(NULL, &cursor, (long)size);
			if (result == NULL) {
				failures++;
				CHECK(cursor == input);
				drain_errors();
				CHECK(err_total == 1U);
				CHECK(err_codes[0] == CODE_WRONG_TAG || err_codes[0] == CODE_HEADER_TOO_LONG);
			} else {
				successes++;
				CHECK(cursor > input && cursor <= input + size);
				CHECK(result->der_size == (size_t)(cursor - input));
				CHECK(no_error());
				X509_free(result);
			}

			/* Releases the changed copy. */
			free(input);
		}
	}

	/* Both outcomes happen often enough for the mutations to mean something. */
	CHECK(failures > 100UL);
	CHECK(successes > 100UL);
	printf("d2i mutations: %lu refused, %lu accepted\n", failures, successes);

	/* Random bytes of random lengths, some with a plausible SEQUENCE header. */
	failures = 0;
	for (round = 0; round < 3000U; round++) {
		length = (size_t)(next_random() % 300U) + 1U;
		random_bytes = malloc(length);
		CHECK(random_bytes != NULL);
		if (random_bytes == NULL)
			break;
		for (index = 0; index < length; index++)
			random_bytes[index] = (unsigned char)next_random();
		if (round % 2U == 0U && length >= 4U) {
			random_bytes[0] = 0x30;
			random_bytes[1] = 0x82;
			random_bytes[2] = (unsigned char)((length - 4U) >> 8);
			random_bytes[3] = (unsigned char)(length - 4U);
		}

		/* Gives the random bytes to d2i_X509 in a buffer of exactly their size. */
		input = exact_copy(random_bytes, length);
		cursor = input;
		result = d2i_X509(NULL, &cursor, (long)length);
		if (result == NULL) {
			failures++;
			CHECK(cursor == input);
			CHECK(drain_errors() == 1U);
		} else {
			CHECK(no_error());
			X509_free(result);
		}

		/* Releases the buffers of this round. */
		free(input);
		free(random_bytes);
	}

	/* Almost every random input is refused. */
	CHECK(failures > 2900UL);

	/* Releases the certificate encoding. */
	free(der);
}

/* Large certificates, the length forms, the largest the interface allows and one octet more. */
static void
test_d2i_sizes(void)
{
	struct cert_spec spec;
	unsigned char *der;
	unsigned char *input;
	unsigned char *two;
	const unsigned char *cursor;
	size_t size;
	size_t size_two;
	size_t padding;
	static const size_t paddings[] = { 0U, 100U, 200U, 40000U, 70000U };
	X509 *result;
	X509 *second;

	/* Padding sizes at the borders of the DER length forms: 0 (no padding), 100 (short form), 200 (one octet), 70000 (three octets). */
	memset(&spec, 0, sizeof(spec));
	spec.common_name = "big.example";
	spec.san_host = "big.example";
	for (padding = 0; padding < sizeof(paddings) / sizeof(paddings[0]); padding++) {
		spec.padding = paddings[padding];
		spec.serial = 100U + (unsigned)padding;
		der = build_cert(&spec, &size);
		input = exact_copy(der, size);
		cursor = input;
		result = d2i_X509(NULL, &cursor, (long)size);
		CHECK(result != NULL);
		CHECK(cursor == input + size);
		if (result != NULL) {
			CHECK(result->der_size == size);
			CHECK_BYTES(result->der, der, size);
			X509_free(result);
		}

		/* A successful call leaves the error queue empty. */
		CHECK(no_error());
		free(input);
		free(der);
	}

	/* Two certificates one after the other, then trailing octets: each call reads one. */
	der = simple_cert(21U, &size);
	two = simple_cert(22U, &size_two);
	input = malloc(size + size_two + 7U);
	CHECK(input != NULL);
	if (input != NULL) {
		memcpy(input, der, size);
		memcpy(input + size, two, size_two);
		memset(input + size + size_two, 0xee, 7U);
		cursor = input;
		result = d2i_X509(NULL, &cursor, (long)(size + size_two + 7U));
		CHECK(result != NULL);
		CHECK(cursor == input + size);
		second = d2i_X509(NULL, &cursor, (long)(size_two + 7U));
		CHECK(second != NULL);
		CHECK(cursor == input + size + size_two);
		if (result != NULL)
			CHECK_BYTES(result->der, der, size);
		if (second != NULL)
			CHECK_BYTES(second->der, two, size_two);
		X509_free(result);
		X509_free(second);

		/* The trailing octets alone are not a certificate: refused, pointer unmoved. */
		CHECK(d2i_X509(NULL, &cursor, 7L) == NULL);
		CHECK(cursor == input + size + size_two);
		CHECK(one_error(CODE_WRONG_TAG));
		free(input);
	}

	/* Releases the certificate encodings. */
	free(der);
	free(two);

	/* A certificate whose outer length is 0xffffff, the largest DER length the reader accepts, is read. */
	memset(&spec, 0, sizeof(spec));
	spec.common_name = "max.example";
	spec.san_host = "max.example";
	spec.serial = 5U;
	spec.padding = 1000000U;
	spec.exact_content = 0x00ffffffUL;
	der = build_cert(&spec, &size);
	CHECK(size == 0x00ffffffUL + 5U);
	input = exact_copy(der, size);
	cursor = input;
	result = d2i_X509(NULL, &cursor, (long)size);
	CHECK(result != NULL);
	CHECK(cursor == input + size);
	if (result != NULL) {
		CHECK(result->der_size == size);
		CHECK(memcmp(result->der, der, size) == 0);
		X509_free(result);
	}

	/* A successful call leaves the error queue empty. */
	CHECK(no_error());
	free(input);

	/* One more octet of length (0x1000000, which needs four length octets) is refused. */
	input = exact_copy(der, size);
	input[0] = 0x30;
	input[1] = 0x84;
	input[2] = 0x01;
	input[3] = 0x00;
	input[4] = 0x00;
	input[5] = 0x00;
	cursor = input;
	CHECK(d2i_X509(NULL, &cursor, (long)size) == NULL);
	CHECK(cursor == input);
	CHECK(one_error(CODE_HEADER_TOO_LONG));
	free(input);

	/* The same through cryc_x509_from_der: the largest size passes the size check and parses; one octet more is refused by the size check. */
	result = cryc_x509_from_der(der, size);
	CHECK(result != NULL);
	CHECK(no_error());
	X509_free(result);
	input = malloc(size + 1U);
	CHECK(input != NULL);
	if (input != NULL) {
		memcpy(input, der, size);
		input[size] = 0x00;
		CHECK(cryc_x509_from_der(input, size + 1U) == NULL);
		CHECK(one_error(CODE_INVALID_ARGUMENT));
		free(input);
	}

	/* Releases the certificate encoding. */
	free(der);
}

/* cryc_x509_from_der. */
static void
test_from_der(void)
{
	unsigned char *der;
	unsigned char *input;
	size_t size;
	X509 *certificate;

	/* Builds the certificate to convert. */
	der = simple_cert(31U, &size);

	/* A certificate: one reference, a private copy. */
	input = exact_copy(der, size);
	certificate = cryc_x509_from_der(input, size);
	CHECK(certificate != NULL);
	CHECK(no_error());
	if (certificate != NULL) {
		CHECK(certificate->references == 1L);
		CHECK(certificate->der != input);
		CHECK(certificate->der_size == size);
		CHECK_BYTES(certificate->der, der, size);
		CHECK(certificate->cert.der == certificate->der);
		memset(input, 0, size);
		CHECK_BYTES(certificate->der, der, size);
		X509_free(certificate);
	}

	/* Releases the input copy. */
	free(input);

	/* No bytes, an empty encoding, and one that is too large. */
	CHECK(cryc_x509_from_der(NULL, 10U) == NULL);
	CHECK(one_error(CODE_NULL_PARAMETER));
	CHECK(cryc_x509_from_der(NULL, 0U) == NULL);
	CHECK(one_error(CODE_NULL_PARAMETER));
	CHECK(cryc_x509_from_der(der, 0U) == NULL);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	CHECK(cryc_x509_from_der(der, (size_t)0x01000005UL) == NULL);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	CHECK(cryc_x509_from_der(der, (size_t)0x7fffffffUL) == NULL);
	CHECK(one_error(CODE_INVALID_ARGUMENT));

	/* Trailing bytes are not allowed here (the encoding is the certificate), and a cut one fails: the parse error. */
	input = malloc(size + 1U);
	CHECK(input != NULL);
	if (input != NULL) {
		memcpy(input, der, size);
		input[size] = 0x00;
		CHECK(cryc_x509_from_der(input, size + 1U) == NULL);
		CHECK(one_error(CODE_WRONG_TAG));
		CHECK(cryc_x509_from_der(input, size - 1U) == NULL);
		CHECK(one_error(CODE_WRONG_TAG));
		CHECK(cryc_x509_from_der(input, 1U) == NULL);
		CHECK(one_error(CODE_WRONG_TAG));
		free(input);
	}

	/* Releases the certificate encoding. */
	free(der);
}

/* The reference counts of certificates and stores. */
static void
test_references(void)
{
	unsigned char *der;
	size_t size;
	X509 *certificate;
	X509_STORE *store;

	/* A certificate with two references survives the first X509_free and is freed by the second. */
	der = simple_cert(41U, &size);
	certificate = make_cert(der, size);
	CHECK(certificate != NULL);
	CHECK(cryc_os_atomic_add(&certificate->references, 1L) == 2L);
	X509_free(certificate);
	CHECK(certificate->references == 1L);
	CHECK(certificate->der_size == size);
	CHECK_BYTES(certificate->der, der, size);
	X509_free(certificate);

	/* NULL is ignored. */
	X509_free(NULL);
	cryc_x509_store_free(NULL);

	/* A store has one reference, an empty list of anchors; with two references the first free keeps it. */
	store = cryc_x509_store_new();
	CHECK(store != NULL);
	CHECK(no_error());
	if (store != NULL) {
		CHECK(store->references == 1L);
		CHECK(store->store.count == 0U);
		CHECK(store->store.distrusted_count == 0U);
		CHECK(cryc_os_atomic_add(&store->references, 1L) == 2L);
		cryc_x509_store_free(store);
		CHECK(store->references == 1L);
		CHECK(store->store.lock.impl != NULL);
		certificate = make_cert(der, size);
		CHECK(X509_STORE_add_cert(store, certificate) == 1);
		CHECK(store->store.count == 1U);
		X509_free(certificate);
		cryc_x509_store_free(store);
	}

	/* A store with certificates is freed with them (the leak check of the run sees a missing free). */
	free(der);
	CHECK(no_error());
}

/* X509_STORE_add_cert. */
static void
test_store(void)
{
	unsigned char *first_der;
	unsigned char *second_der;
	size_t first_size;
	size_t second_size;
	X509 *first;
	X509 *second;
	X509 *again;
	X509_STORE *store;
	const struct cryc_store_entry *entry;

	/* Builds two certificates and their parsed forms. */
	first_der = simple_cert(51U, &first_size);
	second_der = simple_cert(52U, &second_size);
	first = make_cert(first_der, first_size);
	second = make_cert(second_der, second_size);
	store = cryc_x509_store_new();
	CHECK(store != NULL);
	if (store == NULL || first == NULL || second == NULL) {
		X509_free(first);
		X509_free(second);
		cryc_x509_store_free(store);
		free(first_der);
		free(second_der);
		return;
	}

	/* A missing store or certificate: a null-parameter error and the failure value. */
	CHECK(X509_STORE_add_cert(NULL, first) == 0);
	CHECK(one_error(CODE_NULL_PARAMETER));
	CHECK(X509_STORE_add_cert(store, NULL) == 0);
	CHECK(one_error(CODE_NULL_PARAMETER));
	CHECK(X509_STORE_add_cert(NULL, NULL) == 0);
	CHECK(one_error(CODE_NULL_PARAMETER));
	CHECK(store->store.count == 0U);

	/* A certificate is added as its own copy: the entry's DER equals it but is not the caller's buffer. */
	CHECK(X509_STORE_add_cert(store, first) == 1);
	CHECK(no_error());
	CHECK(store->store.count == 1U);
	entry = store->store.entries[0];
	CHECK(entry->der != first->der);
	CHECK(entry->der_size == first_size);
	CHECK_BYTES(entry->der, first_der, first_size);
	CHECK(first->references == 1L);

	/* The same certificate again (the same object, and a second object of the same bytes) succeeds and adds nothing. */
	CHECK(X509_STORE_add_cert(store, first) == 1);
	CHECK(no_error());
	CHECK(store->store.count == 1U);
	again = make_cert(first_der, first_size);
	CHECK(X509_STORE_add_cert(store, again) == 1);
	CHECK(no_error());
	CHECK(store->store.count == 1U);
	X509_free(again);

	/* A different certificate is a second entry, in the order added. */
	CHECK(X509_STORE_add_cert(store, second) == 1);
	CHECK(store->store.count == 2U);
	CHECK_BYTES(store->store.entries[1]->der, second_der, second_size);
	CHECK_BYTES(store->store.entries[0]->der, first_der, first_size);

	/* A certificate object whose bytes no longer parse (it cannot come from the calls here; the store refuses it as an internal fault) is refused with that error and the store is unchanged. */
	again = make_cert(first_der, first_size);
	CHECK(again != NULL);
	again->der[0] = 0x00;
	CHECK(X509_STORE_add_cert(store, again) == 0);
	CHECK(one_error(0x058c0103UL));
	CHECK(store->store.count == 2U);
	X509_free(again);

	/* The store keeps its copy after the caller frees the certificate. */
	X509_free(first);
	CHECK_BYTES(store->store.entries[0]->der, first_der, first_size);
	CHECK(store->store.entries[0]->cert.der == store->store.entries[0]->der);
	X509_free(second);
	CHECK(store->store.count == 2U);

	/* The store's entries are parsed (a trust anchor's name and key are read from them). */
	CHECK(store->store.entries[1]->cert.key.type == CRYC_KEY_EC);
	cryc_x509_store_free(store);
	free(first_der);
	free(second_der);
	CHECK(no_error());
}

/* The store's size limit: the last certificate that fits, and one more. */
static void
test_store_limit(void)
{
	unsigned char *der;
	size_t size;
	unsigned index;
	X509 *certificate;
	X509 *extra;
	X509_STORE *store;
	int added;
	unsigned long failures;

	/* Makes the store that the certificates are added to. */
	store = cryc_x509_store_new();
	CHECK(store != NULL);
	if (store == NULL)
		return;

	/* Adds STORE_MAX different certificates. */
	failures = 0;
	for (index = 0; index < STORE_MAX; index++) {
		der = simple_cert(1000U + index, &size);
		certificate = make_cert(der, size);
		added = X509_STORE_add_cert(store, certificate);
		if (added != 1)
			failures++;
		X509_free(certificate);
		free(der);
	}

	/* Every addition up to the limit succeeded. */
	CHECK(failures == 0UL);
	CHECK(store->store.count == STORE_MAX);
	CHECK(no_error());

	/* One more is refused with the unsupported error and the store is unchanged. */
	der = simple_cert(900000U, &size);
	extra = make_cert(der, size);
	CHECK(X509_STORE_add_cert(store, extra) == 0);
	CHECK(one_error(CODE_UNSUPPORTED));
	CHECK(store->store.count == STORE_MAX);
	X509_free(extra);
	free(der);

	/* A certificate already in the store is still a success at the limit. */
	der = simple_cert(1000U, &size);
	certificate = make_cert(der, size);
	CHECK(X509_STORE_add_cert(store, certificate) == 1);
	CHECK(no_error());
	CHECK(store->store.count == STORE_MAX);
	X509_free(certificate);
	free(der);
	cryc_x509_store_free(store);
}

/* Checks the host name that X509_VERIFY_PARAM_set1_host recorded: the stored form and the flags. */
static int
host_is(
	const X509_VERIFY_PARAM *param,
	const char *expected)
{
	size_t host_length;
	size_t expected_length;
	int comparison;

	/* Set (the flag is exactly 1), with exactly the expected text, ended with a NUL, and no address. */
	if (param->params.has_host != 1)
		return 0;
	host_length = strlen(param->params.host);
	expected_length = strlen(expected);
	if (host_length != expected_length)
		return 0;
	comparison = strcmp(param->params.host, expected);
	if (comparison != 0)
		return 0;
	if (param->params.ip_size != 0U)
		return 0;

	/* Succeeded: the recorded host is the expected one. */
	return 1;
}

/* X509_VERIFY_PARAM_set1_host. */
static void
test_host(void)
{
	X509_VERIFY_PARAM param;
	char text[400];
	char label[80];
	unsigned char *exact;
	size_t index;
	static const char *const invalid[] = {
		".", "..", "a..b", ".example.com", "a b.example", "a@b.example", "a/b.example", "a:b.example",
		"*.example.com", "exa mple.com", "example.com..", "a\tb", "\xc3\xa9.example", "\x80", "\xff.com",
		"[::1]", "a,b", "a#b"
	};

	/* Plain names, case folded, and a trailing dot removed. */
	memset(&param, 0, sizeof(param));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "example.com", 0U) == 1);
	CHECK(host_is(&param, "example.com"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "WWW.Example.COM", 0U) == 1);
	CHECK(host_is(&param, "www.example.com"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "example.com.", 0U) == 1);
	CHECK(host_is(&param, "example.com"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "a", 0U) == 1);
	CHECK(host_is(&param, "a"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "xn--bcher-kva.example", 0U) == 1);
	CHECK(host_is(&param, "xn--bcher-kva.example"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "under_score.example", 0U) == 1);
	CHECK(host_is(&param, "under_score.example"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "1-2.3", 0U) == 1);
	CHECK(host_is(&param, "1-2.3"));
	CHECK(no_error());

	/* A length: only that many bytes are the name; 0 means a C string. */
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "example.comXYZ", 11U) == 1);
	CHECK(host_is(&param, "example.com"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "example.com.XYZ", 12U) == 1);
	CHECK(host_is(&param, "example.com"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "a.bXYZ", 3U) == 1);
	CHECK(host_is(&param, "a.b"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "a.b", 3U) == 1);
	CHECK(host_is(&param, "a.b"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "example.com", 4U) == 1);
	CHECK(host_is(&param, "exam"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "x.y", 0U) == 1);
	CHECK(host_is(&param, "x.y"));
	CHECK(no_error());

	/* A length that cuts into an invalid character: "a b" with length 1 is "a". */
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "a b", 1U) == 1);
	CHECK(host_is(&param, "a"));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "a b", 3U) == 0);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	CHECK(host_is(&param, "a"));

	/* Names that are not host names fail with one error and change nothing. */
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "kept.example", 0U) == 1);
	for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
		CHECK(X509_VERIFY_PARAM_set1_host(&param, invalid[index], 0U) == 0);
		CHECK(one_error(CODE_INVALID_ARGUMENT));
		CHECK(host_is(&param, "kept.example"));
	}

	/* A NUL inside the given length: invalid (the check would otherwise stop at it and accept the front part). */
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "a\0b.com", 7U) == 0);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "evil.com\0.good.com", 18U) == 0);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "\0", 1U) == 0);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	CHECK(host_is(&param, "kept.example"));

	/* Label and name lengths: 63 and 64 characters in a label; 253 and 254 in a name; 254 with a trailing dot; 255. */
	memset(label, 'a', 63U);
	label[63] = '\0';
	sprintf(text, "%s.example", label);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, text, 0U) == 1);
	CHECK(host_is(&param, text));
	memset(label, 'a', 64U);
	label[64] = '\0';
	sprintf(text, "%s.example", label);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, text, 0U) == 0);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	memset(text, 0, sizeof(text));
	for (index = 0; index < HOST_MAX; index++) {
		if (index % 64U == 63U)
			text[index] = '.';
		else
			text[index] = 'b';
	}

	/* The last octet is a letter, so the name of the largest length ends in a label. */
	text[HOST_MAX - 1U] = 'b';
	CHECK(strlen(text) == HOST_MAX);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, text, 0U) == 1);
	CHECK(host_is(&param, text));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, text, HOST_MAX) == 1);
	CHECK(host_is(&param, text));
	text[HOST_MAX] = '.';
	text[HOST_MAX + 1U] = '\0';
	CHECK(X509_VERIFY_PARAM_set1_host(&param, text, 0U) == 1);
	CHECK(host_is(&param, text) == 0);
	text[HOST_MAX] = '\0';
	CHECK(host_is(&param, text));
	text[HOST_MAX] = 'c';
	text[HOST_MAX + 1U] = '\0';
	CHECK(X509_VERIFY_PARAM_set1_host(&param, text, 0U) == 0);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, text, HOST_MAX + 1U) == 0);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	text[HOST_MAX] = '.';
	text[HOST_MAX + 1U] = '.';
	text[HOST_MAX + 2U] = '\0';
	CHECK(X509_VERIFY_PARAM_set1_host(&param, text, 0U) == 0);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, text, HOST_MAX + 2U) == 0);
	CHECK(one_error(CODE_INVALID_ARGUMENT));

	/* A name of the length 255 with no NUL, in a buffer of exactly that size: refused without reading past it. */
	exact = malloc(255U);
	CHECK(exact != NULL);
	if (exact != NULL) {
		memset(exact, 'a', 255U);
		CHECK(X509_VERIFY_PARAM_set1_host(&param, (const char *)exact, 0U) == 0);
		CHECK(one_error(CODE_INVALID_ARGUMENT));
		CHECK(X509_VERIFY_PARAM_set1_host(&param, (const char *)exact, 255U) == 0);
		CHECK(one_error(CODE_INVALID_ARGUMENT));
		free(exact);
	}

	/* A name given with a length, in a buffer that has no NUL after it: only the length is read. */
	exact = malloc(11U);
	CHECK(exact != NULL);
	if (exact != NULL) {
		memcpy(exact, "example.com", 11U);
		CHECK(X509_VERIFY_PARAM_set1_host(&param, (const char *)exact, 11U) == 1);
		CHECK(host_is(&param, "example.com"));
		free(exact);
	}

	/* The empty name and NULL clear the host (the C string "" and a zero length with an empty string). */
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "x.example", 0U) == 1);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "", 0U) == 1);
	CHECK(param.params.has_host == 0);
	CHECK(param.params.host[0] == '\0');
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "x.example", 0U) == 1);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, NULL, 0U) == 1);
	CHECK(param.params.has_host == 0);
	CHECK(param.params.host[0] == '\0');
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "x.example", 0U) == 1);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, NULL, 5U) == 1);
	CHECK(param.params.has_host == 0);
	CHECK(no_error());

	/* A missing parameter object. */
	CHECK(X509_VERIFY_PARAM_set1_host(NULL, "example.com", 0U) == 0);
	CHECK(one_error(CODE_NULL_PARAMETER));
	CHECK(X509_VERIFY_PARAM_set1_host(NULL, NULL, 0U) == 0);
	CHECK(one_error(CODE_NULL_PARAMETER));
}

/* Checks an address text: it must be accepted, recorded as the given octets, and clear a host name set before. */
static void
check_ip(
	const char *text,
	const char *hex)
{
	X509_VERIFY_PARAM param;
	unsigned char expected[16];
	size_t count;

	/* The parameter object holds a host name that the address must replace. */
	memset(&param, 0, sizeof(param));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "old.example", 0U) == 1);
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, text) == 1);
	count = hex_decode(hex, expected, sizeof(expected));
	CHECK(count == 4U || count == 16U);
	CHECK(param.params.ip_size == count);
	CHECK_BYTES(param.params.ip, expected, count);
	CHECK(param.params.has_host == 0);
	CHECK(param.params.host[0] == '\0');
	if (count == 4U) {
		CHECK(param.params.ip[4] == 0U);
		CHECK(param.params.ip[15] == 0U);
	}

	/* The call leaves no error behind. */
	CHECK(no_error());
}

/* IP addresses that must be accepted, with the octets worked out from RFC 4291 2.2. */
static void
test_ip_valid(void)
{
	/* IPv4: four decimal fields. */
	check_ip("0.0.0.0", "00000000");
	check_ip("127.0.0.1", "7f000001");
	check_ip("255.255.255.255", "ffffffff");
	check_ip("192.168.1.10", "c0a8010a");
	check_ip("1.2.3.4", "01020304");
	check_ip("10.20.30.40", "0a141e28");
	check_ip("100.200.250.99", "64c8fa63");
	check_ip("192.0.2.7", "c0000207");

	/* IPv6: the full form, in either case, with leading zeros. */
	check_ip("0:0:0:0:0:0:0:0", "00000000000000000000000000000000");
	check_ip("1:2:3:4:5:6:7:8", "00010002000300040005000600070008");
	check_ip("2001:0db8:0000:0000:0000:0000:0000:0001", "20010db8000000000000000000000001");
	check_ip("FE80:0:0:0:0:0:0:1", "fe800000000000000000000000000001");
	check_ip("abcd:EF01:2345:6789:abcd:ef01:2345:6789", "abcdef0123456789abcdef0123456789");
	check_ip("0001:0002:0003:0004:0005:0006:0007:0008", "00010002000300040005000600070008");

	/* The "::" form in each place. */
	check_ip("::", "00000000000000000000000000000000");
	check_ip("::1", "00000000000000000000000000000001");
	check_ip("1::", "00010000000000000000000000000000");
	check_ip("2001:db8::1", "20010db8000000000000000000000001");
	check_ip("2001:DB8::", "20010db8000000000000000000000000");
	check_ip("fe80::1:2", "fe800000000000000000000000010002");
	check_ip("1::8", "00010000000000000000000000000008");
	check_ip("1:2:3::", "00010002000300000000000000000000");
	check_ip("::3:4", "00000000000000000000000000030004");
	check_ip("1:2:3:4:5:6:7::", "00010002000300040005000600070000");
	check_ip("::2:3:4:5:6:7:8", "00000002000300040005000600070008");
	check_ip("1:2::7:8", "00010002000000000000000000070008");
	check_ip("1:2:3::6:7:8", "00010002000300000000000600070008");
	check_ip("::0001", "00000000000000000000000000000001");

	/* The forms with an IPv4 address in the last 32 bits. */
	check_ip("::ffff:1.2.3.4", "00000000000000000000ffff01020304");
	check_ip("::FFFF:192.0.2.128", "00000000000000000000ffffc0000280");
	check_ip("::1.2.3.4", "00000000000000000000000001020304");
	check_ip("1:2:3:4:5:6:1.2.3.4", "00010002000300040005000601020304");
	check_ip("64:ff9b::192.0.2.33", "0064ff9b0000000000000000c0000221");
	check_ip("1:2:3:4:5::1.2.3.4", "00010002000300040005000001020304");
	check_ip("::0.0.0.0", "00000000000000000000000000000000");
	check_ip("1::255.255.255.255", "000100000000000000000000ffffffff");
}

/* IP address texts that must be refused. */
static void
test_ip_invalid(void)
{
	static const char *const invalid[] = {
		"", " ", "1", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.256.1.1", "1.1.256.1", "1.1.1.256", "300.0.0.1",
		"01.2.3.4", "1.02.3.4", "1.2.03.4", "1.2.3.04", "00.0.0.0", "1.2.3.0004", "1..2.3", ".1.2.3", "1.2.3.", "..1.2",
		"1.2.3.4 ", " 1.2.3.4", "a.b.c.d", "1.2.3.-4", "1.2.3.+4", "1.2.3.4a", "1234.1.1.1", "0x7f.0.0.1", "1.2.3.4/24",
		"1.2.3.4:80", "example.com", "-1.2.3.4",
		":", ":::", "::::", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9", "1::2::3", "::1::", ":1::2", "1::2:", ":1:2:3:4:5:6:7:8",
		"1:2:3:4:5:6:7:8:", "12345::", "g::1", "::g", "1:2:3:4:5:6:7:1.2.3.4", "::1.2.3", "::1.2.3.4.5", "1.2.3.4::",
		"::1.2.3.4:5", "[::1]", "::1%eth0", "::1/128", "1:2:3:4:5:6:7:8 ", " ::1", "1:2:3:4:5:6::7:8", "1:2:3:4:5:6:7::8",
		"1:2:3:4:5:6:7:8::", "::1:2:3:4:5:6:7:8", "::00001", "::01.2.3.4", "::1.2.3.04", "::256.1.1.1", "1:2:3:4:5:1.2.3.4", "1:2:3:4:5:6:7:1.2.3.4", ":1.2.3.4", "::.1.2.3", "1:::2", "1:2:::", "::-1", "::0x1",
		"1:2:3:4:5:6:7:8%1", "fe80::1%", "1.2.3.4::1", "::1.2.3.4.", "::ffff:1.2.3.4:", "::ffff::1.2.3.4",
		"1:2:3:4:5:6:1.2.3.4:", "1:2:3:4:5:6:1.2.3.4.5", "1:2:3:4:5:6:1.2.3", "1:2:3:4:5:6:1.2.3.256",
		"::ffff:1.2.3.4.5.6", "::ffff:a.b.c.d", "z", "1,2,3,4", "1;2", "::1\n", "1.2.3.4\n", "\t1.2.3.4",
		"G::1", "::G", "::fG", "Z::", "::1:G", "::g1", "G.2.3.4", "1:2:3:4:5:6:1.2.3.G"
	};
	X509_VERIFY_PARAM param;
	size_t index;

	/* Each refused with one error, the parameter object unchanged (an address set before stays). */
	memset(&param, 0, sizeof(param));
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "9.8.7.6") == 1);
	for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
		CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, invalid[index]) == 0);
		CHECK(one_error(CODE_INVALID_ARGUMENT));
		CHECK(param.params.ip_size == 4U);
		CHECK(param.params.ip[0] == 9U && param.params.ip[1] == 8U && param.params.ip[2] == 7U && param.params.ip[3] == 6U);
	}

	/* A host name set before stays after a refused address. */
	memset(&param, 0, sizeof(param));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "stay.example", 0U) == 1);
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "1.2.3") == 0);
	CHECK(one_error(CODE_INVALID_ARGUMENT));
	CHECK(host_is(&param, "stay.example"));

	/* Missing arguments. */
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(NULL, "1.2.3.4") == 0);
	CHECK(one_error(CODE_NULL_PARAMETER));
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, NULL) == 0);
	CHECK(one_error(CODE_NULL_PARAMETER));
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(NULL, NULL) == 0);
	CHECK(one_error(CODE_NULL_PARAMETER));
	CHECK(host_is(&param, "stay.example"));
}

/* Reference reader of a decimal IPv4 address, written by splitting at the dots first (the library scans character by character). */
static int
reference_v4(
	const char *begin,
	size_t length,
	unsigned char *out)
{
	size_t start;
	size_t position;
	size_t field_length;
	unsigned field;
	unsigned value;
	size_t digit;

	/* Exactly four fields, each of one to three digits, no leading zero unless it is the single digit 0, at most 255. */
	start = 0;
	field = 0;
	for (position = 0; position <= length; position++) {
		if (position < length && begin[position] != '.')
			continue;
		field_length = position - start;
		if (field_length < 1U || field_length > 3U || field >= 4U)
			return 0;
		if (field_length > 1U && begin[start] == '0')
			return 0;
		value = 0;
		for (digit = 0; digit < field_length; digit++) {
			if (begin[start + digit] < '0' || begin[start + digit] > '9')
				return 0;
			value = value * 10U + (unsigned)(begin[start + digit] - '0');
		}

		/* A field above 255 does not fit in an octet. */
		if (value > 255U)
			return 0;
		out[field] = (unsigned char)value;
		field++;
		start = position + 1U;
	}

	/* Exactly four fields make an address. */
	if (field != 4U)
		return 0;

	/* Succeeded: the text is an address and its octets are in out. */
	return 1;
}

/* Reference reader of an IPv6 address, written by splitting the text at the "::" first. */
static int
reference_v6(
	const char *text,
	unsigned char *out)
{
	const char *double_colon;
	const char *second;
	const char *triple;
	const void *dotted;
	const char *parts[2];
	size_t lengths[2];
	size_t part_count;
	unsigned groups[2][8];
	unsigned counts[2];
	size_t which;
	size_t position;
	size_t start;
	size_t token_length;
	size_t index;
	size_t digit;
	int character;
	unsigned value;
	unsigned char v4[4];
	int last_part;
	int v4_valid;
	unsigned total;
	size_t length;

	/* At most one "::", and no ":::". */
	length = strlen(text);
	triple = strstr(text, ":::");
	if (triple != NULL)
		return 0;
	double_colon = strstr(text, "::");
	if (double_colon != NULL) {
		second = strstr(double_colon + 2, "::");
		if (second != NULL)
			return 0;
	}

	/* The part before and the part after the "::" (or the whole text). */
	if (double_colon == NULL) {
		parts[0] = text;
		lengths[0] = length;
		part_count = 1;
	} else {
		parts[0] = text;
		lengths[0] = (size_t)(double_colon - text);
		parts[1] = double_colon + 2;
		lengths[1] = length - lengths[0] - 2U;
		part_count = 2;
	}

	/* Each part is colon-separated tokens, none empty (a part may be empty only beside the "::"); the last token of the last part may be an IPv4 address. */
	counts[0] = 0;
	counts[1] = 0;
	for (which = 0; which < part_count; which++) {
		last_part = (which == part_count - 1U);
		if (lengths[which] == 0U) {
			if (double_colon == NULL)
				return 0;
			continue;
		}

		/* Splits the part at its colons. */
		start = 0;
		for (position = 0; position <= lengths[which]; position++) {
			if (position < lengths[which] && parts[which][position] != ':')
				continue;
			token_length = position - start;
			if (token_length == 0U)
				return 0;
			dotted = memchr(parts[which] + start, '.', token_length);
			if (dotted != NULL) {
				if (!last_part || position != lengths[which] || counts[which] > 6U)
					return 0;
				v4_valid = reference_v4(parts[which] + start, token_length, v4);
				if (!v4_valid)
					return 0;
				groups[which][counts[which]] = (unsigned)v4[0] * 256U + v4[1];
				counts[which]++;
				groups[which][counts[which]] = (unsigned)v4[2] * 256U + v4[3];
				counts[which]++;
			} else {
				if (token_length > 4U || counts[which] >= 8U)
					return 0;
				value = 0;
				for (digit = 0; digit < token_length; digit++) {
					character = (unsigned char)parts[which][start + digit];
					if (character >= '0' && character <= '9')
						value = value * 16U + (unsigned)(character - '0');
					else if (character >= 'a' && character <= 'f')
						value = value * 16U + (unsigned)(character - 'a' + 10);
					else if (character >= 'A' && character <= 'F')
						value = value * 16U + (unsigned)(character - 'A' + 10);
					else
						return 0;
				}

				/* Stores the group that was read. */
				groups[which][counts[which]] = value;
				counts[which]++;
			}

			/* The next token starts after the colon. */
			start = position + 1U;
		}
	}

	/* The total of groups: eight without "::", at most seven with it. */
	total = counts[0] + counts[1];
	if (double_colon == NULL) {
		if (total != 8U)
			return 0;
	} else if (total > 7U) {
		return 0;
	}

	/* The octets: the first part, zeros, the second part. */
	memset(out, 0, 16U);
	for (index = 0; index < counts[0]; index++) {
		out[index * 2U] = (unsigned char)(groups[0][index] >> 8);
		out[index * 2U + 1U] = (unsigned char)groups[0][index];
	}

	/* The groups after the zeros go to the end of the address. */
	for (index = 0; index < counts[1]; index++) {
		out[(8U - counts[1] + index) * 2U] = (unsigned char)(groups[1][index] >> 8);
		out[(8U - counts[1] + index) * 2U + 1U] = (unsigned char)groups[1][index];
	}

	/* Succeeded: the text is an address and its octets are in out. */
	return 1;
}

/* Random and mutated texts: the library and the reference reader must agree on accepting, and on the octets. */
static void
test_ip_differential(void)
{
	static const char alphabet[] = "0123456789abcdefABCDEF:.:.:1.9fF0 %[]/g-G";
	static const char *const seeds[] = {
		"::", "::1", "2001:db8::1", "1:2:3:4:5:6:7:8", "::ffff:1.2.3.4", "1:2:3:4:5:6:1.2.3.4", "64:ff9b::192.0.2.33",
		"192.168.1.10", "255.255.255.255", "1::", "fe80::1:2", "1:2:3:4:5:6:7::", "::2:3:4:5:6:7:8", "0.0.0.0"
	};
	X509_VERIFY_PARAM param;
	char text[64];
	unsigned char expected[16];
	size_t length;
	size_t index;
	size_t round;
	size_t seed;
	size_t position;
	size_t changes;
	uint32_t choice;
	unsigned expected_size;
	int reference;
	int actual;
	int is_v6;
	unsigned long accepted;
	unsigned long refused;
	unsigned long disagreements;

	/* Starts the counters of the comparison. */
	accepted = 0;
	refused = 0;
	disagreements = 0;
	for (round = 0; round < 400000U; round++) {
		/* Half of the texts are random over an alphabet with the address characters; half are a valid address with a few characters changed. */
		if (round % 2U == 0U) {
			length = (size_t)(next_random() % 30U);
			for (index = 0; index < length; index++)
				text[index] = alphabet[next_random() % (sizeof(alphabet) - 1U)];
			text[length] = '\0';
		} else {
			seed = next_random() % (sizeof(seeds) / sizeof(seeds[0]));
			strcpy(text, seeds[seed]);
			length = strlen(text);
			changes = 0U;
			if (round % 10U != 1U)
				changes = 1U + next_random() % 3U;
			for (index = 0; index < changes; index++) {
				/* Picks where the text changes and whether a character is deleted or inserted. */
				position = next_random() % (length + 1U);
				choice = next_random() % 3U;

				/* A third of the changes delete the character at the position; the others insert one while the text is short. */
				if (choice == 0U &&
				    length > 0U &&
				    position < length) {
					memmove(text + position, text + position + 1U, length - position);
					length--;
				} else if (length < 60U) {
					memmove(text + position + 1U, text + position, length - position + 1U);
					text[position] = alphabet[next_random() % (sizeof(alphabet) - 1U)];
					length++;
				}
			}
		}

		/* The reference reader: IPv6 when a colon is in the text, else IPv4. */
		is_v6 = strchr(text, ':') != NULL;
		memset(expected, 0, sizeof(expected));
		if (is_v6)
			reference = reference_v6(text, expected);
		else
			reference = reference_v4(text, strlen(text), expected);

		/* The library. */
		memset(&param, 0, sizeof(param));
		actual = X509_VERIFY_PARAM_set1_ip_asc(&param, text);
		drain_errors();
		if (actual != reference) {
			disagreements++;
			printf("ip reading differs for \"%s\": library %d, reference %d\n", text, actual, reference);
			continue;
		}

		/* An accepted text has the octets the reference reader found; a refused text records no address. */
		if (actual) {
			accepted++;
			expected_size = 4U;
			if (is_v6)
				expected_size = 16U;
			CHECK(param.params.ip_size == expected_size);
			CHECK_BYTES(param.params.ip, expected, param.params.ip_size);
		} else {
			refused++;
			CHECK(param.params.ip_size == 0U);
		}
	}

	/* The library and the reference reader never disagree, and both outcomes are common. */
	CHECK(disagreements == 0UL);
	CHECK(accepted > 5000UL);
	CHECK(refused > 100000UL);
	printf("ip differential: %lu accepted, %lu refused, %lu differences\n", accepted, refused, disagreements);
}

/* How a host name and an address replace each other, and how clearing works. */
static void
test_param_interplay(void)
{
	X509_VERIFY_PARAM param;

	/* Host then address: the address replaces the host. */
	memset(&param, 0, sizeof(param));
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "a.example", 0U) == 1);
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "1.2.3.4") == 1);
	CHECK(param.params.has_host == 0);
	CHECK(param.params.ip_size == 4U);

	/* Address then host: the host replaces the address (the octets are cleared, not only the size). */
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "2001:db8::1") == 1);
	CHECK(param.params.ip_size == 16U);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "b.example", 0U) == 1);
	CHECK(host_is(&param, "b.example"));
	CHECK(param.params.ip_size == 0U);
	CHECK(param.params.ip[0] == 0U && param.params.ip[15] == 0U);

	/* A shorter address after a longer one leaves no stale octets. */
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "2001:db8::1") == 1);
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "9.9.9.9") == 1);
	CHECK(param.params.ip_size == 4U);
	CHECK(param.params.ip[4] == 0U && param.params.ip[5] == 0U && param.params.ip[15] == 0U);

	/* A shorter host after a longer one leaves no stale characters. */
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "longname.example.org", 0U) == 1);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "x.y", 0U) == 1);
	CHECK(strlen(param.params.host) == 3U);
	CHECK(param.params.host[3] == '\0' && param.params.host[10] == '\0' && param.params.host[19] == '\0');

	/* Clearing the host keeps an address that was set after it... and an address then the NULL host: the address stays. */
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "5.6.7.8") == 1);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, NULL, 0U) == 1);
	CHECK(param.params.ip_size == 4U);
	CHECK(param.params.ip[0] == 5U);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "", 0U) == 1);
	CHECK(param.params.ip_size == 4U);

	/* The other fields of the parameters are not touched: time, depth. */
	memset(&param, 0, sizeof(param));
	param.params.has_time = 1;
	param.params.time = 12345;
	param.params.max_depth = 7U;
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "t.example", 0U) == 1);
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "1.1.1.1") == 1);
	CHECK(param.params.has_time == 1);
	CHECK(param.params.time == 12345);
	CHECK(param.params.max_depth == 7U);
	CHECK(no_error());
}

/* Runs a validation of a peer certificate in a store with the parameters, and returns the X509_V_ code. */
static long
validate(
	X509_STORE *store,
	X509 *peer,
	X509_VERIFY_PARAM *param)
{
	struct cryc_verify_result result;
	int error;

	/* The chain validation, then the mapping the interface applies. */
	error = cryc_chain_verify(&store->store, &peer->cert, 1U, &param->params, &result);
	CHECK(error == 0);
	return cryc_verify_code_to_x509(result.code);
}

/* The public calls working together: a trusted self-signed server, the names, and the verdicts. */
static void
test_validation(void)
{
	struct cert_spec spec;
	X509_STORE *store;
	X509_STORE *empty;
	X509 *server;
	X509 *copy;
	X509 *other;
	X509_VERIFY_PARAM param;
	unsigned char *der;
	unsigned char *other_der;
	size_t size;
	size_t other_size;
	long code;

	/* A server certificate for host.example, 192.0.2.7 and 2001:db8::1. */
	memset(&spec, 0, sizeof(spec));
	spec.serial = 77U;
	spec.common_name = "host.example";
	spec.san_host = "host.example";
	spec.san_ip = doc_ipv4;
	spec.san_ip_size = 4U;
	der = build_cert(&spec, &size);
	server = make_cert(der, size);
	spec.serial = 78U;
	spec.common_name = "ipv6.example";
	spec.san_host = NULL;
	spec.san_ip = doc_ipv6;
	spec.san_ip_size = 16U;
	other_der = build_cert(&spec, &other_size);
	other = make_cert(other_der, other_size);
	store = cryc_x509_store_new();
	empty = cryc_x509_store_new();
	CHECK(server != NULL && other != NULL && store != NULL && empty != NULL);
	if (server == NULL || other == NULL || store == NULL || empty == NULL)
		return;
	memset(&param, 0, sizeof(param));

	/* Not trusted: a self-signed server with no anchor. */
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "host.example", 0U) == 1);
	code = validate(empty, server, &param);
	CHECK(code == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT);
	CHECK(code == 18L);
	CHECK(strcmp(X509_verify_cert_error_string(code), "self-signed certificate") == 0);

	/* Added to the store as an anchor, and the caller's certificate freed: the store's copy is used. */
	copy = make_cert(der, size);
	CHECK(X509_STORE_add_cert(store, copy) == 1);
	X509_free(copy);
	code = validate(store, server, &param);
	CHECK(code == X509_V_OK);
	CHECK(strcmp(X509_verify_cert_error_string(code), "ok") == 0);

	/* The host name must match the dNSName: another name, a case change, a trailing dot, a parent. */
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "other.example", 0U) == 1);
	CHECK(validate(store, server, &param) == X509_V_ERR_HOSTNAME_MISMATCH);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "HOST.Example.", 0U) == 1);
	CHECK(validate(store, server, &param) == X509_V_OK);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "host.example.org", 0U) == 1);
	CHECK(validate(store, server, &param) == X509_V_ERR_HOSTNAME_MISMATCH);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "host.example", 4U) == 1);
	CHECK(validate(store, server, &param) == X509_V_ERR_HOSTNAME_MISMATCH);
	CHECK(strcmp(X509_verify_cert_error_string(62L), "hostname mismatch") == 0);

	/* No name set: the chain alone decides. */
	CHECK(X509_VERIFY_PARAM_set1_host(&param, NULL, 0U) == 1);
	CHECK(validate(store, server, &param) == X509_V_OK);

	/* The address must match the iPAddress: IPv4 in several writings, another address, the IPv6 of the other certificate. */
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "192.0.2.7") == 1);
	CHECK(validate(store, server, &param) == X509_V_OK);
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "192.0.2.8") == 1);
	CHECK(validate(store, server, &param) == X509_V_ERR_IP_ADDRESS_MISMATCH);
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "2001:db8::1") == 1);
	CHECK(validate(store, server, &param) == X509_V_ERR_IP_ADDRESS_MISMATCH);
	CHECK(strcmp(X509_verify_cert_error_string(64L), "IP address mismatch") == 0);

	/* The IPv6 certificate (not yet in the store, then added): written in different ways, the same octets. */
	CHECK(validate(store, other, &param) == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT);
	CHECK(X509_STORE_add_cert(store, other) == 1);
	CHECK(validate(store, other, &param) == X509_V_OK);
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "2001:0DB8:0:0:0:0:0:1") == 1);
	CHECK(validate(store, other, &param) == X509_V_OK);
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "2001:db8:0000::0001") == 1);
	CHECK(validate(store, other, &param) == X509_V_OK);
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "2001:db8::2") == 1);
	CHECK(validate(store, other, &param) == X509_V_ERR_IP_ADDRESS_MISMATCH);
	CHECK(X509_VERIFY_PARAM_set1_ip_asc(&param, "192.0.2.7") == 1);
	CHECK(validate(store, other, &param) == X509_V_ERR_IP_ADDRESS_MISMATCH);

	/* A refused name leaves the previous setting in force: the validation still uses it. */
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "host.example", 0U) == 1);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "bad name", 0U) == 0);
	CHECK(validate(store, server, &param) == X509_V_OK);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "other.example", 0U) == 1);
	CHECK(X509_VERIFY_PARAM_set1_host(&param, "bad name", 0U) == 0);
	CHECK(validate(store, server, &param) == X509_V_ERR_HOSTNAME_MISMATCH);
	drain_errors();

	/* Frees the certificates and the stores. */
	X509_free(server);
	X509_free(other);
	cryc_x509_store_free(store);
	cryc_x509_store_free(empty);
	free(der);
	free(other_der);
}

#ifdef _WIN32
/* The thread function of Windows. */
static DWORD WINAPI
thread_entry(
	LPVOID argument)
{
	/* Runs the body. */
	thread_body((struct thread_job *)argument);
	return 0;
}
#else
/* The thread function of POSIX. */
static void *
thread_entry(
	void *argument)
{
	/* Runs the body. */
	thread_body((struct thread_job *)argument);
	return NULL;
}
#endif

/* What each thread does: adds its own certificates and the shared ones to the store, and takes and drops references to one certificate. */
static void
thread_body(
	struct thread_job *job)
{
	unsigned index;
	long count;
	int added;

	/* The thread's own certificates (distinct from every other thread's). */
	job->ok = 1;
	for (index = 0; index < PER_THREAD; index++) {
		added = X509_STORE_add_cert(job->store, job->own[index]);
		if (added != 1)
			job->ok = 0;
	}

	/* The certificates every thread adds (duplicates between threads). */
	for (index = 0; index < SHARED_CERTS; index++) {
		added = X509_STORE_add_cert(job->store, job->shared[index]);
		if (added != 1)
			job->ok = 0;
	}

	/* References to one certificate, taken and dropped. */
	for (index = 0; index < REF_ROUNDS; index++) {
		count = cryc_os_atomic_add(&job->counted->references, 1L);
		if (count < 2L)
			job->ok = 0;
		X509_free(job->counted);
	}
}

/* Several threads adding to one store and sharing one certificate. */
static void
test_threads(void)
{
	struct thread_job jobs[THREADS];
	X509 *own[THREADS][PER_THREAD];
	X509 *shared[SHARED_CERTS];
	X509 *counted;
	X509_STORE *store;
	unsigned char *der;
	size_t size;
	unsigned thread;
	unsigned index;
#ifdef _WIN32
	HANDLE handles[THREADS];
#else
	pthread_t handles[THREADS];
#endif

	/* The certificates, built before the threads start. */
	store = cryc_x509_store_new();
	CHECK(store != NULL);
	for (thread = 0; thread < THREADS; thread++) {
		for (index = 0; index < PER_THREAD; index++) {
			der = simple_cert(10000U + thread * PER_THREAD + index, &size);
			own[thread][index] = make_cert(der, size);
			free(der);
		}
	}

	/* The certificates that all threads share. */
	for (index = 0; index < SHARED_CERTS; index++) {
		der = simple_cert(20000U + index, &size);
		shared[index] = make_cert(der, size);
		free(der);
	}

	/* The certificate whose reference count the threads move. */
	der = simple_cert(30000U, &size);
	counted = make_cert(der, size);
	free(der);
	if (store == NULL)
		return;

	/* Runs the threads together. */
	for (thread = 0; thread < THREADS; thread++) {
		jobs[thread].store = store;
		jobs[thread].own = own[thread];
		jobs[thread].shared = shared;
		jobs[thread].counted = counted;
		jobs[thread].ok = 0;
#ifdef _WIN32
		handles[thread] = CreateThread(NULL, 0, thread_entry, &jobs[thread], 0, NULL);
		CHECK(handles[thread] != NULL);
#else
		CHECK(pthread_create(&handles[thread], NULL, thread_entry, &jobs[thread]) == 0);
#endif
	}

	/* Waits for every thread to end. */
	for (thread = 0; thread < THREADS; thread++) {
#ifdef _WIN32
		WaitForSingleObject(handles[thread], INFINITE);
		CloseHandle(handles[thread]);
#else
		pthread_join(handles[thread], NULL);
#endif
	}

	/* Every add succeeded; the store holds each distinct certificate once; the reference count came back. */
	for (thread = 0; thread < THREADS; thread++)
		CHECK(jobs[thread].ok == 1U);
	CHECK(store->store.count == THREADS * PER_THREAD + SHARED_CERTS);
	CHECK(counted->references == 1L);
	CHECK(no_error());

	/* Frees everything. */
	for (thread = 0; thread < THREADS; thread++) {
		for (index = 0; index < PER_THREAD; index++)
			X509_free(own[thread][index]);
	}

	/* Frees the shared certificates and the counted one, then the store. */
	for (index = 0; index < SHARED_CERTS; index++)
		X509_free(shared[index]);
	X509_free(counted);
	cryc_x509_store_free(store);
}

/* Reads a whole file into a NUL-terminated buffer; NULL when it cannot be read. */
static char *
read_file(
	const char *path,
	size_t *size)
{
	FILE *stream;
	long length;
	char *data;

	/* Binary mode (a Windows text mode would change line ends). */
	stream = fopen(path, "rb");
	if (stream == NULL)
		return NULL;
	fseek(stream, 0, SEEK_END);
	length = ftell(stream);
	fseek(stream, 0, SEEK_SET);
	data = malloc((size_t)length + 1U);
	if (data == NULL) {
		fclose(stream);
		return NULL;
	}

	/* Reads the whole file and ends the text with a NUL. */
	*size = fread(data, 1, (size_t)length, stream);
	fclose(stream);
	data[*size] = '\0';
	return data;
}

/* Reads every certificate of the ca-certificates bundle with d2i_X509 and puts them in a store. */
static void
test_bundle(void)
{
	const char *path;
	char *text;
	size_t size;
	size_t position;
	uint8_t *der;
	size_t der_size;
	unsigned char *input;
	const unsigned char *cursor;
	unsigned long expected;
	unsigned long read;
	unsigned long same;
	const char *scan;
	X509 *certificate;
	X509_STORE *store;
	struct cryc_x509 direct;
	int error;

	/* The bundle must be readable: a missing file is a failure, not a pass. */
	path = getenv("WS335_CACERT");
	if (path == NULL)
		path = CACERT_DEFAULT;
	text = read_file(path, &size);
	CHECK(text != NULL);
	if (text == NULL)
		return;

	/* The number of certificates, counted without the PEM reader. */
	expected = 0;
	for (scan = strstr(text, "-----BEGIN CERTIFICATE-----"); scan != NULL;
	     scan = strstr(scan + 1, "-----BEGIN CERTIFICATE-----"))
		expected++;
	CHECK(expected > 100UL);

	/* Every block: read as the whole input, equal to the parser's own reading, added to a store. */
	store = cryc_x509_store_new();
	CHECK(store != NULL);
	position = 0;
	read = 0;
	same = 0;
	for (;;) {
		error = cryc_pem_next_certificate(text, size, &position, &der, &der_size);
		if (error == ENOENT)
			break;
		CHECK(error == 0);
		if (error != 0)
			continue;
		input = exact_copy(der, der_size);
		cursor = input;
		certificate = d2i_X509(NULL, &cursor, (long)der_size);
		CHECK(certificate != NULL);
		if (certificate != NULL) {
			read++;
			CHECK(cursor == input + der_size);
			CHECK(cryc_x509_parse(&direct, der, der_size) == 0);
			if (direct.not_before == certificate->cert.not_before &&
			    direct.not_after == certificate->cert.not_after &&
			    direct.key.type == certificate->cert.key.type &&
			    direct.flags == certificate->cert.flags &&
			    direct.subject_size == certificate->cert.subject_size)
				same++;
			CHECK(store != NULL && X509_STORE_add_cert(store, certificate) == 1);
			X509_free(certificate);
		}

		/* Releases the buffers of this certificate. */
		free(input);
		free(der);
	}

	/* Releases the bundle text. */
	free(text);
	CHECK(read == expected);
	CHECK(same == expected);
	CHECK(store != NULL && store->store.count == expected);
	CHECK(no_error());
	printf("bundle: %lu certificates read with d2i_X509 and stored\n", read);
	cryc_x509_store_free(store);
}

/* Reads every certificate of the x509-limbo cases with d2i_X509: it accepts exactly what the parser accepts. */
static void
test_limbo(void)
{
	struct vec_file file;
	unsigned char *buffer;
	unsigned char *input;
	const unsigned char *cursor;
	char key[32];
	size_t size;
	size_t index;
	unsigned long total;
	unsigned long accepted;
	unsigned long refused;
	unsigned long mismatches;
	struct cryc_x509 direct;
	X509 *certificate;
	int direct_error;
	int open_error;
	int more;

	/* The records must be present: a missing file is a failure. */
	open_error = vec_open(&file, "limbo");
	if (open_error != 0) {
		CHECK(0);
		return;
	}

	/* The buffer that holds one certificate of the cases. */
	buffer = malloc(LIMBO_MAX);
	CHECK(buffer != NULL);
	if (buffer == NULL) {
		vec_close(&file);
		return;
	}

	/* The peer, the trusted and the untrusted certificates of every case. */
	total = 0;
	accepted = 0;
	refused = 0;
	mismatches = 0;
	for (;;) {
		/* Reads the next record, and stops at the end of the file. */
		more = vec_next(&file);
		if (!more)
			break;

		/* Every certificate the record holds. */
		for (index = 0; index < 125U; index++) {
			if (index == 0U)
				sprintf(key, "peer");
			else if (index < 5U)
				sprintf(key, "trusted.%u", (unsigned)(index - 1U));
			else
				sprintf(key, "untrusted.%u", (unsigned)(index - 5U));
			size = vec_hex(&file, key, buffer, LIMBO_MAX);
			if (size == (size_t)-1 || size == 0U)
				continue;
			total++;
			input = exact_copy(buffer, size);
			cursor = input;
			certificate = d2i_X509(NULL, &cursor, (long)size);
			direct_error = cryc_x509_parse(&direct, input, size);
			if (certificate != NULL) {
				accepted++;
				if (direct_error != 0 ||
				    cursor != input + size ||
				    certificate->der_size != size ||
				    direct.not_after != certificate->cert.not_after ||
				    direct.flags != certificate->cert.flags)
					mismatches++;
				X509_free(certificate);
				drain_errors();
			} else {
				refused++;
				if (direct_error == 0)
					mismatches++;
				CHECK(drain_errors() == 1U);
			}

			/* Releases the copy of this certificate. */
			free(input);
		}
	}

	/* Closes the file and frees the buffer. */
	vec_close(&file);
	free(buffer);
	CHECK(mismatches == 0UL);
	CHECK(total > 10000UL);
	CHECK(accepted > refused);
	printf("limbo: %lu certificates through d2i_X509 (%lu accepted, %lu refused, %lu differences from the parser)\n", total, accepted, refused, mismatches);
}

/* Runs every check. */
int
main(void)
{
	int status;

	/* The error queue starts empty. */
	drain_errors();

	/* Runs every group of checks. */
	test_verdicts();
	test_error_strings();
	test_d2i_basic();
	test_d2i_out();
	test_d2i_errors();
	test_d2i_hostile();
	test_d2i_sizes();
	test_from_der();
	test_references();
	test_store();
	test_store_limit();
	test_host();
	test_ip_valid();
	test_ip_invalid();
	test_ip_differential();
	test_param_interplay();
	test_validation();
	test_threads();
	test_bundle();
	test_limbo();

	/* The last line the runner reads. */
	status = check_finish("test-api-x509");
	return status;
}
