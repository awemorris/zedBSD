/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the names of certificates (cryc-name.h): the comparison of
 * Names, the normalization of a host name, the matching of a host name
 * and of an IP address against subjectAltName, and name constraints.
 *
 * Every input is built here from RFC 5280 4.1.2.4, 4.2.1.6, 4.2.1.10, 7.1
 * and Appendix A and from RFC 9525 6.3 and 6.4, with the DER writer of
 * derwriter.h; the expected values are written by hand from those rules
 * (the one place that computes an expectation, the random comparisons,
 * does it with a small model written here from the rules and not from the
 * library: labels split into arrays, text folded into a new string).
 * Each condition of each function's comment has cases that must be
 * accepted and cases that must be refused, the comparison functions are
 * run on every prefix and on random and mutated encodings under the
 * sanitizers, and the budget of comparisons is tested at its edge.
 *
 * The x509-limbo cases of the SERVER kind are the outside corpus: the
 * peer name of every case is matched against the peer certificate (every
 * SUCCESS case must match, the others are checked by case where the case
 * says why it fails), and the name constraints of every certificate chain
 * that can be built without a choice are applied (every SUCCESS case must
 * be allowed, and the cases about name constraints must come out as the
 * case says except the ones listed with their reasons).
 */

#include "check.h"
#include "derwriter.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-der.h"
#include "userland/base/libcrypto-compat/cryc-name.h"
#include "userland/base/libcrypto-compat/cryc-x509.h"

/* The room of one built structure in bytes, and how many may be in use at once. */
#define BYTES_MAX	3000U
#define POOL_MAX	5000U

/* The room of the two big constraint lists of the budget test. */
#define BIG_MAX		131072U

/* The most certificates of one x509-limbo case: the peer, four trusted and 120 untrusted. */
#define CHAIN_MAX	125U

/* The largest certificate of the x509-limbo cases is about 78 KB. */
#define LIMBO_MAX	131072U

/* The tags of a GeneralName (RFC 5280 A.1) as the identifier octet, context-specific. */
#define TAG_OTHER	0xa0U
#define TAG_MAIL	0x81U
#define TAG_DNS		0x82U
#define TAG_X400	0xa3U
#define TAG_DIR		0xa4U
#define TAG_EDI		0xa5U
#define TAG_URI		0x86U
#define TAG_IP		0x87U
#define TAG_RID		0x88U

/* The universal tags used for attribute values. */
#define TAG_UTF8	0x0cU
#define TAG_PRINTABLE	0x13U
#define TAG_T61		0x14U
#define TAG_IA5		0x16U
#define TAG_BMP		0x1eU

/* A byte string being built (not a C string). */
struct bytes {
	unsigned char data[BYTES_MAX];
	size_t size;
};

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x2545f491U;

/*
 * The pool of built byte strings.
 *
 * A test function takes a mark, builds from the pool, and puts the mark back, so the pool is
 * empty again between test functions.
 */
static struct bytes pool[POOL_MAX];

/* How many byte strings of the pool are in use; the next one taken is pool[pool_next]. */
static unsigned pool_next;

/* The most exact-size copies that may be in use at once. */
#define EXACT_MAX	8192U

/*
 * The exact-size copies of the views of the certificates under test.
 *
 * A read past the end of a view is then an error of the address sanitizer.  The copies are
 * freed when the mark that preceded them is released.
 */
static unsigned char *exact_pointer[EXACT_MAX];

/* How many entries of exact_pointer are in use. */
static unsigned exact_count;

/* What a test function takes back at its end: the pool of byte strings and the exact-size copies. */
struct mark {
	unsigned pool;
	unsigned exact;
};

/* The permitted list of the budget test, filled by big_subtrees. */
static unsigned char big_permitted[BIG_MAX];

/* The excluded list or the subjectAltName contents of the budget test, filled by big_subtrees or by the DER writer. */
static unsigned char big_san[BIG_MAX];

/* The attribute types: CommonName, countryName, organizationName, organizationalUnitName, emailAddress. */
#define OID_CN		"2.5.4.3"
#define OID_C		"2.5.4.6"
#define OID_O		"2.5.4.10"
#define OID_OU		"2.5.4.11"
#define OID_EMAIL	"1.2.840.113549.1.9.1"

/* Expectations: the check allows (0), refuses (EACCES), or the names or constraints are malformed (EINVAL). */
#define ALLOW		0
#define DENY		EACCES

/* One attribute of the random Names: its type index (0 CN, 1 O), tag index (0 printable, 1 UTF8, 2 IA5) and text. */
struct model_attr {
	unsigned type;
	unsigned tag;
	char text[8];
};

/* One random Name: RDNs of one or two attributes. */
struct model_name {
	unsigned rdn_count;
	unsigned attr_count[3];
	struct model_attr attr[3][2];
};

/* The string tag of each tag index of a model attribute. */
static const unsigned model_tags[3] = { TAG_PRINTABLE, TAG_UTF8, TAG_IA5 };

/* The attribute type of each type index of a model attribute. */
static const char *const model_oids[2] = { OID_CN, OID_O };

/*
 * One x509-limbo case: the certificates as the case lists them (the peer
 * first, then trusted.0 to trusted.3, then untrusted.0 to untrusted.119),
 * each with its DER copy, its parse, whether it parsed and whether it is
 * trusted.  The views of a parse point into the DER copy.
 */
struct limbo_case {
	unsigned char *der[CHAIN_MAX];
	struct cryc_x509 cert[CHAIN_MAX];
	int parsed[CHAIN_MAX];
	int trusted[CHAIN_MAX];
	unsigned count;
};

/* How many x509-limbo cases the run read; counted up by test_limbo and checked at its end. */
static unsigned long limbo_cases;

/* How many cases had a peer name that could be read. */
static unsigned long limbo_peer_names;

/* How many cases expect SUCCESS. */
static unsigned long limbo_success_cases;

/* How many SUCCESS cases had a peer name that matched the peer certificate. */
static unsigned long limbo_success_matched;

/* How many cases that do not expect SUCCESS had a peer name that matched all the same. */
static unsigned long limbo_failure_matched;

/* How many cases had no peer certificate that parsed. */
static unsigned long limbo_unparsed_peer;

/* How many cases were tried for a chain. */
static unsigned long limbo_chains;

/* How many cases had no chain that can be built without a choice. */
static unsigned long limbo_chain_skipped;

/* How many SUCCESS cases had a chain whose name constraints were applied. */
static unsigned long limbo_nc_success_chains;

/* How many cases are about name constraints. */
static unsigned long limbo_nc_cases;

/* How many of those cases came out as the case says. */
static unsigned long limbo_nc_agree;

/* How many of those cases came out otherwise and are not listed with a reason. */
static unsigned long limbo_nc_unexplained;

/* How many of those cases are listed with a reason for coming out otherwise. */
static unsigned long limbo_nc_explained;

/* How many of those cases were refused by the name constraints. */
static unsigned long limbo_nc_refused;

/* How many of those cases were refused by the name only. */
static unsigned long limbo_nc_refused_by_name;

/* One x509-limbo case whose id says whether the peer name matches the peer certificate (1), does not (0), or is malformed (-1). */
struct limbo_name_case {
	const char *id;
	int matches;
};

/*
 * The table of x509-limbo cases whose match is known by id.
 *
 * It holds what a name match gives for the cases where the case itself says why
 * the validation fails: a wrong name, a wildcard that breaks a rule, no
 * subjectAltName.  The ones marked 1 match here because the case fails
 * for a reason this module does not judge: a wildcard on a public suffix
 * (no list is consulted), an underscore (the match accepts it), and the
 * policy of the validation.
 */
static const struct limbo_name_case limbo_name_cases[] = {
	{ "webpki::san::mismatch-domain-san", 0 },
	{ "webpki::san::mismatch-subdomain-san", 0 },
	{ "webpki::san::mismatch-subdomain-apex-san", 0 },
	{ "webpki::san::mismatch-apex-subdomain-san", 0 },
	{ "webpki::san::public-suffix-wildcard-san", 0 },
	{ "webpki::san::public-suffix-multi-label-wildcard-san", 1 },
	{ "webpki::san::public-suffix-private-namespace-wildcard-san", 1 },
	{ "webpki::san::wildcard-embedded-leftmost-san", 0 },
	{ "webpki::san::wildcard-not-in-leftmost-san", 0 },
	{ "webpki::san::wildcard-match-across-labels-san", 0 },
	{ "webpki::san::wildcard-embedded-ulabel-san", 0 },
	{ "webpki::san::unicode-emoji-san", 0 },
	{ "webpki::san::no-san", 0 },
	{ "webpki::san::san-wildcard-only", 0 },
	{ "webpki::san::san-wildcard-only-tld", 0 },
	{ "webpki::san::leftmost-wildcard-san", 1 },
	{ "webpki::san::exact-dns-san", 1 },
	{ "webpki::san::exact-localhost-ip-san", 1 },
	{ "rfc5280::san::underscore-dns", 1 },
	{ "rfc5280::san::ip-in-dns", 0 },
	{ "webpki::cn::ipv4-hex-mismatch", 1 },
	{ "webpki::cn::ipv4-leading-zeros-mismatch", 1 },
	{ "webpki::cn::ipv6-uppercase-mismatch", 1 },
	{ "webpki::cn::ipv6-uncompressed-mismatch", 1 },
	{ "webpki::cn::ipv6-non-rfc5952-mismatch", 1 },
	{ "webpki::cn::punycode-not-in-san", 1 },
	{ "webpki::cn::utf8-vs-punycode-mismatch", 1 },
	{ "webpki::cn::not-in-san", 1 },
	{ "webpki::cn::case-mismatch", 1 },
	{ "webpki::san::san-critical-with-nonempty-subject", 1 },
	{ "rfc5280::san::noncritical-with-empty-subject", 1 },
	{ "rfc5280::san::malformed", -1 },
	{ NULL, 0 }
};

/* One x509-limbo case about name constraints whose outcome this module does not decide, with the result the check gives. */
struct limbo_nc_exception {
	const char *id;
	int check_result;
};

/*
 * The table of the name-constraint cases that come out otherwise.
 *
 * These are the cases (by id) where the constraint check on
 * the chain gives a different answer from the case's expected result: the
 * case fails for a reason that is not a name constraint, or the case
 * expects a rule this check does not apply.  The check's result is the one
 * given here.
 */
static const struct limbo_nc_exception limbo_nc_exceptions[] = {
	{ "rfc5280::nc::permitted-dns-match-noncritical", 0 },
	{ "rfc5280::nc::not-allowed-in-ee-noncritical", 0 },
	{ "rfc5280::nc::not-allowed-in-ee-critical", 0 },
	{ "rfc5280::nc::invalid-dnsname-leading-period", 0 },
	{ NULL, 0 }
};

/* Returns the next pseudo-random 32-bit value. */
static uint32_t rng_next(void);

/* Returns a pseudo-random value below limit. */
static unsigned rng_below(unsigned limit);

/* Takes a fresh byte string from the pool. */
static struct bytes *b_new(void);

/* Takes a mark of the pool and of the exact-size copies. */
static struct mark mark_take(void);

/* Gives back everything taken since the mark. */
static void mark_release(struct mark taken);

/* Copies bytes into a block of exactly their size (a block of one byte for none), kept until the mark is released. */
static unsigned char *exact_copy(const unsigned char *data, size_t size);

/* Builds a byte string from raw bytes. */
static struct bytes *b_raw(const void *data, size_t size);

/* Builds the bytes of a C string, without the NUL. */
static struct bytes *b_text(const char *text);

/* Builds hex text as bytes. */
static struct bytes *b_hex(const char *text);

/* Builds one element: tag, minimal length, contents. */
static struct bytes *b_tlv(unsigned tag, const struct bytes *content);

/* Builds an element with the bytes of a C string as its contents. */
static struct bytes *b_str(unsigned tag, const char *text);

/* Concatenates two byte strings. */
static struct bytes *b_cat(const struct bytes *first, const struct bytes *second);

/* Concatenates three byte strings. */
static struct bytes *b_cat3(const struct bytes *first, const struct bytes *second, const struct bytes *third);

/* Builds an OBJECT IDENTIFIER element from dotted text. */
static struct bytes *b_oid(const char *dotted);

/* Builds an AttributeTypeAndValue: SEQUENCE { OID, string of the given tag }. */
static struct bytes *b_attr(const char *oid, unsigned tag, const char *text);

/* Builds an RDN of one attribute: SET { attribute }. */
static struct bytes *b_rdn(const struct bytes *attribute);

/* Builds a Name from RDNs already concatenated: SEQUENCE { rdns }. */
static struct bytes *b_name(const struct bytes *rdns);

/* The empty Name. */
static struct bytes *n_empty(void);

/* A Name of one RDN with one attribute. */
static struct bytes *n_one(const char *oid, unsigned tag, const char *text);

/* A Name from a short spec: "C=US,O=Org,CN=a b".  Each item is one RDN of one attribute; C is a PrintableString, E an IA5String, the others UTF8Strings. / */
static struct bytes *n_spec(const char *spec);

/* Builds a dNSName GeneralName. */
static struct bytes *g_dns(const char *text);

/* Builds an rfc822Name GeneralName. */
static struct bytes *g_mail(const char *text);

/* Builds a uniformResourceIdentifier GeneralName. */
static struct bytes *g_uri(const char *text);

/* Builds an iPAddress GeneralName of a 4-octet IPv4 address. */
static struct bytes *g_ip4(unsigned a, unsigned b, unsigned c, unsigned d);

/* An iPAddress of 8 octets: an address and a mask (a constraint), or any other length from hex. */
static struct bytes *g_ip_hex(const char *hex);

/* Builds a directoryName GeneralName from a Name. */
static struct bytes *g_dir(const struct bytes *name);

/* A subtree of a base: SEQUENCE { base }. */
static struct bytes *s_sub(const struct bytes *base);

/* Builds a certificate structure with only the fields of this module's functions: subject Name, subjectAltName contents. */
static void make_cert(struct cryc_x509 *cert, const struct bytes *subject, const struct bytes *san);

/* Builds a constraining certificate: the two lists' contents (NULL leaves a list out). */
static void make_ca(struct cryc_x509 *ca, const struct bytes *permitted, const struct bytes *excluded);

/* Runs a name constraints check and returns its result. */
static int nc(const struct bytes *permitted, const struct bytes *excluded, const struct bytes *subject, const struct bytes *san);

/* The names of a SAN of one dNSName with a permitted dNSName constraint, as one call. */
static int nc_dns(const char *permitted, const char *excluded, const char *name);

/* Compares two Names given as raw bytes, each in a block of exactly its size (NULL is passed as NULL). */
static int raw_eq(const unsigned char *left, size_t left_size, const unsigned char *right, size_t right_size);

/* Compares two Names given as byte strings. */
static int name_eq(const struct bytes *left, const struct bytes *right);

/* The model of RFC 5280 7.1 for ASCII text: trim, collapse spaces, lower case, written as a new string. */
static void model_fold(const char *text, char *out);

/* Makes a random Name model. */
static void model_random(struct model_name *model);

/* Builds the DER of a model Name. */
static struct bytes *model_build(const struct model_name *model);

/* The model's answer: are two attributes equal by the rule. */
static int model_attr_equal(const struct model_attr *left, const struct model_attr *right);

/* The model's answer for two Names. */
static int model_equal(const struct model_name *left, const struct model_name *right);

/* Changes a model Name a little: the case of a letter, spaces, the tag between printable and UTF8, or one letter. */
static void model_perturb(struct model_name *model);

/* The hand-written cases of cryc_name_equal. */
static void test_equal_cases(void);

/* Random Names against a model, and reflexivity and symmetry. */
static void test_equal_random(void);

/* Mutated and random encodings: no crash, and equal only to itself-like names. */
static void test_equal_mutations(void);

/* The model of the normalization, written from the rules: returns 1 and the result, or 0 for an error. */
static int model_normalize(const char *host, char *out);

/* Checks one input of the normalization against its expected outcome. */
static void normalize_case(const char *host, const char *expected);

/* Builds a host name of count labels of the given length joined by dots into out. */
static void build_labels(char *out, unsigned count, unsigned length);

/* The cases of cryc_name_host_normalize. */
static void test_normalize(void);

/* Matches a host against a certificate whose SAN has the given GeneralNames. */
static int host_vs(const struct bytes *san, const char *host);

/* Matches a host against one dNSName of a certificate. */
static int dns_vs(const char *presented, const char *host);

/* The model of the matching of one dNSName against a normalized reference, written from RFC 9525 6.3 with labels in arrays. */
static int model_match(const char *presented, const char *reference);

/* The hand-written cases of host matching. */
static void test_match_host(void);

/* The cases of IP address matching. */
static void test_match_ip(void);

/* The cases of dNSName constraints. */
static void test_nc_dns(void);

/* Runs a check with one iPAddress constraint (hex of address and mask) and one name of the given hex. */
static int nc_ip(const char *permitted, const char *excluded, const char *name);

/* The cases of iPAddress constraints. */
static void test_nc_ip(void);

/* Runs a check with one rfc822Name constraint and one mailbox name. */
static int nc_mail(const char *permitted, const char *excluded, const char *name);

/* The cases of rfc822Name constraints and of the emailAddress attribute of the subject. */
static void test_nc_mail(void);

/* The cases of directoryName constraints. */
static void test_nc_directory(void);

/* The forms this module does not evaluate, malformed tags, and the preconditions. */
static void test_nc_other(void);

/* Writes count subtrees "<format with %u>" into dest, then the subtree last when it is not NULL; returns the size. */
static size_t big_subtrees(unsigned char *dest, unsigned count, const char *format, const char *last);

/* The budget of comparisons, tested at its edge. */
static void test_nc_budget(void);

/* Parses a text IP address (IPv4 dotted, or IPv6 with groups and one "::") into out; returns 4, 16, or 0 when it is not one. */
static size_t parse_ip(const char *text, unsigned char *out);

/* Checks the IP parser on the forms the cases use. */
static void test_parse_ip(void);

/* Reads the certificates of one record into a case. */
static void limbo_load(const struct vec_file *file, struct limbo_case *chain);

/* Frees what limbo_load allocated. */
static void limbo_free(struct limbo_case *chain);

/* Builds the chain from the peer up when each step has exactly one certificate whose subject equals the issuer of the one below and the chain ends in a trusted one; writes the indexes, peer first, and returns the length, or 0 when a step has none or has a choice. / */
static unsigned limbo_build_chain(const struct limbo_case *chain, unsigned *order);

/* Applies the name constraints of every certificate of a chain to the certificates below it; returns 0 when all are allowed, else the first error. */
static int limbo_apply_constraints(const struct limbo_case *chain, const unsigned *order, unsigned length);

/* Looks up the expected match of a case id; returns -1 when the id is not in the list. */
static int limbo_name_expected(const char *id);

/* Matches the peer name of a case against the peer certificate; returns 1 or 0, and -1 when the case has no name or it cannot be read. */
static int limbo_match_peer(const struct vec_file *file, const struct limbo_case *chain);

/* Runs the name constraints of the chain of one case and compares with the case. */
static void limbo_check_constraints(const struct vec_file *file, const struct limbo_case *chain, int name_match);

/* Runs the name matching and the name constraints over every SERVER case of x509-limbo. */
static void test_limbo(void);

/*
 * Runs every test of the names of certificates in turn.
 *
 * The exit status is that of check_finish, which prints the result line the test runner reads.
 */
int
main(void)
{
	test_equal_cases();
	test_equal_random();
	test_equal_mutations();
	test_normalize();
	test_match_host();
	test_match_ip();
	test_nc_dns();
	test_nc_ip();
	test_nc_mail();
	test_nc_directory();
	test_nc_other();
	test_nc_budget();
	test_parse_ip();
	test_limbo();
	return check_finish("test-name");
}

/* Returns the next pseudo-random 32-bit value. */
static uint32_t
rng_next(void)
{
	/* Marsaglia's xorshift with the shifts 13, 17 and 5. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;

	/* Reports the new state as the next random value. */
	return rng_state;
}

/* Returns a pseudo-random value below limit. */
static unsigned
rng_below(
	unsigned limit)
{
	unsigned below;

	/* Reduces the next random value into the range. */
	below = (unsigned)(rng_next() % limit);

	/* Reports the random value below the limit. */
	return below;
}

/* Takes a fresh byte string from the pool. */
static struct bytes *
b_new(void)
{
	struct bytes *item;

	/* The pool is sized for the largest test function; running out is a mistake of the test. */
	if (pool_next >= POOL_MAX) {
		printf("FAIL pool exhausted\n");
		exit(EXIT_FAILURE);
	}

	/* Takes the next free byte string and empties it. */
	item = &pool[pool_next++];
	item->size = 0;

	/* Reports the empty byte string. */
	return item;
}

/* Takes a mark of the pool and of the exact-size copies. */
static struct mark
mark_take(void)
{
	struct mark taken;

	/* Records how far the pool and the exact-size copies are used. */
	taken.pool = pool_next;
	taken.exact = exact_count;

	/* Reports the mark. */
	return taken;
}

