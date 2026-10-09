/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the MAP client (ws197-p003, plan/ws197/phase003/
 * phase.md section 8), built with the host's compiler under ASan and
 * UBSan.  map.c runs against hooks: the test plays the phone's MSE, reads
 * each OBEX request map.c writes on the MAS's DLC, checks its headers and
 * application parameters against constants written by hand from MAP
 * 1.4.2's tables (never map.c's macros), and answers with OBEX packets
 * written by hand.
 *
 *   setup      SDP, Connect with the MAS Target and its 60 s wait,
 *              SetPath telecom and msg, the notifications' registration,
 *              ready; the phone's MNS connecting
 *   page       COUNT (asked again in the phone's zone), LIST with its
 *              parameters, a Get answered in two packets, UNREAD, the
 *              item's line and key, PAGE-END and the next folder; stale
 *              cursors; a listing too large; a slow client; a cancel
 *   live       an event's search (again 2 s later), its Get, the
 *              subscribers' line, UNREAD; ahead of a page's operation;
 *              events of another MAS, malformed, of another type
 *   send       PushMessage, an event before its answer held, PHONE SENT
 *              once for each state
 *   read       PHONE READ and its errors, a stale handle, a full queue
 *   failures   the MAS's DLC closing (requests lost, the waits growing),
 *              a refusal (600 s, and at once on a check), no MAS, no
 *              answer to Connect, the MNS going alone
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/map.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Marks a parameter a function takes for its signature's sake. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The phone's MAS channel, the DLCs of the MAS (bluetoothd's) and of the MNS (the phone's, channel 16). */
#define TEST_MAS_CHANNEL	5U
#define TEST_MAS_DLCI		10U
#define TEST_MNS_DLCI		33U

/* The first MAP session's number the daemon draws, and the session of the first connection. */
#define TEST_FIRST_SESSION	0x11110000U
#define TEST_SESSION_TEXT	"11110001"

/* What the test keeps of what map.c wrote and said. */
#define TEST_OUT_MAX		65536U
#define TEST_TEXT_MAX		65536U
#define TEST_CLOSED_MAX		8U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/*
 * The world around map.c: the clocks and zones, whether messages are
 * wanted, what the hooks were asked (SDP, DLCs opened and closed), the
 * bytes written on the MAS and the MNS (not read yet), the answers and
 * the announcements (as text: "token: line" and the bytes), the client's
 * room, and how often the state changed and MAP came up.
 */
struct world {
	uint64_t now;
	int64_t wall;
	int32_t offset;
	int wanted;
	unsigned sdp_queries;
	int sdp_error;
	unsigned dlc_opens;
	unsigned dlc_channel;
	unsigned closed_count;
	unsigned closed[TEST_CLOSED_MAX];
	size_t mas_length;
	uint8_t mas[TEST_OUT_MAX];
	size_t mns_length;
	uint8_t mns[TEST_OUT_MAX];
	size_t answers_length;
	char answers[TEST_TEXT_MAX];
	size_t emits_length;
	char emits[TEST_TEXT_MAX];
	long room;
	unsigned changed;
	unsigned up;
};

/* The world, and the client under test (both live for one scenario: reset by start). */
static struct world world;
static struct btd_map map;

/* MAP's MAS and MNS Targets (MAP 1.4.2 table 6.5). */
static const uint8_t mas_uuid[16] = {
	0xbbU, 0x58U, 0x2bU, 0x40U, 0x42U, 0x0cU, 0x11U, 0xdbU, 0xb0U, 0xdeU, 0x08U, 0x00U, 0x20U, 0x0cU, 0x9aU, 0x66U
};
static const uint8_t mns_uuid[16] = {
	0xbbU, 0x58U, 0x2bU, 0x41U, 0x42U, 0x0cU, 0x11U, 0xdbU, 0xb0U, 0xdeU, 0x08U, 0x00U, 0x20U, 0x0cU, 0x9aU, 0x66U
};

static void check(int condition, const char *what);
static uint64_t hook_clock(void *context);
static int64_t hook_wall(void *context);
static int32_t hook_offset(void *context, int64_t seconds);
static int hook_wanted(void *context);
static int hook_sdp(void *context, uint16_t uuid);
static int hook_open(void *context, unsigned server_channel);
static int hook_write(void *context, unsigned dlci, const uint8_t *data, size_t length, size_t *written);
static void hook_close(void *context, unsigned dlci);
static void hook_answer(void *context, uint64_t token, const char *line, const uint8_t *bytes, size_t length);
static void hook_emit(void *context, const char *line, const uint8_t *bytes, size_t length);
static long hook_room(void *context, uint64_t token);
static void hook_changed(void *context);
static void hook_up(void *context);
static void start(void);
static void sdp_answer(uint8_t types, int features, uint32_t feature_bits);
static size_t take(uint8_t *packet, size_t size);
static void respond(uint8_t code, const uint8_t *headers, size_t length);
static void respond_body(uint8_t code, const char *body);
static void respond_connect(void);
static int find_header(const uint8_t *packet, size_t length, size_t first, uint8_t id, struct btd_obex_header *header);
static int find_parameter(const struct btd_obex_header *header, uint8_t tag, const uint8_t **value, size_t *size);
static int is_text(const struct btd_obex_header *header, const char *text);
static int has_answer(const char *text);
static int has_emit(const char *text);
static int state_is(const char *text);
static void setup(void);
static void mns_connect(void);
static void mns_event(uint8_t instance, const char *type, const char *body, uint8_t *code);
static void page_count(int64_t since, const char *begin, const char *size, const char *mse);
static void test_setup(void);
static void test_page(void);
static void test_page_errors(void);
static void test_live(void);
static void test_send(void);
static void test_read(void);
static void test_failures(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_setup();
	test_page();
	test_page_errors();
	test_live();
	test_send();
	test_read();
	test_failures();

	/* The count of what failed. */
	printf("bt-map-host-test: %u checks, %u failed\n", checks, failures);
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

/* Whether messages are wanted. */
static int
hook_wanted(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* The world's. */
	return world.wanted;
}

/* Counts an SDP query for the MAS. */
static int
hook_sdp(
	void *context,
	uint16_t uuid)
{
	UNUSED_PARAMETER(context);

	/* The MAS's class. */
	check(uuid == 0x1132U, "hooks: SDP for the MAS");
	world.sdp_queries++;

	/* The world's answer. */
	return world.sdp_error;
}

/* Notes a DLC asked for. */
static int
hook_open(
	void *context,
	unsigned server_channel)
{
	UNUSED_PARAMETER(context);

	/* Succeeded: noted. */
	world.dlc_opens++;
	world.dlc_channel = server_channel;
	return 0;
}

/* Keeps what map.c writes on a DLC. */
static int
hook_write(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length,
	size_t *written)
{
	UNUSED_PARAMETER(context);

	/* The MAS's, or the MNS's. */
	if (dlci == TEST_MAS_DLCI) {
		memcpy(world.mas + world.mas_length, data, length);
		world.mas_length += length;
	} else {
		memcpy(world.mns + world.mns_length, data, length);
		world.mns_length += length;
	}

	/* Succeeded: all taken. */
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

/* Keeps an answer as "token: line" and its bytes. */
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

	/* The line, then the bytes. */
	written = snprintf(world.answers + world.answers_length, sizeof(world.answers) - world.answers_length, "%llu: %s\n", (unsigned long long)token, line);
	world.answers_length += (size_t)written;
	if (length != 0U && world.answers_length + length < sizeof(world.answers)) {
		memcpy(world.answers + world.answers_length, bytes, length);
		world.answers_length += length;
		world.answers[world.answers_length] = '\0';
	}
}

/* Keeps an announcement and its bytes. */
static void
hook_emit(
	void *context,
	const char *line,
	const uint8_t *bytes,
	size_t length)
{
	int written;

	UNUSED_PARAMETER(context);

	/* The line, then the bytes. */
	written = snprintf(world.emits + world.emits_length, sizeof(world.emits) - world.emits_length, "%s\n", line);
	world.emits_length += (size_t)written;
	if (length != 0U && world.emits_length + length < sizeof(world.emits)) {
		memcpy(world.emits + world.emits_length, bytes, length);
		world.emits_length += length;
		world.emits[world.emits_length] = '\0';
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

/* Counts MAP coming up. */
static void
hook_up(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* Counted. */
	world.up++;
}

/* A new world and a new client: messages wanted, plenty of room, the clocks at a set time. */
static void
start(void)
{
	struct btd_map_hooks hooks;

	/* The world. */
	memset(&world, 0, sizeof(world));
	world.now = 1000000U;
	world.wall = 1700003600;
	world.offset = 0;
	world.wanted = 1;
	world.room = 1000000;

	/* The hooks. */
	memset(&hooks, 0, sizeof(hooks));
	hooks.clock = hook_clock;
	hooks.wall = hook_wall;
	hooks.local_offset = hook_offset;
	hooks.wanted = hook_wanted;
	hooks.sdp_query = hook_sdp;
	hooks.dlc_open = hook_open;
	hooks.dlc_write = hook_write;
	hooks.dlc_close = hook_close;
	hooks.answer = hook_answer;
	hooks.emit = hook_emit;
	hooks.room = hook_room;
	hooks.changed = hook_changed;
	hooks.up = hook_up;
	btd_map_init(&map, &hooks, TEST_FIRST_SESSION);
}

/*
 * Answers the SDP query with one MAS record written by hand (Core Vol 3
 * Part B section 3): ServiceClassIDList 0x1132; ProtocolDescriptorList
 * L2CAP, RFCOMM channel 5, OBEX; MASInstanceID 0; SupportedMessageTypes;
 * MapSupportedFeatures when asked.
 */
static void
sdp_answer(
	uint8_t types,
	int features,
	uint32_t feature_bits)
{
	static struct btd_sdp sdp;
	uint8_t record[64];
	size_t used;

	/* 0x0001: a sequence of UUID16 0x1132. */
	used = 0U;
	memcpy(record + used, "\x09\x00\x01\x35\x03\x19\x11\x32", 8U);
	used += 8U;

	/* 0x0004: (L2CAP), (RFCOMM, channel), (OBEX). */
	memcpy(record + used, "\x09\x00\x04\x35\x11\x35\x03\x19\x01\x00\x35\x05\x19\x00\x03\x08\x05\x35\x03\x19\x00\x08", 22U);
	used += 22U;

	/* 0x0315 MASInstanceID 0, 0x0316 SupportedMessageTypes. */
	memcpy(record + used, "\x09\x03\x15\x08\x00\x09\x03\x16\x08", 9U);
	used += 9U;
	record[used] = types;
	used++;

	/* 0x0317 MapSupportedFeatures, a 32-bit number. */
	if (features) {
		memcpy(record + used, "\x09\x03\x17\x0a", 4U);
		used += 4U;
		record[used] = (uint8_t)(feature_bits >> 24);
		record[used + 1U] = (uint8_t)(feature_bits >> 16);
		record[used + 2U] = (uint8_t)(feature_bits >> 8);
		record[used + 3U] = (uint8_t)feature_bits;
		used += 4U;
	}

	/* The lists: a sequence of the one record. */
	memset(&sdp, 0, sizeof(sdp));
	sdp.lists[0] = 0x35U;
	sdp.lists[1] = (uint8_t)(used + 2U);
	sdp.lists[2] = 0x35U;
	sdp.lists[3] = (uint8_t)used;
	memcpy(sdp.lists + 4, record, used);
	sdp.used = used + 4U;
	btd_map_sdp_done(&map, &sdp, 0);
}

/* Takes the first packet map.c wrote on the MAS.  Returns its length, or 0 when there is none. */
static size_t
take(
	uint8_t *packet,
	size_t size)
{
	size_t length;

	/* A whole packet. */
	if (world.mas_length < 3U)
		return 0U;
	length = ((size_t)world.mas[1] << 8) | world.mas[2];
	if (length > world.mas_length || length > size)
		return 0U;

	/* Succeeded: out of the bytes written. */
	memcpy(packet, world.mas, length);
	memmove(world.mas, world.mas + length, world.mas_length - length);
	world.mas_length -= length;
	return length;
}

/* Answers on the MAS: the code and the headers. */
static void
respond(
	uint8_t code,
	const uint8_t *headers,
	size_t length)
{
	uint8_t packet[TEST_OUT_MAX];

	/* The packet. */
	packet[0] = code;
	packet[1] = (uint8_t)((length + 3U) >> 8);
	packet[2] = (uint8_t)((length + 3U) & 0xffU);
	if (length != 0U)
		memcpy(packet + 3, headers, length);

	/* Succeeded: to map.c. */
	btd_map_data(&map, TEST_MAS_DLCI, packet, length + 3U);
}

/* Answers on the MAS with End of Body holding text. */
static void
respond_body(
	uint8_t code,
	const char *body)
{
	uint8_t headers[TEST_OUT_MAX];
	size_t length;

	/* End of Body. */
	length = strlen(body);
	headers[0] = 0x49U;
	headers[1] = (uint8_t)((length + 3U) >> 8);
	headers[2] = (uint8_t)((length + 3U) & 0xffU);
	memcpy(headers + 3, body, length);

	/* Succeeded: answered. */
	respond(code, headers, length + 3U);
}

/* Answers Connect: Success, OBEX 1.0, packets of 8192 bytes, Connection ID 1. */
static void
respond_connect(void)
{
	static const uint8_t answer[] = {
		0xa0U, 0x00U, 0x0cU, 0x10U, 0x00U, 0x20U, 0x00U, 0xcbU, 0x00U, 0x00U, 0x00U, 0x01U
	};

	/* Succeeded: to map.c. */
	btd_map_data(&map, TEST_MAS_DLCI, answer, sizeof(answer));
}

/* Finds a header of a request from its first header's offset.  Returns 1 with it, or 0. */
static int
find_header(
	const uint8_t *packet,
	size_t length,
	size_t first,
	uint8_t id,
	struct btd_obex_header *header)
{
	int found;

	/* The headers after the prefix. */
	found = btd_obex_find(packet + first, length - first, id, header);

	/* Found, or not. */
	return found > 0;
}

/* Finds an application parameter.  Returns 1 with its value, or 0. */
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
		if (header->data[at] == tag) {
			*value = header->data + at + 2U;
			*size = header->data[at + 1U];
			return 1;
		}
	}

	/* Not there. */
	return 0;
}

/* Tells whether a Unicode header is text (UTF-16BE with its NUL). */
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
		if (header->data[2U * index] != 0U || header->data[2U * index + 1U] != (uint8_t)text[index])
			return 0;
	}

	/* The NUL. */
	return header->data[2U * length] == 0U && header->data[2U * length + 1U] == 0U;
}

