/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of bluetoothd's pure parts of the HID host (ws143-p005
 * i02a, plan/ws143/phase005/phase.md section 7.4): the SDP client against
 * a HID record built here (in one response and over continuations, with a
 * server repeating its continuation, nesting past eight, an element past
 * its holder, a descriptor past 4096 bytes, no descriptor), the PnP
 * record, the HIDP header, the ATT PDUs and the smallest ATT server, the
 * HID device records' files (written, read, malformed, beside the bonds),
 * and the btsnoop record's bytes.
 * usage: bt-hid-host-test FOLDER   (an empty folder for the records)
 */

#include "userland/base/bluetoothd/att.h"
#include "userland/base/bluetoothd/hidcache.h"
#include "userland/base/bluetoothd/hidp.h"
#include "userland/base/bluetoothd/keys.h"
#include "userland/base/bluetoothd/sdp.h"
#include "userland/base/bluetoothd/snoop.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most a built record holds, and the stack of open sequences. */
#define BUILD_MAX	12288U
#define BUILD_DEPTH	16U

/* A record being built: its bytes, and where each open sequence's length goes. */
struct build {
	uint8_t bytes[BUILD_MAX];
	size_t used;
	size_t open[BUILD_DEPTH];
	unsigned depth;
};

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* The boot keyboard's report descriptor (HID 1.11 Appendix E.6, 63 bytes). */
static const uint8_t keyboard_descriptor[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
	0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
	0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
};

static void check(int condition, const char *what);
static void put(struct build *build, const uint8_t *bytes, size_t length);
static void put_byte(struct build *build, uint8_t byte);
static void put_uint16(struct build *build, uint16_t value);
static void put_attribute(struct build *build, uint16_t id);
static void put_uuid(struct build *build, uint16_t uuid);
static void put_bool(struct build *build, int value);
static void put_uint8(struct build *build, uint8_t value);
static void put_text(struct build *build, const uint8_t *bytes, size_t length);
static void open_sequence(struct build *build);
static void close_sequence(struct build *build);
static void build_hid_lists(struct build *build, const uint8_t *descriptor, size_t size, int with_descriptor, uint16_t interrupt);
static size_t response(uint8_t *out, uint16_t transaction, const uint8_t *fragment, size_t count, const uint8_t *continuation, size_t continuation_length);
static int run_query(struct btd_sdp *sdp, const struct build *lists, size_t chunk, unsigned repeat_after);
static void test_sdp(void);
static void test_sdp_bounds(void);
static void test_hidp(void);
static void test_att(void);
static void test_att_server(void);
static void test_hidcache(const char *folder);
static void test_snoop(void);
static void test_fuzz(void);

