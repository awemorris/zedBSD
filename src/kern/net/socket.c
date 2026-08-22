/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/socket.h"
#include "kern/net/packet-buf.h"
#include "kern/clock.h"
#include "kern/kmem.h"
#include "kern/poll.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/thread.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define SOCKET_FAMILY_MAX 32U

static const struct socket_family_ops *families[SOCKET_FAMILY_MAX];
static atomic_uint_t socket_count;
static struct spinlock socket_registry_lock;

void
socket_core_init(void)
{
	memset(families, 0, sizeof(families));
	atomic_store_release(&socket_count, 0);
	spin_init(&socket_registry_lock, LOCK_RANK_SOCKET_REGISTRY,
	    "socket registry");
}

int
socket_family_register(int family, const struct socket_family_ops *ops)
{
	unsigned long irq;
	int error = 0;

	if (family < 0 || family >= (int)SOCKET_FAMILY_MAX || ops == NULL ||
	    ops->create == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&socket_registry_lock);
	if (families[family] != NULL)
		error = EEXIST;
	else
		families[family] = ops;
	spin_unlock_irqrestore(&socket_registry_lock, irq);
	return error;
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
	refcount_init(&socket->refs, 1);
	spin_init(&socket->lock, LOCK_RANK_SOCKET, "socket");
	waitq_init(&socket->receive_waitq, "socket receive");
	waitq_init(&socket->receive_space_waitq, "socket receive space");
	waitq_init(&socket->send_waitq, "socket send");
	waitq_init(&socket->connect_waitq, "socket connect");
	waitq_init(&socket->accept_waitq, "socket accept");
	socket->lifecycle = SOCKET_OPEN;
	socket->receive_packet_limit = SOCKET_RECEIVE_MESSAGES_MAX;
	socket->receive_hiwat_bytes = SOCKET_BUFFER_DEFAULT;
	socket->send_hiwat_bytes = SOCKET_BUFFER_DEFAULT;
}

int
socket_create(int family, int type, int protocol, struct socket **result)
{
	const struct socket_family_ops *family_ops;
	unsigned count, expected;
	unsigned long irq;
	int error;

	if (result == NULL || family < 0 || family >= (int)SOCKET_FAMILY_MAX ||
	    (type != SOCK_RAW && type != SOCK_DGRAM && type != SOCK_STREAM))
		return EINVAL;
	irq = spin_lock_irqsave(&socket_registry_lock);
	family_ops = families[family];
	spin_unlock_irqrestore(&socket_registry_lock, irq);
	if (family_ops == NULL)
		return EAFNOSUPPORT;
	for (;;) {
		count = atomic_load_acquire(&socket_count);
		if (count >= SOCKET_MAX)
			return ENFILE;
		expected = count;
		if (atomic_compare_exchange(&socket_count, &expected, count + 1U))
			break;
	}
	error = family_ops->create(type, protocol, result);
	if (error != 0)
		(void)atomic_raw_fetch_add_relaxed(&socket_count.value,
		    (unsigned)-1);
	return error;
}

