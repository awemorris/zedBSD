/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of bluetoothd's HID over GATT client (ws143-p005 i03,
 * plan/ws143/phase005/phase.md section 7.4): hog.c and att.c against a
 * GATT server played here from an attribute table (the loopback's HOG
 * mouse of section 6, and its variants): the whole discovery with the map
 * over Read Blob, the PnP numbers, the report protocol's command, the
 * notifications turned on, the battery; notifications before the device
 * is open kept and passed on with their report ID, after it passed at
 * once; a map that is a whole number of reads (an empty part, or Attribute
 * Not Long); report IDs of 0; no Protocol Mode; no HID service; a map that
 * needs encryption (security); a changed service (its indication
 * confirmed); 70 characteristics (64 kept); a server that does not answer
 * (timeout); a larger MTU.
 * usage: bt-hog-host-test
 */

#include "userland/base/bluetoothd/att.h"
#include "userland/base/bluetoothd/hog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most attributes a table holds, and the longest value. */
#define TABLE_MAX	160U
#define VALUE_MAX	128U

/* One attribute: its handle, its type (16-bit UUID), its group's end (a service), its value, and whether reading it needs encryption. */
struct attribute {
	uint16_t handle;
	uint16_t type;
	uint16_t end;
	uint8_t value[VALUE_MAX];
	size_t length;
	int encrypted_only;
};

/*
 * The server: its attributes, its MTU, how it ends a map read at its end
 * (an empty part, else Attribute Not Long), whether it answers at all,
 * and what it saw (the protocol mode written, the CCCs written).
 */
struct server {
	struct attribute attributes[TABLE_MAX];
	unsigned count;
	uint16_t mtu;
	int not_long;
	int silent;
	uint8_t protocol_written;
	unsigned ccc_written;
	unsigned requests;
};

/* One run: the client, what it set up, the actions seen. */
struct hog_run {
	struct btd_hog hog;
	int setup;
	int open;
	int failed;
};

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* A boot mouse's report map with report ID 1 (HID 1.11 Appendix E.10 and an ID), 52 bytes. */
static const uint8_t mouse_map[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01,
	0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
	0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02,
	0x81, 0x06, 0xc0, 0xc0,
};

static void check(int condition, const char *what);
static void server_mouse(struct server *server, int with_protocol, uint8_t report_id, size_t map_size, int map_encrypted);
static void server_add(struct server *server, uint16_t handle, uint16_t type, const uint8_t *value, size_t length);
static void server_service(struct server *server, uint16_t handle, uint16_t end, uint16_t uuid);
static void server_characteristic(struct server *server, uint16_t handle, uint8_t properties, uint16_t uuid, const uint8_t *value, size_t length);
static size_t server_answer(struct server *server, const uint8_t *request, size_t length, uint8_t *out);
static size_t server_error(uint8_t *out, uint8_t request, uint16_t handle, uint8_t code);
static unsigned run(struct hog_run *run_state, struct server *server);
static void test_whole(void);
static void test_map_ends(void);
static void test_variants(void);
static void test_failures(void);

/*
 * Runs every part; the exit status says whether every check held.
 */
