/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of networkd's request-reading rules (request-frame.c): when the
 * bytes of one connection make a whole request, and which requests have
 * their fields decoded, as both of the daemon's reading paths apply them.
 *
 *   cc -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -I. \
 *      userland/base/networkd/request-frame.c userland/base/net/protocol.c \
 *      tests/networkd-request-host-test.c -o build/tmp/networkd-request-host-test
 *
 * It prints one line a case and "networkd-request-host-test: PASS" at the end.
 */

#include "userland/base/networkd/request-frame.h"
#include "userland/base/net/protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* One case: the bytes, whether the client has closed, and what must be told. */
struct frame_case {
	const char *name;
	const char *bytes;

	/* How many of the bytes were received; 0 means the whole string. */
	size_t used;
	int closed;
	enum networkd_request_frame_state state;
	int error;
	uint32_t opcode;
	size_t payload;
};

static int run_case(const struct frame_case *item);
static int frame_bytes(char *buffer, size_t capacity, uint32_t id, uint32_t opcode, const char *payload, size_t *length);
static int check_split_subscribe(void);
static int check_lan_enable_fields(void);

/*
 * The cases of hand-written frames.
 *
 * Each header is "ZNV2 <id> <opcode> <payload length>\n"; SUBSCRIBE is 64,
 * SHOW 1, UP 2 and LAN_ENABLE 48.
 */
static const struct frame_case frame_cases[] = {
	{ "subscribe whole without close", "ZNV2 7 64 0\n", 0U, 0,
	    NETWORKD_REQUEST_FRAME_COMPLETE, 0, 64U, 0U },
	{ "subscribe whole with close", "ZNV2 7 64 0\n", 0U, 1,
	    NETWORKD_REQUEST_FRAME_COMPLETE, 0, 64U, 0U },
	{ "show waits for the close", "ZNV2 3 1 0\n", 0U, 0,
	    NETWORKD_REQUEST_FRAME_PARTIAL, 0, 1U, 0U },
	{ "show whole with close", "ZNV2 3 1 0\n", 0U, 1,
	    NETWORKD_REQUEST_FRAME_COMPLETE, 0, 1U, 0U },
	{ "header line still coming", "ZNV2 3 1", 0U, 0,
	    NETWORKD_REQUEST_FRAME_PARTIAL, 0, 0U, 0U },
	{ "header line cut by the close", "ZNV2 3 1", 0U, 1,
	    NETWORKD_REQUEST_FRAME_INVALID, EINVAL, 0U, 0U },
	{ "header line too long", "ZNV2 3 1 0                                 \n", 0U, 0,
	    NETWORKD_REQUEST_FRAME_INVALID, EINVAL, 0U, 0U },
	{ "bad magic", "ZNV9 3 1 0\n", 0U, 1,
	    NETWORKD_REQUEST_FRAME_INVALID, EINVAL, 0U, 0U },
	{ "payload still coming", "ZNV2 3 2 6\n\001\000\000", 14U, 0,
	    NETWORKD_REQUEST_FRAME_PARTIAL, 0, 2U, 6U },
	{ "payload cut by the close", "ZNV2 3 2 6\nabc", 0U, 1,
	    NETWORKD_REQUEST_FRAME_INVALID, EMSGSIZE, 2U, 6U },
	{ "bytes after a subscription", "ZNV2 7 64 0\nx", 0U, 0,
	    NETWORKD_REQUEST_FRAME_INVALID, EMSGSIZE, 64U, 0U },
	{ "payload over the request limit", "ZNV2 3 2 5000\n", 0U, 0,
	    NETWORKD_REQUEST_FRAME_INVALID, EMSGSIZE, 2U, 5000U },
};

/*
 * Runs every case and reports PASS or the first failure.
 */
int
main(
	void)
{
	size_t index;
	int failed;

	failed = 0;

	/* Checks each hand-written frame. */
	for (index = 0; index < sizeof(frame_cases) / sizeof(frame_cases[0]); index++) {
		if (run_case(&frame_cases[index]) != 0)
			failed = 1;
	}

	/* Checks a subscription that arrives in pieces. */
	if (check_split_subscribe() != 0)
		failed = 1;

	/* Checks which requests have their fields decoded. */
	if (check_lan_enable_fields() != 0)
		failed = 1;

	/* Reports the outcome. */
	if (failed) {
		printf("networkd-request-host-test: FAIL\n");
		return 1;
	}

	/* Succeeded: every case told what it should. */
	printf("networkd-request-host-test: PASS\n");
	return 0;
}

/* Parses one case's bytes and compares what is told with what is expected. */
static int
run_case(
	const struct frame_case *item)
{
	struct networkd_protocol_header header;
	enum networkd_request_frame_state state;
	size_t header_length;
	size_t length;
	int error;

	/* Takes the received length, given by hand where the bytes hold a NUL. */
	memset(&header, 0, sizeof(header));
	length = item->used;
	if (length == 0U)
		length = strlen(item->bytes);

	/* Parses the bytes. */
	state = networkd_request_frame_parse((const unsigned char *)item->bytes,
	    length, item->closed, &header, &header_length, &error);

	/* Compares the state and the error. */
	if (state != item->state || error != item->error) {
		printf("FAIL %s: state %d error %d (want %d %d)\n", item->name,
		    (int)state, error, (int)item->state, item->error);
		return 1;
	}

	/* A decoded header names the opcode and payload expected. */
	if (item->opcode != 0U &&
	    (header.opcode != item->opcode ||
	     header.payload_length != item->payload)) {
		printf("FAIL %s: opcode %u payload %zu (want %u %zu)\n",
		    item->name, (unsigned)header.opcode, header.payload_length,
		    (unsigned)item->opcode, item->payload);
		return 1;
	}

	/* Succeeded. */
	printf("ok %s\n", item->name);
	return 0;
}