/* Runs the checks. */
int
main(
	int argc,
	char **argv)
{
	/* The folder for the records. */
	if (argc != 2) {
		fprintf(stderr, "usage: bt-hid-host-test FOLDER\n");
		return 2;
	}

	/* The parts. */
	test_sdp();
	test_sdp_bounds();
	test_hidp();
	test_att();
	test_att_server();
	test_hidcache(argv[1]);
	test_snoop();
	test_fuzz();

	/* The result. */
	if (failures != 0U) {
		fprintf(stderr, "bt-hid-host-test: %u of %u checks FAILED\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("bt-hid-host-test: ok (%u checks)\n", checks);
	return 0;
}

/* Counts a check, and says one that does not hold. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted. */
	checks++;
	if (condition)
		return;

	/* A failure. */
	failures++;
	fprintf(stderr, "FAIL: %s\n", what);
}

/* Adds bytes to a record being built. */
static void
put(
	struct build *build,
	const uint8_t *bytes,
	size_t length)
{
	/* Past the buffer is a mistake of the test. */
	if (length > BUILD_MAX - build->used) {
		fprintf(stderr, "bt-hid-host-test: a built record is too long\n");
		exit(2);
	}
	memcpy(&build->bytes[build->used], bytes, length);
	build->used += length;
}

/* Adds one byte. */
static void
put_byte(
	struct build *build,
	uint8_t byte)
{
	/* The byte. */
	put(build, &byte, 1U);
}

/* Adds a big-endian 16-bit number. */
static void
put_uint16(
	struct build *build,
	uint16_t value)
{
	/* Most significant first. */
	put_byte(build, (uint8_t)(value >> 8));
	put_byte(build, (uint8_t)value);
}

/* Adds an attribute's ID (a 16-bit unsigned element). */
static void
put_attribute(
	struct build *build,
	uint16_t id)
{
	/* uint16. */
	put_byte(build, 0x09);
	put_uint16(build, id);
}

/* Adds a 16-bit UUID element. */
static void
put_uuid(
	struct build *build,
	uint16_t uuid)
{
	/* uuid16. */
	put_byte(build, 0x19);
	put_uint16(build, uuid);
}

/* Adds a boolean element. */
static void
put_bool(
	struct build *build,
	int value)
{
	/* bool. */
	put_byte(build, 0x28);
	put_byte(build, (uint8_t)(value != 0));
}

/* Adds an 8-bit unsigned element. */
static void
put_uint8(
	struct build *build,
	uint8_t value)
{
	/* uint8. */
	put_byte(build, 0x08);
	put_byte(build, value);
}

/* Adds a text element with a 16-bit length. */
static void
put_text(
	struct build *build,
	const uint8_t *bytes,
	size_t length)
{
	/* text, 2-byte length. */
	put_byte(build, 0x26);
	put_uint16(build, (uint16_t)length);
	put(build, bytes, length);
}

/* Opens a sequence with a 16-bit length, filled in when it closes. */
static void
open_sequence(
	struct build *build)
{
	/* The header, its length to come. */
	put_byte(build, 0x36);
	build->open[build->depth] = build->used;
	build->depth++;
	put_uint16(build, 0);
}

/* Closes the innermost open sequence, writing its length. */
static void
close_sequence(
	struct build *build)
{
	size_t at;
	size_t length;

	/* The length of what came after its header. */
	build->depth--;
	at = build->open[build->depth];
	length = build->used - at - 2U;
	build->bytes[at] = (uint8_t)(length >> 8);
	build->bytes[at + 1U] = (uint8_t)length;
}

/*
 * Builds the attribute lists of a HID device: a HID record (the loopback
 * keyboard of section 6: the channels, the flags, the name, the
 * descriptor) and a PnP record.
 */
static void
build_hid_lists(
	struct build *build,
	const uint8_t *descriptor,
	size_t size,
	int with_descriptor,
	uint16_t interrupt)
{
	static const uint8_t name[] = "Loopback Keyboard";

	/* The lists. */
	memset(build, 0, sizeof(*build));
	open_sequence(build);

	/* The HID record. */
	open_sequence(build);
	put_attribute(build, 0x0001);
	open_sequence(build);
	put_uuid(build, BTD_SDP_UUID_HID);
	close_sequence(build);
	put_attribute(build, 0x0004);
	open_sequence(build);
	open_sequence(build);
	put_uuid(build, 0x0100);
	put_byte(build, 0x09);
	put_uint16(build, BTD_SDP_PSM_CONTROL);
	close_sequence(build);
	open_sequence(build);
	put_uuid(build, 0x0011);
	close_sequence(build);
	close_sequence(build);
	put_attribute(build, 0x000d);
	open_sequence(build);
	open_sequence(build);
	open_sequence(build);
	put_uuid(build, 0x0100);
	put_byte(build, 0x09);
	put_uint16(build, interrupt);
	close_sequence(build);
	open_sequence(build);
	put_uuid(build, 0x0011);
	close_sequence(build);
	close_sequence(build);
	close_sequence(build);
	put_attribute(build, 0x0100);
	put_text(build, name, sizeof(name) - 1U);
	put_attribute(build, 0x0202);
	put_uint8(build, 0x40);
	put_attribute(build, 0x0203);
	put_uint8(build, 0x21);
	put_attribute(build, 0x0204);
	put_bool(build, 1);
	put_attribute(build, 0x0205);
	put_bool(build, 0);
	if (with_descriptor) {
		put_attribute(build, 0x0206);
		open_sequence(build);
		open_sequence(build);
		put_uint8(build, 0x22);
		put_text(build, descriptor, size);
		close_sequence(build);
		close_sequence(build);
	}
	put_attribute(build, 0x020d);
	put_bool(build, 1);
	put_attribute(build, 0x020e);
	put_bool(build, 1);
	close_sequence(build);

	/* The PnP record. */
	open_sequence(build);
	put_attribute(build, 0x0001);
	open_sequence(build);
	put_uuid(build, BTD_SDP_UUID_PNP);
	close_sequence(build);
	put_attribute(build, 0x0201);
	put_byte(build, 0x09);
	put_uint16(build, 0x1209);
	put_attribute(build, 0x0202);
	put_byte(build, 0x09);
	put_uint16(build, 0x4b42);
	put_attribute(build, 0x0203);
	put_byte(build, 0x09);
	put_uint16(build, 0x0100);
	put_attribute(build, 0x0205);
	put_byte(build, 0x09);
	put_uint16(build, 0x0001);
	close_sequence(build);
	close_sequence(build);
}

/* Builds a ServiceSearchAttributeResponse. */
static size_t
response(
	uint8_t *out,
	uint16_t transaction,
	const uint8_t *fragment,
	size_t count,
	const uint8_t *continuation,
	size_t continuation_length)
{
	size_t parameters;

	/* The header, the count, the fragment and the continuation. */
	parameters = 2U + count + 1U + continuation_length;
	out[0] = 0x07;
	out[1] = (uint8_t)(transaction >> 8);
	out[2] = (uint8_t)transaction;
	out[3] = (uint8_t)(parameters >> 8);
	out[4] = (uint8_t)parameters;
	out[5] = (uint8_t)(count >> 8);
	out[6] = (uint8_t)count;
	if (count != 0U)
		memcpy(&out[7], fragment, count);
	out[7U + count] = (uint8_t)continuation_length;
	if (continuation_length != 0U)
		memcpy(&out[8U + count], continuation, continuation_length);
	return 8U + count + continuation_length;
}

/*
 * Plays a server: the lists in chunks, each response answering the
 * request the query builds (its transaction and continuation checked).
 * After repeat_after responses (0: never) the server gives the same
 * continuation without going on.  Returns the query's last answer.
 */
static int
run_query(
	struct btd_sdp *sdp,
	const struct build *lists,
	size_t chunk,
	unsigned repeat_after)
{
	uint8_t request[BTD_SDP_REQUEST_MAX];
	uint8_t pdu[BUILD_MAX + 32U];
	uint8_t continuation[4];
	size_t request_length;
	size_t offset;
	size_t count;
	size_t length;
	unsigned responses;
	int answer;
	int error;

	/* Request and response until the query ends. */
	offset = 0;
	responses = 0;
	for (;;) {
		error = btd_sdp_request(sdp, request, sizeof(request), &request_length);
		if (error != 0)
			return BTD_SDP_FAILED;

		/* The request's continuation names where the server is (4 bytes of the offset). */
		if (request[request_length - 1U - sdp->continuation_length] != sdp->continuation_length)
			return BTD_SDP_FAILED;

		/* The next fragment, with a continuation unless it is the last. */
		count = lists->used - offset;
		if (count > chunk)
			count = chunk;
		continuation[0] = (uint8_t)(offset >> 24);
		continuation[1] = (uint8_t)(offset >> 16);
		continuation[2] = (uint8_t)(offset >> 8);
		continuation[3] = (uint8_t)offset;
		if (repeat_after != 0U && responses >= repeat_after) {
			length = response(pdu, sdp->transaction, &lists->bytes[offset], 0, continuation, 4U);
		} else if (offset + count < lists->used) {
			offset += count;
			continuation[0] = (uint8_t)(offset >> 24);
			continuation[1] = (uint8_t)(offset >> 16);
			continuation[2] = (uint8_t)(offset >> 8);
			continuation[3] = (uint8_t)offset;
			length = response(pdu, sdp->transaction, &lists->bytes[offset - count], count, continuation, 4U);
		} else {
			length = response(pdu, sdp->transaction, &lists->bytes[offset], count, continuation, 0U);
			offset += count;
		}
		responses++;

		/* The answer. */
		answer = btd_sdp_input(sdp, pdu, length);
		if (answer != BTD_SDP_MORE)
			return answer;
		if (responses > 64U)
			return BTD_SDP_FAILED;
	}
}

/* The SDP client against the loopback keyboard's records. */
static void
test_sdp(void)
{
	static struct btd_sdp sdp;
	static struct build lists;
	struct btd_hid_record record;
	uint8_t request[BTD_SDP_REQUEST_MAX];
	size_t length;
	int answer;
	int error;

	/* The first request's bytes. */
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 0x0101);
	error = btd_sdp_request(&sdp, request, sizeof(request), &length);
	{
		static const uint8_t want[] = { 0x06, 0x01, 0x01, 0x00, 0x0f, 0x35, 0x03, 0x19, 0x11, 0x24, 0x02, 0x80,
			0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00 };
		check(error == 0 && length == sizeof(want) && memcmp(request, want, sizeof(want)) == 0, "sdp: the first request's bytes");
	}

	/* The whole lists in one response. */
	build_hid_lists(&lists, keyboard_descriptor, sizeof(keyboard_descriptor), 1, BTD_SDP_PSM_INTERRUPT);
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
	answer = run_query(&sdp, &lists, BUILD_MAX, 0);
	check(answer == BTD_SDP_DONE, "sdp: one response");
	memset(&record, 0, sizeof(record));
	error = btd_sdp_hid(&sdp, &record);
	check(error == 0 && record.hid, "sdp: the HID record is read");
	check(record.descriptor_size == sizeof(keyboard_descriptor) && memcmp(record.descriptor, keyboard_descriptor, sizeof(keyboard_descriptor)) == 0, "sdp: the report descriptor");
	check(record.virtual_cable == 1 && record.reconnect_initiate == 0 && record.normally_connectable == 1 && record.boot_device == 1, "sdp: the flags");
	check(record.subclass == 0x40 && record.country == 0x21 && strcmp(record.name, "Loopback Keyboard") == 0, "sdp: the subclass, country and name");
	error = btd_sdp_pnp(&sdp, &record);
	check(error == 0 && record.pnp && record.vendor == 0x1209 && record.product == 0x4b42 && record.version == 0x0100 && record.vendor_source == 1, "sdp: the PnP record");

	/* The same lists over continuations of 60 bytes. */
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 7);
	answer = run_query(&sdp, &lists, 60U, 0);
	memset(&record, 0, sizeof(record));
	error = btd_sdp_hid(&sdp, &record);
	check(answer == BTD_SDP_DONE && error == 0 && record.descriptor_size == sizeof(keyboard_descriptor), "sdp: over continuations");
	check(sdp.transaction > 7U, "sdp: each request a new transaction");

	/* A server that repeats its continuation. */
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
	answer = run_query(&sdp, &lists, 60U, 2U);
	check(answer == BTD_SDP_FAILED && sdp.why != NULL && strcmp(sdp.why, "protocol") == 0, "sdp: a repeated continuation ends the query");

	/* Records without HID's interrupt channel, or without a descriptor, are no HID. */
	build_hid_lists(&lists, keyboard_descriptor, sizeof(keyboard_descriptor), 1, 0x0015);
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
	(void)run_query(&sdp, &lists, BUILD_MAX, 0);
	memset(&record, 0, sizeof(record));
	check(btd_sdp_hid(&sdp, &record) == ENOENT, "sdp: another interrupt PSM is no HID");
	build_hid_lists(&lists, keyboard_descriptor, sizeof(keyboard_descriptor), 0, BTD_SDP_PSM_INTERRUPT);
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
	(void)run_query(&sdp, &lists, BUILD_MAX, 0);
	memset(&record, 0, sizeof(record));
	check(btd_sdp_hid(&sdp, &record) == ENOENT, "sdp: no descriptor is no HID");
}

