/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the phone's record (ws197-p003, plan/ws197/phase003/
 * phase.md section 3.1), built with the host's compiler under ASan and
 * UBSan.
 *
 *   text      a record's text written by hand read back; the text the
 *             writer makes, compared byte for byte with a hand-written one;
 *             each malformed text refused (a key missing or twice, a value
 *             out of range, an unknown version, a bad name); an unknown key
 *             passed over
 *   names     the account names a record keeps
 *   files     written, read, listed beside a bond and a HID record (the
 *             bonds' listing does not count it), validity (no bond, a Just
 *             Works key, no account, another name), pruned without its
 *             bond, forgotten
 *
 *   plan/ws197/tests/bt-phone-host-test.sh FOLDER
 */

#include "userland/base/bluetoothd/keys.h"
#include "userland/base/bluetoothd/phonerec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The owner's uid, and the link key types: authenticated P-256, Just Works P-256. */
#define TEST_UID		1001U
#define TEST_KEY_MITM		0x08U
#define TEST_KEY_JUST_WORKS	0x07U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* The run's folder of bonds and records. */
static char folder[512];

/* The controller's address, and the phone's. */
static const uint8_t controller[BTD_ADDRESS_BYTES] = { 0x10U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U };
static const uint8_t phone[BTD_ADDRESS_BYTES] = { 0x51U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U };

static void check(int condition, const char *what);
static int hook_account(void *context, uid_t uid, char *name, size_t size);
static int parses(const char *text);
static void write_bond(uint8_t key_type);
static void write_file(const char *name, const char *text);
static void test_text(void);
static void test_names(void);
static void test_migrate(void);
static void test_files(void);

/*
 * Runs every part in a new folder under the one given and reports the
 * checks.
 */
