/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws170-p002: the host test of Phone's store (userland/desktop/phone/
 * store.c) in a folder: contacts and items written and read back after
 * the store is opened again (one file each), the timelines oldest first
 * and the contacts with the latest first, a number found by its digits,
 * a state changed in its file, unread messages marked read, a Japanese
 * name's initials, and a vCard written by another program read.
 *
 *     host-phone-store FOLDER
 *
 * Prints "PASS name" or "FAIL name ..." for each check; exits with 1 when
 * one failed.
 */

#include "userland/desktop/phone/phone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The checks that failed. */
static int test_failures;

int main(int argc, char **argv);
static void test_check(const char *name, int passed, const char *detail);
static int test_file_has(const char *path, const char *text);

/*
 * Writes, reads back and checks.
 */
int
main(
	int argc,
	char **argv)
{
	const struct ph_contact *contacts;
	char path[1024];
	FILE *file;
	size_t count;
	size_t item;
	long ben;
	long mother;
	long found;
	int error;

	/* The folder. */
	if (argc != 2) {
		fprintf(stderr, "usage: host-phone-store FOLDER\n");
		return 2;
	}
	error = ph_store_open(argv[1]);
	test_check("open-empty", error == 0, "");
	contacts = ph_contacts(&count);
	test_check("empty", count == 0U, "");

	/* Two contacts and their items. */
	error = ph_store_add_contact("Ben Carter", "+1 555 0100", &ben);
	test_check("add-ben", error == 0 && ben == 0, "");
	error = ph_store_add_contact("\xe3\x81\x8a\xe6\xaf\x8d \xe3\x81\x95\xe3\x82\x93", "090-1234-5678", &mother);
	test_check("add-mother", error == 0 && mother == 1, "");
	error = ph_store_add_item(ben, PH_TEXT, PH_SMS, 0, (time_t)1790000300, PH_STATE_UNREAD, "Running late", NULL, &item);
	error |= ph_store_add_item(ben, PH_TEXT, PH_SMS, 1, (time_t)1790000400, PH_STATE_SENDING, "No problem", NULL, &item);
	test_check("add-items", error == 0 && item == 1U, "");
	error = ph_store_set_state(ben, item, PH_STATE_DELIVERED, NULL);
	contacts = ph_contacts(&count);
	test_check("state", error == 0 && strcmp(contacts[ben].items[item].detail, "Delivered") == 0 &&
	    test_file_has(contacts[ben].items[item].path, "State: delivered\n"), contacts[ben].items[item].detail);
	error = ph_store_add_item(mother, PH_CALL, PH_LINE, 0, (time_t)1790000100, PH_STATE_MISSED, NULL, NULL, &item);
	error |= ph_store_add_item(mother, PH_TEXT, PH_RCS, 0, (time_t)1790000000, PH_STATE_UNREAD, "\xe9\x87\x8e\xe8\x8f\x9c\nline two", NULL, &item);
	test_check("add-mother-items", error == 0, "");
	test_check("initials", strcmp(contacts[mother].initials, "\xe3\x81\x8a\xe3\x81\x95") == 0, contacts[mother].initials);

	/* A vCard another program wrote. */
	(void)snprintf(path, sizeof(path), "%s/contacts/other.vcf", argv[1]);
	file = fopen(path, "w");
	if (file != NULL) {
		fputs("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Aiko Tanaka\r\nTEL;TYPE=cell:+81 80 1111 2222\r\nEND:VCARD\r\n", file);
		fclose(file);
	}

	/* Read again from the disk. */
	ph_store_close();
	error = ph_store_open(argv[1]);
	contacts = ph_contacts(&count);
	test_check("reopen", error == 0 && count == 3U, "");
	test_check("latest-first", count == 3U && strcmp(contacts[0].name, "Ben Carter") == 0 && strcmp(contacts[2].name, "Aiko Tanaka") == 0, contacts[0].name);
	found = ph_store_find_number("+15550100");
	test_check("find-number", found == 0, "");
	found = ph_store_find_number("+81 80-1111-2222");
	test_check("find-other", found == 2, "");

	/* The mother's timeline oldest first, its words with their two lines, the counts of unread. */
	mother = ph_store_find_number("09012345678");
	test_check("timeline-order", mother >= 0 && contacts[mother].item_count == 2U &&
	    contacts[mother].items[0].kind == PH_TEXT &&
	    strcmp(contacts[mother].items[0].text, "\xe9\x87\x8e\xe8\x8f\x9c\nline two") == 0 &&
	    contacts[mother].items[1].kind == PH_CALL &&
	    contacts[mother].items[1].state == PH_STATE_MISSED &&
	    contacts[mother].items[1].detail == NULL, "");
	test_check("unread", contacts[0].unread == 1U && contacts[mother].unread == 1U, "");
	test_check("delivered-kept", strcmp(contacts[0].items[1].detail, "Delivered") == 0 && contacts[0].items[1].outgoing, "");

	/* Read: the files say so, and again after a reopen. */
	error = ph_store_mark_read(0);
	test_check("mark-read", error == 0 && contacts[0].unread == 0U && contacts[0].items[0].state == PH_STATE_READ, "");
	ph_store_close();
	(void)ph_store_open(argv[1]);
	contacts = ph_contacts(&count);
	test_check("read-kept", contacts[0].unread == 0U, "");
	ph_store_close();

	/* The outcome. */
	if (test_failures != 0) {
		printf("host-phone-store: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-phone-store: PASS\n");
	return 0;
}

/*
 * Supplies the store's diagnostics without retaining fixture message bodies.
 */
void
ph_log(
	const char *format,
	...)
{
	(void)format;
}

/* Prints a check's outcome. */
static void
test_check(
	const char *name,
	int passed,
	const char *detail)
{
	/* Passed. */
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}

	/* Failed, with what was seen. */
	printf("FAIL %s [%s]\n", name, detail != NULL ? detail : "");
	test_failures++;
}

/* Tells whether a file holds a text. */
static int
test_file_has(
	const char *path,
	const char *text)
{
	char bytes[4096];
	size_t length;
	FILE *file;

	/* The file's bytes. */
	file = fopen(path, "r");
	if (file == NULL)
		return 0;
	length = fread(bytes, 1U, sizeof(bytes) - 1U, file);
	fclose(file);
	bytes[length] = '\0';

	/* The text in them. */
	return strstr(bytes, text) != NULL;
}