/* The SDP client's bounds: a long descriptor, nesting, lengths, errors. */
static void
test_sdp_bounds(void)
{
	static struct btd_sdp sdp;
	static struct build lists;
	static uint8_t big[BTD_SDP_DESCRIPTOR_MAX + 1U];
	struct btd_hid_record record;
	uint8_t pdu[64];
	uint8_t nested[32];
	size_t length;
	unsigned index;
	int answer;

	/* A descriptor of 4097 bytes. */
	memset(big, 0x05, sizeof(big));
	build_hid_lists(&lists, big, sizeof(big), 1, BTD_SDP_PSM_INTERRUPT);
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
	answer = run_query(&sdp, &lists, 600U, 0);
	memset(&record, 0, sizeof(record));
	check(answer == BTD_SDP_DONE && btd_sdp_hid(&sdp, &record) == E2BIG, "sdp: a descriptor of 4097 bytes is refused");

	/* Nine sequences deep. */
	for (index = 0; index < 9U; index++) {
		nested[2U * index] = 0x35;
		nested[2U * index + 1U] = (uint8_t)(2U * (8U - index));
	}
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
	length = response(pdu, 1, nested, 18U, NULL, 0U);
	answer = btd_sdp_input(&sdp, pdu, length);
	check(answer == BTD_SDP_FAILED && strcmp(sdp.why, "malformed") == 0, "sdp: nine sequences deep are refused");

	/* Eight deep are taken. */
	for (index = 0; index < 8U; index++) {
		nested[2U * index] = 0x35;
		nested[2U * index + 1U] = (uint8_t)(2U * (7U - index));
	}
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
	length = response(pdu, 1, nested, 16U, NULL, 0U);
	answer = btd_sdp_input(&sdp, pdu, length);
	check(answer == BTD_SDP_DONE, "sdp: eight sequences deep are taken");

	/* An element longer than what holds it. */
	nested[0] = 0x35;
	nested[1] = 0x05;
	nested[2] = 0x09;
	nested[3] = 0x00;
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
	length = response(pdu, 1, nested, 4U, NULL, 0U);
	check(btd_sdp_input(&sdp, pdu, length) == BTD_SDP_FAILED, "sdp: an element past its holder is refused");

	/* Another transaction, an error response, a short PDU, a count past the parameters. */
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
	length = response(pdu, 2, nested, 0U, NULL, 0U);
	check(btd_sdp_input(&sdp, pdu, length) == BTD_SDP_FAILED && strcmp(sdp.why, "protocol") == 0, "sdp: another transaction");
	pdu[0] = 0x01;
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 2);
	check(btd_sdp_input(&sdp, pdu, length) == BTD_SDP_FAILED, "sdp: an error response");
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
	check(btd_sdp_input(&sdp, pdu, 4U) == BTD_SDP_FAILED && strcmp(sdp.why, "malformed") == 0, "sdp: a PDU shorter than its header");
	length = response(pdu, 1, nested, 2U, NULL, 0U);
	pdu[6] = 9;
	btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
	check(btd_sdp_input(&sdp, pdu, length) == BTD_SDP_FAILED && strcmp(sdp.why, "malformed") == 0, "sdp: a count past the parameters");
}

