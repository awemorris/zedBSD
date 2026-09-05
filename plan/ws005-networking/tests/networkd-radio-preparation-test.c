/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises production networkd WLAN-radio preparation policy.
 */

#include "userland/base/networkd/wifi-child.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture_call {
	const char *interface;
	const char *operation;
	int result;
	int error;
};

static int fixture_wifi_child_run(const char *, const char *, const void *, size_t, const void *, size_t, unsigned, struct networkd_wifi_child_result *);
static void fixture_wifi_child_result_clear(struct networkd_wifi_child_result *);
static void fixture_protocol_clear(void *, size_t);

#define networkd_wifi_child_run fixture_wifi_child_run
#define networkd_wifi_child_result_clear fixture_wifi_child_result_clear
#define networkd_protocol_clear fixture_protocol_clear
#define main networkd_program_main
#include "userland/base/networkd/main.c"
#undef main
#undef networkd_protocol_clear
#undef networkd_wifi_child_result_clear
#undef networkd_wifi_child_run

static void expect(int, const char *);
static void set_scenario(const struct fixture_call *, size_t);
static void initialize_radio(struct networkd_wlan_radio *, const char *, uint32_t);
static void expect_scenario_complete(const char *);
static void test_empty_radio_set(void);
static void test_later_radio_survives_up_failure(void);
static void test_later_radio_survives_scan_failure(void);
static void test_all_radios_failed(void);

static int fixture_errno;
static const struct fixture_call *fixture_calls;
static size_t fixture_call_count;
static size_t fixture_call_index;

/*
 * Provides the test libc error slot.
 */
int *
__libc_errno_location(
	void)
{
	/* Returns the deterministic fixture error slot. */
	return &fixture_errno;
}

/*
 * Runs the WLAN-radio preparation fixture.
 */
int
main(
	void)
{
	/* Exercises empty, partial-failure, and total-failure boundaries. */
	test_empty_radio_set();
	test_later_radio_survives_up_failure();
	test_later_radio_survives_scan_failure();
	test_all_radios_failed();

	/* Reports successful completion. */
	puts("networkd-radio-preparation-test: PASS");
	return 0;
}

/* Requires one fixture condition. */
static void
expect(
	int condition,
	const char *message)
{
	/* Terminates the fixture at the first failed invariant. */
	if (!condition) {
		fprintf(stderr, "networkd-radio-preparation-test: %s\n",
		    message);
		exit(1);
	}
}

/* Installs one exact ordered primitive-call scenario. */
static void
set_scenario(
	const struct fixture_call *calls,
	size_t count)
{
	/* Publishes the immutable scenario and resets its cursor. */
	fixture_calls = calls;
	fixture_call_count = count;
	fixture_call_index = 0U;
	fixture_errno = 0;
}

/* Initializes one stable-order WLAN-radio record. */
static void
initialize_radio(
	struct networkd_wlan_radio *radio,
	const char *interface,
	uint32_t ifindex)
{
	size_t length;

	/* Builds the same bounded record produced by radio enumeration. */
	length = strlen(interface);
	expect(length < sizeof(radio->interface), "fixture interface length");
	memset(radio, 0, sizeof(*radio));
	memcpy(radio->interface, interface, length + 1U);
	radio->ifindex = ifindex;
}

/* Requires every expected primitive call to have occurred. */
static void
expect_scenario_complete(
	const char *message)
{
	/* Compares the final cursor with the immutable scenario length. */
	expect(fixture_call_index == fixture_call_count, message);
}

/* Simulates one bounded /sbin/wifi child invocation. */
static int
fixture_wifi_child_run(
	const char *interface,
	const char *operation,
	const void *ssid,
	size_t ssid_length,
	const void *passphrase,
	size_t passphrase_length,
	unsigned timeout,
	struct networkd_wifi_child_result *result)
{
	const struct fixture_call *call;

	/* Requires another exact primitive invocation in stable order. */
	expect(fixture_call_index < fixture_call_count,
	    "unexpected primitive call");
	call = &fixture_calls[fixture_call_index++];
	expect(strcmp(interface, call->interface) == 0,
	    "primitive interface order");
	expect(strcmp(operation, call->operation) == 0,
	    "primitive operation order");
	expect(ssid == NULL && ssid_length == 0U,
	    "preparation carries no SSID");
	expect(passphrase == NULL && passphrase_length == 0U,
	    "preparation carries no passphrase");
	expect(timeout == 10U, "preparation child deadline");
	expect(result != NULL, "primitive result storage");

