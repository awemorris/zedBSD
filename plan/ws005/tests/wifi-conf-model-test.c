/*
 * WS005 NET-T21 strict wifi-conf v1 parser/model/serializer fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#include "userland/base/net/wifi-conf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s (errno=%d)\n", \
		    __FILE__, __LINE__, #expression, errno); \
		exit(1); \
	} \
} while (0)

static int
contains_bytes(const void *haystack_pointer, size_t haystack_length,
	       const void *needle_pointer, size_t needle_length)
{
	const unsigned char *haystack = haystack_pointer;
	const unsigned char *needle = needle_pointer;
	size_t index;

	if (needle_length == 0)
		return 1;
	if (needle_length > haystack_length)
		return 0;
	for (index = 0; index <= haystack_length - needle_length; index++)
		if (memcmp(haystack + index, needle, needle_length) == 0)
			return 1;
	return 0;
}

static void
expect_rejected(const void *input, size_t length)
{
	struct wifi_conf_model model;
	char error[WIFI_CONF_DIAGNOSTIC_MAX];

	wifi_conf_model_init(&model);
	CHECK(wifi_conf_set_key(&model, "sentinel", 8U, "sentinel-pass", 13U,
	    0, error, sizeof(error)) == 0);
	CHECK(wifi_conf_parse(input, length, &model, error, sizeof(error)) != 0);
	CHECK(model.profile_count == 1U);
	CHECK(model.profiles[0].ssid_length == 8U);
	CHECK(memcmp(model.profiles[0].ssid, "sentinel", 8U) == 0);
	CHECK(strlen(error) < WIFI_CONF_DIAGNOSTIC_MAX);
	wifi_conf_model_clear(&model);
}

static void
test_empty_and_round_trip(void)
{
	static const char empty[] = "wifi-conf 1\n";
	static const char source[] =
	    "wifi-conf 1\n"
	    "network \"A\\x42\\\\\\\"\\t\\n\\r\\x80\" wpa2-personal-ccmp "
	    "\"abcdefgh\" auto\n";
	static const char canonical[] =
	    "wifi-conf 1\n"
	    "network \"AB\\\\\\\"\\t\\n\\r\\x80\" wpa2-personal-ccmp "
	    "\"abcdefgh\" auto\n";
	struct wifi_conf_model model, reparsed;
	unsigned char output[WIFI_CONF_FILE_MAX + 1U];
	char error[WIFI_CONF_DIAGNOSTIC_MAX];
	size_t length;

	wifi_conf_model_init(&model);
	CHECK(wifi_conf_parse(empty, sizeof(empty) - 1U, &model, error,
	    sizeof(error)) == 0);
	CHECK(model.profile_count == 0U && model.passphrase_bytes == 0U);
	CHECK(wifi_conf_serialize(&model, output, sizeof(output), &length, error,
	    sizeof(error)) == 0);
	CHECK(length == sizeof(empty) - 1U && memcmp(output, empty, length) == 0);

	CHECK(wifi_conf_parse(source, sizeof(source) - 1U, &model, error,
	    sizeof(error)) == 0);
	CHECK(model.profile_count == 1U && model.profiles[0].ssid_length == 8U);
	CHECK(model.profiles[0].ssid[0] == 'A' &&
	    model.profiles[0].ssid[1] == 'B' &&
	    model.profiles[0].ssid[2] == '\\' &&
	    model.profiles[0].ssid[3] == '"' &&
	    model.profiles[0].ssid[4] == '\t' &&
	    model.profiles[0].ssid[5] == '\n' &&
	    model.profiles[0].ssid[6] == '\r' &&
	    model.profiles[0].ssid[7] == 0x80U);
	CHECK(wifi_conf_serialize(&model, output, sizeof(output), &length, error,
	    sizeof(error)) == 0);
	CHECK(length == sizeof(canonical) - 1U);
	CHECK(memcmp(output, canonical, length) == 0);
	wifi_conf_model_init(&reparsed);
	CHECK(wifi_conf_parse(output, length, &reparsed, error, sizeof(error)) == 0);
	CHECK(reparsed.profile_count == 1U &&
	    memcmp(&reparsed.profiles[0], &model.profiles[0],
	    sizeof(model.profiles[0])) == 0);
	wifi_conf_model_clear(&reparsed);
	wifi_conf_model_clear(&model);
	wifi_conf_explicit_clear(output, sizeof(output));
}

static void
test_strict_rejections(void)
{
	static const char *const invalid[] = {
		"",
		"wifi-conf 1",
		"wifi-conf 2\n",
		"wifi-conf 1\r\n",
		"wifi-conf 1\n\n",
		"wifi-conf 1\nnetwork\t\"ssid\" wpa2-personal-ccmp \"abcdefgh\" auto\n",
		"wifi-conf 1\nnetwork \"\" wpa2-personal-ccmp \"abcdefgh\" auto\n",
		"wifi-conf 1\nnetwork \"ssid\\x00\" wpa2-personal-ccmp \"abcdefgh\" auto\n",
		"wifi-conf 1\nnetwork \"ssid\" open \"abcdefgh\" auto\n",
		"wifi-conf 1\nnetwork \"ssid\" wpa2-personal-ccmp \"abcdefg\" auto\n",
		"wifi-conf 1\nnetwork \"ssid\" wpa2-personal-ccmp \"abcdefgh\\n\" auto\n",
		"wifi-conf 1\nnetwork \"ssid\" wpa2-personal-ccmp \"abcdefgh\" maybe\n",
		"wifi-conf 1\nnetwork \"ssid\" wpa2-personal-ccmp \"abcdefgh\" auto trailing\n",
		"wifi-conf 1\nnetwork \"ssid\\q\" wpa2-personal-ccmp \"abcdefgh\" auto\n",
		"wifi-conf 1\nnetwork \"ssid\\x0\" wpa2-personal-ccmp \"abcdefgh\" auto\n",
		"wifi-conf 1\nnetwork \"ssid\" wpa2-personal-ccmp \"abcdefgh\" auto\n"
		"network \"\\x73sid\" wpa2-personal-ccmp \"ijklmnop\" manual\n",
	};
	unsigned char embedded_nul[] = "wifi-conf 1\nnetwork \"ssid\" wpa2-personal-ccmp \"abcdefgh\" auto\n";
	size_t index;

	for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++)
		expect_rejected(invalid[index], strlen(invalid[index]));
	embedded_nul[20] = 0;
	expect_rejected(embedded_nul, sizeof(embedded_nul) - 1U);
}

static void
test_bounds(void)
{
	struct wifi_conf_model model;
	struct wifi_conf_model parsed;
	unsigned char *input;
	unsigned char serialized[WIFI_CONF_FILE_MAX + 1U];
	char error[WIFI_CONF_DIAGNOSTIC_MAX];
	char ssid[WIFI_CONF_SSID_MAX + 2U];
	char passphrase[WIFI_CONF_PASSPHRASE_MAX + 2U];
	size_t index, length;

	CHECK(wifi_conf_validate_profile("s", 1U, "12345678", 8U, error,
	    sizeof(error)) == 0);
	CHECK(wifi_conf_validate_profile("", 0U, "12345678", 8U, error,
	    sizeof(error)) != 0);
	CHECK(wifi_conf_validate_profile("s", 1U, "1234567", 7U, error,
	    sizeof(error)) != 0);

	memset(ssid, 's', sizeof(ssid));
	ssid[WIFI_CONF_SSID_MAX] = '\0';
	memset(passphrase, 'p', sizeof(passphrase));
	passphrase[WIFI_CONF_PASSPHRASE_MAX] = '\0';
	wifi_conf_model_init(&model);
	CHECK(wifi_conf_set_key(&model, ssid, strlen(ssid), passphrase,
	    strlen(passphrase), 1, error,
	    sizeof(error)) == 0);
	ssid[WIFI_CONF_SSID_MAX] = 's';
	ssid[WIFI_CONF_SSID_MAX + 1U] = '\0';
	CHECK(wifi_conf_set_key(&model, ssid, strlen(ssid), passphrase,
	    strlen(passphrase), 0, error,
	    sizeof(error)) != 0);
	ssid[WIFI_CONF_SSID_MAX] = '\0';
	passphrase[WIFI_CONF_PASSPHRASE_MAX] = 'p';
	passphrase[WIFI_CONF_PASSPHRASE_MAX + 1U] = '\0';
	CHECK(wifi_conf_set_key(&model, "other", 5U, passphrase,
	    strlen(passphrase), 0, error,
	    sizeof(error)) != 0);
	wifi_conf_model_clear(&model);

	input = malloc(WIFI_CONF_FILE_MAX + 1U);
	CHECK(input != NULL);
	memset(input, 'x', WIFI_CONF_FILE_MAX + 1U);
	input[WIFI_CONF_FILE_MAX - 2U] = '\n';
	expect_rejected(input, WIFI_CONF_FILE_MAX - 1U);
	input[WIFI_CONF_FILE_MAX - 1U] = '\n';
	expect_rejected(input, WIFI_CONF_FILE_MAX);
	expect_rejected(input, WIFI_CONF_FILE_MAX + 1U);
	memset(input, 'x', WIFI_CONF_LINE_MAX + 1U);
	input[WIFI_CONF_LINE_MAX - 2U] = '\n';
	expect_rejected(input, WIFI_CONF_LINE_MAX - 1U);
	input[WIFI_CONF_LINE_MAX - 2U] = 'x';
	input[WIFI_CONF_LINE_MAX - 1U] = '\n';
	expect_rejected(input, WIFI_CONF_LINE_MAX);
	input[WIFI_CONF_LINE_MAX - 1U] = 'x';
	input[WIFI_CONF_LINE_MAX] = '\n';
	expect_rejected(input, WIFI_CONF_LINE_MAX + 1U);
	free(input);

	wifi_conf_model_init(&model);
	for (index = 0; index < WIFI_CONF_PROFILE_MAX; index++) {
		char name[8];

		(void)snprintf(name, sizeof(name), "s%02lu",
		    (unsigned long)index);
		CHECK(wifi_conf_set_key(&model, name, strlen(name), "12345678",
		    8U, 0, error, sizeof(error)) == 0);
	}
	CHECK(model.profile_count == WIFI_CONF_PROFILE_MAX);
	CHECK(wifi_conf_set_key(&model, "overflow", 8U, "12345678", 8U, 0,
	    error, sizeof(error)) != 0);
	CHECK(wifi_conf_serialize(&model, serialized, sizeof(serialized), &length,
	    error, sizeof(error)) == 0);
	CHECK(length + sizeof("network \"extra\" wpa2-personal-ccmp \"12345678\" manual\n") - 1U <
	    sizeof(serialized));
	memcpy(serialized + length,
	    "network \"extra\" wpa2-personal-ccmp \"12345678\" manual\n",
	    sizeof("network \"extra\" wpa2-personal-ccmp \"12345678\" manual\n") - 1U);
	wifi_conf_model_init(&parsed);
	CHECK(wifi_conf_parse(serialized,
	    length + sizeof("network \"extra\" wpa2-personal-ccmp \"12345678\" manual\n") - 1U,
	    &parsed, error, sizeof(error)) != 0);
	CHECK(parsed.profile_count == 0U);
	wifi_conf_model_clear(&parsed);
	wifi_conf_model_clear(&model);
	wifi_conf_explicit_clear(serialized, sizeof(serialized));

	wifi_conf_model_init(&model);
	memset(passphrase, 'p', WIFI_CONF_PASSPHRASE_MAX);
	for (index = 0; index < WIFI_CONF_PROFILE_MAX; index++) {
		char name[8];

		(void)snprintf(name, sizeof(name), "m%02lu",
		    (unsigned long)index);
		CHECK(wifi_conf_set_key(&model, name, strlen(name), passphrase,
		    WIFI_CONF_PASSPHRASE_MAX, 1, error, sizeof(error)) == 0);
	}
	CHECK(model.passphrase_bytes == 4032U);
	CHECK(wifi_conf_serialize(&model, serialized, sizeof(serialized), &length,
	    error, sizeof(error)) == 0);
	wifi_conf_model_init(&parsed);
	CHECK(wifi_conf_parse(serialized, length, &parsed, error,
	    sizeof(error)) == 0);
	CHECK(parsed.profile_count == WIFI_CONF_PROFILE_MAX &&
	    parsed.passphrase_bytes == 4032U);
	wifi_conf_model_clear(&parsed);
	wifi_conf_model_clear(&model);
	wifi_conf_explicit_clear(serialized, sizeof(serialized));
}

static void
test_update_order_and_redaction(void)
{
	static const char source[] =
	    "wifi-conf 1\n"
	    "network \"first\" wpa2-personal-ccmp \"first-pass\" auto\n"
	    "network \"second\" wpa2-personal-ccmp \"second-pass\" auto\n";
	static const char dummy_secret[] = "dummy-secret-pass";
	struct wifi_conf_model model;
	unsigned char output[WIFI_CONF_FILE_MAX + 1U];
	unsigned char unchanged[16];
	char error[WIFI_CONF_DIAGNOSTIC_MAX];
	size_t length;

	wifi_conf_model_init(&model);
	CHECK(wifi_conf_parse(source, sizeof(source) - 1U, &model, error,
	    sizeof(error)) == 0);
	CHECK(wifi_conf_set_key(&model, "first", 5U, dummy_secret,
	    sizeof(dummy_secret) - 1U, 0, error, sizeof(error)) == 0);
	CHECK(model.profile_count == 2U && model.profiles[0].automatic == 0 &&
	    model.profiles[1].automatic == 1);
	CHECK(wifi_conf_set_key(&model, "third", 5U, "third-pass", 10U, 1,
	    error, sizeof(error)) == 0);
	CHECK(model.profile_count == 3U &&
	    memcmp(model.profiles[2].ssid, "third", 5U) == 0);
	memset(unchanged, 0xa5, sizeof(unchanged));
	CHECK(wifi_conf_serialize(&model, unchanged, sizeof(unchanged), &length,
	    error, sizeof(error)) != 0);
	CHECK(unchanged[0] == 0xa5U);
	CHECK(strstr(error, dummy_secret) == NULL);
	CHECK(wifi_conf_serialize(&model, output, sizeof(output), &length, error,
	    sizeof(error)) == 0);
	CHECK(contains_bytes(output, length, dummy_secret,
	    sizeof(dummy_secret) - 1U));
	memcpy(output, "wifi-conf 1\nnetwork \"ssid\" wpa2-personal-ccmp "
	    "\"dummy-secret-pass\" broken\n",
	    sizeof("wifi-conf 1\nnetwork \"ssid\" wpa2-personal-ccmp "
	    "\"dummy-secret-pass\" broken\n") - 1U);
	CHECK(wifi_conf_parse(output,
	    sizeof("wifi-conf 1\nnetwork \"ssid\" wpa2-personal-ccmp "
	    "\"dummy-secret-pass\" broken\n") - 1U,
	    &model, error, sizeof(error)) != 0);
	CHECK(strstr(error, dummy_secret) == NULL);
	wifi_conf_model_clear(&model);
	wifi_conf_explicit_clear(output, sizeof(output));
	wifi_conf_explicit_clear(unchanged, sizeof(unchanged));
	for (length = 0; length < sizeof(model); length++)
		CHECK(((unsigned char *)&model)[length] == 0);
}

int
main(void)
{
	test_empty_and_round_trip();
	test_strict_rejections();
	test_bounds();
	test_update_order_and_redaction();
	puts("wifi-conf model test: PASS");
	return 0;
}
