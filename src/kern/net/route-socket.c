/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements read-only routing interface event sockets.
 *
 * A socket made with protocol 0 hears the interfaces' events
 * (RTM_IFINFO); one made with protocol AF_INET6 hears them and IPv6's:
 * the Router Advertisements, the addresses' changes and the routers that
 * stopped answering (ws130-p002, route.h).  Every record is copied into a
 * packet of the endpoint's own pool, reserved when it was made, so a
 * notification never allocates.
 */

#include "kern/net/socket.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/kmem.h"
#include "kern/poll.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <uapi/netif.h>
#include <uapi/netinet.h>
#include <uapi/route.h>

/* The largest record: a Router Advertisement's with its message. */
#define ROUTE_RECORD_MAX	(sizeof(struct rtm_routeradv) + RTM_ROUTERADV_MESSAGE_MAX)

struct route_endpoint {
	struct socket socket;
	struct packet_buf *free_packets;
	unsigned overflow_pending;
	int inet6;
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
static void route_enqueue(struct route_endpoint *endpoint, const void *record, size_t length);
static void route_broadcast(void *record, size_t length, int inet6_only);
static uint64_t route_sequence_next_locked(void);

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

	/* Reports why the registration failed. */
	error = socket_family_register(AF_ROUTE, &route_family);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	struct rtm_ifinfo message;

	/* Rejects an incomplete identity or an unsupported transition. */
	if (ifindex == 0U ||
	    device_generation == 0U ||
	    (transition != RTM_IFINFO_CARRIER_UP &&
	     transition != RTM_IFINFO_CARRIER_DOWN &&
	     transition != RTM_IFINFO_REMOVAL &&
	     transition != RTM_IFINFO_ARRIVAL))
		return;

	/* Builds the fixed-width interface event record. */
	kern_memset(&message, 0, sizeof(message));
	message.rtm_version = RTM_VERSION;
	message.rtm_type = RTM_IFINFO;
	message.rtm_length = sizeof(message);
	message.rtm_ifindex = ifindex;
	message.rtm_device_generation = device_generation;
	message.rtm_if_flags = route_uapi_flags(device_flags);
	message.rtm_transition = transition;

	/* Delivered to every listener. */
	route_broadcast(&message, sizeof(message), 0);
}

/*
 * Publishes a Router Advertisement the kernel took (RTM_ROUTERADV) to the
 * IPv6 listeners: the router's address and the message from its type
 * byte, at most RTM_ROUTERADV_MESSAGE_MAX bytes of it.
 */
void
route_socket_notify_routeradv(
	unsigned ifindex,
	const struct in6_addr *source,
	const void *message,
	size_t length)
{
	static uint8_t record[ROUTE_RECORD_MAX];
	struct rtm_routeradv *header;

	/* A message that fits. */
	if (length > RTM_ROUTERADV_MESSAGE_MAX)
		return;

	/* The record, built in one buffer: the network worker is the only caller. */
	header = (struct rtm_routeradv *)record;
	kern_memset(header, 0, sizeof(*header));
	header->rtm_version = RTM_VERSION;
	header->rtm_type = RTM_ROUTERADV;
	header->rtm_length = (uint32_t)(sizeof(*header) + length);
	header->rtm_ifindex = ifindex;
	header->rtm_source = *source;
	header->rtm_message_length = (uint32_t)length;
	kern_memcpy(record + sizeof(*header), message, length);

	/* Delivered to the IPv6 listeners. */
	route_broadcast(record, sizeof(*header) + length, 1);
}

/* Publishes an IPv6 address's change (RTM_ADDRINFO) to the IPv6 listeners. */
void
route_socket_notify_address(
	unsigned ifindex,
	const struct in6_addr *address,
	unsigned prefixlen,
	unsigned transition,
	unsigned flags)
{
	struct rtm_addrinfo record;

	/* The record. */
	kern_memset(&record, 0, sizeof(record));
	record.rtm_version = RTM_VERSION;
	record.rtm_type = RTM_ADDRINFO;
	record.rtm_length = sizeof(record);
	record.rtm_ifindex = ifindex;
	record.rtm_address = *address;
	record.rtm_prefixlen = prefixlen;
	record.rtm_transition = transition;
	record.rtm_addr_flags = flags;

	/* Delivered to the IPv6 listeners. */
	route_broadcast(&record, sizeof(record), 1);
}

/* Publishes a neighbor that stopped answering (RTM_NEIGHBOR) to the IPv6 listeners. */
void
route_socket_notify_neighbor(
	unsigned ifindex,
	const struct in6_addr *address,
	unsigned transition,
	int router)
{
	struct rtm_neighbor record;

	/* The record. */
	kern_memset(&record, 0, sizeof(record));
	record.rtm_version = RTM_VERSION;
	record.rtm_type = RTM_NEIGHBOR;
	record.rtm_length = sizeof(record);
	record.rtm_ifindex = ifindex;
	record.rtm_address = *address;
	record.rtm_transition = transition;
	record.rtm_router = 0U;
	if (router)
		record.rtm_router = 1U;

	/* Delivered to the IPv6 listeners. */
	route_broadcast(&record, sizeof(record), 1);
}

/*
 * Gives a record (in place) the next sequence and delivers it to every
 * listener (or only those made for IPv6), each a copy in its own packet.
 */
