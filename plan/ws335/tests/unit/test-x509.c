/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the X.509 parser (cryc-x509.h): signature AlgorithmIdentifiers,
 * SubjectPublicKeyInfo and certificates.
 *
 * Every input of the unit cases is built here from RFC 5280 4.1 and 4.2,
 * RFC 5480 2, RFC 3279 2.3.1, RFC 4055 3.1 and 5, RFC 5758 3.2 and RFC
 * 8017 A.1.1 and A.2, with the DER writer of derwriter.h and with hex
 * text whose lengths are computed by the helpers below; OIDs are written
 * as dotted text and encoded by the writer, not copied from the library.
 * Each field, each extension and each rule the header names has cases that
 * must be accepted and cases that must be refused, and the whole parser is
 * run on every prefix and on single-byte and random mutations of valid
 * certificates under the sanitizers.
 *
 * Three corpora come from outside: the ca-certificates bundle (every
 * certificate must parse, and its key kind is cross-checked by a plain
 * search of the encoded key), the x509-limbo certificates (every
 * certificate of a case whose expected result is SUCCESS must parse; the
 * refusals of the others are counted), and the public keys of the
 * Wycheproof files (the key compared with the file's own fields).  The
 * expected values never come from another implementation.
 */

#include "check.h"
#include "derwriter.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-der.h"
#include "userland/base/libcrypto-compat/cryc-pem.h"
#include "userland/base/libcrypto-compat/cryc-x509.h"

/* The room of one built structure, in bytes. */
#define BLOB_MAX	8192U

/* The room of one hex text, and how many a test function may use between hx_reset calls. */
#define HEX_ROOM	1024U
#define HEX_POOL	1024U

/* The largest hex-encoded object read from a Wycheproof file, in bytes. */
#define OBJECT_MAX	8192U

/* The largest certificate of the x509-limbo cases is about 78 KB. */
#define LIMBO_MAX	131072U

/* Where the bundle is looked for when WS335_CACERT is not set. */
#define CACERT_DEFAULT	"/home/awe/zedBSD-claude1/build/distfiles/cacert-2026-08-13.pem"

/* The most mutated certificates of the random run. */
#define RANDOM_RUNS	30000U

/* The seconds since 1970 of 2024-01-01T00:00:00Z and of 2034-01-01T00:00:00Z (3653 days later, three leap days). */
#define TIME_2024	1704067200
#define TIME_2034	2019686400

/* The seconds since 1970 of 1950-01-01T00:00:00Z (7305 days before 1970) and of 2049-12-31T23:59:59Z (one second before 2050). */
#define TIME_1950	(-631152000)
#define TIME_2049_END	((int64_t)CRYC_U64(0, 2524607999U))

/* Hex of the pieces that recur: a CommonName type, TRUE, and the purposes serverAuth, clientAuth, codeSigning, anyExtendedKeyUsage. */
#define HX_CN		"0603550403"
#define HX_TRUE		"0101ff"
#define HX_SERVER	"06082b06010505070301"
#define HX_CLIENT	"06082b06010505070302"
#define HX_CODE		"06082b06010505070303"
#define HX_ANY		"0604551d2500"

/* Records one check that a view of a parsed structure holds the expected bytes; an absent view is a failed check, not a crash. */
#define CHECK_VIEW(view, expected, size) check_view((view), (const unsigned char *)(expected), (size), __FILE__, __LINE__, #view)

/*
 * A structure being built: the bytes and the writer over them.  The
 * writer's error flag is set when the room ran out or a part was bad.
 */
struct blob {
	unsigned char data[BLOB_MAX];
	struct der_writer writer;
};

/*
 * The fields of one certificate to build.  Pointers are to the parts the
 * caller prepared; a NULL part is left out as the comment of the builder
 * says.
 */
struct cert_spec {
	int version;
	const char *version_raw;
	const char *serial;
	const struct blob *tbs_alg;
	const struct blob *outer_alg;
	const struct blob *issuer;
	const struct blob *subject;
	const struct blob *validity;
	const struct blob *spki;
	int uid1;
	int uid2;
	int uid_swapped;
	const char *uid1_raw;
	const struct blob *extensions;
	const char *extensions_raw;
	const char *signature;
	const char *tbs_tail;
	const char *cert_tail;
	const char *outer_tail;
};

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x2545f491U;

/* The exact-size copy the last parse read (kept so the views stay valid), and its size. */
static unsigned char *held;
static size_t held_size;

/* The hex texts the hx_ helpers return, and the next one to use: a test function starts with hx_reset and its texts then stay valid to its end. */
static char hex_pool[HEX_POOL][HEX_ROOM];
static unsigned hex_next;

/*
 * The prepared parts every case starts from: the algorithm identifiers,
 * the Names, the validity, the keys, the default extensions.
 */
static struct blob part_null;
static struct blob alg_rsa256;
static struct blob alg_rsa384;
static struct blob alg_rsa512;
static struct blob alg_ecdsa256;
static struct blob name_root;
static struct blob name_leaf;
static struct blob validity_default;
static struct blob spki_ec256;
static struct blob spki_rsa;
static struct blob exts_default;

/* The counts of the bundle, printed at the end. */
static unsigned long bundle_certs;
static unsigned long bundle_rsa;
static unsigned long bundle_ec256;
static unsigned long bundle_ec384;
static unsigned long bundle_unsupported;
static unsigned long bundle_p521;

/* The counts of the limbo run. */
static unsigned long limbo_certs;
static unsigned long limbo_parsed;
static unsigned long limbo_success_certs;
static unsigned long limbo_success_failed;

/* Compares a view with expected bytes; a NULL view fails the check without being read. */
static int
check_view(
	const unsigned char *view,
	const unsigned char *expected,
	size_t size,
	const char *file,
	int line,
	const char *text)
{
	/* An absent view cannot hold the bytes. */
	if (view == NULL)
		return check_one(0, file, line, text);
	return check_bytes(view, expected, size, file, line, text);
}

/* Returns the byte at index of a view, or -1 when the view is absent or too short. */
static int
view_byte(
	const unsigned char *view,
	size_t size,
	size_t index)
{
	/* Nothing to read in an absent or short view. */
	if (view == NULL || index >= size)
		return -1;
	return view[index];
}

/* Returns the next pseudo-random 32-bit value. */
static uint32_t
random32(void)
{
	/* Marsaglia's xorshift32 with the shifts 13, 17, 5. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* Starts the hex texts over: texts returned before are no longer valid. */
static void
hx_reset(void)
{
	/* The next text is the first buffer. */
	hex_next = 0;
}

/* Returns the next hex text buffer. */
static char *
hx_buffer(void)
{
	char *buffer;

	/* A test function uses far fewer than the pool holds; running out is a bug of the test. */
	if (hex_next >= HEX_POOL) {
		printf("FAIL hex text pool exhausted\n");
		exit(EXIT_FAILURE);
	}
	buffer = hex_pool[hex_next];
	hex_next++;
	return buffer;
}

/* Returns the hex of one element: the tag, the length (up to 65535 octets), then the contents given as hex. */
static const char *
hx_tlv(
	unsigned tag,
	const char *contents)
{
	char *buffer;
	size_t length;

	/* The length in the minimal octets. */
	buffer = hx_buffer();
	length = strlen(contents) / 2U;
	if (length < 128U)
		sprintf(buffer, "%02x%02x%s", tag, (unsigned)length, contents);
	else if (length < 256U)
		sprintf(buffer, "%02x81%02x%s", tag, (unsigned)length, contents);
	else
		sprintf(buffer, "%02x82%04x%s", tag, (unsigned)length, contents);
	return buffer;
}

/* Returns the hex of two hex texts joined. */
static const char *
hx_cat(
	const char *first,
	const char *second)
{
	char *buffer;

	/* One after the other. */
	buffer = hx_buffer();
	sprintf(buffer, "%s%s", first, second);
	return buffer;
}

/* Returns the hex of three hex texts joined. */
static const char *
hx_cat3(
	const char *first,
	const char *second,
	const char *third)
{
	char *buffer;

	/* One after the other. */
	buffer = hx_buffer();
	sprintf(buffer, "%s%s%s", first, second, third);
	return buffer;
}

/* Returns the hex of the bytes of a text (a dNSName's characters, say). */
static const char *
hx_text(
	const char *text)
{
	char *buffer;
	size_t index;

	/* Two hex digits a character. */
	buffer = hx_buffer();
	for (index = 0; text[index] != '\0'; index++)
		sprintf(buffer + index * 2U, "%02x", (unsigned)(unsigned char)text[index]);
	return buffer;
}

/* Starts an empty blob. */
static void
blob_init(
	struct blob *blob)
{
	der_init(&blob->writer, blob->data, BLOB_MAX);
}

/* Returns the bytes written so far. */
static size_t
blob_size(
	const struct blob *blob)
{
	return blob->writer.used;
}

/* Appends raw bytes given as hex text. */
static void
blob_hex(
	struct blob *blob,
	const char *hex)
{
	unsigned char bytes[BLOB_MAX];
	size_t size;

	/* Decodes, or marks the blob bad. */
	size = hex_decode(hex, bytes, sizeof(bytes));
	if (size == (size_t)-1) {
		blob->writer.error = 1;
		return;
	}
	der_raw(&blob->writer, bytes, size);
}

/* Appends another blob's bytes. */
static void
blob_cat(
	struct blob *blob,
	const struct blob *other)
{
	/* A bad part spoils the whole. */
	if (other->writer.error) {
		blob->writer.error = 1;
		return;
	}
	der_raw(&blob->writer, other->data, other->writer.used);
}

/* Appends the bytes of inner as the contents of one element with tag. */
static void
blob_wrap(
	struct blob *blob,
	unsigned tag,
	const struct blob *inner)
{
	der_wrap(&blob->writer, tag, &inner->writer);
}

/* Appends one element with tag whose contents are given as hex text. */
static void
blob_tlv(
	struct blob *blob,
	unsigned tag,
	const char *hex)
{
	unsigned char bytes[BLOB_MAX];
	size_t size;

	/* Decodes the contents, then writes the element. */
	size = hex_decode(hex, bytes, sizeof(bytes));
	if (size == (size_t)-1) {
		blob->writer.error = 1;
		return;
	}
	der_put(&blob->writer, tag, bytes, size);
}

/* Appends one element with tag whose contents are the characters of text (a time, a string). */
static void
blob_text(
	struct blob *blob,
	unsigned tag,
	const char *text)
{
	der_put(&blob->writer, tag, (const unsigned char *)text, strlen(text));
}

/* Appends one OBJECT IDENTIFIER written as dotted text. */
static void
blob_oid(
	struct blob *blob,
	const char *text)
{
	der_oid(&blob->writer, text);
}

/* Builds SEQUENCE { OID, params } (params may be NULL for none). */
static void
make_alg(
	struct blob *out,
	const char *oid,
	const struct blob *params)
{
	struct blob inner;

	/* The OID, then the parameters when given. */
	blob_init(&inner);
	blob_oid(&inner, oid);
	if (params != NULL)
		blob_cat(&inner, params);
	blob_init(out);
	blob_wrap(out, CRYC_DER_SEQUENCE, &inner);
}

/* Builds a Name with one attribute: SEQUENCE { SET { SEQUENCE { 2.5.4.3, UTF8String text } } }. */
static void
make_name(
	struct blob *out,
	const char *text)
{
	struct blob pair;
	struct blob set;

	/* The attribute type and value. */
	blob_init(&pair);
	blob_oid(&pair, "2.5.4.3");
	blob_text(&pair, CRYC_DER_UTF8_STRING, text);
	blob_init(&set);
	blob_wrap(&set, CRYC_DER_SEQUENCE, &pair);

	/* The RDN and the Name. */
	blob_init(&pair);
	blob_wrap(&pair, CRYC_DER_SET, &set);
	blob_init(out);
	blob_wrap(out, CRYC_DER_SEQUENCE, &pair);
}

/* Builds a validity SEQUENCE from two time elements: tag and text each. */
static void
make_validity(
	struct blob *out,
	unsigned first_tag,
	const char *first,
	unsigned second_tag,
	const char *second)
{
	struct blob inner;

	/* notBefore, notAfter. */
	blob_init(&inner);
	blob_text(&inner, first_tag, first);
	blob_text(&inner, second_tag, second);
	blob_init(out);
	blob_wrap(out, CRYC_DER_SEQUENCE, &inner);
}

/* Builds an Extension: SEQUENCE { OID, [BOOLEAN], OCTET STRING value }; critical is 0 (left out), 1 (TRUE) or 2 (an explicit FALSE). */
static void
make_extension(
	struct blob *out,
	const char *oid,
	int critical,
	const char *value_hex)
{
	struct blob inner;

	/* The OID, the flag, the value in its OCTET STRING. */
	blob_init(&inner);
	blob_oid(&inner, oid);
	if (critical == 1)
		blob_tlv(&inner, CRYC_DER_BOOLEAN, "ff");
	if (critical == 2)
		blob_tlv(&inner, CRYC_DER_BOOLEAN, "00");
	blob_tlv(&inner, CRYC_DER_OCTET_STRING, value_hex);
	blob_init(out);
	blob_wrap(out, CRYC_DER_SEQUENCE, &inner);
}

/* Appends a point: the first octet then count octets from a counter so that the bytes differ. */
static void
append_point(
	struct blob *out,
	unsigned char first,
	size_t count,
	unsigned seed)
{
	unsigned char bytes[97];
	size_t index;

	/* The first octet, then the counter bytes. */
	bytes[0] = first;
	for (index = 1; index <= count; index++)
		bytes[index] = (unsigned char)(seed + index * 7U);
	der_raw(&out->writer, bytes, 1U + count);
}

/* Builds a SubjectPublicKeyInfo around an algorithm and the bytes of a key, with the unused-bits octet. */
static void
make_spki(
	struct blob *out,
	const struct blob *algorithm,
	const struct blob *key,
	unsigned unused)
{
	struct blob body;
	unsigned char bits[BLOB_MAX];

	/* The identifier, then the key as a BIT STRING. */
	bits[0] = (unsigned char)unused;
	memcpy(bits + 1, key->data, blob_size(key));
	blob_init(&body);
	blob_cat(&body, algorithm);
	der_put(&body.writer, CRYC_DER_BIT_STRING, bits, 1U + blob_size(key));
	blob_init(out);
	blob_wrap(out, CRYC_DER_SEQUENCE, &body);
}

/* Builds an EC SubjectPublicKeyInfo: the curve OID as text, the point bytes as a blob, the unused bits. */
static void
make_spki_ec(
	struct blob *out,
	const char *curve,
	const struct blob *point,
	unsigned unused)
{
	struct blob parameters;
	struct blob algorithm;

	/* The identifier: id-ecPublicKey with the named curve. */
	blob_init(&parameters);
	blob_oid(&parameters, curve);
	make_alg(&algorithm, "1.2.840.10045.2.1", &parameters);
	make_spki(out, &algorithm, point, unused);
}

/* Builds an RSAPublicKey from big-endian n and e bytes (a leading 0x00 is added as needed). */
static void
make_rsa_public_key(
	struct blob *out,
	const unsigned char *n,
	size_t n_size,
	const unsigned char *e,
	size_t e_size)
{
	struct blob body;

	/* SEQUENCE { INTEGER n, INTEGER e }. */
	blob_init(&body);
	der_unsigned(&body.writer, n, n_size);
	der_unsigned(&body.writer, e, e_size);
	blob_init(out);
	blob_wrap(out, CRYC_DER_SEQUENCE, &body);
}

/* Builds an RSASSA-PSS identifier with each field given as hex of its explicit [n] contents (NULL leaves the field out). */
static void
make_pss(
	struct blob *out,
	const char *hash,
	const char *mask,
	const char *salt,
	const char *trailer)
{
	struct blob body;
	struct blob parameters;
	struct blob inner;

	/* The four optional fields under [0] to [3]. */
	blob_init(&body);
	if (hash != NULL) {
		blob_init(&inner);
		blob_hex(&inner, hash);
		blob_wrap(&body, 0xa0U, &inner);
	}
	if (mask != NULL) {
		blob_init(&inner);
		blob_hex(&inner, mask);
		blob_wrap(&body, 0xa1U, &inner);
	}
	if (salt != NULL) {
		blob_init(&inner);
		blob_hex(&inner, salt);
		blob_wrap(&body, 0xa2U, &inner);
	}
	if (trailer != NULL) {
		blob_init(&inner);
		blob_hex(&inner, trailer);
		blob_wrap(&body, 0xa3U, &inner);
	}
	blob_init(&parameters);
	blob_wrap(&parameters, CRYC_DER_SEQUENCE, &body);
	make_alg(out, "1.2.840.113549.1.1.10", &parameters);
}

/* Frees the exact-size copy of the last parse. */
static void
held_release(void)
{
	/* Nothing is held before the first parse. */
	free(held);
	held = NULL;
	held_size = 0;
}

/* Copies size bytes into a new exact-size allocation, kept until the next call, so that a read past either end is seen by the sanitizer. */
static const unsigned char *
held_copy(
	const unsigned char *bytes,
	size_t size)
{
	held_release();
	held = malloc(size == 0U ? 1U : size);
	if (held == NULL) {
		printf("FAIL out of memory\n");
		exit(EXIT_FAILURE);
	}
	if (size > 0U)
		memcpy(held, bytes, size);
	else
		held[0] = 0;
	held_size = size;
	return held;
}

/* Returns 1 when a struct cryc_x509 is entirely zero. */
static int
cert_is_zero(
	const struct cryc_x509 *cert)
{
	struct cryc_x509 zero;

	/* Compared with a zeroed one. */
	memset(&zero, 0, sizeof(zero));
	return memcmp(cert, &zero, sizeof(zero)) == 0;
}

/* Parses bytes (on an exact-size copy kept until the next call) into cert, filled with a pattern first so that an unwritten field shows. */
static int
parse_bytes(
	struct cryc_x509 *cert,
	const unsigned char *bytes,
	size_t size)
{
	const unsigned char *copy;

	/* The pattern, then the parse. */
	copy = held_copy(bytes, size);
	memset(cert, 0xa5, sizeof(*cert));
	return cryc_x509_parse(cert, copy, size);
}

/* Parses a blob as a certificate; the views stay valid until the next parse. */
static int
parse_blob(
	struct cryc_x509 *cert,
	const struct blob *blob)
{
	return parse_bytes(cert, blob->data, blob_size(blob));
}

/*
 * Builds a certificate from a spec: Certificate { tbsCertificate,
 * signatureAlgorithm, signatureValue }.  The tbs offset and size are given
 * when the pointers are not NULL.  A part is left out when its pointer is
 * NULL: the validity, the extensions; the version when version < 0 and
 * version_raw is NULL (version_raw, hex, is the contents of the [0] when
 * given); the unique identifiers when uid1 and uid2 are zero.
 */
static void
build_cert(
	const struct cert_spec *spec,
	struct blob *out,
	size_t *tbs_offset,
	size_t *tbs_size)
{
	struct blob tbs_body;
	struct blob tbs;
	struct blob certificate_body;
	struct blob inner;
	struct blob wrapper;
	unsigned char version_value;
	size_t tail_size;

	/* The tbsCertificate contents, field by field. */
	blob_init(&tbs_body);
	if (spec->version_raw != NULL)
		blob_tlv(&tbs_body, 0xa0U, spec->version_raw);
	else if (spec->version >= 0) {
		blob_init(&inner);
		version_value = (unsigned char)spec->version;
		der_put(&inner.writer, CRYC_DER_INTEGER, &version_value, 1);
		blob_wrap(&tbs_body, 0xa0U, &inner);
	}
	blob_tlv(&tbs_body, CRYC_DER_INTEGER, spec->serial);
	blob_cat(&tbs_body, spec->tbs_alg);
	blob_cat(&tbs_body, spec->issuer);
	if (spec->validity != NULL)
		blob_cat(&tbs_body, spec->validity);
	blob_cat(&tbs_body, spec->subject);
	blob_cat(&tbs_body, spec->spki);

	/* The unique identifiers: a BIT STRING under [1] and [2]. */
	if (spec->uid_swapped) {
		blob_tlv(&tbs_body, 0x82U, "00aa");
		blob_tlv(&tbs_body, 0x81U, "00bb");
	} else {
		if (spec->uid1_raw != NULL)
			blob_tlv(&tbs_body, 0x81U, spec->uid1_raw);
		else if (spec->uid1)
			blob_tlv(&tbs_body, 0x81U, "00bb");
		if (spec->uid2)
			blob_tlv(&tbs_body, 0x82U, "00aa");
	}

	/* The extensions: [3] { SEQUENCE OF Extension }, or the raw contents of the [3]. */
	if (spec->extensions != NULL) {
		blob_init(&inner);
		blob_wrap(&inner, CRYC_DER_SEQUENCE, spec->extensions);
		blob_wrap(&tbs_body, 0xa3U, &inner);
	}
	if (spec->extensions_raw != NULL)
		blob_tlv(&tbs_body, 0xa3U, spec->extensions_raw);
	if (spec->tbs_tail != NULL)
		blob_hex(&tbs_body, spec->tbs_tail);
	blob_init(&tbs);
	blob_wrap(&tbs, CRYC_DER_SEQUENCE, &tbs_body);

	/* The Certificate contents: tbs, the outer algorithm, the signature. */
	blob_init(&certificate_body);
	blob_cat(&certificate_body, &tbs);
	blob_cat(&certificate_body, spec->outer_alg);
	blob_tlv(&certificate_body, CRYC_DER_BIT_STRING, spec->signature);
	if (spec->cert_tail != NULL)
		blob_hex(&certificate_body, spec->cert_tail);
	blob_init(&wrapper);
	blob_wrap(&wrapper, CRYC_DER_SEQUENCE, &certificate_body);
	tail_size = 0;
	if (spec->outer_tail != NULL) {
		blob_hex(&wrapper, spec->outer_tail);
		tail_size = strlen(spec->outer_tail) / 2U;
	}
	blob_init(out);
	blob_cat(out, &wrapper);

	/* Where the tbs sits: after the SEQUENCE header, whose size is what the wrapper has beyond its contents and the tail. */
	if (tbs_offset != NULL)
		*tbs_offset = blob_size(&wrapper) - tail_size - blob_size(&certificate_body);
	if (tbs_size != NULL)
		*tbs_size = blob_size(&tbs);
}

/* Sets a spec to the default valid v3 certificate. */
static void
spec_default(
	struct cert_spec *spec)
{
	/* Every part is one of the prepared ones. */
	memset(spec, 0, sizeof(*spec));
	spec->version = 2;
	spec->serial = "01020304";
	spec->tbs_alg = &alg_rsa256;
	spec->outer_alg = &alg_rsa256;
	spec->issuer = &name_root;
	spec->subject = &name_leaf;
	spec->validity = &validity_default;
	spec->spki = &spki_ec256;
	spec->extensions = &exts_default;
	spec->signature = "00112233445566778899aabbccddeeff";
}

/* Builds a certificate from a spec and parses it; returns the parse error. */
static int
parse_spec(
	const struct cert_spec *spec,
	struct cryc_x509 *cert)
{
	static struct blob built;

	/* Build, then parse. */
	build_cert(spec, &built, NULL, NULL);
	CHECK(!built.writer.error);
	return parse_blob(cert, &built);
}

/* Checks that a spec is refused and leaves nothing of the certificate. */
static void
expect_refused(
	const struct cert_spec *spec)
{
	struct cryc_x509 cert;
	int error;

	/* EINVAL and a zeroed result. */
	error = parse_spec(spec, &cert);
	CHECK(error == EINVAL);
	CHECK(cert_is_zero(&cert));
}

/* Checks that a spec is accepted, and gives the certificate. */
static void
expect_accepted(
	const struct cert_spec *spec,
	struct cryc_x509 *cert)
{
	int error;

	/* Zero, and every view inside the bytes. */
	error = parse_spec(spec, cert);
	CHECK(error == 0);
	if (error == 0)
		CHECK(cert->der == held && cert->der_size == held_size);
}

/* Builds the default certificate with one extension list and parses it. */
static int
parse_with_extensions(
	const struct blob *extensions,
	struct cryc_x509 *cert)
{
	struct cert_spec spec;

	/* The default certificate with its own extensions. */
	spec_default(&spec);
	spec.extensions = extensions;
	return parse_spec(&spec, cert);
}

/* Parses the default certificate with a single extension given as OID, critical kind and the hex of the value. */
static int
parse_ext(
	struct cryc_x509 *cert,
	const char *oid,
	int critical,
	const char *value_hex)
{
	static struct blob extension;

	/* The one extension. */
	make_extension(&extension, oid, critical, value_hex);
	return parse_with_extensions(&extension, cert);
}

/* Returns 1 when the view lies inside [base, base + size) (an absent view, NULL with size 0, does). */
static int
view_inside(
	const unsigned char *view,
	size_t view_size,
	const unsigned char *base,
	size_t size)
{
	/* An absent view is fine; a present one must be inside. */
	if (view == NULL)
		return view_size == 0U;
	if (view < base)
		return 0;
	if (view_size > size)
		return 0;
	return (size_t)(view - base) <= size - view_size;
}

/* Checks that every view of a parsed certificate lies inside the bytes it was parsed from. */
static int
cert_views_inside(
	const struct cryc_x509 *cert,
	const unsigned char *base,
	size_t size)
{
	/* Each view of the structure. */
	if (cert->der != base || cert->der_size != size)
		return 0;
	if (!view_inside(cert->tbs, cert->tbs_size, base, size))
		return 0;
	if (!view_inside(cert->signature, cert->signature_size, base, size))
		return 0;
	if (!view_inside(cert->serial, cert->serial_size, base, size))
		return 0;
	if (!view_inside(cert->issuer, cert->issuer_size, base, size))
		return 0;
	if (!view_inside(cert->subject, cert->subject_size, base, size))
		return 0;
	if (!view_inside(cert->spki, cert->spki_size, base, size))
		return 0;
	if (!view_inside(cert->san, cert->san_size, base, size))
		return 0;
	if (!view_inside(cert->permitted, cert->permitted_size, base, size))
		return 0;
	if (!view_inside(cert->excluded, cert->excluded_size, base, size))
		return 0;
	if (!view_inside(cert->ski, cert->ski_size, base, size))
		return 0;
	if (!view_inside(cert->aki, cert->aki_size, base, size))
		return 0;
	if (!view_inside(cert->key.rsa.n, cert->key.rsa.n_size, base, size))
		return 0;
	if (!view_inside(cert->key.rsa.e, cert->key.rsa.e_size, base, size))
		return 0;
	if (!view_inside(cert->key.point, cert->key.point_size, base, size))
		return 0;
	return 1;
}

/* Prepares the parts every case starts from. */
static void
prepare_parts(void)
{
	struct blob inner;
	struct blob point;
	struct blob rsa_key;
	struct blob rsa_alg;
	struct blob extension;
	unsigned char modulus[256];
	unsigned char exponent[3];
	size_t index;

	/* NULL, and the identifiers sha256/384/512WithRSAEncryption and ecdsa-with-SHA256. */
	blob_init(&part_null);
	blob_tlv(&part_null, CRYC_DER_NULL, "");
	make_alg(&alg_rsa256, "1.2.840.113549.1.1.11", &part_null);
	make_alg(&alg_rsa384, "1.2.840.113549.1.1.12", &part_null);
	make_alg(&alg_rsa512, "1.2.840.113549.1.1.13", &part_null);
	make_alg(&alg_ecdsa256, "1.2.840.10045.4.3.2", NULL);

	/* Names and the validity 2024-01-01 to 2034-01-01 (a UTCTime and a GeneralizedTime). */
	make_name(&name_root, "Root");
	make_name(&name_leaf, "Leaf");
	make_validity(&validity_default, CRYC_DER_UTC_TIME, "240101000000Z", CRYC_DER_GENERALIZED_TIME, "20340101000000Z");

	/* A P-256 key and a 2048-bit RSA key. */
	blob_init(&point);
	append_point(&point, 0x04, 64, 3);
	make_spki_ec(&spki_ec256, "1.2.840.10045.3.1.7", &point, 0);
	for (index = 0; index < sizeof(modulus); index++)
		modulus[index] = (unsigned char)(0x81U + index * 5U);
	modulus[sizeof(modulus) - 1U] |= 1U;
	exponent[0] = 0x01;
	exponent[1] = 0x00;
	exponent[2] = 0x01;
	make_rsa_public_key(&rsa_key, modulus, sizeof(modulus), exponent, 3);
	make_alg(&rsa_alg, "1.2.840.113549.1.1.1", &part_null);
	make_spki(&spki_rsa, &rsa_alg, &rsa_key, 0);

	/* The default extensions: basicConstraints (CA, pathLen 3, critical), keyUsage (keyCertSign, cRLSign, critical), extKeyUsage, subjectAltName, SKI, AKI. */
	blob_init(&exts_default);
	make_extension(&extension, "2.5.29.19", 1, hx_tlv(0x30, "0101ff020103"));
	blob_cat(&exts_default, &extension);
	make_extension(&extension, "2.5.29.15", 1, hx_tlv(0x03, "0106"));
	blob_cat(&exts_default, &extension);
	blob_init(&inner);
	make_extension(&extension, "2.5.29.37", 0, hx_tlv(0x30, hx_cat(HX_SERVER, HX_CLIENT)));
	blob_cat(&exts_default, &extension);
	make_extension(&extension, "2.5.29.17", 0, hx_tlv(0x30, hx_tlv(0x82, hx_text("example.com"))));
	blob_cat(&exts_default, &extension);
	make_extension(&extension, "2.5.29.14", 0, hx_tlv(0x04, "a1a2a3a4"));
	blob_cat(&exts_default, &extension);
	make_extension(&extension, "2.5.29.35", 0, hx_tlv(0x30, hx_tlv(0x80, "b1b2b3b4")));
	blob_cat(&exts_default, &extension);
}

/* Checks the result of a signature algorithm parse. */
static void
check_algorithm(
	const struct blob *encoding,
	int expected_error,
	enum cryc_sig_alg expected_alg,
	size_t expected_salt)
{
	struct cryc_sig_params params;
	const unsigned char *copy;
	int error;

	/* The parse on an exact-size copy, over a pattern. */
	copy = held_copy(encoding->data, blob_size(encoding));
	memset(&params, 0xa5, sizeof(params));
	error = cryc_x509_signature_algorithm(copy, blob_size(encoding), &params);
	CHECK(error == expected_error);
	CHECK(params.alg == expected_alg);
	CHECK(params.salt_size == expected_salt);
}

/* The hex of SEQUENCE { id-shaN, NULL } for N = 256, 384, 512, the same for SHA-256 without parameters, SHA-1 and SHA-224. */
#define HEX_SHA256	"300d06096086480165030402010500"
#define HEX_SHA384	"300d06096086480165030402020500"
#define HEX_SHA512	"300d06096086480165030402030500"
#define HEX_SHA256_BARE	"300b0609608648016503040201"
#define HEX_SHA1	"300906052b0e03021a0500"
#define HEX_SHA224	"300d06096086480165030402040500"

/* The hex of SEQUENCE { id-mgf1, SEQUENCE { id-shaN, NULL } } for N = 256, 384, 512 and for SHA-1. */
#define HEX_MGF256	"301a06092a864886f70d010108300d06096086480165030402010500"
#define HEX_MGF384	"301a06092a864886f70d010108300d06096086480165030402020500"
#define HEX_MGF512	"301a06092a864886f70d010108300d06096086480165030402030500"
#define HEX_MGF1_SHA1	"301606092a864886f70d010108300906052b0e03021a0500"

/* Checks the signature algorithms: PKCS #1, ECDSA, PSS, the unsupported, the malformed. */
static void
test_signature_algorithms(void)
{
	struct blob alg;
	struct blob parameters;
	struct blob inner;
	unsigned char bytes[256];
	size_t size;
	size_t index;
	unsigned round;
	struct cryc_sig_params params;
	int error;

	/* sha256/384/512WithRSAEncryption, with NULL parameters and without (RFC 4055 5). */
	check_algorithm(&alg_rsa256, 0, CRYC_SIG_RSA_PKCS1_SHA256, 0);
	check_algorithm(&alg_rsa384, 0, CRYC_SIG_RSA_PKCS1_SHA384, 0);
	check_algorithm(&alg_rsa512, 0, CRYC_SIG_RSA_PKCS1_SHA512, 0);
	make_alg(&alg, "1.2.840.113549.1.1.11", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_RSA_PKCS1_SHA256, 0);
	make_alg(&alg, "1.2.840.113549.1.1.12", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_RSA_PKCS1_SHA384, 0);
	make_alg(&alg, "1.2.840.113549.1.1.13", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_RSA_PKCS1_SHA512, 0);

	/* A parameter that is not NULL on a PKCS #1 algorithm is malformed (an INTEGER, a NULL with contents). */
	blob_init(&parameters);
	blob_tlv(&parameters, CRYC_DER_INTEGER, "05");
	make_alg(&alg, "1.2.840.113549.1.1.11", &parameters);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	blob_init(&parameters);
	blob_tlv(&parameters, CRYC_DER_NULL, "00");
	make_alg(&alg, "1.2.840.113549.1.1.11", &parameters);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);

	/* ecdsa-with-SHA256/384/512 with no parameters (RFC 5758 3.2). */
	check_algorithm(&alg_ecdsa256, 0, CRYC_SIG_ECDSA_SHA256, 0);
	make_alg(&alg, "1.2.840.10045.4.3.3", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_ECDSA_SHA384, 0);
	make_alg(&alg, "1.2.840.10045.4.3.4", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_ECDSA_SHA512, 0);

	/* NULL or any other parameters on ECDSA are malformed. */
	make_alg(&alg, "1.2.840.10045.4.3.2", &part_null);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	make_alg(&alg, "1.2.840.10045.4.3.3", &part_null);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	blob_init(&parameters);
	blob_tlv(&parameters, CRYC_DER_OCTET_STRING, "00");
	make_alg(&alg, "1.2.840.10045.4.3.4", &parameters);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);

	/* Well-formed identifiers outside the range: SHA-1, MD5 and SHA-224 with RSA, ECDSA with SHA-1 and SHA-224, Ed25519, an unknown OID with odd parameters. */
	make_alg(&alg, "1.2.840.113549.1.1.5", &part_null);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_alg(&alg, "1.2.840.113549.1.1.4", &part_null);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_alg(&alg, "1.2.840.113549.1.1.14", &part_null);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_alg(&alg, "1.2.840.10045.4.1", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_alg(&alg, "1.2.840.10045.4.3.1", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_alg(&alg, "1.3.101.112", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	blob_init(&parameters);
	blob_tlv(&parameters, CRYC_DER_SEQUENCE, "020105");
	make_alg(&alg, "1.2.3.4.5.6.7", &parameters);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);

	/* RSASSA-PSS with SHA-256, 384 and 512 and the matching MGF1 (salt 32, 48, 64). */
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "020120", "020101");
	check_algorithm(&alg, 0, CRYC_SIG_RSA_PSS_SHA256, 32);
	make_pss(&alg, HEX_SHA384, HEX_MGF384, "020130", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_RSA_PSS_SHA384, 48);
	make_pss(&alg, HEX_SHA512, HEX_MGF512, "020140", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_RSA_PSS_SHA512, 64);

	/* The salt length: absent means 20, 0 and 64 are accepted, 65 and a huge one are outside the range, negative, empty and wrong-type ones are malformed. */
	make_pss(&alg, HEX_SHA256, HEX_MGF256, NULL, NULL);
	check_algorithm(&alg, 0, CRYC_SIG_RSA_PSS_SHA256, 20);
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "020100", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_RSA_PSS_SHA256, 0);
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "020140", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_RSA_PSS_SHA256, 64);
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "020141", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "02050100000000", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "0201ff", NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "0200", NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "04012a", NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);

	/* The hash identifier with no parameters is as good as with NULL (RFC 4055 2.1). */
	make_pss(&alg, HEX_SHA256_BARE, HEX_MGF256, "020120", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_RSA_PSS_SHA256, 32);

	/* A trailer field of 1 is accepted, any other value is outside the range. */
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "020120", "020102");
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "020120", "020100");
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "020120", "02050100000000");
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);

	/* A hash that is absent (SHA-1), SHA-1 or SHA-224, an MGF1 with another hash, an absent mask (MGF1 with SHA-1): outside the range. */
	make_pss(&alg, NULL, HEX_MGF256, "020114", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_pss(&alg, HEX_SHA1, HEX_MGF1_SHA1, "020114", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_pss(&alg, HEX_SHA224, HEX_MGF256, "020120", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_pss(&alg, HEX_SHA256, HEX_MGF384, "020120", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_pss(&alg, HEX_SHA384, HEX_MGF256, "020120", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_pss(&alg, HEX_SHA256, NULL, "020120", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_pss(&alg, HEX_SHA256, HEX_MGF1_SHA1, "020120", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_pss(&alg, NULL, NULL, NULL, NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);

	/* A mask generation function that is not MGF1 (OID ...1.1.9): outside the range. */
	make_pss(&alg, HEX_SHA256, "301a06092a864886f70d010109300d06096086480165030402010500", "020120", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);

	/* MGF1 whose parameters are missing or not a SEQUENCE, a hash with bad parameters or a bad shape, are malformed. */
	make_pss(&alg, HEX_SHA256, "300b06092a864886f70d010108", "020120", NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	make_pss(&alg, HEX_SHA256, "300d06092a864886f70d0101080500", "020120", NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	make_pss(&alg, "300f060960864801650304020104020105", HEX_MGF256, "020120", NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	make_pss(&alg, "3003020105", HEX_MGF256, "020120", NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	make_pss(&alg, "0500", HEX_MGF256, "020120", NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);

	/* A SET where a SEQUENCE belongs, in the parameters, the hash field, the mask field or MGF1's parameters: malformed. */
	blob_init(&parameters);
	blob_tlv(&parameters, CRYC_DER_SET, "");
	make_alg(&alg, "1.2.840.113549.1.1.10", &parameters);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	make_pss(&alg, "310b0609608648016503040201", HEX_MGF256, "020120", NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	make_pss(&alg, HEX_SHA256, "311a06092a864886f70d010108300d06096086480165030402010500", "020120", NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	make_pss(&alg, HEX_SHA256, "301a06092a864886f70d010108310d06096086480165030402010500", "020120", NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);

	/* The parameters absent: outside the range; an empty SEQUENCE is all defaults: outside the range; NULL: malformed. */
	make_alg(&alg, "1.2.840.113549.1.1.10", NULL);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	blob_init(&parameters);
	blob_tlv(&parameters, CRYC_DER_SEQUENCE, "");
	make_alg(&alg, "1.2.840.113549.1.1.10", &parameters);
	check_algorithm(&alg, 0, CRYC_SIG_UNSUPPORTED, 0);
	make_alg(&alg, "1.2.840.113549.1.1.10", &part_null);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);

	/* Trailing junk after the fields, fields out of order, repeated, or with two elements under one tag are malformed. */
	blob_init(&inner);
	blob_tlv(&inner, 0xa0U, HEX_SHA256);
	blob_hex(&inner, "0500");
	blob_init(&parameters);
	blob_wrap(&parameters, CRYC_DER_SEQUENCE, &inner);
	make_alg(&alg, "1.2.840.113549.1.1.10", &parameters);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	blob_init(&inner);
	blob_tlv(&inner, 0xa1U, HEX_MGF256);
	blob_tlv(&inner, 0xa0U, HEX_SHA256);
	blob_init(&parameters);
	blob_wrap(&parameters, CRYC_DER_SEQUENCE, &inner);
	make_alg(&alg, "1.2.840.113549.1.1.10", &parameters);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	blob_init(&inner);
	blob_tlv(&inner, 0xa0U, HEX_SHA256);
	blob_tlv(&inner, 0xa0U, HEX_SHA256);
	blob_init(&parameters);
	blob_wrap(&parameters, CRYC_DER_SEQUENCE, &inner);
	make_alg(&alg, "1.2.840.113549.1.1.10", &parameters);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	make_pss(&alg, HEX_SHA256 "020100", HEX_MGF256, NULL, NULL);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);

	/* Malformed identifiers: empty, not a SEQUENCE, no OID, a bad OID, an empty OID, trailing bytes, two parameters. */
	blob_init(&alg);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	blob_init(&alg);
	blob_tlv(&alg, CRYC_DER_SET, "060100");
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	blob_init(&alg);
	blob_tlv(&alg, CRYC_DER_SEQUENCE, "");
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	blob_init(&alg);
	blob_tlv(&alg, CRYC_DER_SEQUENCE, "0500");
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	blob_init(&alg);
	blob_tlv(&alg, CRYC_DER_SEQUENCE, "06028001");
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	blob_init(&alg);
	blob_tlv(&alg, CRYC_DER_SEQUENCE, "0600");
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	blob_init(&alg);
	blob_cat(&alg, &alg_rsa256);
	blob_hex(&alg, "00");
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);
	blob_init(&inner);
	blob_oid(&inner, "1.2.840.113549.1.1.11");
	blob_cat(&inner, &part_null);
	blob_cat(&inner, &part_null);
	blob_init(&alg);
	blob_wrap(&alg, CRYC_DER_SEQUENCE, &inner);
	check_algorithm(&alg, EINVAL, CRYC_SIG_NONE, 0);

	/* Every strict prefix of well-formed identifiers is refused. */
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "020120", "020101");
	for (index = 0; index < blob_size(&alg); index++) {
		blob_init(&inner);
		der_raw(&inner.writer, alg.data, index);
		check_algorithm(&inner, EINVAL, CRYC_SIG_NONE, 0);
	}
	for (index = 0; index < blob_size(&alg_rsa256); index++) {
		blob_init(&inner);
		der_raw(&inner.writer, alg_rsa256.data, index);
		check_algorithm(&inner, EINVAL, CRYC_SIG_NONE, 0);
	}

	/* Random bytes, half of them shaped like an identifier: no crash, and a refusal leaves the result zeroed. */
	for (round = 0; round < 20000U; round++) {
		size = random32() % sizeof(bytes);
		for (index = 0; index < size; index++)
			bytes[index] = (unsigned char)random32();
		if (round % 2U == 0U && size > 4U) {
			bytes[0] = 0x30;
			bytes[1] = (unsigned char)(size - 2U);
			bytes[2] = 0x06;
		}
		memset(&params, 0xa5, sizeof(params));
		error = cryc_x509_signature_algorithm(held_copy(bytes, size), size, &params);
		if (error != 0) {
			CHECK(error == EINVAL);
			CHECK(params.alg == CRYC_SIG_NONE && params.salt_size == 0U);
		}
	}

	/* The hash of each algorithm. */
	CHECK(cryc_sig_hash(CRYC_SIG_RSA_PKCS1_SHA256) == CRYC_HASH_SHA256);
	CHECK(cryc_sig_hash(CRYC_SIG_RSA_PKCS1_SHA384) == CRYC_HASH_SHA384);
	CHECK(cryc_sig_hash(CRYC_SIG_RSA_PKCS1_SHA512) == CRYC_HASH_SHA512);
	CHECK(cryc_sig_hash(CRYC_SIG_RSA_PSS_SHA256) == CRYC_HASH_SHA256);
	CHECK(cryc_sig_hash(CRYC_SIG_RSA_PSS_SHA384) == CRYC_HASH_SHA384);
	CHECK(cryc_sig_hash(CRYC_SIG_RSA_PSS_SHA512) == CRYC_HASH_SHA512);
	CHECK(cryc_sig_hash(CRYC_SIG_ECDSA_SHA256) == CRYC_HASH_SHA256);
	CHECK(cryc_sig_hash(CRYC_SIG_ECDSA_SHA384) == CRYC_HASH_SHA384);
	CHECK(cryc_sig_hash(CRYC_SIG_ECDSA_SHA512) == CRYC_HASH_SHA512);
	CHECK(cryc_sig_hash(CRYC_SIG_NONE) == CRYC_HASH_NONE);
	CHECK(cryc_sig_hash(CRYC_SIG_UNSUPPORTED) == CRYC_HASH_NONE);
	CHECK(cryc_sig_hash((enum cryc_sig_alg)99) == CRYC_HASH_NONE);
}

/* Checks the result of a public key parse: the error, the type, and for the other kinds the fields left zero. */
static void
check_key(
	const struct blob *spki,
	int expected_error,
	enum cryc_key_type expected_type)
{
	struct cryc_pubkey key;
	const unsigned char *copy;
	int error;

	/* The parse on an exact-size copy, over a pattern. */
	copy = held_copy(spki->data, blob_size(spki));
	memset(&key, 0xa5, sizeof(key));
	error = cryc_x509_public_key(copy, blob_size(spki), &key);
	CHECK(error == expected_error);
	CHECK(key.type == expected_type);
	if (expected_type == CRYC_KEY_NONE || expected_type == CRYC_KEY_UNSUPPORTED) {
		CHECK(key.rsa.n == NULL && key.rsa.n_size == 0U && key.rsa.e == NULL && key.rsa.e_size == 0U);
		CHECK(key.curve == CRYC_CURVE_NONE && key.point == NULL && key.point_size == 0U);
	}
}

/* Checks the RSA keys. */
static void
test_rsa_keys(void)
{
	struct blob spki;
	struct blob alg;
	struct blob parameters;
	struct blob key_bytes;
	struct cryc_pubkey key;
	unsigned char modulus[512];
	unsigned char exponent[3];
	size_t index;
	int error;

	/* A 2048-bit key: the views are the magnitudes (no sign byte), the key is as given. */
	for (index = 0; index < sizeof(modulus); index++)
		modulus[index] = (unsigned char)(0x80U + index * 3U);
	modulus[0] = 0xc3;
	modulus[sizeof(modulus) - 1U] = 0x8f;
	exponent[0] = 0x01;
	exponent[1] = 0x00;
	exponent[2] = 0x01;
	make_rsa_public_key(&key_bytes, modulus, 256, exponent, 3);
	make_alg(&alg, "1.2.840.113549.1.1.1", &part_null);
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, 0, CRYC_KEY_RSA);
	error = cryc_x509_public_key(held_copy(spki.data, blob_size(&spki)), blob_size(&spki), &key);
	CHECK(error == 0);
	CHECK(key.rsa.n_size == 256U);
	CHECK_VIEW(key.rsa.n, modulus, 256U);
	CHECK(key.rsa.e_size == 3U);
	CHECK_VIEW(key.rsa.e, exponent, 3U);
	CHECK(key.curve == CRYC_CURVE_NONE && key.point == NULL && key.point_size == 0U);

	/* A 4096-bit modulus with a one-byte exponent, and a one-byte modulus with exponent 3: no size limit is applied here. */
	exponent[0] = 0x03;
	make_rsa_public_key(&key_bytes, modulus, 512, exponent, 1);
	make_spki(&spki, &alg, &key_bytes, 0);
	error = cryc_x509_public_key(held_copy(spki.data, blob_size(&spki)), blob_size(&spki), &key);
	CHECK(error == 0 && key.type == CRYC_KEY_RSA && key.rsa.n_size == 512U && key.rsa.e_size == 1U && view_byte(key.rsa.e, key.rsa.e_size, 0) == 3);
	modulus[0] = 0x0f;
	make_rsa_public_key(&key_bytes, modulus, 1, exponent, 1);
	make_spki(&spki, &alg, &key_bytes, 0);
	error = cryc_x509_public_key(held_copy(spki.data, blob_size(&spki)), blob_size(&spki), &key);
	CHECK(error == 0 && key.type == CRYC_KEY_RSA && key.rsa.n_size == 1U && view_byte(key.rsa.n, key.rsa.n_size, 0) == 0x0f && view_byte(key.rsa.e, key.rsa.e_size, 0) == 0x03);

	/* Refusals: a zero modulus or exponent, a negative or non-minimal INTEGER, an extra INTEGER, a missing exponent, bytes after the key, a key that is not a SEQUENCE. */
	blob_init(&key_bytes);
	blob_hex(&key_bytes, "3006020100020103");
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&key_bytes);
	blob_hex(&key_bytes, "3006020115020100");
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&key_bytes);
	blob_hex(&key_bytes, "30060201ff020103");
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&key_bytes);
	blob_hex(&key_bytes, "30070202001502010" "3");
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&key_bytes);
	blob_hex(&key_bytes, "3009020115020103020101");
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&key_bytes);
	blob_hex(&key_bytes, "3003020115");
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&key_bytes);
	blob_hex(&key_bytes, "300602011502010300");
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&key_bytes);
	blob_hex(&key_bytes, "020115020103");
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);

	/* Parameters absent, not NULL, or unused bits in the key BIT STRING: refused. */
	make_rsa_public_key(&key_bytes, modulus, 256, exponent, 1);
	make_alg(&alg, "1.2.840.113549.1.1.1", NULL);
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&parameters);
	blob_tlv(&parameters, CRYC_DER_OCTET_STRING, "");
	make_alg(&alg, "1.2.840.113549.1.1.1", &parameters);
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	make_alg(&alg, "1.2.840.113549.1.1.1", &part_null);
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, 0, CRYC_KEY_RSA);
	exponent[0] = 0x10;
	make_rsa_public_key(&key_bytes, modulus, 256, exponent, 1);
	make_spki(&spki, &alg, &key_bytes, 1);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, 0, CRYC_KEY_RSA);
}

