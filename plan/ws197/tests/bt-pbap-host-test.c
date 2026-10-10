/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the PBAP client (ws197-p005, plan/ws197/phase005/
 * phase.md section 5), built with the host's compiler under ASan and
 * UBSan.  pbap.c runs against hooks: the test plays the phone's PSE,
 * reads each OBEX request pbap.c writes on the DLC, checks its headers
 * and application parameters against constants written by hand from
 * PBAP 1.2.3 (sections 5.1, 6.2.1, 6.4 and table 5.1; never pbap.c's
 * macros), and answers with OBEX packets written by hand.
 *
 *   setup      SDP for the PSE, the DLC to its channel, Connect with the
 *              Target and no application parameters, its 60 s; ready
 *   refusals   Connect Forbidden, the phone's DM, DISC, RFCOMM giving up,
 *              another close before ready, no answer to Connect; no PSE,
 *              a PSE without the local phone book; a busy channel
 *   contacts   SIZE's bytes, PhonebookSize from the answer, PULL's bytes,
 *              the owner's card not written, the item's line and vCard,
 *              PAGE-END's cursor, the end by fewer cards, the size asked
 *              again (changed), a body in two packets, a card that does
 *              not end, a phone book of a whole page (the probe answered
 *              Not Found, or empty), a card too large for the body
 *   calls      the three objects in turn, the time and zone, a call
 *              before since not written, a call without a datetime, an
 *              object without a size taken as empty
 *   cursors    stale after another phone, malformed, past the history
 *   idle       the phone's close while idle, the next page connects
 *              again; a close under an operation fails; the page's
 *              wait for the connection
 *   clients    a slow client, a client that went, busy, not ready
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/pbap.h"
#include "userland/base/bluetoothd/rfcomm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Marks a hook's parameter the test does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The phone's PSE channel and the DLC to it, and the first generation of cursors. */
#define TEST_CHANNEL		7U
#define TEST_DLCI		14U
#define TEST_GENERATION		0x22220000U

/* The rooms of the bytes written on the DLC and of the answers. */
#define TEST_OUT_MAX		65536U
#define TEST_TEXT_MAX		65536U
#define TEST_CLOSED_MAX		8U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/*
 * The world around pbap.c: the clocks and zone, whether contacts are
 * wanted, the phone's address, what the hooks were asked, the bytes
 * written on the DLC (not read yet), the answers as text, the client's
 * room, how often the state changed and PBAP came up.
 */
struct world {
	uint64_t now;
	int64_t wall;
	int32_t offset;
	int wanted;
	uint8_t address[6];
	unsigned sdp_queries;
	int sdp_error;
	unsigned dlc_opens;
	unsigned dlc_channel;
	int open_error;
	unsigned closed_count;
	unsigned closed[TEST_CLOSED_MAX];
	size_t out_length;
	uint8_t out[TEST_OUT_MAX];
	size_t answers_length;
	char answers[TEST_TEXT_MAX];
	long room;
	unsigned changed;
	unsigned up;
};

/* The world, and the client under test (reset by start). */
static struct world world;
static struct btd_pbap pbap;

/* PBAP's Target (PBAP section 6.4). */
static const uint8_t target_uuid[16] = {
	0x79U, 0x61U, 0x35U, 0xf0U, 0xf0U, 0xc5U, 0x11U, 0xd8U, 0x09U, 0x66U, 0x08U, 0x00U, 0x20U, 0x0cU, 0x9aU, 0x66U
};

/* The owner's card and two contacts, as a phone gives them. */
static const char card_owner[] =
	"BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Me\r\nTEL:+81900000000\r\nEND:VCARD\r\n";
static const char card_alice[] =
	"BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Alice\r\nTEL;TYPE=CELL:+15551234\r\nUID:a1\r\nEND:VCARD\r\n";
static const char card_bob[] =
	"BEGIN:VCARD\r\nVERSION:2.1\r\nN:Doe;Bob\r\nTEL;HOME:+1 555 9999\r\nEND:VCARD\r\n";

/* Calls: one taken in 2024 (UTC), one in 2020, one without a datetime. */
static const char call_new[] =
	"BEGIN:VCARD\r\nVERSION:2.1\r\nFN:Bob\r\nTEL:+15559999\r\nX-IRMC-CALL-DATETIME;RECEIVED:20240101T120000Z\r\nEND:VCARD\r\n";
static const char call_old[] =
	"BEGIN:VCARD\r\nVERSION:2.1\r\nFN:Old\r\nTEL:+15550000\r\nX-IRMC-CALL-DATETIME;RECEIVED:20200101T000000Z\r\nEND:VCARD\r\n";
static const char call_bare[] =
	"BEGIN:VCARD\r\nVERSION:2.1\r\nTEL:\r\nEND:VCARD\r\n";

