/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws197-p004b: the host test of Phone's store of the paired phone's
 * messages (userland/desktop/phone/store.c, plan/ws197/phase004/phase.md
 * sections 0, 2, 6.2 and 6.3), in a new folder.
 *
 * The number keys (090... and +8190... the same, 080... another, a
 * sender's name in lower case, a short number, 00, another country code);
 * a message kept once by its key, read when the phone read it; the
 * conversation of a number without a contact (messages/n<digits>) and of
 * a sender's name (messages/a<hex>); one's own text taking the phone's
 * key (CRLF and LF the same, within ten minutes, the nearest, one to one,
 * renamed s<key>.txt, sent, delivered staying, failed made sent); a
 * message without a key kept once and taking the key later; a header line
 * not known written back; after reopening, the keys found again (by the
 * source and by the file's name), the conversation named, the contact
 * made later reading the number's folder; the synchronisation's marks.
 *
 * Prints "PASS name" or "FAIL name" for each check; the last line is
 * "phone-store-host-test: PASS" or "... FAIL".
 */

#include "userland/desktop/phone/phone.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The phone's address in the tests. */
#define TEST_ADDRESS	"AA:BB:CC:DD:EE:01"

/* The checks that failed. */
static int test_failures;

int main(int argc, char **argv);
static void test_check(const char *name, int passed);
static int test_key_is(const char *number, const char *expected);
static int test_message(const char *key, int outgoing, time_t date, const char *peer, const char *name, int read, const char *text, long *contact, size_t *item);
static int test_file_has(const char *path, const char *line);
static int test_exists(const char *path);

/* The log of the store (phone.h's ph_log), not shown. */
void
ph_log(
	const char *format,
	...)
{
	/* Nothing. */
	(void)format;
}

/*
 * Drives the store in the folder given and checks what it kept.
 */
int
main(
	int argc,
	char **argv)
{
	const struct ph_contact *contacts;
	char path[2048];
	char line[256];
	const char *root;
	size_t count;
	size_t item;
	size_t own;
	size_t other_own;
	size_t first_count;
	long contact;
	long mother;
	long amazon;
	long found_contact;
	int64_t since;
	int64_t deep_at;
	int error;
	FILE *file;

	/* The folder. */
	if (argc != 2) {
		fprintf(stderr, "usage: phone-store-host-test FOLDER\n");
		return 2;
	}

	/* The store, empty. */
	root = argv[1];
	error = ph_store_open(root);
	test_check("open", error == 0);

	/* The number keys. */
	test_check("key-090", test_key_is("090-1234-5678", "+819012345678"));
	test_check("key-plus", test_key_is("+81 90 1234 5678", "+819012345678"));
	test_check("key-080", test_key_is("080-1234-5678", "+818012345678"));
	test_check("key-00", test_key_is("00819012345678", "+819012345678"));
	test_check("key-name", test_key_is("  Amazon ", "amazon"));
	test_check("key-short", test_key_is("12345", "12345"));
	test_check("key-nanp", test_key_is("(555) 010-0100", "+815550100100"));
	ph_store_set_country("1");
	test_check("key-country", test_key_is("(555) 010-0100", "+15550100100"));
	ph_store_set_country("x1");
	test_check("key-country-bad", test_key_is("5550100100", "+15550100100"));
	ph_store_set_country(PH_COUNTRY_DEFAULT);

	/* A message that came: a conversation of its number, s<key>.txt with its source; again by its key: known. */
	error = test_message("00000000000000a1", 0, 1790000000, "090-1234-5678", "Mother", 0, "Dinner at seven?", &contact, &item);
	contacts = ph_contacts(&count);
	mother = contact;
	(void)snprintf(path, sizeof(path), "%s/messages/n819012345678/s00000000000000a1.txt", root);
	test_check("new", error == PH_MERGE_NEW && count == 1U && contacts[0].conversation && strcmp(contacts[0].id, "n819012345678") == 0 &&
	    strcmp(contacts[0].name, "Mother") == 0 && contacts[0].unread == 1U && test_exists(path));
	test_check("new-source", test_file_has(path, "Source: bt:" TEST_ADDRESS ":map:00000000000000a1") && test_file_has(path, "State: unread") &&
	    test_file_has(path, "Name: Mother"));
	error = test_message("00000000000000a1", 0, 1790000000, "+819012345678", "Mother", 0, "Dinner at seven?", &contact, &item);
	contacts = ph_contacts(&count);
	test_check("known", error == PH_MERGE_KNOWN && count == 1U && contacts[0].item_count == 1U);

	/* Read on the phone: read here. */
	error = test_message("00000000000000a1", 0, 1790000000, "+819012345678", "Mother", 1, "Dinner at seven?", &contact, &item);
	contacts = ph_contacts(&count);
	test_check("read", error == PH_MERGE_KNOWN && contacts[0].items[0].state == PH_STATE_READ && contacts[0].unread == 0U && test_file_has(path, "State: read"));

	/* Another number: another conversation. */
	error = test_message("00000000000000a2", 0, 1790000100, "080-1234-5678", "", 0, "Hi", &contact, &item);
	contacts = ph_contacts(&count);
	test_check("other-number", error == PH_MERGE_NEW && count == 2U && contact == 1 && strcmp(contacts[1].id, "n818012345678") == 0);

	/* A sender's name: a folder of its bytes. */
	error = test_message("00000000000000a3", 0, 1790000200, "Amazon", "", 0, "Your parcel", &contact, &item);
	contacts = ph_contacts(&count);
	amazon = contact;
	test_check("sender-name", error == PH_MERGE_NEW && strcmp(contacts[contact].id, "a616d617a6f6e") == 0);

	/* One's own text takes the phone's key: CRLF and LF the same, two minutes apart, sent, renamed. */
	error = ph_store_add_item(mother, PH_TEXT, PH_SMS, 1, (time_t)1790001000, PH_STATE_SENDING, "Yes\nsee you", NULL, &own);
	contacts = ph_contacts(&count);
	test_check("own-added", error == 0 && contacts[mother].items[own].source == NULL);
	error = test_message("00000000000000b1", 1, 1790001120, "+819012345678", "", 1, "Yes\r\nsee you", &contact, &item);
	contacts = ph_contacts(&count);
	(void)snprintf(path, sizeof(path), "%s/messages/n819012345678/s00000000000000b1.txt", root);
	test_check("overlay", error == PH_MERGE_OVERLAID && contacts[mother].item_count == 2U && contacts[mother].items[item].state == PH_STATE_SENT &&
	    contacts[mother].items[item].date == 1790001120 && test_exists(path) && test_file_has(path, "Source: bt:" TEST_ADDRESS ":map:00000000000000b1"));

	/* Delivered stays, failed becomes sent. */
	error = ph_store_add_item(mother, PH_TEXT, PH_SMS, 1, (time_t)1790002000, PH_STATE_DELIVERED, "One", NULL, &own);
	error |= ph_store_add_item(mother, PH_TEXT, PH_SMS, 1, (time_t)1790002000, PH_STATE_FAILED, "Two", NULL, &own);
	error |= test_message("00000000000000b2", 1, 1790002010, "+819012345678", "", 1, "One", &contact, &item) != PH_MERGE_OVERLAID;
	contacts = ph_contacts(&count);
	test_check("delivered-stays", error == 0 && contacts[mother].items[item].state == PH_STATE_DELIVERED);
	error = test_message("00000000000000b3", 1, 1790002010, "+819012345678", "", 1, "Two", &contact, &item);
	contacts = ph_contacts(&count);
	test_check("failed-sent", error == PH_MERGE_OVERLAID && contacts[mother].items[item].state == PH_STATE_SENT);

	/* Eleven minutes apart: not the same. */
	error = ph_store_add_item(mother, PH_TEXT, PH_SMS, 1, (time_t)1790003000, PH_STATE_SENDING, "Far", NULL, &own);
	error |= test_message("00000000000000b4", 1, 1790003660, "+819012345678", "", 1, "Far", &contact, &item) != PH_MERGE_NEW;
	contacts = ph_contacts(&count);
	test_check("too-far", error == 0 && contacts[mother].item_count == 6U);

	/* One to one, the nearest: two own texts of the same words, two copies. */
	first_count = contacts[mother].item_count;
	error = ph_store_add_item(mother, PH_TEXT, PH_SMS, 1, (time_t)1790004000, PH_STATE_SENDING, "ok", NULL, &own);
	error |= ph_store_add_item(mother, PH_TEXT, PH_SMS, 1, (time_t)1790004100, PH_STATE_UNKNOWN, "ok", NULL, &other_own);
	error |= test_message("00000000000000c1", 1, 1790004090, "+819012345678", "", 1, "ok", &contact, &item) != PH_MERGE_OVERLAID;
	contacts = ph_contacts(&count);
	test_check("nearest", error == 0 && contacts[mother].items[item].date == 1790004090 && contacts[mother].item_count == first_count + 2U);
	error = test_message("00000000000000c2", 1, 1790004010, "+819012345678", "", 1, "ok", &contact, &item);
	contacts = ph_contacts(&count);
	test_check("one-to-one", error == PH_MERGE_OVERLAID && contacts[mother].item_count == first_count + 2U &&
	    contacts[mother].items[item].source != NULL && strstr(contacts[mother].items[item].source, "c2") != NULL);

	/* A message without a key: kept once, then it takes the key. */
	first_count = contacts[amazon].item_count;
	error = test_message("-", 0, 1790005000, "Amazon", "", 0, "Out for delivery", &contact, &item);
	contacts = ph_contacts(&count);
	test_check("partial", error == PH_MERGE_NEW && contacts[amazon].item_count == first_count + 1U && contacts[amazon].items[item].partial &&
	    test_file_has(contacts[amazon].items[item].path, "Partial: yes"));
	error = test_message("-", 0, 1790005030, "Amazon", "", 0, "Out for delivery", &contact, &item);
	contacts = ph_contacts(&count);
	test_check("partial-again", error == PH_MERGE_KNOWN && contacts[amazon].item_count == first_count + 1U);
	error = test_message("00000000000000d1", 0, 1790005060, "Amazon", "", 0, "Out for delivery", &contact, &item);
	contacts = ph_contacts(&count);
	test_check("partial-keyed", error == PH_MERGE_OVERLAID && contacts[amazon].item_count == first_count + 1U && !contacts[amazon].items[item].partial);
	error = test_message("-", 0, 1790000250, "Amazon", "", 0, "Your parcel", &contact, &item);
	test_check("partial-after-key", error == PH_MERGE_KNOWN);

	/* A key that names no file is refused. */
	error = test_message("../x", 0, 1790005000, "Amazon", "", 0, "x", &contact, &item);
	test_check("bad-key", error == -EINVAL);

	/* A header line of a later program kept when the file is written again. */
	(void)snprintf(path, sizeof(path), "%s/messages/n818012345678/s00000000000000a2.txt", root);
	file = fopen(path, "w");
	if (file != NULL) {
		fputs("Kind: text\nChannel: sms\nDirection: in\nDate: 1790000100\nState: unread\nSource: bt:" TEST_ADDRESS ":map:00000000000000a2\n"
		    "X-Later: kept\n\nHi\n", file);
		fclose(file);
	}

	/* A file of a key whose source a program dropped. */
	(void)snprintf(path, sizeof(path), "%s/messages/n818012345678/s00000000000000a4.txt", root);
	file = fopen(path, "w");
	if (file != NULL) {
		fputs("Kind: text\nChannel: sms\nDirection: in\nDate: 1790000150\nState: read\n\nHello\n", file);
		fclose(file);
	}

	/* Read again from the disk. */
	ph_store_close();
	error = ph_store_open(root);
	contacts = ph_contacts(&count);
	found_contact = ph_store_find_number("090 1234 5678");
	test_check("reopen", error == 0 && count == 3U && found_contact >= 0 && contacts[found_contact].conversation &&
	    strcmp(contacts[found_contact].name, "Mother") == 0);
	error = test_message("00000000000000b1", 1, 1790001120, "+819012345678", "", 1, "Yes\r\nsee you", &contact, &item);
	test_check("reopen-known", error == PH_MERGE_KNOWN);
	error = test_message("00000000000000a4", 0, 1790000150, "080-1234-5678", "", 1, "Hello", &contact, &item);
	test_check("by-file-name", error == PH_MERGE_KNOWN);
	error = test_message("00000000000000a2", 0, 1790000100, "080-1234-5678", "", 1, "Hi", &contact, &item);
	(void)snprintf(path, sizeof(path), "%s/messages/n818012345678/s00000000000000a2.txt", root);
	test_check("later-line", error == PH_MERGE_KNOWN && test_file_has(path, "X-Later: kept") && test_file_has(path, "State: read"));

	/* A contact made later for the number: the conversation becomes it, and its folder is read as its after reopening. */
	first_count = contacts[found_contact].item_count;
	error = ph_store_add_contact("Mom", "090-1234-5678", &contact);
	contacts = ph_contacts(&count);
	test_check("contact-later", error == 0 && contact == found_contact && count == 3U && !contacts[contact].conversation &&
	    strcmp(contacts[contact].name, "Mom") == 0);
	error = test_message("00000000000000e1", 0, 1790006000, "+819012345678", "", 0, "New one", &contact, &item);
	contacts = ph_contacts(&count);
	test_check("contact-later-new", error == PH_MERGE_NEW && contact == found_contact && strstr(contacts[contact].items[item].path, "/messages/c") != NULL);
	ph_store_close();
	error = ph_store_open(root);
	contacts = ph_contacts(&count);
	found_contact = ph_store_find_number("+819012345678");
	test_check("contact-reopen", error == 0 && count == 3U && found_contact >= 0 && !contacts[found_contact].conversation &&
	    contacts[found_contact].item_count == first_count + 1U);

	/* The item found by its serial. */
	item = contacts[found_contact].item_count - 1U;
	error = ph_store_find_serial(contacts[found_contact].items[item].serial, &contact, &own);
	test_check("serial", error == 0 && contact == found_contact && own == item && ph_store_find_serial(999999UL, &contact, &own) == ENOENT);

	/* The synchronisation's marks. */
	error = ph_store_sync_load(TEST_ADDRESS, &since, &deep_at);
	test_check("sync-none", error == ENOENT);
	error = ph_store_sync_save(TEST_ADDRESS, 1790000000, 1790100000);
	error |= ph_store_sync_load(TEST_ADDRESS, &since, &deep_at);
	test_check("sync-kept", error == 0 && since == 1790000000 && deep_at == 1790100000);
	test_check("sync-bad", ph_store_sync_save("../../x", 1, 1) == EINVAL);
	(void)snprintf(path, sizeof(path), "%s/sync/bt-" TEST_ADDRESS ".state", root);
	(void)snprintf(line, sizeof(line), "messages_since %d", 1790000000);
	test_check("sync-file", test_file_has(path, line));
	ph_store_close();

	/* The outcome. */
	if (test_failures != 0) {
		printf("phone-store-host-test: FAIL (%d)\n", test_failures);
		return 1;
	}

	/* Succeeded. */
	printf("phone-store-host-test: PASS\n");
	return 0;
}

/* Prints a check's outcome. */
static void
test_check(
	const char *name,
	int passed)
{
	/* Passed. */
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}

	/* Failed. */
	printf("FAIL %s\n", name);
	test_failures++;
}

/* Tells whether a number's key is the one expected. */
static int
test_key_is(
	const char *number,
	const char *expected)
{
	char key[PH_NUMBER_KEY_MAX];
	int error;
	int same;

	/* The key. */
	error = ph_number_key(number, key, sizeof(key));
	if (error != 0)
		return 0;
	same = strcmp(key, expected);
	if (same != 0) {
		printf("  key %s\n", key);
		return 0;
	}

	/* Succeeded: the same. */
	return 1;
}

/* Keeps a message of the phone: its merge (PH_MERGE_*), or the negative errno value. */
static int
test_message(
	const char *key,
	int outgoing,
	time_t date,
	const char *peer,
	const char *name,
	int read,
	const char *text,
	long *contact,
	size_t *item)
{
	struct ph_phone_message message;
	int merge;
	int error;

	/* The message. */
	memset(&message, 0, sizeof(message));
	message.address = TEST_ADDRESS;
	message.key = key;
	message.outgoing = outgoing;
	message.date = date;
	message.peer = peer;
	message.name = name;
	message.read = read;
	message.text = text;

	/* Kept. */
	merge = -1;
	error = ph_store_phone_message(&message, contact, item, &merge);
	if (error != 0)
		return -error;

	/* Succeeded: what was done. */
	return merge;
}

/* Tells whether a file has a line. */
static int
test_file_has(
	const char *path,
	const char *line)
{
	char read_line[1024];
	char *got;
	FILE *file;
	int same;

	/* The file. */
	file = fopen(path, "r");
	if (file == NULL)
		return 0;

	/* Each of its lines. */
	for (;;) {
		got = fgets(read_line, (int)sizeof(read_line), file);
		if (got == NULL)
			break;
		read_line[strcspn(read_line, "\n")] = '\0';
		same = strcmp(read_line, line);
		if (same == 0) {
			fclose(file);
			return 1;
		}
	}

	/* Not there. */
	fclose(file);
	return 0;
}

/* Tells whether a file is there. */
static int
test_exists(
	const char *path)
{
	struct stat status;
	int failed;

	/* Its status. */
	failed = stat(path, &status);
	if (failed != 0)
		return 0;

	/* Succeeded: there. */
	return 1;
}
