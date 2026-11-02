/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs every SERVER case of x509-limbo through cryc_chain_verify and
 * compares the outcome with the case's expectation.
 *
 * Each case's trusted certificates are the store, the peer and its
 * untrusted intermediates the server's list, and the case's time, peer
 * name or address and depth the parameters.  A case whose outcome differs
 * from its expectation must be in the table below with the reason; a
 * listed case that does not differ is a failure too, so the table stays
 * the exact list of differences.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-chain.h"

/* The most certificates this test hands to one validation: the peer and 15 more, what a server may send. */
#define PEER_MAX	16U

/* The largest certificate of the x509-limbo cases is about 78 KB. */
#define LIMBO_MAX	131072U

/* The most trusted and untrusted certificates one case lists. */
#define TRUSTED_MAX	8U
#define UNTRUSTED_MAX	120U

/* The number of SERVER cases of the pinned limbo.json. */
#define LIMBO_CASES	9792UL

/* The largest code a result may hold. */
#define CODE_LAST	((int)CRYC_VERIFY_REJECTED)

/*
 * One case whose outcome here differs from its expectation: the id, and
 * the reason, one of the words of reason_words.
 */
struct limbo_exception {
	const char *id;
	const char *reason;
	int seen;
};

/* The count of one reason, for the summary. */
struct reason_count {
	const char *reason;
	unsigned long count;
};

/* The count of one id prefix (the part before the first "::"), for the summary. */
struct prefix_count {
	char prefix[32];
	unsigned long cases;
	unsigned long agree;
	unsigned long explained;
	unsigned long unexplained;
};

/*
 * The reasons a case may differ.  Each is a decision of the library's
 * design or header, not a fault:
 *
 *   issuance        the case faults a rule RFC 5280 sets for the CAs that
 *                   issue certificates ("conforming CAs MUST"), which
 *                   path validation (6.1) does not check: the presence of
 *                   AKI and SKI, the criticality of AKI, basicConstraints,
 *                   policyConstraints and subjectAltName, keyCertSign
 *                   without cA, a zero serial number.
 *   cabf            the case faults a rule of the CA/Browser Forum
 *                   Baseline Requirements (the webpki profile) that RFC
 *                   5280 does not have: the contents of a root's AKI, the
 *                   common name (never used as a host name here), EKU
 *                   presence and contents, RSA sizes not divisible by 8,
 *                   cA TRUE in a leaf, the contents of AIA (not read), SAN
 *                   criticality with a subject.
 *   anchor          the case faults the trust anchor's own contents,
 *                   which are taken as they are (RFC 5280 6.1.1 (d),
 *                   cryc-chain.h): the same fault in an intermediate is
 *                   refused.
 *   name            the case faults a name rule that cryc-name.h defines
 *                   otherwise: a leading dot in a dNSName constraint
 *                   means the names below it, '_' is accepted in labels,
 *                   no public suffix list is consulted.
 *   key-usage       a leaf whose keyUsage lacks digitalSignature is
 *                   refused (cryc-chain.h: TLS signs with the leaf's key);
 *                   the case accepts it.
 *   depth           the case counts the depth without self-issued
 *                   intermediates; the header counts every certificate.
 *   nc-noncritical  nameConstraints that are not marked critical are
 *                   processed; the case refuses them for the criticality.
 *   nc-leaf         the leaf's own nameConstraints constrain nothing
 *                   (6.1.4 (g) processes those of CA certificates only).
 *   no-crl          revocation is not checked (no CRL, no OCSP).
 *   no-mldsa        ML-DSA keys and signatures are not supported.
 */
static const char *const reason_words[] = {
	"issuance",
	"cabf",
	"anchor",
	"name",
	"key-usage",
	"depth",
	"nc-noncritical",
	"nc-leaf",
	"no-crl",
	"no-mldsa",
	NULL
};

