/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of passkey-fido2's pure parts (ws172-p003,
 * userland/base/passkey-fido2/wire.c): base64url (RFC 4648's examples,
 * the characters and lengths it refuses), hexadecimal, the helper's
 * messages, a key's line of /etc/passkey made, read and counted again,
 * the labels, and the login's hashes against the same bytes hashed by
 * hand.
 */

#include "userland/base/passkey-fido2/fido2.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The checks that failed. */
static unsigned failures;

static void expect(int condition, const char *what);

/*
 * Runs the checks; the exit status says whether they all held.
 */
int
main(void)
{
	static const char *const plain[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
	static const char *const coded[] = { "", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy" };
	static const uint8_t binary[] = { 0xfb, 0xff, 0xbf, 0x00 };
	static struct fido2_message message;
	static struct fido2_record record;
	struct pk_crypto_part part;
	uint8_t bytes[64];
	uint8_t challenge[FIDO2_CHALLENGE_SIZE];
	uint8_t hash[PK_SHA256_SIZE];
	uint8_t expected[PK_SHA256_SIZE];
	uint8_t joined[128];
	char text[256];
	char line[512];
	char again[512];
	size_t size;
	size_t index;
	int error;

	/* base64url: RFC 4648's examples both ways, the URL alphabet, and what is refused. */
	for (index = 0U; index < sizeof(plain) / sizeof(plain[0]); index++) {
		error = fido2_base64_encode((const uint8_t *)plain[index], strlen(plain[index]), text, sizeof(text));
		expect(error == 0 && strcmp(text, coded[index]) == 0, "base64url encodes RFC 4648's example");
		error = fido2_base64_decode(coded[index], bytes, sizeof(bytes), &size);
		expect(error == 0 && size == strlen(plain[index]) && memcmp(bytes, plain[index], size) == 0, "base64url decodes RFC 4648's example");
	}

	/* The URL alphabet, and what is refused. */
	error = fido2_base64_encode(binary, sizeof(binary), text, sizeof(text));
	expect(error == 0 && strcmp(text, "-_-_AA") == 0, "the URL alphabet's - and _");
	expect(fido2_base64_decode("Zm9v+g", bytes, sizeof(bytes), &size) == EINVAL, "a + is refused");
	expect(fido2_base64_decode("Zm9v=", bytes, sizeof(bytes), &size) == EINVAL, "padding is refused");
	expect(fido2_base64_decode("Zm9vY", bytes, sizeof(bytes), &size) == EINVAL, "a length no encoding has");
	expect(fido2_base64_decode("Zm9vYmFy", bytes, 5U, &size) == ENOSPC, "no room");
	expect(fido2_base64_encode(binary, sizeof(binary), text, 6U) == ENOSPC, "no room to encode");

	/* Hexadecimal. */
	error = fido2_hex_encode(binary, sizeof(binary), text, sizeof(text));
	expect(error == 0 && strcmp(text, "fbffbf00") == 0, "hexadecimal");

	/* The helper's messages. */
	(void)snprintf(line, sizeof(line), "touch");
	error = fido2_message_parse(line, &message);
	expect(error == 0 && message.kind == FIDO2_MESSAGE_TOUCH, "touch");
	(void)snprintf(line, sizeof(line), "fail many-keys");
	error = fido2_message_parse(line, &message);
	expect(error == 0 && message.kind == FIDO2_MESSAGE_FAIL && strcmp(message.reason, "many-keys") == 0, "fail");
	(void)snprintf(line, sizeof(line), "assertion 0102 a0a1a2 3044");
	error = fido2_message_parse(line, &message);
	expect(error == 0 && message.kind == FIDO2_MESSAGE_ASSERTION && message.id_size == 2U && message.auth_data_size == 3U &&
	    message.signature_size == 2U && message.auth_data[2] == 0xa2U, "assertion");
	(void)snprintf(line, sizeof(line), "made 00ff");
	error = fido2_message_parse(line, &message);
	expect(error == 0 && message.kind == FIDO2_MESSAGE_MADE && message.auth_data_size == 2U && message.auth_data[1] == 0xffU, "made");
	(void)snprintf(line, sizeof(line), "assertion 0102 a0a1a2");
	expect(fido2_message_parse(line, &message) == EINVAL, "an assertion without its signature");
	(void)snprintf(line, sizeof(line), "assertion 0102 a0a1a2 3044 extra");
	expect(fido2_message_parse(line, &message) == EINVAL, "a word too many");
	(void)snprintf(line, sizeof(line), "made 0G");
	expect(fido2_message_parse(line, &message) == EINVAL, "a byte that is not hexadecimal");
	(void)snprintf(line, sizeof(line), "made ABCD");
	expect(fido2_message_parse(line, &message) == EINVAL, "capital digits are not the helper's");
	(void)snprintf(line, sizeof(line), "owner 0,1");
	error = fido2_message_parse(line, &message);
	expect(error == 0 && message.kind == FIDO2_MESSAGE_OWNER && message.held == 0U && message.owner_card == 1U, "owner: none held");
	(void)snprintf(line, sizeof(line), "owner 5,0 0102 a0a1a2 3044");
	error = fido2_message_parse(line, &message);
	expect(error == 0 && message.kind == FIDO2_MESSAGE_OWNER && message.held == 5U && message.owner_card == 0U &&
	    message.id_size == 2U && message.auth_data_size == 3U && message.signature_size == 2U, "owner: two groups and the first's answer");
	(void)snprintf(line, sizeof(line), "owner 1,0");
	expect(fido2_message_parse(line, &message) == EINVAL, "owner: a group held without its answer");
	(void)snprintf(line, sizeof(line), "owner 0,0 0102 a0a1a2 3044");
	expect(fido2_message_parse(line, &message) == EINVAL, "owner: an answer without a group");
	(void)snprintf(line, sizeof(line), "owner 1,2 0102 a0a1a2 3044");
	expect(fido2_message_parse(line, &message) == EINVAL, "owner: a card that is neither");
	(void)snprintf(line, sizeof(line), "owner 1,0 0102 a0a1a2");
	expect(fido2_message_parse(line, &message) == EINVAL, "owner: an answer without its signature");
	(void)snprintf(line, sizeof(line), "owner 1,0 0102 a0a1a2 3044 extra");
	expect(fido2_message_parse(line, &message) == EINVAL, "owner: a word too many");
	(void)snprintf(line, sizeof(line), "touched");
	expect(fido2_message_parse(line, &message) == EINVAL, "an unknown message");

	/* A key's line: made, read back, counted again. */
	error = fido2_record_line("kei", 1000, "AQI", "pQECAyYgASFYIA", 3U, "YubiKey 5", "2026-10-06", line, sizeof(line));
	expect(error == 0 && strcmp(line, "kei:1000:fido2:AQI:pQECAyYgASFYIA:3:zedbsd.login:YubiKey 5:2026-10-06") == 0, "the line");
	error = fido2_record_parse(line, &record);
	expect(error == 0 && record.id_size == 2U && record.id[0] == 1U && record.id[1] == 2U && record.sign_count == 3U &&
	    strcmp(record.label, "YubiKey 5") == 0 && strcmp(record.id_text, "AQI") == 0, "the line read back");
	error = fido2_record_recount(line, 4294967295U, again, sizeof(again));
	expect(error == 0 && strcmp(again, "kei:1000:fido2:AQI:pQECAyYgASFYIA:4294967295:zedbsd.login:YubiKey 5:2026-10-06") == 0, "counted again");
	expect(fido2_record_parse("kei:1000:fido2:AQI:pQEC:3:example.com:Web:2026-10-06", &record) == EINVAL, "another relying party");
	expect(fido2_record_parse("kei:1000:fido2:AQI:pQEC:x:zedbsd.login:L:2026-10-06", &record) == EINVAL, "a count that is not one");
	expect(fido2_record_parse("kei:1000:fido2:AQI:pQEC:3:zedbsd.login:L", &record) == EINVAL, "a field missing");
	expect(fido2_record_parse("kei:1000:fido2:AQI:pQEC:3:zedbsd.login:L:D:more", &record) == EINVAL, "a field too many");
	expect(fido2_record_parse("kei:1000:pin:AQI:pQEC:3:zedbsd.login:L:D", &record) == EINVAL, "a PIN's line");
	expect(fido2_record_parse("kei:1000:fido2::pQEC:3:zedbsd.login:L:D", &record) == EINVAL, "an empty ID");

	/* Labels. */
	expect(fido2_label_valid("YubiKey 5 NFC") && fido2_label_valid("鍵"), "labels");
	expect(!fido2_label_valid("") && !fido2_label_valid("a:b") && !fido2_label_valid("tab\there") &&
	    !fido2_label_valid("123456789012345678901234567890123"), "labels refused");

	/* The client data hash: SHA-256 of "zedbsd.login" NUL "kei" NUL challenge. */
	memset(challenge, 0x5a, sizeof(challenge));
	error = fido2_client_data_hash("kei", challenge, hash);
	memcpy(joined, "zedbsd.login\0kei\0", 17U);
	memcpy(joined + 17U, challenge, sizeof(challenge));
	part.data = joined;
	part.size = 17U + sizeof(challenge);
	(void)pk_crypto_sha256(&part, 1U, expected);
	expect(error == 0 && memcmp(hash, expected, sizeof(hash)) == 0, "the client data hash");

	/* The user's ID: the first 16 bytes of SHA-256("kei"). */
	error = fido2_user_id("kei", bytes);
	part.data = (const uint8_t *)"kei";
	part.size = 3U;
	(void)pk_crypto_sha256(&part, 1U, expected);
	expect(error == 0 && memcmp(bytes, expected, FIDO2_USER_ID_SIZE) == 0, "the user's ID");

	/* The verdict. */
	if (failures != 0U) {
		printf("fido2-wire-host-test: %u FAILED\n", failures);
		return 1;
	}

	/* Every check held. */
	printf("fido2-wire-host-test: PASS\n");
	return 0;
}

/* Counts and reports a check that does not hold. */
static void
expect(
	int condition,
	const char *what)
{
	/* A check that holds says nothing. */
	if (condition)
		return;
	failures++;
	printf("FAIL: %s\n", what);
}
