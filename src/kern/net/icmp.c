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
 */

#include "kern/net/inet-socket.h"
#include "kern/net/byteorder.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/kmem.h"
#include "internal.h"
#include "wire.h"

#include <zedbsd/netinet.h>
#include <errno.h>
#include <string.h>

#define ICMP_ECHO_REPLY   0U
#define ICMP_ECHO_REQUEST 8U

struct icmp_endpoint {
	struct inet_socket inet;
	struct icmp_endpoint *next;
};

static struct icmp_endpoint *icmp_sockets;
static struct spinlock icmp_registry_lock;

static struct icmp_endpoint *icmp_endpoint(struct socket *socket);
static int icmp_bind(struct socket *socket, const struct sockaddr *address, socklen_t length);
static int icmp_connect(struct socket *socket, const struct sockaddr *address, socklen_t length, unsigned io_flags);
static ssize_t icmp_sendto(struct socket *socket, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
static ssize_t icmp_recvfrom(struct socket *socket, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length);
static int icmp_getsockname(struct socket *socket, struct sockaddr *address, socklen_t *length);
static int icmp_getpeername(struct socket *socket, struct sockaddr *address, socklen_t *length);
static void icmp_close(struct socket *socket);
static void icmp_deliver(struct packet_buf *packet, uint32_t source, uint32_t destination);
static int icmp_input(struct packet_buf *packet, uint32_t source, uint32_t destination);

static const struct socket_ops icmp_ops = {
	.bind = icmp_bind,
	.connect = icmp_connect,
	.sendto = icmp_sendto,
	.recvfrom = icmp_recvfrom,
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
	struct icmp_endpoint *endpoint;
	unsigned long irq;

	/* Rejects a missing result or another protocol. */
	if (result == NULL || (protocol != 0 && protocol != IPPROTO_ICMP))
		return EPROTONOSUPPORT;

	/* Allocates the endpoint as a raw internet socket. */
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	inet_socket_object_init(&endpoint->inet, SOCK_RAW, IPPROTO_ICMP,
	    &icmp_ops);

	/* Registers it for delivery. */
	irq = spin_lock_irqsave(&icmp_registry_lock);
	endpoint->next = icmp_sockets;
	icmp_sockets = endpoint;
	spin_unlock_irqrestore(&icmp_registry_lock, irq);
	*result = &endpoint->inet.socket;

	/* Reports the created socket. */
	return 0;
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
	error = ipv4_protocol_register(IPPROTO_ICMP, icmp_input);

	/* Reports the registration result. */
	return error;
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
	error = inet_socket_bind(&endpoint->inet, address, length);

	/* Reports the bind result. */
	return error;
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
	error = inet_socket_connect(&endpoint->inet, address, length);

	/* Reports the connect result. */
	return error;
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
	int error;

	endpoint = icmp_endpoint(socket);

	/* Rejects unsupported flags, a missing buffer, or a headerless message. */
	if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0 ||
	    buffer == NULL ||
	    length < sizeof(struct icmp_wire))
		return -EINVAL;

	/* Takes the destination from the address, else from the connection. */
	if (address != NULL) {
		if (address_length < sizeof(destination_address) ||
		    address->sa_family != AF_INET)
			return -EINVAL;
		memcpy(&destination_address, address, sizeof(destination_address));
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
	memcpy(payload, buffer, length);

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
	struct packet_buf *packet;
	size_t copied;
	socklen_t actual;
	socklen_t output;
	ssize_t result;
	int error;

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
	memcpy(buffer, packet->data, copied);

	/* Copies the source address, reporting its full length. */
	if (address != NULL && address_length != NULL) {
		actual = packet->source_length;
		if (*address_length < actual)
			output = *address_length;
		else
			output = actual;
		memcpy(address, packet->source_address, output);
		*address_length = actual;
	}

	/* With MSG_TRUNC the full packet length is reported instead. */
	if ((flags & MSG_TRUNC) != 0)
		result = (ssize_t)packet->length;
	else
		result = (ssize_t)copied;
	packet_buf_free(packet);

	/* Reports the received length. */
	return result;
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
	error = inet_socket_getsockname(&endpoint->inet, address, length);

	/* Reports the lookup result. */
	return error;
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
	error = inet_socket_getpeername(&endpoint->inet, address, length);

	/* Reports the lookup result. */
	return error;
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
	struct icmp_endpoint *snapshot[SOCKET_MAX];
	struct packet_buf *copy;
	struct sockaddr_in address;
	unsigned count;
	unsigned index;
	unsigned long irq;

	count = 0;

	/* References every socket under the registry lock. */
	irq = spin_lock_irqsave(&icmp_registry_lock);
	for (endpoint = icmp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		if (count < SOCKET_MAX && socket_tryref(&endpoint->inet.socket))
			snapshot[count++] = endpoint;
	}
	spin_unlock_irqrestore(&icmp_registry_lock, irq);

	/* Queues a copy on each socket whose address filters match. */
	for (index = 0; index < count; index++) {
		endpoint = snapshot[index];

		/* Skips a socket bound or connected to other addresses. */
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
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = net_htonl(source);
		memcpy(copy->source_address, &address, sizeof(address));
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
		error = ipv4_output(packet->device, source, IPPROTO_ICMP, packet);
		return error;
	}

	/* Drops every other message. */
	packet_buf_free(packet);

	/* Reports the consumed message. */
	return 0;
}