/* The HIDP header. */
static void
test_hidp(void)
{
	static uint8_t frame[BTD_HIDP_REPORT_MAX + 2U];
	struct btd_hidp message;
	int error;

	/* Built and taken apart. */
	check(btd_hidp_header(BTD_HIDP_SET_PROTOCOL, BTD_HIDP_PROTOCOL_REPORT) == 0x71, "hidp: SET_PROTOCOL(Report) is 0x71");
	check(btd_hidp_header(BTD_HIDP_CONTROL, BTD_HIDP_VIRTUAL_CABLE_UNPLUG) == 0x15, "hidp: VIRTUAL_CABLE_UNPLUG is 0x15");
	frame[0] = 0xa1;
	frame[1] = 0x00;
	error = btd_hidp_parse(frame, 9U, &message);
	check(error == 0 && message.type == BTD_HIDP_DATA && message.parameter == BTD_HIDP_REPORT_INPUT && message.length == 8U && message.data == &frame[1], "hidp: an input report");
	frame[0] = 0x03;
	error = btd_hidp_parse(frame, 1U, &message);
	check(error == 0 && message.type == BTD_HIDP_HANDSHAKE && message.parameter == BTD_HIDP_UNSUPPORTED, "hidp: a handshake");

	/* Bounds: an empty frame, a report of 512 and of 513 bytes. */
	check(btd_hidp_parse(frame, 0U, &message) == EINVAL, "hidp: an empty frame");
	frame[0] = 0xa1;
	check(btd_hidp_parse(frame, 1U + BTD_HIDP_REPORT_MAX, &message) == 0, "hidp: a report of 512 bytes");
	check(btd_hidp_parse(frame, 2U + BTD_HIDP_REPORT_MAX, &message) == E2BIG, "hidp: a report of 513 bytes is too long");
}

