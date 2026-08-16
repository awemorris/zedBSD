/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/ethernet.h"
#include "kern/net/inet-socket.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"
#include "internal.h"
#include "wire.h"

#include <zedbsd/netinet.h>
#include <errno.h>
#include <string.h>

#define IPV4_HEADER_MIN 20U
#define IPV4_PROTOCOL_MAX 8U

struct ipv4_protocol_entry {
	uint8_t protocol;
	ipv4_input_fn input;
};

static struct ipv4_protocol_entry protocols[IPV4_PROTOCOL_MAX];
static unsigned protocol_count;
static uint16_t next_identification;

int
ipv4_protocol_register(uint8_t protocol, ipv4_input_fn input)
{
	unsigned index;

	if (protocol == 0 || input == NULL)
		return EINVAL;
	for (index = 0; index < protocol_count; index++)
		if (protocols[index].protocol == protocol)
			return EEXIST;
	if (protocol_count >= IPV4_PROTOCOL_MAX)
		return ENOSPC;
	protocols[protocol_count].protocol = protocol;
	protocols[protocol_count].input = input;
	protocol_count++;
	return 0;
}

static int
ipv4_output_common(struct net_device *device, uint32_t destination,
		   uint8_t protocol, uint32_t source, int source_given,
		   struct packet_buf *packet)
{
	struct net_route route;
	struct ipv4_wire *header;
	uint32_t next_hop;
	uint16_t checksum, total;
	uint8_t hardware[6];
	int error, have_route;

	if (packet == NULL)
		return EINVAL;
	have_route = route_lookup_ref(destination, &route) == 0;
	if (device == NULL)
		device = have_route ? route.device : NULL;
	if (device == NULL) {
		packet_buf_free(packet);
		if (have_route)
			route_release(&route);
		return ENETUNREACH;
	}
	if (packet->length > device->mtu - IPV4_HEADER_MIN) {
		packet_buf_free(packet);
		if (have_route)
			route_release(&route);
		return EMSGSIZE;
	}
	if (!source_given) {
		error = inet_interface_address(device, &source, NULL, NULL);
		if (error != 0) {
			packet_buf_free(packet);
			if (have_route)
				route_release(&route);
			return error;
		}
	}
	next_hop = have_route && route.gateway != 0 ? route.gateway : destination;
	error = arp_resolve(device, next_hop, hardware);
	if (error != 0) {
		(void)arp_request(device, next_hop);
		packet_buf_free(packet);
		if (have_route)
			route_release(&route);
		return EAGAIN;
	}
	header = packet_buf_push(packet, sizeof(*header));
	if (header == NULL) {
		packet_buf_free(packet);
		if (have_route)
			route_release(&route);
		return ENOBUFS;
	}
	memset(header, 0, sizeof(*header));
	header->version_ihl = 0x45U;
	total = (uint16_t)packet->length;
	wire_put16(header->total_length, total);
	wire_put16(header->identification, ++next_identification);
	wire_put16(header->fragment, 0x4000U);
	header->ttl = 64;
	header->protocol = protocol;
	wire_put32(header->source, source);
	wire_put32(header->destination, destination);
	checksum = net_checksum(header, sizeof(*header));
	wire_put16(header->checksum, checksum);
	packet->l3_offset = (uint16_t)(packet->data - packet->storage);
	error = ethernet_output(device, hardware, ETHERNET_TYPE_IPV4, packet);
	if (have_route)
		route_release(&route);
	return error;
}

int
ipv4_output(struct net_device *device, uint32_t destination, uint8_t protocol,
	    struct packet_buf *packet)
{
	return ipv4_output_common(device, destination, protocol, 0, 0, packet);
}

int
ipv4_output_source(struct net_device *device, uint32_t destination,
		   uint8_t protocol, uint32_t source, struct packet_buf *packet)
{
	if (source != 0 || destination != INADDR_BROADCAST) {
		packet_buf_free(packet);
		return EINVAL;
	}
	return ipv4_output_common(device, destination, protocol, source, 1,
	    packet);
}

static int
ipv4_input(struct packet_buf *packet)
{
	const struct ipv4_wire *header;
	uint32_t source, destination, local, broadcast;
	uint16_t total, fragment;
	size_t header_length;
	unsigned index;

	if (packet == NULL || packet->device == NULL ||
	    packet->length < sizeof(*header)) {
		packet_buf_free(packet);
		return EINVAL;
	}
	header = (const struct ipv4_wire *)packet->data;
	header_length = (size_t)(header->version_ihl & 0x0fU) * 4U;
	total = wire_get16(header->total_length);
	fragment = wire_get16(header->fragment);
	if ((header->version_ihl >> 4) != 4U ||
	    header_length < IPV4_HEADER_MIN || header_length > packet->length ||
	    total < header_length || total > packet->length ||
	    net_checksum(header, header_length) != 0 ||
	    (fragment & 0x3fffU) != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}
	source = wire_get32(header->source);
	destination = wire_get32(header->destination);
	if (packet->l3_offset == PACKET_OFFSET_NONE)
		packet->l3_offset = (uint16_t)(packet->data - packet->storage);
	packet->l3_length = total;
	if (destination == INADDR_BROADCAST) {
		if ((packet->device->flags & (NET_DEVICE_UP | NET_DEVICE_RUNNING |
		    NET_DEVICE_BROADCAST)) != (NET_DEVICE_UP | NET_DEVICE_RUNNING |
		    NET_DEVICE_BROADCAST)) {
			packet_buf_free(packet);
			return 0;
		}
	} else if (inet_interface_address(packet->device, &local, NULL,
	    &broadcast) != 0 ||
	    (destination != local && destination != broadcast)) {
		packet_buf_free(packet);
		return 0;
	}
	if (packet_buf_trim(packet, total) != 0 ||
	    packet_buf_pull(packet, header_length) == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}
	packet->l4_offset = (uint16_t)(packet->data - packet->storage);
	for (index = 0; index < protocol_count; index++)
		if (protocols[index].protocol == header->protocol)
			return protocols[index].input(packet, source, destination);
	packet_buf_free(packet);
	return 0;
}

int
ipv4_init(void)
{
	memset(protocols, 0, sizeof(protocols));
	protocol_count = 0;
	next_identification = 0;
	return ethernet_protocol_register(ETHERNET_TYPE_IPV4, ipv4_input);
}