/* Checks the EC keys. */
static void
test_ec_keys(void)
{
	struct blob spki;
	struct blob alg;
	struct blob parameters;
	struct blob point;
	struct cryc_pubkey key;
	int error;

	/* A key on P-256 and on P-384: the curve, and the point as a view of the 1 + 2 * size bytes. */
	blob_init(&point);
	append_point(&point, 0x04, 64, 9);
	make_spki_ec(&spki, "1.2.840.10045.3.1.7", &point, 0);
	check_key(&spki, 0, CRYC_KEY_EC);
	error = cryc_x509_public_key(held_copy(spki.data, blob_size(&spki)), blob_size(&spki), &key);
	CHECK(error == 0 && key.curve == CRYC_CURVE_P256 && key.point_size == 65U);
	CHECK_VIEW(key.point, point.data, 65U);
	CHECK(key.rsa.n == NULL && key.rsa.n_size == 0U && key.rsa.e == NULL && key.rsa.e_size == 0U);
	blob_init(&point);
	append_point(&point, 0x04, 96, 11);
	make_spki_ec(&spki, "1.3.132.0.34", &point, 0);
	error = cryc_x509_public_key(held_copy(spki.data, blob_size(&spki)), blob_size(&spki), &key);
	CHECK(error == 0 && key.type == CRYC_KEY_EC && key.curve == CRYC_CURVE_P384 && key.point_size == 97U);
	CHECK_VIEW(key.point, point.data, 97U);

	/* A compressed point (02 or 03 and one coordinate) is well formed but not used. */
	blob_init(&point);
	append_point(&point, 0x02, 32, 1);
	make_spki_ec(&spki, "1.2.840.10045.3.1.7", &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	blob_init(&point);
	append_point(&point, 0x03, 32, 1);
	make_spki_ec(&spki, "1.2.840.10045.3.1.7", &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	blob_init(&point);
	append_point(&point, 0x03, 48, 1);
	make_spki_ec(&spki, "1.3.132.0.34", &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);

	/* Refusals: a wrong length, a wrong first octet (05, 00, and 04 with the compressed length), a point of the other curve's size, unused bits. */
	blob_init(&point);
	append_point(&point, 0x04, 62, 9);
	make_spki_ec(&spki, "1.2.840.10045.3.1.7", &point, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&point);
	append_point(&point, 0x05, 64, 9);
	make_spki_ec(&spki, "1.2.840.10045.3.1.7", &point, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&point);
	append_point(&point, 0x00, 64, 9);
	make_spki_ec(&spki, "1.2.840.10045.3.1.7", &point, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&point);
	append_point(&point, 0x04, 32, 9);
	make_spki_ec(&spki, "1.2.840.10045.3.1.7", &point, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&point);
	append_point(&point, 0x04, 64, 9);
	make_spki_ec(&spki, "1.3.132.0.34", &point, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&point);
	append_point(&point, 0x04, 96, 9);
	make_spki_ec(&spki, "1.2.840.10045.3.1.7", &point, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&point);
	append_point(&point, 0x04, 64, 9);
	point.data[blob_size(&point) - 1U] = 0x80;
	make_spki_ec(&spki, "1.2.840.10045.3.1.7", &point, 1);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	make_spki_ec(&spki, "1.2.840.10045.3.1.7", &point, 0);
	check_key(&spki, 0, CRYC_KEY_EC);

	/* Outside the range: P-521, secp256k1, brainpoolP256r1, no parameters, NULL parameters, explicit parameters. */
	blob_init(&point);
	append_point(&point, 0x04, 64, 9);
	make_spki_ec(&spki, "1.3.132.0.35", &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	make_spki_ec(&spki, "1.3.132.0.10", &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	make_spki_ec(&spki, "1.3.36.3.3.2.8.1.1.7", &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	make_alg(&alg, "1.2.840.10045.2.1", NULL);
	make_spki(&spki, &alg, &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	make_alg(&alg, "1.2.840.10045.2.1", &part_null);
	make_spki(&spki, &alg, &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	blob_init(&parameters);
	blob_tlv(&parameters, CRYC_DER_SEQUENCE, "020101");
	make_alg(&alg, "1.2.840.10045.2.1", &parameters);
	make_spki(&spki, &alg, &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);

	/* A malformed curve OID is malformed. */
	blob_init(&parameters);
	blob_tlv(&parameters, CRYC_DER_OID, "8001");
	make_alg(&alg, "1.2.840.10045.2.1", &parameters);
	make_spki(&spki, &alg, &point, 0);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
}

/* Checks the other kinds of key and the malformed SubjectPublicKeyInfo. */
static void
test_other_keys(void)
{
	struct blob spki;
	struct blob alg;
	struct blob point;
	struct blob key_bytes;
	struct blob inner;
	size_t index;

	/* Other algorithms are well formed and outside the range: Ed25519, X25519, RSASSA-PSS keys with and without parameters, an unknown OID. */
	blob_init(&point);
	blob_hex(&point, "7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa");
	make_alg(&alg, "1.3.101.112", NULL);
	make_spki(&spki, &alg, &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	make_alg(&alg, "1.3.101.110", NULL);
	make_spki(&spki, &alg, &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	blob_init(&key_bytes);
	blob_hex(&key_bytes, "3006020115020103");
	make_alg(&alg, "1.2.840.113549.1.1.10", NULL);
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	make_pss(&alg, HEX_SHA256, HEX_MGF256, "020120", NULL);
	make_spki(&spki, &alg, &key_bytes, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	make_alg(&alg, "1.2.3.4", NULL);
	make_spki(&spki, &alg, &point, 0);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);

	/* Malformed: empty, not a SEQUENCE, no key bits, key bits not a BIT STRING, a bad unused count, an extra element, trailing bytes, a bad identifier. */
	blob_init(&spki);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&spki);
	blob_tlv(&spki, CRYC_DER_SET, "");
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&inner);
	blob_cat(&inner, &alg_rsa256);
	blob_init(&spki);
	blob_wrap(&spki, CRYC_DER_SEQUENCE, &inner);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	make_alg(&alg, "1.3.101.112", NULL);
	blob_init(&inner);
	blob_cat(&inner, &alg);
	blob_tlv(&inner, CRYC_DER_OCTET_STRING, "00aa");
	blob_init(&spki);
	blob_wrap(&spki, CRYC_DER_SEQUENCE, &inner);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&inner);
	blob_cat(&inner, &alg);
	blob_tlv(&inner, CRYC_DER_BIT_STRING, "08aa");
	blob_init(&spki);
	blob_wrap(&spki, CRYC_DER_SEQUENCE, &inner);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&inner);
	blob_cat(&inner, &alg);
	blob_tlv(&inner, CRYC_DER_BIT_STRING, "00aa");
	blob_tlv(&inner, CRYC_DER_BIT_STRING, "00aa");
	blob_init(&spki);
	blob_wrap(&spki, CRYC_DER_SEQUENCE, &inner);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&inner);
	blob_cat(&inner, &alg);
	blob_tlv(&inner, CRYC_DER_BIT_STRING, "00aa");
	blob_init(&spki);
	blob_wrap(&spki, CRYC_DER_SEQUENCE, &inner);
	check_key(&spki, 0, CRYC_KEY_UNSUPPORTED);
	blob_hex(&spki, "00");
	check_key(&spki, EINVAL, CRYC_KEY_NONE);
	blob_init(&inner);
	blob_tlv(&inner, CRYC_DER_SEQUENCE, "0500");
	blob_tlv(&inner, CRYC_DER_BIT_STRING, "00aa");
	blob_init(&spki);
	blob_wrap(&spki, CRYC_DER_SEQUENCE, &inner);
	check_key(&spki, EINVAL, CRYC_KEY_NONE);

	/* Every strict prefix of an RSA and of an EC key is refused. */
	for (index = 0; index < blob_size(&spki_rsa); index++) {
		blob_init(&inner);
		der_raw(&inner.writer, spki_rsa.data, index);
		check_key(&inner, EINVAL, CRYC_KEY_NONE);
	}
	for (index = 0; index < blob_size(&spki_ec256); index++) {
		blob_init(&inner);
		der_raw(&inner.writer, spki_ec256.data, index);
		check_key(&inner, EINVAL, CRYC_KEY_NONE);
	}
}

/* Checks the default certificate field by field. */
static void
test_default_certificate(void)
{
	struct cert_spec spec;
	struct cryc_x509 cert;
	static struct blob built;
	size_t tbs_offset;
	size_t tbs_size;
	static const unsigned char serial[] = { 0x01, 0x02, 0x03, 0x04 };
	static const unsigned char signature[] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
	};
	int error;

	/* Builds the certificate and parses it. */
	spec_default(&spec);
	build_cert(&spec, &built, &tbs_offset, &tbs_size);
	CHECK(!built.writer.error);
	error = parse_blob(&cert, &built);
	CHECK(error == 0);
	if (error != 0)
		return;

	/* The whole certificate and the signed part (the whole tbsCertificate encoding). */
	CHECK(cert.der == held);
	CHECK(cert.der_size == blob_size(&built));
	CHECK(cert.tbs == held + tbs_offset);
	CHECK(cert.tbs_size == tbs_size);
	CHECK(view_byte(cert.tbs, cert.tbs_size, 0) == 0x30);

	/* The algorithm and the signature bytes (the BIT STRING is the unused-bits octet 00 then 15 bytes). */
	CHECK(cert.signature_alg.alg == CRYC_SIG_RSA_PKCS1_SHA256);
	CHECK(cert.signature_alg.salt_size == 0U);
	CHECK(cert.signature_size == sizeof(signature));
	CHECK_VIEW(cert.signature, signature, sizeof(signature));
	CHECK(cert.signature + cert.signature_size == held + held_size);

	/* Version, serial. */
	CHECK(cert.version == 3U);
	CHECK(cert.serial_size == sizeof(serial));
	CHECK_VIEW(cert.serial, serial, sizeof(serial));

	/* The Names are the whole encodings, and they differ so the certificate is not self-issued. */
	CHECK(cert.issuer_size == blob_size(&name_root));
	CHECK_VIEW(cert.issuer, name_root.data, blob_size(&name_root));
	CHECK(cert.subject_size == blob_size(&name_leaf));
	CHECK_VIEW(cert.subject, name_leaf.data, blob_size(&name_leaf));
	CHECK((cert.flags & CRYC_X509_SELF_ISSUED) == 0U);

	/* The validity. */
	CHECK(cert.not_before == TIME_2024);
	CHECK(cert.not_after == TIME_2034);

	/* The key: the whole SubjectPublicKeyInfo, a P-256 point. */
	CHECK(cert.spki_size == blob_size(&spki_ec256));
	CHECK_VIEW(cert.spki, spki_ec256.data, blob_size(&spki_ec256));
	CHECK(cert.key.type == CRYC_KEY_EC);
	CHECK(cert.key.curve == CRYC_CURVE_P256);
	CHECK(cert.key.point_size == 65U);
	CHECK(cert.key.point == cert.spki + cert.spki_size - 65U);

	/* The extensions: basicConstraints CA with pathLen 3, keyUsage keyCertSign and cRLSign, both purposes, one SAN, the identifiers. */
	CHECK((cert.flags & CRYC_X509_HAS_BASIC_CONSTRAINTS) != 0U);
	CHECK((cert.flags & CRYC_X509_CA) != 0U);
	CHECK((cert.flags & CRYC_X509_HAS_PATH_LEN) != 0U);
	CHECK(cert.path_len == 3U);
	CHECK((cert.flags & CRYC_X509_HAS_KEY_USAGE) != 0U);
	CHECK(cert.key_usage == (CRYC_KU_KEY_CERT_SIGN | CRYC_KU_CRL_SIGN));
	CHECK((cert.flags & CRYC_X509_HAS_EXT_KEY_USAGE) != 0U);
	CHECK(cert.ext_key_usage == (CRYC_EKU_SERVER_AUTH | CRYC_EKU_CLIENT_AUTH));
	CHECK((cert.flags & CRYC_X509_HAS_SAN) != 0U);
	CHECK((cert.flags & CRYC_X509_SAN_CRITICAL) == 0U);
	CHECK(cert.san_size == 13U);
	CHECK_VIEW(cert.san, "\x82\x0b" "example.com", 13U);
	CHECK((cert.flags & CRYC_X509_HAS_NAME_CONSTRAINTS) == 0U);
	CHECK((cert.flags & CRYC_X509_UNKNOWN_CRITICAL) == 0U);
	CHECK(cert.permitted == NULL && cert.permitted_size == 0U && cert.excluded == NULL && cert.excluded_size == 0U);
	CHECK(cert.ski_size == 4U);
	CHECK_VIEW(cert.ski, "\xa1\xa2\xa3\xa4", 4U);
	CHECK(cert.aki_size == 4U);
	CHECK_VIEW(cert.aki, "\xb1\xb2\xb3\xb4", 4U);
	CHECK(cert.key.rsa.n == NULL && cert.key.rsa.e == NULL);
	CHECK(cert_views_inside(&cert, held, held_size));
}

/* Checks the version, the unique identifiers and the serial number. */
static void
test_version_and_serial(void)
{
	struct cert_spec spec;
	struct cryc_x509 cert;
	unsigned char expected[CRYC_X509_SERIAL_MAX + 2U];
	char serial[128];
	static const char *const good_serials[] = {
		"01", "00", "7f", "80", "ff", "ff7f", "0080", "00ff", "8000", "ff00", "0102030405060708090a0b0c0d0e0f1011121314",
		NULL
	};
	static const char *const bad_serials[] = {
		"", "0001", "007f", "ff80", "ffff80", "ffff7f", "000000", "0000", "ffff", "ff8000",
		"0102030405060708090a0b0c0d0e0f1011121314151617",
		NULL
	};
	size_t index;
	size_t count;

	/* No version field is v1, explicit 0 is v1 too, 1 is v2, 2 is v3; 3 and larger are refused. */
	spec_default(&spec);
	spec.version = -1;
	spec.extensions = NULL;
	expect_accepted(&spec, &cert);
	CHECK(cert.version == 1U);
	spec.version = 0;
	expect_accepted(&spec, &cert);
	CHECK(cert.version == 1U);
	spec.version = 1;
	expect_accepted(&spec, &cert);
	CHECK(cert.version == 2U);
	spec.version = 2;
	expect_accepted(&spec, &cert);
	CHECK(cert.version == 3U);
	spec.version = 3;
	expect_refused(&spec);
	spec.version = 0x7f;
	expect_refused(&spec);

	/* The [0] holds exactly one INTEGER: another element, two elements, an empty INTEGER, nothing: refused. */
	spec_default(&spec);
	spec.version_raw = "020102";
	expect_accepted(&spec, &cert);
	CHECK(cert.version == 3U);
	spec.version_raw = "0201020500";
	expect_refused(&spec);
	spec.version_raw = "020102020102";
	expect_refused(&spec);
	spec.version_raw = "0200";
	expect_refused(&spec);
	spec.version_raw = "";
	expect_refused(&spec);
	spec.version_raw = "0500";
	expect_refused(&spec);
	spec.version_raw = "02020002";
	expect_refused(&spec);
	spec.version_raw = "0201ff";
	expect_refused(&spec);

	/* Extensions are for v3 only. */
	spec_default(&spec);
	spec.version = -1;
	expect_refused(&spec);
	spec.version = 0;
	expect_refused(&spec);
	spec.version = 1;
	expect_refused(&spec);
	spec.version = 2;
	expect_accepted(&spec, &cert);

	/* Unique identifiers are for v2 and v3: both, one, or neither; swapped order is refused; v1 refuses them. */
	spec_default(&spec);
	spec.uid1 = 1;
	spec.uid2 = 1;
	expect_accepted(&spec, &cert);
	spec.uid2 = 0;
	expect_accepted(&spec, &cert);
	spec.uid1 = 0;
	spec.uid2 = 1;
	expect_accepted(&spec, &cert);
	spec.uid1 = 0;
	spec.uid2 = 0;
	spec.uid_swapped = 1;
	expect_refused(&spec);
	spec.uid_swapped = 0;
	spec.extensions = NULL;
	spec.version = 1;
	spec.uid1 = 1;
	expect_accepted(&spec, &cert);
	spec.version = 0;
	expect_refused(&spec);
	spec.version = -1;
	expect_refused(&spec);
	spec.uid1 = 0;
	spec.uid2 = 1;
	expect_refused(&spec);

	/* A unique identifier is a BIT STRING: unused bits at most 7 and zero, at least the count octet. */
	spec_default(&spec);
	spec.uid1_raw = "01aa";
	expect_accepted(&spec, &cert);
	spec.uid1_raw = "00";
	expect_accepted(&spec, &cert);
	spec.uid1_raw = "08aa";
	expect_refused(&spec);
	spec.uid1_raw = "01ab";
	expect_refused(&spec);
	spec.uid1_raw = "";
	expect_refused(&spec);

	/* Serial numbers: 1 to 21 octets in the minimal form are accepted, negative and zero too. */
	for (index = 0; good_serials[index] != NULL; index++) {
		spec_default(&spec);
		spec.serial = good_serials[index];
		expect_accepted(&spec, &cert);
		count = hex_decode(good_serials[index], expected, sizeof(expected));
		CHECK(cert.serial_size == count);
		CHECK_VIEW(cert.serial, expected, count);
	}
	for (index = 0; bad_serials[index] != NULL; index++) {
		spec_default(&spec);
		spec.serial = bad_serials[index];
		expect_refused(&spec);
	}

	/* The bound: 21 octets (a sign octet and 20) are accepted, 22 refused, 20 octets with the top bit set need the 21st. */
	strcpy(serial, "00");
	for (index = 0; index < 20U; index++)
		strcat(serial, "ff");
	spec_default(&spec);
	spec.serial = serial;
	expect_accepted(&spec, &cert);
	CHECK(cert.serial_size == 21U && view_byte(cert.serial, cert.serial_size, 0) == 0x00 && view_byte(cert.serial, cert.serial_size, 1) == 0xff);
	strcat(serial, "ff");
	expect_refused(&spec);
	strcpy(serial, "7f");
	for (index = 0; index < 20U; index++)
		strcat(serial, "ff");
	expect_accepted(&spec, &cert);
	CHECK(cert.serial_size == 21U && view_byte(cert.serial, cert.serial_size, 0) == 0x7f);
	strcat(serial, "ff");
	expect_refused(&spec);
}

/* Checks the signature algorithm fields and the signature value. */
static void
test_signature_fields(void)
{
	struct cert_spec spec;
	struct cryc_x509 cert;
	struct blob pss;
	struct blob sha1_alg;
	struct blob other_alg;

	/* The algorithm names the certificate's: ECDSA, PSS and the unsupported ones parse. */
	spec_default(&spec);
	spec.tbs_alg = &alg_ecdsa256;
	spec.outer_alg = &alg_ecdsa256;
	expect_accepted(&spec, &cert);
	CHECK(cert.signature_alg.alg == CRYC_SIG_ECDSA_SHA256);
	make_pss(&pss, HEX_SHA384, HEX_MGF384, "020130", NULL);
	spec.tbs_alg = &pss;
	spec.outer_alg = &pss;
	expect_accepted(&spec, &cert);
	CHECK(cert.signature_alg.alg == CRYC_SIG_RSA_PSS_SHA384);
	CHECK(cert.signature_alg.salt_size == 48U);
	make_alg(&sha1_alg, "1.2.840.113549.1.1.5", &part_null);
	spec.tbs_alg = &sha1_alg;
	spec.outer_alg = &sha1_alg;
	expect_accepted(&spec, &cert);
	CHECK(cert.signature_alg.alg == CRYC_SIG_UNSUPPORTED);

	/* The two identifiers must be the same bytes: another algorithm, another parameters encoding. */
	spec_default(&spec);
	spec.tbs_alg = &alg_rsa384;
	expect_refused(&spec);
	spec.tbs_alg = &alg_ecdsa256;
	expect_refused(&spec);
	make_alg(&other_alg, "1.2.840.113549.1.1.11", NULL);
	spec.tbs_alg = &other_alg;
	expect_refused(&spec);
	spec.tbs_alg = &alg_rsa256;
	spec.outer_alg = &other_alg;
	expect_refused(&spec);
	make_alg(&sha1_alg, "1.2.840.113549.1.1.5", &part_null);
	make_alg(&other_alg, "1.2.840.113549.1.1.4", &part_null);
	spec.tbs_alg = &sha1_alg;
	spec.outer_alg = &other_alg;
	expect_refused(&spec);

	/* A malformed identifier in either place is refused. */
	blob_init(&other_alg);
	blob_tlv(&other_alg, CRYC_DER_SEQUENCE, "0500");
	spec_default(&spec);
	spec.outer_alg = &other_alg;
	expect_refused(&spec);
	spec_default(&spec);
	spec.tbs_alg = &other_alg;
	expect_refused(&spec);
	make_alg(&other_alg, "1.2.840.10045.4.3.2", &part_null);
	spec.tbs_alg = &other_alg;
	spec.outer_alg = &other_alg;
	expect_refused(&spec);

	/* The signature: unused bits must be zero; an empty signature is accepted (verification fails it); a BIT STRING with no octet is refused. */
	spec_default(&spec);
	spec.signature = "01ff";
	expect_refused(&spec);
	spec.signature = "0080";
	expect_accepted(&spec, &cert);
	spec.signature = "0780";
	expect_refused(&spec);
	spec.signature = "08ff";
	expect_refused(&spec);
	spec.signature = "00";
	expect_accepted(&spec, &cert);
	CHECK(cert.signature_size == 0U);
	spec.signature = "";
	expect_refused(&spec);
	spec.signature = "00ff";
	expect_accepted(&spec, &cert);
	CHECK(cert.signature_size == 1U && view_byte(cert.signature, cert.signature_size, 0) == 0xff);

	/* Bytes after the Certificate, inside it after the signature, and inside the tbsCertificate after the extensions are refused. */
	spec_default(&spec);
	spec.outer_tail = "00";
	expect_refused(&spec);
	spec.outer_tail = NULL;
	spec.cert_tail = "0500";
	expect_refused(&spec);
	spec.cert_tail = NULL;
	spec.tbs_tail = "0500";
	expect_refused(&spec);
	spec.tbs_tail = "00";
	expect_refused(&spec);
}

/* Checks the issuer, the subject, the validity and the self-issued flag. */
static void
test_names_and_validity(void)
{
	struct cert_spec spec;
	struct cryc_x509 cert;
	struct blob name;
	struct blob validity;
	size_t index;
	const char *atv_ou;
	const char *atv_c;
	const char *rdn_cn;
	const char *bad_names[10];

	/* Issuer equal to subject, byte for byte, is self-issued. */
	spec_default(&spec);
	spec.subject = &name_root;
	expect_accepted(&spec, &cert);
	CHECK((cert.flags & CRYC_X509_SELF_ISSUED) != 0U);
	make_name(&name, "Roop");
	spec.subject = &name;
	expect_accepted(&spec, &cert);
	CHECK((cert.flags & CRYC_X509_SELF_ISSUED) == 0U);
	make_name(&name, "Roots");
	spec.subject = &name;
	expect_accepted(&spec, &cert);
	CHECK((cert.flags & CRYC_X509_SELF_ISSUED) == 0U);

	/* An empty subject is accepted (a critical subjectAltName names the subject); an empty issuer is not. */
	blob_init(&name);
	blob_hex(&name, hx_tlv(0x30, ""));
	spec_default(&spec);
	spec.subject = &name;
	expect_accepted(&spec, &cert);
	CHECK(cert.subject_size == 2U);
	spec.subject = &name_leaf;
	spec.issuer = &name;
	expect_refused(&spec);

	/* A Name with several RDNs and an RDN with several attributes is accepted. */
	atv_c = hx_tlv(0x30, hx_cat("0603550406", hx_tlv(0x13, "5553")));
	atv_ou = hx_tlv(0x30, hx_cat("060355040b", hx_tlv(0x0c, "4f")));
	rdn_cn = hx_tlv(0x31, hx_tlv(0x30, hx_cat(HX_CN, hx_tlv(0x0c, "61"))));
	blob_init(&name);
	blob_hex(&name, hx_tlv(0x30, hx_cat3(hx_tlv(0x31, hx_cat(atv_c, atv_ou)), rdn_cn, rdn_cn)));
	spec_default(&spec);
	spec.issuer = &name;
	expect_accepted(&spec, &cert);
	CHECK(cert.issuer_size == blob_size(&name));
	CHECK_VIEW(cert.issuer, name.data, blob_size(&name));

	/* Malformed Names: an RDN that is not a SET, an empty RDN, a non-SEQUENCE attribute, no value, two values, a type that is not an OID, a bad OID, a truncated value, trailing bytes, not a SEQUENCE. */
	atv_c = hx_cat(HX_CN, hx_tlv(0x0c, "61"));
		bad_names[0] = hx_tlv(0x30, hx_tlv(0x30, hx_tlv(0x30, atv_c)));
		bad_names[1] = hx_tlv(0x30, hx_tlv(0x31, ""));
		bad_names[2] = hx_tlv(0x30, hx_tlv(0x31, hx_tlv(0x0c, "61")));
		bad_names[3] = hx_tlv(0x30, hx_tlv(0x31, hx_tlv(0x30, HX_CN)));
		bad_names[4] = hx_tlv(0x30, hx_tlv(0x31, hx_tlv(0x30, hx_cat(atv_c, hx_tlv(0x0c, "62")))));
		bad_names[5] = hx_tlv(0x30, hx_tlv(0x31, hx_tlv(0x30, hx_cat(hx_tlv(0x0c, "61"), hx_tlv(0x0c, "62")))));
		bad_names[6] = hx_tlv(0x30, hx_tlv(0x31, hx_tlv(0x30, hx_cat("06028001", hx_tlv(0x0c, "61")))));
		bad_names[7] = hx_tlv(0x30, hx_tlv(0x31, hx_tlv(0x30, hx_cat(HX_CN, "0c0561"))));
		bad_names[8] = hx_tlv(0x30, hx_cat(hx_tlv(0x31, hx_tlv(0x30, atv_c)), "00"));
		bad_names[9] = hx_tlv(0x31, hx_tlv(0x31, hx_tlv(0x30, atv_c)));
		for (index = 0; index < 10U; index++) {
			spec_default(&spec);
			blob_init(&name);
			blob_hex(&name, bad_names[index]);
			spec.subject = &name;
			expect_refused(&spec);
			spec.subject = &name_leaf;
			spec.issuer = &name;
			expect_refused(&spec);
		}

	/* The validity: UTCTime for 1950 to 2049, GeneralizedTime from 0000 to 9999, the two kinds mixed; not_before may exceed not_after. */
	make_validity(&validity, CRYC_DER_UTC_TIME, "500101000000Z", CRYC_DER_UTC_TIME, "491231235959Z");
	spec_default(&spec);
	spec.validity = &validity;
	expect_accepted(&spec, &cert);
	CHECK(cert.not_before == TIME_1950);
	CHECK(cert.not_after == TIME_2049_END);
	make_validity(&validity, CRYC_DER_GENERALIZED_TIME, "20340101000000Z", CRYC_DER_GENERALIZED_TIME, "20240101000000Z");
	spec.validity = &validity;
	expect_accepted(&spec, &cert);
	CHECK(cert.not_before == TIME_2034);
	CHECK(cert.not_after == TIME_2024);

	/* Bad validities: a wrong tag, a bad field (month 13, a missing Z), three elements, one element, a trailing byte, not a SEQUENCE, empty, missing. */
	make_validity(&validity, CRYC_DER_INTEGER, "0", CRYC_DER_UTC_TIME, "240101000000Z");
	spec.validity = &validity;
	expect_refused(&spec);
	make_validity(&validity, CRYC_DER_UTC_TIME, "241301000000Z", CRYC_DER_UTC_TIME, "240101000000Z");
	spec.validity = &validity;
	expect_refused(&spec);
	make_validity(&validity, CRYC_DER_UTC_TIME, "240101000000Z", CRYC_DER_UTC_TIME, "240101000000");
	spec.validity = &validity;
	expect_refused(&spec);
	blob_init(&validity);
	blob_hex(&validity, hx_tlv(0x30, hx_cat(hx_tlv(0x17, hx_text("240101000000Z")), hx_tlv(0x17, hx_text("240101000000Z")))));
	spec.validity = &validity;
	expect_accepted(&spec, &cert);
	blob_init(&validity);
	blob_hex(&validity, hx_tlv(0x30, hx_cat3(hx_tlv(0x17, hx_text("240101000000Z")), hx_tlv(0x17, hx_text("240101000000Z")), hx_tlv(0x17, hx_text("240101000000Z")))));
	spec.validity = &validity;
	expect_refused(&spec);
	blob_init(&validity);
	blob_hex(&validity, hx_tlv(0x30, hx_tlv(0x17, hx_text("240101000000Z"))));
	spec.validity = &validity;
	expect_refused(&spec);
	blob_init(&validity);
	blob_hex(&validity, hx_tlv(0x30, hx_cat(hx_cat(hx_tlv(0x17, hx_text("240101000000Z")), hx_tlv(0x17, hx_text("240101000000Z"))), "00")));
	spec.validity = &validity;
	expect_refused(&spec);
	blob_init(&validity);
	blob_hex(&validity, hx_tlv(0x31, ""));
	spec.validity = &validity;
	expect_refused(&spec);
	blob_init(&validity);
	blob_hex(&validity, hx_tlv(0x30, ""));
	spec.validity = &validity;
	expect_refused(&spec);
	spec.validity = NULL;
	expect_refused(&spec);
}

/* Checks basicConstraints. */
static void
test_basic_constraints(void)
{
	struct cryc_x509 cert;
	int error;

	/* cA with and without a path length; the empty SEQUENCE means an end entity. */
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "0101ff"));
	CHECK(error == 0);
	CHECK((cert.flags & (CRYC_X509_HAS_BASIC_CONSTRAINTS | CRYC_X509_CA | CRYC_X509_HAS_PATH_LEN)) == (CRYC_X509_HAS_BASIC_CONSTRAINTS | CRYC_X509_CA));
	error = parse_ext(&cert, "2.5.29.19", 0, hx_tlv(0x30, ""));
	CHECK(error == 0);
	CHECK((cert.flags & (CRYC_X509_HAS_BASIC_CONSTRAINTS | CRYC_X509_CA | CRYC_X509_HAS_PATH_LEN)) == CRYC_X509_HAS_BASIC_CONSTRAINTS);
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "0101ff020100"));
	CHECK(error == 0);
	CHECK((cert.flags & CRYC_X509_HAS_PATH_LEN) != 0U && cert.path_len == 0U);
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "0101ff020200ff"));
	CHECK(error == 0 && cert.path_len == 255U);
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "0101ff02020100"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "0101ff02027fff"));
	CHECK(error == EINVAL);

	/* A path length without cA is refused; so are a negative one, a non-minimal one, and an explicit FALSE (the DEFAULT). */
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "020100"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "010100"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "010100020100"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "0101ff020180"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "0101ff02020001"));
	CHECK(error == EINVAL);

	/* Other malformed values: a BOOLEAN that is neither 00 nor ff, a trailing element, a wrong element, not a SEQUENCE, a trailing byte, an empty value. */
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "010101"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "0101ff0500"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.19", 1, hx_tlv(0x30, "0500"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.19", 1, "0500");
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.19", 1, hx_cat(hx_tlv(0x30, ""), "00"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.19", 1, "");
	CHECK(error == EINVAL);

	/* The flags belong to this extension only. */
	error = parse_ext(&cert, "2.5.29.15", 1, hx_tlv(0x03, "0780"));
	CHECK(error == 0);
	CHECK((cert.flags & (CRYC_X509_HAS_BASIC_CONSTRAINTS | CRYC_X509_CA)) == 0U);
}