int
main(void)
{
	/* The parts. */
	test_whole();
	test_map_ends();
	test_variants();
	test_failures();

	/* The verdict. */
	if (failures != 0U) {
		printf("bt-hog-host-test: FAIL (%u of %u checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("bt-hog-host-test: PASS (%u checks)\n", checks);
	return 0;
}

/* Counts a check, and prints it when it failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted. */
	checks++;
	if (condition)
		return;

	/* A failure, told. */
	failures++;
	fprintf(stderr, "FAIL: %s\n", what);
}

/*
 * Builds the loopback's HOG mouse (phase005 section 6): GAP, the HID
 * service (information, map, one input report with its CCC and
 * reference, protocol mode, control point), Battery (level 80 with its
 * CCC), Device Information (PnP ID 1, 0x1209, 0x4842, 0x0100).
 */
static void
server_mouse(
	struct server *server,
	int with_protocol,
	uint8_t report_id,
	size_t map_size,
	int map_encrypted)
{
	static const uint8_t name[] = "HOG Mouse";
	static const uint8_t information[4] = { 0x11U, 0x01U, 0x00U, 0x02U };
	static const uint8_t level[1] = { 80U };
	static const uint8_t pnp[7] = { 0x01U, 0x09U, 0x12U, 0x42U, 0x48U, 0x00U, 0x01U };
	static const uint8_t ccc[2] = { 0x00U, 0x00U };
	static const uint8_t mode[1] = { 0x01U };
	static uint8_t map[VALUE_MAX];
	uint8_t reference[2];
	unsigned index;

	/* GAP. */
	memset(server, 0, sizeof(*server));
	server->mtu = BTD_ATT_MTU_DEFAULT;
	server_service(server, 0x0001U, 0x0005U, 0x1800U);
	server_characteristic(server, 0x0002U, 0x02U, 0x2a00U, name, sizeof(name) - 1U);

	/* The HID service. */
	server_service(server, 0x0010U, 0x0020U, 0x1812U);
	server_characteristic(server, 0x0011U, 0x02U, 0x2a4aU, information, sizeof(information));
	for (index = 0U; index < map_size; index++)
		map[index] = mouse_map[index % sizeof(mouse_map)];
	server_characteristic(server, 0x0013U, 0x02U, 0x2a4bU, map, map_size);
	server->attributes[server->count - 1U].encrypted_only = map_encrypted;
	server_characteristic(server, 0x0015U, 0x12U, 0x2a4dU, NULL, 0U);
	server_add(server, 0x0017U, 0x2902U, ccc, sizeof(ccc));
	reference[0] = report_id;
	reference[1] = 0x01U;
	server_add(server, 0x0018U, 0x2908U, reference, sizeof(reference));
	if (with_protocol)
		server_characteristic(server, 0x0019U, 0x06U, 0x2a4eU, mode, sizeof(mode));
	server_characteristic(server, 0x001bU, 0x04U, 0x2a4cU, NULL, 0U);

	/* Battery. */
	server_service(server, 0x0030U, 0x0033U, 0x180fU);
	server_characteristic(server, 0x0031U, 0x12U, 0x2a19U, level, sizeof(level));
	server_add(server, 0x0033U, 0x2902U, ccc, sizeof(ccc));

	/* Device Information. */
	server_service(server, 0x0040U, 0x0042U, 0x180aU);
	server_characteristic(server, 0x0041U, 0x02U, 0x2a50U, pnp, sizeof(pnp));
}

/* Adds an attribute. */
static void
server_add(
	struct server *server,
	uint16_t handle,
	uint16_t type,
	const uint8_t *value,
	size_t length)
{
	struct attribute *attribute;

	/* Past the table is a mistake of the test. */
	if (server->count >= TABLE_MAX || length > VALUE_MAX) {
		fprintf(stderr, "bt-hog-host-test: a table too big\n");
		exit(2);
	}

	/* The attribute, in handle order (the test adds them so). */
	attribute = &server->attributes[server->count];
	memset(attribute, 0, sizeof(*attribute));
	attribute->handle = handle;
	attribute->type = type;
	if (length != 0U)
		memcpy(attribute->value, value, length);
	attribute->length = length;
	server->count++;
}

/* Adds a primary service's declaration and its group's end. */
static void
server_service(
	struct server *server,
	uint16_t handle,
	uint16_t end,
	uint16_t uuid)
{
	uint8_t value[2];

	/* The service's UUID as its value. */
	value[0] = (uint8_t)uuid;
	value[1] = (uint8_t)(uuid >> 8);
	server_add(server, handle, 0x2800U, value, sizeof(value));
	server->attributes[server->count - 1U].end = end;
}

/* Adds a characteristic: its declaration at handle, its value at handle + 1. */
static void
server_characteristic(
	struct server *server,
	uint16_t handle,
	uint8_t properties,
	uint16_t uuid,
	const uint8_t *value,
	size_t length)
{
	uint8_t declaration[5];

	/* The declaration: properties, the value's handle, the UUID. */
	declaration[0] = properties;
	declaration[1] = (uint8_t)(handle + 1U);
	declaration[2] = (uint8_t)((handle + 1U) >> 8);
	declaration[3] = (uint8_t)uuid;
	declaration[4] = (uint8_t)(uuid >> 8);
	server_add(server, handle, 0x2803U, declaration, sizeof(declaration));

	/* The value. */
	server_add(server, (uint16_t)(handle + 1U), uuid, value, length);
}

/* Answers one request as a GATT server; returns the response's length (0: none, a command). */
static size_t
server_answer(
	struct server *server,
	const uint8_t *request,
	size_t length,
	uint8_t *out)
{
	const struct attribute *attribute;
	struct btd_att_pdu parsed;
	unsigned index;
	size_t used;
	size_t room;
	size_t element;
	int error;

	/* A request of a known form. */
	server->requests++;
	error = btd_att_parse(request, length, &parsed);
	if (error != 0)
		return server_error(out, request[0], 0U, BTD_ATT_INVALID_PDU);

	/* Each request. */
	room = (size_t)server->mtu;
	switch (parsed.opcode) {
	case BTD_ATT_MTU_REQUEST:
		out[0] = BTD_ATT_MTU_RESPONSE;
		out[1] = (uint8_t)server->mtu;
		out[2] = (uint8_t)(server->mtu >> 8);
		return 3U;
	case BTD_ATT_READ_GROUP_REQUEST:
	case BTD_ATT_READ_BY_TYPE_REQUEST:
		/* The attributes of the type in the range, as many of one length as fit. */
		out[0] = (uint8_t)(parsed.opcode + 1U);
		used = 2U;
		element = 0U;
		for (index = 0U; index < server->count; index++) {
			attribute = &server->attributes[index];
			if (attribute->handle < parsed.handle || attribute->handle > parsed.end || attribute->type != parsed.uuid)
				continue;
			if (element == 0U) {
				element = 2U + attribute->length;
				if (parsed.opcode == BTD_ATT_READ_GROUP_REQUEST)
					element += 2U;
			}

			/* As many as fit. */
			if (used + element > room)
				break;
			out[used] = (uint8_t)attribute->handle;
			out[used + 1U] = (uint8_t)(attribute->handle >> 8);
			used += 2U;
			if (parsed.opcode == BTD_ATT_READ_GROUP_REQUEST) {
				out[used] = (uint8_t)attribute->end;
				out[used + 1U] = (uint8_t)(attribute->end >> 8);
				used += 2U;
			}

			/* The value. */
			memcpy(out + used, attribute->value, attribute->length);
			used += attribute->length;
		}

		/* None in the range. */
		if (element == 0U)
			return server_error(out, parsed.opcode, parsed.handle, BTD_ATT_NOT_FOUND);
		out[1] = (uint8_t)element;
		return used;
	case BTD_ATT_FIND_INFO_REQUEST:
		/* Every attribute in the range, handle and 16-bit type. */
		out[0] = BTD_ATT_FIND_INFO_RESPONSE;
		out[1] = BTD_ATT_FORMAT_16;
		used = 2U;
		for (index = 0U; index < server->count; index++) {
			attribute = &server->attributes[index];
			if (attribute->handle < parsed.handle || attribute->handle > parsed.end)
				continue;
			if (used + 4U > room)
				break;
			out[used] = (uint8_t)attribute->handle;
			out[used + 1U] = (uint8_t)(attribute->handle >> 8);
			out[used + 2U] = (uint8_t)attribute->type;
			out[used + 3U] = (uint8_t)(attribute->type >> 8);
			used += 4U;
		}

		/* None in the range. */
		if (used == 2U)
			return server_error(out, parsed.opcode, parsed.handle, BTD_ATT_NOT_FOUND);
		return used;
	case BTD_ATT_READ_REQUEST:
	case BTD_ATT_READ_BLOB_REQUEST:
		/* The value from the offset, as much as fits. */
		for (index = 0U; index < server->count; index++) {
			attribute = &server->attributes[index];
			if (attribute->handle != parsed.handle)
				continue;
			if (attribute->encrypted_only)
				return server_error(out, parsed.opcode, parsed.handle, BTD_ATT_INSUFFICIENT_ENCRYPTION);
			if (parsed.offset > attribute->length)
				return server_error(out, parsed.opcode, parsed.handle, BTD_ATT_INVALID_OFFSET);
			if (parsed.offset == attribute->length && parsed.offset != 0U && server->not_long)
				return server_error(out, parsed.opcode, parsed.handle, BTD_ATT_NOT_LONG);
			used = attribute->length - parsed.offset;
			if (used > room - 1U)
				used = room - 1U;
			out[0] = (uint8_t)(parsed.opcode + 1U);
			memcpy(out + 1, attribute->value + parsed.offset, used);
			return 1U + used;
		}

		/* No such handle. */
		return server_error(out, parsed.opcode, parsed.handle, BTD_ATT_INVALID_HANDLE);
	case BTD_ATT_WRITE_REQUEST:
		/* A CCC written. */
		if (parsed.length == 2U && parsed.value[0] == 0x01U)
			server->ccc_written++;
		out[0] = BTD_ATT_WRITE_RESPONSE;
		return 1U;
	case BTD_ATT_WRITE_COMMAND:
		/* The protocol mode, no answer. */
		if (parsed.length == 1U)
			server->protocol_written = parsed.value[0];
		return 0U;
	default:
		break;
	}

	/* Anything else is not supported. */
	return server_error(out, parsed.opcode, parsed.handle, BTD_ATT_REQUEST_NOT_SUPPORTED);
}

/* Writes an Error Response. */
static size_t
server_error(
	uint8_t *out,
	uint8_t request,
	uint16_t handle,
	uint8_t code)
{
	/* The request, the handle, the code. */
	return btd_att_build_error(out, BTD_ATT_MTU, request, handle, code);
}

/*
 * Runs a discovery against a server: each request answered, the setup
 * done (resume) when asked, a notification of the report (00 05 00) sent
 * before the setup to wait.  Returns the last actions.
 */
static unsigned
run(
	struct hog_run *run_state,
	struct server *server)
{
	static const uint8_t notification[6] = { BTD_ATT_NOTIFICATION, 0x16U, 0x00U, 0x00U, 0x05U, 0x00U };
	uint8_t answer[BTD_ATT_MTU];
	size_t length;
	unsigned actions;
	unsigned rounds;
	int sent_early;

	/* The start. */
	memset(run_state, 0, sizeof(*run_state));
	btd_hog_init(&run_state->hog);
	actions = btd_hog_start(&run_state->hog, 1000U);
	sent_early = 0;

	/* Until open, failed or silent, 500 rounds at most. */
	for (rounds = 0U; rounds < 500U; rounds++) {
		/* A failure, or open. */
		if ((actions & BTD_HOG_FAILED) != 0U) {
			run_state->failed = 1;
			return actions;
		}

		/* Open. */
		if ((actions & BTD_HOG_OPEN) != 0U) {
			run_state->open = 1;
			return actions;
		}

		/* A notification once the map is being read (before the device is made). */
		if (!sent_early && run_state->hog.report_map != 0U) {
			(void)btd_hog_input(&run_state->hog, notification, sizeof(notification), 1000U);
			sent_early = 1;
		}

		/* The setup, then the rest. */
		if ((actions & BTD_HOG_SETUP) != 0U) {
			if ((actions & BTD_HOG_SEND) != 0U)
				(void)server_answer(server, run_state->hog.out, run_state->hog.out_length, answer);
			run_state->setup = 1;
			actions = btd_hog_resume(&run_state->hog, 1000U);
			continue;
		}

		/* A request answered (or not, by a silent server). */
		if ((actions & BTD_HOG_SEND) == 0U || server->silent)
			return actions;
		length = server_answer(server, run_state->hog.out, run_state->hog.out_length, answer);
		if (length == 0U)
			return actions;
		actions = btd_hog_input(&run_state->hog, answer, length, 1000U);
	}

	/* A discovery that did not end. */
	return actions;
}

/* The whole discovery of the loopback's mouse, with MTU 23 (the map over Read Blob). */
static void
test_whole(void)
{
	static const uint8_t notification[6] = { BTD_ATT_NOTIFICATION, 0x16U, 0x00U, 0x00U, 0x07U, 0x00U };
	static const uint8_t battery[4] = { BTD_ATT_NOTIFICATION, 0x32U, 0x00U, 0x4fU };
	static const uint8_t stranger[4] = { BTD_ATT_NOTIFICATION, 0x99U, 0x00U, 0x01U };
	static struct server server;
	static struct hog_run state;
	unsigned actions;
	int next;

	/* The mouse. */
	server_mouse(&server, 1, 0x01U, sizeof(mouse_map), 0);
	actions = run(&state, &server);
	check(state.open && state.setup, "whole: set up and open");
	check(state.hog.map_size == sizeof(mouse_map) && memcmp(state.hog.map, mouse_map, sizeof(mouse_map)) == 0, "whole: the map over Read Blob");
	check(state.hog.vendor == 0x1209U && state.hog.product == 0x4842U && state.hog.version == 0x0100U, "whole: the PnP numbers");
	check(server.protocol_written == 0x01U, "whole: the report protocol asked for");
	check(server.ccc_written == 2U, "whole: the report's and the battery's notifications on");
	check(state.hog.battery == 80, "whole: the battery read");
	check(state.hog.uses_ids == 1 && state.hog.report_count == 1U && state.hog.reports[0].id == 1U, "whole: the report ID from the reference");
	check(state.hog.mtu == BTD_ATT_MTU_DEFAULT && actions == BTD_HOG_OPEN, "whole: MTU 23");

	/* The notification that came before: passed on now with its ID. */
	next = btd_hog_next(&state.hog);
	check(next == 1 && state.hog.report_length == 4U && state.hog.report[0] == 1U && state.hog.report[2] == 0x05U, "whole: the early notification passed on");
	next = btd_hog_next(&state.hog);
	check(next == 0, "whole: one waited");

	/* After: at once; the battery's updates the level; a stranger's is counted. */
	actions = btd_hog_input(&state.hog, notification, sizeof(notification), 2000U);
	check(actions == BTD_HOG_REPORT && state.hog.report_length == 4U && state.hog.report[2] == 0x07U, "whole: a report at once");
	actions = btd_hog_input(&state.hog, battery, sizeof(battery), 2000U);
	check(actions == 0U && state.hog.battery == 0x4f, "whole: the battery's notification");
	actions = btd_hog_input(&state.hog, stranger, sizeof(stranger), 2000U);
	check(actions == 0U && state.hog.unknown == 1U, "whole: a notification of nothing known");
}

/* A map of a whole number of reads: ended by an empty part, or by Attribute Not Long. */
static void
test_map_ends(void)
{
	static struct server server;
	static struct hog_run state;

	/* 66 bytes = 3 x 22: the fourth read is empty. */
	server_mouse(&server, 1, 0x01U, 66U, 0);
	(void)run(&state, &server);
	check(state.open && state.hog.map_size == 66U, "map: ended by an empty part");

	/* The same, the server answering Attribute Not Long. */
	server_mouse(&server, 1, 0x01U, 66U, 0);
	server.not_long = 1;
	(void)run(&state, &server);
	check(state.open && state.hog.map_size == 66U, "map: ended by Attribute Not Long");

	/* A larger MTU: one read. */
	server_mouse(&server, 1, 0x01U, sizeof(mouse_map), 0);
	server.mtu = 100U;
	(void)run(&state, &server);
	check(state.open && state.hog.mtu == 100U && state.hog.map_size == sizeof(mouse_map), "map: one read with MTU 100");
}

/* Report IDs of 0, no Protocol Mode, 70 characteristics. */
static void
test_variants(void)
{
	static struct server server;
	static struct hog_run state;
	unsigned index;
	int next;

	/* Report IDs of 0: no ID put first. */
	server_mouse(&server, 1, 0x00U, sizeof(mouse_map), 0);
	(void)run(&state, &server);
	next = btd_hog_next(&state.hog);
	check(state.open && state.hog.uses_ids == 0 && next == 1 && state.hog.report_length == 3U && state.hog.report[1] == 0x05U, "variants: no report ID");

	/* No Protocol Mode: no command. */
	server_mouse(&server, 0, 0x01U, sizeof(mouse_map), 0);
	(void)run(&state, &server);
	check(state.open && server.protocol_written == 0U, "variants: no Protocol Mode");

	/* 70 characteristics more in the HID service (after the ones that matter): 64 kept, open all the same. */
	server_mouse(&server, 1, 0x01U, sizeof(mouse_map), 0);
	server.count -= 7U;
	server.attributes[3].end = 0x00ffU;
	for (index = 0U; index < 70U; index++)
		server_characteristic(&server, (uint16_t)(0x0020U + 2U * index), 0x02U, 0x2a4aU, NULL, 0U);
	server_service(&server, 0x0130U, 0x0133U, 0x180fU);
	server_characteristic(&server, 0x0131U, 0x12U, 0x2a19U, (const uint8_t *)"\x50", 1U);
	(void)run(&state, &server);
	check(state.open && state.hog.hid_characteristics == BTD_HOG_HID_CHARACTERISTICS && state.hog.battery == 80, "variants: 64 of 76 characteristics kept");
}

/* No HID service, a map that needs encryption, a changed service, a silent server. */
static void
test_failures(void)
{
	static const uint8_t changed[7] = { BTD_ATT_INDICATION, 0x52U, 0x00U, 0x01U, 0x00U, 0xffU, 0xffU };
	static struct server server;
	static struct hog_run state;
	unsigned actions;

	/* No HID service. */
	server_mouse(&server, 1, 0x01U, sizeof(mouse_map), 0);
	server.attributes[3].value[0] = 0x34U;
	server.attributes[3].value[1] = 0x12U;
	(void)run(&state, &server);
	check(state.failed && state.hog.why != NULL && strcmp(state.hog.why, "no-hid") == 0, "failures: no HID service");

	/* The map needs encryption the link does not have. */
	server_mouse(&server, 1, 0x01U, sizeof(mouse_map), 1);
	(void)run(&state, &server);
	check(state.failed && state.hog.why != NULL && strcmp(state.hog.why, "security") == 0, "failures: security");

	/* A changed service: confirmed, and the connection to end. */
	server_mouse(&server, 1, 0x01U, sizeof(mouse_map), 0);
	server_service(&server, 0x0050U, 0x0053U, 0x1801U);
	server_characteristic(&server, 0x0051U, 0x20U, 0x2a05U, NULL, 0U);
	(void)run(&state, &server);
	actions = btd_hog_input(&state.hog, changed, sizeof(changed), 2000U);
	check(state.open && (actions & BTD_HOG_SEND) != 0U && state.hog.out[0] == BTD_ATT_CONFIRMATION && (actions & BTD_HOG_FAILED) != 0U &&
		      strcmp(state.hog.why, "changed") == 0,
	      "failures: a changed service");

	/* A silent server: its request's time runs out. */
	server_mouse(&server, 1, 0x01U, sizeof(mouse_map), 0);
	server.silent = 1;
	(void)run(&state, &server);
	actions = btd_hog_tick(&state.hog, 1000U + BTD_HOG_REQUEST_MS - 1U);
	check(actions == 0U && btd_hog_deadline(&state.hog) == 1000U + BTD_HOG_REQUEST_MS, "failures: waiting for the answer");
	actions = btd_hog_tick(&state.hog, 1000U + BTD_HOG_REQUEST_MS);
	check(actions == BTD_HOG_FAILED && strcmp(state.hog.why, "timeout") == 0, "failures: timeout");
}
