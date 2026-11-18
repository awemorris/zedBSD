/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ICMPv6 (ws130-p002; RFC 4443): the checksum, the echo's answer, the
 * error messages the layer sends (rate-limited, never about an error or a
 * group but where the RFC allows it), the path MTU a Packet Too Big
 * teaches, and the hand-off of neighbor discovery and multicast listener
 * messages.  Every message is also copied to the raw ICMPv6 sockets, and
 * an error about a UDP datagram or a TCP segment this host sent is told to
 * the transport (ws130-p003).
 */

#include "ipv6.h"

#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"
#include "kern/lock.h"
#include "internal.h"
#include "wire.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <uapi/netinet.h>

/* The most of an invoking packet an error carries: the minimum MTU less the two headers. */
#define ICMP6_ERROR_PAYLOAD_MAX	(IPV6_MINIMUM_MTU - IPV6_HEADER_LENGTH - sizeof(struct icmp6_wire))

/* At most this many errors a second (RFC 4443 section 2.4 (f)). */
#define ICMP6_ERRORS_PER_SECOND	10U

/* The destinations whose path MTU is remembered. */
#define ICMP6_PATHS_MAX		16U

/* A destination's path MTU and when it is forgotten (0: a free entry). */
struct icmp6_path {
	struct in6_addr destination;
	unsigned mtu;
	uint64_t expires_ms;
};

/* Guards the path MTUs and the error budget. */
static struct spinlock icmp6_lock;

/* The path MTUs. */
static struct icmp6_path icmp6_paths[ICMP6_PATHS_MAX];

/* The errors sent in the current second, and the second. */
static unsigned icmp6_errors_sent;
static uint64_t icmp6_errors_second;

static void icmp6_echo(struct packet_buf *packet, const struct in6_addr *source, const struct in6_addr *destination);
static void icmp6_too_big(struct packet_buf *packet);
static int icmp6_error_allowed(void);
static void icmp6_transport_error(struct packet_buf *packet);
static int icmp6_error_number(uint8_t type, uint8_t code);

/* Makes the lock and empties the path MTUs and the error budget (ipv6_init). */
void
icmp6_init(
	void)
{
	spin_init(&icmp6_lock, LOCK_RANK_NETWORK, "ICMPv6");
	kern_memset(icmp6_paths, 0, sizeof(icmp6_paths));
	icmp6_errors_sent = 0U;
	icmp6_errors_second = 0U;
}

/*
 * Receives an ICMPv6 message (packet->data is its first byte); the packet
 * is always consumed.
 */
int
icmp6_input(
	struct packet_buf *packet,
	const struct in6_addr *source,
	const struct in6_addr *destination)
{
	const struct icmp6_wire *message;
	const struct ipv6_wire *header;
	uint16_t sum;
	unsigned hop_limit;

	/* A whole header and a right checksum. */
	if (packet->length < sizeof(*message)) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* The checksum. */
	sum = net_checksum_pseudo6(source->s6_addr, destination->s6_addr, IPV6_NEXT_ICMPV6, packet->data, packet->length);
	if (sum != 0U) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* A copy for the raw ICMPv6 sockets (ws130-p003). */
	icmp6_raw_deliver(packet, source, destination);

	/* The hop limit it came with (neighbor discovery's messages need 255). */
	header = (const struct ipv6_wire *)(packet->storage + packet->l3_offset);
	hop_limit = header->hop_limit;
	message = (const struct icmp6_wire *)packet->data;

	/* Each type. */
	switch (message->type) {
	case ICMP6_ECHO_REQUEST:
		icmp6_echo(packet, source, destination);
		return 0;
	case ICMP6_PACKET_TOO_BIG:
		icmp6_too_big(packet);
		return 0;
	case ICMP6_DESTINATION_UNREACHABLE:
	case ICMP6_TIME_EXCEEDED:
	case ICMP6_PARAMETER_PROBLEM:
		icmp6_transport_error(packet);
		return 0;
	case ICMP6_MLD_QUERY:
	case ICMP6_MLD1_REPORT:
	case ICMP6_MLD1_DONE:
	case ICMP6_MLD2_REPORT:
		mld6_input(packet, source, destination);
		return 0;
	case ICMP6_ROUTER_SOLICITATION:
	case ICMP6_ROUTER_ADVERTISEMENT:
	case ICMP6_NEIGHBOR_SOLICITATION:
	case ICMP6_NEIGHBOR_ADVERTISEMENT:
	case ICMP6_REDIRECT:
		nd6_input(packet, source, destination, hop_limit);
		return 0;
	default:
		break;
	}

	/* Anything else (the echo's answers): for the raw sockets only. */
	packet_buf_free(packet);
	return 0;
}

