/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exercises imported vCard persistence, name precedence, pruning and bounded timelines. */

#include "userland/desktop/phone/phone.h"

#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ADDRESS "AA:BB:CC:DD:EE:01"

/* The failure counter belongs to this single host test process. */
static unsigned failures;

static void check(const char *name, int okay);
static int card(unsigned key, const char *name, const char *numbers);
static void names_and_prune(const char *root);
static void calls_and_overflow(const char *root);
static void mms_store(const char *root);
static void full_book(const char *root);

/*
 * Runs independent on-disk scenarios against the production store.
 */
int
main(
	int argc,
	char **argv)
{
	char root[2048];

	/* Uses new directories rather than altering or deleting existing fixtures. */
	if (argc != 2)
		return 2;
	(void)mkdir(argv[1], 0700);
	(void)snprintf(root, sizeof(root), "%s/names", argv[1]);
	names_and_prune(root);
	(void)snprintf(root, sizeof(root), "%s/calls", argv[1]);
	calls_and_overflow(root);
	(void)snprintf(root, sizeof(root), "%s/full", argv[1]);
	full_book(root);
	(void)snprintf(root, sizeof(root), "%s/mms", argv[1]);
	mms_store(root);
	ph_store_close();

	/* A failed assertion makes this a failing test, not merely a log entry. */
	if (failures != 0U)
		return 1;

	/* Succeeded: persistence and recovery scenarios passed. */
	puts("phone-pbap-store-host-test: PASS");
	return 0;
}

/*
 * Supplies the store's diagnostic hook without retaining any message bodies.
 */
void
ph_log(
	const char *format,
	...)
{
	(void)format;
}

/* Records one independent expectation and continues collecting failures. */
static void
check(
	const char *name,
	int okay)
{
	/* Makes each failed expectation visible to the runner. */
	if (!okay) {
		fprintf(stderr, "FAIL %s\n", name);
		failures++;
	}
}

/* Sends an independently constructed reduced vCard through the import API. */
static int
card(
	unsigned key,
	const char *name,
	const char *numbers)
{
	struct kl_phone_item item;
	char text[1024];
	int error;

	/* The reducer's wire format uses unfolded escaped vCard 3.0 properties. */
	memset(&item, 0, sizeof(item));
	(void)snprintf(item.key, sizeof(item.key), "%016x", key);
	(void)snprintf(text, sizeof(text), "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:%s\r\n%sEND:VCARD\r\n", name, numbers);
	item.text = text;
	item.length = strlen(text);
	error = ph_phonebook_put(ADDRESS, &item);
	if (error != 0)
		return error;

	/* Succeeded: the production store owns a parsed copy. */
	return 0;
}