/* The cases that differ, with the reason (sorted by id). */
static struct limbo_exception limbo_exceptions[] = {
	{ "crl::crlnumber-critical", "no-crl", 0 },
	{ "crl::crlnumber-missing", "no-crl", 0 },
	{ "crl::issuer-missing-crlsign", "no-crl", 0 },
	{ "crl::revoked-certificate-with-crl", "no-crl", 0 },
	{ "crl::structure::crl-duplicate-revoked-serial", "no-crl", 0 },
	{ "crl::structure::crl-invalid-signature", "no-crl", 0 },
	{ "crl::structure::crl-unknown-critical-extension", "no-crl", 0 },
	{ "crl::structure::crl-wrong-signing-key", "no-crl", 0 },
	{ "crl::structure::entry-unknown-critical-extension", "no-crl", 0 },
	{ "crl::structure::entry-unknown-noncritical-extension", "no-crl", 0 },
	{ "pathlen::max-chain-depth-1-self-issued", "depth", 0 },
	{ "pathlen::validation-ignores-pathlen-in-leaf", "key-usage", 0 },
	{ "rfc5280::aki::critical-aki", "issuance", 0 },
	{ "rfc5280::aki::cross-signed-root-missing-aki", "issuance", 0 },
	{ "rfc5280::aki::intermediate-missing-aki", "issuance", 0 },
	{ "rfc5280::aki::leaf-missing-aki", "issuance", 0 },
	{ "rfc5280::ca-as-leaf", "key-usage", 0 },
	{ "rfc5280::leaf-ku-keycertsign", "issuance", 0 },
	{ "rfc5280::nc::invalid-dnsname-leading-period", "name", 0 },
	{ "rfc5280::nc::not-allowed-in-ee-critical", "nc-leaf", 0 },
	{ "rfc5280::nc::not-allowed-in-ee-noncritical", "nc-leaf", 0 },
	{ "rfc5280::nc::permitted-dns-match-noncritical", "nc-noncritical", 0 },
	{ "rfc5280::pc::ica-noncritical-pc", "issuance", 0 },
	{ "rfc5280::root-missing-basic-constraints", "anchor", 0 },
	{ "rfc5280::root-non-critical-basic-constraints", "issuance", 0 },
	{ "rfc5280::san::noncritical-with-empty-subject", "issuance", 0 },
	{ "rfc5280::san::underscore-dns", "name", 0 },
	{ "rfc5280::serial::zero", "issuance", 0 },
	{ "rfc5280::ski::intermediate-missing-ski", "issuance", 0 },
	{ "rfc5280::ski::root-missing-ski", "issuance", 0 },
	{ "rfc9881::ml-dsa-44", "no-mldsa", 0 },
	{ "rfc9881::ml-dsa-65", "no-mldsa", 0 },
	{ "rfc9881::ml-dsa-87", "no-mldsa", 0 },
	{ "webpki::aki::root-with-aki-all-fields", "cabf", 0 },
	{ "webpki::aki::root-with-aki-authoritycertissuer", "cabf", 0 },
	{ "webpki::aki::root-with-aki-authoritycertserialnumber", "cabf", 0 },
	{ "webpki::aki::root-with-aki-missing-keyidentifier", "cabf", 0 },
	{ "webpki::aki::root-with-aki-ski-mismatch", "cabf", 0 },
	{ "webpki::cn::case-mismatch", "cabf", 0 },
	{ "webpki::cn::ipv4-hex-mismatch", "cabf", 0 },
	{ "webpki::cn::ipv4-leading-zeros-mismatch", "cabf", 0 },
	{ "webpki::cn::ipv6-non-rfc5952-mismatch", "cabf", 0 },
	{ "webpki::cn::ipv6-uncompressed-mismatch", "cabf", 0 },
	{ "webpki::cn::ipv6-uppercase-mismatch", "cabf", 0 },
	{ "webpki::cn::not-in-san", "cabf", 0 },
	{ "webpki::cn::punycode-not-in-san", "cabf", 0 },
	{ "webpki::cn::utf8-vs-punycode-mismatch", "cabf", 0 },
	{ "webpki::ee-basicconstraints-ca", "cabf", 0 },
	{ "webpki::eku::ee-anyeku", "cabf", 0 },
	{ "webpki::eku::ee-critical-eku", "cabf", 0 },
	{ "webpki::eku::ee-without-eku", "cabf", 0 },
	{ "webpki::eku::root-has-eku", "cabf", 0 },
	{ "webpki::forbidden-rsa-key-not-divisible-by-8-in-leaf", "cabf", 0 },
	{ "webpki::forbidden-rsa-not-divisible-by-8-in-root", "cabf", 0 },
	{ "webpki::malformed-aia", "cabf", 0 },
	{ "webpki::san::public-suffix-multi-label-wildcard-san", "name", 0 },
	{ "webpki::san::public-suffix-private-namespace-wildcard-san", "name", 0 },
	{ "webpki::san::san-critical-with-nonempty-subject", "cabf", 0 },
	{ NULL, NULL, 0 }
};