int
main(
	int argc,
	char **argv)
{
	char *made;

	/* The folder for the bonds and records. */
	if (argc < 2) {
		fprintf(stderr, "usage: bt-phonerec-host-test FOLDER\n");
		return 2;
	}

	/* A new folder under it. */
	(void)snprintf(folder, sizeof(folder), "%s/phonerec.XXXXXX", argv[1]);
	made = mkdtemp(folder);
	if (made == NULL) {
		perror("mkdtemp");
		return 2;
	}

	/* Each part. */
	test_text();
	test_names();
	test_migrate();
	test_files();

	/* The count of what failed. */
	printf("bt-phonerec-host-test: %u checks, %u failed\n", checks, failures);
	if (failures != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts a check and reports the one that failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted. */
	checks++;
	if (condition)
		return;

	/* Reported. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* The account hook: TEST_UID is "alice", any other uid has no account. */
static int
hook_account(
	void *context,
	uid_t uid,
	char *name,
	size_t size)
{
	UNUSED_PARAMETER(context);

	/* TEST_UID's. */
	if (uid == TEST_UID) {
		(void)snprintf(name, size, "%s", "alice");
		return 0;
	}

	/* No such account. */
	return ENOENT;
}

/* Tells whether a text parses as a record. */
static int
parses(
	const char *text)
{
	struct btd_phonerec record;
	int error;

	/* Parsed. */
	error = btd_phonerec_parse(text, strlen(text), &record);
	if (error != 0)
		return 0;

	/* Succeeded. */
	return 1;
}

/* Writes the phone's bond with a key of a type. */
static void
write_bond(
	uint8_t key_type)
{
	struct btd_bond bond;
	int error;

	/* The bond. */
	memset(&bond, 0, sizeof(bond));
	memcpy(bond.address, phone, BTD_ADDRESS_BYTES);
	bond.type = BTD_ADDRESS_BREDR;
	bond.have_link_key = 1;
	bond.link_key_type = key_type;
	bond.key_size = 16U;

	/* Written, or the test cannot run. */
	error = btd_keys_write(folder, controller, &bond);
	if (error != 0) {
		fprintf(stderr, "btd_keys_write: %s\n", strerror(error));
		exit(2);
	}
}

/* Writes a file of a name in the controller's folder. */
static void
write_file(
	const char *name,
	const char *text)
{
	char path[640];
	ssize_t written;
	size_t length;
	int descriptor;

	/* The file. */
	(void)snprintf(path, sizeof(path), "%s/66:55:44:33:22:10/%s", folder, name);
	descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (descriptor < 0) {
		perror(path);
		exit(2);
	}

	/* Its text. */
	length = strlen(text);
	written = write(descriptor, text, length);
	(void)close(descriptor);
	if (written != (ssize_t)length) {
		perror(path);
		exit(2);
	}
}

/* A record's text, read and written, and the malformed ones. */
static void
test_text(void)
{
	static const char expected[] =
		"version 1\nuid 1001\nuser alice\nmessages 1\ncontacts 0\ncalls 1\nenabled 0\nasked m,h\n";
	struct btd_phonerec record;
	char text[BTD_PHONEREC_TEXT_MAX];
	int same;
	int error;

	/* A text written by hand. */
	error = btd_phonerec_parse(expected, sizeof(expected) - 1U, &record);
	check(error == 0, "text: read");
	check(record.uid == TEST_UID && strcmp(record.user, "alice") == 0, "text: the owner");
	check(record.profiles == (BTD_PHONEREC_MESSAGES | BTD_PHONEREC_CALLS) && record.enabled == 0, "text: the profiles and the switch");
	check(record.have_asked && record.asked == (BTD_PHONEREC_MESSAGES | BTD_PHONEREC_CALLS), "text: the profiles asked for");

	/* The writer's text, byte for byte. */
	error = btd_phonerec_format(&record, text, sizeof(text));
	same = strcmp(text, expected);
	check(error == 0 && same == 0, "text: written as by hand");

	/* An unknown key and empty lines are passed over. */
	check(parses("version 1\n\nuid 7\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\ncolour blue\n"), "text: an unknown key passed over");

	/* Each malformed text. */
	check(!parses("version 1\nuid 7\nuser bob\nmessages 1\ncontacts 1\ncalls 1\n"), "text: a key missing");
	check(!parses("version 1\nuid 7\nuid 8\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\n"), "text: a key twice");
	check(!parses("version 2\nuid 7\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\n"), "text: another version");
	check(!parses("version 1\nuid 2147483648\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\n"), "text: a uid too large");
	check(!parses("version 1\nuid -1\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\n"), "text: a uid not a number");
	check(!parses("version 1\nuid 7\nuser b/b\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\n"), "text: a name with a slash");
	check(!parses("version 1\nuid 7\nuser bob\nmessages 2\ncontacts 1\ncalls 1\nenabled 1\n"), "text: a switch of 2");
	check(!parses("version 1\nuid 7\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled\n"), "text: a line without its value");

	/* The "asked" line (ws197-p005 section 8.1): optional, and its malformed values. */
	check(parses("version 1\nuid 7\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\n"), "text: a record without asked (before ws197-p005)");
	check(parses("version 1\nuid 7\nuser bob\nmessages 0\ncontacts 0\ncalls 0\nenabled 1\nasked -\n"), "text: asked none");
	check(!parses("version 1\nuid 7\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\nasked m,m\n"), "text: asked a letter twice");
	check(!parses("version 1\nuid 7\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\nasked m,x\n"), "text: asked a letter not known");
	check(!parses("version 1\nuid 7\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\nasked mc\n"), "text: asked without its comma");
	check(!parses("version 1\nuid 7\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\nasked m,\n"), "text: asked ending in a comma");
	check(!parses("version 1\nuid 7\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\nasked \n"), "text: asked empty");
	check(!parses("version 1\nuid 7\nuser bob\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\nasked m\nasked m\n"), "text: asked twice");
}

/*
 * A record before ws197-p005 brought up to date: contacts and calls off,
 * messages kept, the "asked" line written; a record up to date unchanged.
 */
static void
test_migrate(void)
{
	static const char old_all[] =
		"version 1\nuid 1001\nuser alice\nmessages 1\ncontacts 1\ncalls 1\nenabled 1\n";
	static const char old_none[] =
		"version 1\nuid 1001\nuser alice\nmessages 0\ncontacts 1\ncalls 0\nenabled 1\n";
	static const char migrated[] =
		"version 1\nuid 1001\nuser alice\nmessages 1\ncontacts 0\ncalls 0\nenabled 1\nasked m\n";
	struct btd_phonerec record;
	char text[BTD_PHONEREC_TEXT_MAX];
	int changed;
	int error;
	int same;

	/* m,c,h from the pairing of p003: messages alone stays. */
	error = btd_phonerec_parse(old_all, sizeof(old_all) - 1U, &record);
	check(error == 0 && !record.have_asked, "migrate: an old record reads");
	changed = btd_phonerec_migrate(&record);
	check(changed == 1 && record.profiles == BTD_PHONEREC_MESSAGES, "migrate: contacts and calls off");
	error = btd_phonerec_format(&record, text, sizeof(text));
	same = strcmp(text, migrated);
	check(error == 0 && same == 0, "migrate: written with asked m");

	/* Up to date: unchanged. */
	error = btd_phonerec_parse(migrated, sizeof(migrated) - 1U, &record);
	changed = btd_phonerec_migrate(&record);
	check(error == 0 && changed == 0 && record.profiles == BTD_PHONEREC_MESSAGES, "migrate: an up to date record unchanged");

	/* Without messages: nothing stays on, asked "-". */
	error = btd_phonerec_parse(old_none, sizeof(old_none) - 1U, &record);
	changed = btd_phonerec_migrate(&record);
	check(error == 0 && changed == 1 && record.profiles == 0U && record.asked == 0U, "migrate: contacts alone goes off");
	error = btd_phonerec_format(&record, text, sizeof(text));
	check(error == 0 && strstr(text, "asked -\n") != NULL, "migrate: written with asked -");
}

/* The account names a record keeps. */
static void
test_names(void)
{
	char long_name[80];

	/* Letters, digits, dot, underscore, dash. */
	check(btd_phonerec_user_ok("alice"), "names: letters");
	check(btd_phonerec_user_ok("a.b_c-9"), "names: the other characters");

	/* Empty, a space, a slash, and too long. */
	check(!btd_phonerec_user_ok(""), "names: empty");
	check(!btd_phonerec_user_ok("a b"), "names: a space");
	check(!btd_phonerec_user_ok("../x"), "names: a slash");
	memset(long_name, 'a', 64U);
	long_name[64] = '\0';
	check(!btd_phonerec_user_ok(long_name), "names: 64 characters");
	long_name[63] = '\0';
	check(btd_phonerec_user_ok(long_name), "names: 63 characters");
}

/* The files: written, read, listed, valid or not, pruned, forgotten. */
static void
test_files(void)
{
	struct btd_phonerec records[BTD_PHONEREC_LIST_MAX];
	struct btd_bond bonds[4];
	struct btd_phonerec record;
	struct btd_phonerec read_back;
	unsigned count;
	int valid;
	int error;

	/* A record written and read back. */
	memset(&record, 0, sizeof(record));
	memcpy(record.address, phone, BTD_ADDRESS_BYTES);
	record.uid = TEST_UID;
	(void)snprintf(record.user, sizeof(record.user), "%s", "alice");
	record.profiles = BTD_PHONEREC_PROFILES;
	record.enabled = 1;
	error = btd_phonerec_write(folder, controller, &record);
	check(error == 0, "files: written");
	error = btd_phonerec_read(folder, controller, phone, &read_back);
	check(error == 0 && read_back.uid == TEST_UID && memcmp(read_back.address, phone, BTD_ADDRESS_BYTES) == 0, "files: read back");

	/* Valid only with its bond, an authenticated key, the account and its name. */
	valid = btd_phonerec_valid(folder, controller, &record, hook_account, NULL);
	check(!valid, "files: no bond, invalid");
	write_bond(TEST_KEY_JUST_WORKS);
	valid = btd_phonerec_valid(folder, controller, &record, hook_account, NULL);
	check(!valid, "files: a Just Works key, invalid");
	write_bond(TEST_KEY_MITM);
	valid = btd_phonerec_valid(folder, controller, &record, hook_account, NULL);
	check(valid, "files: valid");
	(void)snprintf(record.user, sizeof(record.user), "%s", "mallory");
	valid = btd_phonerec_valid(folder, controller, &record, hook_account, NULL);
	check(!valid, "files: another name, invalid");
	record.uid = 4242U;
	valid = btd_phonerec_valid(folder, controller, &record, hook_account, NULL);
	check(!valid, "files: no account, invalid");

	/* Listed beside a HID record and a malformed record: the record alone; the bonds' listing does not count it. */
	write_file("51:22:33:44:55:66-bredr.hid", "state=candidate\n");
	write_file("52:22:33:44:55:66-bredr.phone", "version 1\n");
	error = btd_phonerec_list(folder, controller, records, BTD_PHONEREC_LIST_MAX, &count);
	check(error == 0 && count == 1U && memcmp(records[0].address, phone, BTD_ADDRESS_BYTES) == 0, "files: listed, the malformed passed over");
	error = btd_keys_list(folder, controller, bonds, 4U, &count);
	check(error == 0 && count == 1U, "files: the bonds' listing counts the bond alone");

	/* Pruned: the malformed record has no bond and goes; the phone's stays. */
	error = btd_phonerec_prune(folder, controller);
	check(error == 0, "files: pruned");
	error = btd_phonerec_read(folder, controller, phone, &read_back);
	check(error == 0, "files: the record with its bond stays");
	record.address[0] = 0x52U;
	error = btd_phonerec_read(folder, controller, record.address, &read_back);
	check(error == ENOENT, "files: the record without its bond goes");

	/* Forgotten, then none. */
	error = btd_phonerec_forget(folder, controller, phone);
	check(error == 0, "files: forgotten");
	error = btd_phonerec_forget(folder, controller, phone);
	check(error == ENOENT, "files: forgotten again: none");
}
