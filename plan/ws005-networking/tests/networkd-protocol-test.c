/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/net/protocol.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void fail(const char *);
static void expect(int, const char *);
static void test_header(void);
static void test_fields(void);
static void test_peer_close(void);
static void test_clear(void);
static void record_pipe_signal(int);

static int fixture_errno;
static volatile sig_atomic_t pipe_signals;

int
*__libc_errno_location(
	void)
{
	return &fixture_errno;
}

int
main(
	void)
{
	/* Exercises each independent codec boundary. */
	test_header();
	test_fields();
	test_peer_close();
	test_clear();

	/* Reports successful completion. */
	puts("networkd protocol test: PASS");
	return 0;
}

/* Records an unexpected broken-pipe signal without terminating the fixture. */
static void
record_pipe_signal(
	int signal_number)
{
	(void)signal_number;
	pipe_signals++;
}

/* Reports one fixture failure. */
static void
fail(
	const char *message)
{
	fprintf(stderr, "networkd-protocol-test: %s\n", message);
	exit(1);
}

/* Requires one fixture condition. */
static void
expect(
	int condition,
	const char *message)
{
	if (!condition)
		fail(message);
}

/* Exercises canonical frame-header handling. */
static void
test_header(
	void)
{
	struct networkd_protocol_header input;
	struct networkd_protocol_header output;
	char bytes[NETWORKD_PROTOCOL_HEADER_MAX + 1U];
	size_t length;

	/* Round trips the largest permitted response length. */
	input.request_id = 4294967295U;
	input.opcode = NETWORKD_OP_WIFI_CONNECT;
	input.payload_length = NETWORKD_RESPONSE_MAX;
	expect(networkd_protocol_header_encode(bytes, sizeof(bytes), &input,
	    &length) == 0, "encode maximum header");
	expect(networkd_protocol_header_decode(bytes, length, &output) == 0,
	    "decode maximum header");
	expect(output.request_id == input.request_id &&
	    output.opcode == input.opcode &&
	    output.payload_length == input.payload_length,
	    "header round trip");

	/* Rejects noncanonical and malformed spellings. */
	expect(networkd_protocol_header_decode("ZNV2 01 1 0\n", 12U,
	    &output) != 0, "leading zero");
	expect(networkd_protocol_header_decode("V1 1 1 0\n", 9U,
	    &output) != 0, "legacy magic");
	expect(networkd_protocol_header_decode("ZNV2 1 1 0\r\n", 13U,
	    &output) != 0, "carriage return");
	expect(networkd_protocol_header_decode("ZNV2 1 1 0", 11U,
	    &output) != 0, "missing newline");
}

/* Exercises explicit-length binary fields. */
static void
test_fields(
	void)
{
	static const unsigned char ssid[] = { 'a', ' ', 'b' };
	struct networkd_field_writer writer;
	struct networkd_field_reader reader;
	struct networkd_field field;
	unsigned char bytes[32];
	uint32_t value;

	/* Creates two fields without relying on C-string framing. */
	networkd_field_writer_init(&writer, bytes, sizeof(bytes));
	expect(networkd_field_write(&writer, NETWORKD_FIELD_SSID, ssid,
	    sizeof(ssid)) == 0, "write SSID");
	expect(networkd_field_write_u32(&writer, NETWORKD_FIELD_TIMEOUT, 30U) == 0,
	    "write timeout");

	/* Decodes the fields and exact terminal boundary. */
	networkd_field_reader_init(&reader, bytes, writer.used);
	expect(networkd_field_read(&reader, &field) == 0 &&
	    field.type == NETWORKD_FIELD_SSID && field.length == sizeof(ssid) &&
	    memcmp(field.value, ssid, sizeof(ssid)) == 0, "read SSID");
	expect(networkd_field_read(&reader, &field) == 0 &&
	    field.type == NETWORKD_FIELD_TIMEOUT &&
	    networkd_field_read_u32(&field, &value) == 0 && value == 30U,
	    "read timeout");
	expect(networkd_field_read(&reader, &field) == 1, "field EOF");

	/* Rejects a truncated declared field and bounded writer overflow. */
	bytes[2] = 0U;
	bytes[3] = 31U;
	networkd_field_reader_init(&reader, bytes, writer.used);
	expect(networkd_field_read(&reader, &field) != 0, "truncated field");
	networkd_field_writer_init(&writer, bytes, 4U);
	expect(networkd_field_write(&writer, NETWORKD_FIELD_SSID, ssid,
	    sizeof(ssid)) != 0 && errno == ENOSPC, "writer bound");

	/* Keeps the credential-free global WLAN operation numbers stable. */
	expect(NETWORKD_OP_WIFI_ENABLE == 32 &&
	    NETWORKD_OP_WIFI_DISABLE == 33 &&
	    NETWORKD_OP_WIFI_LIST == 34 &&
	    NETWORKD_OP_WIFI_CONNECT == 35 &&
	    NETWORKD_OP_WIFI_DISCONNECT == 36 &&
	    NETWORKD_OP_WIFI_PROFILES_CHANGED == 37,
	    "global Wi-Fi opcodes");

	/* Reserves the confirmed-commit family without renumbering older requests. */
	expect(NETWORKD_OP_DEFAULT_ROUTE_CLEAR == 9 &&
	    NETWORKD_OP_DNS_CLEAR == 10 &&
	    NETWORKD_OP_CONFIRMED_ARM == 16 &&
	    NETWORKD_OP_CONFIRMED_DISARM == 17 &&
	    NETWORKD_OP_CONFIRMED_ROLLBACK == 18 &&
	    NETWORKD_OP_CONFIRMED_CHECK == 19 &&
	    NETWORKD_FIELD_PATH == 22 && NETWORKD_FIELD_TOKEN == 23,
	    "confirmed-commit protocol numbers");
}

/* Exercises a peer which closes before accepting one request frame. */
static void
test_peer_close(
	void)
{
	struct networkd_protocol_header header;
	void (*previous)(int);
	int descriptor[2];

	/* Makes a closed AF_UNIX peer produce EPIPE instead of SIGPIPE. */
	expect(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptor) == 0,
	    "create closed-peer socket pair");
	expect(close(descriptor[1]) == 0, "close frame peer");
	pipe_signals = 0;
	previous = signal(SIGPIPE, record_pipe_signal);
	expect(previous != SIG_ERR, "install SIGPIPE observer");
	header.request_id = 1U;
	header.opcode = NETWORKD_OP_WIFI_PROFILES_CHANGED;
	header.payload_length = 0U;
	expect(networkd_protocol_write_frame(descriptor[0], &header, NULL) != 0,
	    "closed peer reports write failure");
	expect(pipe_signals == 0, "closed peer suppresses SIGPIPE");
	expect(signal(SIGPIPE, previous) != SIG_ERR, "restore SIGPIPE observer");
	expect(close(descriptor[0]) == 0, "close frame writer");
}

/* Exercises non-elidable secret storage clearing. */
static void
test_clear(
	void)
{
	unsigned char bytes[16];
	size_t index;

	/* Clears every byte in the selected extent. */
	memset(bytes, 0xa5, sizeof(bytes));
	networkd_protocol_clear(bytes, sizeof(bytes));
	for (index = 0U; index < sizeof(bytes); index++)
		expect(bytes[index] == 0U, "clear extent");
}
