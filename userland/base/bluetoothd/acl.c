/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of bluetoothd's ACL data path without system calls (ws143-p004,
 * see acl.h; Core 5.4 Vol 4 Part E §5.4.2 and Vol 3 Part A §3.1).
 */

#include "userland/base/bluetoothd/acl.h"

#include <errno.h>
#include <string.h>

/* The H4 type of an ACL packet, and the bits of the handle in its first two bytes. */
#define ACL_PACKET		0x02U
#define ACL_HANDLE_MASK		0x0fffU

/*
 * Takes an H4 ACL packet apart.  Returns 0, or EBADMSG for a packet that is
 * not ACL or whose length is not its header's.
 */
int
btd_acl_parse(
	const uint8_t *packet,
	size_t length,
	struct btd_acl *acl)
{
	size_t data_length;

	/* An ACL packet with its whole header. */
	if (length < 1U + BTD_ACL_HEADER)
		return EBADMSG;
	if (packet[0] != ACL_PACKET)
		return EBADMSG;

	/* The data's length must be what follows. */
	data_length = (size_t)(packet[3] | (packet[4] << 8));
	if (data_length != length - 1U - BTD_ACL_HEADER)
		return EBADMSG;

	/* Succeeded: the handle, the boundary flag and the data. */
	acl->handle = (uint16_t)(((unsigned)packet[1] | ((unsigned)packet[2] << 8)) & ACL_HANDLE_MASK);
	acl->boundary = (uint8_t)((packet[2] >> 4) & 0x03U);
	acl->data = packet + 1U + BTD_ACL_HEADER;
	acl->length = data_length;
	return 0;
}

/*
 * Builds an H4 ACL packet.  Returns its length, or 0 when it does not fit.
 */
size_t
btd_acl_build(
	uint8_t *packet,
	size_t size,
	uint16_t handle,
	uint8_t boundary,
	const uint8_t *data,
	size_t length)
{
	/* Room for the type, the header and the data. */
	if (length > 0xffffU || size < 1U + BTD_ACL_HEADER + length)
		return 0U;

	/* The type, the handle with its flags, the length. */
	packet[0] = ACL_PACKET;
	packet[1] = (uint8_t)(handle & 0xffU);
	packet[2] = (uint8_t)(((handle >> 8) & 0x0fU) | ((boundary & 0x03U) << 4));
	packet[3] = (uint8_t)(length & 0xffU);
	packet[4] = (uint8_t)(length >> 8);

	/* The data. */
	if (length != 0U)
		memcpy(packet + 1U + BTD_ACL_HEADER, data, length);

	/* Succeeded: the packet's length. */
	return 1U + BTD_ACL_HEADER + length;
}

/*
 * Adds an ACL packet's data to the frame being put together.  A first
 * packet starts a frame (one left unfinished is dropped); a continuing one
 * adds to it.  Returns 1 when the frame is whole (in reassembly->frame,
 * reassembly->expected bytes), 0 when more must come, or -1 when the data
 * was dropped (a continuing packet with no frame, a frame longer than
 * BTD_L2CAP_MAX, more than the frame's length).
 */
int
btd_reassembly_feed(
	struct btd_reassembly *reassembly,
	const struct btd_acl *acl)
{
	size_t payload;

	/* A first packet starts a frame; one left unfinished is dropped. */
	if (acl->boundary != BTD_ACL_CONTINUING) {
		if (reassembly->active)
			reassembly->dropped++;
		reassembly->active = 0;
		reassembly->used = 0U;

		/* The L2CAP header must be in the first packet. */
		if (acl->length < BTD_L2CAP_HEADER) {
			reassembly->dropped++;
			return -1;
		}

		/* The frame's length, within what bluetoothd takes. */
		payload = (size_t)(acl->data[0] | (acl->data[1] << 8));
		if (payload > BTD_L2CAP_MAX) {
			reassembly->dropped++;
			return -1;
		}

		/* A new frame of this length begins. */
		reassembly->expected = BTD_L2CAP_HEADER + payload;
		reassembly->active = 1;
	}

	/* A continuing packet needs a frame. */
	if (!reassembly->active) {
		reassembly->dropped++;
		return -1;
	}

	/* The data must not run past the frame. */
	if (acl->length > reassembly->expected - reassembly->used) {
		reassembly->active = 0;
		reassembly->dropped++;
		return -1;
	}

	/* The data, added. */
	memcpy(reassembly->frame + reassembly->used, acl->data, acl->length);
	reassembly->used += acl->length;

	/* More must come. */
	if (reassembly->used < reassembly->expected)
		return 0;

	/* Succeeded: the frame is whole. */
	reassembly->active = 0;
	return 1;
}

/*
 * Builds an L2CAP basic frame: the payload's length, the channel, the
 * payload.  Returns the frame's length, or 0 when it does not fit.
 */
size_t
btd_l2cap_frame(
	uint8_t *frame,
	size_t size,
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	/* Room for the header and the payload. */
	if (length > BTD_L2CAP_MAX || size < BTD_L2CAP_HEADER + length)
		return 0U;

	/* The header. */
	frame[0] = (uint8_t)(length & 0xffU);
	frame[1] = (uint8_t)(length >> 8);
	frame[2] = (uint8_t)(cid & 0xffU);
	frame[3] = (uint8_t)(cid >> 8);

	/* The payload. */
	if (length != 0U)
		memcpy(frame + BTD_L2CAP_HEADER, payload, length);

	/* Succeeded: the frame's length. */
	return BTD_L2CAP_HEADER + length;
}