int
socket_setsockopt_common(struct socket *socket, int level, int option,
			 const void *value, socklen_t length)
{
	struct timeval timeout;
	uint64_t ticks;
	unsigned long irq;

	if (socket == NULL || level != SOL_SOCKET)
		return ENOPROTOOPT;
	if (option == SO_REUSEADDR) {
		int enabled;
		if (value == NULL || length != sizeof(enabled)) return EINVAL;
		memcpy(&enabled, value, sizeof(enabled));
		irq = spin_lock_irqsave(&socket->lock);
		socket->reuse_address = enabled != 0;
		spin_unlock_irqrestore(&socket->lock, irq);
		return 0;
	}
	if (option == SO_SNDBUF || option == SO_RCVBUF) {
		int requested;
		if (value == NULL || length != sizeof(requested))
			return EINVAL;
		memcpy(&requested, value, sizeof(requested));
		if (requested < (int)SOCKET_BUFFER_MIN ||
		    requested > (int)SOCKET_BUFFER_MAX)
			return EINVAL;
		irq = spin_lock_irqsave(&socket->lock);
		if (option == SO_SNDBUF)
			socket->send_hiwat_bytes = (size_t)requested;
		else
			socket->receive_hiwat_bytes = (size_t)requested;
		waitq_wake_all(&socket->receive_space_waitq);
		waitq_wake_all(&socket->send_waitq);
		spin_unlock_irqrestore(&socket->lock, irq);
		if (socket->ops != NULL && socket->ops->buffer_changed != NULL)
			socket->ops->buffer_changed(socket, option);
		poll_notify();
		return 0;
	}
	if (option != SO_RCVTIMEO && option != SO_SNDTIMEO)
		return ENOPROTOOPT;
	if (value == NULL || length != sizeof(timeout))
		return EINVAL;
	memcpy(&timeout, value, sizeof(timeout));
	if (timeout.tv_sec < 0 || timeout.tv_usec < 0 ||
	    timeout.tv_usec >= 1000000)
		return EINVAL;
	if ((uint64_t)timeout.tv_sec > UINT64_MAX / KERN_CLOCK_HZ)
		return EOVERFLOW;
	ticks = (uint64_t)timeout.tv_sec * KERN_CLOCK_HZ;
	{
		uint64_t fraction = ((uint64_t)timeout.tv_usec *
		    KERN_CLOCK_HZ + 999999U) / 1000000U;
		if (ticks > UINT64_MAX - fraction)
			return EOVERFLOW;
		ticks += fraction;
	}
	irq = spin_lock_irqsave(&socket->lock);
	if (option == SO_RCVTIMEO)
		socket->receive_timeout_ticks = ticks;
	else
		socket->send_timeout_ticks = ticks;
	spin_unlock_irqrestore(&socket->lock, irq);
	return 0;
}

int
socket_getsockopt_common(struct socket *socket, int level, int option,
			 void *value, socklen_t *length)
{
	struct timeval timeout;
	unsigned long irq;

	if (socket == NULL || level != SOL_SOCKET)
		return ENOPROTOOPT;
	if (option == SO_ERROR) {
		int error;
		if (value == NULL || length == NULL || *length < sizeof(error))
			return EINVAL;
		error = socket_take_error(socket);
		memcpy(value, &error, sizeof(error));
		*length = sizeof(error);
		return 0;
	}
	if (option == SO_TYPE || option == SO_ATMARK) {
		int result;
		if (value == NULL || length == NULL || *length < sizeof(result))
			return EINVAL;
		/* The current protocols do not implement out-of-band data, hence a
		 * live socket can never be positioned at an OOB mark. */
		result = option == SO_TYPE ? socket->type : 0;
		memcpy(value, &result, sizeof(result));
		*length = sizeof(result);
		return 0;
	}
	if (option == SO_REUSEADDR) {
		int enabled;
		if (value == NULL || length == NULL || *length < sizeof(enabled))
			return EINVAL;
		irq = spin_lock_irqsave(&socket->lock);
		enabled = socket->reuse_address != 0;
		spin_unlock_irqrestore(&socket->lock, irq);
		memcpy(value, &enabled, sizeof(enabled));
		*length = sizeof(enabled);
		return 0;
	}
	if (option == SO_SNDBUF || option == SO_RCVBUF) {
		int configured;
		if (value == NULL || length == NULL ||
		    *length < sizeof(configured))
			return EINVAL;
		irq = spin_lock_irqsave(&socket->lock);
		configured = (int)(option == SO_SNDBUF ?
		    socket->send_hiwat_bytes : socket->receive_hiwat_bytes);
		spin_unlock_irqrestore(&socket->lock, irq);
		memcpy(value, &configured, sizeof(configured));
		*length = sizeof(configured);
		return 0;
	}
	if (option != SO_RCVTIMEO && option != SO_SNDTIMEO)
		return ENOPROTOOPT;
	if (value == NULL || length == NULL || *length < sizeof(timeout))
		return EINVAL;
	{
		uint64_t ticks;
		irq = spin_lock_irqsave(&socket->lock);
		ticks = option == SO_RCVTIMEO ? socket->receive_timeout_ticks :
		    socket->send_timeout_ticks;
		spin_unlock_irqrestore(&socket->lock, irq);
		timeout.tv_sec = (time_t)(ticks / KERN_CLOCK_HZ);
		timeout.tv_usec = (long)((ticks % KERN_CLOCK_HZ) *
		    (1000000U / KERN_CLOCK_HZ));
	}
	memcpy(value, &timeout, sizeof(timeout));
	*length = sizeof(timeout);
	return 0;
}