static void check(int condition, const char *what);
static uint64_t hook_clock(void *context);
static int64_t hook_wall(void *context);
static int32_t hook_offset(void *context, int64_t seconds);
static int hook_wanted(void *context);
static int hook_address(void *context, uint8_t *address);
static int hook_sdp(void *context, uint16_t uuid);
static int hook_open(void *context, unsigned server_channel);
static int hook_write(void *context, unsigned dlci, const uint8_t *data, size_t length, size_t *written);
static void hook_close(void *context, unsigned dlci);
static void hook_answer(void *context, uint64_t token, const char *line, const uint8_t *bytes, size_t length);
static long hook_room(void *context, uint64_t token);
static void hook_changed(void *context);
static void hook_up(void *context);
static void start(void);
static void sdp_answer(int pse, uint8_t repositories);
static size_t take(uint8_t *packet, size_t size);
static void respond(uint8_t code, const uint8_t *headers, size_t length);
static void respond_size(unsigned size);
static void respond_body(uint8_t code, const char *body);
static void respond_connect(void);
static int find_header(const uint8_t *packet, size_t length, uint8_t id, struct btd_obex_header *header);
static int find_parameter(const struct btd_obex_header *header, uint8_t tag, const uint8_t **value, size_t *size);
static int is_text(const struct btd_obex_header *header, const char *text);
static int has_answer(const char *text);
static int state_is(const char *text);
static int is_size_get(const char *object);
static int is_pull_get(const char *object, unsigned count, unsigned offset, int calls);
static void setup(void);
static void test_key(const char *text, char *key);
static void test_setup(void);
static void test_refusals(void);
static void test_contacts(void);
static void test_edges(void);
static void test_large(void);
static void test_calls(void);
static void test_cursors(void);
static void test_idle(void);
static void test_clients(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_setup();
	test_refusals();
	test_contacts();
	test_edges();
	test_large();
	test_calls();
	test_cursors();
	test_idle();
	test_clients();

	/* The count of what failed. */
	printf("bt-pbap-host-test: %u checks, %u failed\n", checks, failures);
	if (failures != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts a check, and reports it when it failed. */
static void
check(
	int condition,
	const char *what)
{
	/* One more check. */
	checks++;

	/* A failure is reported. */
	if (!condition) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

/* The monotonic milliseconds. */
static uint64_t
hook_clock(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* The world's. */
	return world.now;
}

/* The time of day in seconds. */
static int64_t
hook_wall(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* The world's. */
	return world.wall;
}

/* zedBSD's zone. */
static int32_t
hook_offset(
	void *context,
	int64_t seconds)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(seconds);

	/* The world's. */
	return world.offset;
}

/* Whether contacts are wanted. */
static int
hook_wanted(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* The world's. */
	return world.wanted;
}

/* The phone's address. */
static int
hook_address(
	void *context,
	uint8_t *address)
{
	UNUSED_PARAMETER(context);

	/* The world's. */
	memcpy(address, world.address, sizeof(world.address));
	return 0;
}

/* Counts an SDP query for the PSE. */
static int
hook_sdp(
	void *context,
	uint16_t uuid)
{
	UNUSED_PARAMETER(context);

	/* The PSE's class. */
	check(uuid == 0x112fU, "hooks: SDP for the PSE");
	world.sdp_queries++;
	return world.sdp_error;
}

/* Notes a DLC asked for. */
static int
hook_open(
	void *context,
	unsigned server_channel)
{
	UNUSED_PARAMETER(context);

	/* Noted, answered as the scenario asks. */
	world.dlc_opens++;
	world.dlc_channel = server_channel;
	return world.open_error;
}

/* Keeps what pbap.c writes on its DLC. */
static int
hook_write(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length,
	size_t *written)
{
	UNUSED_PARAMETER(context);

	/* The PSE's DLC only. */
	check(dlci == TEST_DLCI, "hooks: written on the PSE's DLC");
	if (length > sizeof(world.out) - world.out_length) {
		fprintf(stderr, "bt-pbap-host-test: the DLC's room ran out\n");
		exit(2);
	}

	/* Succeeded: all taken. */
	memcpy(world.out + world.out_length, data, length);
	world.out_length += length;
	*written = length;
	return 0;
}

/* Notes a DLC closed. */
static void
hook_close(
	void *context,
	unsigned dlci)
{
	UNUSED_PARAMETER(context);

	/* Noted. */
	if (world.closed_count < TEST_CLOSED_MAX)
		world.closed[world.closed_count] = dlci;
	world.closed_count++;
}

/* Keeps an answer: "token: line" and the bytes after it. */
static void
hook_answer(
	void *context,
	uint64_t token,
	const char *line,
	const uint8_t *bytes,
	size_t length)
{
	int written;

	UNUSED_PARAMETER(context);

	/* The line. */
	written = snprintf(world.answers + world.answers_length, sizeof(world.answers) - world.answers_length, "%llu: %s\n", (unsigned long long)token, line);
	if (written > 0)
		world.answers_length += (size_t)written;

	/* The bytes. */
	if (length != 0U && world.answers_length + length < sizeof(world.answers)) {
		memcpy(world.answers + world.answers_length, bytes, length);
		world.answers_length += length;
		world.answers[world.answers_length] = '\0';
	}
}

/* The client's room. */
static long
hook_room(
	void *context,
	uint64_t token)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(token);

	/* The world's. */
	return world.room;
}

/* Counts a change of the state. */
static void
hook_changed(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* Counted. */
	world.changed++;
}

/* Counts PBAP coming up. */
static void
hook_up(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* Counted. */
	world.up++;
}

/* A new world and a new client: contacts wanted, plenty of room, the clocks at a set time. */
static void
start(void)
{
	struct btd_pbap_hooks hooks;

	/* The world. */
	memset(&world, 0, sizeof(world));
	world.now = 1000000U;
	world.wall = 1700003600;
	world.offset = 32400;
	world.wanted = 1;
	world.room = 1000000;
	world.address[5] = 0x01U;

	/* The hooks. */
	memset(&hooks, 0, sizeof(hooks));
	hooks.clock = hook_clock;
	hooks.wall = hook_wall;
	hooks.local_offset = hook_offset;
	hooks.wanted = hook_wanted;
	hooks.address = hook_address;
	hooks.sdp_query = hook_sdp;
	hooks.dlc_open = hook_open;
	hooks.dlc_write = hook_write;
	hooks.dlc_close = hook_close;
	hooks.answer = hook_answer;
	hooks.room = hook_room;
	hooks.changed = hook_changed;
	hooks.up = hook_up;
	btd_pbap_init(&pbap, &hooks, TEST_GENERATION);
}

/*
 * Answers the SDP query with one record written by hand (Core Vol 3
 * Part B section 3): a PSE (0x112F, L2CAP, RFCOMM channel 7, OBEX, and
 * SupportedRepositories 0x0314) when pse, else a MAS (0x1132).
 */
static void
sdp_answer(
	int pse,
	uint8_t repositories)
{
	static struct btd_sdp sdp;
	uint8_t record[64];
	size_t used;

	/* 0x0001: a sequence of UUID16 0x112F (or 0x1132). */
	used = 0U;
	memcpy(record + used, "\x09\x00\x01\x35\x03\x19\x11\x2f", 8U);
	if (!pse)
		record[used + 7U] = 0x32U;
	used += 8U;

	/* 0x0004: (L2CAP), (RFCOMM, channel 7), (OBEX). */
	memcpy(record + used, "\x09\x00\x04\x35\x11\x35\x03\x19\x01\x00\x35\x05\x19\x00\x03\x08\x07\x35\x03\x19\x00\x08", 22U);
	used += 22U;

	/* 0x0314 SupportedRepositories, an 8-bit number. */
	memcpy(record + used, "\x09\x03\x14\x08", 4U);
	used += 4U;
	record[used] = repositories;
	used++;

	/* The lists: a sequence of the one record. */
	memset(&sdp, 0, sizeof(sdp));
	sdp.lists[0] = 0x35U;
	sdp.lists[1] = (uint8_t)(used + 2U);
	sdp.lists[2] = 0x35U;
	sdp.lists[3] = (uint8_t)used;
	memcpy(sdp.lists + 4, record, used);
	sdp.used = used + 4U;
	btd_pbap_sdp_done(&pbap, &sdp, 0);
}

/* Takes the oldest whole packet pbap.c wrote, or gives 0. */
static size_t
take(
	uint8_t *packet,
	size_t size)
{
	size_t length;

	/* A whole packet. */
	if (world.out_length < 3U)
		return 0U;
	length = ((size_t)world.out[1] << 8) | world.out[2];
	if (length > world.out_length || length > size)
		return 0U;

	/* Succeeded: out of the bytes written. */
	memcpy(packet, world.out, length);
	memmove(world.out, world.out + length, world.out_length - length);
	world.out_length -= length;
	return length;
}

/* Answers with a packet of a code and headers. */
static void
respond(
	uint8_t code,
	const uint8_t *headers,
	size_t length)
{
	static uint8_t packet[TEST_OUT_MAX];

	/* The packet. */
	packet[0] = code;
	packet[1] = (uint8_t)((length + 3U) >> 8);
	packet[2] = (uint8_t)((length + 3U) & 0xffU);
	if (length != 0U)
		memcpy(packet + 3, headers, length);

	/* Succeeded: to pbap.c. */
	btd_pbap_data(&pbap, TEST_DLCI, packet, length + 3U);
}

/* Answers a SIZE with Success and PhonebookSize (tag 0x08, two bytes). */
static void
respond_size(
	unsigned size)
{
	uint8_t headers[7];

	/* Application Parameters: 4c 00 07 08 02 hi lo. */
	headers[0] = 0x4cU;
	headers[1] = 0x00U;
	headers[2] = 0x07U;
	headers[3] = 0x08U;
	headers[4] = 0x02U;
	headers[5] = (uint8_t)(size >> 8);
	headers[6] = (uint8_t)(size & 0xffU);

	/* Succeeded: answered. */
	respond(0xa0U, headers, sizeof(headers));
}

/* Answers with a code and a body (Body for Continue, End of Body for Success). */
static void
respond_body(
	uint8_t code,
	const char *body)
{
	static uint8_t headers[TEST_OUT_MAX];
	size_t length;

	/* The body's header. */
	length = strlen(body);
	headers[0] = 0x49U;
	if (code == 0x90U)
		headers[0] = 0x48U;
	headers[1] = (uint8_t)((length + 3U) >> 8);
	headers[2] = (uint8_t)((length + 3U) & 0xffU);
	memcpy(headers + 3, body, length);

	/* Succeeded: answered. */
	respond(code, headers, length + 3U);
}

/* Answers Connect: Success, OBEX 1.0, 8192 bytes, Connection ID 1. */
static void
respond_connect(void)
{
	static const uint8_t answer[] = {
		0xa0U, 0x00U, 0x0cU, 0x10U, 0x00U, 0x20U, 0x00U, 0xcbU, 0x00U, 0x00U, 0x00U, 0x01U
	};

	/* Succeeded: to pbap.c. */
	btd_pbap_data(&pbap, TEST_DLCI, answer, sizeof(answer));
}

/* Finds a header of a request (after its three bytes). */
static int
find_header(
	const uint8_t *packet,
	size_t length,
	uint8_t id,
	struct btd_obex_header *header)
{
	int found;

	/* The headers after the prefix. */
	found = btd_obex_find(packet + 3, length - 3U, id, header);
	if (found > 0)
		return 1;

	/* Not there. */
	return 0;
}

/* Finds an application parameter by its tag. */
static int
find_parameter(
	const struct btd_obex_header *header,
	uint8_t tag,
	const uint8_t **value,
	size_t *size)
{
	size_t at;

	/* Each tag, length, value. */
	for (at = 0U; at + 2U <= header->length; at += 2U + header->data[at + 1U]) {
		/* The tag sought. */
		if (header->data[at] == tag) {
			*value = header->data + at + 2U;
			*size = header->data[at + 1U];
			return 1;
		}
	}

	/* Not there. */
	return 0;
}

/* Says whether a Unicode header holds a text (UTF-16BE, its NUL included). */
static int
is_text(
	const struct btd_obex_header *header,
	const char *text)
{
	size_t length;
	size_t index;

	/* Two bytes for each letter and the NUL. */
	length = strlen(text);
	if (header->length != 2U * length + 2U)
		return 0;

	/* Each letter. */
	for (index = 0U; index < length; index++) {
		/* A letter that differs. */
		if (header->data[2U * index] != 0U || header->data[2U * index + 1U] != (uint8_t)text[index])
			return 0;
	}

	/* The NUL. */
	if (header->data[2U * length] != 0U || header->data[2U * length + 1U] != 0U)
		return 0;

	/* The same text. */
	return 1;
}

/* Says whether the answers hold a text. */
static int
has_answer(
	const char *text)
{
	const char *found;

	/* Searched. */
	found = strstr(world.answers, text);
	if (found == NULL)
		return 0;

	/* Found. */
	return 1;
}

/* Says whether the state's text is a text. */
static int
state_is(
	const char *text)
{
	char state[96];
	int same;
	int error;

	/* The text. */
	error = btd_pbap_state_text(&pbap, state, sizeof(state));
	if (error != 0)
		return 0;

	/* The same, or not. */
	same = strcmp(state, text);
	if (same != 0)
		return 0;
	return 1;
}

/*
 * Takes the next request and says whether it is a SIZE of the object:
 * Get with Final (0x83), the Connection ID, Name, Type x-bt/phonebook
 * with its NUL, and the application parameters 04 02 00 00 alone.
 */
static int
is_size_get(
	const char *object)
{
	static const uint8_t parameters[] = { 0x04U, 0x02U, 0x00U, 0x00U };
	static const char type[] = "x-bt/phonebook";
	struct btd_obex_header header;
	uint8_t packet[1024];
	size_t length;
	int found;
	int same;

	/* The request. */
	length = take(packet, sizeof(packet));
	if (length < 3U || packet[0] != 0x83U)
		return 0;

	/* Its Connection ID, Name and Type. */
	found = find_header(packet, length, 0xcbU, &header);
	if (!found || header.value != 1U)
		return 0;
	found = find_header(packet, length, 0x01U, &header);
	if (!found)
		return 0;
	same = is_text(&header, object);
	if (!same)
		return 0;
	found = find_header(packet, length, 0x42U, &header);
	if (!found || header.length != sizeof(type))
		return 0;
	same = memcmp(header.data, type, sizeof(type));
	if (same != 0)
		return 0;

	/* Its application parameters, byte for byte. */
	found = find_header(packet, length, 0x4cU, &header);
	if (!found || header.length != sizeof(parameters))
		return 0;
	same = memcmp(header.data, parameters, sizeof(parameters));
	if (same != 0)
		return 0;

	/* A SIZE of the object. */
	return 1;
}

/*
 * Takes the next request and says whether it is a PULL of the object:
 * Get with Final, Name, and the application parameters: PropertySelector
 * (tag 06, 8 bytes: 00 00 00 00 00 20 00 87 for the phone book, 00 00 00
 * 00 10 00 00 87 for calls), Format 07 01 01, MaxListCount and
 * ListStartOffset.
 */
static int
is_pull_get(
	const char *object,
	unsigned count,
	unsigned offset,
	int calls)
{
	static const uint8_t contact_fields[8] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x20U, 0x00U, 0x87U };
	static const uint8_t call_fields[8] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x10U, 0x00U, 0x00U, 0x87U };
	struct btd_obex_header header;
	const uint8_t *value;
	uint8_t packet[1024];
	size_t length;
	size_t size;
	int found;
	int same;

	/* The request and its Name. */
	length = take(packet, sizeof(packet));
	if (length < 3U || packet[0] != 0x83U)
		return 0;
	found = find_header(packet, length, 0x01U, &header);
	if (!found)
		return 0;
	same = is_text(&header, object);
	if (!same)
		return 0;

	/* The application parameters: the fields. */
	found = find_header(packet, length, 0x4cU, &header);
	if (!found)
		return 0;
	found = find_parameter(&header, 0x06U, &value, &size);
	if (!found || size != 8U)
		return 0;
	same = memcmp(value, contact_fields, 8U);
	if (calls)
		same = memcmp(value, call_fields, 8U);
	if (same != 0)
		return 0;

	/* vCard 3.0. */
	found = find_parameter(&header, 0x07U, &value, &size);
	if (!found || size != 1U || value[0] != 0x01U)
		return 0;

	/* The count and the offset, the high byte first. */
	found = find_parameter(&header, 0x04U, &value, &size);
	if (!found || size != 2U || value[0] != (uint8_t)(count >> 8) || value[1] != (uint8_t)(count & 0xffU))
		return 0;
	found = find_parameter(&header, 0x05U, &value, &size);
	if (!found || size != 2U || value[0] != (uint8_t)(offset >> 8) || value[1] != (uint8_t)(offset & 0xffU))
		return 0;

	/* A PULL of the object. */
	return 1;
}