/* Checks keyUsage and extKeyUsage. */
static void
test_usages(void)
{
	struct cryc_x509 cert;
	int error;

	/* Bits: KeyUsage bit 0 (digitalSignature) is the top bit of the first octet; bit i is 1 << i in key_usage. */
	error = parse_ext(&cert, "2.5.29.15", 1, hx_tlv(0x03, "0780"));
	CHECK(error == 0 && cert.key_usage == CRYC_KU_DIGITAL_SIGNATURE && (cert.flags & CRYC_X509_HAS_KEY_USAGE) != 0U);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "05a0"));
	CHECK(error == 0 && cert.key_usage == (CRYC_KU_DIGITAL_SIGNATURE | CRYC_KU_KEY_ENCIPHERMENT));
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "0318"));
	CHECK(error == 0 && cert.key_usage == (CRYC_KU_KEY_AGREEMENT | 0x08U));
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "03a8"));
	CHECK(error == 0 && cert.key_usage == (CRYC_KU_DIGITAL_SIGNATURE | CRYC_KU_KEY_ENCIPHERMENT | CRYC_KU_KEY_AGREEMENT));
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "0106"));
	CHECK(error == 0 && cert.key_usage == (CRYC_KU_KEY_CERT_SIGN | CRYC_KU_CRL_SIGN));
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "078000"));
	CHECK(error == 0 && cert.key_usage == 0x001U);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "070080"));
	CHECK(error == 0 && cert.key_usage == 0x100U);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "078080"));
	CHECK(error == 0 && cert.key_usage == 0x101U);

	/* More than nine bits, three octets, no bit set, an empty bit string, the wrong type, trailing bytes, unused bits not zero: refused. */
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "068080"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "008080"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "00808080"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "0000"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "0700"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "00"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, ""));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x04, "0780"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_cat(hx_tlv(0x03, "0780"), "00"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.15", 0, hx_tlv(0x03, "0781"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.15", 0, "");
	CHECK(error == EINVAL);

	/* extKeyUsage: serverAuth, clientAuth, anyExtendedKeyUsage, another purpose, combinations, a repeat. */
	error = parse_ext(&cert, "2.5.29.37", 0, hx_tlv(0x30, HX_SERVER));
	CHECK(error == 0 && cert.ext_key_usage == CRYC_EKU_SERVER_AUTH && (cert.flags & CRYC_X509_HAS_EXT_KEY_USAGE) != 0U);
	error = parse_ext(&cert, "2.5.29.37", 0, hx_tlv(0x30, HX_CLIENT));
	CHECK(error == 0 && cert.ext_key_usage == CRYC_EKU_CLIENT_AUTH);
	error = parse_ext(&cert, "2.5.29.37", 0, hx_tlv(0x30, HX_ANY));
	CHECK(error == 0 && cert.ext_key_usage == CRYC_EKU_ANY);
	error = parse_ext(&cert, "2.5.29.37", 0, hx_tlv(0x30, HX_CODE));
	CHECK(error == 0 && cert.ext_key_usage == CRYC_EKU_OTHER);
	error = parse_ext(&cert, "2.5.29.37", 0, hx_tlv(0x30, hx_cat(HX_CODE, HX_SERVER)));
	CHECK(error == 0 && cert.ext_key_usage == (CRYC_EKU_OTHER | CRYC_EKU_SERVER_AUTH));
	error = parse_ext(&cert, "2.5.29.37", 1, hx_tlv(0x30, hx_cat(HX_SERVER, HX_SERVER)));
	CHECK(error == 0 && cert.ext_key_usage == CRYC_EKU_SERVER_AUTH);
	error = parse_ext(&cert, "2.5.29.37", 0, hx_tlv(0x30, hx_cat3(HX_SERVER, HX_CLIENT, HX_ANY)));
	CHECK(error == 0 && cert.ext_key_usage == (CRYC_EKU_SERVER_AUTH | CRYC_EKU_CLIENT_AUTH | CRYC_EKU_ANY));

	/* Empty, not OIDs, a bad OID, a trailing byte, not a SEQUENCE: refused. */
	error = parse_ext(&cert, "2.5.29.37", 0, hx_tlv(0x30, ""));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.37", 0, hx_tlv(0x30, "0500"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.37", 0, hx_tlv(0x30, "06028001"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.37", 0, hx_tlv(0x30, hx_cat(HX_SERVER, "00")));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.37", 0, HX_SERVER);
	CHECK(error == EINVAL);
}

