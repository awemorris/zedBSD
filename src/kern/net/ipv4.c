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
#include <kern/kcrt.h>

#include <uapi/netinet.h>
#include <uapi/errno.h>

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
static int ipv4_accepts_destination(struct net_device *device, uint32_t destination);
static void ipv4_header_checksum(struct net_device *device, struct packet_buf *packet, struct ipv4_wire *header);

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
 * Sends a payload to an IPv4 destination from the device's address the
 * caller has already looked up (TCP sums it into its pseudo-header), so
 * the output does not look it up again; no waiting for ARP.
 */
int
ipv4_output_sourced(
	struct net_device *device,
	uint32_t destination,
	uint8_t protocol,
	uint32_t source,
	struct packet_buf *packet)
{
	int error;

	/* Reports why the send failed. */
	error = ipv4_output_common(device, destination, protocol, source, 1, 0, packet);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Puts the checksum in a header just built, or leaves it to the device.
 *
 * A TCP segment that left its checksum to the device
 * (PACKET_BUF_TX_CSUM_TCP) leaves the header's too when the device the
 * frame goes out of computes both; through any other device (the
 * loopback, one without the capability) both are computed here.
 */
static void
ipv4_header_checksum(
	struct net_device *device,
	struct packet_buf *packet,
	struct ipv4_wire *header)
{
	uint16_t checksum;

	/* The device computes both. */
	if ((packet->flags & PACKET_BUF_TX_CSUM_TCP) != 0 &&
	    (device->capabilities & NET_DEVICE_CAP_TX_CSUM_IPV4_TCP) != 0) {
		packet->flags |= PACKET_BUF_TX_CSUM_IPV4;
		return;
	}

	/* A segment's left to a device that does not is computed with the header's. */
	if ((packet->flags & PACKET_BUF_TX_CSUM_TCP) != 0) {
		packet_buf_checksum_finish(packet);
		return;
	}

	/* The header's alone. */
	checksum = net_checksum(header, sizeof(*header));
	wire_put16(header->checksum, checksum);
}

/*
 * Sends a payload to an IPv4 destination that is not one of this host's
 * own, through a device, from its address and to a next hop the caller
 * found with a route lookup it keeps (TCP keeps them while
 * route_generation() does not move), without looking any of them up
 * again; no waiting for ARP.  The device's reference stays the caller's.
 */
int
ipv4_output_routed(
	struct net_device *device,
	uint32_t destination,
	uint32_t next_hop,
	uint8_t protocol,
	uint32_t source,
	struct packet_buf *packet)
{
	struct ipv4_wire *header;
	uint8_t hardware[6];
	int error;

	/* Rejects a missing packet or device. */
	if (packet == NULL)
		return EINVAL;
	if (device == NULL) {
		packet_buf_free(packet);
		return ENETUNREACH;
	}

	/* Rejects a payload that cannot fit the MTU with a header. */
	if (packet->length > device->mtu - IPV4_HEADER_MIN) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}

	/* The next hop's hardware address; an unresolved one is asked for and the packet refused, as ipv4_output does. */
	error = arp_resolve(device, next_hop, hardware);
	if (error != 0) {
		(void)arp_request(device, next_hop);
		packet_buf_free(packet);
		return EAGAIN;
	}

	/* Prepends the header. */
	header = packet_buf_push(packet, sizeof(*header));
	if (header == NULL) {
		packet_buf_free(packet);
		return ENOBUFS;
	}

	/* Fills in the fixed fields of the header. */
	kern_memset(header, 0, sizeof(*header));
	header->version_ihl = 0x45U;
	wire_put16(header->total_length, (uint16_t)packet->length);
	wire_put16(header->identification, ++next_identification);
	wire_put16(header->fragment, 0x4000U);
	header->ttl = 64;
	header->protocol = protocol;
	wire_put32(header->source, source);
	wire_put32(header->destination, destination);
	packet->l3_offset = (uint16_t)(packet->data - packet->storage);
	ipv4_header_checksum(device, packet, header);

	/* Sends the packet as an Ethernet frame. */
	error = ethernet_output(device, hardware, ETHERNET_TYPE_IPV4, packet);
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
 * Sends a payload from one of this host's own addresses.
 *
 * An answer goes out from the address its question was sent to, which need
 * not be the address of the device that sends it: a question for another
 * interface's address arrives round the loopback device.
 */
int
ipv4_output_from(
	struct net_device *device,
	uint32_t destination,
	uint8_t protocol,
	uint32_t source,
	struct packet_buf *packet)
{
	int own;
	int error;

	/* Refuses a source this host does not own. */
	own = inet_address_is_local(source);
	if (!own) {
		packet_buf_free(packet);
		return EADDRNOTAVAIL;
	}

