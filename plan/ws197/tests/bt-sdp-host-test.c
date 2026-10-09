/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of bluetoothd's SDP server and the client's reading of the
 * phone's records (ws197-p002, plan/ws197/phase002/phase.md section
 * 12.1), built with the host's compiler under ASan and UBSan.  The
 * requests are written out here byte by byte from Core 5.4 Vol 3 Part B
 * section 4; the server's records are read back with the client (sdp.c)
 * over a small MTU, so that the continuation states are used.
 *
 *   server   a record of the form MAP 1.4.2 section 7.1.2 gives MNS (the
 *            version 1.1 of Q16): its handle 0x00010000, the browse group
 *            put in, searches with the 16, 32 and 128 bit forms of a UUID
 *            and two UUIDs, an attribute request, the search-attribute
 *            request read back by the client, the errors (a PDU's size, an
 *            unknown PDU, too few bytes asked, no UUID, thirteen UUIDs, a
 *            continuation state too long, of another request, of older
 *            records, a handle not offered), an empty database
 *   client   two MAS records of MAP 1.4.2 table 7.1's form (two instances):
 *            their count, channels, version and unsigned attributes
 *   fuzz     random requests into the server
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/sdp.h"
#include "userland/base/bluetoothd/sdps.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fuzz's rounds, and the MTU of the read back (small, so that answers are cut). */
#define TEST_FUZZ_ROUNDS	200000U
#define TEST_SMALL_MTU		48U

/*
 * The attribute IDs of MAP's records the test uses (Bluetooth Assigned
 * Numbers, as the test writer remembers them; the test only needs them to
 * be IDs): GoepL2CapPsm, MASInstanceID, SupportedMessageTypes,
 * MapSupportedFeatures.
 */
#define TEST_GOEP_PSM		0x0200U
#define TEST_MAS_INSTANCE	0x0315U
#define TEST_MESSAGE_TYPES	0x0316U
#define TEST_MAP_FEATURES	0x0317U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

static void check(int condition, const char *what);
static size_t mns_record(uint8_t *bytes, size_t size);
static size_t mas_record(uint8_t *bytes, size_t size, unsigned channel, unsigned instance);
static size_t request(uint8_t *pdu, uint8_t id, uint16_t transaction, const uint8_t *parameters, size_t length);
static uint16_t error_code(const uint8_t *answer, size_t length);
static int read_back(struct btd_sdps *server, struct btd_sdp *client, uint16_t uuid);
static void test_server(void);
static void test_errors(void);
static void test_client(void);
static void test_fuzz(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_server();
	test_errors();
	test_client();
	test_fuzz();

	/* The count of what failed. */
	printf("bt-sdp-host-test: %u checks, %u failed\n", checks, failures);
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

/*
 * Writes the pairs of an MNS record (MAP 1.4.2 section 7.1.2's form, with
 * MAP 1.1 for Q16): ServiceClassIDList (MNS 0x1133), ProtocolDescriptorList
 * (L2CAP, RFCOMM channel 16, OBEX), BluetoothProfileDescriptorList (MAP
 * 0x1134, 0x0101), ServiceName.
 */
static size_t
mns_record(
	uint8_t *bytes,
	size_t size)
{
	struct btd_sdp_writer writer;

	/* ServiceClassIDList. */
	btd_sdp_writer_init(&writer, bytes, size);
	btd_sdp_put_uint16(&writer, 0x0001U);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x1133U);
	btd_sdp_end(&writer);

	/* ProtocolDescriptorList. */
	btd_sdp_put_uint16(&writer, 0x0004U);
	btd_sdp_begin(&writer);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x0100U);
	btd_sdp_end(&writer);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x0003U);
	btd_sdp_put_uint8(&writer, 16U);
	btd_sdp_end(&writer);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x0008U);
	btd_sdp_end(&writer);
	btd_sdp_end(&writer);

	/* BluetoothProfileDescriptorList. */
	btd_sdp_put_uint16(&writer, 0x0009U);
	btd_sdp_begin(&writer);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x1134U);
	btd_sdp_put_uint16(&writer, 0x0101U);
	btd_sdp_end(&writer);
	btd_sdp_end(&writer);

	/* ServiceName. */
	btd_sdp_put_uint16(&writer, 0x0100U);
	btd_sdp_put_text(&writer, "Keiland MNS");

	/* Succeeded: the pairs' length. */
	check(!writer.overflow, "record: MNS written");
	return writer.used;
}