/* The names of the codes, for the report of a case that differs without a reason. */
static const char *const code_names[] = {
	"OK",
	"NO_PEER_CERTIFICATE",
	"MALFORMED",
	"UNABLE_TO_GET_ISSUER",
	"DEPTH_ZERO_SELF_SIGNED",
	"SELF_SIGNED_IN_CHAIN",
	"CHAIN_TOO_LONG",
	"SIGNATURE_FAILURE",
	"UNSUPPORTED_ALGORITHM",
	"KEY_TOO_SMALL",
	"NOT_YET_VALID",
	"EXPIRED",
	"INVALID_CA",
	"PATH_LENGTH_EXCEEDED",
	"KEY_USAGE",
	"INVALID_PURPOSE",
	"UNHANDLED_CRITICAL_EXTENSION",
	"NAME_CONSTRAINT_VIOLATION",
	"UNSUPPORTED_NAME_CONSTRAINT",
	"HOSTNAME_MISMATCH",
	"IP_ADDRESS_MISMATCH",
	"WORK_EXCEEDED",
	"REJECTED"
};

/* The counts of the run. */
static unsigned long limbo_cases;
static unsigned long limbo_agree;
static unsigned long limbo_explained;
static unsigned long limbo_unexplained;
static unsigned long limbo_success;
static unsigned long limbo_success_ok;
static unsigned long limbo_refused_unparsed;
static unsigned long limbo_truncated;
static unsigned long limbo_reduced;

/* The untrusted certificates of the case being run, and which of them are sent (static: about 120 parsed certificates). */
static struct cryc_x509 pool[UNTRUSTED_MAX];
static unsigned char *pool_der[UNTRUSTED_MAX];
static int keep[UNTRUSTED_MAX];

/* 1 when WS335_LIMBO_TRACE is set: every case's outcome is printed (for looking into a difference). */
static int trace;

/* The counts by id prefix and by reason. */
static struct prefix_count prefixes[16];
static unsigned prefix_used;
static struct reason_count reasons[16];

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

/* Returns the counts of the id's prefix, adding a row for a new prefix. */
static struct prefix_count *
prefix_of(
	const char *id)
{
	size_t length;
	unsigned index;
	const char *end;

	/* The prefix is the text before the first "::". */
	end = strstr(id, "::");
	if (end == NULL)
		length = strlen(id);
	else
		length = (size_t)(end - id);
	if (length >= sizeof(prefixes[0].prefix))
		length = sizeof(prefixes[0].prefix) - 1U;

	/* A known prefix. */
	for (index = 0; index < prefix_used; index++) {
		if (strlen(prefixes[index].prefix) == length && memcmp(prefixes[index].prefix, id, length) == 0)
			return &prefixes[index];
	}

	/* A new one, while there is room (the last row takes the rest). */
	if (prefix_used < sizeof(prefixes) / sizeof(prefixes[0])) {
		memcpy(prefixes[prefix_used].prefix, id, length);
		prefixes[prefix_used].prefix[length] = '\0';
		prefix_used++;
	}
	return &prefixes[prefix_used - 1U];
}