	/* Builds the header with the source and sends the packet. */
	error = ipv4_output_common(device, destination, protocol, source, 1, 0,
	    packet);
	if (error != 0)
		return error;

	/* Succeeded: the packet is on its way. */
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
	kern_memset(protocols, 0, sizeof(protocols));
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
	struct net_device *loopback;
	struct ipv4_wire *header;
	uint32_t next_hop;
	uint16_t total;
	uint8_t hardware[6];
	int have_route;
	int local;
	int error;

	/* Rejects a missing packet. */
	if (packet == NULL)
		return EINVAL;

	/*
	 * Takes the route, and from it the device when none was given.  A
	 * given device uses only its own routes, so that a socket bound to it
	 * never sends to another device's gateway.
	 */
	have_route = 0;
	if (device != NULL)
		error = route_lookup_device_ref(destination, device, &route);
	else
		error = route_lookup_ref(destination, &route);
	if (error == 0)
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

	/*
	 * A packet for one of this host's own addresses never leaves the host:
	 * it goes round the loopback device, which hands it straight back to
	 * input.  The source stays the one the outgoing device gave it, because
	 * the protocol above has already summed it into its checksum.
	 */
	loopback = NULL;
	local = inet_address_is_local(destination);
	if (local) {
		loopback = net_loopback_ref();
		if (loopback == NULL) {
			packet_buf_free(packet);
			if (have_route)
				route_release(&route);
			return ENETUNREACH;
		}

		/* Sends through the loopback device instead of the route's. */
		device = loopback;
	}

	/*
	 * Resolves the next hop, which is the gateway when the route has one.
	 * The limited broadcast stays on the link and never goes to a gateway.
	 */
	if (destination == INADDR_BROADCAST)
		next_hop = destination;
	else if (have_route && route.gateway != 0)
		next_hop = route.gateway;
	else
		next_hop = destination;
	if (loopback != NULL) {
		/* The loopback device takes its own hardware address. */
		kern_memcpy(hardware, loopback->hwaddr, sizeof(hardware));
		error = 0;
	} else if (wait_for_neighbor) {
		error = arp_resolve_wait(device, next_hop, hardware);
	} else {
		error = arp_resolve(device, next_hop, hardware);
	}
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
		if (loopback != NULL)
			net_device_release(loopback);
		return ENOBUFS;
	}

	kern_memset(header, 0, sizeof(*header));
	header->version_ihl = 0x45U;
	total = (uint16_t)packet->length;
	wire_put16(header->total_length, total);
	wire_put16(header->identification, ++next_identification);
	wire_put16(header->fragment, 0x4000U);
	header->ttl = 64;
	header->protocol = protocol;
	wire_put32(header->source, source);
	wire_put32(header->destination, destination);
	packet->l3_offset = (uint16_t)(packet->data - packet->storage);
	ipv4_header_checksum(device, packet, header);

	/* Sends the packet as an Ethernet frame. */
	error = ethernet_output(device, hardware, ETHERNET_TYPE_IPV4, packet);
	if (have_route)
		route_release(&route);

	/* Drops the hold on the loopback device the packet went round. */
	if (loopback != NULL)
		net_device_release(loopback);

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
	int accepted;
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
	    ((packet->flags & PACKET_BUF_RX_CSUM_IPV4) == 0 && net_checksum(header, header_length) != 0) ||
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
	} else {
		/* ... and packets for the device's own or subnet address. */
		accepted = ipv4_accepts_destination(packet->device, destination);
		if (!accepted) {
			packet_buf_free(packet);
			return 0;
		}
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

/*
 * Tells whether a device keeps a packet sent to a unicast destination.
 *
 * A device keeps packets for its own address and its subnet broadcast.  The
 * loopback device also keeps packets for the addresses of the other
 * interfaces, because output sends those round it instead of onto a wire.
 */
static int
ipv4_accepts_destination(
	struct net_device *device,
	uint32_t destination)
{
	uint32_t local;
	uint32_t broadcast;
	unsigned flags;
	int error;
	int own;

	/* Keeps a packet for the device's own address or its broadcast. */
	error = inet_interface_address(device, &local, NULL, &broadcast);
	if (error == 0) {
		if (destination == local)
			return 1;
		if (destination == broadcast)
			return 1;
	}

	/* Any other device keeps nothing else. */
	flags = net_device_flags_get(device);
	if ((flags & NET_DEVICE_LOOPBACK) == 0)
		return 0;

	/* The loopback device keeps what is addressed to this host. */
	own = inet_address_is_local(destination);
	if (!own)
		return 0;

	/* Succeeded: the packet is for this host. */
	return 1;
}