/* Tells whether the answers hold a text. */
static int
has_answer(
	const char *text)
{
	/* Found, or not. */
	return strstr(world.answers, text) != NULL;
}

/* Tells whether the announcements hold a text. */
static int
has_emit(
	const char *text)
{
	/* Found, or not. */
	return strstr(world.emits, text) != NULL;
}

/* Tells whether MAP's state text is the one given. */
static int
state_is(
	const char *text)
{
	char state[96];
	int error;

	/* The text. */
	error = btd_map_state_text(&map, state, sizeof(state));
	if (error != 0)
		return 0;

	/* The same, or not. */
	return strcmp(state, text) == 0;
}

/* Sets MAP up to ready with the phone's MNS connected (the checks are test_setup's). */
static void
setup(void)
{
	uint8_t packet[1024];

	/* The link ready, the MAS found and opened, Connect answered. */
	start();
	btd_map_ready(&map);
	sdp_answer(0x0eU, 1, 0x0000001fU);
	btd_map_opened(&map, TEST_MAS_DLCI, TEST_MAS_CHANNEL, 1);
	(void)take(packet, sizeof(packet));
	respond_connect();

	/* SetPath twice, the registration. */
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);

	/* The MNS. */
	mns_connect();
	world.answers_length = 0U;
	world.answers[0] = '\0';
	world.emits_length = 0U;
	world.emits[0] = '\0';
}

