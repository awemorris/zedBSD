/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * IPv4 input and output.
 *
 * Output resolves the route and the next hop's hardware address, builds a
 * header without options or fragmentation, and hands the packet to
 * Ethernet.  Input validates the header, keeps only packets for a local
 * or broadcast address, and dispatches by protocol.
 */

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

static int ipv4_output_common(struct net_device *device, uint32_t destination, uint8_t protocol, uint32_t source, int source_given, int wait_for_neighbor, struct packet_buf *packet);
static int ipv4_input(struct packet_buf *packet);

/*
 * Registers the input function for an IP protocol number.
 */
int
ipv4_protocol_register(
	uint8_t protocol,
	ipv4_input_fn input)
{
	unsigned index;

	/* Rejects an empty protocol or a missing input function. */
	if (protocol == 0 || input == NULL)
		return EINVAL;

	/* Refuses a protocol that is already registered. */
	for (index = 0; index < protocol_count; index++) {
		if (protocols[index].protocol == protocol)
			return EEXIST;
	}

	/* Appends the protocol to the table. */
	if (protocol_count >= IPV4_PROTOCOL_MAX)
		return ENOSPC;
	protocols[protocol_count].protocol = protocol;
	protocols[protocol_count].input = input;
	protocol_count++;

	/* Reports the registration. */
	return 0;
}

/*
 * Sends a payload to an IPv4 destination without waiting for ARP.
 *
 * An unresolved next hop gets an ARP request and reports EAGAIN.
 */
int
ipv4_output(
	struct net_device *device,
	uint32_t destination,
	uint8_t protocol,
	struct packet_buf *packet)
{
	int error;

	/* Reports why the send failed. */
	error = ipv4_output_common(device, destination, protocol, 0, 0, 0, packet);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sends a payload to an IPv4 destination, waiting for ARP resolution.
 */
int
ipv4_output_wait(
	struct net_device *device,
	uint32_t destination,
	uint8_t protocol,
	struct packet_buf *packet)
{
	int error;

	/* Reports why the send failed. */
	error = ipv4_output_common(device, destination, protocol, 0, 0, 1, packet);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sends a broadcast with an explicit source address.
 *
 * Only the unspecified source to the limited broadcast is allowed, as a
 * DHCP client needs before it has an address.
 */
int
ipv4_output_source(
	struct net_device *device,
	uint32_t destination,
	uint8_t protocol,
	uint32_t source,
	struct packet_buf *packet)
{
	int error;