/* A client set up and ready on channel 7. */
static void
setup(void)
{
	uint8_t packet[64];

	/* The link ready, the PSE found, its DLC opened, Connect answered. */
	start();
	btd_pbap_ready(&pbap);
	sdp_answer(1, 0x03U);
	btd_pbap_opened(&pbap, TEST_DLCI, TEST_CHANNEL, 1);
	(void)take(packet, sizeof(packet));
	respond_connect();
	check(state_is("contacts=ready"), "setup: ready");
}

/* Writes the key of a string: its 64-bit FNV-1a as 16 hexadecimal digits (written here apart from vcard.c's). */
static void
test_key(
	const char *text,
	char *key)
{
	unsigned long long hash;
	size_t index;

	/* The basis, then each byte. */
	hash = 14695981039346656037ULL;
	for (index = 0U; text[index] != '\0'; index++) {
		hash ^= (unsigned char)text[index];
		hash *= 1099511628211ULL;
	}

	/* In hexadecimal. */
	(void)snprintf(key, 17U, "%016llx", hash);
}

/* Setting up: SDP, the DLC, Connect, ready. */
static void
test_setup(void)
{
	uint8_t connect[32];
	uint8_t packet[64];
	size_t length;
	int found;
	struct btd_obex_header header;

	/* Off, then the link ready: the SDP query, connecting. */
	start();
	check(state_is("contacts=off"), "setup: off");
	btd_pbap_ready(&pbap);
	check(world.sdp_queries == 1U && state_is("contacts=connecting"), "setup: SDP asked, connecting");

	/* The PSE record: its DLC to channel 7. */
	sdp_answer(1, 0x01U);
	check(world.dlc_opens == 1U && world.dlc_channel == TEST_CHANNEL, "setup: the DLC to channel 7");
	check(btd_pbap_accept(&pbap, 16U) == 0, "setup: no server channel offered");

	/* The DLC: Connect, OBEX 1.0, 8192 bytes, PBAP's Target, no application parameters; 60 s for the answer. */
	btd_pbap_opened(&pbap, TEST_DLCI, TEST_CHANNEL, 1);
	memcpy(connect, "\x80\x00\x1a\x10\x00\x20\x00\x46\x00\x13", 10U);
	memcpy(connect + 10, target_uuid, 16U);
	length = take(packet, sizeof(packet));
	check(length == 26U && memcmp(packet, connect, 26U) == 0, "setup: Connect's bytes");
	found = btd_obex_find(packet + 7, length - 7U, 0x4cU, &header);
	check(found <= 0, "setup: Connect without application parameters");
	check(btd_pbap_deadline(&pbap) == world.now + 60000U, "setup: 60 s for Connect's answer");

	/* Ready: up told, the usual 10 s for each answer from now on. */
	respond_connect();
	check(state_is("contacts=ready") && world.up == 1U, "setup: ready and up");
	check(btd_pbap_deadline(&pbap) == 0U, "setup: nothing awaited while idle");
}

