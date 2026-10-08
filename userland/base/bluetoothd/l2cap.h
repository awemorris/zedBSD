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
 * build it.
 *
 * ws143-p005 (HID): a table's owner may serve channels the other side asks
 * for (the accept hook: accepted, pending until the link is encrypted, or
 * refused), the other side's Flush Timeout, QoS, retransmission (basic
 * mode only) and FCS options are taken, an Echo Request can be sent, and
 * each signalling frame tells which channels opened and closed.
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

/*
 * The states of a channel: free, connecting (our request out), configuring,
 * open, closing (our Disconnection Request out), and pending (the other
 * side's request answered Pending, the final answer to come).
 */
#define BTD_CHANNEL_FREE	0U
#define BTD_CHANNEL_CONNECTING	1U
#define BTD_CHANNEL_CONFIGURING	2U
#define BTD_CHANNEL_OPEN	3U
#define BTD_CHANNEL_CLOSING	4U
#define BTD_CHANNEL_PENDING	5U

/*
 * The results of a Connection Response (Core 5.4 Vol 3 Part A §4.3):
 * success, pending, PSM not supported, security block, no resources,
 * invalid source CID, source CID already allocated.
 */
#define BTD_L2CAP_SUCCESS		0x0000U
#define BTD_L2CAP_PENDING		0x0001U
#define BTD_L2CAP_PSM_NOT_SUPPORTED	0x0002U
#define BTD_L2CAP_SECURITY_BLOCK	0x0003U
#define BTD_L2CAP_NO_RESOURCES		0x0004U
#define BTD_L2CAP_INVALID_SOURCE	0x0006U
#define BTD_L2CAP_SOURCE_TAKEN		0x0007U

/* Why a channel closed: the other side's Disconnection Request, ours answered, or a refusal (connection, configuration, Command Reject). */
#define BTD_L2CAP_CLOSED_REMOTE		1U
#define BTD_L2CAP_CLOSED_LOCAL		2U
#define BTD_L2CAP_CLOSED_REFUSED	3U

/* How many channels one signalling frame can tell opened, and closed. */
#define BTD_SIGNAL_CHANGES_MAX		4U

/*
 * The owner's answer to a channel the other side asks for on a connection
 * (its PSM): it sets the result (a BTD_L2CAP_* result: success, pending,
 * or a refusal) and the status (with pending: 0x0000, no further
 * information) and returns 0, or returns nonzero to leave the request
 * refused as PSM not supported.
 */
typedef int (*btd_l2cap_accept_fn)(void *context, uint16_t handle, uint16_t psm, uint16_t *result, uint16_t *status);

/*
 * One connection-oriented channel: its connection, its two channel IDs,
 * its PSM, its state, the MTU the other side gave, which side's
 * configuration is done, and the identifier of the request outstanding
 * (ours, or for a pending channel the other side's request still to be
 * answered).  inbound says the other side asked for it; the Flush Timeout
 * and the QoS the other side gave are kept (bluetoothd carries out
 * neither, phase005 Q10).
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
	int inbound;
	int have_flush_timeout;
	uint16_t flush_timeout;
	int have_qos;
};

/*
 * The channels of one owner (the pairing, or one HID device) and the
 * identifier of the next request.  It lives in its owner's state for the
 * owner's life.  The accept hook serves the channels the other side asks
 * for (NULL: every one is refused); an Echo Request of ours waits under
 * echo_identifier while echo_pending is set.
 */
struct btd_l2cap {
	struct btd_channel channels[BTD_CHANNELS_MAX];
	uint8_t next_identifier;
	int information_pending;
	uint8_t information_identifier;
	unsigned rejected;
	unsigned malformed;
	btd_l2cap_accept_fn accept;
	void *accept_context;
	int echo_pending;
	uint8_t echo_identifier;
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

	/*
	 * ws143-p005: the channels that opened (both configurations done) and
	 * that closed (with a BTD_L2CAP_CLOSED_* reason) in the frame, their
	 * local CIDs, and whether the answer to our Echo Request came.
	 */
	unsigned opened_count;
	uint16_t opened[BTD_SIGNAL_CHANGES_MAX];
	unsigned closed_count;
	uint16_t closed[BTD_SIGNAL_CHANGES_MAX];
	unsigned closed_reason[BTD_SIGNAL_CHANGES_MAX];
	int echo;
};

void btd_l2cap_init(struct btd_l2cap *l2cap);
int btd_l2cap_signal(struct btd_l2cap *l2cap, uint16_t handle, int le, const uint8_t *payload, size_t length, uint8_t *answer, size_t size, size_t *answer_length, struct btd_signal_effect *effect);
int btd_l2cap_connect(struct btd_l2cap *l2cap, uint16_t handle, uint16_t psm, uint8_t *request, size_t size, size_t *request_length, uint16_t *local_cid);
int btd_l2cap_information(struct btd_l2cap *l2cap, uint8_t *request, size_t size, size_t *request_length);
int btd_l2cap_disconnect(struct btd_l2cap *l2cap, uint16_t local_cid, uint8_t *request, size_t size, size_t *request_length);
struct btd_channel *btd_l2cap_channel(struct btd_l2cap *l2cap, uint16_t local_cid);
void btd_l2cap_drop(struct btd_l2cap *l2cap, uint16_t handle);
void btd_l2cap_set_accept(struct btd_l2cap *l2cap, btd_l2cap_accept_fn accept, void *context);
int btd_l2cap_answer_pending(struct btd_l2cap *l2cap, uint16_t handle, uint16_t result, uint8_t *answer, size_t size, size_t *answer_length);
int btd_l2cap_echo(struct btd_l2cap *l2cap, uint8_t *request, size_t size, size_t *request_length);

#endif
