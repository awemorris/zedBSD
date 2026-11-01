/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of /sbin/passkey's pure parts (ws172-p002,
 * userland/base/passkey/request.c and record.c): the requests it takes and
 * refuses, and the records of /etc/passkey (finding a PIN by name and user
 * ID, a stale line of another user ID, replacing and removing, keeping
 * lines of other kinds and comments, the header and the version).
 */

#include "userland/base/passkey/passkey.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;

static void
expect(
	int condition,
	const char *what)
{
	if (condition)
		return;
	failures++;
	printf("FAIL: %s\n", what);
}

/* Parses a request given as a C string; returns the error. */
static int
parse(
	const char *text,
	struct passkey_request *request,
	char *buffer)
{
	size_t length;

	length = strlen(text);
	memcpy(buffer, text, length);
	return passkey_request_parse(buffer, length, request);
}

int
main(void)
{
	static const char file[] =
	    "# zedBSD passkey 1\n"
	    "kei:1000:pin:$6$rounds=5$salt$hashkei\n"
	    "kei:1000:fido2:AAAA:BBBB:3:zedbsd.login:YubiKey:2026-10-05\n"
	    "old:1001:pin:$6$stale\n"
	    "kei:1000:chip:tpm:1:Chip\n"
	    "# a comment\n"
	    "kei:1000:fido2:CCCC:DDDD:0:zedbsd.login:Other:2026-10-05\n";
	struct passkey_request request;
	char buffer[PASSKEY_REQUEST_MAX + 1];
	char line[256];
	char output[1024];
	size_t written;
	int error;

	/* Requests taken. */
	error = parse("auth\nkei\npin\n123456\n", &request, buffer);
	expect(error == 0 && request.operation == PASSKEY_OP_AUTH && strcmp(request.fields[3], "123456") == 0, "auth pin");
	error = parse("auth\nkei\npassword\npass word with spaces\n", &request, buffer);
	expect(error == 0 && strcmp(request.fields[3], "pass word with spaces") == 0, "a password with spaces");
	error = parse("auth\nkei\npassword\n\n", &request, buffer);
	expect(error == 0 && request.fields[3][0] == '\0', "an empty password is a field");
	error = parse("styles\nkei\n", &request, buffer);
	expect(error == 0 && request.operation == PASSKEY_OP_STYLES, "styles");
	error = parse("enroll-pin\nkei\ncurrent password\n654321\n", &request, buffer);
	expect(error == 0 && request.operation == PASSKEY_OP_ENROLL_PIN, "enroll-pin");

	/* Requests refused. */
	expect(parse("auth\nkei\npin\n123456", &request, buffer) == EINVAL, "no last line end");
	expect(parse("auth\nkei\npin\n", &request, buffer) == EINVAL, "a field missing");
	expect(parse("auth\nkei\npin\n123456\nextra\n", &request, buffer) == EINVAL, "a field too many");
	expect(parse("styles\nkei\x01\n", &request, buffer) == EINVAL, "a control character");
	expect(parse("auth\nkei\npassword\npass\tword\n", &request, buffer) == EINVAL, "a tab in a secret");
	expect(parse("shell\nkei\n", &request, buffer) == EINVAL, "an unknown operation");
	expect(parse("styles\n\n", &request, buffer) == EINVAL, "an empty name");
	expect(parse("styles\nabcdefghijabcdefghijabcdefghijabc\n", &request, buffer) == EINVAL, "a name too long");

	/* PINs: four to six digits. */
	expect(passkey_is_pin("012345") && passkey_is_pin("01234") && passkey_is_pin("0000") && !passkey_is_pin("000") &&
	    !passkey_is_pin("") && !passkey_is_pin("0123456") && !passkey_is_pin("01234a") && !passkey_is_pin("12a4"),
	    "the PIN rule");

	/* Records: the version, the PIN of the name and user ID, a stale line, counts. */
	expect(passkey_record_version(file, sizeof(file) - 1) == 1, "version 1");
	expect(passkey_record_version("# zedBSD passkey 2\n", 19) == 2, "version 2");
	expect(passkey_record_version("", 0) == 1, "an empty file is version 1");
	error = passkey_record_find(file, sizeof(file) - 1, "kei", 1000, "pin", 0, line, sizeof(line));
	expect(error == 0 && strcmp(line, "kei:1000:pin:$6$rounds=5$salt$hashkei") == 0, "kei's PIN");
	error = passkey_record_find(file, sizeof(file) - 1, "kei", 1002, "pin", 0, line, sizeof(line));
	expect(error == ENOENT, "a PIN of another user ID does not count");
	error = passkey_record_find(file, sizeof(file) - 1, "ke", 1000, "pin", 0, line, sizeof(line));
	expect(error == ENOENT, "a prefix of a name is not the name");
	expect(passkey_record_count(file, sizeof(file) - 1, "kei", 1000, "fido2") == 2, "two keys");
	expect(passkey_record_count(file, sizeof(file) - 1, "old", 1001, "fido2") == 0, "no key");

	/* Replacing kei's PIN: every other line kept, the new one last, the header once. */
	error = passkey_record_replace(file, sizeof(file) - 1, "kei", "pin", "kei:1000:pin:$6$new", output, sizeof(output), &written);
	output[written] = '\0';
	expect(error == 0, "replace");
	expect(strncmp(output, "# zedBSD passkey 1\n", 19) == 0 && strstr(output + 1, "# zedBSD passkey") == NULL, "the header once");
	expect(strstr(output, "hashkei") == NULL && strstr(output, "kei:1000:pin:$6$new\n") != NULL, "the PIN replaced");
	expect(strstr(output, "old:1001:pin:$6$stale\n") != NULL && strstr(output, "kei:1000:chip:tpm:1:Chip\n") != NULL &&
	    strstr(output, "# a comment\n") != NULL && strstr(output, "YubiKey") != NULL, "the other lines kept");

	/* Removing every line of kei (an account removed), and a stale name with any user ID. */
	error = passkey_record_replace(file, sizeof(file) - 1, "kei", NULL, NULL, output, sizeof(output), &written);
	output[written] = '\0';
	expect(error == 0 && strstr(output, "kei:") == NULL && strstr(output, "old:1001") != NULL, "every line of kei removed");
	error = passkey_record_replace(file, sizeof(file) - 1, "old", "pin", NULL, output, sizeof(output), &written);
	output[written] = '\0';
	expect(error == 0 && strstr(output, "old:") == NULL, "a stale PIN removed");

	/* A text that does not fit. */
	error = passkey_record_replace(file, sizeof(file) - 1, "kei", "pin", NULL, output, 40, &written);
	expect(error == ENOSPC, "no room");

	/* One key's line (ws172-p003): added, its count changed, removed; the other key and the other kinds stay. */
	error = passkey_record_edit(file, sizeof(file) - 1, "kei", "fido2", NULL, "kei:1000:fido2:EEEE:FFFF:0:zedbsd.login:New:2026-10-06",
	    output, sizeof(output), &written);
	output[written] = '\0';
	expect(error == 0 && strstr(output, ":AAAA:") != NULL && strstr(output, ":CCCC:") != NULL && strstr(output, ":EEEE:") != NULL,
	    "a key added, the others kept");
	error = passkey_record_edit(file, sizeof(file) - 1, "kei", "fido2", "AAAA", "kei:1000:fido2:AAAA:BBBB:9:zedbsd.login:YubiKey:2026-10-05",
	    output, sizeof(output), &written);
	output[written] = '\0';
	expect(error == 0 && strstr(output, ":AAAA:BBBB:9:") != NULL && strstr(output, ":AAAA:BBBB:3:") == NULL && strstr(output, ":CCCC:") != NULL,
	    "a key's count changed");
	error = passkey_record_edit(file, sizeof(file) - 1, "kei", "fido2", "CCCC", NULL, output, sizeof(output), &written);
	output[written] = '\0';
	expect(error == 0 && strstr(output, ":CCCC:") == NULL && strstr(output, ":AAAA:") != NULL && strstr(output, "kei:1000:pin:") != NULL &&
	    strstr(output, "kei:1000:chip:") != NULL, "a key removed, the PIN and the chip kept");
	error = passkey_record_edit(file, sizeof(file) - 1, "old", "fido2", "AAAA", NULL, output, sizeof(output), &written);
	output[written] = '\0';
	expect(error == 0 && strstr(output, ":AAAA:") != NULL, "another name's ID is not kei's key");

	/* A line's fields and a key's reference. */
	error = passkey_record_field("kei:1000:fido2:AAAA:BBBB:3:zedbsd.login:YubiKey 5:2026-10-05", 7U, line, sizeof(line));
	expect(error == 0 && strcmp(line, "YubiKey 5") == 0, "the label field");
	error = passkey_record_field("kei:1000:fido2:AAAA:BBBB:3:zedbsd.login:YubiKey 5:2026-10-05", 8U, line, sizeof(line));
	expect(error == 0 && strcmp(line, "2026-10-05") == 0, "the last field");
	error = passkey_record_field("kei:1000:pin:hash", 5U, line, sizeof(line));
	expect(error == ENOENT, "no such field");
	error = passkey_record_field("kei:1000:fido2:AAAA", 3U, line, 4U);
	expect(error == ENAMETOOLONG, "a field too long");
	passkey_record_ref("AAAA", line, PASSKEY_REF_SIZE);
	expect(strcmp(line, "8943628b9f33b719") == 0, "a key's reference (FNV-1a)");

	if (failures != 0) {
		printf("passkey-host-test: FAIL (%u)\n", failures);
		return 1;
	}
	printf("passkey-host-test: PASS\n");
	return 0;
}