/* Checks subjectAltName, nameConstraints and the key identifiers. */
static void
test_names_extensions(void)
{
	struct cryc_x509 cert;
	const char *dns;
	const char *address;
	const char *subtree;
	const char *permitted;
	const char *excluded;
	int error;

	/* subjectAltName: one name, several, critical; the view is the SEQUENCE's contents. */
	dns = hx_tlv(0x82, hx_text("a.bc"));
	address = hx_tlv(0x87, "0a000001");
	error = parse_ext(&cert, "2.5.29.17", 0, hx_tlv(0x30, dns));
	CHECK(error == 0 && cert.san_size == 6U && view_byte(cert.san, cert.san_size, 0) == 0x82 && (cert.flags & CRYC_X509_HAS_SAN) != 0U && (cert.flags & CRYC_X509_SAN_CRITICAL) == 0U);
	error = parse_ext(&cert, "2.5.29.17", 1, hx_tlv(0x30, hx_cat(dns, address)));
	CHECK(error == 0 && cert.san_size == 12U && (cert.flags & CRYC_X509_SAN_CRITICAL) != 0U);
	error = parse_ext(&cert, "2.5.29.17", 0, hx_tlv(0x30, ""));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.17", 0, hx_tlv(0x30, "820561"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.17", 0, dns);
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.17", 0, hx_cat(hx_tlv(0x30, dns), "00"));
	CHECK(error == EINVAL);

	/* nameConstraints is processed whether or not it is marked critical. */
	subtree = hx_tlv(0x30, dns);
	permitted = hx_tlv(0xa0, subtree);
	excluded = hx_tlv(0xa1, hx_tlv(0x30, address));
	error = parse_ext(&cert, "2.5.29.30", 0, hx_tlv(0x30, permitted));
	CHECK(error == 0 && (cert.flags & CRYC_X509_HAS_NAME_CONSTRAINTS) != 0U);
	CHECK(cert.permitted_size == 8U && cert.excluded == NULL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, permitted));
	CHECK(error == 0 && (cert.flags & CRYC_X509_HAS_NAME_CONSTRAINTS) != 0U);
	CHECK(cert.permitted_size == 8U && view_byte(cert.permitted, cert.permitted_size, 0) == 0x30 && cert.excluded == NULL && cert.excluded_size == 0U);

	/* Permitted only, excluded only, both; a minimum of 0 is accepted. */
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, excluded));
	CHECK(error == 0 && cert.permitted == NULL && cert.permitted_size == 0U && cert.excluded_size == 8U + 0U);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_cat(permitted, excluded)));
	CHECK(error == 0 && cert.permitted_size == 8U && cert.excluded_size == 8U + 0U);
	CHECK(cert.permitted != cert.excluded);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, hx_tlv(0x30, hx_cat(dns, "800100")))));
	CHECK(error == 0 && cert.permitted_size == 11U);

	/* Neither list, an empty list, a minimum above 0, a maximum, a base that is not context-specific, extra elements, the wrong order, a list item that is not a SEQUENCE. */
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, ""));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, "")));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, hx_tlv(0x30, hx_cat(dns, "800101")))));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, hx_tlv(0x30, hx_cat(dns, "810100")))));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, hx_tlv(0x30, hx_cat3(dns, "800100", "810100")))));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, hx_tlv(0x30, hx_tlv(0x0c, hx_text("a.bc"))))));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, hx_tlv(0x30, hx_tlv(0xc2, hx_text("a.bc"))))));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, hx_tlv(0x30, hx_tlv(0x42, hx_text("a.bc"))))));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, hx_tlv(0x30, hx_tlv(0xa2, hx_text("a.bc"))))));
	CHECK(error == 0);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, hx_tlv(0x30, hx_cat(dns, "0500")))));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_cat(excluded, permitted)));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_cat(permitted, "00")));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, hx_tlv(0x30, "820561"))));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, "0500")));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_tlv(0x30, hx_tlv(0xa0, hx_tlv(0x30, ""))));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.30", 1, hx_cat(hx_tlv(0x30, permitted), "00"));
	CHECK(error == EINVAL);

	/* subjectKeyIdentifier. */
	error = parse_ext(&cert, "2.5.29.14", 0, hx_tlv(0x04, "010203"));
	CHECK(error == 0 && cert.ski_size == 3U && view_byte(cert.ski, cert.ski_size, 0) == 0x01 && cert.aki == NULL);
	error = parse_ext(&cert, "2.5.29.14", 0, hx_tlv(0x04, ""));
	CHECK(error == 0 && cert.ski_size == 0U);
	error = parse_ext(&cert, "2.5.29.14", 0, hx_tlv(0x01, "00"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.14", 0, hx_cat(hx_tlv(0x04, "01"), "00"));
	CHECK(error == EINVAL);

	/* authorityKeyIdentifier: only the key identifier is kept. */
	error = parse_ext(&cert, "2.5.29.35", 0, hx_tlv(0x30, ""));
	CHECK(error == 0 && cert.aki == NULL && cert.aki_size == 0U);
	error = parse_ext(&cert, "2.5.29.35", 0, hx_tlv(0x30, hx_tlv(0x80, "010203")));
	CHECK(error == 0 && cert.aki_size == 3U && view_byte(cert.aki, cert.aki_size, 2) == 0x03);
	error = parse_ext(&cert, "2.5.29.35", 0, hx_tlv(0x30, hx_cat3(hx_tlv(0x80, "09"), hx_tlv(0xa1, dns), hx_tlv(0x82, "07"))));
	CHECK(error == 0 && cert.aki_size == 1U && view_byte(cert.aki, cert.aki_size, 0) == 0x09);
	error = parse_ext(&cert, "2.5.29.35", 0, hx_tlv(0x30, hx_tlv(0xa1, dns)));
	CHECK(error == 0 && cert.aki == NULL);
	error = parse_ext(&cert, "2.5.29.35", 0, hx_tlv(0x30, hx_tlv(0xa1, "820561")));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.35", 0, hx_tlv(0x30, hx_cat(hx_tlv(0x82, "07"), hx_tlv(0x80, "09"))));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.35", 0, hx_tlv(0x30, hx_tlv(0x82, "")));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.35", 0, hx_tlv(0x30, "0500"));
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.35", 0, hx_tlv(0x04, "010203"));
	CHECK(error == EINVAL);
}

