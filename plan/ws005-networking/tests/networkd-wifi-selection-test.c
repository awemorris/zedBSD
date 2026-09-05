/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises production networkd WLAN selection and attempt policy.
 */

#include "userland/base/net/netutil.h"
#include "userland/base/net/wifi-conf.h"
#include "userland/base/networkd/managed-wlan.h"
#include "userland/base/networkd/wifi-child.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE_RADIO_MAX	4U
#define FIXTURE_PROFILE_MAX	8U
#define FIXTURE_CONNECT_MAX	8U

struct fixture_connect_call {
	char interface[IFNAMSIZ];
	unsigned char ssid[WIFI_CONF_SSID_MAX];
	size_t ssid_length;
};

static int fixture_wifi_child_run(const char *, const char *, const void *, size_t, const void *, size_t, unsigned, struct networkd_wifi_child_result *);
static void fixture_wifi_child_result_clear(struct networkd_wifi_child_result *);
static int fixture_wifi_child_parse_list(const struct networkd_wifi_child_result *, const void *, size_t, struct networkd_wifi_list_result *);
static void fixture_protocol_clear(void *, size_t);
static uint64_t fixture_monotonic_us(void);
static int fixture_ifindex(int, const char *, uint32_t *);

#define networkd_wifi_child_run fixture_wifi_child_run
#define networkd_wifi_child_result_clear fixture_wifi_child_result_clear
#define networkd_wifi_child_parse_list fixture_wifi_child_parse_list
#define networkd_protocol_clear fixture_protocol_clear
#define netutil_monotonic_us fixture_monotonic_us
#define netutil_ifindex fixture_ifindex
#define main networkd_program_main
#include "userland/base/networkd/main.c"
#undef main
#undef netutil_ifindex
#undef netutil_monotonic_us
#undef networkd_protocol_clear
#undef networkd_wifi_child_parse_list
#undef networkd_wifi_child_result_clear
#undef networkd_wifi_child_run

static void expect(int, const char *);
static void reset_fixture(void);
static void initialize_radio(struct networkd_wlan_radio *, const char *, uint32_t);
static void initialize_profile(struct wifi_conf_profile *, const char *, int);
static size_t fixture_radio_index(const char *);
static size_t fixture_profile_index(const void *, size_t);
static void test_automatic_profile_then_radio_order(void);
static void test_manual_stable_radio_order(void);
static void test_four_attempt_cap_and_single_connection(void);

static int fixture_errno;
static uint64_t fixture_now;
static struct networkd_wlan_radio fixture_radios[FIXTURE_RADIO_MAX];
static size_t fixture_radio_count;
static struct wifi_conf_model *fixture_model;
static unsigned char fixture_visible[FIXTURE_PROFILE_MAX][FIXTURE_RADIO_MAX];
static struct fixture_connect_call fixture_connects[FIXTURE_CONNECT_MAX];
static size_t fixture_connect_count;
static unsigned fixture_active_connects;
static unsigned fixture_max_active_connects;

/* Provides the test libc error slot. */
int *
__libc_errno_location(
	void)
{
	/* Returns the deterministic fixture error slot. */
	return &fixture_errno;
}

/*
 * Initializes one fixture credential model.
 */
void
wifi_conf_model_init(
	struct wifi_conf_model *model)
{
	/* Mirrors the production model's empty representation. */
	if (model != NULL)
		memset(model, 0, sizeof(*model));
}

/*
 * Clears one fixture credential model.
 */
void
wifi_conf_model_clear(
	struct wifi_conf_model *model)
{
	/* Erases all credential-bearing storage. */
	if (model != NULL)
		memset(model, 0, sizeof(*model));
}

/*
 * Clears one fixture secret-bearing byte span.
 */
void
wifi_conf_explicit_clear(
	void *buffer,
	size_t length)
{
	/* Uses a volatile view to preserve the ownership boundary. */
	volatile unsigned char *cursor;

	cursor = (volatile unsigned char *)buffer;
	while (cursor != NULL && length != 0U) {
		*cursor++ = 0U;
		length--;
	}
}

/*
 * Rejects an unexpected credential-store load in this selection fixture.
 */
int
wifi_store_load_for_user(
	uid_t owner,
	struct wifi_conf_model *model,
	char *diagnostic,
	size_t capacity)
{
	/* Makes an accidental policy reload visible as a fixture failure. */
	(void)owner;
	(void)model;
	(void)diagnostic;
	(void)capacity;
	fixture_errno = EIO;
	return -1;
}

