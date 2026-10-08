/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ICMP and raw ICMP sockets.
 *
 * Echo requests are answered in place.  Every ICMP packet is also copied,
 * with its IP header, to the raw sockets whose local and remote address
 * filters match, and a raw socket sends messages with the checksum
 * filled in.
 *
 * The raw ICMPv6 sockets (AF_INET6, IPPROTO_ICMPV6; ws130-p003) are the
 * same sockets of the second family: every ICMPv6 message is copied to
 * them without its IPv6 header (RFC 3542 section 3), and they send
 * messages whose checksum, over the IPv6 pseudo-header, is filled in.
 * One that asks with IPV6_RECVHOPLIMIT (ws177-p044) gets the hop limit
 * each message came with through recvmsg.
 */

#include "kern/net/inet-socket.h"
#include "kern/net/byteorder.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/kmem.h"
#include "internal.h"
#include "ipv6.h"
#include "wire.h"
#include <kern/kcrt.h>

#include <uapi/netinet.h>
#include <uapi/errno.h>

#define ICMP_ECHO_REPLY   0U
#define ICMP_ECHO_REQUEST 8U

/*
 * A raw ICMP or ICMPv6 socket: its inet part, the next in the registry,
 * and whether it asked for the hop limit of each message (IPV6_RECVHOPLIMIT;
 * written by its setsockopt, read as messages are copied to it).
 */
struct icmp_endpoint {
	struct inet_socket inet;
	struct icmp_endpoint *next;
	int receive_hop_limit;
};

static struct icmp_endpoint *icmp_sockets;
static struct spinlock icmp_registry_lock;