/* Builds one frame the way the client library writes it. */
static int
frame_bytes(
	char *buffer,
	size_t capacity,
	uint32_t id,
	uint32_t opcode,
	const char *payload,
	size_t *length)
{
	int count;

	/* Writes the header line and the payload after it. */
	count = snprintf(buffer, capacity, "ZNV2 %u %u %zu\n", (unsigned)id,
	    (unsigned)opcode, strlen(payload));
	if (count < 0 || (size_t)count + strlen(payload) >= capacity)
		return -1;
	memcpy(buffer + count, payload, strlen(payload));
	*length = (size_t)count + strlen(payload);

	/* Succeeded. */
	return 0;
}

/*
 * Feeds a subscription one byte at a time without a close.
 *
 * Every prefix but the whole frame must wait, and the whole frame must be
 * taken at once: a watcher never closes its direction.
 */
static int
check_split_subscribe(
	void)
{
	struct networkd_protocol_header header;
	enum networkd_request_frame_state state;
	char buffer[64];
	size_t header_length;
	size_t length;
	size_t used;
	int error;

	/* Builds the subscription frame. */
	if (frame_bytes(buffer, sizeof(buffer), 9U, NETWORKD_OP_SUBSCRIBE, "",
	    &length) != 0)
		return 1;

	/* Checks every prefix. */
	for (used = 1; used <= length; used++) {
		state = networkd_request_frame_parse((const unsigned char *)buffer,
		    used, 0, &header, &header_length, &error);
		if (used < length && state != NETWORKD_REQUEST_FRAME_PARTIAL) {
			printf("FAIL split subscribe: prefix %zu state %d\n", used,
			    (int)state);
			return 1;
		}
		if (used == length &&
		    (state != NETWORKD_REQUEST_FRAME_COMPLETE ||
		     header.request_id != 9U || header_length != length)) {
			printf("FAIL split subscribe: whole state %d\n", (int)state);
			return 1;
		}
	}

	/* Succeeded. */
	printf("ok split subscribe\n");
	return 0;
}

/*
 * Checks the decoding rule on a LAN_ENABLE that carries an OUTPUT record.
 *
 * Its frame is whole once closed, and its payload is not given to the
 * single-interface decoder, whose refusal of the OUTPUT field was answered
 * with the reserved pair and taken by net for a mismatched response.
 */
static int
check_lan_enable_fields(
	void)
{
	struct networkd_field_writer writer;
	struct networkd_protocol_header header;
	enum networkd_request_frame_state state;
	unsigned char payload[128];
	unsigned char buffer[256];
	size_t header_length;
	size_t length;
	int count;
	int error;

	/* Writes a LAN_ENABLE payload of one record, as net does. */
	networkd_field_writer_init(&writer, payload, sizeof(payload));
	if (networkd_field_write(&writer, NETWORKD_FIELD_OUTPUT, "ue0 dhcp 10", 11U) != 0)
		return 1;
	count = snprintf((char *)buffer, sizeof(buffer), "ZNV2 3 %u %zu\n",
	    (unsigned)NETWORKD_OP_LAN_ENABLE, writer.used);
	if (count < 0)
		return 1;
	memcpy(buffer + count, payload, writer.used);
	length = (size_t)count + writer.used;

	/* The frame waits for the close, then is whole. */
	state = networkd_request_frame_parse(buffer, length, 0, &header,
	    &header_length, &error);
	if (state != NETWORKD_REQUEST_FRAME_PARTIAL) {
		printf("FAIL lan enable open: state %d\n", (int)state);
		return 1;
	}
	state = networkd_request_frame_parse(buffer, length, 1, &header,
	    &header_length, &error);
	if (state != NETWORKD_REQUEST_FRAME_COMPLETE ||
	    header.request_id != 3U ||
	    header.opcode != NETWORKD_OP_LAN_ENABLE) {
		printf("FAIL lan enable closed: state %d\n", (int)state);
		return 1;
	}

	/* The kinds whose payload is not for the single-interface decoder. */
	if (networkd_request_fields_decoded(NETWORKD_OP_LAN_ENABLE) ||
	    networkd_request_fields_decoded(NETWORKD_OP_LAN_DISABLE) ||
	    networkd_request_fields_decoded(NETWORKD_OP_SUBSCRIBE)) {
		printf("FAIL fields decoded for LAN_ENABLE, LAN_DISABLE or SUBSCRIBE\n");
		return 1;
	}

	/* The kinds that name one interface as fields. */
	if (!networkd_request_fields_decoded(NETWORKD_OP_SHOW) ||
	    !networkd_request_fields_decoded(NETWORKD_OP_UP) ||
	    !networkd_request_fields_decoded(NETWORKD_OP_LAN_CONFIGURE)) {
		printf("FAIL fields not decoded for SHOW, UP or LAN_CONFIGURE\n");
		return 1;
	}

	/* Only a subscription stays open after its frame. */
	if (networkd_request_ends_with_close(NETWORKD_OP_SUBSCRIBE) ||
	    !networkd_request_ends_with_close(NETWORKD_OP_LAN_ENABLE) ||
	    !networkd_request_ends_with_close(NETWORKD_OP_SHOW)) {
		printf("FAIL close rule\n");
		return 1;
	}

	/* Succeeded. */
	printf("ok lan enable and the decoding rule\n");
	return 0;
}