/*
 * Writes the pairs of a phone's MAS record (MAP 1.4.2 table 7.1's form):
 * the class 0x1132, L2CAP, RFCOMM with the channel, OBEX, MAP 1.4,
 * ServiceName, GoepL2CapPsm, MASInstanceID, SupportedMessageTypes (SMS
 * GSM and CDMA), MapSupportedFeatures.
 */
static size_t
mas_record(
	uint8_t *bytes,
	size_t size,
	unsigned channel,
	unsigned instance)
{
	struct btd_sdp_writer writer;

	/* ServiceClassIDList and ProtocolDescriptorList. */
	btd_sdp_writer_init(&writer, bytes, size);
	btd_sdp_put_uint16(&writer, 0x0001U);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x1132U);
	btd_sdp_end(&writer);
	btd_sdp_put_uint16(&writer, 0x0004U);
	btd_sdp_begin(&writer);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x0100U);
	btd_sdp_end(&writer);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x0003U);
	btd_sdp_put_uint8(&writer, (uint8_t)channel);
	btd_sdp_end(&writer);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x0008U);
	btd_sdp_end(&writer);
	btd_sdp_end(&writer);

	/* The profile and the name. */
	btd_sdp_put_uint16(&writer, 0x0009U);
	btd_sdp_begin(&writer);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x1134U);
	btd_sdp_put_uint16(&writer, 0x0104U);
	btd_sdp_end(&writer);
	btd_sdp_end(&writer);
	btd_sdp_put_uint16(&writer, 0x0100U);
	btd_sdp_put_text(&writer, "MAP MAS-name");

	/* MAP's attributes. */
	btd_sdp_put_uint16(&writer, TEST_GOEP_PSM);
	btd_sdp_put_uint16(&writer, 0x1005U);
	btd_sdp_put_uint16(&writer, TEST_MAS_INSTANCE);
	btd_sdp_put_uint8(&writer, (uint8_t)instance);
	btd_sdp_put_uint16(&writer, TEST_MESSAGE_TYPES);
	btd_sdp_put_uint8(&writer, 0x06U);
	btd_sdp_put_uint16(&writer, TEST_MAP_FEATURES);
	btd_sdp_put_uint32(&writer, 0x0000007fU);

	/* Succeeded: the pairs' length. */
	check(!writer.overflow, "record: MAS written");
	return writer.used;
}

/* Writes a request PDU: its ID, transaction, parameters' length and parameters. */
static size_t
request(
	uint8_t *pdu,
	uint8_t id,
	uint16_t transaction,
	const uint8_t *parameters,
	size_t length)
{
	/* The header. */
	pdu[0] = id;
	pdu[1] = (uint8_t)(transaction >> 8);
	pdu[2] = (uint8_t)(transaction & 0xffU);
	pdu[3] = (uint8_t)(length >> 8);
	pdu[4] = (uint8_t)(length & 0xffU);

	/* Succeeded: the parameters after it. */
	memcpy(pdu + 5, parameters, length);
	return 5U + length;
}

/* Reads an error response's code (0 for any other PDU). */
static uint16_t
error_code(
	const uint8_t *answer,
	size_t length)
{
	/* An error response of seven bytes. */
	if (length != 7U || answer[0] != 0x01U)
		return 0U;

	/* Succeeded: its code. */
	return (uint16_t)((answer[5] << 8) | answer[6]);
}