/* Checks the list of extensions as a whole: duplicates, criticality, unknown, the count, the framing. */
static void
test_extension_list(void)
{
	struct cert_spec spec;
	struct cryc_x509 cert;
	static struct blob list;
	struct blob extension;
	char oid[40];
	const char *one_extension;
	unsigned index;
	unsigned count;
	int error;
	static const char *const bad_extensions[] = {
		"06022a03",
		"06022a03" "0301aa",
		"06022a03" "0401aa" "0500",
		"06022a03" "010101" "0401aa",
		"06022a03" "0201ff" "0401aa",
		"06022a03" "0401aa" "0101ff",
		"06028001" "0401aa",
		"0401aa",
		NULL
	};

	/* An unknown extension: ignored when not critical, flagged when critical. */
	error = parse_ext(&cert, "1.2.3.4.5", 0, "0500");
	CHECK(error == 0 && (cert.flags & CRYC_X509_UNKNOWN_CRITICAL) == 0U);
	error = parse_ext(&cert, "1.2.3.4.5", 1, "0500");
	CHECK(error == 0 && (cert.flags & CRYC_X509_UNKNOWN_CRITICAL) != 0U);
	error = parse_ext(&cert, "1.2.3.4.5", 1, "");
	CHECK(error == 0 && (cert.flags & CRYC_X509_UNKNOWN_CRITICAL) != 0U);

	/* The policy extensions are recognized and not processed, critical or not, whatever their value. */
	error = parse_ext(&cert, "2.5.29.32", 1, "3000");
	CHECK(error == 0 && (cert.flags & CRYC_X509_UNKNOWN_CRITICAL) == 0U);
	error = parse_ext(&cert, "2.5.29.33", 1, "ff");
	CHECK(error == 0 && (cert.flags & CRYC_X509_UNKNOWN_CRITICAL) == 0U);
	error = parse_ext(&cert, "2.5.29.36", 1, "3000");
	CHECK(error == 0 && (cert.flags & CRYC_X509_UNKNOWN_CRITICAL) == 0U);
	error = parse_ext(&cert, "2.5.29.54", 1, "020100");
	CHECK(error == 0 && (cert.flags & CRYC_X509_UNKNOWN_CRITICAL) == 0U);
	error = parse_ext(&cert, "2.5.29.32", 0, "3000");
	CHECK(error == 0 && (cert.flags & CRYC_X509_UNKNOWN_CRITICAL) == 0U);

	/* An explicit critical FALSE is the DEFAULT and DER leaves it out. */
	error = parse_ext(&cert, "1.2.3.4.5", 2, "0500");
	CHECK(error == EINVAL);
	error = parse_ext(&cert, "2.5.29.14", 2, hx_tlv(0x04, "aa"));
	CHECK(error == EINVAL);

	/* A repeated extension is refused, known or unknown, with other values or flags too. */
	blob_init(&list);
	make_extension(&extension, "2.5.29.14", 0, hx_tlv(0x04, "aa"));
	blob_cat(&list, &extension);
	make_extension(&extension, "2.5.29.15", 0, hx_tlv(0x03, "0780"));
	blob_cat(&list, &extension);
	error = parse_with_extensions(&list, &cert);
	CHECK(error == 0);
	make_extension(&extension, "2.5.29.14", 1, hx_tlv(0x04, "bb"));
	blob_cat(&list, &extension);
	error = parse_with_extensions(&list, &cert);
	CHECK(error == EINVAL);
	blob_init(&list);
	make_extension(&extension, "1.2.3.4.5", 0, "0500");
	blob_cat(&list, &extension);
	make_extension(&extension, "1.2.3.4.5", 0, "0500");
	blob_cat(&list, &extension);
	error = parse_with_extensions(&list, &cert);
	CHECK(error == EINVAL);
	blob_init(&list);
	make_extension(&extension, "1.2.3.4.5", 0, "0500");
	blob_cat(&list, &extension);
	make_extension(&extension, "1.2.3.4.6", 0, "0500");
	blob_cat(&list, &extension);
	make_extension(&extension, "1.2.3.4", 0, "0500");
	blob_cat(&list, &extension);
	error = parse_with_extensions(&list, &cert);
	CHECK(error == 0);

	/* At most 64 extensions are read: from 1 to 64 distinct ones are accepted, 65 and 66 are refused. */
	for (index = 1; index <= 66U; index++) {
		blob_init(&list);
		for (count = 0; count < index; count++) {
			sprintf(oid, "1.2.3.4.%u", count + 1U);
			make_extension(&extension, oid, 0, "0500");
			blob_cat(&list, &extension);
		}
		error = parse_with_extensions(&list, &cert);
		if (index <= 64U)
			CHECK(error == 0);
		else
			CHECK(error == EINVAL);
	}

	/* A list with an empty SEQUENCE is refused: at least one extension. */
	blob_init(&list);
	error = parse_with_extensions(&list, &cert);
	CHECK(error == EINVAL);

	/* Malformed extensions: no value, a value that is not an OCTET STRING, an extra element, a flag that is not TRUE or FALSE, a flag that is not a BOOLEAN, a flag after the value, a bad OID, no OID, not a SEQUENCE. */
	for (index = 0; bad_extensions[index] != NULL; index++) {
		blob_init(&list);
		blob_hex(&list, hx_tlv(0x30, bad_extensions[index]));
		error = parse_with_extensions(&list, &cert);
		CHECK(error == EINVAL);
	}
	blob_init(&list);
	blob_hex(&list, "06022a030401aa");
	error = parse_with_extensions(&list, &cert);
	CHECK(error == EINVAL);

	/* The [3] holds one SEQUENCE and nothing else: two SEQUENCEs, a trailing byte, another element, nothing. */
	spec_default(&spec);
	spec.extensions = NULL;
	one_extension = hx_tlv(0x30, "06022a030401aa");
	spec.extensions_raw = hx_tlv(0x30, one_extension);
	expect_accepted(&spec, &cert);
	CHECK((cert.flags & CRYC_X509_UNKNOWN_CRITICAL) == 0U && cert.ski == NULL);
	spec.extensions_raw = hx_tlv(0x30, "");
	expect_refused(&spec);
	spec.extensions_raw = hx_cat(hx_tlv(0x30, one_extension), "00");
	expect_refused(&spec);
	spec.extensions_raw = hx_cat(hx_tlv(0x30, one_extension), hx_tlv(0x30, one_extension));
	expect_refused(&spec);
	spec.extensions_raw = hx_tlv(0x31, one_extension);
	expect_refused(&spec);
	spec.extensions_raw = one_extension;
	expect_refused(&spec);
	spec.extensions_raw = "";
	expect_refused(&spec);
}

