/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Ethernet framing.
 *
 * Received frames are classified by destination, offered to the packet
 * sockets, and handed to the protocol registered for their type.  Sent
 * frames get a header in the packet's headroom before going to the
 * device.
 */

#include "kern/net/ethernet.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/socket.h"

#include <errno.h>
#include <string.h>

#define ETHERNET_PROTOCOL_MAX 8U

struct ethernet_protocol {
	uint16_t type;
	ethernet_input_fn input;
};

static struct ethernet_protocol protocols[ETHERNET_PROTOCOL_MAX];
static unsigned protocol_count;

static int is_broadcast(const uint8_t address[6]);

/*
 * Initializes the protocol table.
 */
void
ethernet_init(
	void)
{
	memset(protocols, 0, sizeof(protocols));
	protocol_count = 0;
}

/*
 * Registers the input function for an Ethernet type.
 */
int
ethernet_protocol_register(
	uint16_t type,
	ethernet_input_fn input)
{
	unsigned index;

	/* Rejects an empty type or a missing input function. */
	if (type == 0 || input == NULL)
		return EINVAL;

	/* Refuses a type that is already registered. */
	for (index = 0; index < protocol_count; index++) {
		if (protocols[index].type == type)
			return EEXIST;
	}

	/* Appends the protocol to the table. */
	if (protocol_count >= ETHERNET_PROTOCOL_MAX)
		return ENOSPC;
	protocols[protocol_count].type = type;
	protocols[protocol_count].input = input;
	protocol_count++;

	/* Reports the registration. */
	return 0;
}

/*
 * Receives an Ethernet frame from a device.
 *
 * The packet is consumed: the registered protocol takes it, or it is
 * freed.  Frames for other unicast addresses are dropped silently.
 */
int
ethernet_input(
	struct packet_buf *packet)
{
	const uint8_t *header;
	uint16_t type;
	uint8_t packet_type;
	unsigned index;
	int error;

	/* Rejects a missing packet, and frees one without a device or header. */
	if (packet == NULL)
		return EINVAL;
	if (packet->device == NULL || packet->length < ETHERNET_HEADER_LENGTH) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Classifies the frame by its destination address. */
	header = packet->data;
	if (!memcmp(header, packet->device->hwaddr, 6)) {
		packet_type = L2_PACKET_HOST;
	} else if (is_broadcast(header)) {
		packet_type = L2_PACKET_BROADCAST;
	} else if (header[0] & 1U) {
		packet_type = L2_PACKET_MULTICAST;
	} else {
		packet_buf_free(packet);
		return 0;
	}

	/* Records the header and offers the frame to the packet sockets. */
	type = (uint16_t)((uint16_t)header[12] << 8 | header[13]);
	packet->l2_offset = (uint16_t)(packet->data - packet->storage);
	packet->protocol = type;
	packet_socket_deliver(packet, header + 6, packet_type);

	/* Strips the header and hands the payload to its protocol. */
	if (packet_buf_pull(packet, ETHERNET_HEADER_LENGTH) == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}
	packet->l3_offset = (uint16_t)(packet->data - packet->storage);
	for (index = 0; index < protocol_count; index++) {
		if (protocols[index].type == type) {
			error = protocols[index].input(packet);
			return error;
		}
	}

	/* Drops a frame of an unregistered type. */
	packet_buf_free(packet);

	/* Reports the dropped frame. */
	return 0;
}

/*
 * Sends a payload as an Ethernet frame.
 *
 * The packet is consumed whether or not the send succeeds.
 */
int
ethernet_output(
	struct net_device *device,
	const uint8_t destination[6],
	uint16_t type,
	struct packet_buf *packet)
{
	uint8_t *header;
	int error;

	/* Rejects a missing operand. */
	if (device == NULL || destination == NULL || packet == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Rejects a payload larger than the device MTU. */
	if (packet->length > device->mtu) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}

	/* Prepends the header in the packet's headroom. */
	header = packet_buf_push(packet, ETHERNET_HEADER_LENGTH);
	if (header == NULL) {
		packet_buf_free(packet);
		return ENOBUFS;
	}
	memcpy(header, destination, 6);
	memcpy(header + 6, device->hwaddr, 6);
	header[12] = (uint8_t)(type >> 8);
	header[13] = (uint8_t)type;
	packet->l2_offset = (uint16_t)(packet->data - packet->storage);
	packet->protocol = type;

	/* Hands the frame to the device. */
	error = net_device_transmit(device, packet);

	/* Reports the transmit result. */
	return error;
}

/* Tests whether an address is the broadcast address. */
static int
is_broadcast(
	const uint8_t address[6])
{
	unsigned index;

	/* Every byte must be 0xff. */
	for (index = 0; index < 6; index++) {
		if (address[index] != 0xff)
			return 0;
	}

	/* Reports the broadcast address. */
	return 1;
}