int
socket_take_error(struct socket *socket)
{
	int error;
	unsigned long irq;

	if (socket == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&socket->lock);
	error = socket->error;
	socket->error = 0;
	spin_unlock_irqrestore(&socket->lock, irq);
	return error;
}

void
socket_set_error(struct socket *socket, int error)
{
	unsigned long irq;

	if (socket == NULL || error == 0)
		return;
	irq = spin_lock_irqsave(&socket->lock);
	if (socket->error == 0)
		socket->error = error;
	waitq_wake_all(&socket->receive_waitq);
	waitq_wake_all(&socket->receive_space_waitq);
	waitq_wake_all(&socket->send_waitq);
	waitq_wake_all(&socket->connect_waitq);
	waitq_wake_all(&socket->accept_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);
	poll_notify();
}

void
socket_ref(struct socket *socket)
{
	if (socket != NULL)
		refcount_get(&socket->refs);
}

int
socket_tryref(struct socket *socket)
{
	return socket != NULL && refcount_tryget(&socket->refs);
}

void
socket_close_endpoint(struct socket *socket)
{
	unsigned long irq;
	int close = 0;

	if (socket == NULL || socket->ops == NULL ||
	    socket->ops->endpoint_close == NULL)
		return;
	irq = spin_lock_irqsave(&socket->lock);
	if (socket->lifecycle == SOCKET_OPEN) {
		socket->lifecycle = SOCKET_CLOSING;
		socket->read_shutdown = 1;
		socket->write_shutdown = 1;
		waitq_wake_all(&socket->receive_waitq);
		waitq_wake_all(&socket->receive_space_waitq);
		waitq_wake_all(&socket->send_waitq);
		waitq_wake_all(&socket->connect_waitq);
		waitq_wake_all(&socket->accept_waitq);
		close = 1;
	}
	spin_unlock_irqrestore(&socket->lock, irq);
	if (!close)
		return;
	socket->ops->endpoint_close(socket);
	irq = spin_lock_irqsave(&socket->lock);
	socket->lifecycle = SOCKET_CLOSED;
	waitq_wake_all(&socket->receive_waitq);
	waitq_wake_all(&socket->receive_space_waitq);
	waitq_wake_all(&socket->send_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);
	poll_notify();
}

void
socket_release(struct socket *socket)
{
	struct packet_buf *packet, *packets;
	unsigned long irq;

	if (socket == NULL || !refcount_put(&socket->refs))
		return;
	socket_close_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	if (socket->lifecycle == SOCKET_OPEN)
		socket->lifecycle = SOCKET_CLOSING;
	packets = socket->receive_head;
	socket->receive_head = socket->receive_tail = NULL;
	socket->receive_packets = 0;
	socket->receive_bytes = 0;
	waitq_wake_all(&socket->receive_waitq);
	waitq_wake_all(&socket->receive_space_waitq);
	waitq_wake_all(&socket->send_waitq);
	waitq_wake_all(&socket->connect_waitq);
	waitq_wake_all(&socket->accept_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);
	poll_notify();
	while ((packet = packets) != NULL) {
		packets = packet->next;
		packet_buf_free(packet);
	}
	if (socket->ops != NULL && socket->ops->close != NULL)
		socket->ops->close(socket);
	(void)atomic_raw_fetch_add_relaxed(&socket_count.value, (unsigned)-1);
}