/*
 * Sends an ICMPv6 message (packet->data is its first byte, the checksum
 * field anything): the source chosen when source is NULL, the checksum
 * computed, and the message given to IPv6.  The packet is consumed.
 */
int
icmp6_send(
	struct net_device *device,
	const struct in6_addr *source,
	const struct in6_addr *destination,
	unsigned hop_limit,
	struct packet_buf *packet)
{
	struct icmp6_wire *message;
	struct in6_addr chosen;
	uint16_t sum;
	int error;

	/* The source the checksum covers. */
	if (source == NULL) {
		error = ipv6_source_select(device, destination, &chosen);
		if (error != 0) {
			packet_buf_free(packet);
			return error;
		}

		/* The chosen one. */
		source = &chosen;
	}

	/* The checksum. */
	message = (struct icmp6_wire *)packet->data;
	message->checksum[0] = 0U;
	message->checksum[1] = 0U;
	sum = net_checksum_pseudo6(source->s6_addr, destination->s6_addr, IPV6_NEXT_ICMPV6, packet->data, packet->length);
	wire_put16(message->checksum, sum);

	/* Sent. */
	error = ipv6_output(device, source, destination, IPV6_NEXT_ICMPV6, hop_limit, packet);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Answers a packet with an error (RFC 4443 section 2.4): never about an
 * error message, a packet from :: or a group, or (but a Packet Too Big or
 * an option's Parameter Problem) a packet to a group; and at most
 * ICMP6_ERRORS_PER_SECOND a second.  The error carries as much of the
 * invoking packet (from its IPv6 header, packet->l3_offset) as the
 * minimum MTU allows.  The packet is consumed.
 */
void
icmp6_error(
	struct packet_buf *packet,
	uint8_t type,
	uint8_t code,
	uint32_t data)
{
	const struct ipv6_wire *header;
	const uint8_t *invoking;
	struct icmp6_wire *message;
	struct packet_buf *answer;
	struct in6_addr source;
	struct in6_addr destination;
	struct net_device *device;
	size_t length;
	size_t offset;
	unsigned flags;
	int multicast;
	int unspecified;
	int allowed;
	int own;

	/* The invoking packet's header and length. */
	header = (const struct ipv6_wire *)(packet->storage + packet->l3_offset);
	invoking = packet->storage + packet->l3_offset;
	length = packet->l3_length;
	kern_memcpy(source.s6_addr, header->source, 16U);
	kern_memcpy(destination.s6_addr, header->destination, 16U);

	/* Not about a packet from nowhere, from a group, or (mostly) to a group. */
	unspecified = in6_is_unspecified(&source);
	multicast = in6_is_multicast(&source);
	if (unspecified || multicast) {
		packet_buf_free(packet);
		return;
	}

	/* Not to a group, but where the RFC allows it. */
	multicast = in6_is_multicast(&destination);
	if (multicast && type != ICMP6_PACKET_TOO_BIG && !(type == ICMP6_PARAMETER_PROBLEM && code == ICMP6_PARAMETER_OPTION)) {
		packet_buf_free(packet);
		return;
	}

	/* Not about an error message (an ICMPv6 type below 128 right after the header). */
	offset = IPV6_HEADER_LENGTH;
	if (header->next_header == IPV6_NEXT_ICMPV6 && length > offset && invoking[offset] < 128U) {
		packet_buf_free(packet);
		return;
	}

	/* Within the budget. */
	allowed = icmp6_error_allowed();
	if (!allowed) {
		packet_buf_free(packet);
		return;
	}

	/* The answer: the header and the start of the invoking packet. */
	if (length > ICMP6_ERROR_PAYLOAD_MAX)
		length = ICMP6_ERROR_PAYLOAD_MAX;
	answer = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	message = NULL;
	if (answer != NULL)
		message = packet_buf_append(answer, sizeof(*message) + length);
	if (message == NULL) {
		if (answer != NULL)
			packet_buf_free(answer);
		packet_buf_free(packet);
		return;
	}

	/* The error's type, code and data. */
	message->type = type;
	message->code = code;
	wire_put32(message->data, data);
	kern_memcpy((uint8_t *)(message + 1), invoking, length);

	/* From the address it was sent to when that is the host's, to its source, out of the interface it came in on. */
	device = packet->device;
	net_device_ref(device);
	packet_buf_free(packet);
	own = ipv6_address_state(device, &destination, &flags);
	if (own == 0 && !multicast)
		(void)icmp6_send(device, &destination, &source, 0U, answer);
	else
		(void)icmp6_send(device, NULL, &source, 0U, answer);
	net_device_release(device);
}

/* Gives the MTU of the path to a destination: the link's, or less when a Packet Too Big said so. */
unsigned
icmp6_path_mtu(
	const struct in6_addr *destination,
	unsigned link_mtu)
{
	unsigned long irq;
	unsigned index;
	unsigned mtu;
	uint64_t now;
	int same;

	/* A remembered path. */
	now = ipv6_now_ms();
	mtu = link_mtu;
	irq = spin_lock_irqsave(&icmp6_lock);

	for (index = 0U; index < ICMP6_PATHS_MAX; index++) {
		if (icmp6_paths[index].expires_ms == 0U || now >= icmp6_paths[index].expires_ms)
			continue;
		same = in6_equal(&icmp6_paths[index].destination, destination);
		if (same && icmp6_paths[index].mtu < mtu)
			mtu = icmp6_paths[index].mtu;
	}

	spin_unlock_irqrestore(&icmp6_lock, irq);

	/* Succeeded: the MTU. */
	return mtu;
}

/* Forgets every remembered path (the timer's housekeeping is the expiry; this is for a reset). */
void
icmp6_path_mtu_purge(
	void)
{
	unsigned long irq;

	/* Every entry. */
	irq = spin_lock_irqsave(&icmp6_lock);

	kern_memset(icmp6_paths, 0, sizeof(icmp6_paths));

	spin_unlock_irqrestore(&icmp6_lock, irq);
}

/* Answers an echo request with its reply: the same identifier, sequence and data, back to the sender. */
static void
icmp6_echo(
	struct packet_buf *packet,
	const struct in6_addr *source,
	const struct in6_addr *destination)
{
	int alloc_error;
	struct icmp6_wire *message;
	struct packet_buf *reply;
	struct net_device *device;
	int multicast;

	/* A copy of the request, made the reply. */
	reply = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM, packet->length, 0, &alloc_error);
	message = NULL;
	if (reply != NULL)
		message = packet_buf_append(reply, packet->length);
	if (message == NULL) {
		if (reply != NULL)
			packet_buf_free(reply);
		packet_buf_free(packet);
		return;
	}

	/* The reply's type. */
	kern_memcpy(message, packet->data, packet->length);
	message->type = ICMP6_ECHO_REPLY;
	message->code = 0U;

	/* Back out of the interface it came in on, from the address it was sent to (a group's: chosen). */
	device = packet->device;
	net_device_ref(device);
	packet_buf_free(packet);
	multicast = in6_is_multicast(destination);
	if (multicast)
		(void)icmp6_send(device, NULL, source, 0U, reply);
	else
		(void)icmp6_send(device, destination, source, 0U, reply);
	net_device_release(device);
}

/*
 * Takes a Packet Too Big: the path to the invoking packet's destination
 * has the MTU it gives (never under the minimum), for a while.  Only a
 * packet this host sent is believed (its source one of the host's own),
 * and an estimate is never raised by one (RFC 8201 section 4): a larger
 * MTU than the one learnt leaves it.
 */
static void
icmp6_too_big(
	struct packet_buf *packet)
{
	const struct icmp6_wire *message;
	const struct ipv6_wire *inner;
	struct in6_addr destination;
	struct in6_addr source;
	unsigned long irq;
	unsigned index;
	unsigned slot;
	unsigned mtu;
	uint64_t now;
	int lowered;
	int same;
	int own;

