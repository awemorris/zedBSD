/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/lock.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/socket.h"
#include "kern/waitq.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zedbsd/netif.h>
#include <zedbsd/poll.h>
#include <zedbsd/route.h>

static const struct socket_family_ops *route_family;
static unsigned endpoint_allocations;
static unsigned packet_allocations;
static unsigned poll_notifications;

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	while (__atomic_exchange_n(&lock->held.value, 1U,
	    __ATOMIC_ACQUIRE) != 0U)
		;
	return 0U;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)enabled;
	__atomic_store_n(&lock->held.value, 0U, __ATOMIC_RELEASE);
}

void
waitq_init(struct wait_queue *queue, const char *name)
{
	memset(queue, 0, sizeof(*queue));
	queue->name = name;
}

void
waitq_wake_one(struct wait_queue *queue)
{
	queue->sequence++;
}

void
waitq_wake_all(struct wait_queue *queue)
{
	queue->sequence++;
}

void
poll_notify(void)
{
	poll_notifications++;
}

void *
kern_calloc(size_t count, size_t size)
{
	void *result = calloc(count, size);

	if (result != NULL)
		endpoint_allocations++;
	return result;
}

void
kern_free(void *pointer)
{
	if (pointer != NULL) {
		assert(endpoint_allocations != 0U);
		endpoint_allocations--;
	}
	free(pointer);
}

struct packet_buf *
packet_buf_alloc(size_t headroom)
{
	struct packet_buf *packet;

	packet = calloc(1, sizeof(*packet));
	if (packet == NULL)
		return NULL;
	packet->storage = malloc(PACKET_BUF_STORAGE_SIZE);
	if (packet->storage == NULL) {
		free(packet);
		return NULL;
	}
	packet->data = packet->storage + headroom;
	packet->capacity = PACKET_BUF_STORAGE_SIZE - headroom;
	packet_allocations++;
	return packet;
}

void *
packet_buf_append(struct packet_buf *packet, size_t length)
{
	void *result;

	if (packet == NULL || length > packet->capacity - packet->length)
		return NULL;
	result = packet->data + packet->length;
	packet->length += length;
	return result;
}

void
packet_buf_free(struct packet_buf *packet)
{
	if (packet == NULL)
		return;
	assert(packet_allocations != 0U);
	packet_allocations--;
	free(packet->storage);
	free(packet);
}

int
socket_family_register(int family, const struct socket_family_ops *ops)
{
	assert(family == AF_ROUTE && ops != NULL && ops->create != NULL);
	if (route_family != NULL)
		return EEXIST;
	route_family = ops;
	return 0;
}

void
socket_init_object(struct socket *socket, int family, int type, int protocol,
	const struct socket_ops *ops)
{
	memset(socket, 0, sizeof(*socket));
	socket->family = family;
	socket->type = type;
	socket->protocol = protocol;
	socket->ops = ops;
	refcount_init(&socket->refs, 1U);
	spin_init(&socket->lock, LOCK_RANK_SOCKET, "route test socket");
	waitq_init(&socket->receive_waitq, "route test receive");
	socket->lifecycle = SOCKET_OPEN;
	socket->receive_packet_limit = SOCKET_RECEIVE_MESSAGES_MAX;
	socket->receive_hiwat_bytes = SOCKET_BUFFER_DEFAULT;
}

int
socket_tryref(struct socket *socket)
{
	unsigned long irq = spin_lock_irqsave(&socket->lock);
	int result = socket->lifecycle == SOCKET_OPEN &&
	    refcount_tryget(&socket->refs);

	spin_unlock_irqrestore(&socket->lock, irq);
	return result;
}

void
socket_release(struct socket *socket)
{
	struct packet_buf *packet;

	if (!refcount_put(&socket->refs))
		return;
	socket->lifecycle = SOCKET_CLOSING;
	while ((packet = socket->receive_head) != NULL) {
		socket->receive_head = packet->next;
		packet_buf_free(packet);
	}
	socket->receive_tail = NULL;
	if (socket->ops != NULL && socket->ops->close != NULL)
		socket->ops->close(socket);
}

int
socket_dequeue_packet(struct socket *socket, int flags,
	struct packet_buf **result)
{
	struct packet_buf *packet;
	unsigned long irq;

	(void)flags;
	irq = spin_lock_irqsave(&socket->lock);
	packet = socket->receive_head;
	if (packet == NULL) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EAGAIN;
	}
	socket->receive_head = packet->next;
	if (socket->receive_head == NULL)
		socket->receive_tail = NULL;
	packet->next = NULL;
	socket->receive_packets--;
	socket->receive_bytes -= packet->length;
	spin_unlock_irqrestore(&socket->lock, irq);
	*result = packet;
	return 0;
}

