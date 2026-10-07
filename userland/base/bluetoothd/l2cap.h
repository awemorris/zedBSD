/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's L2CAP signalling (ws143-p004, Core 5.4 Vol 3 Part A §4):
 * the BR/EDR signalling channel (connection, configuration with the MTU
 * alone, disconnection, information, command reject) and the LE
 * signalling channel (connection parameter update), with the table of
 * connection-oriented channels.  Without system calls; the host tests
 * build it.  Channels the other side asks for are refused in this Phase
 * (HID's are p005's).
 */

#ifndef BLUETOOTHD_L2CAP_H
#define BLUETOOTHD_L2CAP_H

#include <stddef.h>
#include <stdint.h>

/* The fixed channels. */
#define BTD_CID_SIGNALLING	0x0001U
#define BTD_CID_ATT		0x0004U
#define BTD_CID_LE_SIGNALLING	0x0005U
#define BTD_CID_SMP		0x0006U

/* The first dynamic channel, and how many channels the table holds. */
#define BTD_CID_DYNAMIC		0x0040U
#define BTD_CHANNELS_MAX	16U

/* The least MTU of BR/EDR (Core Vol 3 Part A §5.1), the one bluetoothd asks for, and the longest signalling answer. */
#define BTD_L2CAP_MTU_MIN	48U
#define BTD_L2CAP_MTU		672U
#define BTD_SIGNAL_MAX		128U

/* The states of a channel bluetoothd opened. */
#define BTD_CHANNEL_FREE	0U
#define BTD_CHANNEL_CONNECTING	1U
#define BTD_CHANNEL_CONFIGURING	2U
#define BTD_CHANNEL_OPEN	3U
#define BTD_CHANNEL_CLOSING	4U

/*
 * One connection-oriented channel: its connection, its two channel IDs,
 * its PSM, its state, the MTU the other side gave, and which side's
 * configuration is done.
 */
struct btd_channel {
	unsigned state;
	uint16_t handle;
	uint16_t local_cid;
	uint16_t remote_cid;
	uint16_t psm;
	uint16_t remote_mtu;
	int local_done;
	int remote_done;
	uint8_t identifier;
};

/*
 * The channels of the daemon and the identifier of the next request.  It
 * lives in the daemon's state for its life.
 */
struct btd_l2cap {
	struct btd_channel channels[BTD_CHANNELS_MAX];
	uint8_t next_identifier;
	int information_pending;
	uint8_t information_identifier;
	unsigned rejected;
	unsigned malformed;
};

/*
 * What a signalling PDU asked of the daemon besides its answers: a
 * connection parameter update to carry out (LE), the interval's bounds,
 * the latency and the timeout as the request gave them; and the answer to
 * bluetoothd's own Information Request (its result and the features).
 */
struct btd_signal_effect {
	int update;
	uint16_t interval_min;
	uint16_t interval_max;
	uint16_t latency;
	uint16_t timeout;
	int information;
	uint16_t information_result;
	uint32_t features;
};

void btd_l2cap_init(struct btd_l2cap *l2cap);
int btd_l2cap_signal(struct btd_l2cap *l2cap, uint16_t handle, int le, const uint8_t *payload, size_t length, uint8_t *answer, size_t size, size_t *answer_length, struct btd_signal_effect *effect);
int btd_l2cap_connect(struct btd_l2cap *l2cap, uint16_t handle, uint16_t psm, uint8_t *request, size_t size, size_t *request_length, uint16_t *local_cid);
int btd_l2cap_information(struct btd_l2cap *l2cap, uint8_t *request, size_t size, size_t *request_length);
int btd_l2cap_disconnect(struct btd_l2cap *l2cap, uint16_t local_cid, uint8_t *request, size_t size, size_t *request_length);
struct btd_channel *btd_l2cap_channel(struct btd_l2cap *l2cap, uint16_t local_cid);
void btd_l2cap_drop(struct btd_l2cap *l2cap, uint16_t handle);

#endif