	/* The message and the start of the packet that was too big. */
	if (packet->length < sizeof(*message) + sizeof(*inner)) {
		packet_buf_free(packet);
		return;
	}

	/* Its MTU and the destination. */
	message = (const struct icmp6_wire *)packet->data;
	inner = (const struct ipv6_wire *)(packet->data + sizeof(*message));
	mtu = wire_get32(message->data);
	kern_memcpy(destination.s6_addr, inner->destination, 16U);
	kern_memcpy(source.s6_addr, inner->source, 16U);
	packet_buf_free(packet);
	if (mtu < IPV6_MINIMUM_MTU)
		mtu = IPV6_MINIMUM_MTU;

	/* A packet another host sent is not this host's path to learn. */
	own = ipv6_address_is_local(&source);
	if (!own)
		return;

	/* The destination's entry, a free one, or the one that runs out first. */
	now = ipv6_now_ms();
	irq = spin_lock_irqsave(&icmp6_lock);

	slot = 0U;
	for (index = 0U; index < ICMP6_PATHS_MAX; index++) {
		same = in6_equal(&icmp6_paths[index].destination, &destination);
		if (same && icmp6_paths[index].expires_ms != 0U) {
			slot = index;
			break;
		}

		/* An entry that runs out sooner is the one to reuse. */
		if (icmp6_paths[index].expires_ms < icmp6_paths[slot].expires_ms)
			slot = index;
	}