/* Gives back everything taken since the mark. */
static void
mark_release(
	struct mark taken)
{
	/* Frees the exact-size copies taken since the mark. */
	while (exact_count > taken.exact)
		free(exact_pointer[--exact_count]);

	/* Puts the pool back to where it stood at the mark. */
	pool_next = taken.pool;
}

/* Copies bytes into a block of exactly their size (a block of one byte for none), kept until the mark is released. */
static unsigned char *
exact_copy(
	const unsigned char *data,
	size_t size)
{
	unsigned char *copy;
	size_t room;

	/* The table of copies has a fixed size; running out is a mistake of the test. */
	if (exact_count >= EXACT_MAX) {
		printf("FAIL too many exact copies\n");
		exit(EXIT_FAILURE);
	}

	/* Takes a block of exactly the size, or of one byte when there is nothing to copy. */
	room = size;
	if (size == 0U)
		room = 1U;
	copy = malloc(room);
	if (copy == NULL) {
		printf("FAIL out of memory\n");
		exit(EXIT_FAILURE);
	}

	/* Fills the block when there are bytes to copy. */
	if (size > 0U)
		memcpy(copy, data, size);

	/* Keeps the block until the mark is released. */
	exact_pointer[exact_count++] = copy;

	/* Reports the block. */
	return copy;
}

/* Builds a byte string from raw bytes. */
static struct bytes *
b_raw(
	const void *data,
	size_t size)
{
	struct bytes *item;

	/* Takes a byte string from the pool. */
	item = b_new();

	/* Data over the room of a byte string is a mistake of the test. */
	if (size > BYTES_MAX) {
		printf("FAIL byte string too large\n");
		exit(EXIT_FAILURE);
	}

	/* Copies the bytes in when there are any. */
	if (size > 0U)
		memcpy(item->data, data, size);

	/* Records how many bytes the string holds. */
	item->size = size;

	/* Reports the byte string. */
	return item;
}

/* Builds the bytes of a C string, without the NUL. */
static struct bytes *
b_text(
	const char *text)
{
	struct bytes *item;

	/* Copies the characters of the text, without the NUL, into a byte string. */
	item = b_raw(text, strlen(text));

	/* Reports the byte string. */
	return item;
}

/* Builds hex text as bytes. */
static struct bytes *
b_hex(
	const char *text)
{
	struct bytes *item;
	size_t size;

	/* Takes a byte string from the pool and decodes the text into it. */
	item = b_new();
	size = hex_decode(text, item->data, BYTES_MAX);
	if (size == (size_t)-1) {
		printf("FAIL bad hex %s\n", text);
		exit(EXIT_FAILURE);
	}

	/* Records how many bytes the text decoded to. */
	item->size = size;

	/* Reports the byte string. */
	return item;
}

/* Builds one element: tag, minimal length, contents. */
static struct bytes *
b_tlv(
	unsigned tag,
	const struct bytes *content)
{
	struct bytes *item;
	struct der_writer writer;

	/* Takes a byte string from the pool and writes the element into it. */
	item = b_new();
	der_init(&writer, item->data, BYTES_MAX);
	der_put(&writer, tag, content->data, content->size);
	if (writer.error) {
		printf("FAIL element too large\n");
		exit(EXIT_FAILURE);
	}

	/* Records how many bytes the element takes. */
	item->size = writer.used;

	/* Reports the element. */
	return item;
}

/* Builds an element with the bytes of a C string as its contents. */
static struct bytes *
b_str(
	unsigned tag,
	const char *text)
{
	struct bytes *contents;
	struct bytes *item;

	/* Takes the bytes of the text as the contents of the element. */
	contents = b_text(text);

	/* Wraps the contents in the element of the tag. */
	item = b_tlv(tag, contents);

	/* Reports the element. */
	return item;
}

/* Concatenates two byte strings. */
static struct bytes *
b_cat(
	const struct bytes *first,
	const struct bytes *second)
{
	struct bytes *item;

	/* Two parts that together do not fit are a mistake of the test. */
	if (first->size + second->size > BYTES_MAX) {
		printf("FAIL concatenation too large\n");
		exit(EXIT_FAILURE);
	}

	/* Takes a byte string and fills it with the first part and then the second. */
	item = b_new();
	memcpy(item->data, first->data, first->size);
	memcpy(item->data + first->size, second->data, second->size);
	item->size = first->size + second->size;

	/* Reports the concatenation. */
	return item;
}

/* Concatenates three byte strings. */
static struct bytes *
b_cat3(
	const struct bytes *first,
	const struct bytes *second,
	const struct bytes *third)
{
	struct bytes *pair;
	struct bytes *item;

	/* Joins the first two parts, then the third after them. */
	pair = b_cat(first, second);
	item = b_cat(pair, third);

	/* Reports the concatenation. */
	return item;
}

/* Builds an OBJECT IDENTIFIER element from dotted text. */
static struct bytes *
b_oid(
	const char *dotted)
{
	struct bytes *item;
	struct der_writer writer;

	/* Takes a byte string from the pool and writes the identifier into it. */
	item = b_new();
	der_init(&writer, item->data, BYTES_MAX);
	der_oid(&writer, dotted);
	item->size = writer.used;

	/* Reports the OBJECT IDENTIFIER element. */
	return item;
}

/* Builds an AttributeTypeAndValue: SEQUENCE { OID, string of the given tag }. */
static struct bytes *
b_attr(
	const char *oid,
	unsigned tag,
	const char *text)
{
	struct bytes *type;
	struct bytes *value;
	struct bytes *pair;
	struct bytes *item;

	/* Builds the two parts of the attribute: its type and its value. */
	type = b_oid(oid);
	value = b_str(tag, text);

	/* Joins them and wraps them in the SEQUENCE. */
	pair = b_cat(type, value);
	item = b_tlv(0x30U, pair);

	/* Reports the attribute. */
	return item;
}

/* Builds an RDN of one attribute: SET { attribute }. */
static struct bytes *
b_rdn(
	const struct bytes *attribute)
{
	struct bytes *item;

	/* Wraps the attribute in the SET. */
	item = b_tlv(0x31U, attribute);

	/* Reports the RDN. */
	return item;
}

/* Builds a Name from RDNs already concatenated: SEQUENCE { rdns }. */
static struct bytes *
b_name(
	const struct bytes *rdns)
{
	struct bytes *item;

	/* Wraps the RDNs in the SEQUENCE. */
	item = b_tlv(0x30U, rdns);

	/* Reports the Name. */
	return item;
}

/* The empty Name. */
static struct bytes *
n_empty(void)
{
	struct bytes *none;
	struct bytes *item;

	/* Takes an empty list of RDNs and wraps it as a Name. */
	none = b_new();
	item = b_name(none);

	/* Reports the empty Name. */
	return item;
}

/* A Name of one RDN with one attribute. */
static struct bytes *
n_one(
	const char *oid,
	unsigned tag,
	const char *text)
{
	struct bytes *attribute;
	struct bytes *rdn;
	struct bytes *item;

	/* Builds the attribute, the RDN around it, and the Name around that. */
	attribute = b_attr(oid, tag, text);
	rdn = b_rdn(attribute);
	item = b_name(rdn);

	/* Reports the Name. */
	return item;
}

/*
 * A Name from a short spec: "C=US,O=Org,CN=a b".  Each item is one RDN of
 * one attribute; C is a PrintableString, E an IA5String, the others
 * UTF8Strings.
 */
static struct bytes *
n_spec(
	const char *spec)
{
	struct bytes *rdns;
	struct bytes *attribute;
	struct bytes *rdn;
	struct bytes *item;
	char key[8];
	char value[64];
	const char *oid;
	unsigned tag;
	size_t length;
	int is_country;
	int is_organization;
	int is_unit;
	int is_mail;

	/* Starts with no RDN; each item of the spec adds one. */
	rdns = b_new();
	while (*spec != '\0') {
		/* Cuts the key at the "=". */
		length = 0;
		while (spec[length] != '=')
			length++;
		memcpy(key, spec, length);
		key[length] = '\0';
		spec += length + 1U;

		/* Cuts the value at the next "," or at the end. */
		length = 0;
		while (spec[length] != ',' && spec[length] != '\0')
			length++;
		memcpy(value, spec, length);
		value[length] = '\0';
		spec += length;

		/* Steps over the separator when another item follows. */
		if (*spec == ',')
			spec++;

		/* Looks the key up in the table of the keys of the spec. */
		is_country = strcmp(key, "C");
		is_organization = strcmp(key, "O");
		is_unit = strcmp(key, "OU");
		is_mail = strcmp(key, "E");

		/* Picks the attribute type and the string tag of the key; CommonName and UTF8String are the default. */
		oid = OID_CN;
		tag = TAG_UTF8;
		if (is_country == 0) {
			oid = OID_C;
			tag = TAG_PRINTABLE;
		} else if (is_organization == 0) {
			oid = OID_O;
		} else if (is_unit == 0) {
			oid = OID_OU;
		} else if (is_mail == 0) {
			oid = OID_EMAIL;
			tag = TAG_IA5;
		}

		/* Appends the RDN of the item to the list. */
		attribute = b_attr(oid, tag, value);
		rdn = b_rdn(attribute);
		rdns = b_cat(rdns, rdn);
	}

	/* Wraps the RDNs in the Name. */
	item = b_name(rdns);

	/* Reports the Name. */
	return item;
}

/* Builds a dNSName GeneralName. */
static struct bytes *
g_dns(
	const char *text)
{
	struct bytes *item;

	/* Wraps the text in the GeneralName of the kind. */
	item = b_str(TAG_DNS, text);

	/* Reports the GeneralName. */
	return item;
}

/* Builds an rfc822Name GeneralName. */
static struct bytes *
g_mail(
	const char *text)
{
	struct bytes *item;

	/* Wraps the text in the GeneralName of the kind. */
	item = b_str(TAG_MAIL, text);

	/* Reports the GeneralName. */
	return item;
}

/* Builds a uniformResourceIdentifier GeneralName. */
static struct bytes *
g_uri(
	const char *text)
{
	struct bytes *item;

	/* Wraps the text in the GeneralName of the kind. */
	item = b_str(TAG_URI, text);

	/* Reports the GeneralName. */
	return item;
}

/* Builds an iPAddress GeneralName of a 4-octet IPv4 address. */
static struct bytes *
g_ip4(
	unsigned a,
	unsigned b,
	unsigned c,
	unsigned d)
{
	unsigned char octets[4];
	struct bytes *raw;
	struct bytes *item;

	/* Lays out the four octets of the address. */
	octets[0] = (unsigned char)a;
	octets[1] = (unsigned char)b;
	octets[2] = (unsigned char)c;
	octets[3] = (unsigned char)d;

	/* Wraps them in the iPAddress GeneralName. */
	raw = b_raw(octets, sizeof(octets));
	item = b_tlv(TAG_IP, raw);

	/* Reports the GeneralName. */
	return item;
}

/* An iPAddress of 8 octets: an address and a mask (a constraint), or any other length from hex. */
static struct bytes *
g_ip_hex(
	const char *hex)
{
	struct bytes *octets;
	struct bytes *item;

	/* Decodes the hex text and wraps it in the iPAddress GeneralName. */
	octets = b_hex(hex);
	item = b_tlv(TAG_IP, octets);

	/* Reports the GeneralName. */
	return item;
}

/* Builds a directoryName GeneralName from a Name. */
static struct bytes *
g_dir(
	const struct bytes *name)
{
	struct bytes *item;

	/* Wraps the Name in the directoryName GeneralName. */
	item = b_tlv(TAG_DIR, name);

	/* Reports the GeneralName. */
	return item;
}

/* A subtree of a base: SEQUENCE { base }. */
static struct bytes *
s_sub(
	const struct bytes *base)
{
	struct bytes *item;

	/* Wraps the base in the SEQUENCE of a subtree. */
	item = b_tlv(0x30U, base);

	/* Reports the subtree. */
	return item;
}

/* Builds a certificate structure with only the fields of this module's functions: subject Name, subjectAltName contents. */
static void
make_cert(
	struct cryc_x509 *cert,
	const struct bytes *subject,
	const struct bytes *san)
{
	/* Starts from a certificate with nothing set. */
	memset(cert, 0, sizeof(*cert));

	/* Sets the subject Name when one is given. */
	if (subject != NULL) {
		cert->subject = exact_copy(subject->data, subject->size);
		cert->subject_size = subject->size;
	}

	/* Sets the subjectAltName when one is given. */
	if (san != NULL) {
		cert->flags |= CRYC_X509_HAS_SAN;
		cert->san = exact_copy(san->data, san->size);
		cert->san_size = san->size;
	}
}

/* Builds a constraining certificate: the two lists' contents (NULL leaves a list out). */
static void
make_ca(
	struct cryc_x509 *ca,
	const struct bytes *permitted,
	const struct bytes *excluded)
{
	/* Starts from a certificate that has name constraints and no list. */
	memset(ca, 0, sizeof(*ca));
	ca->flags |= CRYC_X509_HAS_NAME_CONSTRAINTS;

	/* Sets the permitted list when one is given. */
	if (permitted != NULL) {
		ca->permitted = exact_copy(permitted->data, permitted->size);
		ca->permitted_size = permitted->size;
	}

	/* Sets the excluded list when one is given. */
	if (excluded != NULL) {
		ca->excluded = exact_copy(excluded->data, excluded->size);
		ca->excluded_size = excluded->size;
	}
}

/* Runs a name constraints check and returns its result. */
static int
nc(
	const struct bytes *permitted,
	const struct bytes *excluded,
	const struct bytes *subject,
	const struct bytes *san)
{
	struct cryc_x509 ca;
	struct cryc_x509 leaf;
	int first;
	int second;

	/* Builds the constraining certificate and the certificate under test. */
	make_ca(&ca, permitted, excluded);
	make_cert(&leaf, subject, san);

	/* Checks the certificate as the leaf of the chain. */
	first = cryc_name_constraints_check(&ca, &leaf, 1);

	/* Checks it again as a certificate in the middle: is_leaf does not change the result. */
	second = cryc_name_constraints_check(&ca, &leaf, 0);
	CHECK(first == second);

	/* Reports the result of the check. */
	return first;
}

/* The names of a SAN of one dNSName with a permitted dNSName constraint, as one call. */
static int
nc_dns(
	const char *permitted,
	const char *excluded,
	const char *name)
{
	struct bytes *permitted_list;
	struct bytes *excluded_list;
	struct bytes *subject;
	struct bytes *san;
	int result;

	/* Starts with no list. */
	permitted_list = NULL;
	excluded_list = NULL;

	/* Builds the permitted list when a constraint is given. */
	if (permitted != NULL)
		permitted_list = s_sub(g_dns(permitted));

	/* Builds the excluded list when a constraint is given. */
	if (excluded != NULL)
		excluded_list = s_sub(g_dns(excluded));

	/* Runs the check on a certificate with an empty subject and the one name. */
	subject = n_empty();
	san = g_dns(name);
	result = nc(permitted_list, excluded_list, subject, san);

	/* Reports the result of the check. */
	return result;
}

/* Compares two Names given as raw bytes, each in a block of exactly its size (NULL is passed as NULL). */
static int
raw_eq(
	const unsigned char *left,
	size_t left_size,
	const unsigned char *right,
	size_t right_size)
{
	struct mark taken;
	const unsigned char *left_copy;
	const unsigned char *right_copy;
	int result;

	/* Remembers where the copies start, to give them back after the comparison. */
	taken = mark_take();

	/* Gives each Name a block of exactly its size; a NULL stays NULL. */
	left_copy = NULL;
	if (left != NULL)
		left_copy = exact_copy(left, left_size);
	right_copy = NULL;
	if (right != NULL)
		right_copy = exact_copy(right, right_size);

	/* Compares the Names in their blocks. */
	result = cryc_name_equal(left_copy, left_size, right_copy, right_size);

	/* Gives the copies back. */
	mark_release(taken);

	/* Reports the result of the comparison. */
	return result;
}

/* Compares two Names given as byte strings. */
static int
name_eq(
	const struct bytes *left,
	const struct bytes *right)
{
	int result;

	/* Compares the bytes of the two Names. */
	result = raw_eq(left->data, left->size, right->data, right->size);

	/* Reports the result of the comparison. */
	return result;
}

/* ------------------------------------------------------------------ */
/* cryc_name_equal                                                      */
/* ------------------------------------------------------------------ */

/* The model of RFC 5280 7.1 for ASCII text: trim, collapse spaces, lower case, written as a new string. */
static void
model_fold(
	const char *text,
	char *out)
{
	size_t length;
	size_t index;
	size_t used;
	int in_space;
	char letter;

	/* Starts at the beginning of the text, outside a run of spaces. */
	length = strlen(text);
	used = 0;
	in_space = 0;

	/* Copies the text, folding runs of spaces and the case of letters. */
	for (index = 0; index < length; index++) {
		if (text[index] == ' ') {
			/* A space is held back until a letter follows, so the ends are trimmed. */
			in_space = 1;
		} else {
			/* A held space between two words becomes one space. */
			if (in_space && used > 0U)
				out[used++] = ' ';
			in_space = 0;

			/* An upper-case ASCII letter becomes lower case. */
			letter = text[index];
			if (letter >= 'A' && letter <= 'Z')
				letter = (char)(letter + 32);
			out[used++] = letter;
		}
	}

	/* Ends the folded text. */
	out[used] = '\0';
}

/* Makes a random Name model. */
static void
model_random(
	struct model_name *model)
{
	unsigned rdn;
	unsigned attr;
	unsigned length;
	unsigned index;
	static const char alphabet[4] = { 'a', 'A', ' ', 'b' };

	/* Starts from an empty model, then draws the number of RDNs. */
	memset(model, 0, sizeof(*model));
	model->rdn_count = rng_below(4);

	/* Draws the attributes of each RDN. */
	for (rdn = 0; rdn < model->rdn_count; rdn++) {
		model->attr_count[rdn] = 1U + rng_below(2);
		for (attr = 0; attr < model->attr_count[rdn]; attr++) {
			model->attr[rdn][attr].type = rng_below(2);
			model->attr[rdn][attr].tag = rng_below(3);
			length = rng_below(5);
			for (index = 0; index < length; index++)
				model->attr[rdn][attr].text[index] = alphabet[rng_below(4)];
		}
	}
}

/* Builds the DER of a model Name. */
static struct bytes *
model_build(
	const struct model_name *model)
{
	struct bytes *rdns;
	struct bytes *attrs;
	struct bytes *attribute;
	struct bytes *rdn_set;
	struct bytes *item;
	const char *oid;
	unsigned tag;
	unsigned rdn;
	unsigned attr;

	/* Starts with no RDN. */
	rdns = b_new();

	/* Builds each RDN as a SET of its attributes. */
	for (rdn = 0; rdn < model->rdn_count; rdn++) {
		attrs = b_new();
		for (attr = 0; attr < model->attr_count[rdn]; attr++) {
			oid = model_oids[model->attr[rdn][attr].type];
			tag = model_tags[model->attr[rdn][attr].tag];
			attribute = b_attr(oid, tag, model->attr[rdn][attr].text);
			attrs = b_cat(attrs, attribute);
		}

		/* Wraps the attributes in the SET of the RDN and appends it. */
		rdn_set = b_tlv(0x31U, attrs);
		rdns = b_cat(rdns, rdn_set);
	}

	/* Wraps the RDNs in the Name. */
	item = b_name(rdns);

	/* Reports the Name. */
	return item;
}

/* The model's answer: are two attributes equal by the rule. */
static int
model_attr_equal(
	const struct model_attr *left,
	const struct model_attr *right)
{
	char left_folded[16];
	char right_folded[16];
	int order;

	/* Different types are different. */
	if (left->type != right->type)
		return 0;

	/* Printable and UTF8 strings compare folded (the alphabet is ASCII). */
	if (left->tag != 2U && right->tag != 2U) {
		model_fold(left->text, left_folded);
		model_fold(right->text, right_folded);
		order = strcmp(left_folded, right_folded);
		if (order != 0)
			return 0;

		/* Succeeded: the folded texts are the same. */
		return 1;
	}

	/* Anything with an IA5String compares byte for byte, tag included. */
	if (left->tag != right->tag)
		return 0;
	order = strcmp(left->text, right->text);
	if (order != 0)
		return 0;

	/* Succeeded: the tags and the texts are the same. */
	return 1;
}

/* The model's answer for two Names. */
static int
model_equal(
	const struct model_name *left,
	const struct model_name *right)
{
	unsigned rdn;
	unsigned attr;
	int same;

	/* Names with another number of RDNs are different. */
	if (left->rdn_count != right->rdn_count)
		return 0;

	/* Compares the RDNs one by one. */
	for (rdn = 0; rdn < left->rdn_count; rdn++) {
		/* RDNs with another number of attributes are different. */
		if (left->attr_count[rdn] != right->attr_count[rdn])
			return 0;

		/* Compares the attributes of the RDN one by one. */
		for (attr = 0; attr < left->attr_count[rdn]; attr++) {
			same = model_attr_equal(&left->attr[rdn][attr], &right->attr[rdn][attr]);
			if (!same)
				return 0;
		}
	}

	/* Succeeded: every RDN and attribute is equal. */
	return 1;
}