/* Marks in keep the certificates of pool that chain up from leaf by name: the subject of each is the issuer of the leaf or of another marked one. */
static void
keep_closure(
	const struct cryc_x509 *leaf,
	const struct cryc_x509 *candidates,
	size_t count,
	int *marks)
{
	size_t index;
	size_t below;
	int changed;
	int issues;

	/* Grows the set until no certificate is added. */
	changed = 1;
	while (changed != 0) {
		changed = 0;
		for (index = 0; index < count; index++) {
			if (marks[index] != 0)
				continue;

			/* Is it a possible issuer of the leaf or of a marked certificate? */
			issues = cryc_name_equal(candidates[index].subject, candidates[index].subject_size, leaf->issuer, leaf->issuer_size);
			for (below = 0; below < count && issues == 0; below++) {
				if (marks[below] == 0)
					continue;
				issues = cryc_name_equal(candidates[index].subject, candidates[index].subject_size, candidates[below].issuer, candidates[below].issuer_size);
			}
			if (issues != 0) {
				marks[index] = 1;
				changed = 1;
			}
		}
	}
}

/* Counts a case that differs: with its listed reason, or reported as unexplained. */
static void
count_difference(
	struct prefix_count *row,
	const char *id,
	const char *expected,
	const char *outcome,
	size_t depth)
{
	unsigned index;
	unsigned word;
	int explained;

	/* The table lists the case at most once. */
	explained = 0;
	for (index = 0; limbo_exceptions[index].id != NULL; index++) {
		if (strcmp(limbo_exceptions[index].id, id) != 0)
			continue;
		limbo_exceptions[index].seen++;
		explained = 1;
		for (word = 0; reasons[word].reason != NULL; word++) {
			if (strcmp(reasons[word].reason, limbo_exceptions[index].reason) == 0)
				reasons[word].count++;
		}
	}

	/* Counted by prefix either way. */
	if (explained != 0) {
		limbo_explained++;
		row->explained++;
		return;
	}
	limbo_unexplained++;
	row->unexplained++;
	printf("limbo: %s: expected %s, got %s depth %u\n", id, expected, outcome, (unsigned)depth);
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
	unsigned char *leaf_der;
	size_t pool_count;
	struct prefix_count *row;
	const char *id;
	const char *expected;
	const char *time_text;
	const char *kind;
	const char *value;
	const char *features;
	char key[32];
	size_t size;
	size_t peer_count;
	size_t depth;
	unsigned index;
	int success;
	int ok;
	int error;

	id = vec_get(file, "id");
	expected = vec_get(file, "expected");
	CHECK(id != NULL);
	CHECK(expected != NULL);
	if (id == NULL || expected == NULL)
		return;
	success = strcmp(expected, "SUCCESS") == 0;
	row = prefix_of(id);
	row->cases++;
	limbo_cases++;
	if (success)
		limbo_success++;

	/* The trusted certificates are the store; one that does not parse is left out (it cannot be an anchor). */
	error = cryc_store_init(&store);
	CHECK(error == 0);
	if (error != 0)
		return;
	for (index = 0; index < TRUSTED_MAX; index++) {
		sprintf(key, "trusted.%u", index);
		size = vec_hex(file, key, buffer, LIMBO_MAX);
		if (size == (size_t)-1)
			continue;
		error = cryc_store_add_der(&store, buffer, size);
		if (trace != 0 && error != 0)
			printf("trace: %s %s not added (%d)\n", id, key, error);
	}

	/* The peer; a leaf that does not parse is refused before any validation, a FAILURE. */
	size = vec_hex(file, "peer", buffer, LIMBO_MAX);
	CHECK(size != (size_t)-1);
	if (size == (size_t)-1) {
		cryc_store_free(&store);
		return;
	}
	leaf_der = malloc(size);
	CHECK(leaf_der != NULL);
	if (leaf_der == NULL) {
		cryc_store_free(&store);
		return;
	}
	memcpy(leaf_der, buffer, size);
	error = cryc_x509_parse(&peer[0], leaf_der, size);
	if (error != 0) {
		limbo_refused_unparsed++;
		if (trace != 0)
			printf("trace: %s %s unparsed 0\n", id, expected);
		if (success == 0) {
			limbo_agree++;
			row->agree++;
		} else {
			count_difference(row, id, expected, "unparsed leaf", 0);
		}
		free(leaf_der);
		cryc_store_free(&store);
		return;
	}

	/* Every untrusted certificate that parses (one that does not is not sent: a server's list is what parses). */
	pool_count = 0;
	for (index = 0; index < UNTRUSTED_MAX; index++) {
		sprintf(key, "untrusted.%u", index);
		size = vec_hex(file, key, buffer, LIMBO_MAX);
		if (size == (size_t)-1)
			continue;
		pool_der[pool_count] = malloc(size);
		CHECK(pool_der[pool_count] != NULL);
		if (pool_der[pool_count] == NULL)
			break;
		memcpy(pool_der[pool_count], buffer, size);
		error = cryc_x509_parse(&pool[pool_count], pool_der[pool_count], size);
		if (trace != 0 && error != 0)
			printf("trace: %s %s does not parse (%d)\n", id, key, error);
		if (error != 0) {
			free(pool_der[pool_count]);
			pool_der[pool_count] = NULL;
			continue;
		}
		pool_count++;
	}

	/*
	 * A server sends at most 16 certificates.  A longer list is first
	 * reduced to the certificates that can chain up from the leaf by name
	 * (a certificate whose subject is not the issuer of the leaf or of
	 * another kept one is never a candidate, so leaving it out changes
	 * nothing), in their order; what is still over 15 is cut.
	 */
	for (index = 0; index < pool_count; index++)
		keep[index] = pool_count <= PEER_MAX - 1U;
	if (pool_count > PEER_MAX - 1U) {
		limbo_reduced++;
		keep_closure(&peer[0], pool, pool_count, keep);
	}
	peer_count = 1;
	for (index = 0; index < pool_count; index++) {
		if (keep[index] == 0)
			continue;
		if (peer_count >= PEER_MAX) {
			limbo_truncated++;
			break;
		}
		peer[peer_count] = pool[index];
		peer_count++;
	}

	/* The case's time (else the clock). */
	memset(&params, 0, sizeof(params));
	time_text = vec_get(file, "time");
	if (time_text != NULL && time_text[0] != '\0') {
		params.has_time = 1;
		params.time = parse_time(time_text);
	}

	/* The name (normalized as a caller does; a name that does not normalize is matched as it is) or the address to match. */
	kind = vec_get(file, "peer_kind");
	value = vec_get(file, "peer_value");
	if (kind != NULL && value != NULL && strcmp(kind, "DNS") == 0) {
		params.has_host = 1;
		if (cryc_name_host_normalize(value, params.host) != 0)
			strncpy(params.host, value, CRYC_NAME_HOST_MAX);
	} else if (kind != NULL && value != NULL && strcmp(kind, "IP") == 0) {
		params.ip_size = parse_ip(value, params.ip);
		CHECK(params.ip_size != 0U);
	}

	/* The depth: limbo counts the intermediates, the header every certificate; a case's 0 (the converter's "none" too) with the feature is 0 intermediates. */
	value = vec_get(file, "max_depth");
	depth = 0;
	if (value != NULL)
		depth = (size_t)atol(value);
	features = vec_get(file, "features");
	if (depth != 0U)
		params.max_depth = depth + 2U;
	else if (features != NULL && strstr(features, "max-chain-depth") != NULL)
		params.max_depth = 2;

	/* A depth beyond the header's limit is the limit (one case asks for 255 intermediates). */
	if (params.max_depth > CRYC_CHAIN_MAX_DEPTH)
		params.max_depth = CRYC_CHAIN_MAX_DEPTH;

	/* Validates, and compares with the case's expectation. */
	error = cryc_chain_verify(&store, peer, peer_count, &params, &result);
	CHECK(error == 0);
	CHECK((int)result.code >= 0 && (int)result.code <= CODE_LAST);
	ok = result.code == CRYC_VERIFY_OK;
	if (trace != 0 && (int)result.code >= 0 && (int)result.code <= CODE_LAST)
		printf("trace: %s %s %s %u\n", id, expected, code_names[result.code], (unsigned)result.depth);
	if (ok != 0 && success != 0)
		limbo_success_ok++;
	if (ok == success) {
		limbo_agree++;
		row->agree++;
	} else if ((int)result.code >= 0 && (int)result.code <= CODE_LAST) {
		count_difference(row, id, expected, code_names[result.code], result.depth);
	} else {
		count_difference(row, id, expected, "a code out of range", result.depth);
	}

	/* A valid result is well formed. */
	if (ok != 0) {
		CHECK(result.chain_count >= 1U);
		CHECK(result.chain[0]->der == peer[0].der);
	}
	free(leaf_der);
	for (index = 0; index < pool_count; index++)
		free(pool_der[index]);
	cryc_store_free(&store);
}

