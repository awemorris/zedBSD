/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The production receiver imports an MMS through the real compositor media fixture. */
#include "userland/desktop/libmms/mms.h"
#include "userland/desktop/phone/phone.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void phone_media_regression(struct kl_system *system, const char *root);

/*
 * Omits message contents from the fixture log.
 */
void
ph_log(
	const char *format,
	...)
{
	(void)format;
}

/*
 * Verifies original bytes, deduplication and message paths after reopening.
 */
void
phone_media_regression(
	struct kl_system *system,
	const char *root)
{
	static const uint8_t png[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x08, 0x06, 0x00, 0x00, 0x00, 0xb9, 0xea, 0xde, 0x81, 0x00, 0x00, 0x00, 0x16, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x14, 0x09, 0xe8, 0xf9, 0xcf, 0xc0, 0xc0, 0xc0, 0xc0, 0xc4, 0x00, 0x05, 0x70, 0x06, 0x00, 0x2e, 0x4d, 0x01, 0xf5, 0xc4, 0x99, 0x1f, 0x4a, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
	struct kl_image decoded;
	struct ph_received received;
	struct ph_received repeated;
	struct ph_phone_message message;
	const struct ph_contact *contacts;
	FILE *mime;
	FILE *saved;
	char store[2048];
	uint8_t bytes[sizeof(png)];
	size_t count;
	size_t item;
	size_t got;
	long contact;
	int merged;
	int error;
	int same;

	/* This image-like binary has embedded NULs, and no name suffix. */
	mime = tmpfile();
	assert(mime != NULL);
	error = mms_write(mime, "image/png", "phone-picture", png, sizeof(png), "日本語の写真");
	assert(error == 0);
	error = fflush(mime);
	assert(error == 0);
	error = ph_receive_media(system, fileno(mime), &received);
	assert(error == 0 && received.count == 1U && received.video[0] == 0U);
	saved = fopen(received.paths[0], "rb");
	assert(saved != NULL);
	got = fread(bytes, 1U, sizeof(bytes), saved);
	assert(got == sizeof(bytes));
	same = memcmp(bytes, png, sizeof(png));
	assert(same == 0);
	fclose(saved);

	/* The exact saved original is accepted by the production photo decoder. */
	error = ph_decode(received.paths[0], &decoded);
	assert(error == 0 && decoded.width == 2 && decoded.height == 3 && decoded.pixels != NULL);
	kl_image_release(&decoded);

	/* Another import resolves the same canonical original rather than a guessed date/path. */
	error = ph_receive_media(system, fileno(mime), &repeated);
	assert(error == 0 && repeated.count == 1U);
	same = strcmp(received.paths[0], repeated.paths[0]);
	assert(same == 0);
	ph_received_release(&repeated);
	fclose(mime);

	/* One message retains the caption and attachment across close/reopen and repeated sync. */
	(void)snprintf(store, sizeof(store), "%s/Phone", root);
	error = ph_store_open(store);
	assert(error == 0);
	memset(&message, 0, sizeof(message));
	message.channel = PH_MMS;
	message.address = "AA:BB:CC:DD:EE:01";
	message.key = "0123456789abcdef";
	message.peer = "+15550100";
	message.name = "Fixture";
	message.text = "日本語の写真";
	message.date = 1700000000;
	error = ph_store_phone_message(&message, &contact, &item, &merged);
	assert(error == 0 && merged == PH_MERGE_NEW);
	error = ph_store_media(contact, item, &received);
	assert(error == 0);
	ph_store_close();
	error = ph_store_open(store);
	assert(error == 0);
	error = ph_store_phone_message(&message, &contact, &item, &merged);
	assert(error == 0 && merged == PH_MERGE_KNOWN);
	contacts = ph_contacts(&count);
	assert(contact >= 0 && (size_t)contact < count);
	assert(contacts[contact].item_count == 1U && contacts[contact].items[item].media_count == 1U);
	same = strcmp(contacts[contact].items[item].media[0], received.paths[0]);
	assert(same == 0);
	same = strcmp(contacts[contact].items[item].text, message.text);
	assert(same == 0);
	ph_store_close();
	ph_received_release(&received);
	puts("host-media-receive: PASS (binary, canonical paths, dedup, message reopen)");
}