/* The ATT PDUs. */
static void
test_att(void)
{
	struct btd_att_pdu parsed;
	uint8_t pdu[64];
	size_t length;
	int error;

	/* The requests' bytes. */
	length = btd_att_build_mtu(pdu, sizeof(pdu), BTD_ATT_MTU_REQUEST, BTD_ATT_MTU);
	check(length == 3U && pdu[0] == 0x02 && pdu[1] == 185 && pdu[2] == 0, "att: Exchange MTU Request");
	length = btd_att_build_range(pdu, sizeof(pdu), BTD_ATT_READ_GROUP_REQUEST, 0x0001, 0xffff, 0x2800);
	{
		static const uint8_t want[] = { 0x10, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28 };
		check(length == sizeof(want) && memcmp(pdu, want, sizeof(want)) == 0, "att: Read By Group Type Request");
	}
	length = btd_att_build_range(pdu, sizeof(pdu), BTD_ATT_FIND_INFO_REQUEST, 0x0016, 0x0018, 0);
	check(length == 5U && pdu[0] == 0x04 && pdu[1] == 0x16 && pdu[3] == 0x18, "att: Find Information Request");
	length = btd_att_build_read(pdu, sizeof(pdu), 0x0014, 22, 1);
	check(length == 5U && pdu[0] == 0x0c && pdu[1] == 0x14 && pdu[3] == 22, "att: Read Blob Request");
	{
		static const uint8_t value[] = { 0x01, 0x00 };
		length = btd_att_build_write(pdu, sizeof(pdu), BTD_ATT_WRITE_REQUEST, 0x0017, value, sizeof(value));
		check(length == 5U && pdu[0] == 0x12 && pdu[1] == 0x17 && pdu[3] == 0x01 && pdu[4] == 0x00, "att: Write Request of a CCC");
	}
	check(btd_att_build_confirmation(pdu, sizeof(pdu)) == 1U && pdu[0] == 0x1e, "att: Handle Value Confirmation");
	check(btd_att_build_mtu(pdu, 2U, BTD_ATT_MTU_REQUEST, 23) == 0U, "att: a buffer too small");

	/* Responses taken apart. */
	{
		static const uint8_t group[] = { 0x11, 0x06, 0x10, 0x00, 0x20, 0x00, 0x12, 0x18, 0x30, 0x00, 0x33, 0x00, 0x0f, 0x18 };
		error = btd_att_parse(group, sizeof(group), &parsed);
		check(error == 0 && parsed.count == 2U && parsed.element == 6U && parsed.list == &group[2], "att: Read By Group Type Response");
	}
	{
		static const uint8_t ragged[] = { 0x11, 0x06, 0x10, 0x00, 0x20, 0x00, 0x12, 0x18, 0x30 };
		check(btd_att_parse(ragged, sizeof(ragged), &parsed) == EINVAL, "att: a list not of whole elements");
	}
	{
		static const uint8_t short_element[] = { 0x09, 0x01, 0x10, 0x00 };
		check(btd_att_parse(short_element, sizeof(short_element), &parsed) == EINVAL, "att: an element shorter than its handle");
	}
	{
		static const uint8_t info[] = { 0x05, 0x01, 0x17, 0x00, 0x02, 0x29, 0x18, 0x00, 0x08, 0x29 };
		error = btd_att_parse(info, sizeof(info), &parsed);
		check(error == 0 && parsed.format == BTD_ATT_FORMAT_16 && parsed.count == 2U, "att: Find Information Response, format 1");
	}
	{
		static const uint8_t bad_format[] = { 0x05, 0x03, 0x17, 0x00, 0x02, 0x29 };
		check(btd_att_parse(bad_format, sizeof(bad_format), &parsed) == EINVAL, "att: an unknown format");
	}
	{
		static const uint8_t error_pdu[] = { 0x01, 0x0a, 0x16, 0x00, 0x0f };
		error = btd_att_parse(error_pdu, sizeof(error_pdu), &parsed);
		check(error == 0 && parsed.request == 0x0a && parsed.handle == 0x0016 && parsed.error == BTD_ATT_INSUFFICIENT_ENCRYPTION, "att: Error Response");
	}
	{
		static const uint8_t notification[] = { 0x1b, 0x16, 0x00, 0x00, 0x05, 0x00 };
		error = btd_att_parse(notification, sizeof(notification), &parsed);
		check(error == 0 && parsed.handle == 0x0016 && parsed.length == 3U, "att: Handle Value Notification");
	}
	{
		static const uint8_t unknown[] = { 0x7f };
		check(btd_att_parse(unknown, sizeof(unknown), &parsed) == EPROTO, "att: an unknown opcode");
	}
}