/* Verifies isolated storage, per-number precedence, modification times and two-pass deletion. */
static void
names_and_prune(
	const char *root)
{
	const struct ph_contact *contacts;
	struct kl_phone_link link;
	struct stat before;
	struct stat after;
	char path[2048];
	const char *name;
	size_t count;
	long conversation;
	long local;
	int error;

	/* Starts with a number's conversation and a different editable local contact. */
	error = ph_store_open(root);
	check("open names", error == 0);
	conversation = ph_store_conversation("09012345678", NULL, 1);
	check("number conversation", conversation >= 0);
	error = ph_store_add_contact("Local", "08012345678", &local);
	check("local contact", error == 0);
	error = ph_phonebook_begin();
	check("begin", error == 0);
	error = card(1U, "Imported\\, Name", "TEL:09012345678\r\nTEL:08012345678\r\n");
	check("import", error == 0);
	error = card(2U, "Second", "TEL:07012345678\r\n");
	check("second import", error == 0);
	error = ph_phonebook_end(ADDRESS, 1, 0U);
	check("complete import", error == 0);
	contacts = ph_contacts(&count);
	check("no phonebook rows", count == 2U);
	check("name on conversation", contacts[conversation].phone_named && strcmp(contacts[conversation].name, "Imported, Name") == 0);
	name = ph_store_phone_name("08012345678");
	check("per-number local precedence", name == NULL);
	name = ph_store_phone_name("09012345678");
	check("other number still imported", name != NULL && strcmp(name, "Imported, Name") == 0);

	/* Reimporting identical content preserves an independently changed mtime. */
	(void)snprintf(path, sizeof(path), "%s/phonebook/bt-AABBCCDDEE01/0000000000000001.vcf", root);
	error = stat(path, &before);
	check("card separate from contacts", error == 0);
	error = ph_phonebook_begin();
	check("repeat begin", error == 0);
	error = card(1U, "Imported\\, Name", "TEL:09012345678\r\nTEL:08012345678\r\n");
	check("repeat card", error == 0);
	error = ph_phonebook_end(ADDRESS, 1, 0U);
	check("first absent pass", error == 0);
	error = stat(path, &after);
	check("same card mtime", error == 0 && before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec);
	(void)snprintf(path, sizeof(path), "%s/phonebook/bt-AABBCCDDEE01/0000000000000002.vcf", root);
	error = access(path, F_OK);
	check("first miss retained", error == 0);

	/* A capped pass must not become the second deletion pass. */
	error = ph_phonebook_begin();
	check("capped begin", error == 0);
	error = card(1U, "Imported\\, Name", "TEL:09012345678\r\nTEL:08012345678\r\n");
	check("capped card", error == 0);
	error = ph_phonebook_end(ADDRESS, 1, 4U);
	check("capped end", error == 0);
	error = access(path, F_OK);
	check("capped retains", error == 0);

	/* A second credible pass removes the previously absent card. */
	error = ph_phonebook_begin();
	check("second miss begin", error == 0);
	error = card(1U, "Imported\\, Name", "TEL:09012345678\r\nTEL:08012345678\r\n");
	check("second miss card", error == 0);
	error = ph_phonebook_end(ADDRESS, 1, 0U);
	check("second miss end", error == 0);
	error = access(path, F_OK);
	check("twice missing removed", error != 0 && errno == ENOENT);
	ph_store_close();
	error = ph_store_open(root);
	check("reopen names", error == 0);
	conversation = ph_store_find_number("09012345678");
	contacts = ph_contacts(&count);
	check("reopen imported name", conversation >= 0 && contacts[conversation].phone_named);
	ph_store_set_country("1");
	name = ph_store_phone_name("+19012345678");
	check("country rebuild", name != NULL);
	ph_store_set_country(PH_COUNTRY_DEFAULT);

	/* Unknown record states retain the copy; a confirmed stop forgets it. */
	memset(&link, 0, sizeof(link));
	error = ph_phonebook_link(&link);
	check("unknown link", error == 0);
	name = ph_store_phone_name("09012345678");
	check("unknown keeps names", name != NULL);
	link.record = 2U;
	link.owner = 1U;
	(void)snprintf(link.address, sizeof(link.address), "%s", ADDRESS);
	error = ph_phonebook_link(&link);
	check("stop deletes phonebook", error == 0);
	name = ph_store_phone_name("09012345678");
	check("stop removes lookup", name == NULL);
	contacts = ph_contacts(&count);
	check("stop preserves rows", count == 2U);
	ph_store_close();
}

/* Verifies deduplicated call files, withheld recovery and overflow preservation on reopen. */
static void
calls_and_overflow(
	const char *root)
{
	struct kl_phone_item call;
	struct ph_phone_message message;
	const struct ph_contact *contacts;
	struct stat before;
	struct stat after;
	char path[2048];
	char number[32];
	size_t count;
	size_t item;
	long contact;
	unsigned i;
	int merge;
	int error;

	/* Imports each kind into the existing timeline representation. */
	error = ph_store_open(root);
	check("open calls", error == 0);
	memset(&call, 0, sizeof(call));
	(void)snprintf(call.key, sizeof(call.key), "%016x", 10U);
	(void)snprintf(call.peer, sizeof(call.peer), "09012345678");
	call.time = 1790000000;
	error = ph_store_phone_call(ADDRESS, &call);
	check("received call", error == 0);
	contact = ph_store_find_number(call.peer);
	contacts = ph_contacts(&count);
	check("received answered", contact >= 0 && contacts[contact].items[0].state == PH_STATE_ANSWERED);
	(void)snprintf(path, sizeof(path), "%s", contacts[contact].items[0].path);
	error = stat(path, &before);
	check("call file", error == 0);
	error = ph_store_phone_call(ADDRESS, &call);
	check("duplicate call", error == 0);
	error = stat(path, &after);
	check("duplicate call mtime", error == 0 && before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec);
	contacts = ph_contacts(&count);
	check("duplicate call count", contacts[contact].item_count == 1U);
	(void)snprintf(call.key, sizeof(call.key), "%016x", 11U);
	call.folder = 1U;
	error = ph_store_phone_call(ADDRESS, &call);
	check("dialled call", error == 0);
	(void)snprintf(call.key, sizeof(call.key), "%016x", 12U);
	call.folder = 2U;
	call.peer[0] = '\0';
	call.zone = 3U;
	error = ph_store_phone_call(ADDRESS, &call);
	check("withheld missed call", error == 0);
	contact = ph_store_withheld_conversation(0);
	contacts = ph_contacts(&count);
	check("withheld separate", contact >= 0 && contacts[contact].items[0].partial && contacts[contact].items[0].state == PH_STATE_MISSED);

	/* Fills ordinary rows, then imports both a call and SMS beyond their bound. */
	for (i = 0U; i < 1021U; i++) {
		(void)snprintf(number, sizeof(number), "+8191%08u", i);
		contact = ph_store_conversation(number, NULL, 1);
		check("ordinary row", contact >= 0);
	}

	/* Retains a new peer when ordinary conversations have reached their bound. */
	(void)snprintf(call.key, sizeof(call.key), "%016x", 13U);
	(void)snprintf(call.peer, sizeof(call.peer), "+819999999999");
	error = ph_store_phone_call(ADDRESS, &call);
	check("overflow call", error == 0);
	memset(&message, 0, sizeof(message));
	message.address = ADDRESS;
	message.key = "000000000000000e";
	message.peer = "+818888888888";
	message.date = 1790000001;
	message.text = "overflow message";
	error = ph_store_phone_message(&message, &contact, &item, &merge);
	check("overflow SMS", error == 0);
	contacts = ph_contacts(&count);
	check("bounded rows", count == 1024U && strcmp(contacts[contact].id, "o") == 0);
	check("overflow peer retained", contacts[contact].items[item].extra != NULL && strstr(contacts[contact].items[item].extra, message.peer) != NULL);
	ph_store_close();

	/* Reopening an older store with excess folders still succeeds. */
	(void)snprintf(path, sizeof(path), "%s/messages/n819999999998", root);
	(void)mkdir(path, 0700);
	error = ph_store_open(root);
	check("reopen full store", error == 0);
	contacts = ph_contacts(&count);
	check("reopen bound", count <= 1024U);
	contact = ph_store_withheld_conversation(0);
	check("reopen withheld", contact >= 0);
	ph_store_close();
}