/* The refusals and failures of setting up. */
static void
test_refusals(void)
{
	static const uint8_t forbidden[] = { 0xc3U, 0x00U, 0x07U, 0x10U, 0x00U, 0x20U, 0x00U };
	uint8_t packet[64];

	/* Connect Forbidden: permission, ten minutes; a check tries at once. */
	start();
	btd_pbap_ready(&pbap);
	sdp_answer(1, 0x01U);
	btd_pbap_opened(&pbap, TEST_DLCI, TEST_CHANNEL, 1);
	(void)take(packet, sizeof(packet));
	btd_pbap_data(&pbap, TEST_DLCI, forbidden, sizeof(forbidden));
	check(state_is("contacts=failed contacts_why=permission"), "refusals: Forbidden is permission");
	btd_pbap_tick(&pbap, world.now);
	check(world.closed_count == 1U && world.closed[0] == TEST_DLCI, "refusals: the DLC closed");
	check(btd_pbap_deadline(&pbap) == world.now + 600000U, "refusals: again in 600 s");
	btd_pbap_check(&pbap);
	btd_pbap_tick(&pbap, world.now);
	check(world.sdp_queries == 2U, "refusals: a check tries at once");

	/* The phone's DM before the DLC opened: permission, ten minutes. */
	start();
	btd_pbap_ready(&pbap);
	sdp_answer(1, 0x01U);
	btd_pbap_closed(&pbap, TEST_DLCI, BTD_RFCOMM_CLOSED_REFUSED);
	check(state_is("contacts=failed contacts_why=permission"), "refusals: the phone's DM is permission");
	check(btd_pbap_deadline(&pbap) == world.now + 600000U, "refusals: the DM's wait 600 s");

	/* The phone's DISC while Connect waits: permission. */
	start();
	btd_pbap_ready(&pbap);
	sdp_answer(1, 0x01U);
	btd_pbap_opened(&pbap, TEST_DLCI, TEST_CHANNEL, 1);
	btd_pbap_closed(&pbap, TEST_DLCI, BTD_RFCOMM_CLOSED_REMOTE);
	check(state_is("contacts=failed contacts_why=permission"), "refusals: the phone's DISC is permission");

	/* RFCOMM giving up (an unanswered prompt): timeout, ten minutes. */
	start();
	btd_pbap_ready(&pbap);
	sdp_answer(1, 0x01U);
	btd_pbap_closed(&pbap, TEST_DLCI, BTD_RFCOMM_CLOSED_TIMEOUT);
	check(state_is("contacts=failed contacts_why=timeout"), "refusals: RFCOMM giving up is timeout");
	check(btd_pbap_deadline(&pbap) == world.now + 600000U, "refusals: the timeout's wait 600 s");

	/* Another close before ready: closed, 30 s. */
	start();
	btd_pbap_ready(&pbap);
	sdp_answer(1, 0x01U);
	btd_pbap_closed(&pbap, TEST_DLCI, BTD_RFCOMM_CLOSED_LOST);
	check(state_is("contacts=failed contacts_why=closed"), "refusals: a lost DLC is closed");
	check(btd_pbap_deadline(&pbap) == world.now + 30000U, "refusals: again in 30 s");

	/* No answer to Connect in 60 s: timeout, ten minutes. */
	start();
	btd_pbap_ready(&pbap);
	sdp_answer(1, 0x01U);
	btd_pbap_opened(&pbap, TEST_DLCI, TEST_CHANNEL, 1);
	world.now += 60000U;
	btd_pbap_tick(&pbap, world.now);
	check(state_is("contacts=failed contacts_why=timeout"), "refusals: Connect unanswered");
	btd_pbap_tick(&pbap, world.now);
	check(btd_pbap_deadline(&pbap) == world.now + 600000U, "refusals: Connect's timeout waits 600 s");

	/* No PSE, and a PSE without the local phone book: not tried again on this link. */
	start();
	btd_pbap_ready(&pbap);
	sdp_answer(0, 0x01U);
	check(state_is("contacts=failed contacts_why=no-pse") && btd_pbap_deadline(&pbap) == 0U, "refusals: no PSE");
	start();
	btd_pbap_ready(&pbap);
	sdp_answer(1, 0x02U);
	check(state_is("contacts=failed contacts_why=no-pb") && btd_pbap_deadline(&pbap) == 0U, "refusals: no local phone book");

	/* A busy channel: asked again 2 s later, not failed. */
	start();
	world.open_error = EBUSY;
	btd_pbap_ready(&pbap);
	sdp_answer(1, 0x01U);
	check(state_is("contacts=connecting") && world.dlc_opens == 1U, "refusals: a busy channel waits");
	world.open_error = 0;
	world.now += 2000U;
	btd_pbap_tick(&pbap, world.now);
	check(world.dlc_opens == 2U, "refusals: the channel asked again after 2 s");

	/* Not wanted: nothing starts; turned off by a check. */
	start();
	world.wanted = 0;
	btd_pbap_ready(&pbap);
	check(world.sdp_queries == 0U && state_is("contacts=off"), "refusals: not wanted, not started");
}