/* Runs a client's whole query against the server: each request answered until the lists are whole.  Returns the client's last result. */
static int
read_back(
	struct btd_sdps *server,
	struct btd_sdp *client,
	uint16_t uuid)
{
	uint8_t out[64];
	uint8_t answer[TEST_SMALL_MTU];
	size_t length;
	size_t answer_length;
	unsigned round;
	int result;
	int error;

	/* Requests and answers, a bounded number. */
	btd_sdp_init(client, uuid, 0x0100U);
	result = BTD_SDP_FAILED;
	for (round = 0U; round < 100U; round++) {
		error = btd_sdp_request(client, out, sizeof(out), &length);
		if (error != 0)
			return BTD_SDP_FAILED;
		error = btd_sdps_input(server, out, length, answer, sizeof(answer), &answer_length);
		if (error != 0)
			return BTD_SDP_FAILED;
		result = btd_sdp_input(client, answer, answer_length);
		if (result != BTD_SDP_MORE)
			break;
	}

	/* Succeeded: the last result. */
	return result;
}

/*
 * The server answering one MNS record: searches, the attribute request,
 * and the client reading the record back over an MTU of 48.
 */
static void
test_server(void)
{
	static struct btd_sdps_db db;
	static struct btd_sdps server;
	static struct btd_sdp client;
	static const uint8_t search16[] = { 0x35U, 0x03U, 0x19U, 0x11U, 0x33U, 0x00U, 0x05U, 0x00U };
	static const uint8_t search32[] = { 0x35U, 0x05U, 0x1aU, 0x00U, 0x00U, 0x11U, 0x33U, 0x00U, 0x05U, 0x00U };
	static const uint8_t search128[] = {
		0x35U, 0x11U, 0x1cU, 0x00U, 0x00U, 0x11U, 0x33U, 0x00U, 0x00U, 0x10U, 0x00U,
		0x80U, 0x00U, 0x00U, 0x80U, 0x5fU, 0x9bU, 0x34U, 0xfbU, 0x00U, 0x05U, 0x00U
	};
	static const uint8_t search_two[] = { 0x35U, 0x06U, 0x19U, 0x11U, 0x33U, 0x19U, 0x00U, 0x03U, 0x00U, 0x05U, 0x00U };
	static const uint8_t search_none[] = { 0x35U, 0x06U, 0x19U, 0x11U, 0x33U, 0x19U, 0x12U, 0x34U, 0x00U, 0x05U, 0x00U };
	static const uint8_t attribute[] = { 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x40U, 0x35U, 0x03U, 0x09U, 0x00U, 0x04U, 0x00U };
	uint8_t pairs[256];
	uint8_t pdu[64];
	uint8_t answer[TEST_SMALL_MTU];
	size_t length;
	size_t answer_length;
	uint32_t handle;
	unsigned channel;
	uint16_t version;
	int result;
	int error;

	/* The record: handle 0x00010000. */
	btd_sdps_db_init(&db);
	length = mns_record(pairs, sizeof(pairs));
	error = btd_sdps_register(&db, pairs, length, &handle);
	check(error == 0 && handle == 0x00010000U, "server: registered at 0x00010000");
	check(db.records[0].attributes[0] == 0x09U && db.records[0].attributes[2] == 0x00U, "server: the handle's attribute first");
	btd_sdps_init(&server, &db, TEST_SMALL_MTU);

	/* The search by the 16-bit UUID: one handle, no continuation (03, transaction, 9 bytes: 1, 1, the handle, 0). */
	length = request(pdu, 0x02U, 0x0001U, search16, sizeof(search16));
	error = btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(error == 0 && answer_length == 14U && answer[0] == 0x03U, "server: search answered");
	check(answer[5] == 0x00U && answer[6] == 0x01U && answer[8] == 0x01U, "server: one handle");
	check(answer[9] == 0x00U && answer[10] == 0x01U && answer[11] == 0x00U && answer[12] == 0x00U && answer[13] == 0x00U, "server: the handle and no continuation");

	/* The 32 and 128 bit forms and two UUIDs find it; a UUID it lacks finds none. */
	length = request(pdu, 0x02U, 0x0002U, search32, sizeof(search32));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(answer_length == 14U, "server: 32-bit UUID");
	length = request(pdu, 0x02U, 0x0003U, search128, sizeof(search128));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(answer_length == 14U, "server: 128-bit UUID");
	length = request(pdu, 0x02U, 0x0004U, search_two, sizeof(search_two));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(answer_length == 14U, "server: the class and RFCOMM");
	length = request(pdu, 0x02U, 0x0005U, search_none, sizeof(search_none));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(answer_length == 10U && answer[6] == 0x00U, "server: no match");

	/* The attribute request for 0x0004 alone. */
	length = request(pdu, 0x04U, 0x0006U, attribute, sizeof(attribute));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(answer[0] == 0x05U && answer[7] == 0x36U && answer[10] == 0x09U && answer[12] == 0x04U, "server: attribute 0x0004");

	/* Read back by the client over the small MTU: its channel and its version. */
	result = read_back(&server, &client, 0x1133U);
	check(result == BTD_SDP_DONE, "server: read back whole");
	error = btd_sdp_rfcomm_channel(&client, 0x1133U, 0U, &channel);
	check(error == 0 && channel == 16U, "server: channel 16 read back");
	error = btd_sdp_profile_version(&client, 0x1133U, 0U, 0x1134U, &version);
	check(error == 0 && version == 0x0101U, "server: MAP 1.1 read back");
	check(client.transaction > 0x0101U, "server: continuations were used");

	/* An empty database: no match. */
	btd_sdps_db_init(&db);
	btd_sdps_init(&server, &db, TEST_SMALL_MTU);
	length = request(pdu, 0x02U, 0x0007U, search16, sizeof(search16));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(answer_length == 10U && answer[6] == 0x00U, "server: empty database");
}