/* The phone's MNS DLC opens and its OBEX connects with the MNS's Target. */
static void
mns_connect(void)
{
	uint8_t request[32];

	/* The DLC. */
	btd_map_opened(&map, TEST_MNS_DLCI, 16U, 0);

	/* Connect: OBEX 1.0, 8192 bytes, the MNS's Target (MAP section 6.4.2). */
	memcpy(request, "\x80\x00\x1a\x10\x00\x20\x00\x46\x00\x13", 10U);
	memcpy(request + 10, mns_uuid, 16U);
	world.mns_length = 0U;
	btd_map_data(&map, TEST_MNS_DLCI, request, 26U);
}

/*
 * The phone puts an event report on the MNS (MAP section 5.1): Type, the
 * MAS's instance, the body.  Gives the MNS's answer code.
 */
static void
mns_event(
	uint8_t instance,
	const char *type,
	const char *body,
	uint8_t *code)
{
	uint8_t packet[4096];
	size_t used;
	size_t length;

	/* Put with the Final bit, the Connection ID map.c's server gave (1). */
	used = 3U;
	memcpy(packet + used, "\xcb\x00\x00\x00\x01", 5U);
	used += 5U;

	/* Type with its NUL. */
	length = strlen(type) + 1U;
	packet[used] = 0x42U;
	packet[used + 1U] = (uint8_t)((length + 3U) >> 8);
	packet[used + 2U] = (uint8_t)(length + 3U);
	memcpy(packet + used + 3U, type, length);
	used += length + 3U;

	/* MASInstanceID. */
	memcpy(packet + used, "\x4c\x00\x06\x0f\x01", 5U);
	packet[used + 5U] = instance;
	used += 6U;

	/* End of Body. */
	length = strlen(body);
	packet[used] = 0x49U;
	packet[used + 1U] = (uint8_t)((length + 3U) >> 8);
	packet[used + 2U] = (uint8_t)(length + 3U);
	memcpy(packet + used + 3U, body, length);
	used += length + 3U;

	/* Succeeded: sent, the answer read. */
	packet[0] = 0x82U;
	packet[1] = (uint8_t)(used >> 8);
	packet[2] = (uint8_t)used;
	world.mns_length = 0U;
	btd_map_data(&map, TEST_MNS_DLCI, packet, used);
	*code = 0U;
	if (world.mns_length != 0U)
		*code = world.mns[0];
}

/*
 * Checks the COUNT a page starts with (Get of the listing, MaxListCount 0,
 * the filter of types and time) and answers it with the listing's size
 * and the phone's time (each NULL for none).
 */
static void
page_count(
	int64_t since,
	const char *begin,
	const char *size,
	const char *mse)
{
	struct btd_obex_header header;
	uint8_t packet[1024];
	uint8_t answer[64];
	const uint8_t *value;
	size_t length;
	size_t used;
	int found;

	UNUSED_PARAMETER(since);

	/* Get with the Final bit, the listing's Type, Name inbox or sent. */
	length = take(packet, sizeof(packet));
	check(length != 0U && packet[0] == 0x83U, "count: a Get");
	found = find_header(packet, length, 3U, 0x42U, &header);
	check(found && header.length == 21U && memcmp(header.data, "x-bt/MAP-msg-listing", 21U) == 0, "count: Type with its NUL");

	/* MaxListCount 0, FilterMessageType 0x0C, FilterPeriodBegin. */
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found, "count: application parameters");
	found = find_parameter(&header, 0x01U, &value, &used);
	check(found && used == 2U && value[0] == 0U && value[1] == 0U, "count: MaxListCount 0");
	found = find_parameter(&header, 0x03U, &value, &used);
	check(found && used == 1U && value[0] == 0x0cU, "count: FilterMessageType 0x0C");
	found = find_parameter(&header, 0x04U, &value, &used);
	check(found && used == 15U && memcmp(value, begin, 15U) == 0, "count: FilterPeriodBegin");
	found = find_parameter(&header, 0x10U, &value, &used);
	check(!found, "count: no ParameterMask");

	/* The answer: ListingSize and MSETime when given. */
	used = 0U;
	answer[0] = 0x4cU;
	used = 3U;
	if (size != NULL) {
		memcpy(answer + used, size, 4U);
		used += 4U;
	}

	/* The phone's time. */
	if (mse != NULL) {
		answer[used] = 0x19U;
		answer[used + 1U] = (uint8_t)strlen(mse);
		memcpy(answer + used + 2U, mse, strlen(mse));
		used += 2U + strlen(mse);
	}

	/* The header's length, then the answer. */
	answer[1] = (uint8_t)(used >> 8);
	answer[2] = (uint8_t)used;
	respond(0xa0U, answer, used);
}

