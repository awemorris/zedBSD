/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's CTAPHID (ws161-p004; FIDO CTAP 2.1 section 11.2): messages
 * cut into 64-byte reports and put back together, over any pair of report
 * functions (a raw HID node, or the host test's software authenticator).
 *
 * A channel is taken with INIT on the broadcast channel, matching the
 * answer by its nonce (every open of a raw node sees every report).  A
 * transaction sends one message and reads reports until its own channel's
 * answer: other channels' reports are skipped, KEEPALIVE is handed to the
 * caller (the key waits for a touch), ERROR is an answer of its own.
 */

#ifndef LIBPASSKEY_HID_H
#define LIBPASSKEY_HID_H

#include <stddef.h>
#include <stdint.h>

/* The report, the data of an initialization and a continuation packet, and the longest message. */
#define PK_HID_REPORT		64U
#define PK_HID_INIT_DATA	(PK_HID_REPORT - 7U)
#define PK_HID_CONT_DATA	(PK_HID_REPORT - 5U)
#define PK_HID_MESSAGE_MAX	(PK_HID_INIT_DATA + 128U * PK_HID_CONT_DATA)

/* The commands (with the initialization packet's top bit) and the broadcast channel. */
#define PK_HID_PING		0x81U
#define PK_HID_MSG		0x83U
#define PK_HID_INIT		0x86U
#define PK_HID_WINK		0x88U
#define PK_HID_CBOR		0x90U
#define PK_HID_CANCEL		0x91U
#define PK_HID_KEEPALIVE	0xbbU
#define PK_HID_ERROR		0xbfU
#define PK_HID_BROADCAST	0xffffffffU

/* INIT's capabilities, and KEEPALIVE's states. */
#define PK_HID_CAPABILITY_WINK	0x01U
#define PK_HID_CAPABILITY_CBOR	0x04U
#define PK_HID_CAPABILITY_NMSG	0x08U
#define PK_HID_KEEPALIVE_PROCESSING	1U
#define PK_HID_KEEPALIVE_UP_NEEDED	2U

/* ERROR's codes the library acts on. */
#define PK_HID_ERR_CHANNEL_BUSY	0x06U

/*
 * The report functions: write sends one output report (the report ID's
 * byte, 0, then PK_HID_REPORT bytes); read waits at most timeout_ms for
 * one input report of PK_HID_REPORT bytes (ETIMEDOUT when none came).
 * Both answer 0 or an errno value.
 */
struct pk_hid_io {
	void *context;
	int (*write)(void *context, const uint8_t *report, size_t size);
	int (*read)(void *context, uint8_t *report, size_t size, unsigned timeout_ms);
};

/* A key's channel: the reports, the channel INIT gave, and what INIT said of the key. */
struct pk_hid {
	struct pk_hid_io io;
	uint32_t channel;
	uint8_t protocol;
	uint8_t version[3];
	uint8_t capabilities;
};

/* Told of each KEEPALIVE while a transaction waits: the state (PK_HID_KEEPALIVE_*). */
typedef void (*pk_hid_keepalive_t)(void *context, uint8_t status);

int pk_hid_open(struct pk_hid *hid, const struct pk_hid_io *io, unsigned timeout_ms);
int pk_hid_transact(struct pk_hid *hid, uint8_t command, const uint8_t *data, size_t size,
    uint8_t *reply_command, uint8_t *reply, size_t capacity, size_t *reply_size, unsigned timeout_ms,
    pk_hid_keepalive_t keepalive, void *keepalive_context);
int pk_hid_cancel(struct pk_hid *hid);

#endif
