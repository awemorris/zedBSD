/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the private net-to-networkd protocol.
 */

#ifndef ZEDBSD_NETWORKD_PROTOCOL_H
#define ZEDBSD_NETWORKD_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifndef NETWORKD_SOCKET
#define NETWORKD_SOCKET "/run/networkd.sock"
#endif

#define NETWORKD_PROTOCOL_MAGIC		"ZNV2"
#define NETWORKD_PROTOCOL_HEADER_MAX	32U
#define NETWORKD_REQUEST_MAX		4096U
#define NETWORKD_RESPONSE_MAX		32768U
#define NETWORKD_DIAGNOSTIC_MAX		512U
#define NETWORKD_CONFIRMED_MINUTES_MAX	1440U
#define NETWORKD_ROLLBACK_OPERATION_MAX	64U
#define NETWORKD_ROLLBACK_LINE_MAX	4096U
#define NETWORKD_ROLLBACK_PROGRAM_MAX	32768U
#define NETWORKD_ROLLBACK_DIAGNOSTIC_MAX	512U
#define NETWORKD_ROLLBACK_PATH_MAX	255U

enum networkd_opcode {
	NETWORKD_OP_SHOW = 1,
	NETWORKD_OP_UP = 2,
	NETWORKD_OP_DOWN = 3,
	NETWORKD_OP_DHCP = 4,
	NETWORKD_OP_STATIC = 5,
	NETWORKD_OP_DEFAULT_ROUTE = 6,
	NETWORKD_OP_DNS = 7,
	NETWORKD_OP_RELOAD = 8,
	NETWORKD_OP_DEFAULT_ROUTE_CLEAR = 9,
	NETWORKD_OP_DNS_CLEAR = 10,
	NETWORKD_OP_CONFIRMED_ARM = 16,
	NETWORKD_OP_CONFIRMED_DISARM = 17,
	NETWORKD_OP_CONFIRMED_ROLLBACK = 18,
	NETWORKD_OP_CONFIRMED_CHECK = 19,
	NETWORKD_OP_WIFI_ENABLE = 32,
	NETWORKD_OP_WIFI_DISABLE = 33,
	NETWORKD_OP_WIFI_LIST = 34,
	NETWORKD_OP_WIFI_CONNECT = 35,
	NETWORKD_OP_WIFI_DISCONNECT = 36,
	NETWORKD_OP_WIFI_PROFILES_CHANGED = 37
};

enum networkd_field_type {
	NETWORKD_FIELD_STATUS = 1,
	NETWORKD_FIELD_ERROR = 2,
	NETWORKD_FIELD_STAGE = 3,
	NETWORKD_FIELD_OUTPUT = 4,
	NETWORKD_FIELD_INTERFACE = 16,
	NETWORKD_FIELD_TIMEOUT = 17,
	NETWORKD_FIELD_ADDRESS = 18,
	NETWORKD_FIELD_NETMASK = 19,
	NETWORKD_FIELD_GATEWAY = 20,
	NETWORKD_FIELD_DNS = 21,
	NETWORKD_FIELD_PATH = 22,
	NETWORKD_FIELD_TOKEN = 23,
	NETWORKD_FIELD_SSID = 32
};

enum networkd_result_status {
	NETWORKD_RESULT_OK = 0,
	NETWORKD_RESULT_ERROR = 1,
	NETWORKD_RESULT_DEGRADED = 2,
	NETWORKD_RESULT_NO_CANDIDATE = 3,
	NETWORKD_RESULT_IN_PROGRESS = 4
};

struct networkd_protocol_header {
	uint32_t request_id;
	uint32_t opcode;
	size_t payload_length;
};

struct networkd_field_writer {
	unsigned char *bytes;
	size_t capacity;
	size_t used;
};

struct networkd_field_reader {
	const unsigned char *bytes;
	size_t length;
	size_t offset;
};

struct networkd_field {
	uint16_t type;
	const unsigned char *value;
	size_t length;
};

int networkd_protocol_header_encode(char *, size_t,
	const struct networkd_protocol_header *, size_t *);
int networkd_protocol_header_decode(const char *, size_t,
	struct networkd_protocol_header *);
void networkd_field_writer_init(struct networkd_field_writer *, void *,
	size_t);
int networkd_field_write(struct networkd_field_writer *, uint16_t,
	const void *, size_t);
int networkd_field_write_u32(struct networkd_field_writer *, uint16_t,
	uint32_t);
void networkd_field_reader_init(struct networkd_field_reader *, const void *,
	size_t);
int networkd_field_read(struct networkd_field_reader *, struct networkd_field *);
int networkd_field_read_u32(const struct networkd_field *, uint32_t *);
int networkd_protocol_write_frame(int,
	const struct networkd_protocol_header *, const void *);
int networkd_protocol_read_frame(int, struct networkd_protocol_header *,
	void *, size_t, size_t);
void networkd_protocol_clear(void *, size_t);

#endif
