/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements read-only routing interface event sockets.
 */

#include "kern/net/socket.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/kmem.h"
#include "kern/poll.h"

#include <errno.h>
#include <string.h>
#include <zedbsd/netif.h>
#include <zedbsd/route.h>

struct route_endpoint {
	struct socket socket;
	struct packet_buf *free_packets;
	unsigned overflow_pending;
	struct route_endpoint *next;
};

static struct route_endpoint *route_sockets;
static struct spinlock route_registry_lock;
static uint64_t route_event_sequence;

static unsigned route_uapi_flags(unsigned flags);
static struct route_endpoint *route_endpoint(struct socket *socket);
static void route_endpoint_release_free_packets(struct route_endpoint *endpoint);
static int route_endpoint_reserve_packets(struct route_endpoint *endpoint);
static ssize_t route_recvfrom(struct socket *socket, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length);
static int route_poll(struct socket *socket, short events, short *revents);
static void route_close(struct socket *socket);
static int route_create(int type, int protocol, struct socket **result);
static void route_enqueue(struct route_endpoint *endpoint, const struct rtm_ifinfo *message);

/*
 * Initializes the routing socket family.
 */
int
route_socket_init(
	void)
{
	static const struct socket_family_ops route_family = {
		.create = route_create,
	};
	int error;

	/* Initializes the interface-event registry. */
	route_sockets = NULL;
	route_event_sequence = 0U;
	spin_init(
		&route_registry_lock,
		LOCK_RANK_SOCKET_REGISTRY,
		"route socket registry");

	/* Registers the read-only routing socket family. */
	error = socket_family_register(AF_ROUTE, &route_family);

	/* Reports the registration result. */
	return error;
}

/*
 * Publishes an interface state transition to routing socket listeners.
 */
void
route_socket_notify(
	unsigned ifindex,
	uint64_t device_generation,
	unsigned device_flags,
	unsigned transition)
{
	struct route_endpoint *endpoint;
	struct route_endpoint *snapshot[SOCKET_MAX];
	struct rtm_ifinfo message;
	unsigned count;
	unsigned index;
	unsigned long irq;
	int referenced;

	/* Rejects an incomplete identity or an unsupported transition. */
	if (ifindex == 0U ||
	    device_generation == 0U ||
	    (transition != RTM_IFINFO_CARRIER_UP &&
	     transition != RTM_IFINFO_CARRIER_DOWN &&
	     transition != RTM_IFINFO_REMOVAL))
		return;

	/* Builds the fixed-width interface event record. */
	memset(&message, 0, sizeof(message));
	message.rtm_version = RTM_VERSION;
	message.rtm_type = RTM_IFINFO;
	message.rtm_length = sizeof(message);
	message.rtm_ifindex = ifindex;
	message.rtm_device_generation = device_generation;
	message.rtm_if_flags = route_uapi_flags(device_flags);
	message.rtm_transition = transition;

	/* Assigns the next nonzero global event sequence. */
	irq = spin_lock_irqsave(&route_registry_lock);
	route_event_sequence++;

	/* Skips zero when the sequence counter wraps. */
	if (route_event_sequence == 0U)
		route_event_sequence++;

	/* Stores the assigned sequence in the immutable event record. */
	message.rtm_sequence = route_event_sequence;

	/* Retains every live listener that fits in the bounded snapshot. */
	count = 0U;
	for (endpoint = route_sockets;
	     endpoint != NULL;
	     endpoint = endpoint->next) {
		/* Leaves additional listeners untouched after filling the snapshot. */
		if (count >= SOCKET_MAX)
			continue;

		/* Retains this listener or skips it when closing has begun. */
		referenced = socket_tryref(&endpoint->socket);
		if (!referenced)
			continue;

		/* Appends the retained listener to the delivery snapshot. */
		snapshot[count] = endpoint;
		count++;
	}

	/* Releases the registry after completing the stable snapshot. */
	spin_unlock_irqrestore(&route_registry_lock, irq);

	/* Delivers the record independently to every retained listener. */
	for (index = 0U; index < count; index++) {
		route_enqueue(snapshot[index], &message);
		socket_release(&snapshot[index]->socket);
	}
}

/* Converts internal network-device flags to public interface flags. */
static unsigned
route_uapi_flags(
	unsigned flags)
{
	unsigned result;

	/* Starts with no public interface properties. */
	result = 0U;

	/* Exposes administrative readiness. */
	if ((flags & NET_DEVICE_UP) != 0U)
		result |= IFF_UP;

	/* Exposes operational readiness. */
	if ((flags & NET_DEVICE_RUNNING) != 0U)
		result |= IFF_RUNNING;

	/* Exposes broadcast support. */
	if ((flags & NET_DEVICE_BROADCAST) != 0U)
		result |= IFF_BROADCAST;

	/* Exposes multicast support. */
	if ((flags & NET_DEVICE_MULTICAST) != 0U)
		result |= IFF_MULTICAST;

	/* Exposes loopback semantics. */
	if ((flags & NET_DEVICE_LOOPBACK) != 0U)
		result |= IFF_LOOPBACK;

	/* Returns the public flag set. */
	return result;
}