/* Setting up: SDP, Connect, SetPath twice, the registration, ready, the MNS. */
static void
test_setup(void)
{
	static const uint8_t telecom[] = {
		0x85U, 0x00U, 0x1dU, 0x02U, 0x00U, 0xcbU, 0x00U, 0x00U, 0x00U, 0x01U, 0x01U, 0x00U, 0x13U,
		0x00U, 't', 0x00U, 'e', 0x00U, 'l', 0x00U, 'e', 0x00U, 'c', 0x00U, 'o', 0x00U, 'm', 0x00U, 0x00U
	};
	static const uint8_t msg[] = {
		0x85U, 0x00U, 0x15U, 0x02U, 0x00U, 0xcbU, 0x00U, 0x00U, 0x00U, 0x01U, 0x01U, 0x00U, 0x0bU,
		0x00U, 'm', 0x00U, 's', 0x00U, 'g', 0x00U, 0x00U
	};
	static const char registration[] =
		"\x82\x00\x37\xcb\x00\x00\x00\x01\x42\x00\x25" "x-bt/MAP-NotificationRegistration" "\x00\x4c\x00\x06\x0e\x01\x01\x49\x00\x04\x30";
	uint8_t packet[1024];
	uint8_t connect[32];
	size_t length;
	int accepted;

	/* The link ready: the SDP query, connecting. */
	start();
	btd_map_ready(&map);
	check(world.sdp_queries == 1U, "setup: SDP asked");
	check(state_is("messages=connecting send=0 notify=0"), "setup: connecting");

	/* The MAS record: its DLC to channel 5. */
	sdp_answer(0x0eU, 1, 0x0000001fU);
	check(world.dlc_opens == 1U && world.dlc_channel == TEST_MAS_CHANNEL, "setup: the DLC to channel 5");

	/* The DLC: Connect, OBEX 1.0, 8192 bytes, the MAS's Target; 60 s for the answer. */
	btd_map_opened(&map, TEST_MAS_DLCI, TEST_MAS_CHANNEL, 1);
	memcpy(connect, "\x80\x00\x1a\x10\x00\x20\x00\x46\x00\x13", 10U);
	memcpy(connect + 10, mas_uuid, 16U);
	length = take(packet, sizeof(packet));
	check(length == 26U && memcmp(packet, connect, 26U) == 0, "setup: Connect's bytes");
	check(btd_map_deadline(&map) == world.now + 60000U, "setup: 60 s for Connect's answer");

	/* The answer 20 s later: SetPath telecom, not creating it, 10 s for its answer. */
	world.now += 20000U;
	btd_map_tick(&map, world.now);
	respond_connect();
	length = take(packet, sizeof(packet));
	check(length == sizeof(telecom) && memcmp(packet, telecom, sizeof(telecom)) == 0, "setup: SetPath telecom's bytes");
	check(btd_map_deadline(&map) == world.now + 10000U, "setup: 10 s for the next answer");

	/* SetPath msg. */
	respond(0xa0U, NULL, 0U);
	length = take(packet, sizeof(packet));
	check(length == sizeof(msg) && memcmp(packet, msg, sizeof(msg)) == 0, "setup: SetPath msg's bytes");

	/* The registration: NotificationStatus 1, the filler byte. */
	respond(0xa0U, NULL, 0U);
	length = take(packet, sizeof(packet));
	check(length == 55U && memcmp(packet, registration, 55U) == 0, "setup: the registration's bytes");

	/* Ready, sending (Uploading in the features), no notification before the MNS. */
	respond(0xa0U, NULL, 0U);
	check(world.up == 1U, "setup: MAP up");
	check(state_is("messages=ready send=1 notify=0"), "setup: ready");

	/* The MNS's channel offered while ready, no other. */
	accepted = btd_map_accept(&map, 16U);
	check(accepted == 1, "setup: channel 16 offered");
	accepted = btd_map_accept(&map, 5U);
	check(accepted == 0, "setup: another channel not offered");

	/* The phone's MNS: Connect answered Success, notifications on, no second MNS. */
	mns_connect();
	check(world.mns_length >= 3U && world.mns[0] == 0xa0U, "setup: the MNS's Connect answered");
	check(state_is("messages=ready send=1 notify=1"), "setup: notifications on");
	accepted = btd_map_accept(&map, 16U);
	check(accepted == 0, "setup: one MNS");
}