/* Every table entry is in order, unique and has a known reason. */
static void
check_table(void)
{
	unsigned index;
	unsigned word;
	int known;

	for (index = 0; limbo_exceptions[index].id != NULL; index++) {
		known = 0;
		for (word = 0; reason_words[word] != NULL; word++) {
			if (strcmp(reason_words[word], limbo_exceptions[index].reason) == 0)
				known = 1;
		}
		if (known == 0)
			printf("limbo: unknown reason %s for %s\n", limbo_exceptions[index].reason, limbo_exceptions[index].id);
		CHECK(known != 0);
		if (index > 0U) {
			if (strcmp(limbo_exceptions[index - 1U].id, limbo_exceptions[index].id) >= 0)
				printf("limbo: table out of order at %s\n", limbo_exceptions[index].id);
			CHECK(strcmp(limbo_exceptions[index - 1U].id, limbo_exceptions[index].id) < 0);
		}
	}
	for (word = 0; reason_words[word] != NULL; word++)
		reasons[word].reason = reason_words[word];
}

/* Runs every case and checks the counts. */
static void
test_limbo(void)
{
	struct vec_file file;
	unsigned char *buffer;
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
	while (vec_next(&file))
		limbo_run(&file, buffer);
	vec_close(&file);
	free(buffer);

	/* Every listed case must really differ (a stale entry is a failure), and no other may. */
	for (index = 0; limbo_exceptions[index].id != NULL; index++) {
		if (limbo_exceptions[index].seen != 1)
			printf("limbo: listed case did not differ once: %s\n", limbo_exceptions[index].id);
		CHECK(limbo_exceptions[index].seen == 1);
	}

	/* The summary, by prefix and by reason. */
	for (index = 0; index < prefix_used; index++)
		printf("limbo: %-14s %5lu cases, %5lu agree, %4lu differ with a reason, %lu unexplained\n", prefixes[index].prefix, prefixes[index].cases, prefixes[index].agree, prefixes[index].explained, prefixes[index].unexplained);
	for (index = 0; reasons[index].reason != NULL; index++)
		printf("limbo: reason %-14s %lu\n", reasons[index].reason, reasons[index].count);
	printf("limbo: %lu cases (%lu SUCCESS); %lu agree with the case, %lu differ with a listed reason, %lu unexplained; %lu SUCCESS cases valid; %lu unparsed leaves; %lu long lists reduced, %lu still cut at 16\n", limbo_cases, limbo_success, limbo_agree, limbo_explained, limbo_unexplained, limbo_success_ok, limbo_refused_unparsed, limbo_reduced, limbo_truncated);
	CHECK(limbo_unexplained == 0UL);
	CHECK(limbo_cases == LIMBO_CASES);
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	trace = getenv("WS335_LIMBO_TRACE") != NULL;
	check_table();
	test_limbo();
	return check_finish("test-limbo");
}