/* Recovers the containing routing endpoint from its socket. */
static struct route_endpoint *
route_endpoint(
	struct socket *socket)
{
	/* Returns the socket's enclosing endpoint. */
	return (struct route_endpoint *)socket;
}

/* Releases every unused event packet owned by an endpoint. */
static void
route_endpoint_release_free_packets(
	struct route_endpoint *endpoint)
{
	struct packet_buf *packet;

	/* Releases the complete unused packet chain. */
	packet = endpoint->free_packets;
	while (packet != NULL) {
		endpoint->free_packets = packet->next;
		packet_buf_free(packet);
		packet = endpoint->free_packets;
	}
}

/* Reserves the endpoint's complete bounded event packet pool. */
static int
route_endpoint_reserve_packets(
	struct route_endpoint *endpoint)
{
	struct packet_buf *packet;
	void *record;
	unsigned index;

	/*
	 * Carrier changes can originate below a driver or station lock.
	 * Reserves every packet now so notification never enters an allocator.
	 */
	for (index = 0U; index < SOCKET_RECEIVE_MESSAGES_MAX; index++) {
		/* Allocates one packet or releases the incomplete pool. */
		packet = packet_buf_alloc(0U);
		if (packet == NULL) {
			route_endpoint_release_free_packets(endpoint);
			return ENOMEM;
		}

		/* Reserves a record or releases the packet and incomplete pool. */
		record = packet_buf_append(packet, sizeof(struct rtm_ifinfo));
		if (record == NULL) {
			packet_buf_free(packet);
			route_endpoint_release_free_packets(endpoint);
			return ENOMEM;
		}

		/* Prepends the completed packet to the endpoint's free pool. */
		packet->next = endpoint->free_packets;
		endpoint->free_packets = packet;
	}

	/* Reports a complete packet pool. */
	return 0;
}

/* Receives one fixed-width interface event record. */
static ssize_t
route_recvfrom(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length)
{
	struct route_endpoint *endpoint;
	struct packet_buf *packet;
	unsigned long irq;
	int error;

	/* Recovers the endpoint that owns recycled event packets. */
	endpoint = route_endpoint(socket);

	/* Rejects a missing destination buffer. */
	if (buffer == NULL)
		return -EINVAL;

	/* Rejects message flags outside the read-only routing contract. */
	if ((flags & ~(MSG_DONTWAIT | MSG_TRUNC)) != 0)
		return -EOPNOTSUPP;

	/* Preserves a complete event unless the caller requested truncation. */
	if (length < sizeof(struct rtm_ifinfo) && (flags & MSG_TRUNC) == 0)
		return -EMSGSIZE;

	/* Removes the next event or propagates an unavailable receive. */
	error = socket_dequeue_packet(socket, flags & MSG_DONTWAIT, &packet);
	if (error != 0)
		return -error;

	/* Reports that routing events do not carry a source address. */
	if (address != NULL && address_length != NULL)
		*address_length = 0;

	/* Limits the copied record to the caller's available buffer. */
	if (length > packet->length)
		length = packet->length;
	memcpy(buffer, packet->data, length);

	/* Reports the complete record length for a truncating receive. */
	if ((flags & MSG_TRUNC) != 0)
		length = packet->length;

	/* Returns the consumed packet to the endpoint's fixed pool. */
	irq = spin_lock_irqsave(&socket->lock);
	packet->next = endpoint->free_packets;
	endpoint->free_packets = packet;
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the copied or complete record length. */
	return (ssize_t)length;
}

/* Polls a routing endpoint for readable events. */
static int
route_poll(
	struct socket *socket,
	short events,
	short *revents)
{
	int error;

	/* Polls common state and suppresses write readiness after success. */
	error = socket_poll_common(socket, events, revents);
	if (error == 0)
		*revents &= (short)~(POLLOUT | POLLWRNORM);

	/* Reports the common poll result. */
	return error;
}

/* Closes and releases a routing endpoint. */
static void
route_close(
	struct socket *socket)
{
	struct route_endpoint *endpoint;
	struct route_endpoint **link;
	unsigned long irq;

	/* Recovers the endpoint and locks the listener registry. */
	endpoint = route_endpoint(socket);
	irq = spin_lock_irqsave(&route_registry_lock);

	/* Finds and unlinks this endpoint from the listener registry. */
	for (link = &route_sockets; *link != NULL; link = &(*link)->next) {
		/* Continues until the endpoint's registry link is found. */
		if (*link != endpoint)
			continue;

		/* Removes the endpoint from future notification snapshots. */
		*link = endpoint->next;

		/* Stops after unlinking the unique endpoint. */
		break;
	}

	/* Releases the listener registry before freeing endpoint storage. */
	spin_unlock_irqrestore(&route_registry_lock, irq);