/* A page of the inbox, the next ones, and the end. */
static void
test_page(void)
{
	static const char list_parameters[] =
		"\x01\x02\x00\x02\x02\x02\x00\x00\x03\x01\x0c\x04\x0f" "20231115T091320" "\x10\x04\x00\x00\x11\x7e";
	static const char listing[] =
		"<MAP-msg-listing version=\"1.0\">"
		"<msg handle=\"10\" datetime=\"20231115T090000+1100\" sender_name=\"Jamie\" sender_addressing=\"+15551234\" type=\"SMS_GSM\" read=\"no\"/>"
		"<msg handle=\"11\" datetime=\"20231115T080000+1100\" type=\"EMAIL\" read=\"yes\"/>"
		"</MAP-msg-listing>";
	/* The text "Hello" (5): LENGTH 11 + 5 + 11 = 27. */
	static const char message[] =
		"BEGIN:BMSG\r\nVERSION:1.0\r\nSTATUS:UNREAD\r\nTYPE:SMS_GSM\r\nFOLDER:TELECOM/MSG/INBOX\r\n"
		"BEGIN:VCARD\r\nVERSION:2.1\r\nN:\r\nTEL:+15551234\r\nEND:VCARD\r\n"
		"BEGIN:BENV\r\nBEGIN:BBODY\r\nCHARSET:UTF-8\r\nLENGTH:27\r\nBEGIN:MSG\r\nHello\r\nEND:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n";
	static const char item[] =
		"7: PHONE MESSAGE handle=" TEST_SESSION_TEXT ".0000000000000010 key=23ef6660912108a2 folder=inbox dir=in time=1699999200 zone=phone "
		"datetime=\"20231115T090000+1100\" peer=\"+15551234\" name=\"Jamie\" read=0 partial=0 truncated=0 length=5\nHello";
	struct btd_obex_header header;
	uint8_t packet[1024];
	uint8_t first[1024];
	size_t length;
	size_t half;
	const char *refused;
	int found;

	/* A page of two from the inbox's start: its COUNT in zedBSD's zone, then again in the phone's (+11:00). */
	setup();
	refused = btd_map_page(&map, 7U, 1700000000, 500U, "", 2U);
	check(refused == NULL, "page: started");
	page_count(1700000000, "20231114T221320", "\x12\x02\x00\x03", "20231115T101320+1100");
	page_count(1700000000, "20231115T091320", "\x12\x02\x00\x03", NULL);

	/* LIST: two from 0, the filter, the fields (ParameterMask 0x0000117E). */
	length = take(packet, sizeof(packet));
	check(length != 0U && packet[0] == 0x83U, "page: the listing's Get");
	found = find_header(packet, length, 3U, 0x01U, &header);
	check(found && is_text(&header, "inbox"), "page: Name inbox");
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found && header.length == 34U && memcmp(header.data, list_parameters, 34U) == 0, "page: the listing's parameters");
	respond_body(0xa0U, listing);

	/* GET of the SMS: its handle in 16 digits, no attachment, UTF-8. */
	length = take(packet, sizeof(packet));
	check(length != 0U && packet[0] == 0x83U, "page: the message's Get");
	found = find_header(packet, length, 3U, 0x42U, &header);
	check(found && header.length == 13U && memcmp(header.data, "x-bt/message", 13U) == 0, "page: Type x-bt/message");
	found = find_header(packet, length, 3U, 0x01U, &header);
	check(found && is_text(&header, "0000000000000010"), "page: Name the handle");
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found && header.length == 6U && memcmp(header.data, "\x0a\x01\x00\x14\x01\x01", 6U) == 0, "page: Attachment 0, Charset 1");

	/* Answered in two packets: Continue with a Body, the next Get, then Success with End of Body. */
	half = strlen(message) / 2U;
	first[0] = 0x48U;
	first[1] = (uint8_t)((half + 3U) >> 8);
	first[2] = (uint8_t)(half + 3U);
	memcpy(first + 3, message, half);
	respond(0x90U, first, half + 3U);
	length = take(packet, sizeof(packet));
	check(length == 3U && packet[0] == 0x83U, "page: the next Get");
	respond_body(0xa0U, message + half);

	/* The item to the page's client. */
	check(has_answer(item), "page: the item's line and text");

	/* UNREAD: the read status set to 0, the filler. */
	length = take(packet, sizeof(packet));
	check(length != 0U && packet[0] == 0x82U, "page: the status's Put");
	found = find_header(packet, length, 3U, 0x42U, &header);
	check(found && header.length == 19U && memcmp(header.data, "x-bt/messageStatus", 19U) == 0, "page: Type x-bt/messageStatus");
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found && header.length == 6U && memcmp(header.data, "\x17\x01\x00\x18\x01\x00", 6U) == 0, "page: unread");
	found = find_header(packet, length, 3U, 0x49U, &header);
	check(found && header.length == 1U && header.data[0] == 0x30U, "page: the filler");
	respond(0xa0U, NULL, 0U);

	/* The page's end: the e-mail skipped, the next offset 2 of 3 in the inbox. */
	check(has_answer("7: PHONE PAGE-END cursor=" TEST_SESSION_TEXT ".6553f100.0.2.0 more=1 count=1 skipped=1 capped=0\n7: DONE\n"), "page: PAGE-END");

	/* The next page: LIST from 2 at once (no COUNT); empty, so the sent folder next. */
	world.answers_length = 0U;
	refused = btd_map_page(&map, 7U, 1700000000, 500U, TEST_SESSION_TEXT ".6553f100.0.2.0", 2U);
	check(refused == NULL, "page: the next started");
	length = take(packet, sizeof(packet));
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found && memcmp(header.data, "\x01\x02\x00\x02\x02\x02\x00\x02", 8U) == 0, "page: from offset 2");
	respond_body(0xa0U, "<MAP-msg-listing version=\"1.0\"/>");
	check(has_answer("7: PHONE PAGE-END cursor=" TEST_SESSION_TEXT ".6553f100.1.0.0 more=1 count=0 skipped=0 capped=0"), "page: the sent folder next");

	/* The sent folder: its COUNT (the phone's zone known now), its LIST empty, the end. */
	world.answers_length = 0U;
	(void)btd_map_page(&map, 7U, 1700000000, 500U, TEST_SESSION_TEXT ".6553f100.1.0.0", 2U);
	length = take(packet, sizeof(packet));
	found = find_header(packet, length, 3U, 0x01U, &header);
	check(found && is_text(&header, "sent"), "page: Name sent");
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found && memcmp(header.data, "\x01\x02\x00\x00", 4U) == 0, "page: the sent folder's COUNT");
	respond(0xa0U, NULL, 0U);
	(void)take(packet, sizeof(packet));
	respond_body(0xa0U, "<MAP-msg-listing version=\"1.0\"/>");
	check(has_answer("7: PHONE PAGE-END cursor=" TEST_SESSION_TEXT ".6553f100.2.0.0 more=0 count=0 skipped=0 capped=0"), "page: no more");

	/* Past the last folder: the end at once. */
	world.answers_length = 0U;
	refused = btd_map_page(&map, 7U, 1700000000, 500U, TEST_SESSION_TEXT ".6553f100.2.0.0", 2U);
	check(refused == NULL && has_answer("more=0 count=0") && take(packet, sizeof(packet)) == 0U, "page: past the last folder");

	/* A limit of 2 on an inbox of 3: the inbox cut after a whole page, said in PAGE-END and its cursor. */
	setup();
	world.answers_length = 0U;
	refused = btd_map_page(&map, 8U, 1700000000, 2U, "", 2U);
	check(refused == NULL, "page: a page with a limit");
	page_count(1700000000, "20231114T221320", "\x12\x02\x00\x03", NULL);
	(void)take(packet, sizeof(packet));
	respond_body(0xa0U, "<MAP-msg-listing><msg handle=\"1\" type=\"EMAIL\"/><msg handle=\"2\" type=\"EMAIL\"/></MAP-msg-listing>");
	check(has_answer("8: PHONE PAGE-END cursor=" TEST_SESSION_TEXT ".6553f100.1.0.1 more=1 count=0 skipped=2 capped=1"), "page: cut at the limit");

	/* A limit above 500, a cursor without its last part. */
	refused = btd_map_page(&map, 8U, 1700000000, 501U, "", 2U);
	check(refused != NULL && strcmp(refused, "argument") == 0, "page: a limit above 500");
	refused = btd_map_page(&map, 8U, 1700000000, 2U, TEST_SESSION_TEXT ".6553f100.1.0", 2U);
	check(refused != NULL && strcmp(refused, "argument") == 0, "page: an old cursor");
}