/* Measures a bounded 5000-card copy and rejects a card beyond that cap. */
static void
full_book(
	const char *root)
{
	struct timespec start;
	struct timespec end;
	const char *name;
	char number[64];
	unsigned i;
	int error;
	long milliseconds;

	/* Writes the maximum complete book with distinct keys and numbers. */
	error = ph_store_open(root);
	check("open full book", error == 0);
	error = ph_phonebook_begin();
	check("full begin", error == 0);
	for (i = 1U; i <= 5000U; i++) {
		(void)snprintf(number, sizeof(number), "TEL:+8190%08u\r\n", i);
		error = card(i, "Person", number);
		check("full card", error == 0);
	}

	/* Rejects the next card before creating an unloadable copy file. */
	error = card(5001U, "Excess", "TEL:+819999999999\r\n");
	check("book cap", error == ENOSPC);
	error = ph_phonebook_end(ADDRESS, 1, 0U);
	check("full end", error == 0);
	ph_store_close();

	/* Times complete reopen and verifies a number at the far end of the book. */
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	error = ph_store_open(root);
	(void)clock_gettime(CLOCK_MONOTONIC, &end);
	check("full reopen", error == 0);
	milliseconds = (end.tv_sec - start.tv_sec) * 1000L + (end.tv_nsec - start.tv_nsec) / 1000000L;
	printf("5000-card reopen: %ld ms\n", milliseconds);
	name = ph_store_phone_name("+819000005000");
	check("full lookup", name != NULL && strcmp(name, "Person") == 0);
	ph_store_close();
}

/* Verifies MMS persistence and its separation from matching SMS text. */
static void
mms_store(
	const char *root)
{
	struct ph_phone_message message;
	const struct ph_contact *contacts;
	size_t count;
	size_t item;
	long contact;
	int merge;
	int error;

	/* Imports a partial MMS with a known body and peer. */
	error = ph_store_open(root);
	check("MMS open", error == 0);
	memset(&message, 0, sizeof(message));
	message.address = ADDRESS;
	message.channel = PH_MMS;
	message.key = "-";
	message.peer = "+819055550001";
	message.date = 1790000001;
	message.text = "decoded text";
	error = ph_store_phone_message(&message, &contact, &item, &merge);
	check("MMS import", error == 0 && merge == PH_MERGE_NEW);

	/* Matching SMS words are a distinct message rather than an MMS candidate. */
	message.channel = PH_SMS;
	error = ph_store_phone_message(&message, &contact, &item, &merge);
	check("SMS distinct from MMS", error == 0 && merge == PH_MERGE_NEW);
	ph_store_close();
	error = ph_store_open(root);
	check("MMS reopen", error == 0);
	contacts = ph_contacts(&count);
	check("MMS channel persisted", count == 1U && contacts[0].item_count == 2U && contacts[0].items[0].channel == PH_MMS && contacts[0].items[1].channel == PH_SMS);
	ph_store_close();
}