/* Checks a certificate with an RSA key, a P-384 key, an unsupported key and bad keys. */
static void
test_certificate_keys(void)
{
	struct cert_spec spec;
	struct cryc_x509 cert;
	struct blob spki;
	struct blob alg;
	struct blob key_bytes;
	struct blob point;
	unsigned char modulus[256];
	unsigned char exponent[3];
	size_t index;

	/* An RSA key. */
	spec_default(&spec);
	spec.spki = &spki_rsa;
	expect_accepted(&spec, &cert);
	CHECK(cert.key.type == CRYC_KEY_RSA && cert.key.rsa.n_size == 256U && cert.key.rsa.e_size == 3U);
	CHECK(view_byte(cert.key.rsa.n, cert.key.rsa.n_size, 0) == 0x81 && view_byte(cert.key.rsa.e, cert.key.rsa.e_size, 0) == 0x01 && view_byte(cert.key.rsa.e, cert.key.rsa.e_size, 2) == 0x01);
	CHECK(cert.spki_size == blob_size(&spki_rsa));
	CHECK(cert_views_inside(&cert, held, held_size));

	/* A P-384 key. */
	blob_init(&point);
	append_point(&point, 0x04, 96, 5);
	make_spki_ec(&spki, "1.3.132.0.34", &point, 0);
	spec.spki = &spki;
	expect_accepted(&spec, &cert);
	CHECK(cert.key.type == CRYC_KEY_EC && cert.key.curve == CRYC_CURVE_P384 && cert.key.point_size == 97U);

	/* An Ed25519 key parses as unsupported. */
	blob_init(&key_bytes);
	for (index = 0; index < 32U; index++)
		blob_hex(&key_bytes, "5a");
	make_alg(&alg, "1.3.101.112", NULL);
	make_spki(&spki, &alg, &key_bytes, 0);
	expect_accepted(&spec, &cert);
	CHECK(cert.key.type == CRYC_KEY_UNSUPPORTED);

	/* A malformed key makes the certificate malformed. */
	for (index = 0; index < sizeof(modulus); index++)
		modulus[index] = (unsigned char)(0x80U + index);
	exponent[0] = 0;
	exponent[1] = 0;
	exponent[2] = 0;
	make_rsa_public_key(&key_bytes, modulus, sizeof(modulus), exponent, 3);
	make_alg(&alg, "1.2.840.113549.1.1.1", &part_null);
	make_spki(&spki, &alg, &key_bytes, 0);
	expect_refused(&spec);
	blob_init(&spki);
	blob_tlv(&spki, CRYC_DER_SEQUENCE, "0500");
	expect_refused(&spec);
	spec.spki = &name_leaf;
	expect_refused(&spec);
}