/* The smallest ATT server's answers. */
static void
test_att_server(void)
{
	uint8_t out[32];
	size_t length;
	int error;

	/* The MTU exchange. */
	{
		static const uint8_t mtu[] = { 0x02, 23, 0 };
		error = btd_att_answer(mtu, sizeof(mtu), out, sizeof(out), &length);
		check(error == 0 && length == 3U && out[0] == 0x03 && out[1] == 185, "att server: Exchange MTU Response with 185");
	}

	/* Discovery: Attribute Not Found at the request's start. */
	{
		static const uint8_t group[] = { 0x10, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28 };
		error = btd_att_answer(group, sizeof(group), out, sizeof(out), &length);
		check(error == 0 && length == 5U && out[0] == 0x01 && out[1] == 0x10 && out[2] == 0x01 && out[3] == 0x00 && out[4] == BTD_ATT_NOT_FOUND, "att server: Read By Group Type is not found");
	}
	{
		static const uint8_t read[] = { 0x0a, 0x03, 0x00 };
		error = btd_att_answer(read, sizeof(read), out, sizeof(out), &length);
		check(error == 0 && length == 5U && out[1] == 0x0a && out[2] == 0x03 && out[4] == BTD_ATT_NOT_FOUND, "att server: Read is not found");
	}

	/* Writes are not supported; commands, responses and confirmations get nothing. */
	{
		static const uint8_t write[] = { 0x12, 0x05, 0x00, 0x01 };
		error = btd_att_answer(write, sizeof(write), out, sizeof(out), &length);
		check(error == 0 && length == 5U && out[1] == 0x12 && out[4] == BTD_ATT_REQUEST_NOT_SUPPORTED, "att server: Write Request is not supported");
	}
	{
		static const uint8_t command[] = { 0x52, 0x05, 0x00, 0x01 };
		error = btd_att_answer(command, sizeof(command), out, sizeof(out), &length);
		check(error == 0 && length == 0U, "att server: a Write Command gets nothing");
	}
	{
		static const uint8_t notification[] = { 0x1b, 0x05, 0x00, 0x01 };
		error = btd_att_answer(notification, sizeof(notification), out, sizeof(out), &length);
		check(error == 0 && length == 0U, "att server: a notification gets nothing");
	}
	{
		static const uint8_t confirmation[] = { 0x1e };
		error = btd_att_answer(confirmation, sizeof(confirmation), out, sizeof(out), &length);
		check(error == 0 && length == 0U, "att server: a confirmation gets nothing");
	}

	/* A malformed request and an unknown one. */
	{
		static const uint8_t malformed[] = { 0x0a, 0x03 };
		error = btd_att_answer(malformed, sizeof(malformed), out, sizeof(out), &length);
		check(error == 0 && length == 5U && out[4] == BTD_ATT_INVALID_PDU, "att server: a malformed request is Invalid PDU");
	}
	{
		static const uint8_t unknown[] = { 0x20, 0x01 };
		error = btd_att_answer(unknown, sizeof(unknown), out, sizeof(out), &length);
		check(error == 0 && length == 5U && out[1] == 0x20 && out[4] == BTD_ATT_REQUEST_NOT_SUPPORTED, "att server: an unknown request is not supported");
	}
}

