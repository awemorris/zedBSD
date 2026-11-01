/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Observes real body-decoding errors independently of unrelated native MIME fallback.
 */

#include "page/page.h"
#include "net/net.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* One ordinary URL supplies real decoded bytes, MIME and an expected native error outcome. */
struct data_case {
	const char *url;
	int error;
	const char *body;
	const char *mime;
};

/* Count independent body, MIME and response observations. */
static unsigned checks;

/* Preserve every failed observation while ordinary C cleanup continues. */
static unsigned failures;

static void data_check(int condition, const char *name);
static int data_cases(void);
static int data_case_run(const struct data_case *test);
static int data_bytes(const struct wb_buffer *bytes, const char *expected);
static void data_consumers(void);

/*
 * Tests production data URL failure routing without replacing its native decoder or MIME parser.
 */
int
main(
	void)
{
	int error;
	int printed;

	/* The same real native parser supplies valid and invalid-body observations. */
	error = data_cases();
	if (error != 0)
		return 2;

	/* Metadata and byte-only consumers must refuse the same native body error. */
	data_consumers();

	/* Reports all completed body, MIME and consumer observations. */
	printed = printf("native data URL error routing: %u/%u passed\n", checks - failures, checks);
	if (printed < 0)
		return 2;

	/* Any failed native observation rejects this regression. */
	if (failures != 0)
		return 1;

	/* Succeeded: invalid body errors and MIME-only fallback remain separate native outcomes. */
	return 0;
}

/* Records a semantic observation while retaining normal native cleanup on failure. */
static void
data_check(
	int condition,
	const char *name)
{
	int printed;

	/* Counts this independent observation in the final fixture total. */
	checks++;

	/* Counts named failures while preserving later independent observations. */
	if (!condition) {
		failures++;
		printed = fprintf(stderr, "FAIL %s\n", name);
		if (printed < 0)
			failures++;
	}

	/* Succeeded: this observation and any lost failure report were counted. */
	return;
}