/* Pages of the phone book. */
static void
test_contacts(void)
{
	char body[1024];
	char key[17];
	char text[256];
	const char *error;

	/* The first page: SIZE, then PULL of two cards from the start. */
	setup();
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "", 2U);
	check(error == NULL, "contacts: started");
	check(is_size_get("telecom/pb.vcf"), "contacts: SIZE's bytes");
	respond_size(3U);
	check(is_pull_get("telecom/pb.vcf", 2U, 0U, 0), "contacts: PULL's bytes");

	/* The owner's card and Alice: Alice alone written, with her vCard. */
	(void)snprintf(body, sizeof(body), "%s%s", card_owner, card_alice);
	respond_body(0xa0U, body);
	test_key("u|a1", key);
	(void)snprintf(text, sizeof(text), "7: PHONE CONTACT key=%s tels=1 length=", key);
	check(has_answer(text), "contacts: Alice's line");
	check(has_answer("peer=\"+15551234\" name=\"Alice\"\nBEGIN:VCARD\r\nVERSION:3.0\r\nFN:Alice\r\nTEL;TYPE=CELL:+15551234\r\nUID:a1\r\nEND:VCARD\r\n"), "contacts: her number, name and reduced vCard");
	check(!has_answer("name=\"Me\""), "contacts: the owner's card not written");
	check(has_answer("7: PHONE PAGE-END cursor=22220000.0.2.3 more=1 count=1 skipped=0 capped=0\n7: DONE\n"), "contacts: the first page's end");

	/* The second page from the cursor: one card, fewer than asked: the end, the size asked again. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.2.3", 2U);
	check(error == NULL, "contacts: the second page started");
	check(is_pull_get("telecom/pb.vcf", 2U, 2U, 0), "contacts: PULL from offset 2");
	respond_body(0xa0U, card_bob);
	check(has_answer("name=\"Bob Doe\""), "contacts: Bob's name from N");
	check(is_size_get("telecom/pb.vcf"), "contacts: the size asked again at the end");
	respond_size(3U);
	check(has_answer("7: PHONE PAGE-END cursor=22220000.0.3.3 more=0 count=1 skipped=0 capped=0\n7: DONE\n"), "contacts: the pass's end");

	/* The size changed during the pass: capped 2. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.2.3", 2U);
	(void)is_pull_get("telecom/pb.vcf", 2U, 2U, 0);
	respond_body(0xa0U, card_bob);
	(void)is_size_get("telecom/pb.vcf");
	respond_size(4U);
	check(has_answer("more=0 count=1 skipped=0 capped=2"), "contacts: a size that changed is capped 2");

	/* A body in two packets (Continue, then Success). */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	(void)is_pull_get("telecom/pb.vcf", 2U, 1U, 0);
	respond_body(0x90U, card_alice);
	check(world.out_length > 0U && world.out[0] == 0x83U, "contacts: the Get goes on after Continue");
	world.out_length = 0U;
	respond_body(0xa0U, card_bob);
	check(has_answer("name=\"Alice\"") && has_answer("name=\"Bob Doe\"") && has_answer("more=1 count=2"), "contacts: both packets' cards");

	/* A card that does not end: the cards before it written, capped 4, the offset by the BEGINs. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.9", 3U);
	(void)is_pull_get("telecom/pb.vcf", 3U, 1U, 0);
	(void)snprintf(body, sizeof(body), "%sBEGIN:VCARD\r\nFN:Cut\r\n%s", card_alice, card_bob);
	respond_body(0xa0U, body);
	check(has_answer("name=\"Alice\"") && !has_answer("name=\"Cut\""), "contacts: the cards before the cut one");
	check(has_answer("cursor=22220000.0.3.9 more=1 count=1 skipped=0 capped=4"), "contacts: capped 4, the offset by the cards begun");

	/* A card that cannot be read: skipped. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.9", 2U);
	(void)is_pull_get("telecom/pb.vcf", 2U, 1U, 0);
	(void)snprintf(body, sizeof(body), "BEGIN:VCARD\r\nNOTE:x\r\nEND:VCARD\r\n%s", card_alice);
	respond_body(0xa0U, body);
	check(has_answer("more=1 count=1 skipped=1 capped=0"), "contacts: a card without a name or number skipped");
}

/* The phone book's end at a whole page: the probe at the size. */
static void
test_edges(void)
{
	static const uint8_t not_found[] = { 0xc4U, 0x00U, 0x03U };
	char body[1024];

	/* Three cards (owner and two), pages of three: the first page full. */
	setup();
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "", 3U);
	(void)is_size_get("telecom/pb.vcf");
	respond_size(3U);
	(void)is_pull_get("telecom/pb.vcf", 3U, 0U, 0);
	(void)snprintf(body, sizeof(body), "%s%s%s", card_owner, card_alice, card_bob);
	respond_body(0xa0U, body);
	check(has_answer("cursor=22220000.0.3.3 more=1 count=2"), "edges: a whole page, more");

	/* The next page at the size: the probe answered Not Found is the end, not an error. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.3.3", 3U);
	check(is_pull_get("telecom/pb.vcf", 3U, 3U, 0), "edges: the probe at the size");
	btd_pbap_data(&pbap, TEST_DLCI, not_found, sizeof(not_found));
	check(is_size_get("telecom/pb.vcf"), "edges: the size asked again");
	respond_size(3U);
	check(has_answer("cursor=22220000.0.3.3 more=0 count=0 skipped=0 capped=0"), "edges: Not Found at the size ends the pass");

	/* The probe answered with an empty body: the end too. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.3.3", 3U);
	(void)is_pull_get("telecom/pb.vcf", 3U, 3U, 0);
	respond_body(0xa0U, "");
	(void)is_size_get("telecom/pb.vcf");
	respond_size(3U);
	check(has_answer("more=0 count=0 skipped=0 capped=0"), "edges: an empty body at the size ends the pass");

	/* A phone whose size leaves out the owner's card: the probe finds the last contact. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.2.2", 2U);
	(void)is_pull_get("telecom/pb.vcf", 2U, 2U, 0);
	respond_body(0xa0U, card_bob);
	check(has_answer("name=\"Bob Doe\""), "edges: the last contact past the size written");
	(void)is_size_get("telecom/pb.vcf");
	respond_size(2U);

	/* A failure before the size refuses the page. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	(void)is_pull_get("telecom/pb.vcf", 2U, 1U, 0);
	btd_pbap_data(&pbap, TEST_DLCI, not_found, sizeof(not_found));
	check(has_answer("7: ERROR refused\n7: DONE\n") && state_is("contacts=ready"), "edges: Not Found before the size refuses the page");
}

/* A body past 256 KB: half the cards asked again, one card too large left out. */
static void
test_large(void)
{
	static char chunk[8001];
	static const uint8_t success[] = { 0xa0U, 0x00U, 0x03U };
	uint8_t packet[1024];
	const uint8_t *value;
	struct btd_obex_header header;
	size_t length;
	size_t size;
	unsigned round;
	int found;

	/* Two cards asked, the body past 256 KB in Continues: Abort, then one card asked. */
	setup();
	memset(chunk, 'x', sizeof(chunk) - 1U);
	chunk[sizeof(chunk) - 1U] = '\0';
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.9", 2U);
	(void)is_pull_get("telecom/pb.vcf", 2U, 1U, 0);
	for (round = 0U; round < 33U; round++) {
		/* Another 8000 bytes; the Get's next packet taken. */
		respond_body(0x90U, chunk);
		length = take(packet, sizeof(packet));
		if (length == 0U)
			break;
	}

	/* The last packet the Abort; its answer, then one card asked. */
	check(packet[0] == 0xffU, "large: Abort past 256 KB");
	btd_pbap_data(&pbap, TEST_DLCI, success, sizeof(success));
	length = take(packet, sizeof(packet));
	found = find_header(packet, length, 0x4cU, &header);
	if (found)
		found = find_parameter(&header, 0x04U, &value, &size);
	check(found && size == 2U && value[0] == 0U && value[1] == 1U, "large: one card asked again");

	/* That one too large too: left out, the next page past it. */
	for (round = 0U; round < 33U; round++) {
		/* Another 8000 bytes. */
		respond_body(0x90U, chunk);
		length = take(packet, sizeof(packet));
		if (length == 0U)
			break;
	}

	/* The Abort answered. */
	btd_pbap_data(&pbap, TEST_DLCI, success, sizeof(success));
	check(has_answer("cursor=22220000.0.2.9 more=1 count=0 skipped=1 capped=0"), "large: the card left out, the next page past it");
}