/* Changes a model Name a little: the case of a letter, spaces, the tag between printable and UTF8, or one letter. */
static void
model_perturb(
	struct model_name *model)
{
	unsigned rdn;
	unsigned attr;
	struct model_attr *target;
	size_t length;
	unsigned kind;

	/* Leaves an empty Name as it is. */
	if (model->rdn_count == 0U)
		return;

	/* Picks an attribute and the kind of change. */
	rdn = rng_below(model->rdn_count);
	attr = rng_below(model->attr_count[rdn]);
	target = &model->attr[rdn][attr];
	length = strlen(target->text);
	kind = rng_below(5);

	/* Applies the change of the kind when the attribute allows it. */
	if (kind == 0U && length > 0U) {
		/* Flips the case of one letter. */
		if (target->text[0] == 'a')
			target->text[0] = 'A';
		else if (target->text[0] == 'A')
			target->text[0] = 'a';
	} else if (kind == 1U && length < 6U) {
		/* Adds a space at the end. */
		target->text[length] = ' ';
	} else if (kind == 2U && target->tag < 2U) {
		/* The other of the two folded string types. */
		target->tag = 1U - target->tag;
	} else if (kind == 3U && length > 0U) {
		/* Changes one letter. */
		if (target->text[length - 1U] == 'b')
			target->text[length - 1U] = 'a';
		else
			target->text[length - 1U] = 'b';
	}
}

/* The hand-written cases of cryc_name_equal. */
static void
test_equal_cases(void)
{
	struct mark mark;
	struct bytes *left;
	struct bytes *right;
	struct bytes *both;
	struct bytes *item;
	struct bytes *rdns;
	size_t index;
	unsigned char changed[256];

	/* Remembers the pool and the copies, to give them back at the end. */
	mark = mark_take();

	/* A Name equals itself byte for byte, and two empty Names are equal. */
	left = n_spec("C=US,O=Org,CN=host");
	CHECK(name_eq(left, left) == 1);
	CHECK(name_eq(n_empty(), n_empty()) == 1);
	CHECK(name_eq(n_empty(), left) == 0);
	CHECK(name_eq(left, n_empty()) == 0);

	/* The same text in the three string types: printable and UTF8 compare equal either way, IA5 only with IA5. */
	left = n_one(OID_CN, TAG_PRINTABLE, "abc");
	right = n_one(OID_CN, TAG_UTF8, "abc");
	CHECK(name_eq(left, right) == 1);
	CHECK(name_eq(right, left) == 1);
	CHECK(name_eq(left, n_one(OID_CN, TAG_IA5, "abc")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_IA5, "abc"), right) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_IA5, "abc"), n_one(OID_CN, TAG_IA5, "abc")) == 1);
	CHECK(name_eq(n_one(OID_CN, TAG_IA5, "abc"), n_one(OID_CN, TAG_IA5, "ABC")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_T61, "abc"), n_one(OID_CN, TAG_T61, "abc")) == 1);
	CHECK(name_eq(n_one(OID_CN, TAG_T61, "abc"), n_one(OID_CN, TAG_T61, "ABC")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_T61, "abc"), n_one(OID_CN, TAG_UTF8, "abc")) == 0);

	/* Case folding, ASCII letters only. */
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "Example Corp"), n_one(OID_CN, TAG_PRINTABLE, "EXAMPLE CORP")) == 1);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "a"), n_one(OID_CN, TAG_UTF8, "b")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "az"), n_one(OID_CN, TAG_UTF8, "AZ")) == 1);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "@"), n_one(OID_CN, TAG_UTF8, "`")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "["), n_one(OID_CN, TAG_UTF8, "{")) == 0);

	/* Spaces: trimmed at both ends, runs folded, never removed. */
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "  a   b  "), n_one(OID_CN, TAG_UTF8, "a b")) == 1);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "a b"), n_one(OID_CN, TAG_UTF8, "ab")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "a  b"), n_one(OID_CN, TAG_UTF8, "a b")) == 1);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, " a"), n_one(OID_CN, TAG_UTF8, "a ")) == 1);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "a"), n_one(OID_CN, TAG_UTF8, "a b")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "a b c"), n_one(OID_CN, TAG_UTF8, "a b  d")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "   "), n_one(OID_CN, TAG_UTF8, "")) == 1);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, " "), n_one(OID_CN, TAG_UTF8, "a")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "\t"), n_one(OID_CN, TAG_UTF8, "")) == 0);

	/* A NUL is a character like any other, and the bytes 0x7f and 0x80 are the last ASCII byte and the first other one. */
	left = b_name(b_rdn(b_tlv(0x30U, b_cat(b_oid(OID_CN), b_tlv(TAG_UTF8, b_raw("a\0b", 3))))));
	right = b_name(b_rdn(b_tlv(0x30U, b_cat(b_oid(OID_CN), b_tlv(TAG_UTF8, b_raw("a\0c", 3))))));
	CHECK(name_eq(left, left) == 1);
	CHECK(name_eq(left, right) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "A\x7f"), n_one(OID_CN, TAG_UTF8, "a\x7f")) == 1);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "A\x80"), n_one(OID_CN, TAG_UTF8, "a\x80")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "\x80  a"), n_one(OID_CN, TAG_UTF8, "\x80 a")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "\xe9  a"), n_one(OID_CN, TAG_UTF8, "\xe9 a")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "a  \xe9"), n_one(OID_CN, TAG_UTF8, "a \xe9")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "\xe9 "), n_one(OID_CN, TAG_UTF8, "\xe9")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "a"), n_one(OID_CN, TAG_UTF8, "a\x01")) == 0);

	/* A type that is a prefix of another is another type. */
	CHECK(name_eq(n_one("2.5.4.3", TAG_UTF8, "a"), n_one("2.5.4.3.1", TAG_UTF8, "a")) == 0);
	CHECK(name_eq(n_one("2.5.4.3.1", TAG_UTF8, "a"), n_one("2.5.4.3", TAG_UTF8, "a")) == 0);
	CHECK(name_eq(n_one("2.5.4.3.1", TAG_UTF8, "a"), n_one("2.5.4.3.1", TAG_UTF8, "a")) == 1);

	/* A non-ASCII value compares byte for byte: neither case nor spaces are folded. */
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "\xc3\xa9"), n_one(OID_CN, TAG_UTF8, "\xc3\xa9")) == 1);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "\xc3\xa9"), n_one(OID_CN, TAG_UTF8, "\xc3\x89")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "A\xc3\xa9"), n_one(OID_CN, TAG_UTF8, "a\xc3\xa9")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "a \xc3\xa9"), n_one(OID_CN, TAG_UTF8, "a  \xc3\xa9")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "\xc3\xa9"), n_one(OID_CN, TAG_PRINTABLE, "\xc3\xa9")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_BMP, "ab"), n_one(OID_CN, TAG_BMP, "ab")) == 1);
	CHECK(name_eq(n_one(OID_CN, TAG_BMP, "ab"), n_one(OID_CN, TAG_BMP, "AB")) == 0);

	/* Types compare by OID. */
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "a"), n_one(OID_O, TAG_UTF8, "a")) == 0);
	CHECK(name_eq(n_one(OID_CN, TAG_UTF8, "a"), n_one("2.5.4.30", TAG_UTF8, "a")) == 0);

	/* The RDNs must be the same in number and order. */
	CHECK(name_eq(n_spec("C=US,O=Org"), n_spec("C=US,O=Org")) == 1);
	CHECK(name_eq(n_spec("C=US,O=Org"), n_spec("O=Org,C=US")) == 0);
	CHECK(name_eq(n_spec("C=US,O=Org"), n_spec("C=US")) == 0);
	CHECK(name_eq(n_spec("C=US"), n_spec("C=US,O=Org")) == 0);
	CHECK(name_eq(n_spec("C=US,O=Org,CN=x"), n_spec("C=US,O=Org,CN=y")) == 0);
	CHECK(name_eq(n_spec("C=us,O=org,CN=X"), n_spec("C=US,O=Org,CN=x")) == 1);

	/* An RDN with two attributes: equal in the same order, different in the other, and not equal to one attribute. */
	left = b_name(b_rdn(b_attr(OID_CN, TAG_UTF8, "a")));
	rdns = b_tlv(0x31U, b_cat(b_attr(OID_CN, TAG_UTF8, "a"), b_attr(OID_O, TAG_UTF8, "b")));
	right = b_name(rdns);
	CHECK(name_eq(right, right) == 1);
	CHECK(name_eq(left, right) == 0);
	CHECK(name_eq(right, left) == 0);
	item = b_name(b_tlv(0x31U, b_cat(b_attr(OID_O, TAG_UTF8, "b"), b_attr(OID_CN, TAG_UTF8, "a"))));
	CHECK(name_eq(right, item) == 0);
	item = b_name(b_tlv(0x31U, b_cat(b_attr(OID_CN, TAG_UTF8, "A"), b_attr(OID_O, TAG_UTF8, "B"))));
	CHECK(name_eq(right, item) == 1);

	/* Two RDNs of one attribute are not one RDN of two. */
	item = b_name(b_cat(b_rdn(b_attr(OID_CN, TAG_UTF8, "a")), b_rdn(b_attr(OID_O, TAG_UTF8, "b"))));
	CHECK(name_eq(right, item) == 0);

	/* Malformed encodings are never equal, not even to themselves. */
	left = n_spec("C=US,O=Org");
	CHECK(name_eq(left, left) == 1);
	CHECK(raw_eq(NULL, 0, NULL, 0) == 0);
	CHECK(raw_eq(left->data, left->size, NULL, 0) == 0);
	CHECK(raw_eq(NULL, 0, left->data, left->size) == 0);
	CHECK(raw_eq(left->data, 0, left->data, left->size) == 0);
	CHECK(raw_eq(NULL, 10, left->data, left->size) == 0);
	CHECK(raw_eq(left->data, left->size, NULL, 10) == 0);
	CHECK(raw_eq(left->data, left->size, left->data, left->size - 1U) == 0);
	CHECK(raw_eq(left->data, left->size - 1U, left->data, left->size - 1U) == 0);
	both = b_cat(left, b_hex("00"));
	CHECK(raw_eq(both->data, both->size, both->data, both->size) == 0);
	CHECK(raw_eq(left->data, left->size, both->data, both->size) == 0);
	CHECK(raw_eq(both->data, both->size, left->data, left->size) == 0);

	/* An RDN with no attribute, an attribute with three parts or one part, an OID that is not one, a SET where a SEQUENCE belongs. */
	item = b_name(b_tlv(0x31U, b_new()));
	CHECK(raw_eq(item->data, item->size, item->data, item->size) == 0);
	item = b_name(b_rdn(b_tlv(0x30U, b_cat3(b_oid(OID_CN), b_str(TAG_UTF8, "a"), b_str(TAG_UTF8, "b")))));
	CHECK(raw_eq(item->data, item->size, item->data, item->size) == 0);
	item = b_name(b_rdn(b_tlv(0x30U, b_oid(OID_CN))));
	CHECK(raw_eq(item->data, item->size, item->data, item->size) == 0);
	item = b_name(b_rdn(b_tlv(0x30U, b_cat(b_str(TAG_UTF8, "a"), b_str(TAG_UTF8, "b")))));
	CHECK(raw_eq(item->data, item->size, item->data, item->size) == 0);
	item = b_name(b_rdn(b_tlv(0x30U, b_cat(b_hex("060180"), b_str(TAG_UTF8, "a")))));
	CHECK(raw_eq(item->data, item->size, item->data, item->size) == 0);
	item = b_name(b_tlv(0x30U, b_attr(OID_CN, TAG_UTF8, "a")));
	CHECK(raw_eq(item->data, item->size, item->data, item->size) == 0);
	item = b_name(b_tlv(0x31U, b_rdn(b_attr(OID_CN, TAG_UTF8, "a"))));
	CHECK(raw_eq(item->data, item->size, item->data, item->size) == 0);

	/* The outer element must be a SEQUENCE. */
	item = b_tlv(0x31U, b_rdn(b_attr(OID_CN, TAG_UTF8, "a")));
	CHECK(raw_eq(item->data, item->size, item->data, item->size) == 0);
	item = b_tlv(0x04U, b_new());
	CHECK(raw_eq(item->data, item->size, item->data, item->size) == 0);

	/* Every proper prefix of a Name is malformed, and a Name changed at any byte is either malformed or not equal when the value changed. */
	left = n_spec("C=US,O=Org,CN=host name");
	for (index = 0; index < left->size; index++) {
		CHECK(raw_eq(left->data, index, left->data, left->size) == 0);
		CHECK(raw_eq(left->data, left->size, left->data, index) == 0);
		CHECK(raw_eq(left->data, index, left->data, index) == 0);
	}

	/* A change of one byte of a value that is not a letter or space (the digit 1 to 2) makes the Names different, the same change in a header makes one malformed or different. */
	left = n_one(OID_CN, TAG_UTF8, "host1");
	for (index = 0; index < left->size; index++) {
		memcpy(changed, left->data, left->size);
		changed[index] = (unsigned char)(changed[index] + 1U);
		CHECK(raw_eq(left->data, left->size, changed, left->size) == 0);
		CHECK(raw_eq(changed, left->size, left->data, left->size) == 0);
	}

	/* Gives back everything this test function built. */
	mark_release(mark);
}

/* Random Names against a model, and reflexivity and symmetry. */
static void
test_equal_random(void)
{
	unsigned run;
	struct mark mark;
	struct model_name left;
	struct model_name right;
	struct bytes *left_bytes;
	struct bytes *right_bytes;
	int expected;
	int got;
	int back;
	unsigned long equal_count;
	unsigned mostly;
	unsigned twice;

	/* Counts how many pairs come out equal. */
	equal_count = 0;

	/* Compares random pairs of Names with the model. */
	for (run = 0; run < 30000U; run++) {
		/* Remembers the pool and the copies of this run. */
		mark = mark_take();

		/* Draws a Name and a second one that starts as a copy of it. */
		model_random(&left);
		right = left;
		mostly = rng_below(4);
		if (mostly != 0U) {
			/* Mostly a perturbation of the first, so that equal pairs are common. */
			model_perturb(&right);
			twice = rng_below(2);
			if (twice == 0U)
				model_perturb(&right);
		} else {
			model_random(&right);
		}

		/* Builds both Names, asks the model, and compares them both ways with the library. */
		left_bytes = model_build(&left);
		right_bytes = model_build(&right);
		expected = model_equal(&left, &right);
		got = name_eq(left_bytes, right_bytes);
		back = name_eq(right_bytes, left_bytes);
		CHECK(got == expected);
		CHECK(back == expected);
		CHECK(name_eq(left_bytes, left_bytes) == 1);

		/* Counts the pair when the Names are equal. */
		if (got)
			equal_count++;

		/* Gives back what this run built. */
		mark_release(mark);
	}

	/* Both outcomes were common in the run. */
	CHECK(equal_count > 3000UL);
	CHECK(equal_count < 29000UL);
}

/* Mutated and random encodings: no crash, and equal only to itself-like names. */
static void
test_equal_mutations(void)
{
	unsigned run;
	struct mark mark;
	struct bytes *base;
	unsigned char copy[BYTES_MAX];
	size_t index;
	size_t position;
	size_t length;
	unsigned value;
	unsigned second_change;
	unsigned header;
	int equal;
	int same_letter;
	int same_tag;

	/* Builds the Name that each run changes. */
	base = n_spec("C=US,O=Example Org,CN=www.example.com");

	/* Changes one or two bytes of the Name at random and compares it with the original. */
	for (run = 0; run < 20000U; run++) {
		/* Remembers the pool and the copies of this run. */
		mark = mark_take();

		/* Changes one byte, and in a third of the runs another one. */
		memcpy(copy, base->data, base->size);
		index = rng_below((unsigned)base->size);
		value = rng_below(256);
		copy[index] = (unsigned char)value;
		second_change = rng_below(3);
		if (second_change == 0U)
			copy[rng_below((unsigned)base->size)] = (unsigned char)rng_below(256);

		/* Compares the changed encoding with the original. */
		equal = raw_eq(copy, base->size, base->data, base->size);

		/* A changed encoding that is still equal differs only in the case of letters (the same size, so no space run changed). */
		if (equal == 1) {
			for (position = 0; position < base->size; position++) {
				if (copy[position] != base->data[position]) {
					/* Either the case of a letter, or a string tag turned into the other folded string tag. */
					same_letter = 0;
					if ((copy[position] | 0x20U) == (base->data[position] | 0x20U)) {
						if ((copy[position] | 0x20U) >= 'a' && (copy[position] | 0x20U) <= 'z')
							same_letter = 1;
					}

					/* A string tag turned into the other folded string tag. */
					same_tag = 0;
					if (copy[position] == TAG_PRINTABLE && base->data[position] == TAG_UTF8)
						same_tag = 1;
					else if (copy[position] == TAG_UTF8 && base->data[position] == TAG_PRINTABLE)
						same_tag = 1;
					CHECK(same_letter || same_tag);
				}
			}
		}

		/* The answer is either equal or not equal. */
		CHECK(equal == 0 || equal == 1);

		/* Gives back what this run built. */
		mark_release(mark);
	}

	/* Compares fully random bytes with themselves. */
	for (run = 0; run < 20000U; run++) {
		/* Fills a random number of bytes. */
		length = rng_below(40);
		for (index = 0; index < length; index++)
			copy[index] = (unsigned char)rng_below(256);

		/* Makes a plausible header out of some of the longer inputs: a SEQUENCE whose length covers the rest. */
		if (length > 3U) {
			header = rng_below(2);
			if (header == 0U) {
				copy[0] = 0x30;
				copy[1] = (unsigned char)(length - 2U);
			}
		}

		/* A Name is equal to itself or malformed; either way the answer is 0 or 1. */
		equal = raw_eq(copy, length, copy, length);
		CHECK(equal == 0 || equal == 1);
	}
}

/* ------------------------------------------------------------------ */
/* cryc_name_host_normalize                                             */
/* ------------------------------------------------------------------ */

/* The model of the normalization, written from the rules: returns 1 and the result, or 0 for an error. */
static int
model_normalize(
	const char *host,
	char *out)
{
	size_t length;
	size_t index;
	size_t label;
	char c;
	int valid;

	/* Drops one trailing dot. */
	length = strlen(host);
	if (length > 0U && host[length - 1U] == '.')
		length--;

	/* Refuses an empty name and a name over 253 characters. */
	if (length < 1U || length > 253U)
		return 0;

	/* Copies the name label by label, lower-casing it and refusing what a host name cannot hold. */
	label = 0;
	for (index = 0; index < length; index++) {
		c = host[index];

		/* A dot ends a label; an empty label is refused. */
		if (c == '.') {
			if (label == 0U)
				return 0;
			label = 0;
			out[index] = c;
			continue;
		}

		/* Only letters, digits, hyphen and underscore are allowed in a label. */
		valid = 0;
		if (c >= 'a' && c <= 'z')
			valid = 1;
		else if (c >= 'A' && c <= 'Z')
			valid = 1;
		else if (c >= '0' && c <= '9')
			valid = 1;
		else if (c == '-' || c == '_')
			valid = 1;
		if (!valid)
			return 0;

		/* A label has at most 63 characters. */
		label++;
		if (label > 63U)
			return 0;

		/* Lower-cases the letter and copies it. */
		if (c >= 'A' && c <= 'Z')
			c = (char)(c + 32);
		out[index] = c;
	}

	/* The last label must not be empty. */
	if (label == 0U)
		return 0;

	/* Ends the normalized name. */
	out[length] = '\0';

	/* Succeeded: the host name is valid and out holds its normal form. */
	return 1;
}

/* Checks one input of the normalization against its expected outcome. */
static void
normalize_case(
	const char *host,
	const char *expected)
{
	char out[CRYC_NAME_HOST_MAX + 1U + 8U];
	size_t index;
	int error;

	/* A canary after the room shows a write past it. */
	memset(out, 0x5a, sizeof(out));
	error = cryc_name_host_normalize(host, out);

	/* The canary must be untouched. */
	for (index = CRYC_NAME_HOST_MAX + 1U; index < sizeof(out); index++)
		CHECK((unsigned char)out[index] == 0x5aU);

	/* A refused name leaves an empty text; an accepted one gives the expected text. */
	if (expected == NULL) {
		CHECK(error == EINVAL);
		CHECK(out[0] == '\0');
	} else {
		CHECK(error == 0);
		CHECK(strcmp(out, expected) == 0);
	}
}

/* Builds a host name of count labels of the given length joined by dots into out. */
static void
build_labels(
	char *out,
	unsigned count,
	unsigned length)
{
	unsigned label;
	unsigned index;
	size_t used;

	/* Writes the labels one after the other. */
	used = 0;
	for (label = 0; label < count; label++) {
		/* A dot goes between two labels. */
		if (label > 0U)
			out[used++] = '.';

		/* The letters of one label run through the alphabet. */
		for (index = 0; index < length; index++)
			out[used++] = (char)('a' + (index % 26U));
	}

	/* Ends the text. */
	out[used] = '\0';
}