	/* An estimate that is still held and no larger stays as it is. */
	lowered = 1;
	same = in6_equal(&icmp6_paths[slot].destination, &destination);
	if (same && now < icmp6_paths[slot].expires_ms && icmp6_paths[slot].mtu <= mtu)
		lowered = 0;

	/* The path's MTU, for a while. */
	if (lowered) {
		icmp6_paths[slot].destination = destination;
		icmp6_paths[slot].mtu = mtu;
		icmp6_paths[slot].expires_ms = now + IPV6_PATH_MTU_MS;
	}

	spin_unlock_irqrestore(&icmp6_lock, irq);

	/* Nothing learnt. */
	if (!lowered)
		return;

	/*
	 * What senders kept of the path is looked up again, and TCP's
	 * connections to the destination cut their segments to it (RFC 8201
	 * section 4).
	 */
	route_changed();
	tcp6_path_mtu_lowered(&destination, mtu);
}

/* Tells whether one more error may be sent this second, and counts it. */
static int
icmp6_error_allowed(
	void)
{
	unsigned long irq;
	uint64_t second;
	int allowed;

	/* The current second's count. */
	second = ipv6_now_ms() / 1000U;
	irq = spin_lock_irqsave(&icmp6_lock);

	if (second != icmp6_errors_second) {
		icmp6_errors_second = second;
		icmp6_errors_sent = 0U;
	}

	/* One more error, within the budget. */
	allowed = 0;
	if (icmp6_errors_sent < ICMP6_ERRORS_PER_SECOND) {
		icmp6_errors_sent++;
		allowed = 1;
	}

	spin_unlock_irqrestore(&icmp6_lock, irq);

	/* Succeeded: allowed or not. */
	return allowed;
}

/*
 * Tells the transport of an error about a datagram or segment this host
 * sent: the invoking packet's IPv6 header (with no extension header) and
 * its first ports, after the message's header.  The packet is consumed.
 */
static void
icmp6_transport_error(
	struct packet_buf *packet)
{
	const struct icmp6_wire *message;
	const struct ipv6_wire *inner;
	const uint8_t *ports;
	struct in6_addr source;
	struct in6_addr destination;
	uint16_t source_port;
	uint16_t destination_port;
	uint8_t next_header;
	int error;

	/* The message, the invoking packet's header and its ports. */
	if (packet->length < sizeof(*message) + sizeof(*inner) + 4U) {
		packet_buf_free(packet);
		return;
	}

	message = (const struct icmp6_wire *)packet->data;
	inner = (const struct ipv6_wire *)(packet->data + sizeof(*message));
	ports = packet->data + sizeof(*message) + sizeof(*inner);
	kern_memcpy(source.s6_addr, inner->source, 16U);
	kern_memcpy(destination.s6_addr, inner->destination, 16U);
	next_header = inner->next_header;
	source_port = wire_get16(ports);
	destination_port = wire_get16(ports + 2);
	error = icmp6_error_number(message->type, message->code);
	packet_buf_free(packet);

	/* The transport it was sent by. */
	if (next_header == IPPROTO_UDP)
		udp6_error(&source, &destination, source_port, destination_port, error);
	else if (next_header == IPPROTO_TCP)
		tcp6_error(&source, &destination, source_port, destination_port, error);
}

/* Gives the errno value an ICMPv6 error stands for (as the other systems give it). */
static int
icmp6_error_number(
	uint8_t type,
	uint8_t code)
{
	/* Time Exceeded: the hops ran out, or the reassembly did. */
	if (type == ICMP6_TIME_EXCEEDED)
		return EHOSTUNREACH;

	/* Parameter Problem: the packet was malformed for the receiver. */
	if (type == ICMP6_PARAMETER_PROBLEM)
		return EPROTO;

	/* Destination Unreachable, by its code (RFC 4443 section 3.1). */
	switch (code) {
	case 0U:
		return ENETUNREACH;
	case 1U:
		return EACCES;
	case 4U:
		return ECONNREFUSED;
	default:
		break;
	}

	/* Succeeded: another code is the host's being unreachable. */
	return EHOSTUNREACH;
}