/* Checks that every strict prefix of a valid certificate is refused and leaves nothing. */
static void
test_prefixes(void)
{
	struct cert_spec spec;
	struct cryc_x509 cert;
	static struct blob built;
	size_t index;
	int error;

	/* The default certificate with an RSA key and both unique identifiers. */
	spec_default(&spec);
	spec.uid1 = 1;
	spec.uid2 = 1;
	spec.spki = &spki_rsa;
	build_cert(&spec, &built, NULL, NULL);
	CHECK(!built.writer.error);
	for (index = 0; index < blob_size(&built); index++) {
		error = parse_bytes(&cert, built.data, index);
		CHECK(error == EINVAL);
		CHECK(cert_is_zero(&cert));
	}
	error = parse_bytes(&cert, built.data, blob_size(&built));
	CHECK(error == 0);
	CHECK(cert_views_inside(&cert, held, held_size));
}

/* Mutates a valid certificate one byte at a time and at random: no crash, a refusal is zeroed, an acceptance has its views inside. */
static void
test_mutations(void)
{
	struct cert_spec spec;
	struct cryc_x509 cert;
	static struct blob built;
	static struct blob list;
	static unsigned char mutated[BLOB_MAX];
	struct blob extension;
	size_t size;
	size_t index;
	size_t position;
	size_t count;
	unsigned run;
	unsigned mode;
	unsigned changes;
	unsigned char original;
	unsigned char values[4];
	int error;

	/* A certificate with name constraints, a SAN, every extension, an unknown critical one, over RSA with unique identifiers. */
	blob_init(&list);
	blob_cat(&list, &exts_default);
	make_extension(&extension, "2.5.29.30", 1, hx_tlv(0x30, hx_cat(hx_tlv(0xa0, hx_tlv(0x30, hx_tlv(0x82, hx_text("a.bc")))), hx_tlv(0xa1, hx_tlv(0x30, hx_tlv(0x87, "0a000001"))))));
	blob_cat(&list, &extension);
	make_extension(&extension, "1.2.3.4.5", 1, "0500");
	blob_cat(&list, &extension);
	spec_default(&spec);
	spec.extensions = &list;
	spec.spki = &spki_rsa;
	spec.uid1 = 1;
	spec.uid2 = 1;
	build_cert(&spec, &built, NULL, NULL);
	CHECK(!built.writer.error);
	size = blob_size(&built);
	error = parse_blob(&cert, &built);
	CHECK(error == 0);
	CHECK(cert_views_inside(&cert, held, held_size));
	CHECK((cert.flags & CRYC_X509_HAS_NAME_CONSTRAINTS) != 0U && (cert.flags & CRYC_X509_UNKNOWN_CRITICAL) != 0U);

	/* Every byte replaced by four values: 00, ff, and the original with the low bit and with the top bit flipped. */
	for (position = 0; position < size; position++) {
		original = built.data[position];
		values[0] = 0x00;
		values[1] = 0xff;
		values[2] = (unsigned char)(original ^ 0x01U);
		values[3] = (unsigned char)(original ^ 0x80U);
		for (index = 0; index < 4U; index++) {
			memcpy(mutated, built.data, size);
			mutated[position] = values[index];
			error = parse_bytes(&cert, mutated, size);
			if (error == 0) {
				CHECK(cert_views_inside(&cert, held, held_size));
			} else {
				CHECK(error == EINVAL);
				CHECK(cert_is_zero(&cert));
			}
		}
	}

	/* Random mutations: one to three bytes changed, a byte inserted, a byte removed, two bytes nudged. */
	for (run = 0; run < RANDOM_RUNS; run++) {
		memcpy(mutated, built.data, size);
		count = size;
		mode = random32() % 4U;
		if (mode == 0U) {
			for (changes = 0; changes < 1U + random32() % 3U; changes++)
				mutated[random32() % size] = (unsigned char)random32();
		} else if (mode == 1U) {
			position = random32() % size;
			memmove(mutated + position + 1U, mutated + position, size - position);
			mutated[position] = (unsigned char)random32();
			count = size + 1U;
		} else if (mode == 2U) {
			position = random32() % size;
			memmove(mutated + position, mutated + position + 1U, size - position - 1U);
			count = size - 1U;
		} else {
			position = random32() % size;
			mutated[position] = (unsigned char)(mutated[position] + 1U);
			position = random32() % size;
			mutated[position] = (unsigned char)(mutated[position] - 1U);
		}
		error = parse_bytes(&cert, mutated, count);
		if (error == 0) {
			CHECK(cert_views_inside(&cert, held, held_size));
		} else {
			CHECK(error == EINVAL);
			CHECK(cert_is_zero(&cert));
		}
	}
}

/* Returns 1 when a byte string occurs in a buffer (a plain search, independent of the DER reader). */
static int
contains_bytes(
	const unsigned char *haystack,
	size_t haystack_size,
	const unsigned char *needle,
	size_t needle_size)
{
	size_t index;

	/* Every start position. */
	if (needle_size > haystack_size)
		return 0;
	for (index = 0; index + needle_size <= haystack_size; index++) {
		if (memcmp(haystack + index, needle, needle_size) == 0)
			return 1;
	}
	return 0;
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
	*size = fread(data, 1, (size_t)length, stream);
	fclose(stream);
	data[*size] = '\0';
	return data;
}