	/* Rejects anything but the unspecified source to the broadcast. */
	if (source != 0 || destination != INADDR_BROADCAST) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Reports why the send failed. */
	error = ipv4_output_common(device, destination, protocol, source, 1, 0,
	    packet);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Initializes IPv4 and registers it with Ethernet.
 */
int
ipv4_init(
	void)
{
	int error;

	/* Starts with an empty protocol table. */
	memset(protocols, 0, sizeof(protocols));
	protocol_count = 0;
	next_identification = 0;

	/* Receives IPv4 frames from Ethernet. */

	/* Reports why the registration failed. */
	error = ethernet_protocol_register(ETHERNET_TYPE_IPV4, ipv4_input);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Builds and sends one IPv4 packet; the packet is always consumed. */
static int
ipv4_output_common(
	struct net_device *device,
	uint32_t destination,
	uint8_t protocol,
	uint32_t source,
	int source_given,
	int wait_for_neighbor,
	struct packet_buf *packet)
{
	struct net_route route;
	struct ipv4_wire *header;
	uint32_t next_hop;
	uint16_t checksum;
	uint16_t total;
	uint8_t hardware[6];
	int have_route;
	int error;

	/* Rejects a missing packet. */
	if (packet == NULL)
		return EINVAL;

	/* Takes the route, and from it the device when none was given. */
	have_route = 0;
	if (route_lookup_ref(destination, &route) == 0)
		have_route = 1;
	if (device == NULL && have_route)
		device = route.device;
	if (device == NULL) {
		packet_buf_free(packet);
		if (have_route)
			route_release(&route);
		return ENETUNREACH;
	}

	/* Rejects a payload that cannot fit the MTU with a header. */
	if (packet->length > device->mtu - IPV4_HEADER_MIN) {
		packet_buf_free(packet);
		if (have_route)
			route_release(&route);
		return EMSGSIZE;
	}

	/* Takes the device address as the source unless one was given. */
	if (!source_given) {
		error = inet_interface_address(device, &source, NULL, NULL);
		if (error != 0) {
			packet_buf_free(packet);
			if (have_route)
				route_release(&route);
			return error;
		}
	}

	/* Resolves the next hop, which is the gateway when the route has one. */
	if (have_route && route.gateway != 0)
		next_hop = route.gateway;
	else
		next_hop = destination;
	if (wait_for_neighbor)
		error = arp_resolve_wait(device, next_hop, hardware);
	else
		error = arp_resolve(device, next_hop, hardware);
	if (error != 0) {
		/* Without waiting, starts the resolution for a later retry. */
		if (!wait_for_neighbor)
			(void)arp_request(device, next_hop);
		packet_buf_free(packet);
		if (have_route)
			route_release(&route);
		if (wait_for_neighbor)
			return error;
		return EAGAIN;
	}

	/* Prepends the header. */
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

	/* Sends the packet as an Ethernet frame. */
	error = ethernet_output(device, hardware, ETHERNET_TYPE_IPV4, packet);
	if (have_route)
		route_release(&route);

	/* Reports why the send failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Receives an IPv4 packet from Ethernet; the packet is always consumed. */
static int
ipv4_input(
	struct packet_buf *packet)
{
	const struct ipv4_wire *header;
	uint32_t source;
	uint32_t destination;
	uint32_t local;
	uint32_t broadcast;
	uint16_t total;
	uint16_t fragment;
	size_t header_length;
	unsigned index;
	int error;

	/* Drops a packet without a device or a complete header. */
	if (packet == NULL ||
	    packet->device == NULL ||
	    packet->length < sizeof(*header)) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Drops a malformed, corrupt, or fragmented packet. */
	header = (const struct ipv4_wire *)packet->data;
	header_length = (size_t)(header->version_ihl & 0x0fU) * 4U;
	total = wire_get16(header->total_length);
	fragment = wire_get16(header->fragment);
	if ((header->version_ihl >> 4) != 4U ||
	    header_length < IPV4_HEADER_MIN ||
	    header_length > packet->length ||
	    total < header_length ||
	    total > packet->length ||
	    net_checksum(header, header_length) != 0 ||
	    (fragment & 0x3fffU) != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Records the addresses and the network layer position. */
	source = wire_get32(header->source);
	destination = wire_get32(header->destination);
	if (packet->l3_offset == PACKET_OFFSET_NONE)
		packet->l3_offset = (uint16_t)(packet->data - packet->storage);
	packet->l3_length = total;

	/* Keeps only broadcasts on a broadcast-capable running device ... */
	if (destination == INADDR_BROADCAST) {
		if ((net_device_flags_get(packet->device) &
		     (NET_DEVICE_UP | NET_DEVICE_RUNNING |
		    NET_DEVICE_BROADCAST)) != (NET_DEVICE_UP | NET_DEVICE_RUNNING |
		    NET_DEVICE_BROADCAST)) {
			packet_buf_free(packet);
			return 0;
		}
	} else if (inet_interface_address(packet->device, &local, NULL,
	    &broadcast) != 0 ||
	    (destination != local && destination != broadcast)) {
		/* ... and packets for the device's own or subnet address. */
		packet_buf_free(packet);
		return 0;
	}

	/* Trims the frame padding and strips the header. */
	if (packet_buf_trim(packet, total) != 0 ||
	    packet_buf_pull(packet, header_length) == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}

	packet->l4_offset = (uint16_t)(packet->data - packet->storage);

	/* Hands the payload to its protocol. */
	for (index = 0; index < protocol_count; index++) {
		if (protocols[index].protocol == header->protocol) {
			error = protocols[index].input(packet, source, destination);
			return error;
		}
	}

	/* Drops a packet of an unregistered protocol. */
	packet_buf_free(packet);

	/* Reports the dropped packet. */
	return 0;
}