/* The cases of cryc_name_host_normalize. */
static void
test_normalize(void)
{
	char name[512];
	char expected[512];
	char out[CRYC_NAME_HOST_MAX + 1U];
	unsigned run;
	size_t index;
	size_t length;
	int error;
	int model_ok;
	char model_out[512];
	static const char alphabet[] = { 'a', 'Z', '0', '-', '_', '.', ' ', '*', '\xc3', '@', 'b' };

	/* Plain names, case, a trailing dot. */
	normalize_case("example.com", "example.com");
	normalize_case("Example.COM", "example.com");
	normalize_case("example.com.", "example.com");
	normalize_case("EXAMPLE.COM.", "example.com");
	normalize_case("a", "a");
	normalize_case("A", "a");
	normalize_case("a.", "a");
	normalize_case("xn--n3h.com", "xn--n3h.com");
	normalize_case("foo_bar.example.com", "foo_bar.example.com");
	normalize_case("a-b.c-d", "a-b.c-d");
	normalize_case("-a.b-", "-a.b-");
	normalize_case("1.2.3.4", "1.2.3.4");
	normalize_case("0", "0");
	normalize_case("9", "9");
	normalize_case("a9z.A9Z", "a9z.a9z");
	normalize_case("0123456789abcdefghijklmnopqrstuvwxyz-_", "0123456789abcdefghijklmnopqrstuvwxyz-_");
	normalize_case("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz");
	normalize_case("a.b.c.d.e.f", "a.b.c.d.e.f");

	/* Empty names and empty labels. */
	normalize_case("", NULL);
	normalize_case(".", NULL);
	normalize_case("..", NULL);
	normalize_case(".a", NULL);
	normalize_case("a..b", NULL);
	normalize_case("a.b..", NULL);
	normalize_case("a...", NULL);
	normalize_case(".example.com", NULL);

	/* Characters a host name cannot hold. */
	normalize_case("a b", NULL);
	normalize_case("a*b", NULL);
	normalize_case("*.example.com", NULL);
	normalize_case("*", NULL);
	normalize_case("a@b", NULL);
	normalize_case("a/b", NULL);
	normalize_case("a:b", NULL);
	normalize_case("\xc3\xa9.com", NULL);
	normalize_case("a\x7f", NULL);
	normalize_case("a\t", NULL);
	normalize_case("a\n", NULL);
	normalize_case("a=", NULL);
	normalize_case("a`", NULL);
	normalize_case("a{", NULL);
	normalize_case("a[", NULL);
	normalize_case("a/", NULL);
	normalize_case("a:", NULL);
	normalize_case("a@", NULL);
	normalize_case("a\x60", NULL);

	/* The label limit of 63 and the name limit of 253. */
	build_labels(name, 1, 63);
	normalize_case(name, name);
	build_labels(name, 1, 64);
	normalize_case(name, NULL);
	build_labels(name, 2, 63);
	normalize_case(name, name);
	strcat(name, ".");
	build_labels(expected, 2, 63);
	normalize_case(name, expected);
	build_labels(name, 2, 64);
	normalize_case(name, NULL);

	/* 253 characters: 63 + 1 + 63 + 1 + 63 + 1 + 61 (the last label is shorter). */
	build_labels(name, 3, 63);
	strcat(name, ".");
	strcat(name, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	length = strlen(name);
	CHECK(length == 253U);
	normalize_case(name, name);
	strcpy(expected, name);
	name[length] = '.';
	name[length + 1U] = '\0';
	normalize_case(name, expected);
	CHECK(cryc_name_host_normalize(name, out) == 0);
	CHECK(strlen(out) == 253U);

	/* One character more is over the limit, with or without the dot. */
	name[length] = 'c';
	name[length + 1U] = '\0';
	normalize_case(name, NULL);
	name[length + 1U] = '.';
	name[length + 2U] = '\0';
	normalize_case(name, NULL);
	name[length] = '.';
	name[length + 1U] = '.';
	name[length + 2U] = '\0';
	normalize_case(name, NULL);

	/* A very long name stops at the limit without reading all of it. */
	memset(name, 'a', 400);
	name[400] = '\0';
	normalize_case(name, NULL);
	normalize_case("a.b.c.d.e.f.g.h.i.j.k.l.m.n.o.p.q.r.s.t.u.v.w.x.y.z", "a.b.c.d.e.f.g.h.i.j.k.l.m.n.o.p.q.r.s.t.u.v.w.x.y.z");

	/* Missing arguments. */
	CHECK(cryc_name_host_normalize(NULL, out) == EINVAL);
	CHECK(out[0] == '\0');
	CHECK(cryc_name_host_normalize("a", NULL) == EINVAL);

	/* Normalizing a normal name changes nothing. */
	CHECK(cryc_name_host_normalize("WWW.Example.COM.", out) == 0);
	CHECK(cryc_name_host_normalize(out, name) == 0);
	CHECK(strcmp(out, name) == 0);

	/* Random strings against the model. */
	for (run = 0; run < 40000U; run++) {
		/* Draws a random text of up to 11 characters. */
		length = rng_below(12);
		for (index = 0; index < length; index++)
			name[index] = alphabet[rng_below(sizeof(alphabet))];
		name[length] = '\0';

		/* Asks the model and the library; they must agree on whether the name is valid. */
		model_ok = model_normalize(name, model_out);
		error = cryc_name_host_normalize(name, out);
		CHECK((error == 0) == (model_ok == 1));

		/* A valid name has the normal form of the model. */
		if (model_ok == 1 && error == 0)
			CHECK(strcmp(out, model_out) == 0);

		/* A refused name leaves an empty text. */
		if (error != 0)
			CHECK(out[0] == '\0');
	}
}

/* ------------------------------------------------------------------ */
/* cryc_name_match_host and cryc_name_match_ip                          */
/* ------------------------------------------------------------------ */

/* Matches a host against a certificate whose SAN has the given GeneralNames. */
static int
host_vs(
	const struct bytes *san,
	const char *host)
{
	struct cryc_x509 cert;
	struct bytes *subject;
	int result;

	/* Builds a certificate with an empty subject and the SAN. */
	subject = n_empty();
	make_cert(&cert, subject, san);

	/* Matches the host against it. */
	result = cryc_name_match_host(&cert, host);

	/* Reports the result of the match. */
	return result;
}

/* Matches a host against one dNSName of a certificate. */
static int
dns_vs(
	const char *presented,
	const char *host)
{
	struct bytes *san;
	int result;

	/* Builds a SAN of the one dNSName and matches the host against it. */
	san = g_dns(presented);
	result = host_vs(san, host);

	/* Reports the result of the match. */
	return result;
}

/* The model of the matching of one dNSName against a normalized reference, written from RFC 9525 6.3 with labels in arrays. */
static int
model_match(
	const char *presented,
	const char *reference)
{
	const char *p_label[16];
	size_t p_length[16];
	const char *r_label[16];
	size_t r_length[16];
	unsigned p_count;
	unsigned r_count;
	unsigned index;
	unsigned first_label;
	size_t at;
	size_t start;
	size_t length;
	size_t k;
	char presented_char;
	char reference_char;
	int star;

	/* Splits the presented identifier into labels. */
	p_count = 0;
	start = 0;
	length = strlen(presented);
	for (at = 0; at <= length; at++) {
		/* A label ends at a dot and at the end of the text. */
		if (presented[at] == '.' || presented[at] == '\0') {
			if (p_count >= 16U)
				return 0;
			p_label[p_count] = presented + start;
			p_length[p_count] = at - start;
			p_count++;
			start = at + 1U;
		}
	}

	/* Splits the reference into labels. */
	r_count = 0;
	start = 0;
	length = strlen(reference);
	for (at = 0; at <= length; at++) {
		/* A label ends at a dot and at the end of the text. */
		if (reference[at] == '.' || reference[at] == '\0') {
			if (r_count >= 16U)
				return 0;
			r_label[r_count] = reference + start;
			r_length[r_count] = at - start;
			r_count++;
			start = at + 1U;
		}
	}

	/* Empty labels make the identifier invalid; so does a star anywhere but alone as the first label. */
	star = 0;
	for (index = 0; index < p_count; index++) {
		if (p_length[index] == 0U)
			return 0;
		for (k = 0; k < p_length[index]; k++) {
			if (p_label[index][k] == '*') {
				if (index != 0U || p_length[index] != 1U)
					return 0;
				star = 1;
			}
		}
	}

	/* The label counts must be the same, and a star needs at least three labels. */
	if (p_count != r_count)
		return 0;
	if (star && p_count < 3U)
		return 0;

	/* The star stands for the first label of the reference; the other labels are compared one by one. */
	first_label = 0;
	if (star)
		first_label = 1U;
	for (index = first_label; index < p_count; index++) {
		if (p_length[index] != r_length[index])
			return 0;

		/* Compares the letters of the label ignoring ASCII case. */
		for (k = 0; k < p_length[index]; k++) {
			presented_char = p_label[index][k];
			reference_char = r_label[index][k];
			if (presented_char >= 'A' && presented_char <= 'Z')
				presented_char = (char)(presented_char + 32);
			if (reference_char >= 'A' && reference_char <= 'Z')
				reference_char = (char)(reference_char + 32);
			if (presented_char != reference_char)
				return 0;
		}
	}

	/* Succeeded: the identifier matches the reference. */
	return 1;
}

/* The hand-written cases of host matching. */
static void
test_match_host(void)
{
	struct mark mark;
	struct bytes *san;
	struct cryc_x509 cert;
	char longname[512];
	unsigned run;
	size_t length;
	size_t index;
	char presented[16];
	char reference[16];
	char normal[CRYC_NAME_HOST_MAX + 1U];
	size_t end;
	static const char presented_alphabet[] = { 'a', 'B', '*', '.', '-', '1' };
	static const char reference_alphabet[] = { 'a', 'b', 'c', '.', 'A', '1' };
	unsigned long matches;
	unsigned coin;
	int normalize_error;
	int model_result;

	/* Remembers the pool and the copies, to give them back at the end. */
	mark = mark_take();

	/* Exact names, case ignored on both sides, a trailing dot on the reference. */
	CHECK(dns_vs("example.com", "example.com") == 1);
	CHECK(dns_vs("EXAMPLE.com", "example.COM") == 1);
	CHECK(dns_vs("example.com", "example.com.") == 1);
	CHECK(dns_vs("example.com", "example.org") == 0);
	CHECK(dns_vs("example.com", "xexample.com") == 0);
	CHECK(dns_vs("xexample.com", "example.com") == 0);
	CHECK(dns_vs("example.com", "www.example.com") == 0);
	CHECK(dns_vs("www.example.com", "example.com") == 0);
	CHECK(dns_vs("a.example.com", "b.example.com") == 0);
	CHECK(dns_vs("example.co", "example.com") == 0);
	CHECK(dns_vs("example.com", "example.co") == 0);
	CHECK(dns_vs("a", "a") == 1);
	CHECK(dns_vs("a", "b") == 0);
	CHECK(dns_vs("xn--n3h.example.com", "xn--n3h.example.com") == 1);
	CHECK(dns_vs("foo_bar.example.com", "foo_bar.example.com") == 1);

	/* A wildcard is the whole left-most label, standing for exactly one label. */
	CHECK(dns_vs("*.example.com", "foo.example.com") == 1);
	CHECK(dns_vs("*.example.com", "FOO.EXAMPLE.COM") == 1);
	CHECK(dns_vs("*.EXAMPLE.COM", "foo.example.com") == 1);
	CHECK(dns_vs("*.example.com", "foo.example.com.") == 1);
	CHECK(dns_vs("*.example.com", "xn--bliss-1b3c148a.example.com") == 1);
	CHECK(dns_vs("*.example.com", "example.com") == 0);
	CHECK(dns_vs("*.example.com", "foo.bar.example.com") == 0);
	CHECK(dns_vs("*.example.com", "foo.example.org") == 0);
	CHECK(dns_vs("*.example.com", "foo.xexample.com") == 0);
	CHECK(dns_vs("*.example.com", "foo.example.co") == 0);
	CHECK(dns_vs("*.b.c", "a.b.c") == 1);
	CHECK(dns_vs("*.b.c", "a.b.d") == 0);
	CHECK(dns_vs("*.b.c", "a.d.c") == 0);
	CHECK(dns_vs("*.b.c", "b.c") == 0);
	CHECK(dns_vs("*.b.c", "x.y.b.c") == 0);
	CHECK(dns_vs("*.co.uk", "example.co.uk") == 1);
	CHECK(dns_vs("*.b.c", "a.b.cc") == 0);
	CHECK(dns_vs("*.b.c", "a.b.c.") == 1);
	CHECK(dns_vs("*.example.com", "foo.example.comm") == 0);
	CHECK(dns_vs("*.example.com", "foo.example.co") == 0);
	CHECK(dns_vs("*.example.com", "foo.exampleXcom") == 0);
	CHECK(dns_vs("*.b.c", "a.b.x") == 0);
	CHECK(dns_vs("*.bb.cc", "a.bb.c") == 0);

	/* Fewer than three labels with a wildcard never match. */
	CHECK(dns_vs("*.com", "example.com") == 0);
	CHECK(dns_vs("*", "com") == 0);
	CHECK(dns_vs("*", "example") == 0);
	CHECK(dns_vs("*.b", "a.b") == 0);

	/* A star inside a label, not first, twice, or in a later label makes the identifier invalid. */
	CHECK(dns_vs("f*.example.com", "foo.example.com") == 0);
	CHECK(dns_vs("*o.example.com", "foo.example.com") == 0);
	CHECK(dns_vs("f*o.example.com", "foo.example.com") == 0);
	CHECK(dns_vs("*f.example.com", "xf.example.com") == 0);
	CHECK(dns_vs("foo.*.com", "foo.bar.com") == 0);
	CHECK(dns_vs("foo.*.example.com", "foo.bar.example.com") == 0);
	CHECK(dns_vs("*.*.example.com", "a.b.example.com") == 0);
	CHECK(dns_vs("**.example.com", "a.example.com") == 0);
	CHECK(dns_vs("*.example.*", "a.example.com") == 0);
	CHECK(dns_vs("foo.example.*", "foo.example.com") == 0);
	CHECK(dns_vs("xn--*.example.com", "xn--abc.example.com") == 0);
	CHECK(dns_vs("a.b*.example.com", "a.bc.example.com") == 0);

	/* Empty labels, a leading or trailing dot, and an empty identifier match nothing. */
	CHECK(dns_vs("", "a") == 0);
	CHECK(dns_vs(".", "a") == 0);
	CHECK(dns_vs(".example.com", "example.com") == 0);
	CHECK(dns_vs(".example.com", "a.example.com") == 0);
	CHECK(dns_vs("example.com.", "example.com") == 0);
	CHECK(dns_vs("a..b", "a.b") == 0);
	CHECK(dns_vs("*..example.com", "a.example.com") == 0);
	CHECK(dns_vs("*.", "a") == 0);
	CHECK(dns_vs("*.example..com", "a.example.com") == 0);

	/* Octets outside the host-name set never match a reference (which cannot hold them), and a NUL does not end an identifier. */
	CHECK(dns_vs("\xc3\xa9.example.com", "xn--9ca.example.com") == 0);
	CHECK(dns_vs("\xc3\xa9.example.com", "a.example.com") == 0);
	CHECK(host_vs(b_tlv(TAG_DNS, b_raw("example.com\0.evil.com", 21)), "example.com") == 0);
	CHECK(host_vs(b_tlv(TAG_DNS, b_raw("example.com\0.evil.com", 21)), "example.com.evil.com") == 0);
	CHECK(host_vs(b_tlv(TAG_DNS, b_raw("a\0b", 3)), "a") == 0);

	/* An identifier longer than a host name matches nothing; at the limit it can match. */
	build_labels(longname, 3, 63);
	strcat(longname, ".");
	strcat(longname, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	CHECK(strlen(longname) == 253U);
	CHECK(dns_vs(longname, longname) == 1);
	length = strlen(longname);
	longname[length] = 'b';
	longname[length + 1U] = '\0';
	CHECK(dns_vs(longname, longname) == 0);
	CHECK(dns_vs(longname, "a") == 0);

	/* Several entries: any may match, others are of other kinds or do not match. */
	san = b_cat3(g_dns("one.example.com"), g_ip4(10, 0, 0, 1), g_dns("two.example.com"));
	CHECK(host_vs(san, "one.example.com") == 1);
	CHECK(host_vs(san, "two.example.com") == 1);
	CHECK(host_vs(san, "three.example.com") == 0);
	CHECK(host_vs(san, "10.0.0.1") == 0);
	san = b_cat(g_mail("example.com"), g_uri("example.com"));
	CHECK(host_vs(san, "example.com") == 0);
	san = b_cat(b_tlv(0xa2U, b_text("example.com")), b_str(0x02U, "example.com"));
	CHECK(host_vs(san, "example.com") == 0);
	san = b_str(0x85U, "example.com");
	CHECK(host_vs(san, "example.com") == 0);
	san = b_cat(g_dir(n_spec("CN=example.com")), g_dns("other.example.com"));
	CHECK(host_vs(san, "example.com") == 0);
	CHECK(host_vs(san, "other.example.com") == 1);

	/* The common name is never used: the subject has it, the SAN has not. */
	make_cert(&cert, n_spec("CN=example.com"), g_dns("other.example.com"));
	CHECK(cryc_name_match_host(&cert, "example.com") == 0);
	make_cert(&cert, n_spec("CN=example.com"), NULL);
	CHECK(cryc_name_match_host(&cert, "example.com") == 0);

	/* A certificate without the SAN flag or without bytes matches nothing. */
	make_cert(&cert, n_empty(), g_dns("example.com"));
	cert.flags = 0;
	CHECK(cryc_name_match_host(&cert, "example.com") == 0);
	make_cert(&cert, n_empty(), g_dns("example.com"));
	cert.san = NULL;
	cert.san_size = 0;
	CHECK(cryc_name_match_host(&cert, "example.com") == 0);
	CHECK(cryc_name_match_host(NULL, "example.com") == 0);

	/* A reference that does not normalize matches nothing; an unnormalized one is normalized. */
	make_cert(&cert, n_empty(), g_dns("example.com"));
	CHECK(cryc_name_match_host(&cert, NULL) == 0);
	CHECK(cryc_name_match_host(&cert, "") == 0);
	CHECK(cryc_name_match_host(&cert, "example..com") == 0);
	CHECK(cryc_name_match_host(&cert, "*.example.com") == 0);
	CHECK(cryc_name_match_host(&cert, "Example.COM.") == 1);
	make_cert(&cert, n_empty(), g_dns("*"));
	CHECK(cryc_name_match_host(&cert, "*") == 0);
	make_cert(&cert, n_empty(), g_dns("*.example.com"));
	CHECK(cryc_name_match_host(&cert, "*.example.com") == 0);

	/* A SAN that does not parse matches nothing, even with a match before the damage. */
	san = b_cat(g_dns("example.com"), b_hex("82"));
	CHECK(host_vs(san, "example.com") == 0);
	san = b_cat(g_dns("example.com"), b_hex("8205aa"));
	CHECK(host_vs(san, "example.com") == 0);
	san = b_cat(b_hex("8205aa"), g_dns("example.com"));
	CHECK(host_vs(san, "example.com") == 0);
	san = b_cat(g_dns("example.com"), b_hex("8280"));
	CHECK(host_vs(san, "example.com") == 0);

	/* Every prefix of a SAN with two names: matches only when whole elements remain. */
	san = b_cat(g_dns("a.example.com"), g_dns("b.example.com"));
	for (index = 0; index <= san->size; index++) {
		make_cert(&cert, n_empty(), NULL);
		cert.flags |= CRYC_X509_HAS_SAN;
		cert.san = san->data;
		cert.san_size = index;

		/* The first name is whole when the SAN is cut right after it (15 bytes) or not cut at all. */
		if (index == san->size || index == 15U) {
			CHECK(cryc_name_match_host(&cert, "a.example.com") == 1);
		} else {
			CHECK(cryc_name_match_host(&cert, "a.example.com") == 0);
		}
	}

	/* Random identifiers and references against the model. */
	matches = 0;
	for (run = 0; run < 60000U; run++) {
		/* Gives back what the run before built. */
		mark_release(mark);

		/* Draws a random identifier. */
		length = rng_below(9);
		for (index = 0; index < length; index++)
			presented[index] = presented_alphabet[rng_below(sizeof(presented_alphabet))];
		presented[length] = '\0';

		/* Draws the length of a random reference, and whether to make it from the identifier. */
		length = rng_below(9);
		coin = rng_below(2);
		if (coin == 0U) {
			/* A reference made from the identifier: the star replaced by a label, letters in either case. */
			strcpy(reference, presented);
			if (reference[0] == '*')
				reference[0] = 'c';
			for (index = 0; reference[index] != '\0'; index++) {
				if (reference[index] == 'a') {
					coin = rng_below(2);
					if (coin == 0U)
						reference[index] = 'A';
				}

				/* The star and the capital B become lower-case b. */
				if (reference[index] == '*' || reference[index] == 'B')
					reference[index] = 'b';
			}

			/* Now and then ends the reference with a dot. */
			coin = rng_below(8);
			end = strlen(reference);
			if (coin == 0U && end < 14U) {
				reference[end] = '.';
				reference[end + 1U] = '\0';
			}
		} else {
			/* A reference of random letters. */
			for (index = 0; index < length; index++)
				reference[index] = reference_alphabet[rng_below(sizeof(reference_alphabet))];
			reference[length] = '\0';
		}

		/* Skips a reference that does not normalize; the matching only gets normal references. */
		normalize_error = cryc_name_host_normalize(reference, normal);
		if (normalize_error != 0)
			continue;

		/* The library must give the answer of the model. */
		model_result = model_match(presented, normal);
		CHECK(dns_vs(presented, reference) == model_result);

		/* Counts the pairs that match. */
		if (model_result)
			matches++;
	}

	/* The matches were common in the run. */
	CHECK(matches > 200UL);

	/* Gives back everything this test function built. */
	mark_release(mark);
}

/* The cases of IP address matching. */
static void
test_match_ip(void)
{
	struct mark mark;
	struct cryc_x509 cert;
	struct bytes *san;
	unsigned char v4[4];
	unsigned char v6[16];
	unsigned char v6_other[16];
	size_t index;

	/* Remembers the pool and the copies, to give them back at the end. */
	mark = mark_take();

	/* The IPv4 address 192.0.2.1 and the IPv6 addresses ::1 and ::2 that the cases change. */
	v4[0] = 192;
	v4[1] = 0;
	v4[2] = 2;
	v4[3] = 1;
	memset(v6, 0, sizeof(v6));
	v6[15] = 1;
	memset(v6_other, 0, sizeof(v6_other));
	v6_other[15] = 2;

	/* An IPv4 entry matches the same 4 octets only. */
	make_cert(&cert, n_empty(), g_ip4(192, 0, 2, 1));
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 1);
	for (index = 0; index < 4U; index++) {
		v4[index] = (unsigned char)(v4[index] + 1U);
		CHECK(cryc_name_match_ip(&cert, v4, 4) == 0);
		v4[index] = (unsigned char)(v4[index] - 1U);
	}

	/* The address is back as it was and matches again. */
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 1);

	/* An IPv6 entry matches the same 16 octets only. */
	make_cert(&cert, n_empty(), b_tlv(TAG_IP, b_raw(v6, 16)));
	CHECK(cryc_name_match_ip(&cert, v6, 16) == 1);
	CHECK(cryc_name_match_ip(&cert, v6_other, 16) == 0);
	for (index = 0; index < 16U; index++) {
		v6[index] = (unsigned char)(v6[index] ^ 0x80U);
		CHECK(cryc_name_match_ip(&cert, v6, 16) == 0);
		v6[index] = (unsigned char)(v6[index] ^ 0x80U);
	}

	/* The address is back as it was and matches again. */
	CHECK(cryc_name_match_ip(&cert, v6, 16) == 1);

	/* The families and sizes do not mix: ::ffff:192.0.2.1 is not 192.0.2.1, and a 4-octet prefix of an IPv6 entry is not a match. */
	make_cert(&cert, n_empty(), g_ip4(0, 0, 0, 1));
	CHECK(cryc_name_match_ip(&cert, v6, 16) == 0);
	CHECK(cryc_name_match_ip(&cert, v6, 4) == 0);
	make_cert(&cert, n_empty(), b_tlv(TAG_IP, b_raw(v6, 16)));
	v4[0] = 0;
	v4[1] = 0;
	v4[2] = 0;
	v4[3] = 0;
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 0);
	CHECK(cryc_name_match_ip(&cert, v6, 4) == 0);
	CHECK(cryc_name_match_ip(&cert, v6, 8) == 0);
	CHECK(cryc_name_match_ip(&cert, v6, 15) == 0);
	CHECK(cryc_name_match_ip(&cert, v6, 0) == 0);

	/* Only 4 and 16 octets are addresses: the same octets given as another size match nothing. */
	make_cert(&cert, n_empty(), g_ip_hex("c0000200ffffff00"));
	v6[0] = 0xc0;
	v6[1] = 0;
	v6[2] = 2;
	v6[3] = 0;
	v6[4] = 0xff;
	v6[5] = 0xff;
	v6[6] = 0xff;
	v6[7] = 0;
	CHECK(cryc_name_match_ip(&cert, v6, 8) == 0);
	make_cert(&cert, n_empty(), g_ip_hex(""));
	CHECK(cryc_name_match_ip(&cert, v6, 0) == 0);
	make_cert(&cert, n_empty(), g_ip_hex("0a"));
	CHECK(cryc_name_match_ip(&cert, v6, 1) == 0);
	make_cert(&cert, n_empty(), g_ip_hex("0a0000000000000000000000000000000000"));
	CHECK(cryc_name_match_ip(&cert, v6, 18) == 0);
	CHECK(cryc_name_match_ip(&cert, v6, 17) == 0);
	v6[0] = 0;
	v6[1] = 0;
	v6[2] = 0;
	v6[3] = 0;
	v6[4] = 0;
	v6[5] = 0;
	v6[6] = 0;
	v6[7] = 0;
	v6[15] = 1;

	/* An entry of 8 or 32 octets (a constraint's form) is not an address. */
	make_cert(&cert, n_empty(), g_ip_hex("c0000200ffffff00"));
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 0);
	v4[0] = 0xc0;
	v4[2] = 2;
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 0);

	/* Several entries, of several kinds. */
	san = b_cat3(g_dns("192.0.2.1"), g_ip4(10, 1, 1, 1), b_tlv(TAG_IP, b_raw(v6, 16)));
	make_cert(&cert, n_empty(), san);
	v4[0] = 10;
	v4[1] = 1;
	v4[2] = 1;
	v4[3] = 1;
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 1);
	CHECK(cryc_name_match_ip(&cert, v6, 16) == 1);
	v4[3] = 2;
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 0);
	v4[0] = 192;
	v4[1] = 0;
	v4[2] = 2;
	v4[3] = 1;
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 0);

	/* A name that is not tagged as an iPAddress is not one. */
	make_cert(&cert, n_empty(), b_str(0x04U, "\x0a\x01\x01\x01"));
	v4[0] = 10;
	v4[1] = 1;
	v4[2] = 1;
	v4[3] = 1;
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 0);
	make_cert(&cert, n_empty(), b_tlv(0xa7U, b_raw(v4, 4)));
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 0);

	/* Missing pieces. */
	make_cert(&cert, n_empty(), g_ip4(10, 1, 1, 1));
	CHECK(cryc_name_match_ip(NULL, v4, 4) == 0);
	CHECK(cryc_name_match_ip(&cert, NULL, 4) == 0);
	cert.flags = 0;
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 0);

	/* A SAN that does not parse matches nothing, even after a match. */
	san = b_cat(g_ip4(10, 1, 1, 1), b_hex("8704aa"));
	make_cert(&cert, n_empty(), san);
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 0);
	san = b_cat(b_hex("87"), g_ip4(10, 1, 1, 1));
	make_cert(&cert, n_empty(), san);
	CHECK(cryc_name_match_ip(&cert, v4, 4) == 0);

	/* Gives back everything this test function built. */
	mark_release(mark);
}