int
socket_poll_common(struct socket *socket, short events, short *revents)
{
	short result = 0;
	unsigned long irq = spin_lock_irqsave(&socket->lock);

	if (socket->receive_head != NULL)
		result |= events & (POLLIN | POLLRDNORM);
	if (socket->lifecycle == SOCKET_OPEN)
		result |= events & (POLLOUT | POLLWRNORM);
	else
		result |= POLLHUP;
	spin_unlock_irqrestore(&socket->lock, irq);
	*revents = result;
	return 0;
}

void
net_worker_wakeup(void)
{
}

int
net_input_enqueue(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	packet_buf_free(packet);
	return 0;
}

static int
device_transmit(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	packet_buf_free(packet);
	return 0;
}

static const struct net_device_ops device_ops = {
	.transmit = device_transmit,
};

static struct net_device *
device_create(const char *name)
{
	struct net_device *device = net_device_alloc();

	assert(device != NULL);
	strcpy(device->name, name);
	device->mtu = 1500U;
	device->hwaddr_len = 6U;
	device->hwaddr[0] = 0x02U;
	device->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->ops = &device_ops;
	assert(net_device_create(device) == 0);
	return device;
}

static struct socket *
route_open(int type, int protocol, int expected)
{
	struct socket *socket = NULL;
	int error;

	assert(route_family != NULL);
	error = route_family->create(type, protocol, &socket);
	assert(error == expected);
	if (error != 0)
		assert(socket == NULL);
	return socket;
}

static struct rtm_ifinfo
route_read(struct socket *socket)
{
	struct rtm_ifinfo message;
	ssize_t length;

	memset(&message, 0xa5, sizeof(message));
	length = socket->ops->recvfrom(socket, &message, sizeof(message),
	    MSG_DONTWAIT, NULL, NULL);
	assert(length == (ssize_t)sizeof(message));
	assert(message.rtm_version == RTM_VERSION &&
	    message.rtm_type == RTM_IFINFO &&
	    message.rtm_length == sizeof(message) &&
	    message.rtm_sequence != 0U &&
	    message.rtm_ifindex != 0U &&
	    message.rtm_device_generation != 0U &&
	    message.rtm_reserved[0] == 0U && message.rtm_reserved[1] == 0U);
	return message;
}