/* What a page refuses, a listing too large, a slow client, a client that went. */
static void
test_page_errors(void)
{
	struct btd_obex_header header;
	uint8_t packet[TEST_OUT_MAX];
	uint8_t body[8003];
	const char *refused;
	size_t length;
	unsigned round;
	int found;

	/* Not ready. */
	start();
	refused = btd_map_page(&map, 1U, 1700000000, 500U, "", 2U);
	check(refused != NULL && strcmp(refused, "not-ready") == 0, "errors: not ready");

	/* Cursors of another session or time, malformed ones, counts out of range. */
	setup();
	refused = btd_map_page(&map, 1U, 1700000000, 500U, "22220000.6553f100.0.2.0", 2U);
	check(refused != NULL && strcmp(refused, "stale-cursor") == 0, "errors: another session's cursor");
	refused = btd_map_page(&map, 1U, 1700000001, 500U, TEST_SESSION_TEXT ".6553f100.0.2.0", 2U);
	check(refused != NULL && strcmp(refused, "stale-cursor") == 0, "errors: another time's cursor");
	refused = btd_map_page(&map, 1U, 1700000000, 500U, "x", 2U);
	check(refused != NULL && strcmp(refused, "argument") == 0, "errors: a malformed cursor");
	refused = btd_map_page(&map, 1U, 1700000000, 500U, "", 0U);
	check(refused != NULL && strcmp(refused, "argument") == 0, "errors: count 0");
	refused = btd_map_page(&map, 1U, 1700000000, 500U, "", 33U);
	check(refused != NULL && strcmp(refused, "argument") == 0, "errors: count 33");

	/* A listing past 64 KB: aborted, asked again for half as many. */
	refused = btd_map_page(&map, 2U, 1700000000, 500U, "", 4U);
	check(refused == NULL, "errors: a page of four");
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);
	(void)take(packet, sizeof(packet));
	body[0] = 0x48U;
	body[1] = (uint8_t)(8003U >> 8);
	body[2] = (uint8_t)(8003U & 0xffU);
	memset(body + 3, 'a', 8000U);
	for (round = 0U; round < 12U; round++) {
		respond(0x90U, body, sizeof(body));
		length = take(packet, sizeof(packet));
		if (length != 0U && packet[0] == 0xffU)
			break;
	}

	/* Aborted once past 64 KB, after eight or more packets. */
	check(round < 12U && round >= 8U, "errors: Abort past 64 KB");
	respond(0xa0U, NULL, 0U);
	length = take(packet, sizeof(packet));
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found && memcmp(header.data, "\x01\x02\x00\x02", 4U) == 0, "errors: asked again for two");
	respond_body(0xa0U, "<MAP-msg-listing/>");

	/* A client with no room: the page waits, then goes on when it reads. */
	setup();
	world.room = 1000;
	(void)btd_map_page(&map, 3U, 1700000000, 500U, "", 2U);
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);
	(void)take(packet, sizeof(packet));
	respond_body(0xa0U, "<MAP-msg-listing><msg handle=\"1\" type=\"SMS_GSM\"/></MAP-msg-listing>");
	length = take(packet, sizeof(packet));
	check(length == 0U && btd_map_deadline(&map) == world.now + 30000U, "errors: the page waits for its client");
	world.room = 1000000;
	btd_map_pump(&map);
	length = take(packet, sizeof(packet));
	check(length != 0U && packet[0] == 0x83U, "errors: the page goes on when the client reads");

	/* A client too slow: 30 s, then ERROR slow. */
	setup();
	world.room = 1000;
	(void)btd_map_page(&map, 4U, 1700000000, 500U, "", 2U);
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);
	(void)take(packet, sizeof(packet));
	respond_body(0xa0U, "<MAP-msg-listing><msg handle=\"1\" type=\"SMS_GSM\"/></MAP-msg-listing>");
	world.now += 30000U;
	btd_map_tick(&map, world.now);
	check(has_answer("4: ERROR slow\n4: DONE\n"), "errors: a slow client");

	/* A client that went during its LIST: nothing more is asked or answered for it. */
	setup();
	(void)btd_map_page(&map, 5U, 1700000000, 500U, "", 2U);
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);
	(void)take(packet, sizeof(packet));
	btd_map_cancel(&map, 5U);
	respond_body(0xa0U, "<MAP-msg-listing><msg handle=\"1\" type=\"SMS_GSM\"/></MAP-msg-listing>");
	length = take(packet, sizeof(packet));
	check(length == 0U && world.answers_length == 0U, "errors: a client that went hears nothing");
}

/* Announced messages. */
static void
test_live(void)
{
	static const char event_20[] =
		"<MAP-event-report version=\"1.0\"><event type=\"NewMessage\" handle=\"20\" folder=\"TELECOM/MSG/INBOX\" msg_type=\"SMS_GSM\"/></MAP-event-report>";
	static const char listing_20[] =
		"<MAP-msg-listing version=\"1.0\"><msg handle=\"20\" datetime=\"20231115T120000+1100\" sender_name=\"Ann\" "
		"sender_addressing=\"+15550000\" type=\"SMS_GSM\" read=\"no\"/></MAP-msg-listing>";
	/* "Live one" (8): LENGTH 11 + 8 + 11 = 30. */
	static const char message_20[] =
		"BEGIN:BMSG\r\nVERSION:1.0\r\nSTATUS:UNREAD\r\nTYPE:SMS_GSM\r\nFOLDER:\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nCHARSET:UTF-8\r\n"
		"LENGTH:30\r\nBEGIN:MSG\r\nLive one\r\nEND:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n";
	struct btd_obex_header header;
	uint8_t packet[4096];
	const uint8_t *value;
	size_t length;
	size_t size;
	uint8_t code;
	int found;

	/* NewMessage: answered Success; the search of the inbox's last hour (32, the filter, the fields). */
	setup();
	mns_event(0U, "x-bt/MAP-event-report", event_20, &code);
	check(code == 0xa0U, "live: the event taken");
	length = take(packet, sizeof(packet));
	found = find_header(packet, length, 3U, 0x01U, &header);
	check(found && is_text(&header, "inbox"), "live: the search in the inbox");
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found, "live: the search's parameters");
	found = find_parameter(&header, 0x01U, &value, &size);
	check(found && size == 2U && value[0] == 0U && value[1] == 32U, "live: MaxListCount 32");
	found = find_parameter(&header, 0x02U, &value, &size);
	check(!found, "live: no offset");
	found = find_parameter(&header, 0x04U, &value, &size);
	check(found && size == 15U && memcmp(value, "20231114T221320", 15U) == 0, "live: from an hour ago");

	/* Not there yet: searched again 2 s later. */
	respond_body(0xa0U, "<MAP-msg-listing version=\"1.0\"/>");
	length = take(packet, sizeof(packet));
	check(length == 0U && btd_map_deadline(&map) == world.now + 2000U, "live: searched again in 2 s");
	world.now += 2000U;
	btd_map_tick(&map, world.now);
	length = take(packet, sizeof(packet));
	check(length != 0U && packet[0] == 0x83U, "live: the second search");

	/* Found: its Get, the line to the subscribers, unread again. */
	respond_body(0xa0U, listing_20);
	length = take(packet, sizeof(packet));
	found = find_header(packet, length, 3U, 0x01U, &header);
	check(found && is_text(&header, "0000000000000020"), "live: the message's Get");
	respond_body(0xa0U, message_20);
	check(has_emit("PHONE MESSAGE handle=" TEST_SESSION_TEXT ".0000000000000020 key=871260e9569ecf1c folder=inbox dir=in time=1700010000 zone=phone "
		       "datetime=\"20231115T120000+1100\" peer=\"+15550000\" name=\"Ann\" read=0 partial=0 truncated=0 length=8\nLive one"),
	      "live: the subscribers' line and text");
	length = take(packet, sizeof(packet));
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found && memcmp(header.data, "\x17\x01\x00\x18\x01\x00", 6U) == 0, "live: unread again");
	respond(0xa0U, NULL, 0U);

	/* Never found: fetched anyway, partial, its time when it came. */
	world.emits_length = 0U;
	mns_event(0U, "x-bt/MAP-event-report", "<MAP-event-report><event type=\"NewMessage\" handle=\"21\" folder=\"telecom/msg/inbox\" msg_type=\"SMS_CDMA\"/></MAP-event-report>", &code);
	(void)take(packet, sizeof(packet));
	respond_body(0xa0U, "<MAP-msg-listing/>");
	world.now += 2000U;
	btd_map_tick(&map, world.now);
	(void)take(packet, sizeof(packet));
	respond_body(0xa0U, "<MAP-msg-listing/>");
	(void)take(packet, sizeof(packet));
	respond_body(0xa0U, message_20);
	check(has_emit("handle=" TEST_SESSION_TEXT ".0000000000000021 key=- folder=inbox dir=in time=1700003600 zone=received datetime=\"\""), "live: not found, partial");
	check(has_emit("read=0 partial=1"), "live: partial=1");
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);

	/* Ahead of a page's operation: the search runs after the COUNT, before the LIST. */
	(void)btd_map_page(&map, 7U, 1700000000, 500U, "", 2U);
	(void)take(packet, sizeof(packet));
	mns_event(0U, "x-bt/MAP-event-report", event_20, &code);
	respond(0xa0U, NULL, 0U);
	length = take(packet, sizeof(packet));
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found && memcmp(header.data, "\x01\x02\x00\x20", 4U) == 0, "live: the search before the page's LIST");

	/* Events of another MAS, malformed, of another type. */
	setup();
	mns_event(1U, "x-bt/MAP-event-report", event_20, &code);
	length = take(packet, sizeof(packet));
	check(code == 0xa0U && length == 0U, "live: another MAS's event taken and dropped");
	mns_event(0U, "x-bt/MAP-event-report", "<MAP-event-report", &code);
	check(code == 0xc0U, "live: a malformed report is Bad Request");
	mns_event(0U, "x-bt/other", event_20, &code);
	check(code == 0xd1U, "live: another type is Not Implemented");

	/* A deleted message told. */
	mns_event(0U, "x-bt/MAP-event-report", "<MAP-event-report><event type=\"MessageDeleted\" handle=\"20\" folder=\"telecom/msg/inbox\" msg_type=\"SMS_GSM\"/></MAP-event-report>", &code);
	check(has_emit("PHONE MESSAGE-GONE handle=" TEST_SESSION_TEXT ".0000000000000020"), "live: MESSAGE-GONE");
}