/*
 * Rejects an unexpected generic interface request in this fixture.
 */
int
netutil_ifreq(
	struct ifreq *request,
	const char *interface)
{
	/* Selection must use only the explicitly replaced ifindex boundary. */
	(void)request;
	(void)interface;
	fixture_errno = EIO;
	return -1;
}

/*
 * Rejects unexpected host-interface enumeration in this fixture.
 */
int
netutil_interfaces(
	int descriptor,
	struct ifreq **interfaces,
	unsigned *count)
{
	/* Selection consumes the stable radio array supplied by each test. */
	(void)descriptor;
	if (interfaces != NULL)
		*interfaces = NULL;
	if (count != NULL)
		*count = 0U;
	fixture_errno = EIO;
	return -1;
}

/* Runs the WLAN-selection fixture. */
int
main(
	void)
{
	/* Exercises both selection forms and the complete automatic loop. */
	test_automatic_profile_then_radio_order();
	test_manual_stable_radio_order();
	test_four_attempt_cap_and_single_connection();

	/* Reports successful completion. */
	puts("networkd-wifi-selection-test: PASS");
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
		fprintf(stderr, "networkd-wifi-selection-test: %s\n", message);
		exit(1);
	}
}

/* Resets every deterministic fixture input and observation. */
static void
reset_fixture(
	void)
{
	/* Erases selection inputs, connection records, and global policy. */
	memset(fixture_radios, 0, sizeof(fixture_radios));
	memset(fixture_visible, 0, sizeof(fixture_visible));
	memset(fixture_connects, 0, sizeof(fixture_connects));
	fixture_radio_count = 0U;
	fixture_model = NULL;
	fixture_connect_count = 0U;
	fixture_active_connects = 0U;
	fixture_max_active_connects = 0U;
	fixture_now = 1000000ULL;
	fixture_errno = 0;
	route_events = -1;
	route_event_sequence = 0U;
	networkd_managed_wlan_init(&managed_wlan);
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
	radio->ready = 1;
}

/* Initializes one saved profile without retaining a real credential. */
static void
initialize_profile(
	struct wifi_conf_profile *profile,
	const char *ssid,
	int automatic)
{
	static const unsigned char passphrase[] = "fixture-passphrase";
	size_t ssid_length;

	/* Populates only fields consumed by selection and the child boundary. */
	ssid_length = strlen(ssid);
	expect(ssid_length <= sizeof(profile->ssid), "fixture SSID length");
	memset(profile, 0, sizeof(*profile));
	memcpy(profile->ssid, ssid, ssid_length);
	profile->ssid_length = ssid_length;
	memcpy(profile->passphrase, passphrase, sizeof(passphrase) - 1U);
	profile->passphrase_length = sizeof(passphrase) - 1U;
	profile->automatic = automatic;
}

/* Resolves one fixture interface in the supplied stable order. */
static size_t
fixture_radio_index(
	const char *interface)
{
	size_t index;

	/* Uses record order rather than lexical or numeric name order. */
	for (index = 0U; index < fixture_radio_count; index++) {
		if (strcmp(fixture_radios[index].interface, interface) == 0)
			return index;
	}
	expect(0, "unknown fixture radio");
	return 0U;
}

/* Resolves one counted SSID in profile-file order. */
static size_t
fixture_profile_index(
	const void *ssid,
	size_t ssid_length)
{
	size_t index;

	/* Matches the exact counted bytes used by the production selector. */
	expect(fixture_model != NULL, "fixture model installed");
	for (index = 0U; index < fixture_model->profile_count; index++) {
		if (fixture_model->profiles[index].ssid_length == ssid_length &&
		    memcmp(fixture_model->profiles[index].ssid, ssid,
		    ssid_length) == 0)
			return index;
	}
	expect(0, "unknown fixture profile");
	return 0U;
}

