/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/socket.h"
#include "kern/net/packet-buf.h"
#include "kern/kmem.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/thread.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define SOCKET_FAMILY_MAX 32U
#define SOCKET_CLOCK_HZ 100U

static const struct socket_family_ops *families[SOCKET_FAMILY_MAX];
static unsigned socket_count;

void
socket_core_init(void)
{
	memset(families, 0, sizeof(families));
	socket_count = 0;
}

int
socket_family_register(int family, const struct socket_family_ops *ops)
{
	if (family < 0 || family >= (int)SOCKET_FAMILY_MAX || ops == NULL ||
	    ops->create == NULL)
		return EINVAL;
	if (families[family] != NULL)
		return EEXIST;
	families[family] = ops;
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
	socket->refcount = 1;
	socket->receive_limit = SOCKET_RECEIVE_PACKETS_MAX;
}

int
socket_create(int family, int type, int protocol, struct socket **result)
{
	int error;

	if (result == NULL || family < 0 || family >= (int)SOCKET_FAMILY_MAX ||
	    (type != SOCK_RAW && type != SOCK_DGRAM && type != SOCK_STREAM))
		return EINVAL;
	if (families[family] == NULL)
		return EAFNOSUPPORT;
	if (socket_count >= SOCKET_MAX)
		return ENFILE;
	error = families[family]->create(type, protocol, result);
	if (error == 0)
		socket_count++;
	return error;
}

int
socket_setsockopt_common(struct socket *socket, int level, int option,
			 const void *value, socklen_t length)
{
	struct timeval timeout;
	uint64_t ticks;

	if (socket == NULL || level != SOL_SOCKET || option != SO_RCVTIMEO)
		return EOPNOTSUPP;
	if (value == NULL || length != sizeof(timeout))
		return EINVAL;
	memcpy(&timeout, value, sizeof(timeout));
	if (timeout.tv_sec < 0 || timeout.tv_usec < 0 ||
	    timeout.tv_usec >= 1000000)
		return EINVAL;
	ticks = (uint64_t)timeout.tv_sec * SOCKET_CLOCK_HZ;
	ticks += ((uint64_t)timeout.tv_usec * SOCKET_CLOCK_HZ +
	    999999U) / 1000000U;
	socket->receive_timeout_ticks = ticks;
	return 0;
}

int
socket_getsockopt_common(struct socket *socket, int level, int option,
			 void *value, socklen_t *length)
{
	struct timeval timeout;

	if (socket == NULL || level != SOL_SOCKET || option != SO_RCVTIMEO)
		return EOPNOTSUPP;
	if (value == NULL || length == NULL || *length < sizeof(timeout))
		return EINVAL;
	timeout.tv_sec = (time_t)(socket->receive_timeout_ticks /
	    SOCKET_CLOCK_HZ);
	timeout.tv_usec = (long)((socket->receive_timeout_ticks %
	    SOCKET_CLOCK_HZ) * (1000000U / SOCKET_CLOCK_HZ));
	memcpy(value, &timeout, sizeof(timeout));
	*length = sizeof(timeout);
	return 0;
}

void
socket_ref(struct socket *socket)
{
	if (socket != NULL && socket->refcount != 0)
		socket->refcount++;
}

void
socket_release(struct socket *socket)
{
	struct packet_buf *packet;

	if (socket == NULL || socket->refcount == 0 || --socket->refcount != 0)
		return;
	while ((packet = socket->receive_head) != NULL) {
		socket->receive_head = packet->next;
		packet_buf_free(packet);
	}
	if (socket_count != 0)
		socket_count--;
	if (socket->ops != NULL && socket->ops->close != NULL)
		socket->ops->close(socket);
}

int
socket_enqueue_packet(struct socket *socket, struct packet_buf *packet)
{
	bool enabled;

	if (socket == NULL || packet == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}
	enabled = hal_irq_disable();
	if (socket->receive_packets >= socket->receive_limit) {
		if (enabled)
			hal_irq_enable();
		packet_buf_free(packet);
		return ENOBUFS;
	}
	packet->next = NULL;
	if (socket->receive_tail != NULL)
		socket->receive_tail->next = packet;
	else
		socket->receive_head = packet;
	socket->receive_tail = packet;
	socket->receive_packets++;
	socket->receive_bytes += packet->length;
	if (socket->receive_waiter != NULL)
		sched_wakeup(socket->receive_waiter);
	if (enabled)
		hal_irq_enable();
	return 0;
}

int
socket_requeue_packet_front(struct socket *socket, struct packet_buf *packet)
{
	bool enabled;

	if (socket == NULL || packet == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}
	enabled = hal_irq_disable();
	packet->next = socket->receive_head;
	socket->receive_head = packet;
	if (socket->receive_tail == NULL)
		socket->receive_tail = packet;
	socket->receive_packets++;
	socket->receive_bytes += packet->length;
	if (enabled)
		hal_irq_enable();
	return 0;
}

int
socket_dequeue_packet(struct socket *socket, int flags,
		      struct packet_buf **result)
{
	struct thread *thread;
	uint64_t deadline = 0;
	bool enabled;

	if (socket == NULL || result == NULL || (flags & ~MSG_DONTWAIT) != 0)
		return EINVAL;
	thread = thread_current();
	if (socket->receive_timeout_ticks != 0)
		deadline = sched_ticks() + socket->receive_timeout_ticks;
	enabled = hal_irq_disable();
	while (socket->receive_head == NULL) {
		if (socket->error != 0) {
			int error = socket->error;
			socket->error = 0;
			if (enabled)
				hal_irq_enable();
			return error;
		}
		if ((flags & MSG_DONTWAIT) != 0 || thread == NULL) {
			if (enabled)
				hal_irq_enable();
			return EAGAIN;
		}
		if (deadline != 0 && sched_ticks() >= deadline) {
			socket->receive_waiter = NULL;
			if (enabled)
				hal_irq_enable();
			return EAGAIN;
		}
		if (socket->receive_waiter != NULL &&
		    socket->receive_waiter != thread) {
			if (enabled)
				hal_irq_enable();
			return EBUSY;
		}
		socket->receive_waiter = thread;
		sched_sleep(deadline);
		if (signal_pending_unblocked(thread)) {
			socket->receive_waiter = NULL;
			if (enabled)
				hal_irq_enable();
			return EINTR;
		}
	}
	*result = socket->receive_head;
	socket->receive_head = (*result)->next;
	if (socket->receive_head == NULL)
		socket->receive_tail = NULL;
	(*result)->next = NULL;
	if (socket->receive_packets != 0)
		socket->receive_packets--;
	if (socket->receive_bytes >= (*result)->length)
		socket->receive_bytes -= (*result)->length;
	socket->receive_waiter = NULL;
	if (enabled)
		hal_irq_enable();
	return 0;
}