/* Pages of the call history. */
static void
test_calls(void)
{
	static const uint8_t not_found[] = { 0xc4U, 0x00U, 0x03U };
	char body[1024];
	char key[17];
	char text[256];

	/* The calls taken since 1700000000: SIZE of ich, PULL with the history's fields. */
	setup();
	(void)btd_pbap_page(&pbap, 9U, BTD_PBAP_WHAT_CALLS, 1700000000, "", 32U);
	check(is_size_get("telecom/ich.vcf"), "calls: SIZE of ich");
	respond_size(3U);
	check(is_pull_get("telecom/ich.vcf", 32U, 0U, 1), "calls: PULL of ich with the history's fields");
	(void)snprintf(body, sizeof(body), "%s%s%s", call_new, call_old, call_bare);
	respond_body(0xa0U, body);

	/* The new call with its time in UTC; the old one not written; the bare one at the time now, partial. */
	test_key("r|20240101T120000Z|+15559999", key);
	(void)snprintf(text, sizeof(text), "9: PHONE CALL-LOG key=%s kind=received time=1704110400 zone=phone partial=0 length=0 datetime=\"20240101T120000Z\" peer=\"+15559999\" name=\"Bob\"\n", key);
	check(has_answer(text), "calls: the new call's line");
	check(!has_answer("name=\"Old\""), "calls: a call before since not written");
	check(has_answer("kind=received time=1700003600 zone=none partial=1 length=0 datetime=\"\" peer=\"\" name=\"\""), "calls: a call without a datetime");
	check(has_answer("9: PHONE PAGE-END cursor=22220000.2.0.ffffffff more=1 count=2 skipped=0 capped=0"), "calls: the next object, och");

	/* och without a size: taken as empty, mch next. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 9U, BTD_PBAP_WHAT_CALLS, 1700000000, "22220000.2.0.ffffffff", 32U);
	check(is_size_get("telecom/och.vcf"), "calls: SIZE of och");
	btd_pbap_data(&pbap, TEST_DLCI, not_found, sizeof(not_found));
	check(has_answer("cursor=22220000.3.0.ffffffff more=1 count=0"), "calls: och without a size is empty");

	/* mch empty: the probe at the size 0 answered empty, the pass's end. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 9U, BTD_PBAP_WHAT_CALLS, 1700000000, "22220000.3.0.ffffffff", 32U);
	check(is_size_get("telecom/mch.vcf"), "calls: SIZE of mch");
	respond_size(0U);
	check(is_pull_get("telecom/mch.vcf", 32U, 0U, 1), "calls: the probe of mch");
	respond_body(0xa0U, "");
	check(has_answer("cursor=22220000.4.0.ffffffff more=0 count=0"), "calls: the pass's end");

	/* A missed call's kind by its folder. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 9U, BTD_PBAP_WHAT_CALLS, 0, "22220000.3.0.1", 32U);
	(void)is_pull_get("telecom/mch.vcf", 32U, 0U, 1);
	respond_body(0xa0U, "BEGIN:VCARD\r\nVERSION:3.0\r\nTEL:+15551111\r\nX-IRMC-CALL-DATETIME:20240101T120000\r\nEND:VCARD\r\n");
	check(has_answer("kind=missed time=1704078000 zone=local partial=0"), "calls: missed by folder, the local time in zedBSD's zone");

	/* Past the history: nothing more at once. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 9U, BTD_PBAP_WHAT_CALLS, 0, "22220000.4.0.ffffffff", 32U);
	check(has_answer("9: PHONE PAGE-END cursor=22220000.4.0.ffffffff more=0 count=0 skipped=0 capped=0\n9: DONE\n"), "calls: past the history");
}

/* Cursors: stale, malformed, of the other kind. */
static void
test_cursors(void)
{
	const char *error;
	uint8_t packet[64];

	/* Malformed and of the other kind. */
	setup();
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "2222000.0.1.3", 2U);
	check(error != NULL && strcmp(error, "argument") == 0, "cursors: a short generation");
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3x", 2U);
	check(error != NULL && strcmp(error, "argument") == 0, "cursors: a trailing byte");
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.1.0.3", 2U);
	check(error != NULL && strcmp(error, "argument") == 0, "cursors: a history cursor for contacts");
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CALLS, 0, "22220000.0.0.3", 2U);
	check(error != NULL && strcmp(error, "argument") == 0, "cursors: a phone book cursor for calls");

	/* Another phone on the next link: the earlier cursors stale; the same phone keeps them. */
	btd_pbap_ended(&pbap);
	btd_pbap_ready(&pbap);
	sdp_answer(1, 0x01U);
	btd_pbap_opened(&pbap, TEST_DLCI, TEST_CHANNEL, 1);
	(void)take(packet, sizeof(packet));
	respond_connect();
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	check(error == NULL, "cursors: the same phone keeps its cursors");
	btd_pbap_ended(&pbap);
	world.address[5] = 0x02U;
	btd_pbap_ready(&pbap);
	sdp_answer(1, 0x01U);
	btd_pbap_opened(&pbap, TEST_DLCI, TEST_CHANNEL, 1);
	(void)take(packet, sizeof(packet));
	respond_connect();
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	check(error != NULL && strcmp(error, "stale-cursor") == 0, "cursors: another phone's cursor stale");
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220001.0.1.3", 2U);
	check(error == NULL, "cursors: the new generation's");
}