/* Covers actual forbidden characters, padding and partial decode alongside unchanged valid defaults. */
static int
data_cases(
	void)
{
	/* Immutable URL/body/MIME expectations belong to this one corpus traversal. */
	const struct data_case tests[] = {
	    {"data:application/xml;base64,!", EINVAL, "", ""},
	    {"data:application/xml;base64,A", EINVAL, "", ""},
	    {"data:application/xml;base64,!!!!", EINVAL, "", ""},
	    {"data:application/xml;base64,AA=A", EINVAL, "", ""},
	    {"data:application/xml;base64,YQ===", EINVAL, "", ""},
	    {"data:application/xml;base64,PHIvPg=Q", EINVAL, "", ""},
	    {"data:application/xml;base64,PHIvPg==!", EINVAL, "", ""},
	    {"data:application/xml;base64,%00", EINVAL, "", ""},
	    {"data:bad%20type;base64,!", EINVAL, "", ""},
	    {"data:application/xml", EINVAL, "", ""},
	    {"data:application/xml;base64,YQ==", 0, "a", "application/xml"},
	    {"data:application/xml;base64,YQ", 0, "a", "application/xml"},
	    {"data:application/xml;base64,YQ%0A==", 0, "a", "application/xml"},
	    {"data:application/xml;base64,", 0, "", "application/xml"},
	    {"data:bad%20type;base64,YQ==", 0, "a", "text/plain;charset=US-ASCII"},
	    {"data:,markup", 0, "markup", "text/plain;charset=US-ASCII"},
	    {"data:;charset=UTF-8,a", 0, "a", "text/plain;charset=UTF-8"},
	    {"data:TEXT/HTML,%3Cp%3E#fragment", 0, "<p>", "text/html"},
	    {"data:text/ html,x", 0, "x", "text/plain;charset=US-ASCII"},
	    {"data:text/plain;a=\";b=x\";b=y,z", 0, "z", "text/plain;a=\";b=x\";b=y"},
	    {"data:text/plain;a=\";b=x\";B=y;b=z,k", 0, "k", "text/plain;a=\";b=x\";b=y"},
	    {"data:text/plain;a=\"escaped\\\";b=x\";b=y,t", 0, "t", "text/plain;a=\"escaped\\\";b=x\";b=y"}};
	size_t index;
	int error;

	/* Each independent case owns and releases its genuine URL and native decoded result. */
	for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
		error = data_case_run(&tests[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: no earlier case supplied parser state or accidental output to another case. */
	return 0;
}

/* Parses actual URL text and observes all native body and MIME cleanup fields. */
static int
data_case_run(
	const struct data_case *test)
{
	struct net_url url;
	struct net_data decoded;
	size_t length;
	int error;
	int same;
	int observed;

	/* The native URL parser and genuine data processor receive ordinary independent input. */
	length = strlen(test->url);
	error = net_url_parse(test->url, length, NULL, &url);
	if (error != 0)
		return error;

	/* Decodes the actual body independently of MIME fallback classification. */
	error = net_data_parse(&url, &decoded);

	/* The native error must equal this independent case expectation. */
	observed = 0;
	if (error == test->error)
		observed = 1;
	data_check(observed, test->url);

	/* Complete owned body bytes must match, including empty failed-decode storage. */
	same = data_bytes(&decoded.body, test->body);
	data_check(same, "actual native body bytes or empty failed decode");

	/* MIME fallback may change the media type without concealing a body error. */
	same = data_bytes(&decoded.mime, test->mime);
	data_check(same, "actual native MIME or empty failed body outcome");

	/* Failed decoding must relinquish both partial output allocations. */
	if (error != 0) {
		observed = 0;
		if (decoded.body.data == NULL && decoded.mime.data == NULL)
			observed = 1;
		data_check(observed, "failed decode releases partial native C allocations");
	}

	/* Both failed and successful native outputs support ordinary idempotent release. */
	net_data_release(&decoded);
	net_url_release(&url);

	/* Succeeded: this independent semantic case has no remaining parser allocations. */
	return 0;
}

/* Compares actual owned bytes without relying on terminators or partial-prefix equality. */
static int
data_bytes(
	const struct wb_buffer *bytes,
	const char *expected)
{
	size_t length;
	int same;

	/* Complete lengths agree before any actual byte observation. */
	length = strlen(expected);
	if (bytes->length != length)
		return 0;

	/* Equal empty outputs require no dereference of absent byte storage. */
	if (length == 0)
		return 1;

	/* A complete byte comparison distinguishes content from equal-sized storage. */
	same = memcmp(bytes->data, expected, length);

	/* Different native bytes refuse the expected body or MIME result. */
	if (same != 0)
		return 0;

	/* Succeeded: every expected byte agrees with the owned native result. */
	return 1;
}

/* Confirms both actual metadata and existing byte-only resource consumers reject a malformed body. */
static void
data_consumers(
	void)
{
	struct net_response response;
	struct wb_buffer bytes;
	int error;
	int observed;

	/* New metadata ownership cannot publish an invented status200 for a genuine decoding failure. */
	error = page_fetch_response(NULL, "https://example.invalid/base", "data:application/xml;base64,!", &response);

	/* Metadata consumer preserves body EINVAL. */
	observed = 0;
	if (error == EINVAL && response.status == 0)
		observed = 1;
	data_check(observed, "metadata consumer preserves body EINVAL");

	/* Metadata consumer has no partial body or MIME. */
	observed = 0;
	if (response.body.length == 0 && response.content_type.length == 0)
		observed = 1;
	data_check(observed, "metadata consumer has no partial body or MIME");

	/* Metadata consumer releases failed response allocations. */
	observed = 0;
	if (response.url.length == 0 && response.body.data == NULL)
		observed = 1;
	data_check(observed, "metadata consumer releases failed response allocations");
	net_response_release(&response);

	/* Existing consumers receive the same real decoder failure without a synthetic successful empty body. */
	wb_buffer_init(&bytes);
	error = page_fetch(NULL, "https://example.invalid/base", "data:application/xml;base64,!", &bytes, NULL);

	/* Legacy byte-only consumer preserves body EINVAL. */
	observed = 0;
	if (error == EINVAL && bytes.length == 0)
		observed = 1;
	data_check(observed, "legacy byte-only consumer preserves body EINVAL");
	wb_buffer_release(&bytes);

	/* Succeeded: ordinary actual consumers share the repaired native failure semantics. */
	return;
}
