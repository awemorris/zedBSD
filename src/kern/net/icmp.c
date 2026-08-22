/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static struct icmp_endpoint *icmp_endpoint(struct socket *socket)
{
	return (struct icmp_endpoint *)socket;
}

static int icmp_bind(struct socket *socket, const struct sockaddr *address,
		     socklen_t length)
{
	return inet_socket_bind(&icmp_endpoint(socket)->inet, address, length);
}

static int icmp_connect(struct socket *socket, const struct sockaddr *address,
			 socklen_t length, unsigned io_flags)
{
	(void)io_flags;
	return inet_socket_connect(&icmp_endpoint(socket)->inet, address, length);
}

static ssize_t
icmp_sendto(struct socket *socket, const void *buffer, size_t length, int flags,
	    const struct sockaddr *address, socklen_t address_length)
{
	struct icmp_endpoint *endpoint = icmp_endpoint(socket);
	struct sockaddr_in destination_address;
	struct packet_buf *packet;
	uint8_t *payload;
	uint32_t destination;
	uint16_t checksum;
	int error;

	if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0 || buffer == NULL ||
	    length < sizeof(struct icmp_wire))
		return -EINVAL;
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
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	if (packet == NULL)
		return -ENOBUFS;
	payload = packet_buf_append(packet, length);
	if (payload == NULL) {
		packet_buf_free(packet);
		return -EMSGSIZE;
	}
	memcpy(payload, buffer, length);
	payload[2] = payload[3] = 0;
	checksum = net_checksum(payload, length);
	payload[2] = (uint8_t)(checksum >> 8);
	payload[3] = (uint8_t)checksum;
	{
		struct net_device *device = endpoint->inet.ifindex != 0 ?
		    net_device_find_by_index_ref(endpoint->inet.ifindex) : NULL;
		error = ipv4_output(device, destination, IPPROTO_ICMP, packet);
		net_device_release(device);
	}
	return error == 0 ? (ssize_t)length : -error;
}

static ssize_t
icmp_recvfrom(struct socket *socket, void *buffer, size_t length, int flags,
	      struct sockaddr *address, socklen_t *address_length)
{
	struct packet_buf *packet;
	size_t copied;
	int error;
	if ((flags & ~(MSG_DONTWAIT | MSG_TRUNC)) != 0)
		return -EOPNOTSUPP;
	error = socket_dequeue_packet(socket, flags & MSG_DONTWAIT, &packet);

	if (error != 0)
		return -error;
	copied = length < packet->length ? length : packet->length;
	memcpy(buffer, packet->data, copied);
	if (address != NULL && address_length != NULL) {
		socklen_t actual = packet->source_length;
		socklen_t output = *address_length < actual ? *address_length : actual;
		memcpy(address, packet->source_address, output);
		*address_length = actual;
	}
	{
		ssize_t result = (flags & MSG_TRUNC) != 0 ?
		    (ssize_t)packet->length : (ssize_t)copied;
		packet_buf_free(packet);
		return result;
	}
}

static int icmp_getsockname(struct socket *socket, struct sockaddr *address,
			    socklen_t *length)
{
	return inet_socket_getsockname(&icmp_endpoint(socket)->inet, address, length);
}

static int icmp_getpeername(struct socket *socket, struct sockaddr *address,
			    socklen_t *length)
{
	return inet_socket_getpeername(&icmp_endpoint(socket)->inet, address, length);
}

static void
icmp_close(struct socket *socket)
{
	struct icmp_endpoint *endpoint = icmp_endpoint(socket);
	struct icmp_endpoint **link;
	unsigned long irq = spin_lock_irqsave(&icmp_registry_lock);

	for (link = &icmp_sockets; *link != NULL; link = &(*link)->next)
		if (*link == endpoint) {
			*link = endpoint->next;
			break;
		}
	spin_unlock_irqrestore(&icmp_registry_lock, irq);
	kern_free(endpoint);
}

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

int
icmp_socket_create(int protocol, struct socket **result)
{
	struct icmp_endpoint *endpoint;
	unsigned long irq;

	if (result == NULL || (protocol != 0 && protocol != IPPROTO_ICMP))
		return EPROTONOSUPPORT;
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	inet_socket_object_init(&endpoint->inet, SOCK_RAW, IPPROTO_ICMP,
	    &icmp_ops);
	irq = spin_lock_irqsave(&icmp_registry_lock);
	endpoint->next = icmp_sockets;
	icmp_sockets = endpoint;
	spin_unlock_irqrestore(&icmp_registry_lock, irq);
	*result = &endpoint->inet.socket;
	return 0;
}

static void
icmp_deliver(struct packet_buf *packet, uint32_t source, uint32_t destination)
{
	struct icmp_endpoint *endpoint, *snapshot[SOCKET_MAX];
	unsigned count = 0, index;
	unsigned long irq = spin_lock_irqsave(&icmp_registry_lock);

	for (endpoint = icmp_sockets; endpoint != NULL; endpoint = endpoint->next)
		if (count < SOCKET_MAX && socket_tryref(&endpoint->inet.socket))
			snapshot[count++] = endpoint;
	spin_unlock_irqrestore(&icmp_registry_lock, irq);
	for (index = 0; index < count; index++) {
		struct packet_buf *copy;
		struct sockaddr_in address;
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
		copy->l4_offset = packet->l4_offset >= packet->l3_offset ?
		    (uint16_t)(packet->l4_offset - packet->l3_offset) :
		    PACKET_OFFSET_NONE;
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = net_htonl(source);
		memcpy(copy->source_address, &address, sizeof(address));
		copy->source_length = sizeof(address);
		(void)socket_enqueue_packet(&endpoint->inet.socket, copy);
		socket_release(&endpoint->inet.socket);
	}
}

static int
icmp_input(struct packet_buf *packet, uint32_t source, uint32_t destination)
{
	struct icmp_wire *icmp;
	uint16_t checksum;

	if (packet == NULL || packet->length < sizeof(*icmp) ||
	    net_checksum(packet->data, packet->length) != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}
	icmp_deliver(packet, source, destination);
	icmp = (struct icmp_wire *)packet->data;
	if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
		icmp->type = ICMP_ECHO_REPLY;
		icmp->checksum[0] = icmp->checksum[1] = 0;
		checksum = net_checksum(packet->data, packet->length);
		wire_put16(icmp->checksum, checksum);
		return ipv4_output(packet->device, source, IPPROTO_ICMP, packet);
	}
	packet_buf_free(packet);
	return 0;
}

int
icmp_init(void)
{
	icmp_sockets = NULL;
	spin_init(&icmp_registry_lock, LOCK_RANK_SOCKET_REGISTRY,
	    "ICMP socket registry");
	return ipv4_protocol_register(IPPROTO_ICMP, icmp_input);
}