static struct icmp_endpoint *icmp_endpoint(struct socket *socket);
static int icmp_bind(struct socket *socket, const struct sockaddr *address, socklen_t length);
static int icmp_connect(struct socket *socket, const struct sockaddr *address, socklen_t length, unsigned io_flags);
static ssize_t icmp_sendto(struct socket *socket, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
static ssize_t icmp_recvfrom(struct socket *socket, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length);
static ssize_t icmp_recvfrom_hop(struct socket *socket, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length, int *hop_limit);
static int icmp_setsockopt(struct socket *socket, int level, int option, const void *value, socklen_t length);
static int icmp_getsockname(struct socket *socket, struct sockaddr *address, socklen_t *length);
static int icmp_getpeername(struct socket *socket, struct sockaddr *address, socklen_t *length);
static void icmp_close(struct socket *socket);
static void icmp_deliver(struct packet_buf *packet, uint32_t source, uint32_t destination);
static int icmp_input(struct packet_buf *packet, uint32_t source, uint32_t destination);
static int icmp_register(int protocol, struct socket **result);
static ssize_t icmp6_sendto(struct icmp_endpoint *endpoint, const void *buffer, size_t length, const struct sockaddr *address, socklen_t address_length);
static int icmp6_accepts(const struct icmp_endpoint *endpoint, const struct in6_addr *source, const struct in6_addr *destination);

static const struct socket_ops icmp_ops = {
	.bind = icmp_bind,
	.connect = icmp_connect,
	.sendto = icmp_sendto,
	.recvfrom = icmp_recvfrom,
	.recvfrom_hop = icmp_recvfrom_hop,
	.setsockopt = icmp_setsockopt,
	.getsockname = icmp_getsockname,
	.getpeername = icmp_getpeername,
	.ioctl = inet_socket_ioctl,
	.close = icmp_close,
};

/*
 * Creates a raw ICMP socket.
 */
int
icmp_socket_create(
	int protocol,
	struct socket **result)
{
	int error;

	/* Rejects a missing result or another protocol. */
	if (result == NULL || (protocol != 0 && protocol != IPPROTO_ICMP))
		return EPROTONOSUPPORT;

	/* Reports why the socket could not be made. */
	error = icmp_register(IPPROTO_ICMP, result);
	if (error != 0)
		return error;

	/* Succeeded: the created socket. */
	return 0;
}

/*
 * Creates a raw ICMPv6 socket (ws130-p003); the caller makes it an
 * AF_INET6 one.
 */
int
icmp6_socket_create(
	int protocol,
	struct socket **result)
{
	int error;

	/* Rejects a missing result or another protocol. */
	if (result == NULL || (protocol != 0 && protocol != IPPROTO_ICMPV6))
		return EPROTONOSUPPORT;

	/* Reports why the socket could not be made. */
	error = icmp_register(IPPROTO_ICMPV6, result);
	if (error != 0)
		return error;

	/* Succeeded: the created socket. */
	return 0;
}

/*
 * Queues a copy of an ICMPv6 message (from its ICMPv6 header, without the
 * IPv6 one) on every raw ICMPv6 socket whose addresses match, named by
 * its sender with the interface of a link-local one (ws130-p003).
 */
void
icmp6_raw_deliver(
	struct packet_buf *packet,
	const struct in6_addr *source,
	const struct in6_addr *destination)
{
	struct icmp_endpoint *endpoint;
	struct icmp_endpoint *snapshot[SOCKET_BROADCAST_MAX];
	const struct ipv6_wire *header;
	struct packet_buf *copy;
	unsigned count;
	unsigned index;
	unsigned arrived;
	unsigned long irq;
	int referenced;
	int accepts;

	count = 0;

	/* References every ICMPv6 socket under the registry lock. */
	irq = spin_lock_irqsave(&icmp_registry_lock);

	for (endpoint = icmp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		/* Stops at a full snapshot, which creation keeps from happening. */
		if (count >= SOCKET_BROADCAST_MAX)
			break;

		/* Only the second family's sockets, and not one whose closing has begun. */
		if (endpoint->inet.family != AF_INET6)
			continue;
		referenced = socket_tryref(&endpoint->inet.socket);
		if (!referenced)
			continue;

		/* Keeps the referenced socket in the snapshot. */
		snapshot[count] = endpoint;
		count++;
	}

	spin_unlock_irqrestore(&icmp_registry_lock, irq);

	/* The interface it came in on, for a link-local sender's name. */
	arrived = 0;
	if (packet->device != NULL)
		arrived = packet->device->ifindex;

	/* Queues a copy on each socket whose addresses match. */
	for (index = 0; index < count; index++) {
		/* Skips a socket bound or connected to other addresses. */
		endpoint = snapshot[index];
		accepts = icmp6_accepts(endpoint, source, destination);
		if (!accepts) {
			socket_release(&endpoint->inet.socket);
			continue;
		}

		/* Copies the message. */
		copy = packet_buf_copy_region(packet, (size_t)(packet->data - packet->storage), packet->length);
		if (copy == NULL) {
			socket_release(&endpoint->inet.socket);
			continue;
		}

		/* The hop limit it came with, for a socket that asked (ws177-p044). */
		if (endpoint->receive_hop_limit && packet->l3_offset != PACKET_OFFSET_NONE) {
			header = (const struct ipv6_wire *)(packet->storage + packet->l3_offset);
			copy->hop_limit = header->hop_limit;
			copy->hop_limit_known = 1;
		}

		/* Names the sender, and queues the copy. */
		copy->l3_offset = PACKET_OFFSET_NONE;
		copy->l4_offset = 0;
		inet_socket_peer_name(&endpoint->inet, 0, source, arrived, 0, copy->source_address, &copy->source_length);
		(void)socket_enqueue_packet(&endpoint->inet.socket, copy);
		socket_release(&endpoint->inet.socket);
	}
}

/*
 * Initializes the socket registry and registers ICMP with IPv4.
 */
int
icmp_init(
	void)
{
	int error;

	/* Starts with no sockets. */
	icmp_sockets = NULL;
	spin_init(&icmp_registry_lock, LOCK_RANK_SOCKET_REGISTRY,
	    "ICMP socket registry");

	/* Receives ICMP packets from IPv4. */

	/* Reports why the registration failed. */
	error = ipv4_protocol_register(IPPROTO_ICMP, icmp_input);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Converts a socket to its ICMP endpoint. */
static struct icmp_endpoint *
icmp_endpoint(
	struct socket *socket)
{
	return (struct icmp_endpoint *)socket;
}

/* Binds the socket to a local address. */
static int
icmp_bind(
	struct socket *socket,
	const struct sockaddr *address,
	socklen_t length)
{
	struct icmp_endpoint *endpoint;
	int error;

	endpoint = icmp_endpoint(socket);

	/* Reports why the bind failed. */
	error = inet_socket_bind(&endpoint->inet, address, length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Sets the remote address the socket sends to and receives from. */
static int
icmp_connect(
	struct socket *socket,
	const struct sockaddr *address,
	socklen_t length,
	unsigned io_flags)
{
	struct icmp_endpoint *endpoint;
	int error;

	(void)io_flags;

	endpoint = icmp_endpoint(socket);

	/* Reports why the connect failed. */
	error = inet_socket_connect(&endpoint->inet, address, length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Sends an ICMP message, filling in its checksum. */
static ssize_t
icmp_sendto(
	struct socket *socket,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length)
{
	struct icmp_endpoint *endpoint;
	struct sockaddr_in destination_address;
	struct packet_buf *packet;
	struct net_device *device;
	uint8_t *payload;
	uint32_t destination;
	uint16_t checksum;
	ssize_t sent;
	int error;

	endpoint = icmp_endpoint(socket);

	/* Rejects unsupported flags, a missing buffer, or a headerless message. */
	if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0 ||
	    buffer == NULL ||
	    length < sizeof(struct icmp_wire))
		return -EINVAL;

	/* An ICMPv6 socket sends over IPv6 (ws130-p003). */
	if (endpoint->inet.family == AF_INET6) {
		sent = icmp6_sendto(endpoint, buffer, length, address, address_length);
		if (sent < 0)
			return sent;
		return sent;
	}

	/* Takes the destination from the address, else from the connection. */
	if (address != NULL) {
		if (address_length < sizeof(destination_address) ||
		    address->sa_family != AF_INET)
			return -EINVAL;
		kern_memcpy(&destination_address, address, sizeof(destination_address));
		destination = net_ntohl(destination_address.sin_addr.s_addr);
	} else if (endpoint->inet.inet_flags & INET_SOCKET_CONNECTED) {
		destination = endpoint->inet.remote_address;
	} else {
		return -EDESTADDRREQ;
	}

	/* Copies the message into a packet. */
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	if (packet == NULL)
		return -ENOBUFS;
	payload = packet_buf_append(packet, length);
	if (payload == NULL) {
		packet_buf_free(packet);
		return -EMSGSIZE;
	}

	kern_memcpy(payload, buffer, length);

	/* Computes the checksum over the message with the field cleared. */
	payload[2] = 0;
	payload[3] = 0;
	checksum = net_checksum(payload, length);
	payload[2] = (uint8_t)(checksum >> 8);
	payload[3] = (uint8_t)checksum;

	/* Sends through the bound interface, or the routed one. */
	if (endpoint->inet.ifindex != 0)
		device = net_device_find_by_index_ref(endpoint->inet.ifindex);
	else
		device = NULL;
	error = ipv4_output(device, destination, IPPROTO_ICMP, packet);
	net_device_release(device);

	/* Reports the sent length or the error. */
	if (error == 0)
		return (ssize_t)length;
	return -error;
}

/* Receives one queued ICMP packet with its IP header. */
static ssize_t
icmp_recvfrom(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length)
{
	ssize_t result;
	int hop_limit;

	/* The message, its hop limit not asked for. */
	result = icmp_recvfrom_hop(socket, buffer, length, flags, address, address_length, &hop_limit);
	return result;
}

/*
 * Takes the next queued message: as much of it as fits, its sender, and
 * the hop limit it came with when the socket asked for it (-1 otherwise).
 */
static ssize_t
icmp_recvfrom_hop(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length,
	int *hop_limit)
{
	struct packet_buf *packet;
	size_t copied;
	socklen_t actual;
	socklen_t output;
	ssize_t result;
	int error;

	/* No hop limit unless the message kept one. */
	*hop_limit = -1;

	/* Rejects unsupported flags. */
	if ((flags & ~(MSG_DONTWAIT | MSG_TRUNC)) != 0)
		return -EOPNOTSUPP;

	/* Takes the next queued packet. */
	error = socket_dequeue_packet(socket, flags & MSG_DONTWAIT, &packet);
	if (error != 0)
		return -error;

	/* Copies as much of the packet as fits. */
	if (length < packet->length)
		copied = length;
	else
		copied = packet->length;
	kern_memcpy(buffer, packet->data, copied);

	/* Copies the source address, reporting its full length. */
	if (address != NULL && address_length != NULL) {
		actual = packet->source_length;
		if (*address_length < actual)
			output = *address_length;
		else
			output = actual;
		kern_memcpy(address, packet->source_address, output);
		*address_length = actual;
	}

	/* The hop limit it came with (ws177-p044). */
	if (packet->hop_limit_known)
		*hop_limit = (int)packet->hop_limit;

	/* With MSG_TRUNC the full packet length is reported instead. */
	if ((flags & MSG_TRUNC) != 0)
		result = (ssize_t)packet->length;
	else
		result = (ssize_t)copied;
	packet_buf_free(packet);

	/* Reports the received length. */
	return result;
}

/*
 * Sets an option: IPV6_RECVHOPLIMIT of an ICMPv6 socket (ws177-p044), or
 * the ones every inet socket takes (IPV6_V6ONLY, SO_BINDTODEVICE).
 */
static int
icmp_setsockopt(
	struct socket *socket,
	int level,
	int option,
	const void *value,
	socklen_t length)
{
	struct icmp_endpoint *endpoint;
	int enabled;
	int error;

	/* The hop limit of each message, asked for or not. */
	endpoint = icmp_endpoint(socket);
	if (level == IPPROTO_IPV6 && option == IPV6_RECVHOPLIMIT) {
		if (endpoint->inet.family != AF_INET6)
			return ENOPROTOOPT;
		if (value == NULL || length != sizeof(enabled))
			return EINVAL;
		kern_memcpy(&enabled, value, sizeof(enabled));
		endpoint->receive_hop_limit = enabled != 0;
		return 0;
	}

	/* The inet socket's options; one it does not know is no option of this socket's (as before it had any). */
	error = inet_socket_setsockopt(&endpoint->inet, level, option, value, length);
	if (error == EOPNOTSUPP)
		return ENOPROTOOPT;
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the socket's local address. */
static int
icmp_getsockname(
	struct socket *socket,
	struct sockaddr *address,
	socklen_t *length)
{
	struct icmp_endpoint *endpoint;
	int error;

	endpoint = icmp_endpoint(socket);

	/* Reports why the lookup failed. */
	error = inet_socket_getsockname(&endpoint->inet, address, length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the socket's remote address. */
static int
icmp_getpeername(
	struct socket *socket,
	struct sockaddr *address,
	socklen_t *length)
{
	struct icmp_endpoint *endpoint;
	int error;

	endpoint = icmp_endpoint(socket);

	/* Reports why the lookup failed. */
	error = inet_socket_getpeername(&endpoint->inet, address, length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Unregisters and frees an ICMP socket. */
static void
icmp_close(
	struct socket *socket)
{
	struct icmp_endpoint *endpoint;
	struct icmp_endpoint **link;
	unsigned long irq;

	endpoint = icmp_endpoint(socket);

	/* Unlinks the endpoint from the registry. */
	irq = spin_lock_irqsave(&icmp_registry_lock);

	for (link = &icmp_sockets; *link != NULL; link = &(*link)->next) {
		if (*link == endpoint) {
			*link = endpoint->next;
			break;
		}
	}

	spin_unlock_irqrestore(&icmp_registry_lock, irq);

	kern_free(endpoint);
}

/* Queues a copy of a received packet on every matching raw socket. */
static void
icmp_deliver(
	struct packet_buf *packet,
	uint32_t source,
	uint32_t destination)
{
	struct icmp_endpoint *endpoint;
	struct icmp_endpoint *snapshot[SOCKET_BROADCAST_MAX];
	struct packet_buf *copy;
	struct sockaddr_in address;
	unsigned count;
	unsigned index;
	unsigned long irq;
	int referenced;

	count = 0;

	/* References every socket under the registry lock. */
	irq = spin_lock_irqsave(&icmp_registry_lock);

	for (endpoint = icmp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		/* Stops at a full snapshot, which creation keeps from happening. */
		if (count >= SOCKET_BROADCAST_MAX)
			break;

		/* Skips an ICMPv6 socket, and one whose closing has begun. */
		if (endpoint->inet.family != AF_INET)
			continue;
		referenced = socket_tryref(&endpoint->inet.socket);
		if (!referenced)
			continue;

		/* Keeps the referenced socket in the snapshot. */
		snapshot[count] = endpoint;
		count++;
	}

	spin_unlock_irqrestore(&icmp_registry_lock, irq);

	/* Queues a copy on each socket whose address filters match. */
	for (index = 0; index < count; index++) {
		/* Skips a socket bound or connected to other addresses. */
		endpoint = snapshot[index];
		if (endpoint->inet.local_address != 0 &&
		    endpoint->inet.local_address != destination) {
			socket_release(&endpoint->inet.socket);
			continue;
		}

		if ((endpoint->inet.inet_flags & INET_SOCKET_CONNECTED) &&
		    endpoint->inet.remote_address != source) {
			socket_release(&endpoint->inet.socket);
			continue;
		}

		/* Copies the packet from its IP header on. */
		if (packet->l3_offset == PACKET_OFFSET_NONE ||
		    packet->l3_length == 0) {
			socket_release(&endpoint->inet.socket);
			continue;
		}

		copy = packet_buf_copy_region(packet, packet->l3_offset,
		    packet->l3_length);
		if (copy == NULL) {
			socket_release(&endpoint->inet.socket);
			continue;
		}

		copy->l3_offset = 0;
		copy->l3_length = packet->l3_length;
		if (packet->l4_offset >= packet->l3_offset)
			copy->l4_offset = (uint16_t)(packet->l4_offset - packet->l3_offset);
		else
			copy->l4_offset = PACKET_OFFSET_NONE;

		/* Names the sender in the copy's source address. */
		kern_memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = net_htonl(source);
		kern_memcpy(copy->source_address, &address, sizeof(address));
		copy->source_length = sizeof(address);
		(void)socket_enqueue_packet(&endpoint->inet.socket, copy);
		socket_release(&endpoint->inet.socket);
	}
}

/* Receives an ICMP packet: delivers it to sockets and answers an echo. */
static int
icmp_input(
	struct packet_buf *packet,
	uint32_t source,
	uint32_t destination)
{
	struct icmp_wire *icmp;
	uint16_t checksum;
	int error;
	int own;

	/* Drops a short or corrupt message. */
	if (packet == NULL ||
	    packet->length < sizeof(*icmp) ||
	    net_checksum(packet->data, packet->length) != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Offers the message to the raw sockets. */
	icmp_deliver(packet, source, destination);

	/* Turns an echo request into a reply and sends it back. */
	icmp = (struct icmp_wire *)packet->data;
	if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
		icmp->type = ICMP_ECHO_REPLY;
		icmp->checksum[0] = 0;
		icmp->checksum[1] = 0;
		checksum = net_checksum(packet->data, packet->length);
		wire_put16(icmp->checksum, checksum);

		/*
		 * Answers from the address the request was sent to, so the
		 * asker recognizes the reply even when it came round the
		 * loopback device; a broadcast request is answered from the
		 * device's own address.
		 */
		own = inet_address_is_local(destination);
		if (own) {
			error = ipv4_output_from(packet->device, source,
			    IPPROTO_ICMP, destination, packet);
		} else {
			error = ipv4_output(packet->device, source, IPPROTO_ICMP,
			    packet);
		}

		/* Reports why the reply could not be sent. */
		if (error != 0)
			return error;

		/* Succeeded: the reply is on its way. */
		return 0;
	}

	/* Drops every other message. */
	packet_buf_free(packet);

	/* Reports the consumed message. */
	return 0;
}

/*
 * Allocates a raw ICMP or ICMPv6 socket and registers it for delivery,
 * unless the delivery snapshot is already as large as it can be.
 */
static int
icmp_register(
	int protocol,
	struct socket **result)
{
	struct icmp_endpoint *endpoint;
	struct icmp_endpoint *other;
	unsigned registered;
	unsigned long irq;

	/* Allocates the endpoint as a raw internet socket. */
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	inet_socket_object_init(&endpoint->inet, SOCK_RAW, protocol, &icmp_ops);

	/* Registers it while the snapshot can still hold it. */
	irq = spin_lock_irqsave(&icmp_registry_lock);

	/* Counts the sockets already registered. */
	registered = 0;
	for (other = icmp_sockets; other != NULL; other = other->next)
		registered++;

	/* Links the new socket while the snapshot can still hold it. */
	if (registered < SOCKET_BROADCAST_MAX) {
		endpoint->next = icmp_sockets;
		icmp_sockets = endpoint;
	}

	spin_unlock_irqrestore(&icmp_registry_lock, irq);

	/* Refuses a socket the delivery could not reach. */
	if (registered >= SOCKET_BROADCAST_MAX) {
		kern_free(endpoint);
		return ENFILE;
	}

	*result = &endpoint->inet.socket;

	/* Succeeded: the registered socket. */
	return 0;
}

/*
 * Sends an ICMPv6 message from a raw ICMPv6 socket (ws130-p003): to the
 * address given or the peer, out of the bound interface or the one a
 * link-local or group destination's scope names, from the bound address
 * or the one chosen; the checksum is filled in.
 */
static ssize_t
icmp6_sendto(
	struct icmp_endpoint *endpoint,
	const void *buffer,
	size_t length,
	const struct sockaddr *address,
	socklen_t address_length)
{
	struct sockaddr_in6 output;
	struct in6_addr destination;
	struct packet_buf *packet;
	struct net_device *device;
	const struct in6_addr *source;
	uint8_t *payload;
	unsigned scope;
	unsigned ifindex;
	int unspecified;
	int mapped;
	int linklocal;
	int multicast;
	int error;

	/* Takes the destination from the address, else from the connection. */
	if (address != NULL) {
		if (address_length < sizeof(output) || address->sa_family != AF_INET6)
			return -EINVAL;
		kern_memcpy(&output, address, sizeof(output));
		destination = output.sin6_addr;
		scope = output.sin6_scope_id;
	} else if (endpoint->inet.inet_flags & INET_SOCKET_CONNECTED) {
		destination = endpoint->inet.remote6;
		scope = endpoint->inet.scope6;
	} else {
		return -EDESTADDRREQ;
	}

	/* A specific destination; an IPv4-mapped one is not for ICMPv6. */
	unspecified = in6_is_unspecified(&destination);
	if (unspecified)
		return -EADDRNOTAVAIL;
	mapped = in6_is_v4mapped(&destination);
	if (mapped)
		return -EAFNOSUPPORT;

	/* The interface: the bound one, or the scope of a destination on a link. */
	ifindex = endpoint->inet.ifindex;
	linklocal = in6_is_linklocal(&destination);
	multicast = in6_is_multicast(&destination);
	if (ifindex == 0 &&
	    (linklocal ||
	     multicast))
		ifindex = scope;
	if (ifindex == 0 && linklocal)
		return -EINVAL;

	/* The device of that interface, held for the send. */
	device = NULL;
	if (ifindex != 0) {
		device = net_device_find_by_index_ref(ifindex);
		if (device == NULL)
			return -ENXIO;
	}

	/* Copies the message into a packet. */
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	if (packet == NULL) {
		if (device != NULL)
			net_device_release(device);
		return -ENOBUFS;
	}

	payload = packet_buf_append(packet, length);
	if (payload == NULL) {
		packet_buf_free(packet);
		if (device != NULL)
			net_device_release(device);
		return -EMSGSIZE;
	}

	kern_memcpy(payload, buffer, length);

	/* Sends it from the bound address, or the chosen one, with the checksum filled in. */
	source = NULL;
	unspecified = in6_is_unspecified(&endpoint->inet.local6);
	if (!unspecified)
		source = &endpoint->inet.local6;
	error = icmp6_send(device, source, &destination, 0U, packet);
	if (device != NULL)
		net_device_release(device);
	if (error != 0)
		return -error;

	/* Succeeded: the sent length. */
	return (ssize_t)length;
}

/* Tells whether a raw ICMPv6 socket's bound and connected addresses take a message. */
static int
icmp6_accepts(
	const struct icmp_endpoint *endpoint,
	const struct in6_addr *source,
	const struct in6_addr *destination)
{
	int unspecified;
	int same;

	/* A socket bound to another address. */
	unspecified = in6_is_unspecified(&endpoint->inet.local6);
	same = in6_equal(&endpoint->inet.local6, destination);
	if (!unspecified && !same)
		return 0;

	/* A socket connected to another peer. */
	if ((endpoint->inet.inet_flags & INET_SOCKET_CONNECTED) != 0) {
		same = in6_equal(&endpoint->inet.remote6, source);
		if (!same)
			return 0;
	}

	/* Succeeded: the socket takes it. */
	return 1;
}