	/* Releases the endpoint's unused packet pool and storage. */
	route_endpoint_release_free_packets(endpoint);
	kern_free(endpoint);
}

/* Creates a read-only routing endpoint. */
static int
route_create(
	int type,
	int protocol,
	struct socket **result)
{
	static const struct socket_ops route_ops = {
		.recvfrom = route_recvfrom,
		.poll = route_poll,
		.close = route_close,
	};
	struct route_endpoint *endpoint;
	unsigned long irq;
	int error;

	/* Accepts only the raw default routing protocol. */
	if (type != SOCK_RAW || protocol != 0)
		return EPROTONOSUPPORT;

	/* Allocates an empty endpoint or reports exhausted storage. */
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;

	/* Initializes the endpoint's common socket state. */
	socket_init_object(
		&endpoint->socket,
		AF_ROUTE,
		type,
		protocol,
		&route_ops);

	/* Reserves the bounded queue or releases an incomplete endpoint. */
	error = route_endpoint_reserve_packets(endpoint);
	if (error != 0) {
		kern_free(endpoint);
		return ENOMEM;
	}

	/* Publishes the initialized endpoint to interface notifications. */
	irq = spin_lock_irqsave(&route_registry_lock);
	endpoint->next = route_sockets;
	route_sockets = endpoint;
	spin_unlock_irqrestore(&route_registry_lock, irq);

	/* Returns the initialized socket to its caller. */
	*result = &endpoint->socket;

	/* Reports successful endpoint creation. */
	return 0;
}

/* Enqueues one interface event on a routing endpoint. */
static void
route_enqueue(
	struct route_endpoint *endpoint,
	const struct rtm_ifinfo *message)
{
	struct packet_buf *packet;
	struct rtm_ifinfo *output;
	unsigned long irq;

	/* Locks the endpoint across queue selection and publication. */
	irq = spin_lock_irqsave(&endpoint->socket.lock);

	/* Ignores a listener that began closing after the registry snapshot. */
	if (endpoint->socket.lifecycle != SOCKET_OPEN) {
		spin_unlock_irqrestore(&endpoint->socket.lock, irq);
		return;
	}

	/*
	 * Reuses the oldest queued packet when any receive bound is reached.
	 * Otherwise consumes one unused packet from the fixed endpoint pool.
	 */
	if (endpoint->free_packets == NULL ||
	    (endpoint->socket.receive_packet_limit != 0U &&
	     endpoint->socket.receive_packets >=
	     endpoint->socket.receive_packet_limit) ||
	    sizeof(*output) > endpoint->socket.receive_hiwat_bytes ||
	    endpoint->socket.receive_bytes >
	    endpoint->socket.receive_hiwat_bytes - sizeof(*output)) {
		packet = endpoint->socket.receive_head;

		/* Removes the oldest queued packet when one is available. */
		if (packet != NULL) {
			endpoint->socket.receive_head = packet->next;

			/* Clears the tail when removing the final queued packet. */
			if (endpoint->socket.receive_head == NULL)
				endpoint->socket.receive_tail = NULL;
			packet->next = NULL;
			endpoint->socket.receive_packets--;
			endpoint->socket.receive_bytes -= packet->length;
		}

		/* Marks the next retained record as following lost state. */
		endpoint->overflow_pending = 1U;
	} else {
		packet = endpoint->free_packets;

		/* Removes one packet from the unused endpoint pool. */
		if (packet != NULL) {
			endpoint->free_packets = packet->next;
			packet->next = NULL;
		}
	}

	/*
	 * A fixed pool is empty only when every record is queued.  Keeps the
	 * notifier fail-safe if a future socket policy changes that invariant.
	 */
	if (packet == NULL) {
		spin_unlock_irqrestore(&endpoint->socket.lock, irq);
		return;
	}

	/* Copies the immutable event into the selected packet. */
	output = (struct rtm_ifinfo *)packet->data;
	memcpy(output, message, sizeof(*output));

	/* Reports any event loss on the next retained record. */
	if (endpoint->overflow_pending != 0U) {
		output->rtm_flags |= RTM_IFINFO_F_OVERFLOW;
		endpoint->overflow_pending = 0U;
	}

	/* Appends the packet to the endpoint's receive queue. */
	packet->next = NULL;

	/* Links after an existing tail or establishes the queue head. */
	if (endpoint->socket.receive_tail != NULL) {
		endpoint->socket.receive_tail->next = packet;
	} else {
		endpoint->socket.receive_head = packet;
	}
	endpoint->socket.receive_tail = packet;
	endpoint->socket.receive_packets++;
	endpoint->socket.receive_bytes += packet->length;

	/* Wakes one receiver after publishing the complete queue state. */
	waitq_wake_one(&endpoint->socket.receive_waitq);
	spin_unlock_irqrestore(&endpoint->socket.lock, irq);

	/* Wakes pollers after making the event readable. */
	poll_notify();
}