/* The server's errors. */
static void
test_errors(void)
{
	static struct btd_sdps_db db;
	static struct btd_sdps server;
	static const uint8_t short_bytes[] = { 0x35U, 0x03U, 0x19U, 0x11U, 0x33U, 0x00U, 0x06U, 0x35U, 0x05U, 0x0aU, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U };
	static const uint8_t all[] = { 0x35U, 0x03U, 0x19U, 0x11U, 0x33U, 0x01U, 0x00U, 0x35U, 0x05U, 0x0aU, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U };
	static const uint8_t no_uuid[] = { 0x35U, 0x00U, 0x00U, 0x05U, 0x00U };
	static const uint8_t long_continuation[] = { 0x35U, 0x03U, 0x19U, 0x11U, 0x33U, 0x00U, 0x05U, 0x11U };
	static const uint8_t bad_handle[] = { 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x40U, 0x35U, 0x03U, 0x09U, 0x00U, 0x04U, 0x00U };
	uint8_t parameters[128];
	uint8_t pairs[256];
	uint8_t pdu[160];
	uint8_t answer[TEST_SMALL_MTU];
	uint8_t continuation[4];
	size_t length;
	size_t answer_length;
	size_t used;
	struct btd_sdp_writer writer;
	uint32_t handle;
	unsigned index;
	int error;

	/* A record, a server. */
	btd_sdps_db_init(&db);
	length = mns_record(pairs, sizeof(pairs));
	(void)btd_sdps_register(&db, pairs, length, &handle);
	btd_sdps_init(&server, &db, TEST_SMALL_MTU);

	/* A parameters' length that is not what follows: 0x0004. */
	length = request(pdu, 0x06U, 0x0001U, all, sizeof(all));
	pdu[4] = (uint8_t)(pdu[4] + 1U);
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(error_code(answer, answer_length) == 0x0004U, "errors: PDU size");

	/* An unknown PDU: 0x0003. */
	length = request(pdu, 0x08U, 0x0002U, all, sizeof(all));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(error_code(answer, answer_length) == 0x0003U, "errors: unknown PDU");

	/* Six bytes asked: 0x0003. */
	length = request(pdu, 0x06U, 0x0003U, short_bytes, sizeof(short_bytes));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(error_code(answer, answer_length) == 0x0003U, "errors: too few bytes asked");

	/* No UUID: 0x0003. */
	length = request(pdu, 0x02U, 0x0004U, no_uuid, sizeof(no_uuid));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(error_code(answer, answer_length) == 0x0003U, "errors: no UUID");

	/* Thirteen UUIDs: 0x0003. */
	parameters[0] = 0x35U;
	parameters[1] = 13U * 3U;
	used = 2U;
	for (index = 0U; index < 13U; index++) {
		parameters[used] = 0x19U;
		parameters[used + 1U] = 0x11U;
		parameters[used + 2U] = 0x33U;
		used += 3U;
	}

	/* The most records and no continuation state. */
	parameters[used] = 0x00U;
	parameters[used + 1U] = 0x05U;
	parameters[used + 2U] = 0x00U;
	used += 3U;
	length = request(pdu, 0x02U, 0x0005U, parameters, used);
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(error_code(answer, answer_length) == 0x0003U, "errors: thirteen UUIDs");

	/* A continuation state of 17 bytes: 0x0005. */
	memcpy(parameters, long_continuation, sizeof(long_continuation));
	memset(parameters + sizeof(long_continuation), 0, 17U);
	length = request(pdu, 0x02U, 0x0006U, parameters, sizeof(long_continuation) + 17U);
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(error_code(answer, answer_length) == 0x0005U, "errors: continuation too long");

	/* A handle not offered: 0x0002. */
	length = request(pdu, 0x04U, 0x0007U, bad_handle, sizeof(bad_handle));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(error_code(answer, answer_length) == 0x0002U, "errors: handle not offered");

	/* A request cut by the MTU: its continuation state, then the same with it is answered. */
	length = request(pdu, 0x06U, 0x0008U, all, sizeof(all));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(answer[0] == 0x07U && answer[answer_length - 4U] == 0x03U, "errors: an answer with a continuation");
	memcpy(continuation, answer + answer_length - 4U, sizeof(continuation));
	memcpy(parameters, all, sizeof(all) - 1U);
	memcpy(parameters + sizeof(all) - 1U, continuation, sizeof(continuation));
	length = request(pdu, 0x06U, 0x0009U, parameters, sizeof(all) - 1U + sizeof(continuation));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(answer[0] == 0x07U, "errors: the continuation answered");

	/* The same state with another request (another class): 0x0005. */
	parameters[4] = 0x34U;
	length = request(pdu, 0x06U, 0x000aU, parameters, sizeof(all) - 1U + sizeof(continuation));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(error_code(answer, answer_length) == 0x0005U, "errors: continuation of another request");

	/* The records changed since: 0x0005. */
	parameters[4] = 0x33U;
	length = request(pdu, 0x06U, 0x000bU, all, sizeof(all));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	memcpy(continuation, answer + answer_length - 4U, sizeof(continuation));
	memcpy(parameters + sizeof(all) - 1U, continuation, sizeof(continuation));
	(void)btd_sdps_register(&db, pairs, mns_record(pairs, sizeof(pairs)), &handle);
	length = request(pdu, 0x06U, 0x000cU, parameters, sizeof(all) - 1U + sizeof(continuation));
	(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
	check(error_code(answer, answer_length) == 0x0005U, "errors: continuation of older records");

	/* Pairs out of order are not registered. */
	btd_sdp_writer_init(&writer, pairs, sizeof(pairs));
	btd_sdp_put_uint16(&writer, 0x0009U);
	btd_sdp_put_uint8(&writer, 1U);
	btd_sdp_put_uint16(&writer, 0x0001U);
	btd_sdp_put_uint8(&writer, 1U);
	error = btd_sdps_register(&db, pairs, writer.used, &handle);
	check(error == EINVAL, "errors: pairs out of order");
}

/* The client reading two MAS records of a phone: the count, each one's channel, the version and MAP's numbers. */
static void
test_client(void)
{
	static struct btd_sdps_db db;
	static struct btd_sdps server;
	static struct btd_sdp client;
	uint8_t pairs[256];
	size_t length;
	uint32_t handle;
	uint32_t value;
	unsigned channel;
	unsigned count;
	uint16_t version;
	int result;
	int error;

	/* Two instances, on channels 5 and 6. */
	btd_sdps_db_init(&db);
	length = mas_record(pairs, sizeof(pairs), 5U, 0U);
	error = btd_sdps_register(&db, pairs, length, &handle);
	check(error == 0, "client: MAS 0 registered");
	length = mas_record(pairs, sizeof(pairs), 6U, 1U);
	error = btd_sdps_register(&db, pairs, length, &handle);
	check(error == 0, "client: MAS 1 registered");
	btd_sdps_init(&server, &db, TEST_SMALL_MTU);

	/* Read back. */
	result = read_back(&server, &client, BTD_SDP_UUID_MAS);
	check(result == BTD_SDP_DONE, "client: read back");
	count = btd_sdp_records(&client, BTD_SDP_UUID_MAS);
	check(count == 2U, "client: two MAS");

	/* The second instance: channel 6, instance 1, SMS GSM and CDMA, the features. */
	error = btd_sdp_rfcomm_channel(&client, BTD_SDP_UUID_MAS, 1U, &channel);
	check(error == 0 && channel == 6U, "client: second channel");
	error = btd_sdp_uint_attribute(&client, BTD_SDP_UUID_MAS, 1U, TEST_MAS_INSTANCE, &value);
	check(error == 0 && value == 1U, "client: second instance");
	error = btd_sdp_uint_attribute(&client, BTD_SDP_UUID_MAS, 0U, TEST_MESSAGE_TYPES, &value);
	check(error == 0 && value == 0x06U, "client: message types");
	error = btd_sdp_uint_attribute(&client, BTD_SDP_UUID_MAS, 0U, TEST_MAP_FEATURES, &value);
	check(error == 0 && value == 0x7fU, "client: features");
	error = btd_sdp_profile_version(&client, BTD_SDP_UUID_MAS, 0U, 0x1134U, &version);
	check(error == 0 && version == 0x0104U, "client: MAP 1.4");

	/* A third instance and an attribute it lacks are not there. */
	error = btd_sdp_rfcomm_channel(&client, BTD_SDP_UUID_MAS, 2U, &channel);
	check(error == ENOENT, "client: no third");
	error = btd_sdp_uint_attribute(&client, BTD_SDP_UUID_MAS, 0U, 0x0777U, &value);
	check(error == ENOENT, "client: no such attribute");
}

/* Random requests into the server (a fixed seed): nothing crashes, and every answer fits the MTU. */
static void
test_fuzz(void)
{
	static struct btd_sdps_db db;
	static struct btd_sdps server;
	uint8_t pairs[256];
	uint8_t pdu[128];
	uint8_t answer[TEST_SMALL_MTU];
	size_t length;
	size_t answer_length;
	size_t index;
	uint32_t handle;
	unsigned round;
	unsigned seed;
	int fits;

	/* A record and a server. */
	btd_sdps_db_init(&db);
	length = mns_record(pairs, sizeof(pairs));
	(void)btd_sdps_register(&db, pairs, length, &handle);
	btd_sdps_init(&server, &db, TEST_SMALL_MTU);

	/* Random PDUs, often with a valid header. */
	seed = 0x5d95eedU;
	fits = 1;
	for (round = 0U; round < TEST_FUZZ_ROUNDS; round++) {
		seed = seed * 1103515245U + 12345U;
		length = (size_t)((seed >> 16) % sizeof(pdu)) + 1U;
		for (index = 0U; index < length; index++) {
			seed = seed * 1103515245U + 12345U;
			pdu[index] = (uint8_t)(seed >> 16);
		}

		/* Often a known PDU with a matching length. */
		if ((seed & 0x300U) != 0U && length >= 5U) {
			pdu[0] = (uint8_t)(2U + 2U * ((seed >> 10) % 3U));
			pdu[3] = (uint8_t)((length - 5U) >> 8);
			pdu[4] = (uint8_t)((length - 5U) & 0xffU);
		}

		/* Answered within the MTU. */
		(void)btd_sdps_input(&server, pdu, length, answer, sizeof(answer), &answer_length);
		if (answer_length > TEST_SMALL_MTU)
			fits = 0;
	}

	/* Every answer fitted. */
	check(fits, "fuzz: answers within the MTU");
}