/* The phone closing the connection, and pages waiting for it. */
static void
test_idle(void)
{
	uint8_t packet[64];
	const char *error;

	/* Closed while nothing runs: idle, told as ready; the next page connects again. */
	setup();
	btd_pbap_closed(&pbap, TEST_DLCI, BTD_RFCOMM_CLOSED_REMOTE);
	check(state_is("contacts=ready") && pbap.state == BTD_PBAP_IDLE, "idle: the phone's close while idle");
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	check(error == NULL && world.dlc_opens == 2U && state_is("contacts=connecting"), "idle: the page opens the DLC again");
	btd_pbap_opened(&pbap, TEST_DLCI, TEST_CHANNEL, 1);
	(void)take(packet, sizeof(packet));
	respond_connect();
	check(is_pull_get("telecom/pb.vcf", 2U, 1U, 0), "idle: the page's PULL after Connect, its cursor still good");
	respond_body(0xa0U, card_alice);
	check(has_answer("name=\"Alice\"") && world.up == 2U, "idle: the page answered");

	/* Closed under an operation: failed, the page lost. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	(void)is_pull_get("telecom/pb.vcf", 2U, 1U, 0);
	btd_pbap_closed(&pbap, TEST_DLCI, BTD_RFCOMM_CLOSED_REMOTE);
	check(has_answer("7: ERROR lost\n7: DONE\n") && state_is("contacts=failed contacts_why=closed"), "idle: a close under an operation fails");

	/* A page waiting for a connection that does not come: timeout at 100 s, the attempt going on. */
	setup();
	btd_pbap_closed(&pbap, TEST_DLCI, BTD_RFCOMM_CLOSED_REMOTE);
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	check(btd_pbap_deadline(&pbap) == world.now + 100000U, "idle: the page waits 100 s");
	world.now += 100000U;
	btd_pbap_tick(&pbap, world.now);
	check(has_answer("7: ERROR timeout\n7: DONE\n") && pbap.state == BTD_PBAP_OPENING, "idle: timeout, still opening");
}