/* Sending, its events, once each. */
static void
test_send(void)
{
	/* "Hi" (2): LENGTH 11 + 2 + 11 = 24. */
	static const char sent[] =
		"BEGIN:BMSG\r\nVERSION:1.0\r\nSTATUS:READ\r\nTYPE:SMS_GSM\r\nFOLDER:\r\nBEGIN:BENV\r\nBEGIN:VCARD\r\nVERSION:2.1\r\nN:\r\n"
		"TEL:+15551234\r\nEND:VCARD\r\nBEGIN:BBODY\r\nCHARSET:UTF-8\r\nLENGTH:24\r\nBEGIN:MSG\r\nHi\r\nEND:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n";
	static const uint8_t name_30[] = { 0x01U, 0x00U, 0x09U, 0x00U, '3', 0x00U, '0', 0x00U, 0x00U };
	struct btd_obex_header header;
	uint8_t packet[4096];
	const char *refused;
	const char *at;
	size_t length;
	unsigned count;
	uint8_t code;
	int found;

	/* PushMessage to the outbox: UTF-8, the bMessage written by hand. */
	setup();
	refused = btd_map_send(&map, 5U, "+15551234", (const uint8_t *)"Hi", 2U);
	check(refused == NULL, "send: started");
	length = take(packet, sizeof(packet));
	check(length != 0U && packet[0] == 0x82U, "send: one Put with the Final bit");
	found = find_header(packet, length, 3U, 0x01U, &header);
	check(found && is_text(&header, "outbox"), "send: Name outbox");
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found && header.length == 3U && memcmp(header.data, "\x14\x01\x01", 3U) == 0, "send: Charset 1");
	found = find_header(packet, length, 3U, 0x49U, &header);
	check(found && header.length == strlen(sent) && memcmp(header.data, sent, strlen(sent)) == 0, "send: the bMessage");

	/* SendingSuccess before PushMessage's answer: held. */
	mns_event(0U, "x-bt/MAP-event-report", "<MAP-event-report><event type=\"SendingSuccess\" handle=\"30\" folder=\"telecom/msg/sent\" msg_type=\"SMS_GSM\"/></MAP-event-report>", &code);
	check(world.emits_length == 0U, "send: the early event held");

	/* The answer names the message 30: the client told, the held event told. */
	respond(0xa0U, name_30, sizeof(name_30));
	check(has_answer("5: PHONE SENT request=286326785 handle=" TEST_SESSION_TEXT ".0000000000000030 state=pushed\n5: DONE\n"), "send: pushed");
	check(has_emit("PHONE SENT request=286326785 handle=" TEST_SESSION_TEXT ".0000000000000030 state=sent"), "send: sent");

	/* The shift to the sent folder: not told twice; then delivered. */
	mns_event(0U, "x-bt/MAP-event-report", "<MAP-event-report><event type=\"MessageShift\" handle=\"30\" folder=\"telecom/msg/sent\" old_folder=\"telecom/msg/outbox\" msg_type=\"SMS_GSM\"/></MAP-event-report>", &code);
	mns_event(0U, "x-bt/MAP-event-report", "<MAP-event-report><event type=\"DeliverySuccess\" handle=\"30\" folder=\"telecom/msg/sent\" msg_type=\"SMS_GSM\"/></MAP-event-report>", &code);
	count = 0U;
	for (at = strstr(world.emits, "state=sent"); at != NULL; at = strstr(at + 1, "state=sent"))
		count++;
	check(count == 1U, "send: sent told once");
	check(has_emit("handle=" TEST_SESSION_TEXT ".0000000000000030 state=delivered"), "send: delivered");
	check(take(packet, sizeof(packet)) == 0U, "send: the shift fetched nothing");

	/* A number that is not one, a text that is not UTF-8. */
	refused = btd_map_send(&map, 5U, "+1 555", (const uint8_t *)"Hi", 2U);
	check(refused != NULL && strcmp(refused, "number") == 0, "send: a bad number");
	refused = btd_map_send(&map, 5U, "+15551234", (const uint8_t *)"\xff", 1U);
	check(refused != NULL && strcmp(refused, "argument") == 0, "send: a text not UTF-8");

	/* A phone without Uploading. */
	start();
	btd_map_ready(&map);
	sdp_answer(0x0eU, 1, 0x00000007U);
	btd_map_opened(&map, TEST_MAS_DLCI, TEST_MAS_CHANNEL, 1);
	(void)take(packet, sizeof(packet));
	respond_connect();
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);
	refused = btd_map_send(&map, 5U, "+15551234", (const uint8_t *)"Hi", 2U);
	check(refused != NULL && strcmp(refused, "no-send") == 0, "send: no Uploading, no sending");
}