unsigned socket_count_current(void)
{ return atomic_load_acquire(&socket_count); }

int
socket_enqueue_packet(struct socket *socket, struct packet_buf *packet)
{
	unsigned long irq;

	if (socket == NULL || packet == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}
	irq = spin_lock_irqsave(&socket->lock);
	if (socket->lifecycle != SOCKET_OPEN ||
	    (socket->receive_packet_limit != 0 &&
	    socket->receive_packets >= socket->receive_packet_limit) ||
	    packet->length > socket->receive_hiwat_bytes ||
	    socket->receive_bytes > socket->receive_hiwat_bytes - packet->length) {
		int error = socket->lifecycle != SOCKET_OPEN ? EPIPE : ENOBUFS;
		spin_unlock_irqrestore(&socket->lock, irq);
		packet_buf_free(packet);
		return error;
	}
	packet->next = NULL;
	if (socket->receive_tail != NULL)
		socket->receive_tail->next = packet;
	else
		socket->receive_head = packet;
	socket->receive_tail = packet;
	socket->receive_packets++;
	socket->receive_bytes += packet->length;
	waitq_wake_one(&socket->receive_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);
	poll_notify();
	return 0;
}

int
socket_enqueue_packet_wait(struct socket *socket, struct packet_buf *packet,
	int flags, uint64_t timeout_ticks)
{
	uint64_t deadline = 0;
	unsigned long irq;
	int error = 0;

	if (socket == NULL || packet == NULL || (flags & ~MSG_DONTWAIT) != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}
	if (timeout_ticks != 0 &&
	    kern_deadline_after(sched_ticks(), timeout_ticks, &deadline) != 0) {
		packet_buf_free(packet);
		return EOVERFLOW;
	}
	irq = spin_lock_irqsave(&socket->lock);
	for (;;) {
		int full = (socket->receive_packet_limit != 0 &&
		    socket->receive_packets >= socket->receive_packet_limit) ||
		    packet->length > socket->receive_hiwat_bytes ||
		    socket->receive_bytes >
		    socket->receive_hiwat_bytes - packet->length;
		if (socket->lifecycle != SOCKET_OPEN || socket->read_shutdown) {
			error = EPIPE;
			break;
		}
		if (!full) {
			packet->next = NULL;
			if (socket->receive_tail != NULL)
				socket->receive_tail->next = packet;
			else
				socket->receive_head = packet;
			socket->receive_tail = packet;
			socket->receive_packets++;
			socket->receive_bytes += packet->length;
			waitq_wake_one(&socket->receive_waitq);
			break;
		}
		if ((flags & MSG_DONTWAIT) != 0 || thread_current() == NULL) {
			error = EAGAIN;
			break;
		}
		if (deadline != 0 && sched_ticks() >= deadline) {
			error = EAGAIN;
			break;
		}
		{
			uint64_t sequence =
			    waitq_sequence(&socket->receive_space_waitq);
			error = waitq_sleep(&socket->receive_space_waitq,
			    &socket->lock, sequence, deadline, WAITQ_INTERRUPTIBLE);
			if (error == ETIMEDOUT)
				error = EAGAIN;
			if (error != 0)
				break;
		}
	}
	spin_unlock_irqrestore(&socket->lock, irq);
	if (error != 0)
		packet_buf_free(packet);
	else
		poll_notify();
	return error;
}