	/* Publishes either a clean terminal record or the selected failure. */
	memset(result, 0, sizeof(*result));
	result->terminal_error = call->error;
	fixture_errno = call->error;
	return call->result;
}

/* Clears one child result through the production ownership boundary. */
static void
fixture_wifi_child_result_clear(
	struct networkd_wifi_child_result *result)
{
	/* Erases the complete bounded result. */
	if (result != NULL)
		memset(result, 0, sizeof(*result));
}

/* Clears one private protocol buffer. */
static void
fixture_protocol_clear(
	void *buffer,
	size_t length)
{
	/* Erases the complete caller-provided span. */
	if (buffer != NULL)
		memset(buffer, 0, length);
}

/* Accepts a machine with no currently discovered WLAN radio. */
static void
test_empty_radio_set(
	void)
{
	/* Verifies that an empty stable-order set needs no primitive call. */
	set_scenario(NULL, 0U);
	expect(prepare_wlan_radios(NULL, 0U, NULL, 0U, NULL) == 0,
	    "empty radio set accepted");
	expect_scenario_complete("empty radio primitive count");
}

/* Preserves a later ready radio after the first radio cannot rise. */
static void
test_later_radio_survives_up_failure(
	void)
{
	static const struct fixture_call calls[] = {
		{"wlan0", "up", -1, EIO},
		{"wlan1", "up", 0, 0},
		{"wlan1", "search-start", 0, 0}
	};
	struct networkd_wlan_radio radios[2];

	/* Prepares both records in their kernel discovery order. */
	initialize_radio(&radios[0], "wlan0", 10U);
	initialize_radio(&radios[1], "wlan1", 11U);
	set_scenario(calls, sizeof(calls) / sizeof(calls[0]));

	/* Requires the later usable radio to survive the first up failure. */
	expect(prepare_wlan_radios(radios, 2U, NULL, 0U, NULL) == 0,
	    "later radio survives up failure");
	expect(radios[0].ready == 0 && radios[1].ready == 1,
	    "up failure ready publication");
	expect_scenario_complete("up failure primitive sequence");
}

/* Preserves a later ready radio after the first radio cannot scan. */
static void
test_later_radio_survives_scan_failure(
	void)
{
	static const struct fixture_call calls[] = {
		{"wlan0", "up", 0, 0},
		{"wlan0", "search-start", -1, ETIMEDOUT},
		{"wlan1", "up", 0, 0},
		{"wlan1", "search-start", 0, 0}
	};
	struct networkd_wlan_radio radios[2];

	/* Prepares both records in their kernel discovery order. */
	initialize_radio(&radios[0], "wlan0", 20U);
	initialize_radio(&radios[1], "wlan1", 21U);
	set_scenario(calls, sizeof(calls) / sizeof(calls[0]));

	/* Requires the later usable radio to survive the first scan failure. */
	expect(prepare_wlan_radios(radios, 2U, NULL, 0U, NULL) == 0,
	    "later radio survives scan failure");
	expect(radios[0].ready == 0 && radios[1].ready == 1,
	    "scan failure ready publication");
	expect_scenario_complete("scan failure primitive sequence");
}

/* Rejects a nonempty set only when every discovered radio fails. */
static void
test_all_radios_failed(
	void)
{
	static const struct fixture_call calls[] = {
		{"wlan0", "up", -1, ENODEV},
		{"wlan1", "up", -1, EIO}
	};
	struct networkd_wlan_radio radios[2];

	/* Prepares both records in their kernel discovery order. */
	initialize_radio(&radios[0], "wlan0", 30U);
	initialize_radio(&radios[1], "wlan1", 31U);
	set_scenario(calls, sizeof(calls) / sizeof(calls[0]));

	/* Retains the first failure only after exhausting every radio. */
	expect(prepare_wlan_radios(radios, 2U, NULL, 0U, NULL) == -1,
	    "all failed radio set rejected");
	expect(fixture_errno == ENODEV, "first preparation error retained");
	expect(radios[0].ready == 0 && radios[1].ready == 0,
	    "failed radios never published ready");
	expect_scenario_complete("all failed primitive sequence");
}