/* Simulates the list and failing connect Wi-Fi child operations. */
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
	struct fixture_connect_call *call;
	size_t radio_index;

	/* Publishes one terminal scan snapshot tagged with its stable index. */
	expect(result != NULL, "child result storage");
	memset(result, 0, sizeof(*result));
	radio_index = fixture_radio_index(interface);
	if (strcmp(operation, "list") == 0) {
		expect(ssid == NULL && ssid_length == 0U,
		    "list carries no SSID");
		expect(passphrase == NULL && passphrase_length == 0U,
		    "list carries no credential");
		expect(timeout == 5U, "list child deadline");
		result->child_exit_status = (int)radio_index;
		return 0;
	}

	/* Records each actual connect while enforcing sole state ownership. */
	expect(strcmp(operation, "connect") == 0,
	    "unexpected selection primitive");
	expect(timeout != 0U && timeout <= NETWORKD_WLAN_CONNECT_SECONDS,
	    "connect child deadline");
	expect(ssid != NULL && ssid_length != 0U,
	    "connect carries counted SSID");
	expect(passphrase != NULL && passphrase_length != 0U,
	    "connect carries credential");
	expect(fixture_connect_count < FIXTURE_CONNECT_MAX,
	    "connect observation capacity");
	expect(fixture_active_connects == 0U,
	    "only one connect child active");
	expect(managed_wlan.state == NETWORKD_WLAN_CONNECTING,
	    "managed policy owns one connecting state");
	expect(strcmp(managed_wlan.connection.interface, interface) == 0,
	    "managed policy owns selected radio");
	fixture_active_connects++;
	if (fixture_active_connects > fixture_max_active_connects)
		fixture_max_active_connects = fixture_active_connects;
	call = &fixture_connects[fixture_connect_count++];
	memcpy(call->interface, interface, strlen(interface) + 1U);
	memcpy(call->ssid, ssid, ssid_length);
	call->ssid_length = ssid_length;
	fixture_active_connects--;
	result->terminal_error = EHOSTUNREACH;
	fixture_errno = EHOSTUNREACH;
	return -1;
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

/* Converts a tagged terminal scan into one profile visibility result. */
static int
fixture_wifi_child_parse_list(
	const struct networkd_wifi_child_result *result,
	const void *ssid,
	size_t ssid_length,
	struct networkd_wifi_list_result *parsed)
{
	size_t profile_index;
	size_t radio_index;