/* ------------------------------------------------------------------ */
/* cryc_name_constraints_check                                          */
/* ------------------------------------------------------------------ */

/* The cases of dNSName constraints. */
static void
test_nc_dns(void)
{
	struct mark mark;
	struct bytes *san;
	char longname[512];

	/* Remembers the pool and the copies, to give them back at the end. */
	mark = mark_take();

	/* A constraint "example.com" permits the name and every name with labels added on the left, ignoring case. */
	CHECK(nc_dns("example.com", NULL, "example.com") == ALLOW);
	CHECK(nc_dns("example.com", NULL, "a.example.com") == ALLOW);
	CHECK(nc_dns("example.com", NULL, "b.a.example.com") == ALLOW);
	CHECK(nc_dns("example.com", NULL, "A.EXAMPLE.COM") == ALLOW);
	CHECK(nc_dns("EXAMPLE.COM", NULL, "a.example.com") == ALLOW);
	CHECK(nc_dns("example.com", NULL, "badexample.com") == DENY);
	CHECK(nc_dns("example.com", NULL, "xexample.com") == DENY);
	CHECK(nc_dns("example.com", NULL, "example.com.evil.org") == DENY);
	CHECK(nc_dns("example.com", NULL, "example.co") == DENY);
	CHECK(nc_dns("example.com", NULL, "example") == DENY);
	CHECK(nc_dns("example.com", NULL, "com") == DENY);
	CHECK(nc_dns("example.com", NULL, "other.org") == DENY);
	CHECK(nc_dns("com", NULL, "example.com") == ALLOW);
	CHECK(nc_dns("com", NULL, "com") == ALLOW);
	CHECK(nc_dns("com", NULL, "xcom") == DENY);
	CHECK(nc_dns("a.b", NULL, "a.b") == ALLOW);
	CHECK(nc_dns("a.b", NULL, "b") == DENY);
	CHECK(nc_dns("a.b", NULL, ".b") == EINVAL);

	/* A constraint with a leading period permits only the names below it. */
	CHECK(nc_dns(".example.com", NULL, "example.com") == DENY);
	CHECK(nc_dns(".example.com", NULL, "a.example.com") == ALLOW);
	CHECK(nc_dns(".example.com", NULL, "a.b.example.com") == ALLOW);
	CHECK(nc_dns(".example.com", NULL, "A.Example.Com") == ALLOW);
	CHECK(nc_dns(".example.com", NULL, "xexample.com") == DENY);
	CHECK(nc_dns(".example.com", NULL, "a.xexample.com") == DENY);
	CHECK(nc_dns(".example.com", NULL, "a.example.co") == DENY);

	/* The empty constraint is every name. */
	CHECK(nc_dns("", NULL, "anything.example") == ALLOW);
	CHECK(nc_dns("", NULL, "a") == ALLOW);
	CHECK(nc_dns(NULL, "", "a") == DENY);
	CHECK(nc_dns(NULL, "", "a.b.c") == DENY);

	/* Excluded subtrees. */
	CHECK(nc_dns(NULL, "example.com", "example.com") == DENY);
	CHECK(nc_dns(NULL, "example.com", "a.example.com") == DENY);
	CHECK(nc_dns(NULL, "example.com", "A.EXAMPLE.COM") == DENY);
	CHECK(nc_dns(NULL, "example.com", "other.org") == ALLOW);
	CHECK(nc_dns(NULL, "example.com", "badexample.com") == ALLOW);
	CHECK(nc_dns(NULL, "example.com", "example.com.evil.org") == ALLOW);
	CHECK(nc_dns(NULL, ".example.com", "example.com") == ALLOW);
	CHECK(nc_dns(NULL, ".example.com", "a.example.com") == DENY);

	/* Both lists: the excluded subtree wins inside the permitted one. */
	CHECK(nc_dns("example.com", "bad.example.com", "ok.example.com") == ALLOW);
	CHECK(nc_dns("example.com", "bad.example.com", "bad.example.com") == DENY);
	CHECK(nc_dns("example.com", "bad.example.com", "x.bad.example.com") == DENY);
	CHECK(nc_dns("example.com", "bad.example.com", "example.com") == ALLOW);
	CHECK(nc_dns("example.com", "example.com", "example.com") == DENY);
	CHECK(nc_dns("example.com", "bad.example.com", "other.org") == DENY);

	/* Several names: one outside makes the check fail, whatever the order. */
	san = b_cat(g_dns("a.example.com"), g_dns("b.example.org"));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_empty(), san) == DENY);
	san = b_cat(g_dns("b.example.org"), g_dns("a.example.com"));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_empty(), san) == DENY);
	san = b_cat(g_dns("a.example.com"), g_dns("b.example.com"));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_empty(), san) == ALLOW);

	/* Several subtrees: any permitted one will do, in any position. */
	san = b_cat(s_sub(g_dns("one.org")), b_cat(s_sub(g_dns("two.org")), s_sub(g_dns("three.org"))));
	CHECK(nc(san, NULL, n_empty(), g_dns("a.one.org")) == ALLOW);
	CHECK(nc(san, NULL, n_empty(), g_dns("a.two.org")) == ALLOW);
	CHECK(nc(san, NULL, n_empty(), g_dns("a.three.org")) == ALLOW);
	CHECK(nc(san, NULL, n_empty(), g_dns("a.four.org")) == DENY);
	CHECK(nc(NULL, san, n_empty(), g_dns("a.three.org")) == DENY);
	CHECK(nc(NULL, san, n_empty(), g_dns("a.four.org")) == ALLOW);

	/* Names of other forms are not constrained by dNSName subtrees; no dNSName at all is allowed. */
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_empty(), g_ip4(10, 0, 0, 1)) == ALLOW);
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_empty(), g_mail("a@other.org")) == ALLOW);
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_empty(), NULL) == ALLOW);
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_spec("CN=other.org"), NULL) == ALLOW);
	CHECK(nc(NULL, s_sub(g_dns("example.com")), n_spec("CN=a.example.com"), g_ip4(10, 0, 0, 1)) == ALLOW);

	/* A permitted list of another form does not restrict dNSNames. */
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), g_dns("anything.org")) == ALLOW);
	CHECK(nc(NULL, s_sub(g_ip_hex("0a000000ff000000")), n_empty(), g_dns("anything.org")) == ALLOW);

	/* A wildcard name is read as the names it could stand for. */
	CHECK(nc_dns("foo.com", NULL, "*.foo.com") == ALLOW);
	CHECK(nc_dns("foo.example.com", NULL, "*.example.com") == DENY);
	CHECK(nc_dns("example.com", NULL, "*.example.com") == ALLOW);
	CHECK(nc_dns("example.com", NULL, "*.com") == DENY);
	CHECK(nc_dns(".example.com", NULL, "*.example.com") == ALLOW);
	CHECK(nc_dns(NULL, "bar.example.com", "*.example.com") == DENY);
	CHECK(nc_dns(NULL, "BAR.example.com", "*.EXAMPLE.com") == DENY);
	CHECK(nc_dns(NULL, "example.com", "*.example.com") == DENY);
	CHECK(nc_dns(NULL, "x.y.example.com", "*.example.com") == ALLOW);
	CHECK(nc_dns(NULL, "bar.other.com", "*.example.com") == ALLOW);
	CHECK(nc_dns(NULL, "bar.example.org", "*.example.com") == ALLOW);
	CHECK(nc_dns(NULL, ".bar.example.com", "*.example.com") == ALLOW);
	CHECK(nc_dns(NULL, ".example.com", "*.example.com") == DENY);
	CHECK(nc_dns(NULL, "com", "*") == DENY);
	CHECK(nc_dns(NULL, "a.com", "*") == ALLOW);
	CHECK(nc_dns(NULL, ".com", "*") == ALLOW);
	CHECK(nc_dns("com", NULL, "*") == DENY);
	CHECK(nc_dns("", NULL, "*") == ALLOW);
	CHECK(nc_dns(NULL, "", "*.example.com") == DENY);
	CHECK(nc_dns("a.b", "c.b", "*.b") == DENY);
	CHECK(nc_dns("b", "c.b", "*.b") == DENY);
	CHECK(nc_dns("b", "c.d", "*.b") == ALLOW);

	/* A wildcard against single-label and shorter or longer constraints. */
	CHECK(nc_dns(NULL, "c", "*") == DENY);
	CHECK(nc_dns(NULL, "c.d", "*") == ALLOW);
	CHECK(nc_dns(NULL, "com", "*.example.com") == DENY);
	CHECK(nc_dns(NULL, "net", "*.example.com") == ALLOW);
	CHECK(nc_dns(NULL, "example", "*.example.com") == ALLOW);
	CHECK(nc_dns(NULL, "bar.example", "*.example.com") == ALLOW);
	CHECK(nc_dns(NULL, "bar.example.co", "*.example.com") == ALLOW);
	CHECK(nc_dns(NULL, "bar.example.comm", "*.example.com") == ALLOW);
	CHECK(nc_dns(NULL, "bar.example.com", "*.example.com") == DENY);

	/* Printable ASCII is the range of the octets of a name: its two ends are in, the octets around it are out, at any position. */
	CHECK(nc_dns("example.com", NULL, "!.example.com") == ALLOW);
	CHECK(nc_dns("example.com", NULL, "~.example.com") == ALLOW);
	CHECK(nc_dns("example.com", NULL, "a!~.example.com") == ALLOW);
	CHECK(nc_dns("example.com", NULL, " a.example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "\x01" "a.example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "\x7f" "a.example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "\x80" "a.example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "a.example.com ") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "a.example.com\x7f") == EINVAL);
	CHECK(nc_dns(" example.com", NULL, "a.example.com") == EINVAL);
	CHECK(nc_dns("!", NULL, "a.!") == ALLOW);
	CHECK(nc_dns("~", NULL, "a.~") == ALLOW);

	/* A wildcard only as the whole first label: a star in a later label is malformed whatever follows the first octets. */
	CHECK(nc_dns("example.com", NULL, "a.*.example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "a.*") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "*x") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "*.") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "*") == DENY);
	CHECK(nc_dns("example.com", NULL, "*.com") == DENY);
	CHECK(nc_dns("", NULL, "*") == ALLOW);

	/* Names that are malformed make a constrained check fail. */
	CHECK(nc_dns("example.com", NULL, "") == EINVAL);
	CHECK(nc_dns("example.com", NULL, ".example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "example.com.") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "a..example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "foo.*.example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "f*.example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "*f.example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "**.example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "\xc3\xa9.example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "a b.example.com") == EINVAL);
	CHECK(nc_dns("example.com", NULL, "a\x7f.example.com") == EINVAL);
	CHECK(nc_dns(NULL, "example.com", "") == EINVAL);
	CHECK(nc_dns(NULL, "example.com", "a..b") == EINVAL);
	CHECK(nc_dns(NULL, "example.com", "\xc3\xa9") == EINVAL);
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_empty(), b_tlv(TAG_DNS, b_raw("a\0b", 3))) == EINVAL);
	build_labels(longname, 1, 64);
	CHECK(nc_dns("example.com", NULL, longname) == EINVAL);
	build_labels(longname, 3, 63);
	strcat(longname, ".");
	strcat(longname, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	CHECK(strlen(longname) == 253U);
	CHECK(nc_dns("example.com", NULL, longname) == DENY);
	CHECK(nc_dns("", NULL, longname) == ALLOW);
	strcat(longname, "b");
	CHECK(nc_dns("example.com", NULL, longname) == EINVAL);
	CHECK(nc_dns("", NULL, longname) == EINVAL);

	/* A malformed name of a form no constraint mentions is not looked at. */
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), g_dns("a..b")) == ALLOW);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_cat(g_dns(""), g_dns("\xc3"))) == ALLOW);
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_empty(), g_ip_hex("0a0000")) == ALLOW);

	/* A constraint that is malformed makes the check fail whatever the names. */
	CHECK(nc(s_sub(g_dns("*.example.com")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(g_dns("a..b")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(g_dns("example.com.")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(g_dns("..example.com")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(g_dns(".")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(g_dns("a b")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(g_dns("\xc3\xa9")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(NULL, s_sub(g_dns("*.example.com")), n_empty(), NULL) == EINVAL);
	CHECK(nc(NULL, s_sub(g_dns("a..b")), n_empty(), NULL) == EINVAL);
	CHECK(nc(b_cat(s_sub(g_dns("example.com")), s_sub(g_dns("a..b"))), NULL, n_empty(), g_dns("a.example.com")) == EINVAL);
	build_labels(longname, 1, 64);
	CHECK(nc(s_sub(g_dns(longname)), NULL, n_empty(), NULL) == EINVAL);
	build_labels(longname, 1, 63);
	CHECK(nc(s_sub(g_dns(longname)), NULL, n_empty(), NULL) == ALLOW);
	build_labels(longname, 3, 63);
	strcat(longname, ".");
	strcat(longname, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	CHECK(nc(s_sub(g_dns(longname)), NULL, n_empty(), NULL) == ALLOW);
	memmove(longname + 1, longname, strlen(longname) + 1U);
	longname[0] = '.';
	CHECK(nc(s_sub(g_dns(longname)), NULL, n_empty(), NULL) == ALLOW);
	strcat(longname, "b");
	CHECK(nc(s_sub(g_dns(longname)), NULL, n_empty(), NULL) == EINVAL);

	/* Gives back everything this test function built. */
	mark_release(mark);
}

/* Runs a check with one iPAddress constraint (hex of address and mask) and one name of the given hex. */
static int
nc_ip(
	const char *permitted,
	const char *excluded,
	const char *name)
{
	struct bytes *permitted_list;
	struct bytes *excluded_list;
	struct bytes *subject;
	struct bytes *san;
	int result;

	/* Starts with no list. */
	permitted_list = NULL;
	excluded_list = NULL;

	/* Builds the permitted list when a constraint is given. */
	if (permitted != NULL)
		permitted_list = s_sub(g_ip_hex(permitted));

	/* Builds the excluded list when a constraint is given. */
	if (excluded != NULL)
		excluded_list = s_sub(g_ip_hex(excluded));

	/* Runs the check on a certificate with an empty subject and the one name. */
	subject = n_empty();
	san = g_ip_hex(name);
	result = nc(permitted_list, excluded_list, subject, san);

	/* Reports the result of the check. */
	return result;
}

/* The cases of iPAddress constraints. */
static void
test_nc_ip(void)
{
	struct mark mark;
	char hex[128];
	size_t index;
	unsigned long value;

	/* Remembers the pool and the copies, to give them back at the end. */
	mark = mark_take();

	/* 192.0.2.0/24 (RFC 5280 4.2.1.10's example). */
	CHECK(nc_ip("c0000200ffffff00", NULL, "c0000201") == ALLOW);
	CHECK(nc_ip("c0000200ffffff00", NULL, "c00002ff") == ALLOW);
	CHECK(nc_ip("c0000200ffffff00", NULL, "c0000200") == ALLOW);
	CHECK(nc_ip("c0000200ffffff00", NULL, "c0000301") == DENY);
	CHECK(nc_ip("c0000200ffffff00", NULL, "c0000101") == DENY);
	CHECK(nc_ip("c0000200ffffff00", NULL, "c1000201") == DENY);
	CHECK(nc_ip("c0000200ffffff00", NULL, "bf000201") == DENY);
	CHECK(nc_ip(NULL, "c0000200ffffff00", "c0000201") == DENY);
	CHECK(nc_ip(NULL, "c0000200ffffff00", "c0000301") == ALLOW);

	/* The bits of the address under zero mask bits do not matter. */
	CHECK(nc_ip("c00002ffffffff00", NULL, "c0000201") == ALLOW);
	CHECK(nc_ip("c00002ffffffff00", NULL, "c0000301") == DENY);

	/* A mask that stops inside a byte: 192.0.2.128/25. */
	CHECK(nc_ip("c0000280ffffff80", NULL, "c00002c8") == ALLOW);
	CHECK(nc_ip("c0000280ffffff80", NULL, "c0000280") == ALLOW);
	CHECK(nc_ip("c0000280ffffff80", NULL, "c00002ff") == ALLOW);
	CHECK(nc_ip("c0000280ffffff80", NULL, "c0000205") == DENY);
	CHECK(nc_ip("c0000280ffffff80", NULL, "c000027f") == DENY);
	CHECK(nc_ip("c0000200ffffffc0", NULL, "c000023f") == ALLOW);
	CHECK(nc_ip("c0000200ffffffc0", NULL, "c0000240") == DENY);
	CHECK(nc_ip("c0000200fffffffe", NULL, "c0000201") == ALLOW);
	CHECK(nc_ip("c0000200fffffffe", NULL, "c0000202") == DENY);
	CHECK(nc_ip("c0000200fffffffe", NULL, "c0000203") == DENY);

	/* A /32 and a /0. */
	CHECK(nc_ip("c0000201ffffffff", NULL, "c0000201") == ALLOW);
	CHECK(nc_ip("c0000201ffffffff", NULL, "c0000202") == DENY);
	CHECK(nc_ip("0000000000000000", NULL, "c0000201") == ALLOW);
	CHECK(nc_ip(NULL, "0000000000000000", "c0000201") == DENY);

	/* Every bit of a /32 counts. */
	for (index = 0; index < 32U; index++) {
		/* Flips the one bit of the address and writes the address as hex. */
		value = 0xc0000201UL ^ (1UL << index);
		sprintf(hex, "%08lx", value);
		CHECK(nc_ip("c0000201ffffffff", NULL, hex) == DENY);
		CHECK(nc_ip(NULL, "c0000201ffffffff", hex) == ALLOW);
	}

	/* IPv6: ::1/128, and a /64 prefix. */
	CHECK(nc_ip("00000000000000000000000000000001ffffffffffffffffffffffffffffffff", NULL, "00000000000000000000000000000001") == ALLOW);
	CHECK(nc_ip("00000000000000000000000000000001ffffffffffffffffffffffffffffffff", NULL, "00000000000000000000000000000002") == DENY);
	CHECK(nc_ip(NULL, "00000000000000000000000000000001ffffffffffffffffffffffffffffffff", "00000000000000000000000000000001") == DENY);
	CHECK(nc_ip(NULL, "00000000000000000000000000000001ffffffffffffffffffffffffffffffff", "00000000000000000000000000000002") == ALLOW);
	CHECK(nc_ip("20010db8000000000000000000000000ffffffffffffffff0000000000000000", NULL, "20010db800000000123456789abcdef0") == ALLOW);
	CHECK(nc_ip("20010db8000000000000000000000000ffffffffffffffff0000000000000000", NULL, "20010db800000001123456789abcdef0") == DENY);
	CHECK(nc_ip("20010db8000000000000000000000000fffffffffffffffe0000000000000000", NULL, "20010db800000001123456789abcdef0") == ALLOW);
	CHECK(nc_ip("20010db8000000000000000000000000fffffffffffffffe0000000000000000", NULL, "20010db800000002123456789abcdef0") == DENY);

	/* The families do not mix: a permitted list of one family refuses a name of the other, an excluded list of one family does not hit the other. */
	CHECK(nc_ip("c0000200ffffff00", NULL, "00000000000000000000000000000001") == DENY);
	CHECK(nc_ip("00000000000000000000000000000001ffffffffffffffffffffffffffffffff", NULL, "c0000201") == DENY);
	CHECK(nc_ip(NULL, "c0000200ffffff00", "00000000000000000000000000000001") == ALLOW);
	CHECK(nc_ip(NULL, "0000000000000000", "00000000000000000000000000000001") == ALLOW);
	CHECK(nc_ip("0000000000000000", NULL, "00000000000000000000000000000001") == DENY);

	/* A name that is not 4 or 16 octets is malformed. */
	CHECK(nc_ip("c0000200ffffff00", NULL, "c0000200ffffff00") == EINVAL);
	CHECK(nc_ip("c0000200ffffff00", NULL, "c00002") == EINVAL);
	CHECK(nc_ip("c0000200ffffff00", NULL, "c000020001") == EINVAL);
	CHECK(nc_ip("c0000200ffffff00", NULL, "") == EINVAL);
	CHECK(nc_ip(NULL, "c0000200ffffff00", "000000000000000000000000000001") == EINVAL);

	/* Constraints that are not an address and a CIDR mask are malformed. */
	CHECK(nc_ip("c0000200ffffff00", NULL, "c0000201") == ALLOW);
	CHECK(nc_ip("c0000200ffff00ff", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200ff00ff00", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c000020000ffffff", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c00002007fffffff", NULL, "c0000201") == EINVAL);

	/* After the byte where the ones end, every byte must be zero (a partial byte later is not a CIDR mask). */
	CHECK(nc_ip("c0000200fff0f000", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200ff80c000", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200ffe00080", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200fff00000", NULL, "c0000201") == ALLOW);
	CHECK(nc_ip("c0000200fe000000", NULL, "c0000201") == ALLOW);
	CHECK(nc_ip("c0000200f0000000", NULL, "c0000201") == ALLOW);
	CHECK(nc_ip("c0000200f0000000", NULL, "d0000201") == DENY);
	CHECK(nc_ip("c0000200ffffff81", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200ffffff0f", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200ffffff01", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200ffffff7f", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200fffffffd", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200ffffffa0", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200ff0000ff", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200000000ff", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200ffffff00ff", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200ffffff", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("c0000200ffffffffc0000200ffffff00", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip(NULL, "c0000200ffff00ff", "c0000201") == EINVAL);
	CHECK(nc_ip("", NULL, "c0000201") == EINVAL);
	CHECK(nc_ip("00000000000000000000000000000001ffffffffffffffffffffffffffffff00", NULL, "00000000000000000000000000000001") == ALLOW);
	CHECK(nc_ip("00000000000000000000000000000001ffffffffffffffff00ffffffffffffff", NULL, "00000000000000000000000000000001") == EINVAL);
	CHECK(nc_ip("00000000000000000000000000000001ffffffffffffffffffffffffffffff", NULL, "00000000000000000000000000000001") == EINVAL);
	CHECK(nc_ip("00000000000000000000000000000001ffffffffffffffffffffffffffffffffff", NULL, "00000000000000000000000000000001") == EINVAL);
	CHECK(nc_ip("00000000000000000000000000000001", NULL, "00000000000000000000000000000001") == EINVAL);
	CHECK(nc_ip("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", NULL, "00000000000000000000000000000001") == DENY);

	/* Gives back everything this test function built. */
	mark_release(mark);
}

/* Runs a check with one rfc822Name constraint and one mailbox name. */
static int
nc_mail(
	const char *permitted,
	const char *excluded,
	const char *name)
{
	struct bytes *permitted_list;
	struct bytes *excluded_list;
	struct bytes *subject;
	struct bytes *san;
	int result;

	/* Starts with no list. */
	permitted_list = NULL;
	excluded_list = NULL;

	/* Builds the permitted list when a constraint is given. */
	if (permitted != NULL)
		permitted_list = s_sub(g_mail(permitted));

	/* Builds the excluded list when a constraint is given. */
	if (excluded != NULL)
		excluded_list = s_sub(g_mail(excluded));

	/* Runs the check on a certificate with an empty subject and the one name. */
	subject = n_empty();
	san = g_mail(name);
	result = nc(permitted_list, excluded_list, subject, san);

	/* Reports the result of the check. */
	return result;
}

/* The cases of rfc822Name constraints and of the emailAddress attribute of the subject. */
static void
test_nc_mail(void)
{
	struct mark mark;
	struct bytes *subject;
	struct bytes *san;
	char longname[600];

	/* Remembers the pool and the copies, to give them back at the end. */
	mark = mark_take();

	/* A host: the mailboxes on that host only. */
	CHECK(nc_mail("example.com", NULL, "a@example.com") == ALLOW);
	CHECK(nc_mail("example.com", NULL, "a@EXAMPLE.com") == ALLOW);
	CHECK(nc_mail("example.com", NULL, "A@example.com") == ALLOW);
	CHECK(nc_mail("EXAMPLE.com", NULL, "a@example.COM") == ALLOW);
	CHECK(nc_mail("example.com", NULL, "a@sub.example.com") == DENY);
	CHECK(nc_mail("example.com", NULL, "a@xexample.com") == DENY);
	CHECK(nc_mail("example.com", NULL, "a@example.org") == DENY);
	CHECK(nc_mail("example.com", NULL, "a@example.co") == DENY);
	CHECK(nc_mail("example.com", NULL, "a@b@example.com") == ALLOW);
	CHECK(nc_mail("example.com", NULL, "a@example.com@evil.org") == DENY);

	/* A domain with a leading period: the hosts below it, not the domain itself. */
	CHECK(nc_mail(".example.com", NULL, "a@sub.example.com") == ALLOW);
	CHECK(nc_mail(".example.com", NULL, "a@a.b.example.com") == ALLOW);
	CHECK(nc_mail(".example.com", NULL, "a@SUB.EXAMPLE.com") == ALLOW);
	CHECK(nc_mail(".example.com", NULL, "a@example.com") == DENY);
	CHECK(nc_mail(".example.com", NULL, "a@.example.com") == DENY);
	CHECK(nc_mail(".example.com", NULL, "a@x.example.com") == ALLOW);
	CHECK(nc_mail(".example.com", NULL, "a@notexample.com") == DENY);
	CHECK(nc_mail(".example.com", NULL, "a@xexample.com") == DENY);
	CHECK(nc_mail(".example.com", NULL, "a@sub.example.org") == DENY);

	/* A mailbox: the local part exactly as written, the host ignoring case. */
	CHECK(nc_mail("root@example.com", NULL, "root@example.com") == ALLOW);
	CHECK(nc_mail("root@example.com", NULL, "root@EXAMPLE.COM") == ALLOW);
	CHECK(nc_mail("root@example.com", NULL, "Root@example.com") == DENY);
	CHECK(nc_mail("root@example.com", NULL, "other@example.com") == DENY);
	CHECK(nc_mail("root@example.com", NULL, "root@sub.example.com") == DENY);
	CHECK(nc_mail("root@example.com", NULL, "root@example.org") == DENY);
	CHECK(nc_mail("root@example.com", NULL, "roo@example.com") == DENY);
	CHECK(nc_mail("root@example.com", NULL, "rootx@example.com") == DENY);
	CHECK(nc_mail("root@example.com", NULL, "root@example.co") == DENY);

	/* Excluded subtrees. */
	CHECK(nc_mail(NULL, "example.com", "a@example.com") == DENY);
	CHECK(nc_mail(NULL, "example.com", "a@sub.example.com") == ALLOW);
	CHECK(nc_mail(NULL, ".example.com", "a@sub.example.com") == DENY);
	CHECK(nc_mail(NULL, ".example.com", "a@example.com") == ALLOW);
	CHECK(nc_mail(NULL, "root@example.com", "root@example.com") == DENY);
	CHECK(nc_mail(NULL, "root@example.com", "other@example.com") == ALLOW);

	/* A host constraint is the whole host: a longer host with the same start is another host. */
	CHECK(nc_mail("example.com", NULL, "a@example.com.evil.org") == DENY);
	CHECK(nc_mail("example.com", NULL, "a@example.comx") == DENY);
	CHECK(nc_mail("example.com", NULL, "a@example.com.") == DENY);
	CHECK(nc_mail("root@example.com", NULL, "root@example.com.evil.org") == DENY);
	CHECK(nc_mail("root@example.com", NULL, "root@example.comx") == DENY);

	/* The octets of a mailbox are printable ASCII, the first and the last too. */
	CHECK(nc_mail("example.com", NULL, "a!b~c@example.com") == ALLOW);
	CHECK(nc_mail("example.com", NULL, "!@example.com") == ALLOW);
	CHECK(nc_mail("example.com", NULL, "~@example.com") == ALLOW);
	CHECK(nc_mail("example.com", NULL, " a@example.com") == EINVAL);
	CHECK(nc_mail("example.com", NULL, "a@example.com ") == EINVAL);
	CHECK(nc_mail("example.com", NULL, "\x7f" "a@example.com") == EINVAL);
	CHECK(nc_mail("example.com", NULL, "a@example.com\x80") == EINVAL);
	CHECK(nc_mail(" example.com", NULL, "a@example.com") == EINVAL);
	CHECK(nc_mail("example.com ", NULL, "a@example.com") == EINVAL);

	/* Names that are not mailboxes, and constraints that are not host, domain or mailbox, are malformed. */
	CHECK(nc_mail("example.com", NULL, "noat") == EINVAL);
	CHECK(nc_mail("example.com", NULL, "@example.com") == EINVAL);
	CHECK(nc_mail("example.com", NULL, "a@") == EINVAL);
	CHECK(nc_mail("example.com", NULL, "") == EINVAL);
	CHECK(nc_mail("example.com", NULL, "a b@example.com") == EINVAL);
	CHECK(nc_mail("example.com", NULL, "\xc3\xa9@example.com") == EINVAL);
	CHECK(nc_mail("", NULL, "a@example.com") == EINVAL);
	CHECK(nc_mail(".", NULL, "a@example.com") == EINVAL);
	CHECK(nc_mail("@example.com", NULL, "a@example.com") == EINVAL);
	CHECK(nc_mail("root@", NULL, "a@example.com") == EINVAL);
	CHECK(nc_mail("a b", NULL, "a@example.com") == EINVAL);
	CHECK(nc_mail(NULL, "", "a@example.com") == EINVAL);
	memset(longname, 'a', 600);
	longname[511] = '@';
	longname[512] = 'b';
	longname[513] = '\0';
	CHECK(nc_mail("b", NULL, longname) == EINVAL);
	longname[510] = '@';
	longname[511] = 'b';
	longname[512] = '\0';
	CHECK(nc_mail("b", NULL, longname) == ALLOW);
	memset(longname, 'a', 600);
	longname[600 - 1] = '\0';
	CHECK(nc_mail(longname, NULL, "a@b") == EINVAL);

	/* A name constrained as mail does not constrain other forms. */
	CHECK(nc(s_sub(g_mail("example.com")), NULL, n_empty(), g_dns("anything.org")) == ALLOW);

	/* The emailAddress attribute of the subject counts when there is no subjectAltName, in any RDN. */
	subject = n_spec("CN=a,E=bob@example.org");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == DENY);
	CHECK(nc(s_sub(g_mail("example.org")), NULL, subject, NULL) == ALLOW);
	CHECK(nc(NULL, s_sub(g_mail("example.org")), subject, NULL) == DENY);
	CHECK(nc(NULL, s_sub(g_mail("example.com")), subject, NULL) == ALLOW);
	subject = n_spec("E=bob@example.org,CN=a");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == DENY);
	subject = b_name(b_rdn(b_cat(b_attr(OID_CN, TAG_UTF8, "a"), b_attr(OID_EMAIL, TAG_IA5, "bob@example.org"))));
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == DENY);
	CHECK(nc(s_sub(g_mail("example.org")), NULL, subject, NULL) == ALLOW);

	/* A subject with two mail addresses: both are checked. */
	subject = b_name(b_cat(b_rdn(b_attr(OID_EMAIL, TAG_IA5, "a@example.com")), b_rdn(b_attr(OID_EMAIL, TAG_IA5, "b@example.org"))));
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == DENY);
	CHECK(nc(NULL, s_sub(g_mail("example.org")), subject, NULL) == DENY);
	CHECK(nc(NULL, s_sub(g_mail("example.net")), subject, NULL) == ALLOW);

	/* With a subjectAltName the attribute is not used. */
	san = g_dns("a.example.com");
	subject = n_spec("CN=a,E=bob@example.org");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, san) == ALLOW);
	san = g_mail("bob@example.com");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, san) == ALLOW);
	san = g_mail("bob@example.org");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, san) == DENY);

	/* Other attributes with mail-like text are not mail addresses; other string types of the attribute are accepted, others are malformed. */
	subject = n_spec("CN=bob@example.org");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == ALLOW);
	subject = n_one(OID_EMAIL, TAG_UTF8, "bob@example.org");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == DENY);
	subject = n_one(OID_EMAIL, TAG_PRINTABLE, "bob@example.org");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == DENY);
	subject = n_one(OID_EMAIL, TAG_BMP, "bob@example.org");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == EINVAL);
	subject = n_one(OID_EMAIL, TAG_IA5, "bob");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == EINVAL);
	subject = n_one("1.2.840.113549.1.9.2", TAG_IA5, "bob@example.org");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == ALLOW);
	subject = n_one("1.2.840.113549.1.9.1.1", TAG_IA5, "bob@example.org");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == ALLOW);
	subject = n_one("1.2.840.113549.1.9", TAG_IA5, "bob@example.org");
	CHECK(nc(s_sub(g_mail("example.com")), NULL, subject, NULL) == ALLOW);

	/* Without mail constraints the attribute is not looked at. */
	subject = n_one(OID_EMAIL, TAG_BMP, "x");
	CHECK(nc(s_sub(g_dns("example.com")), NULL, subject, NULL) == ALLOW);

	/* Gives back everything this test function built. */
	mark_release(mark);
}