int
main(void)
{
	struct net_device *device, *replacement;
	struct socket *observer, *restart, *socket;
	struct rtm_ifinfo message, observed;
	uint64_t generation;
	uint64_t sequence = 0U;
	unsigned ifindex;
	unsigned index;
	unsigned overflow_seen = 0U;
	short revents;
	uint8_t tiny[8];

	net_device_registry_init();
	assert(route_socket_init() == 0);
	assert(route_open(SOCK_DGRAM, 0, EPROTONOSUPPORT) == NULL);
	assert(route_open(SOCK_RAW, 1, EPROTONOSUPPORT) == NULL);
	socket = route_open(SOCK_RAW, 0, 0);
	assert(socket != NULL && endpoint_allocations == 1U &&
	    packet_allocations == SOCKET_RECEIVE_MESSAGES_MAX);
	assert(socket->ops->recvfrom(socket, &message, sizeof(message),
	    MSG_DONTWAIT, NULL, NULL) == -EAGAIN);
	assert(socket->ops->poll(socket, POLLIN | POLLOUT, &revents) == 0 &&
	    revents == 0);

	device = device_create("wlan0");
	ifindex = device->ifindex;
	generation = device->generation;
	assert(net_device_open(device) == 0);
	/* Subscribe before the canonical state snapshot.  A transition between
	 * snapshot and reconciliation is retained, with the same global sequence
	 * delivered independently to every live listener. */
	observer = route_open(SOCK_RAW, 0, 0);
	assert(observer != NULL && endpoint_allocations == 2U &&
	    packet_allocations == SOCKET_RECEIVE_MESSAGES_MAX * 2U);
	assert(net_device_set_carrier(device, 0) == 0);
	assert(socket->ops->recvfrom(socket, &message, sizeof(message),
	    MSG_DONTWAIT, NULL, NULL) == -EAGAIN);
	assert(net_device_set_carrier(device, 1) == 0);
	assert(socket->ops->poll(socket, POLLIN | POLLOUT, &revents) == 0 &&
	    revents == POLLIN);
	/* A short non-truncating read must leave the fixed record queued. */
	assert(socket->ops->recvfrom(socket, tiny, sizeof(tiny), MSG_DONTWAIT,
	    NULL, NULL) == -EMSGSIZE);
	message = route_read(socket);
	observed = route_read(observer);
	assert(message.rtm_ifindex == ifindex &&
	    message.rtm_device_generation == generation &&
	    message.rtm_transition == RTM_IFINFO_CARRIER_UP &&
	    (message.rtm_if_flags & (IFF_UP | IFF_RUNNING)) ==
	    (IFF_UP | IFF_RUNNING) && message.rtm_flags == 0U);
	assert(observed.rtm_sequence == message.rtm_sequence &&
	    observed.rtm_ifindex == message.rtm_ifindex &&
	    observed.rtm_device_generation == message.rtm_device_generation &&
	    observed.rtm_transition == message.rtm_transition);
	sequence = message.rtm_sequence;
	socket_release(observer);
	assert(endpoint_allocations == 1U &&
	    packet_allocations == SOCKET_RECEIVE_MESSAGES_MAX);
	assert(net_device_set_carrier(device, 1) == 0);
	assert(socket->ops->recvfrom(socket, &message, sizeof(message),
	    MSG_DONTWAIT, NULL, NULL) == -EAGAIN);

	assert(net_device_set_carrier(device, 0) == 0);
	memset(tiny, 0, sizeof(tiny));
	assert(socket->ops->recvfrom(socket, tiny, sizeof(tiny),
	    MSG_DONTWAIT | MSG_TRUNC, NULL, NULL) ==
	    (ssize_t)sizeof(struct rtm_ifinfo));
	assert(tiny[0] == RTM_VERSION && tiny[2] == RTM_IFINFO);

	/* The queue is bounded.  A dropped event is made explicit on a retained
	 * record so consumers know to resnapshot canonical interface state. */
	for (index = 0U; index < SOCKET_RECEIVE_MESSAGES_MAX + 6U; index++)
		assert(net_device_set_carrier(device, (index & 1U) == 0U) == 0);
	for (index = 0U; index < SOCKET_RECEIVE_MESSAGES_MAX; index++) {
		message = route_read(socket);
		assert(message.rtm_sequence > sequence);
		sequence = message.rtm_sequence;
		if ((message.rtm_flags & RTM_IFINFO_F_OVERFLOW) != 0U)
			overflow_seen = 1U;
	}
	assert(overflow_seen != 0U);
	assert(socket->ops->recvfrom(socket, &message, sizeof(message),
	    MSG_DONTWAIT, NULL, NULL) == -EAGAIN);

	assert(net_device_gone(device) == 0);
	message = route_read(socket);
	assert(message.rtm_ifindex == ifindex &&
	    message.rtm_device_generation == generation &&
	    message.rtm_transition == RTM_IFINFO_REMOVAL &&
	    (message.rtm_if_flags & (IFF_UP | IFF_RUNNING)) == 0U &&
	    message.rtm_sequence > sequence);
	assert(net_device_gone(device) == 0);
	assert(socket->ops->recvfrom(socket, &message, sizeof(message),
	    MSG_DONTWAIT, NULL, NULL) == -EAGAIN);
	net_device_destroy(device);

	/* A registry restart may reuse an ifindex, but never its generation. */
	net_device_registry_init();
	replacement = device_create("wlan0");
	assert(replacement->ifindex == ifindex &&
	    replacement->generation != generation);
	restart = route_open(SOCK_RAW, 0, 0);
	assert(restart != NULL && restart->ops->recvfrom(restart, &observed,
	    sizeof(observed), MSG_DONTWAIT, NULL, NULL) == -EAGAIN);
	assert(net_device_set_carrier(replacement, 1) == 0);
	message = route_read(socket);
	observed = route_read(restart);
	assert(message.rtm_ifindex == ifindex &&
	    message.rtm_device_generation == replacement->generation &&
	    message.rtm_transition == RTM_IFINFO_CARRIER_UP);
	assert(observed.rtm_sequence == message.rtm_sequence &&
	    observed.rtm_device_generation == message.rtm_device_generation);
	socket_release(restart);
	assert(net_device_gone(replacement) == 0);
	message = route_read(socket);
	assert(message.rtm_transition == RTM_IFINFO_REMOVAL);
	net_device_destroy(replacement);
	assert(packet_allocations == SOCKET_RECEIVE_MESSAGES_MAX);

	socket_release(socket);
	assert(endpoint_allocations == 0U && packet_allocations == 0U &&
	    poll_notifications != 0U);
	return 0;
}