/* PHONE READ, its errors, a stale handle, a full queue. */
static void
test_read(void)
{
	struct btd_obex_header header;
	uint8_t packet[1024];
	const char *refused;
	size_t length;
	unsigned index;
	int found;

	/* Read: the status Put with the value 1; Not Found told. */
	setup();
	refused = btd_map_read(&map, 3U, TEST_SESSION_TEXT ".0000000000000010");
	check(refused == NULL, "read: started");
	length = take(packet, sizeof(packet));
	found = find_header(packet, length, 3U, 0x4cU, &header);
	check(found && memcmp(header.data, "\x17\x01\x00\x18\x01\x01", 6U) == 0, "read: read");
	respond(0xc4U, NULL, 0U);
	check(has_answer("3: ERROR not-found\n3: DONE\n"), "read: not-found");

	/* Service Unavailable, Forbidden, Success. */
	world.answers_length = 0U;
	(void)btd_map_read(&map, 3U, TEST_SESSION_TEXT ".10");
	(void)take(packet, sizeof(packet));
	respond(0xd3U, NULL, 0U);
	check(has_answer("3: ERROR unavailable"), "read: unavailable");
	(void)btd_map_read(&map, 3U, TEST_SESSION_TEXT ".10");
	(void)take(packet, sizeof(packet));
	respond(0xc3U, NULL, 0U);
	check(has_answer("3: ERROR permission"), "read: permission");
	world.answers_length = 0U;
	world.answers[0] = '\0';
	(void)btd_map_read(&map, 3U, TEST_SESSION_TEXT ".10");
	(void)take(packet, sizeof(packet));
	respond(0xa0U, NULL, 0U);
	check(strcmp(world.answers, "3: DONE\n") == 0, "read: done");

	/* Another session's handle, a malformed one. */
	refused = btd_map_read(&map, 3U, "22220000.0000000000000010");
	check(refused != NULL && strcmp(refused, "stale") == 0, "read: stale");
	refused = btd_map_read(&map, 3U, "zz");
	check(refused != NULL && strcmp(refused, "argument") == 0, "read: malformed");

	/* One running and 24 waiting fill the queue: the next is busy. */
	refused = NULL;
	for (index = 0U; index < 30U; index++) {
		refused = btd_map_read(&map, 3U, TEST_SESSION_TEXT ".10");
		if (refused != NULL)
			break;
	}

	/* The 26th refused. */
	check(index == 25U && refused != NULL && strcmp(refused, "busy") == 0, "read: a full queue");
}

/* Failures and the attempts after them. */
static void
test_failures(void)
{
	uint8_t packet[1024];
	static const uint8_t forbidden[] = { 0xc3U, 0x00U, 0x07U, 0x10U, 0x00U, 0x20U, 0x00U };
	size_t length;

	/* The MAS's DLC closed: a page and a read lost, failed, again in 30 s; the MNS closed at the next tick. */
	setup();
	(void)btd_map_page(&map, 7U, 1700000000, 500U, "", 2U);
	(void)btd_map_read(&map, 3U, TEST_SESSION_TEXT ".10");
	btd_map_closed(&map, TEST_MAS_DLCI, 0);
	check(has_answer("7: ERROR lost\n7: DONE\n") && has_answer("3: ERROR lost\n3: DONE\n"), "failures: the requests lost");
	check(state_is("messages=failed send=0 notify=0 why=closed"), "failures: failed (closed)");
	check(btd_map_deadline(&map) == world.now, "failures: the MNS to close at once");
	btd_map_tick(&map, world.now);
	check(world.closed_count == 1U && world.closed[0] == TEST_MNS_DLCI, "failures: the MNS's DLC closed");
	check(btd_map_deadline(&map) == world.now + 30000U, "failures: again in 30 s");

	/* Again: no answer to Connect in 60 s, failed (timeout), the next in 60 s. */
	world.now += 30000U;
	btd_map_tick(&map, world.now);
	check(world.sdp_queries == 2U, "failures: set up again");
	sdp_answer(0x0eU, 1, 0x0000001fU);
	btd_map_opened(&map, TEST_MAS_DLCI, TEST_MAS_CHANNEL, 1);
	(void)take(packet, sizeof(packet));
	world.now += 60000U;
	btd_map_tick(&map, world.now);
	check(state_is("messages=failed send=0 notify=0 why=timeout"), "failures: no answer to Connect");
	btd_map_tick(&map, world.now);
	check(btd_map_deadline(&map) == world.now + 60000U, "failures: the next in 60 s");

	/* Refused by the phone's user: 600 s; a check tries at once. */
	start();
	btd_map_ready(&map);
	sdp_answer(0x06U, 0, 0U);
	btd_map_opened(&map, TEST_MAS_DLCI, TEST_MAS_CHANNEL, 1);
	(void)take(packet, sizeof(packet));
	btd_map_data(&map, TEST_MAS_DLCI, forbidden, sizeof(forbidden));
	check(state_is("messages=failed send=0 notify=0 why=permission"), "failures: refused");
	btd_map_tick(&map, world.now);
	check(btd_map_deadline(&map) == world.now + 600000U, "failures: again in 600 s");
	btd_map_check(&map);
	btd_map_tick(&map, world.now);
	check(world.sdp_queries == 2U, "failures: a check tries at once");

	/* A phone without a MAS for SMS: failed, not tried again. */
	start();
	btd_map_ready(&map);
	sdp_answer(0x01U, 1, 0x1fU);
	check(state_is("messages=failed send=0 notify=0 why=no-mas"), "failures: no MAS");
	check(btd_map_deadline(&map) == 0U, "failures: no-mas not tried again");

	/* The MNS alone closed: registered again; not back in 30 s, no notifications. */
	setup();
	btd_map_closed(&map, TEST_MNS_DLCI, 0);
	length = take(packet, sizeof(packet));
	check(length == 55U && packet[0] == 0x82U, "failures: registered again");
	respond(0xa0U, NULL, 0U);
	check(state_is("messages=ready send=1 notify=1"), "failures: notify kept while awaited");
	world.now += 30000U;
	btd_map_tick(&map, world.now);
	check(state_is("messages=ready send=1 notify=0"), "failures: the MNS did not come back");

	/* The link's end: off, requests lost. */
	setup();
	(void)btd_map_read(&map, 3U, TEST_SESSION_TEXT ".10");
	btd_map_ended(&map);
	check(state_is("messages=off send=0 notify=0") && has_answer("3: ERROR lost"), "failures: the link's end");
}