/* The cases of directoryName constraints. */
static void
test_nc_directory(void)
{
	struct mark mark;
	struct bytes *constraint;
	struct bytes *san;
	struct bytes *subject;
	struct bytes *bad;

	/* Remembers the pool and the copies, to give them back at the end. */
	mark = mark_take();

	/* The subject must begin with the constraint's RDNs. */
	constraint = s_sub(g_dir(n_spec("C=US,O=Org")));
	CHECK(nc(constraint, NULL, n_spec("C=US,O=Org"), NULL) == ALLOW);
	CHECK(nc(constraint, NULL, n_spec("C=US,O=Org,CN=host"), NULL) == ALLOW);
	CHECK(nc(constraint, NULL, n_spec("C=US,O=Org,OU=a,CN=host"), NULL) == ALLOW);
	CHECK(nc(constraint, NULL, n_spec("C=us,O=org"), NULL) == ALLOW);
	CHECK(nc(constraint, NULL, n_spec("C=US"), NULL) == DENY);
	CHECK(nc(constraint, NULL, n_spec("C=US,O=Other"), NULL) == DENY);
	CHECK(nc(constraint, NULL, n_spec("C=FR,O=Org"), NULL) == DENY);
	CHECK(nc(constraint, NULL, n_spec("O=Org,C=US"), NULL) == DENY);
	CHECK(nc(constraint, NULL, n_spec("CN=host,C=US,O=Org"), NULL) == DENY);
	CHECK(nc(constraint, NULL, n_spec("OU=a,C=US,O=Org"), NULL) == DENY);
	CHECK(nc(constraint, NULL, n_spec("CN=US,O=Org"), NULL) == DENY);

	/* Excluded: the same comparison. */
	CHECK(nc(NULL, constraint, n_spec("C=US,O=Org,CN=host"), NULL) == DENY);
	CHECK(nc(NULL, constraint, n_spec("C=US,O=Org"), NULL) == DENY);
	CHECK(nc(NULL, constraint, n_spec("C=US"), NULL) == ALLOW);
	CHECK(nc(NULL, constraint, n_spec("C=FR,O=Org"), NULL) == ALLOW);
	CHECK(nc(NULL, constraint, n_spec("CN=host"), NULL) == ALLOW);

	/* A constraint of one RDN. */
	constraint = s_sub(g_dir(n_spec("C=US")));
	CHECK(nc(constraint, NULL, n_spec("C=US"), NULL) == ALLOW);
	CHECK(nc(constraint, NULL, n_spec("C=US,O=x"), NULL) == ALLOW);
	CHECK(nc(constraint, NULL, n_spec("O=x,C=US"), NULL) == DENY);

	/* The empty constraint is every Name. */
	constraint = s_sub(g_dir(n_empty()));
	CHECK(nc(constraint, NULL, n_spec("C=US"), NULL) == ALLOW);
	CHECK(nc(NULL, constraint, n_spec("C=US"), NULL) == DENY);

	/* An empty subject (an empty Name, or no bytes) is not constrained. */
	constraint = s_sub(g_dir(n_spec("C=US")));
	CHECK(nc(constraint, NULL, n_empty(), NULL) == ALLOW);
	CHECK(nc(constraint, NULL, NULL, NULL) == ALLOW);
	CHECK(nc(NULL, s_sub(g_dir(n_empty())), n_empty(), NULL) == ALLOW);

	/* The directoryName entries of the subjectAltName are compared too, and only those of that form. */
	san = g_dir(n_spec("C=US,O=Org,CN=a"));
	CHECK(nc(constraint, NULL, n_empty(), san) == ALLOW);
	san = g_dir(n_spec("C=FR,O=Org,CN=a"));
	CHECK(nc(constraint, NULL, n_empty(), san) == DENY);
	CHECK(nc(NULL, constraint, n_empty(), san) == ALLOW);
	san = b_cat(g_dir(n_spec("C=US")), g_dir(n_spec("C=FR")));
	CHECK(nc(constraint, NULL, n_empty(), san) == DENY);
	san = b_cat(g_dir(n_spec("C=FR")), g_dir(n_spec("C=US")));
	CHECK(nc(constraint, NULL, n_empty(), san) == DENY);
	san = b_cat(g_dir(n_spec("C=US")), g_dns("example.org"));
	CHECK(nc(constraint, NULL, n_empty(), san) == ALLOW);
	san = g_dir(n_empty());
	CHECK(nc(constraint, NULL, n_empty(), san) == DENY);
	CHECK(nc(s_sub(g_dir(n_empty())), NULL, n_empty(), san) == ALLOW);
	CHECK(nc(NULL, constraint, n_empty(), san) == ALLOW);

	/* Both the subject and the subjectAltName are checked (the names are separate). */
	CHECK(nc(constraint, NULL, n_spec("C=FR"), g_dir(n_spec("C=US"))) == DENY);
	CHECK(nc(constraint, NULL, n_spec("C=US"), g_dir(n_spec("C=FR"))) == DENY);
	CHECK(nc(constraint, NULL, n_spec("C=US"), g_dir(n_spec("C=US"))) == ALLOW);

	/* A multi-valued RDN in the constraint must be in the subject in the same order. */
	constraint = s_sub(g_dir(b_name(b_tlv(0x31U, b_cat(b_attr(OID_C, TAG_PRINTABLE, "US"), b_attr(OID_O, TAG_UTF8, "Org"))))));
	subject = b_name(b_tlv(0x31U, b_cat(b_attr(OID_C, TAG_PRINTABLE, "US"), b_attr(OID_O, TAG_UTF8, "org"))));
	CHECK(nc(constraint, NULL, subject, NULL) == ALLOW);
	subject = b_name(b_tlv(0x31U, b_cat(b_attr(OID_O, TAG_UTF8, "Org"), b_attr(OID_C, TAG_PRINTABLE, "US"))));
	CHECK(nc(constraint, NULL, subject, NULL) == DENY);
	CHECK(nc(constraint, NULL, n_spec("C=US,O=Org"), NULL) == DENY);

	/* A subject or a name that is malformed is refused whatever is constrained. */
	bad = b_hex("300300");
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	bad = b_hex("3003");
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	bad = b_cat(n_spec("C=US"), b_hex("00"));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	bad = b_name(b_tlv(0x31U, b_new()));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	bad = b_name(b_tlv(0x30U, b_attr(OID_CN, TAG_UTF8, "a")));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_empty(), g_dir(bad)) == ALLOW);
	CHECK(nc(s_sub(g_dir(bad)), NULL, n_empty(), NULL) == EINVAL);
	bad = b_name(b_cat(b_rdn(b_attr(OID_CN, TAG_UTF8, "a")), b_tlv(0x30U, b_attr(OID_CN, TAG_UTF8, "a"))));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	bad = b_name(b_tlv(0x31U, b_tlv(0x04U, b_text("x"))));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	bad = b_name(b_rdn(b_tlv(0x30U, b_cat(b_str(TAG_UTF8, "a"), b_str(TAG_UTF8, "b")))));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	bad = b_name(b_rdn(b_tlv(0x30U, b_cat3(b_oid(OID_CN), b_str(TAG_UTF8, "a"), b_str(TAG_UTF8, "b")))));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	bad = b_name(b_rdn(b_tlv(0x30U, b_cat(b_hex("060180"), b_str(TAG_UTF8, "a")))));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	bad = b_name(b_tlv(0x31U, b_tlv(0x30U, b_oid(OID_CN))));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	bad = b_name(b_tlv(0x31U, b_cat(b_attr(OID_CN, TAG_UTF8, "a"), b_hex("0500"))));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, bad, NULL) == EINVAL);
	CHECK(nc(constraint, NULL, bad, NULL) == EINVAL);
	CHECK(nc(constraint, NULL, n_empty(), g_dir(bad)) == EINVAL);
	CHECK(nc(NULL, constraint, n_empty(), g_dir(b_cat(n_spec("C=US"), b_hex("00")))) == EINVAL);
	CHECK(nc(constraint, NULL, n_empty(), g_dir(b_hex("04"))) == EINVAL);
	CHECK(nc(constraint, NULL, n_empty(), g_dir(b_new())) == EINVAL);

	/* A directoryName of the subjectAltName that is malformed is not looked at when directoryName is not constrained. */
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_empty(), b_cat(g_dir(bad), g_dns("a.example.com"))) == ALLOW);

	/* A directoryName constraint that is malformed. */
	CHECK(nc(s_sub(g_dir(bad)), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(g_dir(b_new())), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(g_dir(b_hex("0500"))), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(g_dir(b_cat(n_spec("C=US"), n_spec("C=US")))), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(NULL, s_sub(g_dir(bad)), n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(b_tlv(0x84U, n_spec("C=US"))), NULL, n_empty(), NULL) == EINVAL);

	/* Gives back everything this test function built. */
	mark_release(mark);
}

