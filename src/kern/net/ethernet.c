/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

void
ethernet_init(void)
{
	memset(protocols, 0, sizeof(protocols));
	protocol_count = 0;
}

int
ethernet_protocol_register(uint16_t type, ethernet_input_fn input)
{
	unsigned index;

	if (type == 0 || input == NULL)
		return EINVAL;
	for (index = 0; index < protocol_count; index++)
		if (protocols[index].type == type)
			return EEXIST;
	if (protocol_count >= ETHERNET_PROTOCOL_MAX)
		return ENOSPC;
	protocols[protocol_count].type = type;
	protocols[protocol_count].input = input;
	protocol_count++;
	return 0;
}

static int
is_broadcast(const uint8_t address[6])
{
	unsigned index;

	for (index = 0; index < 6; index++)
		if (address[index] != 0xff)
			return 0;
	return 1;
}

int
ethernet_input(struct packet_buf *packet)
{
	const uint8_t *header;
	uint16_t type;
	uint8_t packet_type;
	unsigned index;

	if (packet == NULL)
		return EINVAL;
	if (packet->device == NULL || packet->length < ETHERNET_HEADER_LENGTH) {
		packet_buf_free(packet);
		return EINVAL;
	}
	header = packet->data;
	if (!memcmp(header, packet->device->hwaddr, 6))
		packet_type = L2_PACKET_HOST;
	else if (is_broadcast(header))
		packet_type = L2_PACKET_BROADCAST;
	else if (header[0] & 1U)
		packet_type = L2_PACKET_MULTICAST;
	else {
		packet_buf_free(packet);
		return 0;
	}
	type = (uint16_t)((uint16_t)header[12] << 8 | header[13]);
	packet->l2_offset = (uint16_t)(packet->data - packet->storage);
	packet->protocol = type;
	packet_socket_deliver(packet, header + 6, packet_type);
	if (packet_buf_pull(packet, ETHERNET_HEADER_LENGTH) == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}
	packet->l3_offset = (uint16_t)(packet->data - packet->storage);
	for (index = 0; index < protocol_count; index++) {
		if (protocols[index].type == type)
			return protocols[index].input(packet);
	}
	packet_buf_free(packet);
	return 0;
}

int
ethernet_output(struct net_device *device, const uint8_t destination[6],
		uint16_t type, struct packet_buf *packet)
{
	uint8_t *header;

	if (device == NULL || destination == NULL || packet == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}
	if (packet->length > device->mtu) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}
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
	return net_device_transmit(device, packet);
}