	/* Looks up the exact profile/radio cell in the fixture matrix. */
	expect(result != NULL && parsed != NULL, "parse result storage");
	profile_index = fixture_profile_index(ssid, ssid_length);
	radio_index = (size_t)result->child_exit_status;
	expect(profile_index < FIXTURE_PROFILE_MAX,
	    "profile visibility capacity");
	expect(radio_index < fixture_radio_count,
	    "radio visibility capacity");
	memset(parsed, 0, sizeof(*parsed));
	parsed->scan_terminal = 1;
	parsed->scan_complete = 1;
	parsed->ssid_found = fixture_visible[profile_index][radio_index] != 0U;
	parsed->ssid_supported = parsed->ssid_found;
	return 0;
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

/* Returns a fixed monotonic instant inside every fixture deadline. */
static uint64_t
fixture_monotonic_us(
	void)
{
	/* Avoids sleeping because every simulated scan is terminal. */
	return fixture_now;
}

/* Resolves one fixture radio without consulting the host kernel. */
static int
fixture_ifindex(
	int descriptor,
	const char *interface,
	uint32_t *ifindex)
{
	size_t radio_index;

	/* Mirrors the stable identity returned by enumeration. */
	(void)descriptor;
	expect(ifindex != NULL, "ifindex output storage");
	radio_index = fixture_radio_index(interface);
	*ifindex = fixture_radios[radio_index].ifindex;
	return 0;
}

/* Requires automatic profile order before stable radio order. */
static void
test_automatic_profile_then_radio_order(
	void)
{
	struct wifi_conf_model model;
	const struct wifi_conf_profile *selected;
	size_t selected_radio;
	int selection_result;

	/* Installs deliberately nonlexical radio names and mixed profiles. */
	reset_fixture();
	memset(&model, 0, sizeof(model));
	initialize_profile(&model.profiles[0], "manual-first", 0);
	initialize_profile(&model.profiles[1], "auto-first", 1);
	initialize_profile(&model.profiles[2], "auto-second", 1);
	model.profile_count = 3U;
	initialize_radio(&fixture_radios[0], "wlan9", 91U);
	initialize_radio(&fixture_radios[1], "wlan2", 22U);
	fixture_radio_count = 2U;
	fixture_model = &model;
	fixture_visible[1][0] = 1U;
	fixture_visible[1][1] = 1U;
	fixture_visible[2][0] = 1U;
	fixture_visible[2][1] = 1U;

	/* Walks the candidate order used by successive actual attempts. */
	selection_result = select_profile_radio(fixture_radios,
	    fixture_radio_count, &model, 0U, &selected, &selected_radio,
	    fixture_now + 1U);
	expect(selection_result == 0 &&
	    selected == &model.profiles[1] && selected_radio == 0U,
	    "first auto profile on first stable radio");
	expect(select_profile_radio(fixture_radios, fixture_radio_count,
	    &model, 1U, &selected, &selected_radio, fixture_now + 1U) == 0 &&
	    selected == &model.profiles[1] && selected_radio == 1U,
	    "first auto profile on second stable radio");
	expect(select_profile_radio(fixture_radios, fixture_radio_count,
	    &model, 2U, &selected, &selected_radio, fixture_now + 1U) == 0 &&
	    selected == &model.profiles[2] && selected_radio == 0U,
	    "second auto profile follows first profile radios");
}

/* Requires manual selection to use the first stable radio which sees SSID. */
static void
test_manual_stable_radio_order(
	void)
{
	struct wifi_conf_model model;
	size_t selected_radio;

	/* Makes only the second and third stable-order radios see the target. */
	reset_fixture();
	memset(&model, 0, sizeof(model));
	initialize_profile(&model.profiles[0], "manual-target", 0);
	model.profile_count = 1U;
	initialize_radio(&fixture_radios[0], "wlan8", 81U);
	initialize_radio(&fixture_radios[1], "wlan3", 32U);
	initialize_radio(&fixture_radios[2], "wlan1", 13U);
	fixture_radio_count = 3U;
	fixture_model = &model;
	fixture_visible[0][1] = 1U;
	fixture_visible[0][2] = 1U;

	/* Refuses to prefer the lexically earlier or faster-looking radio. */
	expect(select_manual_radio(fixture_radios, fixture_radio_count,
	    &model.profiles[0], &selected_radio, fixture_now + 1U) == 0 &&
	    selected_radio == 1U, "manual stable radio order");
}

/* Requires four sequential actual attempts and no fifth candidate attempt. */
static void
test_four_attempt_cap_and_single_connection(
	void)
{
	struct wifi_conf_model model;
	char output[NETWORKD_RESPONSE_MAX];
	size_t output_length;
	int no_candidate;
	int result;

	/* Exposes six ordered candidate pairs while making every connect fail. */
	reset_fixture();
	memset(&model, 0, sizeof(model));
	initialize_profile(&model.profiles[0], "auto-one", 1);
	initialize_profile(&model.profiles[1], "auto-two", 1);
	initialize_profile(&model.profiles[2], "auto-three", 1);
	model.profile_count = 3U;
	initialize_radio(&fixture_radios[0], "wlan5", 51U);
	initialize_radio(&fixture_radios[1], "wlan0", 2U);
	fixture_radio_count = 2U;
	fixture_model = &model;
	memset(fixture_visible, 1, sizeof(fixture_visible));
	memset(output, 0, sizeof(output));
	output_length = 0U;
	no_candidate = 0;
	expect(networkd_managed_wlan_enable(&managed_wlan, 1001U) == 0,
	    "enable automatic fixture policy");

	/* Runs the production four-attempt loop against the failing child. */
	result = connect_automatic(fixture_radios, fixture_radio_count, &model,
	    fixture_now + 90000000ULL, output, sizeof(output), &output_length,
	    &no_candidate);
	expect(result == -1 && fixture_errno == EHOSTUNREACH,
	    "automatic terminal failure retained");
	expect(no_candidate == 0, "visible candidates are not no-candidate");
	expect(fixture_connect_count == 4U, "four actual connect attempt cap");
	expect(fixture_max_active_connects == 1U,
	    "one simultaneous connection attempt");
	expect(strcmp(fixture_connects[0].interface, "wlan5") == 0 &&
	    fixture_connects[0].ssid_length == strlen("auto-one") &&
	    memcmp(fixture_connects[0].ssid, "auto-one",
	    strlen("auto-one")) == 0, "attempt zero candidate");
	expect(strcmp(fixture_connects[1].interface, "wlan0") == 0 &&
	    memcmp(fixture_connects[1].ssid, "auto-one",
	    strlen("auto-one")) == 0, "attempt one candidate");
	expect(strcmp(fixture_connects[2].interface, "wlan5") == 0 &&
	    memcmp(fixture_connects[2].ssid, "auto-two",
	    strlen("auto-two")) == 0, "attempt two candidate");
	expect(strcmp(fixture_connects[3].interface, "wlan0") == 0 &&
	    memcmp(fixture_connects[3].ssid, "auto-two",
	    strlen("auto-two")) == 0, "attempt three candidate");
	expect(managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING &&
	    managed_wlan.connection.interface[0] == '\0',
	    "failed generation releases sole connection");
}