/* The forms this module does not evaluate, malformed tags, and the preconditions. */
static void
test_nc_other(void)
{
	struct mark mark;
	struct bytes *other;
	struct bytes *san;
	struct bytes *subtrees;
	struct cryc_x509 ca;
	struct cryc_x509 leaf;
	size_t index;
	unsigned char flipped[256];
	struct bytes *copy;
	int result;

	/* Remembers the pool and the copies, to give them back at the end. */
	mark = mark_take();

	/* A constraint of a form not evaluated fails the check when the subject has a name of that form, and is ignored otherwise. */
	other = b_tlv(TAG_OTHER, b_cat(b_oid("1.2.3.4"), b_tlv(0xa0U, b_str(TAG_UTF8, "x"))));
	CHECK(nc(s_sub(g_uri("example.com")), NULL, n_empty(), g_uri("https://example.com/")) == ENOTSUP);
	CHECK(nc(NULL, s_sub(g_uri(".example.com")), n_empty(), g_uri("https://a.example.com/")) == ENOTSUP);
	CHECK(nc(s_sub(g_uri("example.com")), NULL, n_empty(), g_dns("example.com")) == ALLOW);
	CHECK(nc(s_sub(g_uri("example.com")), NULL, n_empty(), NULL) == ALLOW);
	CHECK(nc(s_sub(g_uri("example.com")), NULL, n_spec("CN=a"), g_ip4(1, 2, 3, 4)) == ALLOW);
	CHECK(nc(NULL, s_sub(other), n_empty(), other) == ENOTSUP);
	CHECK(nc(NULL, s_sub(other), n_empty(), g_dns("a.example.com")) == ALLOW);
	CHECK(nc(s_sub(b_tlv(TAG_X400, b_hex("3000"))), NULL, n_empty(), b_tlv(TAG_X400, b_hex("3000"))) == ENOTSUP);
	CHECK(nc(s_sub(b_tlv(TAG_EDI, b_hex("3000"))), NULL, n_empty(), b_tlv(TAG_EDI, b_hex("3000"))) == ENOTSUP);
	CHECK(nc(s_sub(b_str(TAG_RID, "\x2a\x03")), NULL, n_empty(), b_str(TAG_RID, "\x2a\x03")) == ENOTSUP);
	CHECK(nc(s_sub(b_str(TAG_RID, "\x2a\x03")), NULL, n_empty(), g_uri("a")) == ALLOW);

	/* The unevaluated form is reached while other names are checked: a name outside a dNSName constraint is still refused, in either order. */
	subtrees = b_cat(s_sub(g_uri("example.com")), s_sub(g_dns("example.com")));
	san = b_cat(g_uri("https://example.com/"), g_dns("a.example.com"));
	CHECK(nc(subtrees, NULL, n_empty(), san) == ENOTSUP);
	CHECK(nc(subtrees, NULL, n_empty(), g_dns("a.example.com")) == ALLOW);
	CHECK(nc(subtrees, NULL, n_empty(), g_dns("a.other.org")) == DENY);

	/* Names that are not GeneralNames: a universal tag, a tag number above 8, a constructed dNSName, a primitive directoryName, a long tag. */
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_str(0x02U, "a")) == EINVAL);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_str(0x89U, "a")) == EINVAL);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_str(0x9eU, "a")) == EINVAL);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_tlv(0xa2U, b_text("a"))) == EINVAL);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_str(0x84U, "a")) == EINVAL);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_str(0xc2U, "a")) == EINVAL);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_str(0x42U, "a")) == EINVAL);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_str(0x81U, "a")) == ALLOW);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_hex("bf8100")) == EINVAL);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_cat(g_dns("a"), b_hex("82"))) == EINVAL);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), b_cat(g_dns("a"), b_hex("8205aa"))) == EINVAL);

	/* Subtrees whose base is not a GeneralName. */
	CHECK(nc(s_sub(b_str(0x02U, "a")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(b_str(0x89U, "a")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(b_tlv(0xa2U, b_text("a"))), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(b_str(0x84U, "a")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(b_str(0x41U, "a")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(s_sub(b_hex("")), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(b_str(0x04U, "a"), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(b_hex("30"), NULL, n_empty(), NULL) == EINVAL);
	CHECK(nc(b_hex("3003820161"), NULL, n_empty(), NULL) == ALLOW);
	CHECK(nc(NULL, b_hex("3003820161"), n_empty(), NULL) == ALLOW);
	CHECK(nc(b_hex("3003820161"), NULL, n_empty(), g_dns("a")) == ALLOW);
	CHECK(nc(b_hex("3003820161"), NULL, n_empty(), g_dns("b")) == DENY);

	/* A minimum of 0 after the base is ignored. */
	CHECK(nc(b_tlv(0x30U, b_cat(g_dns("example.com"), b_hex("800100"))), NULL, n_empty(), g_dns("a.example.com")) == ALLOW);

	/* Every position of a valid list of two subtrees changed to another value makes the check come out as allowed, refused or malformed without a crash. */
	subtrees = b_cat(s_sub(g_dns("example.com")), s_sub(g_ip_hex("0a000000ff000000")));
	for (index = 0; index < subtrees->size; index++) {
		/* Changes one byte of the list and runs the check on it. */
		memcpy(flipped, subtrees->data, subtrees->size);
		flipped[index] = (unsigned char)(flipped[index] ^ 0x5aU);
		copy = b_raw(flipped, subtrees->size);
		result = nc(copy, NULL, n_empty(), b_cat(g_dns("a.example.com"), g_ip4(10, 0, 0, 1)));
		CHECK(result == ALLOW || result == DENY || result == EINVAL || result == ENOTSUP);
	}

	/* Preconditions: a constraining certificate without the flag, missing certificates, a list with a size but no bytes. */
	make_ca(&ca, s_sub(g_dns("example.com")), NULL);
	make_cert(&leaf, n_empty(), g_dns("a.example.com"));
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == 0);
	CHECK(cryc_name_constraints_check(NULL, &leaf, 1) == EINVAL);
	CHECK(cryc_name_constraints_check(&ca, NULL, 1) == EINVAL);
	ca.flags = 0;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == EINVAL);
	make_ca(&ca, s_sub(g_dns("example.com")), NULL);
	ca.permitted = NULL;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == EINVAL);
	ca.permitted_size = 0;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == 0);
	make_ca(&ca, NULL, s_sub(g_dns("example.com")));
	ca.excluded = NULL;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == EINVAL);

	/* Neither list present: nothing is constrained. */
	make_ca(&ca, NULL, NULL);
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == 0);
	make_cert(&leaf, n_spec("C=US"), b_cat(g_dns("a"), g_uri("b")));
	CHECK(cryc_name_constraints_check(&ca, &leaf, 0) == 0);

	/* A certificate whose subject is absent and has no SAN. */
	make_ca(&ca, s_sub(g_dns("example.com")), NULL);
	make_cert(&leaf, NULL, NULL);
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == 0);

	/* A subject with no bytes and a pointer has no name. */
	make_cert(&leaf, n_spec("C=US"), NULL);
	leaf.subject_size = 0;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == 0);
	make_ca(&ca, s_sub(g_dir(n_spec("C=FR"))), NULL);
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == 0);
	make_ca(&ca, s_sub(g_dns("example.com")), NULL);

	/* Names of the forms that are not evaluated do not count when no constraint of the form is there. */
	subtrees = b_cat(g_dns("a.example.com"), b_cat(other, b_cat(b_tlv(TAG_X400, b_hex("3000")), b_cat(b_tlv(TAG_EDI, b_hex("3000")), b_cat(g_uri("https://a/"), b_str(TAG_RID, "\x2a\x03"))))));
	CHECK(nc(s_sub(g_dns("example.com")), NULL, n_empty(), subtrees) == ALLOW);
	CHECK(nc(NULL, s_sub(g_dns("example.com")), n_empty(), b_cat(other, g_dns("a.example.org"))) == ALLOW);
	CHECK(nc(s_sub(g_ip_hex("0a000000ff000000")), NULL, n_empty(), subtrees) == ALLOW);
	CHECK(nc(s_sub(g_dir(n_spec("C=US"))), NULL, n_empty(), subtrees) == ALLOW);
	CHECK(nc(s_sub(g_mail("example.com")), NULL, n_empty(), subtrees) == ALLOW);

	/* Names in the SAN bytes are not read when the SAN flag is off. */
	make_cert(&leaf, NULL, b_cat(g_dns("a..b"), g_dns("other.org")));
	leaf.flags = 0;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == 0);

	/* A SAN flag with no bytes has no names. */
	make_cert(&leaf, NULL, NULL);
	leaf.flags |= CRYC_X509_HAS_SAN;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == 0);

	/* Gives back everything this test function built. */
	mark_release(mark);
}

/* Writes count subtrees "<format with %u>" into dest, then the subtree last when it is not NULL; returns the size. */
static size_t
big_subtrees(
	unsigned char *dest,
	unsigned count,
	const char *format,
	const char *last)
{
	struct der_writer writer;
	struct der_writer inner;
	unsigned char scratch[64];
	char name[48];
	unsigned index;

	/* Starts the list. */
	der_init(&writer, dest, BIG_MAX);

	/* Writes the numbered subtrees. */
	for (index = 0; index < count; index++) {
		/* Writes one subtree of a dNSName named by the format and the number. */
		sprintf(name, format, index);
		der_init(&inner, scratch, sizeof(scratch));
		der_put(&inner, TAG_DNS, (const unsigned char *)name, strlen(name));
		der_wrap(&writer, 0x30U, &inner);
	}

	/* Writes the last subtree when there is one. */
	if (last != NULL) {
		der_init(&inner, scratch, sizeof(scratch));
		der_put(&inner, TAG_DNS, (const unsigned char *)last, strlen(last));
		der_wrap(&writer, 0x30U, &inner);
	}

	/* The lists must fit their room. */
	CHECK(writer.error == 0);

	/* Reports how many bytes the list takes. */
	return writer.used;
}

/* The budget of comparisons, tested at its edge. */
static void
test_nc_budget(void)
{
	struct mark mark;
	struct bytes *permitted;
	struct bytes *san;
	struct cryc_x509 ca;
	struct cryc_x509 leaf;
	char name[32];
	unsigned count;
	unsigned names;
	size_t used;
	size_t excluded_size;
	struct der_writer writer;

	/* Remembers the pool and the copies, to give them back at the end. */
	mark = mark_take();

	/* 64 subtrees that do not match, then one that does: a name costs 65 comparisons, so 63 names cost 4095. */
	permitted = b_new();
	for (count = 0; count < 64U; count++) {
		sprintf(name, "n%u.example", count);
		permitted = b_cat(permitted, s_sub(g_dns(name)));
	}

	/* Adds the subtree that matches after the ones that do not. */
	permitted = b_cat(permitted, s_sub(g_dns("match.example")));

	/* Builds a SAN of 63 names that all match the last subtree. */
	san = b_new();
	for (names = 0; names < 63U; names++)
		san = b_cat(san, g_dns("a.match.example"));

	/* Checks the names against the list. */
	CHECK(nc(permitted, NULL, n_empty(), san) == ALLOW);

	/* 64 names cost 4160: refused as too much work. */
	CHECK(nc(permitted, NULL, n_empty(), b_cat(san, g_dns("a.match.example"))) == ENOTSUP);

	/* The edge itself: 4096 comparisons are allowed and the 4097th is not.  63 names use 4095; one more name matching the first subtree uses 1 (4096), matching the second uses 2 (4097), in either place. */
	CHECK(nc(permitted, NULL, n_empty(), b_cat(san, g_dns("a.n0.example"))) == ALLOW);
	CHECK(nc(permitted, NULL, n_empty(), b_cat(san, g_dns("a.n1.example"))) == ENOTSUP);
	CHECK(nc(permitted, NULL, n_empty(), b_cat(g_dns("a.n1.example"), san)) == ENOTSUP);
	CHECK(nc(permitted, NULL, n_empty(), b_cat(g_dns("a.n0.example"), san)) == ALLOW);

	/* The budget is shared by the two lists: an excluded list of 2048 subtrees that never hit costs 2048, the permitted list of 2048 then fits exactly (4096) and one more does not. */
	excluded_size = big_subtrees(big_san, 2048, "e%u.excl", NULL);
	used = big_subtrees(big_permitted, 2047, "p%u.perm", "match.example");
	memset(&ca, 0, sizeof(ca));
	ca.flags = CRYC_X509_HAS_NAME_CONSTRAINTS;
	ca.permitted = big_permitted;
	ca.permitted_size = used;
	ca.excluded = big_san;
	ca.excluded_size = excluded_size;
	make_cert(&leaf, n_empty(), g_dns("a.match.example"));
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == ALLOW);
	used = big_subtrees(big_permitted, 2048, "p%u.perm", "match.example");
	ca.permitted_size = used;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == ENOTSUP);

	/* Without the excluded list the larger permitted list fits. */
	ca.excluded = NULL;
	ca.excluded_size = 0;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == ALLOW);

	/* A hit in the excluded list stops its walk: the name costs only what was compared. */
	used = big_subtrees(big_permitted, 2047, "p%u.perm", "match.example");
	excluded_size = big_subtrees(big_san, 2047, "e%u.excl", "match.example");
	ca.permitted = big_permitted;
	ca.permitted_size = used;
	ca.excluded = big_san;
	ca.excluded_size = excluded_size;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == DENY);

	/* Subtrees of other forms in the list cost nothing: 64 dNSName subtrees that do not match, 100 iPAddress subtrees, then the dNSName that matches. */
	permitted = b_new();
	for (count = 0; count < 64U; count++) {
		sprintf(name, "n%u.example", count);
		permitted = b_cat(permitted, s_sub(g_dns(name)));
	}

	/* Adds the iPAddress subtrees. */
	for (count = 0; count < 100U; count++)
		permitted = b_cat(permitted, s_sub(g_ip_hex("0a000000ff000000")));

	/* Adds the dNSName subtree that matches. */
	permitted = b_cat(permitted, s_sub(g_dns("match.example")));

	/* Builds a SAN of 63 names that all match the last dNSName subtree. */
	san = b_new();
	for (names = 0; names < 63U; names++)
		san = b_cat(san, g_dns("a.match.example"));

	/* Checks the names against the list. */
	CHECK(nc(permitted, NULL, n_empty(), san) == ALLOW);
	CHECK(nc(permitted, NULL, n_empty(), b_cat(san, g_dns("a.n1.example"))) == ENOTSUP);

	/* An excluded list that runs out of budget is an error on its own, with no permitted list to hide it. */
	used = big_subtrees(big_permitted, 4096, "e%u.excl", NULL);
	memset(&ca, 0, sizeof(ca));
	ca.flags = CRYC_X509_HAS_NAME_CONSTRAINTS;
	ca.excluded = big_permitted;
	ca.excluded_size = used;
	make_cert(&leaf, n_empty(), g_dns("a.match.example"));
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == ALLOW);
	used = big_subtrees(big_permitted, 4097, "e%u.excl", NULL);
	ca.excluded_size = used;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == ENOTSUP);

	/* The pathological chain: 2049 permitted subtrees and 2049 names that all match the last subtree cost more than the budget, and 2049 names that match none are refused at the first. */
	used = big_subtrees(big_permitted, 2048, "t%u.test", "tlast.test");
	ca.permitted = big_permitted;
	ca.permitted_size = used;
	ca.excluded = NULL;
	ca.excluded_size = 0;
	der_init(&writer, big_san, BIG_MAX);
	for (count = 0; count < 2049U; count++)
		der_put(&writer, TAG_DNS, (const unsigned char *)"tlast.test", 10U);

	/* The names must fit their room. */
	CHECK(writer.error == 0);

	/* Builds the certificate whose SAN is that list of names. */
	memset(&leaf, 0, sizeof(leaf));
	leaf.flags = CRYC_X509_HAS_SAN;
	leaf.san = big_san;
	leaf.san_size = writer.used;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == ENOTSUP);
	der_init(&writer, big_san, BIG_MAX);
	for (count = 0; count < 2049U; count++) {
		sprintf(name, "x%u.nomatch", count);
		der_put(&writer, TAG_DNS, (const unsigned char *)name, strlen(name));
	}

	/* The names must fit their room. */
	CHECK(writer.error == 0);

	/* Makes the list of names that match none the SAN of the certificate. */
	leaf.san_size = writer.used;
	CHECK(cryc_name_constraints_check(&ca, &leaf, 1) == DENY);

	/* Gives back everything this test function built. */
	mark_release(mark);
}

