/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * limbo-api: every SERVER case of x509-limbo through the public interface
 * of libssl-compat, the way a program meets a certificate chain: in a TLS
 * handshake.
 *
 * The client side uses only the public calls: the case's trusted
 * certificates go into the context's store (d2i_X509,
 * SSL_CTX_get_cert_store, X509_STORE_add_cert), the peer's name or
 * address is set with SSL_set1_host, and SSL_connect runs over one end of
 * a socket pair with SSL_VERIFY_PEER and a verify callback.  The callback
 * says whether the chain validated (preverify_ok) and ends the handshake
 * there; a handshake that never reaches it (a certificate that does not
 * parse) is a FAILURE.  The result code is SSL_get_verify_result.
 *
 * The other end is this program's server thread: a TLS 1.2 ServerHello
 * and Certificate (plaintext in TLS 1.2, so no key of the case is
 * needed), with the case's peer certificate and untrusted intermediates.
 * What it sends follows test-limbo.c (unit/): an untrusted certificate
 * that does not parse is not sent, and a list longer than 15 is first
 * reduced to the certificates that chain up from the leaf by name, then
 * cut (the server side may use the library's parser for this choice).
 *
 * The case's validation time is the clock the library reads: this program
 * defines time(), which the library's cryc_os_time calls (the program is
 * built with the library's sources, as the unit tests are); without a
 * time in the case the real clock is used.
 *
 * Differences from the case's expectation must be in the table below with
 * a reason, as in test-limbo.c: the reasons of p038 for the library's
 * decisions, and the reasons of the public interface.  The last line is
 * "limbo-api: PASS (<n> checks)" or "limbo-api: FAIL (...)".
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "vecfile.h"

#include <compat/crypto/err.h>
#include <compat/crypto/x509.h>
#include <compat/ssl/ssl.h>

#include "userland/base/libcrypto-compat/cryc-chain.h"

/* The most certificates sent: the peer and 15 more, what the client takes. */
#define PEER_MAX	16U

/* The largest certificate of the x509-limbo cases is about 78 KB. */
#define LIMBO_MAX	131072U

/* The most trusted and untrusted certificates one case lists. */
#define TRUSTED_MAX	8U
#define UNTRUSTED_MAX	120U

/* The number of SERVER cases of the pinned limbo.json. */
#define LIMBO_CASES	9792UL

/* The largest server flight: a ServerHello and a Certificate of up to 16 certificates, in records. */
#define FLIGHT_MAX	(2U * 1024U * 1024U)

/* The most plaintext bytes of one record (RFC 5246 6.2.1). */
#define RECORD_MAX	16384U

/* One case whose outcome here differs from its expectation: the id and the reason. */
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

/* The count of one id prefix, for the summary. */
struct prefix_count {
	char prefix[32];
	unsigned long cases;
	unsigned long agree;
	unsigned long explained;
	unsigned long unexplained;
};

/*
 * The reasons a case may differ.  All but the last are test-limbo.c's
 * (the library's decisions, see there; its "depth" does not arise here,
 * since no depth is set); the last comes from the public interface:
 *
 *   depth-api       the case limits the depth of the path; the public
 *                   subset has no call that sets it (no
 *                   SSL_CTX_set_verify_depth), so the library's limit of
 *                   CRYC_CHAIN_MAX_DEPTH certificates applies and the
 *                   longer path validates.
 */
static const char *const reason_words[] = {
	"issuance",
	"cabf",
	"anchor",
	"name",
	"key-usage",
	"nc-noncritical",
	"nc-leaf",
	"no-crl",
	"no-mldsa",
	"depth-api",
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
	{ "pathlen::max-chain-depth-0-exhausted", "depth-api", 0 },
	{ "pathlen::max-chain-depth-1-exhausted", "depth-api", 0 },
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

/* The number of SERVER cases run. */
static unsigned long limbo_cases;

/* The number of cases whose outcome agrees with the case's expectation. */
static unsigned long limbo_agree;

/* The number of cases that differ and have a listed reason. */
static unsigned long limbo_explained;

/* The number of cases that differ without a listed reason; the run fails unless it is zero. */
static unsigned long limbo_unexplained;

/* The number of cases that expect SUCCESS. */
static unsigned long limbo_success;

/* The number of cases that expect SUCCESS and whose chain validated here. */
static unsigned long limbo_success_ok;

/* The number of cases where the handshake never reached the verify callback. */
static unsigned long limbo_not_reached;

/* The number of cases whose list of untrusted certificates was reduced to the chain from the leaf. */
static unsigned long limbo_reduced;

/* The number of cases whose list was still cut at 16 certificates. */
static unsigned long limbo_truncated;

/* The number of cases whose name or address SSL_set1_host refused. */
static unsigned long limbo_name_refused;

/* The counts of each id prefix, filled as cases run, and prefix_used of them are in use. */
static struct prefix_count prefixes[16];

/* The number of rows of prefixes in use. */
static unsigned prefix_used;

/* The count of each reason, filled by check_table and counted as differences are found. */
static struct reason_count reasons[16];

/* Whether to print a line for every case, set from the environment. */
static int trace;

/* The reason of the first error the last handshake queued (for the report of a difference). */
static int last_reason;

/* Whether the library's clock is the case's time; when 0 the real clock is read. */
static int has_fake_time;

/* The case's time, valid while has_fake_time is set. */
static time_t fake_time;

/* The number of times the verify callback was called in the current handshake. */
static int callback_calls;

/* What the verify callback was told about the chain in the current handshake. */
static int callback_ok;

/* The parsed untrusted certificates of the case being run. */
static struct cryc_x509 pool[UNTRUSTED_MAX];

/* The DER of each untrusted certificate in pool, kept alive until the case ends. */
static unsigned char *pool_der[UNTRUSTED_MAX];

/* The size of each DER in pool_der. */
static size_t pool_size[UNTRUSTED_MAX];

/* Whether each certificate of pool is sent by the server. */
static int keep[UNTRUSTED_MAX];

/* The server's flight, made once for each case. */
static unsigned char flight[FLIGHT_MAX];

/* The number of bytes of flight in use. */
static size_t flight_used;

/* The server thread's end of the socket pair, set before the thread starts. */
static int server_end;

/* The verify callback: records the verdict and ends the handshake (the leaf's key is not the server's). */
static int verify_callback(int preverify_ok, X509_STORE_CTX *store);

/* Returns the counts of the id's prefix (the text before the first "::"). */
static struct prefix_count *prefix_of(const char *id);

/* Marks the certificates of candidates that chain up from leaf by name (test-limbo.c's rule). */
static void keep_closure(const struct cryc_x509 *leaf, const struct cryc_x509 *candidates, size_t count, int *marks);

/* Counts a case that differs: with its listed reason, or reported as unexplained. */
static void count_difference(struct prefix_count *row, const char *id, const char *expected, long code);

/* Appends bytes to the flight (a flight over FLIGHT_MAX is a test failure). */
static void put(const unsigned char *data, size_t size);

/* Appends a 24-bit length. */
static void put24(size_t value);

/* Makes the server's flight into flight. */
static void make_flight(const unsigned char *const *der, const size_t *der_size, size_t count, unsigned suite);

/* The server thread: writes the flight, then reads (and drops) what the client sends until it closes. */
static void *server_main(void *argument);

/* Runs one case. */
static void limbo_run(const struct vec_file *file, unsigned char *buffer);

/* Every table entry is in order, unique and has a known reason. */
static void check_table(void);

/* The library's clock (cryc_os_time calls time(NULL)): the case's time when it has one. */
time_t
time(
	time_t *out)
{
	struct timespec now;
	time_t value;

	/* Takes the case's time when it has one, and the real clock otherwise. */
	if (has_fake_time) {
		value = fake_time;
	} else {
		clock_gettime(CLOCK_REALTIME, &now);
		value = now.tv_sec;
	}

	/* Stores the time where the caller asked. */
	if (out != NULL)
		*out = value;

	/* The time the library reads. */
	return value;
}

/* Runs every SERVER case and prints the summary; the last line is the verdict. */
int
main(void)
{
	struct vec_file file;
	unsigned char *buffer;
	const char *only;
	const char *current;
	const char *trace_text;
	unsigned index;
	int opened;
	int more;
	int cmp;
	int status;

	/* Makes the output line by line, and reads the controls of the run. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	trace = 0;
	trace_text = getenv("WS335_LIMBO_TRACE");
	if (trace_text != NULL)
		trace = 1;
	only = getenv("WS335_LIMBO_ONLY");

	/* Puts the table of reasons in order and checks it. */
	check_table();

	/* Allocates the buffer a case's certificates are decoded into. */
	buffer = malloc(LIMBO_MAX);
	CHECK(buffer != NULL);

	/* Opens the case file, when the buffer exists. */
	opened = -1;
	if (buffer != NULL)
		opened = vec_open(&file, "limbo");

	/* Gives up when the buffer or the case file is missing. */
	if (buffer == NULL || opened != 0) {
		CHECK(0);
		free(buffer);
		status = check_finish("limbo-api");
		return status;
	}

	/* Runs the cases one after another, or only the one named in the environment. */
	for (;;) {
		more = vec_next(&file);
		if (!more)
			break;

		/* Skips every case but the one asked for. */
		if (only != NULL) {
			current = vec_get(&file, "id");
			if (current == NULL)
				continue;

			/* Compares the id with the one asked for. */
			cmp = strcmp(current, only);
			if (cmp != 0)
				continue;
		}

		/* Runs the case. */
		limbo_run(&file, buffer);
	}

	/* Releases the case file and the buffer. */
	vec_close(&file);
	free(buffer);

	/* Every listed case must really differ, and no other may. */
	for (index = 0; limbo_exceptions[index].id != NULL; index++) {
		if (limbo_exceptions[index].seen != 1)
			printf("limbo-api: listed case did not differ once: %s\n", limbo_exceptions[index].id);
		CHECK(limbo_exceptions[index].seen == 1);
	}

	/* Prints the counts of each id prefix. */
	for (index = 0; index < prefix_used; index++)
		printf("limbo-api: %-14s %5lu cases, %5lu agree, %4lu differ with a reason, %lu unexplained\n", prefixes[index].prefix, prefixes[index].cases, prefixes[index].agree, prefixes[index].explained, prefixes[index].unexplained);

	/* Prints the count of each reason. */
	for (index = 0; reasons[index].reason != NULL; index++)
		printf("limbo-api: reason %-14s %lu\n", reasons[index].reason, reasons[index].count);

	/* Prints the totals of the run. */
	printf("limbo-api: %lu cases (%lu SUCCESS); %lu agree with the case, %lu differ with a listed reason, %lu unexplained; %lu SUCCESS cases valid; %lu never reached the callback; %lu names SSL_set1_host refused; %lu long lists reduced, %lu still cut at 16\n", limbo_cases, limbo_success, limbo_agree, limbo_explained, limbo_unexplained, limbo_success_ok, limbo_not_reached, limbo_name_refused, limbo_reduced, limbo_truncated);

	/* Requires every difference to be explained, and the number of cases to be the pinned one. */
	CHECK(limbo_unexplained == 0UL);
	CHECK(limbo_cases == LIMBO_CASES);

	/* Reports the verdict of the checks. */
	status = check_finish("limbo-api");
	return status;
}

/* The verify callback: records the verdict and ends the handshake (the leaf's key is not the server's). */
static int
verify_callback(
	int preverify_ok,
	X509_STORE_CTX *store)
{
	(void)store;

	/* Records that the callback was reached and what it was told. */
	callback_calls++;
	callback_ok = preverify_ok;

	/* Refuses the chain, which ends the handshake there. */
	return 0;
}

/* Returns the counts of the id's prefix (the text before the first "::"). */
static struct prefix_count *
prefix_of(
	const char *id)
{
	size_t length;
	size_t prefix_length;
	unsigned index;
	const char *end;
	int cmp;

	/* Takes the text before the first "::", or the whole id, cut to fit a row. */
	end = strstr(id, "::");
	if (end == NULL)
		length = strlen(id);
	else
		length = (size_t)(end - id);
	if (length >= sizeof(prefixes[0].prefix))
		length = sizeof(prefixes[0].prefix) - 1U;

	/* Looks for a row with the prefix already. */
	for (index = 0; index < prefix_used; index++) {
		prefix_length = strlen(prefixes[index].prefix);
		if (prefix_length != length)
			continue;

		/* Takes the row when the text is the same. */
		cmp = memcmp(prefixes[index].prefix, id, length);
		if (cmp == 0)
			return &prefixes[index];
	}

	/* Starts a row for a prefix not seen yet, when there is room. */
	if (prefix_used < sizeof(prefixes) / sizeof(prefixes[0])) {
		memcpy(prefixes[prefix_used].prefix, id, length);
		prefixes[prefix_used].prefix[length] = '\0';
		prefix_used++;
	}

	/* The row of the prefix (the last one when the table is full). */
	return &prefixes[prefix_used - 1U];
}

/* Marks the certificates of candidates that chain up from leaf by name (test-limbo.c's rule). */
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

	/* Repeats until a pass marks no new certificate. */
	changed = 1;
	while (changed != 0) {
		changed = 0;

		/* Looks at each certificate not marked yet. */
		for (index = 0; index < count; index++) {
			if (marks[index] != 0)
				continue;

			/* Asks whether it issued the leaf, or a certificate already marked. */
			issues = cryc_name_equal(candidates[index].subject, candidates[index].subject_size, leaf->issuer, leaf->issuer_size);
			for (below = 0; below < count && issues == 0; below++) {
				if (marks[below] == 0)
					continue;

				/* Asks whether it issued a certificate already marked. */
				issues = cryc_name_equal(candidates[index].subject, candidates[index].subject_size, candidates[below].issuer, candidates[below].issuer_size);
			}

			/* Marks the certificate that chains up from the leaf. */
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
	long code)
{
	const char *error_text;
	const char *reached_text;
	unsigned index;
	unsigned word;
	int explained;
	int cmp;

	/* Looks the case up in the table of listed differences, and counts its reason. */
	explained = 0;
	for (index = 0; limbo_exceptions[index].id != NULL; index++) {
		cmp = strcmp(limbo_exceptions[index].id, id);
		if (cmp != 0)
			continue;

		/* Records that the listed difference came up, and that this case is explained. */
		limbo_exceptions[index].seen++;
		explained = 1;

		/* Counts the case under its reason. */
		for (word = 0; reasons[word].reason != NULL; word++) {
			cmp = strcmp(reasons[word].reason, limbo_exceptions[index].reason);
			if (cmp == 0)
				reasons[word].count++;
		}
	}

	/* Counts a difference with a listed reason. */
	if (explained != 0) {
		limbo_explained++;
		row->explained++;
		return;
	}

	/* Counts a difference without a reason and reports it. */
	limbo_unexplained++;
	row->unexplained++;
	error_text = X509_verify_cert_error_string(code);
	reached_text = "reached";
	if (callback_calls == 0)
		reached_text = "not reached";
	printf("limbo-api: %s: expected %s, got verify result %ld (%s), callback %s, reason %d\n", id, expected, code,
	    error_text, reached_text, last_reason);
}

/* Appends bytes to the flight (a flight over FLIGHT_MAX is a test failure). */
static void
put(
	const unsigned char *data,
	size_t size)
{
	/* Fails the test when the flight would not fit. */
	CHECK(flight_used + size <= FLIGHT_MAX);
	if (flight_used + size > FLIGHT_MAX)
		return;

	/* Appends the bytes at the end of the flight. */
	memcpy(flight + flight_used, data, size);
	flight_used += size;
}

/* Appends a 24-bit length. */
static void
put24(
	size_t value)
{
	unsigned char bytes[3];

	/* Splits the value into three bytes, most significant first. */
	bytes[0] = (unsigned char)(value >> 16);
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)value;

	/* Appends the three bytes. */
	put(bytes, 3);
}

/*
 * Makes the server's flight into flight: the handshake messages
 * ServerHello and Certificate (RFC 5246 7.4.1.3, 7.4.2), cut into
 * handshake records of at most RECORD_MAX bytes.  suite is the
 * ECDHE suite that matches the leaf's key (it is not reached).
 */
static void
make_flight(
	const unsigned char *const *der,
	const size_t *der_size,
	size_t count,
	unsigned suite)
{
	static unsigned char messages[FLIGHT_MAX];
	static const unsigned char hello_head[] = {
		0x02, 0x00, 0x00, 0x31,		/* server_hello, 49 bytes */
		0x03, 0x03			/* TLS 1.2 */
	};
	static const unsigned char hello_tail[] = {
		0x00,				/* compression: null */
		0x00, 0x09,			/* extensions, 9 bytes */
		0xff, 0x01, 0x00, 0x01, 0x00,	/* renegotiation_info, empty */
		0x00, 0x17, 0x00, 0x00		/* extended_master_secret */
	};
	unsigned char random[32];
	unsigned char suite_bytes[2];
	unsigned char empty_id;
	unsigned char header[5];
	size_t total;
	size_t at;
	size_t piece;
	size_t index;

	/* The handshake messages, built in the flight buffer and then moved into records. */
	flight_used = 0;

	/* Appends the ServerHello: head, random, empty session id, suite and extensions. */
	put(hello_head, sizeof(hello_head));
	for (index = 0; index < sizeof(random); index++)
		random[index] = (unsigned char)(0x40U + index);
	put(random, sizeof(random));
	empty_id = 0;
	put(&empty_id, 1);
	suite_bytes[0] = (unsigned char)(suite >> 8);
	suite_bytes[1] = (unsigned char)suite;
	put(suite_bytes, 2);
	put(hello_tail, sizeof(hello_tail));

	/* Appends the Certificate header, with the length of every certificate entry. */
	total = 0;
	for (index = 0; index < count; index++)
		total += 3U + der_size[index];
	header[0] = 0x0b;			/* certificate */
	put(header, 1);
	put24(3U + total);
	put24(total);

	/* Appends every certificate with its length. */
	for (index = 0; index < count; index++) {
		put24(der_size[index]);
		put(der[index], der_size[index]);
	}

	/* Fails the test when the messages do not fit, and moves them aside. */
	CHECK(flight_used <= sizeof(messages));
	if (flight_used > sizeof(messages)) {
		flight_used = 0;
		return;
	}

	/* Moves the messages aside, to be cut into records. */
	memcpy(messages, flight, flight_used);
	total = flight_used;

	/* The records. */
	flight_used = 0;
	for (at = 0; at < total; at += piece) {
		piece = total - at;
		if (piece > RECORD_MAX)
			piece = RECORD_MAX;
		header[0] = 22;
		header[1] = 3;
		header[2] = 3;
		header[3] = (unsigned char)(piece >> 8);
		header[4] = (unsigned char)piece;
		put(header, 5);
		put(messages + at, piece);
	}
}

/* The server thread: writes the flight, then reads (and drops) what the client sends until it closes. */
static void *
server_main(
	void *argument)
{
	unsigned char sink[4096];
	size_t at;
	ssize_t done;

	/* The thread takes no argument. */
	(void)argument;

	/* Writes the flight, until it is all out or the client has gone. */
	for (at = 0; at < flight_used; at += (size_t)done) {
		done = write(server_end, flight + at, flight_used - at);
		if (done <= 0)
			break;
	}

	/* Reads and drops what the client sends, until it closes its end. */
	for (;;) {
		done = read(server_end, sink, sizeof(sink));
		if (done <= 0)
			break;
	}

	/* The thread has no result. */
	return NULL;
}

/* Runs one case. */
static void
limbo_run(
	const struct vec_file *file,
	unsigned char *buffer)
{
	const unsigned char *send_der[PEER_MAX];
	size_t send_size[PEER_MAX];
	struct cryc_x509 leaf;
	unsigned char *leaf_der;
	size_t leaf_size;
	struct prefix_count *row;
	const char *id;
	const char *expected;
	const char *time_text;
	const char *kind;
	const char *value;
	const unsigned char *in;
	char key[32];
	SSL_CTX *ctx;
	SSL *ssl;
	X509 *certificate;
	pthread_t thread;
	size_t size;
	size_t pool_count;
	size_t send_count;
	unsigned suite;
	unsigned index;
	long code;
	int pair[2];
	int leaf_parsed;
	int success;
	int ok;
	int cmp;
	int status;
	int use_host;

	/* Reads the case's id and expected outcome. */
	id = vec_get(file, "id");
	expected = vec_get(file, "expected");
	CHECK(id != NULL && expected != NULL);
	if (id == NULL || expected == NULL)
		return;

	/* Counts the case. */
	cmp = strcmp(expected, "SUCCESS");
	success = 0;
	if (cmp == 0)
		success = 1;
	row = prefix_of(id);
	row->cases++;
	limbo_cases++;
	if (success)
		limbo_success++;

	/* The client's context: TLS 1.2 (the Certificate is plaintext), verification with the callback, the trusted certificates. */
	ctx = SSL_CTX_new(TLS_client_method());
	CHECK(ctx != NULL);
	if (ctx == NULL)
		return;
	CHECK(SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION) == 1);
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, verify_callback);
	for (index = 0; index < TRUSTED_MAX; index++) {
		sprintf(key, "trusted.%u", index);
		size = vec_hex(file, key, buffer, LIMBO_MAX);
		if (size == (size_t)-1)
			continue;

		/* Parses the trusted certificate through the public decoder. */
		in = buffer;
		certificate = d2i_X509(NULL, &in, (long)size);
		if (certificate == NULL) {
			if (trace != 0)
				printf("trace: %s %s does not parse\n", id, key);
			continue;
		}

		/* Trusts the certificate. */
		CHECK(X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), certificate) == 1);
		X509_free(certificate);
	}

	/* Drops the errors the setup queued. */
	ERR_clear_error();

	/* The server's list: the peer, then the untrusted certificates that parse (reduced and cut as test-limbo.c does). */
	leaf_size = vec_hex(file, "peer", buffer, LIMBO_MAX);
	CHECK(leaf_size != (size_t)-1);
	if (leaf_size == (size_t)-1) {
		SSL_CTX_free(ctx);
		return;
	}

	/* Keeps a copy of the peer's certificate, and parses it. */
	leaf_der = malloc(leaf_size);
	CHECK(leaf_der != NULL);
	if (leaf_der == NULL) {
		SSL_CTX_free(ctx);
		return;
	}

	/* Copies the peer's certificate, which the parsed form points into. */
	memcpy(leaf_der, buffer, leaf_size);
	status = cryc_x509_parse(&leaf, leaf_der, leaf_size);
	leaf_parsed = 0;
	if (status == 0)
		leaf_parsed = 1;

	/* Keeps a copy of each untrusted certificate that parses. */
	pool_count = 0;
	for (index = 0; index < UNTRUSTED_MAX; index++) {
		sprintf(key, "untrusted.%u", index);
		size = vec_hex(file, key, buffer, LIMBO_MAX);
		if (size == (size_t)-1)
			continue;

		/* Copies the certificate, which the parsed form points into. */
		pool_der[pool_count] = malloc(size);
		CHECK(pool_der[pool_count] != NULL);
		if (pool_der[pool_count] == NULL)
			break;
		memcpy(pool_der[pool_count], buffer, size);
		pool_size[pool_count] = size;

		/* Drops a certificate that does not parse. */
		status = cryc_x509_parse(&pool[pool_count], pool_der[pool_count], size);
		if (status != 0) {
			free(pool_der[pool_count]);
			pool_der[pool_count] = NULL;
			continue;
		}

		/* Takes the certificate into the pool. */
		pool_count++;
	}

	/* Sends every certificate when the list fits, or only those that chain up from the leaf when it is too long. */
	for (index = 0; index < pool_count; index++) {
		keep[index] = 0;
		if (pool_count <= PEER_MAX - 1U)
			keep[index] = 1;
	}

	/* Reduces a long list to the certificates that chain up from the leaf. */
	if (pool_count > PEER_MAX - 1U) {
		limbo_reduced++;
		if (leaf_parsed)
			keep_closure(&leaf, pool, pool_count, keep);
	}

	/* Puts the peer first and then the certificates to send, cut at PEER_MAX. */
	send_der[0] = leaf_der;
	send_size[0] = leaf_size;
	send_count = 1;
	for (index = 0; index < pool_count; index++) {
		if (keep[index] == 0)
			continue;

		/* Cuts the list at the most the client takes. */
		if (send_count >= PEER_MAX) {
			limbo_truncated++;
			break;
		}

		/* Appends the certificate to the list. */
		send_der[send_count] = pool_der[index];
		send_size[send_count] = pool_size[index];
		send_count++;
	}

	/* Chooses the ECDHE suite that matches the leaf's key, and makes the flight. */
	suite = 0xc02bU;
	if (leaf_parsed && leaf.key.type == CRYC_KEY_RSA)
		suite = 0xc02fU;
	make_flight(send_der, send_size, send_count, suite);

	/* The case's time is the library's clock. */
	time_text = vec_get(file, "time");
	has_fake_time = 0;
	if (time_text != NULL && time_text[0] != '\0') {
		has_fake_time = 1;
		fake_time = (time_t)atol(time_text);
	}

	/* The connection: the name or the address to match, through SSL_set1_host. */
	ssl = SSL_new(ctx);
	CHECK(ssl != NULL);
	kind = vec_get(file, "peer_kind");
	value = vec_get(file, "peer_value");
	use_host = 0;
	if (ssl != NULL && kind != NULL && value != NULL) {
		cmp = strcmp(kind, "DNS");
		if (cmp == 0) {
			use_host = 1;
		} else {
			cmp = strcmp(kind, "IP");
			if (cmp == 0)
				use_host = 1;
		}
	}

	/* Sets the name or the address to match. */
	if (use_host) {
		status = SSL_set1_host(ssl, value);
		if (status != 1) {
			limbo_name_refused++;
			if (trace != 0)
				printf("trace: %s SSL_set1_host refused %s\n", id, value);
		}
	}

	/* Drops the errors the setup queued. */
	ERR_clear_error();

	/* The handshake against the server thread. */
	callback_calls = 0;
	callback_ok = 0;
	code = -1;
	status = -1;
	if (ssl != NULL)
		status = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	if (status == 0) {
		server_end = pair[1];
		status = pthread_create(&thread, NULL, server_main, NULL);
		if (status == 0) {
			CHECK(SSL_set_fd(ssl, pair[0]) == 1);
			CHECK(SSL_connect(ssl) != 1);
			code = SSL_get_verify_result(ssl);
			last_reason = ERR_GET_REASON(ERR_peek_error());
			ERR_clear_error();
			SSL_free(ssl);
			ssl = NULL;
			shutdown(pair[0], SHUT_RDWR);
			close(pair[0]);
			pthread_join(thread, NULL);
		} else {
			CHECK(0);
			close(pair[0]);
		}

		/* Closes the server's end. */
		close(pair[1]);
	} else {
		CHECK(0);
	}

	/* Releases the connection and the context, and puts the real clock back. */
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	has_fake_time = 0;

	/* The verdict: the callback reached with a valid chain is a SUCCESS. */
	CHECK(callback_calls <= 1);
	if (callback_calls == 0)
		limbo_not_reached++;
	ok = 0;
	if (callback_calls == 1 && callback_ok == 1)
		ok = 1;
	if (callback_calls == 1)
		CHECK((code == X509_V_OK) == (callback_ok == 1));
	if (trace != 0)
		printf("trace: %s %s verify %ld callback %d/%d reason %d\n", id, expected, code, callback_calls, callback_ok, last_reason);
	if (ok && success)
		limbo_success_ok++;

	/* Counts the case as agreeing with its expectation, or as a difference. */
	if (ok == success) {
		limbo_agree++;
		row->agree++;
	} else {
		count_difference(row, id, expected, code);
	}

	/* Releases the certificates kept for the case. */
	free(leaf_der);
	for (index = 0; index < pool_count; index++)
		free(pool_der[index]);
}

/* Every table entry is in order, unique and has a known reason. */
static void
check_table(void)
{
	unsigned index;
	unsigned word;
	int known;
	int cmp;

	/* Checks that each entry has a known reason, and that the ids are in order. */
	for (index = 0; limbo_exceptions[index].id != NULL; index++) {
		known = 0;
		for (word = 0; reason_words[word] != NULL; word++) {
			cmp = strcmp(reason_words[word], limbo_exceptions[index].reason);
			if (cmp == 0)
				known = 1;
		}

		/* Requires a known reason, and ids in order. */
		CHECK(known != 0);
		if (index > 0U)
			CHECK(strcmp(limbo_exceptions[index - 1U].id, limbo_exceptions[index].id) < 0);
	}

	/* Gives each known reason a counter. */
	for (word = 0; reason_words[word] != NULL; word++)
		reasons[word].reason = reason_words[word];
}