static void
route_broadcast(
	void *record,
	size_t length,
	int inet6_only)
{
	struct route_endpoint *endpoint;
	struct route_endpoint *snapshot[SOCKET_BROADCAST_MAX];
	struct rtm_header *header;
	unsigned count;
	unsigned index;
	unsigned long irq;
	int referenced;

	/* A record that fits. */
	if (length > ROUTE_RECORD_MAX || length < sizeof(*header))
		return;
	header = record;

	/* The sequence, and the listeners it goes to. */
	irq = spin_lock_irqsave(&route_registry_lock);

	header->rtm_sequence = route_sequence_next_locked();
	count = 0U;
	for (endpoint = route_sockets;
	     endpoint != NULL;
	     endpoint = endpoint->next) {
		/* Leaves additional listeners untouched after filling the snapshot. */
		if (count >= SOCKET_BROADCAST_MAX)
			continue;

		/* An IPv6 record goes only to a listener made for it. */
		if (inet6_only && !endpoint->inet6)
			continue;

		/* Retains this listener or skips it when closing has begun. */
		referenced = socket_tryref(&endpoint->socket);
		if (!referenced)
			continue;
		snapshot[count] = endpoint;
		count++;
	}

	spin_unlock_irqrestore(&route_registry_lock, irq);

	/* Delivers the record independently to every retained listener. */
	for (index = 0U; index < count; index++) {
		route_enqueue(snapshot[index], record, length);
		socket_release(&snapshot[index]->socket);
	}
}

/* Gives the next nonzero global event sequence; the caller holds the registry lock. */
static uint64_t
route_sequence_next_locked(
	void)
{
	/* The next, skipping zero when the counter wraps. */
	route_event_sequence++;
	if (route_event_sequence == 0U)
		route_event_sequence++;
	return route_event_sequence;
}

/* Converts internal network-device flags to public interface flags. */
static unsigned
route_uapi_flags(
	unsigned flags)
{
	unsigned result;

	/* Starts with no public interface properties. */

	/* Exposes administrative readiness. */
	result = 0U;
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
	kern_memcpy(buffer, packet->data, length);

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

	/* Reports why the common poll failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	struct route_endpoint *other;
	unsigned registered;
	unsigned long irq;
	int error;

	/* Accepts the raw default routing protocol, or IPv6's (its events too). */
	if (type != SOCK_RAW || (protocol != 0 && protocol != AF_INET6))
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

	/* A listener made for IPv6 hears its events. */
	if (protocol == AF_INET6)
		endpoint->inet6 = 1;

	/* Reserves the bounded queue or releases an incomplete endpoint. */
	error = route_endpoint_reserve_packets(endpoint);
	if (error != 0) {
		kern_free(endpoint);
		return ENOMEM;
	}

	/*
	 * Publishes the initialized endpoint to interface notifications,
	 * unless the notification snapshot is already as large as it can be.
	 */
	irq = spin_lock_irqsave(&route_registry_lock);

	/* Counts the listeners already registered. */
	registered = 0;
	for (other = route_sockets; other != NULL; other = other->next)
		registered++;

	/* Links the new listener while the snapshot can still hold it. */
	if (registered < SOCKET_BROADCAST_MAX) {
		endpoint->next = route_sockets;
		route_sockets = endpoint;
	}

	spin_unlock_irqrestore(&route_registry_lock, irq);

	/* Refuses a listener the notifications could not reach. */
	if (registered >= SOCKET_BROADCAST_MAX) {
		route_endpoint_release_free_packets(endpoint);
		kern_free(endpoint);
		return ENFILE;
	}

	/* Returns the initialized socket to its caller. */
	*result = &endpoint->socket;

	/* Reports successful endpoint creation. */
	return 0;
}

/* Enqueues one event record on a routing endpoint. */
static void
route_enqueue(
	struct route_endpoint *endpoint,
	const void *record,
	size_t length)
{
	struct packet_buf *packet;
	struct rtm_header *output;
	unsigned long irq;
	void *room;
	int error;

	/* Locks the endpoint across queue selection and publication. */

	/* Ignores a listener that began closing after the registry snapshot. */
	irq = spin_lock_irqsave(&endpoint->socket.lock);
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
	    length > endpoint->socket.receive_hiwat_bytes ||
	    endpoint->socket.receive_bytes >
	    endpoint->socket.receive_hiwat_bytes - length) {
		/* Removes the oldest queued packet when one is available. */
		packet = endpoint->socket.receive_head;
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
		/* Removes one packet from the unused endpoint pool. */
		packet = endpoint->free_packets;
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

	/* The packet made the record's length (its storage holds the largest record). */
	error = packet_buf_trim(packet, 0U);
	room = NULL;
	if (error == 0)
		room = packet_buf_append(packet, length);
	if (room == NULL) {
		packet->next = endpoint->free_packets;
		endpoint->free_packets = packet;
		endpoint->overflow_pending = 1U;
		spin_unlock_irqrestore(&endpoint->socket.lock, irq);
		return;
	}

	/* Copies the immutable event into the selected packet. */
	kern_memcpy(room, record, length);
	output = room;

	/* Reports any event loss on the next retained record (its flags' place depends on its type). */
	if (endpoint->overflow_pending != 0U) {
		if (output->rtm_type == RTM_IFINFO) {
			((struct rtm_ifinfo *)room)->rtm_flags |= RTM_IFINFO_F_OVERFLOW;
		} else {
			((struct rtm_addrinfo *)room)->rtm_flags |= RTM_F_OVERFLOW;
		}

		/* Said once. */
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

	/* The queue's tail and counts. */
	endpoint->socket.receive_tail = packet;
	endpoint->socket.receive_packets++;
	endpoint->socket.receive_bytes += packet->length;

	/* Wakes one receiver after publishing the complete queue state. */
	waitq_wake_one(&endpoint->socket.receive_waitq);

	spin_unlock_irqrestore(&endpoint->socket.lock, irq);

	/* Wakes pollers after making the event readable. */
	poll_notify();
}