/* ------------------------------------------------------------------ */
/* x509-limbo                                                           */
/* ------------------------------------------------------------------ */

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
	const char *colon;
	int double_colon;
	unsigned groups[8];
	unsigned count;

	/* A text without a colon is IPv4: four decimal numbers separated by dots. */
	colon = strchr(text, ':');
	if (colon == NULL) {
		at = text;
		for (index = 0; index < 4U; index++) {
			/* Reads the digits of one number: at most three, and at most 255. */
			value = 0;
			digits = 0;
			while (*at >= '0' && *at <= '9') {
				value = value * 10U + (unsigned)(*at - '0');
				digits++;
				at++;
				if (value > 255U || digits > 3U)
					return 0;
			}

			/* A number has at least one digit. */
			if (digits == 0U)
				return 0;
			out[index] = (unsigned char)value;

			/* A dot separates a number from the next one. */
			if (index < 3U) {
				if (*at != '.')
					return 0;
				at++;
			}
		}

		/* Nothing may follow the fourth number. */
		if (*at != '\0')
			return 0;

		/* Succeeded: an IPv4 address of 4 octets. */
		return 4;
	}

	/* IPv6: groups of up to four hex digits, with at most one "::" standing for zero groups. */
	head = 0;
	tail = 0;
	double_colon = 0;
	count = 0;
	at = text;

	/* A leading "::" is the double colon; a single leading colon is wrong. */
	if (at[0] == ':' && at[1] == ':') {
		double_colon = 1;
		at += 2;
	} else if (at[0] == ':') {
		return 0;
	}

	/* Reads the groups one by one. */
	while (*at != '\0') {
		/* Reads the hex digits of one group. */
		value = 0;
		digits = 0;
		while ((*at >= '0' && *at <= '9') ||
		       (*at >= 'a' && *at <= 'f') ||
		       (*at >= 'A' && *at <= 'F')) {
			if (*at <= '9')
				value = value * 16U + (unsigned)(*at - '0');
			else if (*at >= 'a')
				value = value * 16U + (unsigned)(*at - 'a' + 10);
			else
				value = value * 16U + (unsigned)(*at - 'A' + 10);
			digits++;
			at++;
		}

		/* A group has one to four digits, and there are at most eight groups. */
		if (digits == 0U ||
		    digits > 4U ||
		    count >= 8U)
			return 0;

		/* Keeps the group, counted before or after the double colon. */
		groups[count++] = value;
		if (double_colon)
			tail++;
		else
			head++;

		/* A colon separates a group from the next; two colons are the double colon. */
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

	/* A double colon stands for at least one group; without one there are exactly eight. */
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

	/* Writes the eight groups as 16 octets. */
	for (index = 0; index < 8U; index++) {
		out[index * 2U] = (unsigned char)(parts[index] >> 8);
		out[index * 2U + 1U] = (unsigned char)(parts[index] & 0xffU);
	}

	/* Succeeded: an IPv6 address of 16 octets. */
	return 16;
}

/* Checks the IP parser on the forms the cases use. */
static void
test_parse_ip(void)
{
	unsigned char out[16];
	unsigned char expected[16];

	/* IPv4 addresses: the largest numbers, a number over 255, and the wrong count of numbers. */
	CHECK(parse_ip("192.0.2.1", out) == 4U && out[0] == 192 && out[1] == 0 && out[2] == 2 && out[3] == 1);
	CHECK(parse_ip("255.255.255.255", out) == 4U && out[3] == 255);
	CHECK(parse_ip("256.0.0.1", out) == 0U);
	CHECK(parse_ip("1.2.3", out) == 0U);
	CHECK(parse_ip("1.2.3.4.5", out) == 0U);

	/* IPv6 addresses with the double colon at the start. */
	memset(expected, 0, sizeof(expected));
	expected[15] = 1;
	CHECK(parse_ip("::1", out) == 16U && memcmp(out, expected, 16) == 0);
	memset(expected, 0, sizeof(expected));
	CHECK(parse_ip("::", out) == 16U && memcmp(out, expected, 16) == 0);

	/* IPv6 addresses with the double colon in the middle, and written out in full. */
	memset(expected, 0, sizeof(expected));
	expected[0] = 0x20;
	expected[1] = 0x01;
	expected[2] = 0x0d;
	expected[3] = 0xb8;
	expected[14] = 0x73;
	expected[15] = 0x34;
	expected[12] = 0x03;
	expected[13] = 0x70;
	expected[10] = 0x8a;
	expected[11] = 0x2e;
	CHECK(parse_ip("2001:db8::8a2e:370:7334", out) == 16U && memcmp(out, expected, 16) == 0);
	CHECK(parse_ip("2001:0db8:0000:0000:0000:8a2e:0370:7334", out) == 16U && memcmp(out, expected, 16) == 0);

	/* Forms that are not addresses. */
	CHECK(parse_ip("2001:db8:::1", out) == 0U);
	CHECK(parse_ip(":1", out) == 0U);
	CHECK(parse_ip("1:2:3:4:5:6:7", out) == 0U);
	CHECK(parse_ip("1::2::3", out) == 0U);
}

/* Reads the certificates of one record into a case. */
static void
limbo_load(
	const struct vec_file *file,
	struct limbo_case *chain)
{
	unsigned index;
	char key[32];
	size_t size;
	unsigned char *der;
	const char *kind;
	unsigned slot;
	int order;
	int error;

	/* Takes a buffer for the largest certificate. */
	der = malloc(LIMBO_MAX);

	/* Reads the peer, the trusted and the untrusted certificates in that order. */
	chain->count = 0;
	for (index = 0; index < CHAIN_MAX; index++) {
		/* Names the key of the certificate and its kind. */
		if (index == 0U) {
			strcpy(key, "peer");
			kind = "peer";
		} else if (index <= 4U) {
			sprintf(key, "trusted.%u", index - 1U);
			kind = "trusted";
		} else {
			sprintf(key, "untrusted.%u", index - 5U);
			kind = "untrusted";
		}

		/* Skips a certificate the record does not have. */
		size = vec_hex(file, key, der, LIMBO_MAX);
		if (size == (size_t)-1)
			continue;

		/* Keeps a copy of the certificate in the case. */
		slot = chain->count++;
		chain->der[slot] = malloc(size);
		memcpy(chain->der[slot], der, size);

		/* Records whether the certificate is trusted. */
		order = strcmp(kind, "trusted");
		chain->trusted[slot] = 0;
		if (order == 0)
			chain->trusted[slot] = 1;

		/* Records whether the certificate parses. */
		error = cryc_x509_parse(&chain->cert[slot], chain->der[slot], size);
		chain->parsed[slot] = 0;
		if (error == 0)
			chain->parsed[slot] = 1;

		/* A record whose peer is missing would put another certificate first. */
		if (index == 0U && slot != 0U)
			printf("FAIL limbo case without a peer\n");
	}

	/* Gives back the buffer. */
	free(der);
}

/* Frees what limbo_load allocated. */
static void
limbo_free(
	struct limbo_case *chain)
{
	unsigned index;

	/* Frees the DER copy of every certificate. */
	for (index = 0; index < chain->count; index++)
		free(chain->der[index]);

	/* Empties the case. */
	chain->count = 0;
}

/*
 * Builds the chain from the peer up when each step has exactly one
 * certificate whose subject equals the issuer of the one below and the
 * chain ends in a trusted one; writes the indexes, peer first, and
 * returns the length, or 0 when a step has none or has a choice.
 */
static unsigned
limbo_build_chain(
	const struct limbo_case *chain,
	unsigned *order)
{
	unsigned current;
	unsigned candidate;
	unsigned found;
	unsigned length;
	unsigned step;
	unsigned seen;
	unsigned index;
	unsigned previous;
	int same_name;

	/* A case whose peer does not parse has no chain. */
	if (!chain->parsed[0])
		return 0;

	/* Starts the chain with the peer. */
	order[0] = 0;
	length = 1;
	current = 0;

	/* Climbs from the peer through the issuers, at most 15 steps. */
	for (step = 0; step < 15U; step++) {
		/* The chain is done when it has reached a trusted certificate. */
		if (chain->trusted[current])
			return length;

		/* Counts the certificates, not yet in the chain, whose subject is the issuer of the current one. */
		found = 0;
		candidate = 0;
		for (index = 0; index < chain->count; index++) {
			if (!chain->parsed[index])
				continue;
			same_name = cryc_name_equal(chain->cert[current].issuer, chain->cert[current].issuer_size, chain->cert[index].subject, chain->cert[index].subject_size);
			if (!same_name)
				continue;

			/* Looks the certificate up in the chain. */
			seen = 0;
			for (previous = 0; previous < length; previous++) {
				if (order[previous] == index)
					seen = 1;
			}

			/* A certificate already in the chain is not an issuer to choose. */
			if (seen)
				continue;

			/* Counts the issuer and remembers it. */
			found++;
			candidate = index;
		}

		/* No issuer, or a choice between several, ends the attempt. */
		if (found != 1U)
			return 0;

		/* Adds the one issuer to the chain and climbs to it. */
		order[length++] = candidate;
		current = candidate;
	}

	/* Gives up: no trusted certificate within 15 steps. */
	return 0;
}

/* Applies the name constraints of every certificate of a chain to the certificates below it; returns 0 when all are allowed, else the first error. */
static int
limbo_apply_constraints(
	const struct limbo_case *chain,
	const unsigned *order,
	unsigned length)
{
	unsigned above;
	unsigned below;
	int error;
	int self_issued;
	int is_leaf;
	const struct cryc_x509 *lower;

	/* From the trust anchor down; self-issued certificates in the middle of the path are not checked (RFC 5280 6.1.3). */
	for (above = length - 1U; above > 0U; above--) {
		/* Skips a certificate without name constraints. */
		if ((chain->cert[order[above]].flags & CRYC_X509_HAS_NAME_CONSTRAINTS) == 0U)
			continue;

		/* Applies its constraints to every certificate below it. */
		for (below = above; below > 0U; below--) {
			lower = &chain->cert[order[below - 1U]];
			self_issued = cryc_name_equal(lower->issuer, lower->issuer_size, lower->subject, lower->subject_size);

			/* Skips a self-issued certificate that is not the leaf. */
			if (self_issued && below - 1U != 0U)
				continue;

			/* Checks the certificate below; only the peer is the leaf. */
			is_leaf = 0;
			if (below - 1U == 0U)
				is_leaf = 1;
			error = cryc_name_constraints_check(&chain->cert[order[above]], lower, is_leaf);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: every certificate is allowed. */
	return 0;
}

/* Looks up the expected match of a case id; returns -1 when the id is not in the list. */
static int
limbo_name_expected(
	const char *id)
{
	unsigned index;
	int order;

	/* Looks the id up in the table. */
	for (index = 0; limbo_name_cases[index].id != NULL; index++) {
		order = strcmp(limbo_name_cases[index].id, id);
		if (order == 0)
			return limbo_name_cases[index].matches;
	}

	/* The id is not in the table. */
	return -1;
}

/* Matches the peer name of a case against the peer certificate; returns 1 or 0, and -1 when the case has no name or it cannot be read. */
static int
limbo_match_peer(
	const struct vec_file *file,
	const struct limbo_case *chain)
{
	const char *kind;
	const char *value;
	unsigned char address[16];
	size_t size;
	char normal[CRYC_NAME_HOST_MAX + 1U];
	int order;
	int error;
	int result;

	/* Reads the kind and the value of the peer name. */
	kind = vec_get(file, "peer_kind");
	value = vec_get(file, "peer_value");

	/* A case with no name, or no peer certificate that parses, has nothing to match. */
	if (kind == NULL ||
	    value == NULL ||
	    chain->count == 0U ||
	    !chain->parsed[0])
		return -1;

	/* A DNS name is normalized and matched as a host name. */
	order = strcmp(kind, "DNS");
	if (order == 0) {
		error = cryc_name_host_normalize(value, normal);
		if (error != 0)
			return -1;
		result = cryc_name_match_host(&chain->cert[0], value);

		/* Reports whether the host name matches. */
		return result;
	}

	/* Any other kind is an IP address; a text that is not one cannot be read. */
	size = parse_ip(value, address);
	if (size == 0U)
		return -1;

	/* Matches the address against the peer certificate. */
	result = cryc_name_match_ip(&chain->cert[0], address, size);

	/* Reports whether the address matches. */
	return result;
}

/* Runs the name constraints of the chain of one case and compares with the case. */
static void
limbo_check_constraints(
	const struct vec_file *file,
	const struct limbo_case *chain,
	int name_match)
{
	unsigned order[16];
	unsigned length;
	const char *id;
	const char *expected;
	int constraints;
	int allowed;
	int expect_success;
	unsigned index;
	int is_nc_case;
	int compared;
	int prefix_better;
	int prefix_rfc;
	int prefix_webpki;
	int prefix_cve;
	int prefix_pathological;

	/* Reads the id of the case and what it expects. */
	id = vec_get(file, "id");
	expected = vec_get(file, "expected");
	compared = strcmp(expected, "SUCCESS");
	expect_success = 0;
	if (compared == 0)
		expect_success = 1;

	/* Builds the chain of the case; a case with no chain that can be built without a choice is skipped. */
	length = limbo_build_chain(chain, order);
	limbo_chains++;
	if (length == 0U) {
		limbo_chain_skipped++;
		return;
	}

	/* Applies the name constraints of the chain, and decides whether the case is allowed by them and by its name. */
	constraints = limbo_apply_constraints(chain, order, length);
	allowed = 0;
	if (constraints == 0 && name_match != 0)
		allowed = 1;

	/* Every SUCCESS case must pass its name constraints, and match its name when it has one. */
	if (expect_success) {
		limbo_nc_success_chains++;
		if (constraints != 0)
			printf("success case refused by name constraints: %s (%d)\n", id, constraints);
		CHECK(constraints == 0);
	}

	/* Finds out whether the case is about name constraints, by the prefix of its id. */
	prefix_better = strncmp(id, "bettertls::nameconstraints", 26);
	prefix_rfc = strncmp(id, "rfc5280::nc::", 13);
	prefix_webpki = strncmp(id, "webpki::nc::", 12);
	prefix_cve = strncmp(id, "cve::", 5);
	prefix_pathological = strncmp(id, "pathological::nc-dos", 20);
	is_nc_case = 0;
	if (prefix_better == 0)
		is_nc_case = 1;
	else if (prefix_rfc == 0)
		is_nc_case = 1;
	else if (prefix_webpki == 0)
		is_nc_case = 1;
	else if (prefix_cve == 0)
		is_nc_case = 1;
	else if (prefix_pathological == 0)
		is_nc_case = 1;

	/* The other cases are not about name constraints and are not judged here. */
	if (!is_nc_case)
		return;

	/* The cases about name constraints must come out as the case says, but for the listed ones. */
	limbo_nc_cases++;
	for (index = 0; limbo_nc_exceptions[index].id != NULL; index++) {
		compared = strcmp(limbo_nc_exceptions[index].id, id);
		if (compared == 0) {
			limbo_nc_explained++;
			CHECK((constraints == 0) == (limbo_nc_exceptions[index].check_result == 0));
			return;
		}
	}

	/* A case with no name to match counts as matching. */
	if (name_match < 0)
		name_match = 1;

	/* Counts the cases the constraints refuse, and the ones only the name refuses. */
	if (constraints != 0)
		limbo_nc_refused++;
	if (constraints == 0 && name_match == 0)
		limbo_nc_refused_by_name++;

	/* Counts the case as agreeing when it comes out as the case says; prints the ones that do not. */
	if (allowed == expect_success) {
		limbo_nc_agree++;
	} else {
		limbo_nc_unexplained++;
		printf("name constraint case differs: %s expected %s, constraints %d, name match %d\n", id, expected, constraints, name_match);
	}
}

/* Runs the name matching and the name constraints over every SERVER case of x509-limbo. */
static void
test_limbo(void)
{
	struct vec_file file;
	struct limbo_case *chain;
	const char *expected;
	const char *id;
	int match;
	int known;
	int success;
	int compared;
	int open_error;
	int more;

	/* Takes room for one case. */
	chain = malloc(sizeof(*chain));
	CHECK(chain != NULL);
	if (chain == NULL)
		return;

	/* Opens the records of x509-limbo. */
	open_error = vec_open(&file, "limbo");
	if (open_error != 0) {
		CHECK(0);
		free(chain);
		return;
	}

	/* Runs the name matching and the name constraints on every record. */
	more = vec_next(&file);
	while (more) {
		/* Reads what the record expects. */
		limbo_cases++;
		id = vec_get(&file, "id");
		expected = vec_get(&file, "expected");
		compared = strcmp(expected, "SUCCESS");
		success = 0;
		if (compared == 0)
			success = 1;
		if (success)
			limbo_success_cases++;

		/* Loads the certificates of the record and counts a peer that does not parse. */
		limbo_load(&file, chain);
		if (chain->count == 0U || !chain->parsed[0])
			limbo_unparsed_peer++;

		/* Matches the peer name of the record against the peer certificate. */
		match = limbo_match_peer(&file, chain);
		if (match >= 0)
			limbo_peer_names++;

		/* A SUCCESS case needs the peer name to match the peer certificate. */
		if (success && match >= 0) {
			if (match != 1)
				printf("success case with a name that does not match: %s\n", id);
			CHECK(match == 1);
			limbo_success_matched++;
		}

		/* Counts the cases that fail for another reason though their name matches. */
		if (!success && match == 1)
			limbo_failure_matched++;

		/* The cases whose id says why they fail by name. */
		known = limbo_name_expected(id);
		if (known >= 0) {
			if (match != known)
				printf("name case differs: %s expected match %d got %d\n", id, known, match);
			CHECK(match == known);
		}

		/* Applies the name constraints of the chain and frees the case. */
		limbo_check_constraints(&file, chain, match);
		limbo_free(chain);

		/* Reads the next record. */
		more = vec_next(&file);
	}

	/* Closes the records and gives back the room. */
	vec_close(&file);
	free(chain);

	/* Prints the statistics of the run. */
	printf("limbo: %lu cases, %lu SUCCESS, %lu with a peer name, %lu SUCCESS names matched, %lu FAILURE names matched, %lu unparsed peers\n", limbo_cases, limbo_success_cases, limbo_peer_names, limbo_success_matched, limbo_failure_matched, limbo_unparsed_peer);
	printf("limbo chains: %lu tried, %lu skipped, %lu SUCCESS chains checked; name constraint cases %lu: %lu agree, %lu listed, %lu differ; %lu refused by the constraints, %lu by the name only\n", limbo_chains, limbo_chain_skipped, limbo_nc_success_chains, limbo_nc_cases, limbo_nc_agree, limbo_nc_explained, limbo_nc_unexplained, limbo_nc_refused, limbo_nc_refused_by_name);

	/* The statistics must be those of the corpus. */
	CHECK(limbo_cases == 9792UL);
	CHECK(limbo_success_cases == 930UL);
	CHECK(limbo_peer_names == 9784UL);
	CHECK(limbo_success_matched == 929UL);
	CHECK(limbo_failure_matched == 2266UL);
	CHECK(limbo_unparsed_peer == 7UL);
	CHECK(limbo_chain_skipped == 99UL);
	CHECK(limbo_nc_success_chains == 880UL);
	CHECK(limbo_nc_cases == 9538UL);
	CHECK(limbo_nc_agree == 9534UL);
	CHECK(limbo_nc_explained == 4UL);
	CHECK(limbo_nc_unexplained == 0UL);
	CHECK(limbo_nc_refused == 5863UL);
	CHECK(limbo_nc_refused_by_name == 2840UL);
}