/* Clients: slow, gone, busy, not ready, arguments. */
static void
test_clients(void)
{
	char body[1024];
	const char *error;

	/* Not ready before the link, and arguments. */
	start();
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "", 2U);
	check(error != NULL && strcmp(error, "not-ready") == 0, "clients: not ready before the link");
	setup();
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "", 0U);
	check(error != NULL && strcmp(error, "argument") == 0, "clients: no card asked");
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "", 33U);
	check(error != NULL && strcmp(error, "argument") == 0, "clients: 33 cards asked");
	error = btd_pbap_page(&pbap, 7U, 3U, 0, "", 2U);
	check(error != NULL && strcmp(error, "argument") == 0, "clients: another kind");

	/* Two pages, a third busy; the second waits for the first. */
	error = btd_pbap_page(&pbap, 7U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	check(error == NULL, "clients: the first page");
	error = btd_pbap_page(&pbap, 8U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	check(error == NULL, "clients: the second page");
	error = btd_pbap_page(&pbap, 9U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	check(error != NULL && strcmp(error, "busy") == 0, "clients: a third busy");

	/* The first's client goes: its answer ends quietly, the second's PULL follows. */
	(void)is_pull_get("telecom/pb.vcf", 2U, 1U, 0);
	btd_pbap_cancel(&pbap, 7U);
	respond_body(0xa0U, card_alice);
	check(!has_answer("7: "), "clients: nothing to a client that went");
	check(is_pull_get("telecom/pb.vcf", 2U, 1U, 0), "clients: the second page's PULL");

	/* The second's client short of room: the page waits; 30 s later, slow. */
	world.room = 100;
	(void)snprintf(body, sizeof(body), "%s%s", card_alice, card_bob);
	respond_body(0xa0U, body);
	check(!has_answer("8: PHONE CONTACT") && btd_pbap_deadline(&pbap) == world.now + 30000U, "clients: a short client waits");
	world.now += 30000U;
	btd_pbap_tick(&pbap, world.now);
	check(has_answer("8: ERROR slow\n8: DONE\n"), "clients: slow");

	/* Room again: a waiting page goes on at the pump. */
	world.room = 100;
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 8U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	(void)is_pull_get("telecom/pb.vcf", 2U, 1U, 0);
	respond_body(0xa0U, body);
	world.room = 1000000;
	btd_pbap_pump(&pbap);
	check(has_answer("8: PHONE CONTACT") && has_answer("more=1 count=2"), "clients: the pump goes on");

	/* The link's end: off, a page lost. */
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_pbap_page(&pbap, 8U, BTD_PBAP_WHAT_CONTACTS, 0, "22220000.0.1.3", 2U);
	btd_pbap_ended(&pbap);
	check(has_answer("8: ERROR lost\n8: DONE\n") && state_is("contacts=off"), "clients: the link's end");
}
