/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's SDP server (ws197-p002, plan/ws197/phase002/phase.md section
 * 10.1): the records bluetoothd offers a phone (MAP's MNS, HFP's HF, PBAP's
 * PCE, registered by those profiles), and the answers to
 * ServiceSearchRequest, ServiceAttributeRequest and
 * ServiceSearchAttributeRequest (Core 5.4 Vol 3 Part B section 4), with
 * partial answers and continuation states.  Also the writer of data
 * elements the profiles build their records with.
 *
 * Without system calls; the host tests build it.
 */

#ifndef BLUETOOTHD_SDPS_H
#define BLUETOOTHD_SDPS_H

#include <stddef.h>
#include <stdint.h>

/* How many records, the bytes of one record's attributes, of one whole answer, and of a request kept for its continuation. */
#define BTD_SDPS_RECORDS_MAX		6U
#define BTD_SDPS_RECORD_MAX		512U
#define BTD_SDPS_RESPONSE_MAX		4096U
#define BTD_SDPS_REQUEST_MAX		256U

/* The first record handle given (Core 5.4 Vol 3 Part B section 5.1.1: 0x00000001 to 0x0000ffff are reserved). */
#define BTD_SDPS_HANDLE_FIRST		0x00010000U

/* The most UUIDs of a search pattern (Core 5.4 Vol 3 Part B section 4.5.1), and of attribute IDs and ranges in a request. */
#define BTD_SDPS_PATTERN_MAX		12U
#define BTD_SDPS_RANGES_MAX		32U

/* The error codes of SDP_ERROR_RSP (Core 5.4 Vol 3 Part B section 4.4.1). */
#define BTD_SDPS_ERROR_VERSION		0x0001U
#define BTD_SDPS_ERROR_HANDLE		0x0002U
#define BTD_SDPS_ERROR_SYNTAX		0x0003U
#define BTD_SDPS_ERROR_PDU_SIZE		0x0004U
#define BTD_SDPS_ERROR_CONTINUATION	0x0005U
#define BTD_SDPS_ERROR_RESOURCES	0x0006U

/* How deep a writer's sequences may nest. */
#define BTD_SDP_WRITER_DEPTH		6U

/*
 * Data elements being written into a buffer: its room, how much is used,
 * whether something did not fit or a sequence was ended that was not
 * begun, and the offsets of the sequences begun and not ended (their
 * lengths are written when they end).
 */
struct btd_sdp_writer {
	uint8_t *bytes;
	size_t size;
	size_t used;
	int overflow;
	unsigned depth;
	size_t open[BTD_SDP_WRITER_DEPTH];
};

/* One record offered: its handle and its attribute pairs (ascending IDs, the handle's and the browse group's included), or none. */
struct btd_sdps_record {
	int used;
	uint32_t handle;
	size_t length;
	uint8_t attributes[BTD_SDPS_RECORD_MAX];
};

/*
 * The records bluetoothd offers, for every server: the version that
 * changes with each registration (a continuation from before a change is
 * refused), and the next handle.  It lives in the daemon for its life.
 */
struct btd_sdps_db {
	unsigned version;
	uint32_t next_handle;
	struct btd_sdps_record records[BTD_SDPS_RECORDS_MAX];
};

/*
 * The server of one L2CAP channel: the records, the peer's MTU (an answer
 * fits it), and the whole answer kept for the continuations of the request
 * that made it (its PDU, the database's version, its parameters).
 */
struct btd_sdps {
	const struct btd_sdps_db *db;
	size_t mtu;
	uint8_t kept_pdu;
	unsigned kept_version;
	size_t kept_request_length;
	uint8_t kept_request[BTD_SDPS_REQUEST_MAX];
	size_t kept_length;
	uint8_t kept[BTD_SDPS_RESPONSE_MAX];
	unsigned errors;
};

void btd_sdp_writer_init(struct btd_sdp_writer *writer, uint8_t *bytes, size_t size);
void btd_sdp_put_uint8(struct btd_sdp_writer *writer, uint8_t value);
void btd_sdp_put_uint16(struct btd_sdp_writer *writer, uint16_t value);
void btd_sdp_put_uint32(struct btd_sdp_writer *writer, uint32_t value);
void btd_sdp_put_uuid16(struct btd_sdp_writer *writer, uint16_t value);
void btd_sdp_put_uuid128(struct btd_sdp_writer *writer, const uint8_t *value);
void btd_sdp_put_text(struct btd_sdp_writer *writer, const char *text);
void btd_sdp_begin(struct btd_sdp_writer *writer);
void btd_sdp_end(struct btd_sdp_writer *writer);

void btd_sdps_db_init(struct btd_sdps_db *db);
int btd_sdps_register(struct btd_sdps_db *db, const uint8_t *pairs, size_t length, uint32_t *handle);
int btd_sdps_unregister(struct btd_sdps_db *db, uint32_t handle);
void btd_sdps_init(struct btd_sdps *server, const struct btd_sdps_db *db, size_t mtu);
int btd_sdps_input(struct btd_sdps *server, const uint8_t *pdu, size_t length, uint8_t *answer, size_t size, size_t *answer_length);

#endif