/* Cross-checks one certificate of the bundle against the plain bytes: its structure offsets and its key kind. */
static void
check_bundle_certificate(
	const struct cryc_x509 *cert,
	const unsigned char *der,
	size_t size)
{
	static const unsigned char rsa_oid[] = { 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
	static const unsigned char ec_oid[] = { 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
	static const unsigned char p256_oid[] = { 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
	static const unsigned char p384_oid[] = { 0x06, 0x05, 0x2b, 0x81, 0x04, 0x00, 0x22 };
	static const unsigned char p521_oid[] = { 0x06, 0x05, 0x2b, 0x81, 0x04, 0x00, 0x23 };
	size_t header;

	/* The Certificate header is 30 and a length of 2 or 3 octets here; the tbs follows it and the signature ends the certificate. */
	CHECK(cert_views_inside(cert, der, size));
	CHECK(cert->der == der && cert->der_size == size);
	header = 4;
	if (der[1] == 0x81)
		header = 3;
	CHECK(cert->tbs == der + header);
	CHECK(cert->signature + cert->signature_size == der + size);

	/* The key kind agrees with a plain search of the encoded key. */
	if (contains_bytes(cert->spki, cert->spki_size, rsa_oid, sizeof(rsa_oid))) {
		CHECK(cert->key.type == CRYC_KEY_RSA);
		CHECK(cert->key.rsa.n_size >= 128U && view_byte(cert->key.rsa.n, cert->key.rsa.n_size, 0) >= 0x80);
	} else if (contains_bytes(cert->spki, cert->spki_size, ec_oid, sizeof(ec_oid))) {
		if (contains_bytes(cert->spki, cert->spki_size, p256_oid, sizeof(p256_oid))) {
			CHECK(cert->key.type == CRYC_KEY_EC && cert->key.curve == CRYC_CURVE_P256);
			CHECK(cert->key.point_size == 65U && view_byte(cert->key.point, cert->key.point_size, 0) == 0x04);
		} else if (contains_bytes(cert->spki, cert->spki_size, p384_oid, sizeof(p384_oid))) {
			CHECK(cert->key.type == CRYC_KEY_EC && cert->key.curve == CRYC_CURVE_P384);
			CHECK(cert->key.point_size == 97U && view_byte(cert->key.point, cert->key.point_size, 0) == 0x04);
		} else {
			CHECK(cert->key.type == CRYC_KEY_UNSUPPORTED);
			if (contains_bytes(cert->spki, cert->spki_size, p521_oid, sizeof(p521_oid)))
				bundle_p521++;
		}
	} else {
		CHECK(cert->key.type == CRYC_KEY_UNSUPPORTED);
	}
}

/* Parses every certificate of the ca-certificates bundle. */
static void
test_bundle(void)
{
	const char *path;
	char *text;
	size_t size;
	size_t position;
	uint8_t *der;
	size_t der_size;
	struct cryc_x509 cert;
	struct cryc_pubkey key;
	unsigned long expected;
	unsigned long parsed;
	unsigned long failed;
	unsigned long self_issued;
	unsigned long v3;
	const char *scan;
	int error;

	/* The bundle must be readable: a missing file is a failure, not a pass. */
	path = getenv("WS335_CACERT");
	if (path == NULL)
		path = CACERT_DEFAULT;
	text = read_file(path, &size);
	CHECK(text != NULL);
	if (text == NULL)
		return;

	/* The number of certificates, counted independently of the PEM reader. */
	expected = 0;
	for (scan = strstr(text, "-----BEGIN CERTIFICATE-----"); scan != NULL;
	     scan = strstr(scan + 1, "-----BEGIN CERTIFICATE-----"))
		expected++;
	CHECK(expected > 100UL);

	/* Every block: decoded, parsed, cross-checked, and its key parsed alone. */
	position = 0;
	parsed = 0;
	failed = 0;
	self_issued = 0;
	v3 = 0;
	for (;;) {
		error = cryc_pem_next_certificate(text, size, &position, &der, &der_size);
		if (error == ENOENT)
			break;
		CHECK(error == 0);
		if (error != 0)
			continue;
		error = cryc_x509_parse(&cert, der, der_size);
		if (error != 0) {
			failed++;
			free(der);
			continue;
		}
		parsed++;
		check_bundle_certificate(&cert, der, der_size);
		if ((cert.flags & CRYC_X509_SELF_ISSUED) != 0U)
			self_issued++;
		if (cert.version == 3U)
			v3++;
		CHECK(cert.not_before < cert.not_after);
		CHECK(cert.signature_alg.alg != CRYC_SIG_NONE);
		CHECK(cert.key.type != CRYC_KEY_NONE);
		error = cryc_x509_public_key(cert.spki, cert.spki_size, &key);
		CHECK(error == 0 && key.type == cert.key.type);
		bundle_certs++;
		if (cert.key.type == CRYC_KEY_RSA)
			bundle_rsa++;
		else if (cert.key.type == CRYC_KEY_EC && cert.key.curve == CRYC_CURVE_P256)
			bundle_ec256++;
		else if (cert.key.type == CRYC_KEY_EC && cert.key.curve == CRYC_CURVE_P384)
			bundle_ec384++;
		else
			bundle_unsupported++;
		free(der);
	}
	free(text);

	/* All of them parse; the bundle holds roots, every one self-issued; the keys outside the range are exactly the P-521 ones. */
	CHECK(parsed == expected);
	CHECK(failed == 0UL);
	CHECK(self_issued == parsed);
	CHECK(v3 > 100UL);
	CHECK(bundle_unsupported == bundle_p521);
	CHECK(bundle_rsa + bundle_ec256 + bundle_ec384 + bundle_unsupported == bundle_certs);
	CHECK(bundle_rsa > 0UL && bundle_ec256 > 0UL && bundle_ec384 > 0UL);
	printf("bundle: %lu certificates parsed (%lu RSA, %lu P-256, %lu P-384, %lu P-521 as unsupported), %lu failed, %lu v3\n", bundle_certs, bundle_rsa, bundle_ec256, bundle_ec384, bundle_p521, failed, v3);
}

/*
 * The x509-limbo cases whose certificates this parser refuses, each for a
 * reason in RFC 5280: a malformed or empty field, a repeated extension, a
 * signature algorithm that differs between tbsCertificate and the
 * Certificate, a serial number of more than 20 octets.  All of them are
 * cases a verifier must reject.  (A nameConstraints that is not critical
 * is read and processed, so those cases parse.)
 */
static const char *const limbo_refused_ids[] = {
	"invalid::invalid-issuer-key",
	"rfc5280::eku::ee-eku-empty",
	"rfc5280::nc::permitted-empty-sequence-excluded-nonempty",
	"rfc5280::nc::permitted-nonempty-excluded-empty-sequence",
	"rfc5280::san::malformed",
	"rfc5280::serial::too-long",
	"rfc5280::ee-empty-issuer",
	"rfc5280::ca-empty-subject",
	"rfc5280::root-inconsistent-ca-extensions",
	"rfc5280::duplicate-extensions",
	"rfc5280::mismatching-signature-algorithm",
	"webpki::nc::intermediate-permitted-excluded-subtrees-both-null",
	"webpki::nc::intermediate-permitted-excluded-subtrees-both-empty-sequences",
	NULL
};

/* How many certificates of each id of limbo_refused_ids were refused. */
static unsigned long limbo_refused_count[14];

/* Parses one limbo certificate (hex in the record): a refusal must be one of the known ones. */
static void
limbo_one(
	const struct vec_file *file,
	const char *key,
	unsigned char *der,
	int expect_success)
{
	struct cryc_x509 cert;
	const char *id;
	size_t size;
	size_t index;
	int error;
	int known;

	/* Absent keys are skipped. */
	size = vec_hex(file, key, der, LIMBO_MAX);
	if (size == (size_t)-1)
		return;
	limbo_certs++;
	if (expect_success)
		limbo_success_certs++;
	error = parse_bytes(&cert, der, size);
	if (error == 0) {
		limbo_parsed++;
		CHECK(cert_views_inside(&cert, held, held_size));
		return;
	}

	/* A refusal leaves nothing, and its case is one of the known ones. */
	CHECK(error == EINVAL);
	CHECK(cert_is_zero(&cert));
	id = vec_get(file, "id");
	known = 0;
	for (index = 0; limbo_refused_ids[index] != NULL; index++) {
		if (strcmp(id, limbo_refused_ids[index]) == 0) {
			known = 1;
			limbo_refused_count[index]++;
		}
	}
	if (!known)
		printf("unexpected refusal in %s (%s)\n", id, key);
	CHECK(known);
	if (expect_success) {
		limbo_success_failed++;
		CHECK(0);
	}
}

/* Parses the certificates (peer, trusted, untrusted) of every x509-limbo case. */
static void
test_limbo(void)
{
	struct vec_file file;
	unsigned char *der;
	unsigned long records;
	unsigned long success_cases;
	size_t index;
	char key[32];
	int expect_success;

	/* The records. */
	der = malloc(LIMBO_MAX);
	CHECK(der != NULL);
	if (der == NULL)
		return;
	if (vec_open(&file, "limbo") != 0) {
		CHECK(0);
		free(der);
		return;
	}
	records = 0;
	success_cases = 0;
	while (vec_next(&file)) {
		records++;
		expect_success = strcmp(vec_get(&file, "expected"), "SUCCESS") == 0;
		if (expect_success)
			success_cases++;
		limbo_one(&file, "peer", der, expect_success);
		for (index = 0; index < 4U; index++) {
			sprintf(key, "trusted.%u", (unsigned)index);
			limbo_one(&file, key, der, expect_success);
		}
		for (index = 0; index < 120U; index++) {
			sprintf(key, "untrusted.%u", (unsigned)index);
			limbo_one(&file, key, der, expect_success);
		}
	}
	vec_close(&file);
	free(der);
	CHECK(records == 9792UL);
	CHECK(success_cases == 930UL);

	/* Every certificate parses but the 13 known ones, one in each case of the list; no certificate of a SUCCESS case is refused. */
	CHECK(limbo_certs == 39936UL);
	CHECK(limbo_certs - limbo_parsed == 13UL);
	for (index = 0; limbo_refused_ids[index] != NULL; index++)
		CHECK(limbo_refused_count[index] == 1UL);
	CHECK(limbo_success_failed == 0UL);
	printf("limbo: %lu certificates, %lu parsed, %lu refused (%lu certificates of SUCCESS cases, %lu of them refused)\n", limbo_certs, limbo_parsed, limbo_certs - limbo_parsed, limbo_success_certs, limbo_success_failed);
}

/* Checks the public keys of a Wycheproof file against the file's own fields. */
static void
test_wycheproof_keys(
	const char *name,
	enum cryc_key_type expected_type,
	enum cryc_curve_id expected_curve,
	unsigned long *seen)
{
	struct vec_file file;
	unsigned char der[OBJECT_MAX];
	unsigned char point[OBJECT_MAX];
	unsigned char asn[OBJECT_MAX];
	unsigned char rebuilt[OBJECT_MAX];
	unsigned char body_bytes[OBJECT_MAX];
	size_t der_size;
	size_t point_size;
	size_t asn_size;
	struct cryc_pubkey key;
	struct der_writer writer;
	struct der_writer body;
	int error;

	/* Every record carries its group's key. */
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return;
	}
	while (vec_next(&file)) {
		der_size = vec_hex(&file, "group.publicKeyDer", der, sizeof(der));
		if (der_size == (size_t)-1)
			continue;
		(*seen)++;
		error = cryc_x509_public_key(held_copy(der, der_size), der_size, &key);
		CHECK(error == 0);
		CHECK(key.type == expected_type);
		if (expected_type == CRYC_KEY_EC) {
			point_size = vec_hex(&file, "group.publicKey.uncompressed", point, sizeof(point));
			CHECK(key.curve == expected_curve);
			CHECK(point_size != (size_t)-1 && key.point_size == point_size);
			if (point_size != (size_t)-1 && key.point_size == point_size)
				CHECK_VIEW(key.point, point, point_size);
		}
		if (expected_type == CRYC_KEY_RSA) {
			/* The RSAPublicKey the file gives, rebuilt from the parsed n and e. */
			asn_size = vec_hex(&file, "group.publicKeyAsn", asn, sizeof(asn));
			CHECK(asn_size != (size_t)-1);
			der_init(&body, body_bytes, sizeof(body_bytes));
			der_unsigned(&body, key.rsa.n, key.rsa.n_size);
			der_unsigned(&body, key.rsa.e, key.rsa.e_size);
			der_init(&writer, rebuilt, sizeof(rebuilt));
			der_wrap(&writer, CRYC_DER_SEQUENCE, &body);
			CHECK(asn_size != (size_t)-1 && writer.used == asn_size);
			if (asn_size != (size_t)-1 && writer.used == asn_size)
				CHECK_BYTES(rebuilt, asn, asn_size);
		}
	}
	vec_close(&file);
}

/* Runs the checks. */
int
main(void)
{
	unsigned long keys;

	/* The prepared parts. */
	prepare_parts();
	CHECK(!part_null.writer.error && !alg_rsa256.writer.error && !alg_rsa384.writer.error && !alg_rsa512.writer.error);
	CHECK(!alg_ecdsa256.writer.error && !name_root.writer.error && !name_leaf.writer.error);
	CHECK(!validity_default.writer.error && !spki_ec256.writer.error && !spki_rsa.writer.error && !exts_default.writer.error);

	/* The unit cases. */
	hx_reset();
	test_signature_algorithms();
	hx_reset();
	test_rsa_keys();
	hx_reset();
	test_ec_keys();
	hx_reset();
	test_other_keys();
	hx_reset();
	test_default_certificate();
	hx_reset();
	test_version_and_serial();
	hx_reset();
	test_signature_fields();
	hx_reset();
	test_names_and_validity();
	hx_reset();
	test_basic_constraints();
	hx_reset();
	test_usages();
	hx_reset();
	test_names_extensions();
	hx_reset();
	test_extension_list();
	hx_reset();
	test_certificate_keys();
	hx_reset();
	test_prefixes();
	hx_reset();
	test_mutations();

	/* The corpora. */
	test_bundle();
	test_limbo();
	keys = 0;
	test_wycheproof_keys("wycheproof-ecdsa_secp256r1_sha256_test", CRYC_KEY_EC, CRYC_CURVE_P256, &keys);
	test_wycheproof_keys("wycheproof-ecdsa_secp256r1_sha512_test", CRYC_KEY_EC, CRYC_CURVE_P256, &keys);
	test_wycheproof_keys("wycheproof-ecdsa_secp384r1_sha384_test", CRYC_KEY_EC, CRYC_CURVE_P384, &keys);
	test_wycheproof_keys("wycheproof-ecdsa_secp384r1_sha512_test", CRYC_KEY_EC, CRYC_CURVE_P384, &keys);
	test_wycheproof_keys("wycheproof-rsa_signature_2048_sha256_test", CRYC_KEY_RSA, CRYC_CURVE_NONE, &keys);
	test_wycheproof_keys("wycheproof-rsa_signature_2048_sha384_test", CRYC_KEY_RSA, CRYC_CURVE_NONE, &keys);
	test_wycheproof_keys("wycheproof-rsa_signature_2048_sha512_test", CRYC_KEY_RSA, CRYC_CURVE_NONE, &keys);
	test_wycheproof_keys("wycheproof-rsa_signature_3072_sha256_test", CRYC_KEY_RSA, CRYC_CURVE_NONE, &keys);
	test_wycheproof_keys("wycheproof-rsa_signature_4096_sha512_test", CRYC_KEY_RSA, CRYC_CURVE_NONE, &keys);
	test_wycheproof_keys("wycheproof-ed25519_test", CRYC_KEY_UNSUPPORTED, CRYC_CURVE_NONE, &keys);
	CHECK(keys > 1000UL);
	printf("wycheproof: %lu keys parsed\n", keys);

	/* Frees the last copy. */
	held_release();
	return check_finish("test-x509");
}