int
socket_requeue_packet_front(struct socket *socket, struct packet_buf *packet)
{
	unsigned long irq;

	if (socket == NULL || packet == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}
	irq = spin_lock_irqsave(&socket->lock);
	if (socket->lifecycle != SOCKET_OPEN) {
		spin_unlock_irqrestore(&socket->lock, irq);
		packet_buf_free(packet);
		return EPIPE;
	}
	packet->next = socket->receive_head;
	socket->receive_head = packet;
	if (socket->receive_tail == NULL)
		socket->receive_tail = packet;
	socket->receive_packets++;
	socket->receive_bytes += packet->length;
	waitq_wake_one(&socket->receive_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);
	poll_notify();
	return 0;
}

int
socket_dequeue_packet(struct socket *socket, int flags,
		      struct packet_buf **result)
{
	uint64_t deadline = 0;
	unsigned long irq;

	if (socket == NULL || result == NULL || (flags & ~MSG_DONTWAIT) != 0)
		return EINVAL;
	irq = spin_lock_irqsave(&socket->lock);
	if (socket->receive_timeout_ticks != 0 &&
	    kern_deadline_after(sched_ticks(), socket->receive_timeout_ticks,
	    &deadline) != 0)
	{
		spin_unlock_irqrestore(&socket->lock, irq);
		return EOVERFLOW;
	}
	while (socket->receive_head == NULL) {
		if (socket->error != 0) {
			int error = socket->error;
			socket->error = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return error;
		}
		if (socket->lifecycle != SOCKET_OPEN) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EPIPE;
		}
		if ((flags & MSG_DONTWAIT) != 0 || thread_current() == NULL) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EAGAIN;
		}
		if (deadline != 0 && sched_ticks() >= deadline) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EAGAIN;
		}
		{
			uint64_t sequence = waitq_sequence(&socket->receive_waitq);
			int error = waitq_sleep(&socket->receive_waitq, &socket->lock,
			    sequence, deadline, WAITQ_INTERRUPTIBLE);
			if (error == EINTR) {
				spin_unlock_irqrestore(&socket->lock, irq);
				return EINTR;
			}
			if (error == ETIMEDOUT) {
				spin_unlock_irqrestore(&socket->lock, irq);
				return EAGAIN;
			}
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
	waitq_wake_all(&socket->receive_space_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);
	return 0;
}

static void
socket_wake_queue(struct socket *socket, struct wait_queue *queue)
{
	unsigned long irq;

	if (socket == NULL)
		return;
	irq = spin_lock_irqsave(&socket->lock);
	waitq_wake_all(queue);
	spin_unlock_irqrestore(&socket->lock, irq);
	poll_notify();
}

void socket_wake_receive(struct socket *socket)
{ if (socket != NULL) socket_wake_queue(socket, &socket->receive_waitq); }
void socket_wake_send(struct socket *socket)
{ if (socket != NULL) socket_wake_queue(socket, &socket->send_waitq); }
void socket_wake_connect(struct socket *socket)
{ if (socket != NULL) socket_wake_queue(socket, &socket->connect_waitq); }
void socket_wake_accept(struct socket *socket)
{ if (socket != NULL) socket_wake_queue(socket, &socket->accept_waitq); }

int
socket_poll_common(struct socket *socket, short events, short *revents)
{
	short result = 0;
	unsigned long irq;

	if (socket == NULL || revents == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&socket->lock);
	if (socket->receive_head != NULL || socket->read_shutdown ||
	    socket->lifecycle != SOCKET_OPEN)
		result |= events & (POLLIN | POLLRDNORM);
	if (socket->error != 0)
		result |= POLLERR;
	if (socket->lifecycle != SOCKET_OPEN || socket->read_shutdown)
		result |= POLLHUP;
	if (!socket->write_shutdown && socket->lifecycle == SOCKET_OPEN)
		result |= events & (POLLOUT | POLLWRNORM);
	else if (socket->write_shutdown)
		result |= POLLERR;
	spin_unlock_irqrestore(&socket->lock, irq);
	*revents = result;
	return 0;
}