/* The HID device records' files. */
static void
test_hidcache(
	const char *folder)
{
	static const uint8_t controller[BTD_ADDRESS_BYTES] = { 0x55, 0x44, 0x33, 0x22, 0x11, 0x00 };
	static const uint8_t address[BTD_ADDRESS_BYTES] = { 0x01, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a };
	static struct btd_hidcache record;
	static struct btd_hidcache back;
	static char text[BTD_HIDCACHE_TEXT_MAX];
	struct btd_bond bond;
	struct btd_bond bonds[4];
	char path[256];
	char *line;
	unsigned count;
	size_t index;
	int error;

	/* A confirmed record with a descriptor of 4096 bytes (32 full lines). */
	memset(&record, 0, sizeof(record));
	memcpy(record.address, address, sizeof(address));
	record.type = BTD_ADDRESS_BREDR;
	record.confirmed = 1;
	record.normally_connectable = 1;
	record.virtual_cable = 1;
	record.boot_device = 1;
	record.vendor = 0x1209;
	record.product = 0x4b42;
	record.version = 0x0100;
	record.country = 0x21;
	record.device_class = 0x002540;
	(void)snprintf(record.name, sizeof(record.name), "Loopback Keyboard");
	record.descriptor_size = BTD_HIDCACHE_DESCRIPTOR_MAX;
	for (index = 0; index < record.descriptor_size; index++)
		record.descriptor[index] = (uint8_t)(index * 7U);
	error = btd_hidcache_write(folder, controller, &record);
	check(error == 0, "hidcache: written");
	error = btd_hidcache_read(folder, controller, address, BTD_ADDRESS_BREDR, &back);
	check(error == 0, "hidcache: read");
	check(back.confirmed && !back.le && back.normally_connectable && back.virtual_cable && back.boot_device && !back.reconnect_initiate, "hidcache: the state and flags come back");
	check(back.vendor == 0x1209 && back.product == 0x4b42 && back.version == 0x0100 && back.country == 0x21 && back.device_class == 0x002540, "hidcache: the numbers come back");
	check(strcmp(back.name, "Loopback Keyboard") == 0, "hidcache: the name comes back");
	check(back.descriptor_size == BTD_HIDCACHE_DESCRIPTOR_MAX && memcmp(back.descriptor, record.descriptor, BTD_HIDCACHE_DESCRIPTOR_MAX) == 0, "hidcache: the descriptor of 4096 bytes comes back");

	/* A short descriptor (a last line of 63 bytes), a candidate. */
	record.confirmed = 0;
	record.descriptor_size = sizeof(keyboard_descriptor) + 128U;
	memcpy(&record.descriptor[128], keyboard_descriptor, sizeof(keyboard_descriptor));
	error = btd_hidcache_format(&record, text, sizeof(text));
	check(error == 0, "hidcache: formatted");
	error = btd_hidcache_parse(text, strlen(text), &back);
	check(error == 0 && !back.confirmed && back.descriptor_size == record.descriptor_size && memcmp(back.descriptor, record.descriptor, record.descriptor_size) == 0, "hidcache: a last line shorter than 128 bytes");

	/* Malformed: a line missing, repeated, cut, a size past its lines, no state. */
	line = strstr(text, "descriptor_1=");
	if (line != NULL) {
		line[11] = '9';
		check(btd_hidcache_parse(text, strlen(text), &back) == EBADMSG, "hidcache: a descriptor line missing");
		line[11] = '0';
		check(btd_hidcache_parse(text, strlen(text), &back) == EBADMSG, "hidcache: a descriptor line repeated");
		line[11] = '1';
	} else {
		check(0, "hidcache: the second line is there");
	}
	error = btd_hidcache_format(&record, text, sizeof(text));
	text[strlen(text) - 3U] = '\0';
	check(error == 0 && btd_hidcache_parse(text, strlen(text), &back) == EBADMSG, "hidcache: a last line cut");
	record.descriptor_size = 0;
	(void)btd_hidcache_format(&record, text, sizeof(text));
	check(btd_hidcache_parse(text, strlen(text), &back) == 0 && back.descriptor_size == 0U, "hidcache: no descriptor (LE)");
	line = strstr(text, "descriptor_size=0");
	if (line != NULL)
		line[16] = '5';
	check(btd_hidcache_parse(text, strlen(text), &back) == EBADMSG, "hidcache: a size without its lines");
	line = strstr(text, "state=");
	if (line != NULL)
		line[0] = 'x';
	check(btd_hidcache_parse(text, strlen(text), &back) == EBADMSG, "hidcache: no state");
	check(btd_hidcache_parse("state=confirmed\nstate=candidate\ntransport=le\ndescriptor_size=0\n", 63U, &back) == EBADMSG, "hidcache: a key repeated");

	/* Beside a bond, the record is not a bond. */
	memset(&bond, 0, sizeof(bond));
	memcpy(bond.address, address, sizeof(address));
	bond.type = BTD_ADDRESS_BREDR;
	bond.have_link_key = 1;
	bond.key_size = 16;
	error = btd_keys_write(folder, controller, &bond);
	check(error == 0, "hidcache: a bond beside");
	error = btd_keys_list(folder, controller, bonds, 4U, &count);
	check(error == 0 && count == 1U, "hidcache: the bonds' list passes over the record");

	/* Forgotten. */
	error = btd_hidcache_forget(folder, controller, address, BTD_ADDRESS_BREDR);
	check(error == 0, "hidcache: forgotten");
	error = btd_hidcache_read(folder, controller, address, BTD_ADDRESS_BREDR, &back);
	check(error == ENOENT, "hidcache: gone");
	error = btd_hidcache_path(folder, controller, address, BTD_ADDRESS_LE_PUBLIC, path, sizeof(path));
	check(error == 0 && strstr(path, "/00:11:22:33:44:55/0A:0B:0C:0D:0E:01-le-public.hid") != NULL, "hidcache: the path");
}

/* The btsnoop record's bytes. */
static void
test_snoop(void)
{
	static const uint8_t reset[] = { 0x03, 0x0c, 0x00 };
	uint8_t out[64];
	size_t length;

	/* The header. */
	length = btd_snoop_header(out, sizeof(out));
	{
		static const uint8_t want[] = { 'b', 't', 's', 'n', 'o', 'o', 'p', 0, 0, 0, 0, 1, 0, 0, 0x03, 0xea };
		check(length == 16U && memcmp(out, want, sizeof(want)) == 0, "snoop: the header");
	}

	/* A sent command at the epoch of 1970. */
	length = btd_snoop_record(out, sizeof(out), BTD_SNOOP_COMMAND, 0, 0, reset, sizeof(reset));
	{
		static const uint8_t want[] = { 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 2, 0, 0, 0, 0,
			0x00, 0xdc, 0xdd, 0xb3, 0x0f, 0x2f, 0x80, 0x00, 0x01, 0x03, 0x0c, 0x00 };
		check(length == sizeof(want) && memcmp(out, want, sizeof(want)) == 0, "snoop: a sent command's record");
	}

	/* A received ACL packet cut to the buffer: the original length kept. */
	length = btd_snoop_record(out, 27U, BTD_SNOOP_ACL, 1, 0, reset, sizeof(reset));
	check(length == 27U && out[3] == 4 && out[7] == 3 && out[11] == 1 && out[24] == 0x02, "snoop: a received ACL record cut");
}

/*
 * A fixed-seed fuzz: random and mutated SDP responses into the client
 * (and the records read from what it took), random ATT PDUs into the
 * parser and the server.  Nothing may crash or read out of bounds (the
 * sanitizers watch); the answers must be ones the parts give.
 */
static void
test_fuzz(void)
{
	static struct btd_sdp sdp;
	static struct build lists;
	struct btd_hid_record record;
	struct btd_att_pdu parsed;
	uint8_t pdu[700];
	uint8_t out[32];
	uint32_t state;
	size_t length;
	size_t index;
	unsigned round;
	int answer;
	int strange;
	int error;

	/* The loopback keyboard's lists, the base of the mutations. */
	build_hid_lists(&lists, keyboard_descriptor, sizeof(keyboard_descriptor), 1, BTD_SDP_PSM_INTERRUPT);
	state = 0x2545f491U;
	strange = 0;
	for (round = 0; round < 20000U; round++) {
		/* A response: the lists cut at random and mutated, or random bytes. */
		btd_sdp_init(&sdp, BTD_SDP_UUID_HID, 1);
		state = state * 1103515245U + 12345U;
		length = (state >> 8) % 600U;
		if (length > lists.used)
			length = lists.used;
		length = response(pdu, 1, lists.bytes, length, NULL, 0U);
		for (index = 0; index < 4U; index++) {
			state = state * 1103515245U + 12345U;
			pdu[5U + (state >> 8) % (length - 5U)] = (uint8_t)(state >> 20);
		}
		if ((round & 7U) == 0U) {
			for (index = 0; index < length; index++) {
				state = state * 1103515245U + 12345U;
				pdu[index] = (uint8_t)(state >> 16);
			}
		}
		answer = btd_sdp_input(&sdp, pdu, length);
		if (answer != BTD_SDP_DONE && answer != BTD_SDP_MORE && answer != BTD_SDP_FAILED)
			strange++;
		if (answer == BTD_SDP_DONE) {
			memset(&record, 0, sizeof(record));
			error = btd_sdp_hid(&sdp, &record);
			if (error != 0 && error != ENOENT && error != EINVAL && error != E2BIG)
				strange++;
			if (error == 0 && record.descriptor_size > BTD_SDP_DESCRIPTOR_MAX)
				strange++;
			(void)btd_sdp_pnp(&sdp, &record);
		}

		/* An ATT PDU of random bytes. */
		state = state * 1103515245U + 12345U;
		length = (state >> 8) % 40U;
		for (index = 0; index < length; index++) {
			state = state * 1103515245U + 12345U;
			pdu[index] = (uint8_t)(state >> 16);
		}
		error = btd_att_parse(pdu, length, &parsed);
		if (error != 0 && error != EINVAL && error != EPROTO)
			strange++;
		if (error == 0 && parsed.list != NULL && 2U + parsed.count * parsed.element != length)
			strange++;
		error = btd_att_answer(pdu, length, out, sizeof(out), &index);
		if (error != 0 || (index != 0U && index != 3U && index != 5U))
			strange++;
	}
	check(strange == 0, "fuzz: 20000 SDP responses and ATT PDUs give only the parts' answers");
}
