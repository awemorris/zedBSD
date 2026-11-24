/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The relay between ssh-thread.c and sftp-worker.c: two byte rings,
 * channel lifecycle flags, and reference counting.
 */

#include "sftp-link.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One SFTP channel relay, shared by up to four holders (the terminal
 * session, the network thread, the worker thread, and the SFTP tab).
 * Everything after network_waker is protected by lock.
 */
struct sftp_link {
	struct kl_socket_waker *network_waker;
	struct kl_mutex lock;
	struct kl_cond worker_cond;
	unsigned references;

	/* Incoming ring: bytes from the remote sftp-server for the worker. */
	uint8_t *in_buf;
	size_t in_start;
	size_t in_length;
	size_t consumed;

	/* Outgoing ring: bytes from the worker for the remote sftp-server. */
	uint8_t *out_buf;
	size_t out_start;
	size_t out_length;

	/* Channel state as driven by the worker and observed by the network thread. */
	int open_wanted;
	int close_wanted;
	int open_in_flight;
	int channel_active;
	uint32_t channel;
	int eof_in;
	int channel_closed;
	int ended;

	/* Result of the latest subsystem open attempt. */
	int reply_ready;
	int reply_accepted;
	uint32_t reply_code;
	char reply_reason[SFTP_LINK_REASON_MAX];
};

static void ring_push(uint8_t *buf, size_t cap, size_t *start, size_t *length, const uint8_t *src, size_t count);
static void ring_pop(const uint8_t *buf, size_t cap, size_t *start, size_t *length, uint8_t *dst, size_t count);
static void ring_peek_span(const uint8_t *buf, size_t cap, size_t start, size_t length, const uint8_t **span, size_t *span_size);
static void ring_drop(size_t cap, size_t *start, size_t *length, size_t count);
static void wake_network(struct sftp_link *link);

/*
 * Creates a relay with reference count 1 and both byte rings allocated.
 * Returns 0, ENOMEM, or an error from initializing the mutex or condition.
 */
int
sftp_link_create(
	struct kl_socket_waker *network_waker,
	struct sftp_link **link_out)
{
	struct sftp_link *link;
	int error;

	/* Allocates the relay structure. */
	*link_out = NULL;
	link = calloc(1U, sizeof(*link));
	if (link == NULL)
		return ENOMEM;

	/* Allocates the two ring buffers. */
	link->in_buf = malloc(SFTP_LINK_IN_MAX);
	link->out_buf = malloc(SFTP_LINK_OUT_MAX);
	if (link->in_buf == NULL || link->out_buf == NULL) {
		free(link->in_buf);
		free(link->out_buf);
		free(link);
		return ENOMEM;
	}

	/* Initializes the relay mutex. */
	error = kl_mutex_init(&link->lock);
	if (error != 0) {
		free(link->in_buf);
		free(link->out_buf);
		free(link);
		return error;
	}

	/* Initializes the worker condition variable. */
	error = kl_cond_init(&link->worker_cond);
	if (error != 0) {
		kl_mutex_destroy(&link->lock);
		free(link->in_buf);
		free(link->out_buf);
		free(link);
		return error;
	}

	/* Succeeded: publishes the initialized relay with reference count 1. */
	link->network_waker = network_waker;
	link->references = 1U;
	*link_out = link;
	return 0;
}

/*
 * Adds one reference to the relay.
 */
void
sftp_link_retain(
	struct sftp_link *link)
{
	/* Increments the reference count under the relay lock. */
	kl_mutex_lock(&link->lock);

	link->references++;

	kl_mutex_unlock(&link->lock);
}

/*
 * Drops one reference and frees the relay when none remain.
 */
void
sftp_link_release(
	struct sftp_link *link)
{
	unsigned remaining;

	/* Ignores a NULL relay pointer. */
	if (link == NULL)
		return;

	/* Decrements the reference count under the relay lock. */
	kl_mutex_lock(&link->lock);

	remaining = --link->references;

	kl_mutex_unlock(&link->lock);

	/* Keeps the relay alive while other holders remain. */
	if (remaining != 0U)
		return;

	/* Releases synchronization primitives and ring buffers. */
	kl_cond_destroy(&link->worker_cond);
	kl_mutex_destroy(&link->lock);
	free(link->in_buf);
	free(link->out_buf);
	free(link);
}

/*
 * Asks the network thread to open a new "sftp" subsystem channel.
 */
void
sftp_link_request_open(
	struct sftp_link *link)
{
	/* Resets channel state and marks an open request pending. */
	kl_mutex_lock(&link->lock);

	link->in_start = 0U;
	link->in_length = 0U;
	link->consumed = 0U;
	link->out_start = 0U;
	link->out_length = 0U;
	link->eof_in = 0;
	link->channel_closed = 0;
	link->reply_ready = 0;
	link->reply_accepted = 0;
	link->reply_code = 0U;
	link->reply_reason[0] = '\0';
	link->open_wanted = 1;

	kl_mutex_unlock(&link->lock);

	/* Wakes the network thread to issue the channel open. */
	wake_network(link);
}

/*
 * Asks the network thread to close the active subsystem channel.
 */
void
sftp_link_request_close(
	struct sftp_link *link)
{
	/* Clears pending opens and marks a close wanted if a channel is active. */
	kl_mutex_lock(&link->lock);

	link->open_wanted = 0;
	if (link->channel_active)
		link->close_wanted = 1;
	kl_cond_broadcast(&link->worker_cond);

	kl_mutex_unlock(&link->lock);

	/* Wakes the network thread to send the channel close. */
	wake_network(link);
}

/*
 * Writes up to size bytes into the outgoing ring for the network thread.
 * Returns 0 on success (with *written set, possibly 0 if full), or EPIPE
 * if the channel or connection is closed.
 */
int
sftp_link_write(
	struct sftp_link *link,
	const uint8_t *bytes,
	size_t size,
	size_t *written)
{
	size_t room;
	size_t take;

	/* Starts with zero bytes written. */
	*written = 0U;

	/* Copies as many bytes as fit into the outgoing ring under the lock. */
	kl_mutex_lock(&link->lock);

	if (link->ended || link->channel_closed || link->close_wanted) {
		kl_mutex_unlock(&link->lock);
		return EPIPE;
	}

	/* Computes available ring room and pushes the prefix that fits. */
	room = SFTP_LINK_OUT_MAX - link->out_length;
	if (size < room)
		take = size;
	else
		take = room;
	if (take != 0U)
		ring_push(link->out_buf, SFTP_LINK_OUT_MAX, &link->out_start, &link->out_length, bytes, take);

	kl_mutex_unlock(&link->lock);

	/* Reports bytes written and wakes the network thread if any were queued. */
	*written = take;
	if (take != 0U)
		wake_network(link);

	/* Succeeded: *written holds the number of queued bytes. */
	return 0;
}

/*
 * Reads up to capacity bytes from the incoming ring.  Returns 0, with
 * *read_size set, *eof set when EOF arrived and the ring is empty, and
 * *closed set when the channel or connection has ended.
 */
int
sftp_link_read(
	struct sftp_link *link,
	uint8_t *bytes,
	size_t capacity,
	size_t *read_size,
	int *eof,
	int *closed)
{
	size_t take;
	int wake;

	/* Clears output parameters first. */
	*read_size = 0U;
	*eof = 0;
	*closed = 0;
	wake = 0;

	/* Pops available bytes from the incoming ring under the lock. */
	kl_mutex_lock(&link->lock);

	if (capacity < link->in_length)
		take = capacity;
	else
		take = link->in_length;
	if (take != 0U) {
		ring_pop(link->in_buf, SFTP_LINK_IN_MAX, &link->in_start, &link->in_length, bytes, take);
		link->consumed += take;
		if (link->consumed >= SFTP_LINK_CONSUMED_STEP)
			wake = 1;
	}

	/* Reports EOF or closed status once the incoming ring is drained. */
	if (link->in_length == 0U) {
		*eof = link->eof_in;
		*closed = link->channel_closed || link->ended;
	}

	kl_mutex_unlock(&link->lock);

	/* Reports read count and wakes the network thread to return window credits. */
	*read_size = take;
	if (wake)
		wake_network(link);

	/* Succeeded: *read_size holds the number of bytes read. */
	return 0;
}

/* Waits until the worker is signaled or timeout_ms elapses. */
void
sftp_link_wait(
	struct sftp_link *link,
	uint32_t timeout_ms)
{
	kl_mutex_lock(&link->lock);
	if (link->in_length == 0U && !link->channel_closed && !link->ended && !link->reply_ready)
		(void)kl_cond_wait_for(&link->worker_cond, &link->lock, timeout_ms);
	kl_mutex_unlock(&link->lock);
}

/* Wakes the worker thread if it is waiting on the relay. */
void
sftp_link_wake_worker(
	struct sftp_link *link)
{
	kl_mutex_lock(&link->lock);
	kl_cond_broadcast(&link->worker_cond);
	kl_mutex_unlock(&link->lock);
}

/*
 * Reads the latest subsystem open reply.  Sets *ready to 1 if a reply
 * has arrived, along with *accepted, *code, and reason.
 */
int
sftp_link_reply(
	struct sftp_link *link,
	int *ready,
	int *accepted,
	uint32_t *code,
	char *reason,
	size_t reason_cap)
{
	kl_mutex_lock(&link->lock);
	*ready = link->reply_ready;
	*accepted = link->reply_accepted;
	*code = link->reply_code;
	if (reason != NULL && reason_cap != 0U)
		(void)snprintf(reason, reason_cap, "%s", link->reply_reason);
	kl_mutex_unlock(&link->lock);
	return 0;
}

/* Returns 1 if the underlying SSH connection has ended. */
int
sftp_link_is_ended(
	struct sftp_link *link)
{
	int ended;

	/* Reads the ended flag under the relay lock. */
	kl_mutex_lock(&link->lock);

	ended = link->ended;

	kl_mutex_unlock(&link->lock);

	/* Returns whether the SSH connection has ended. */
	return ended;
}

/*
 * Returns 1 if the subsystem channel is currently open and active.
 */
int
sftp_link_is_open(
	struct sftp_link *link)
{
	int open;

	/* Checks the active channel flags under the relay lock. */
	kl_mutex_lock(&link->lock);

	open = link->channel_active && !link->open_in_flight && !link->channel_closed && !link->ended;

	kl_mutex_unlock(&link->lock);

	/* Returns whether the subsystem channel is ready for traffic. */
	return open;
}

/*
 * Returns the number of outgoing bytes currently buffered in the relay.
 */
size_t
sftp_link_out_pending(
	struct sftp_link *link)
{
	size_t pending;

	/* Reads the outgoing ring length under the relay lock. */
	kl_mutex_lock(&link->lock);

	pending = link->out_length;

	kl_mutex_unlock(&link->lock);

	/* Returns the pending outgoing byte count. */
	return pending;
}

/*
 * Moves pending open requests, consumed window credits, outgoing bytes,
 * and close requests between the relay and the SSH engine.
 */
void
sftp_link_pump(
	struct sftp_link *link,
	struct ssh_engine *engine)
{
	uint32_t channel;
	int open_now;
	int close_now;
	int active;
	int in_flight;
	size_t consumed;
	const uint8_t *span;
	size_t span_size;
	size_t written;
	size_t engine_pending;
	int woke_worker;
	int error;

	/* Initializes local tracking flags before sampling relay state. */
	open_now = 0;
	close_now = 0;
	consumed = 0U;
	woke_worker = 0;

	/* Samples pending open, close, and window-credit actions under the lock. */
	kl_mutex_lock(&link->lock);

	if (link->ended) {
		kl_mutex_unlock(&link->lock);
		return;
	}

	/* Takes any pending open request when no channel is active. */
	if (link->open_wanted && !link->channel_active && !link->open_in_flight) {
		open_now = 1;
		link->open_wanted = 0;
	}

	/* Captures channel state and any pending close or consumed credits. */
	active = link->channel_active;
	in_flight = link->open_in_flight;
	channel = link->channel;
	if (active && link->close_wanted) {
		close_now = 1;
		link->close_wanted = 0;
	}

	/* Claims accumulated consumed byte credits to return to the SSH engine. */
	if (active && !in_flight && link->consumed != 0U) {
		consumed = link->consumed;
		link->consumed = 0U;
	}

	kl_mutex_unlock(&link->lock);

	/* Opens the subsystem channel if requested. */
	if (open_now) {
		error = ssh_engine_open_subsystem(engine, "sftp", &channel);

		/* Records the subsystem open result under the relay lock. */
		kl_mutex_lock(&link->lock);

		if (error == 0) {
			link->channel = channel;
			link->channel_active = 1;
			link->open_in_flight = 1;
			active = 1;
			in_flight = 1;
		} else {
			link->reply_ready = 1;
			link->reply_accepted = 0;
			link->reply_code = 0U;
			(void)snprintf(link->reply_reason, sizeof(link->reply_reason),
			    "Could not open SFTP subsystem channel (errno %d).", error);
			woke_worker = 1;
		}

		kl_mutex_unlock(&link->lock);
	}

	/* Returns consumed window to the engine. */
	if (consumed != 0U)
		ssh_engine_channel_consumed(engine, channel, consumed);

	/* Sends outgoing bytes unless the engine output backlog is above the threshold. */
	if (active && !in_flight && !close_now) {
		for (;;) {
			(void)ssh_engine_output(engine, &engine_pending);
			if (engine_pending >= SFTP_LINK_ENGINE_BACKPRESSURE)
				break;

			/* Peeks at the next contiguous outgoing span under the lock. */
			kl_mutex_lock(&link->lock);

			if (!link->channel_active || link->open_in_flight || link->out_length == 0U) {
				kl_mutex_unlock(&link->lock);
				break;
			}

			/* Locates the next contiguous byte span to write. */
			ring_peek_span(link->out_buf, SFTP_LINK_OUT_MAX, link->out_start, link->out_length, &span, &span_size);

			kl_mutex_unlock(&link->lock);

			/* Writes the span to the SSH channel and drops sent bytes. */
			written = 0U;
			error = ssh_engine_channel_write(engine, channel, span, span_size, &written);
			if (written != 0U) {
				kl_mutex_lock(&link->lock);

				ring_drop(SFTP_LINK_OUT_MAX, &link->out_start, &link->out_length, written);
				woke_worker = 1;

				kl_mutex_unlock(&link->lock);
			}

			/* Stops pumping if the channel write stalled or errored. */
			if (error != 0 || written < span_size)
				break;
		}
	}

	/* Closes the subsystem channel if the worker asked for it. */
	if (close_now)
		(void)ssh_engine_channel_close(engine, channel);

	/* Wakes the worker thread if outgoing space opened or an open failed. */
	if (woke_worker)
		sftp_link_wake_worker(link);
}

/*
 * Consumes SSH engine events belonging to the SFTP subsystem channel.
 * Sets *taken to 1 when the event belonged to this relay.
 */
void
sftp_link_event(
	struct sftp_link *link,
	struct ssh_engine *engine,
	const struct ssh_engine_event *event,
	int *taken)
{
	size_t room;
	size_t copy_size;
	size_t overflow;

	/* Clears the taken flag before inspecting the event. */
	(void)engine;
	*taken = 0;

	/* Handles subsystem replies and channel events under the relay lock. */
	kl_mutex_lock(&link->lock);

	if (event->kind == SSH_ENGINE_SUBSYSTEM_REPLY) {
		if (link->open_in_flight && event->channel == link->channel) {
			*taken = 1;
			link->open_in_flight = 0;
			link->reply_ready = 1;
			link->reply_accepted = event->accepted;
			link->reply_code = event->code;
			(void)snprintf(link->reply_reason, sizeof(link->reply_reason), "%s", event->reason);
			if (!event->accepted && event->code != 0U) {
				/* OPEN_FAILURE freed the channel number immediately. */
				link->channel_active = 0;
				link->channel = 0U;
				link->channel_closed = 1;
			}

			/* Wakes the worker to observe the subsystem reply. */
			kl_cond_broadcast(&link->worker_cond);
		}

		/* Releases the lock after checking the subsystem reply. */
		kl_mutex_unlock(&link->lock);
		return;
	}

	/* Ignores events that do not belong to the active SFTP channel. */
	if (!link->channel_active || event->channel != link->channel) {
		kl_mutex_unlock(&link->lock);
		return;
	}

	/* Dispatches incoming data, EOF, and close events for the SFTP channel. */
	switch (event->kind) {
	case SSH_ENGINE_DATA:
		*taken = 1;
		room = SFTP_LINK_IN_MAX - link->in_length;
		if (event->size < room)
			copy_size = event->size;
		else
			copy_size = room;
		if (copy_size != 0U)
			ring_push(link->in_buf, SFTP_LINK_IN_MAX, &link->in_start, &link->in_length, event->bytes, copy_size);
		overflow = event->size - copy_size;
		if (overflow != 0U)
			link->consumed += overflow;
		kl_cond_broadcast(&link->worker_cond);
		break;
	case SSH_ENGINE_CHANNEL_EOF:
		*taken = 1;
		link->eof_in = 1;
		kl_cond_broadcast(&link->worker_cond);
		break;
	case SSH_ENGINE_CHANNEL_CLOSED:
		*taken = 1;
		link->channel_active = 0;
		link->open_in_flight = 0;
		link->channel = 0U;
		link->channel_closed = 1;
		kl_cond_broadcast(&link->worker_cond);
		break;
	default:
		break;
	}

	kl_mutex_unlock(&link->lock);
}

/*
 * Marks the relay ended when the SSH connection finishes.
 */
void
sftp_link_end(
	struct sftp_link *link)
{
	/* Clears active channel state and wakes the worker to exit. */
	kl_mutex_lock(&link->lock);

	link->ended = 1;
	link->open_wanted = 0;
	link->open_in_flight = 0;
	link->channel_active = 0;
	link->channel = 0U;
	link->channel_closed = 1;
	kl_cond_broadcast(&link->worker_cond);

	kl_mutex_unlock(&link->lock);
}

/*
 * Returns 1 if the relay currently holds an open or opening subsystem channel.
 */
int
sftp_link_channel_open(
	struct sftp_link *link)
{
	int open;

	/* Reads the active channel flag under the relay lock. */
	kl_mutex_lock(&link->lock);

	open = link->channel_active;

	kl_mutex_unlock(&link->lock);

	/* Returns whether the subsystem channel is active. */
	return open;
}

/*
 * Returns 1 if an open was requested and clears the pending flag.
 */
int
sftp_link_take_open(
	struct sftp_link *link)
{
	int wanted;

	/* Checks and claims a pending open request under the relay lock. */
	wanted = 0;
	kl_mutex_lock(&link->lock);

	if (link->open_wanted && !link->channel_active && !link->open_in_flight && !link->ended) {
		wanted = 1;
		link->open_wanted = 0;
		link->open_in_flight = 1;
		link->channel_closed = 0;
		link->eof_in = 0;
	}

	kl_mutex_unlock(&link->lock);

	/* Returns whether an open request was claimed. */
	return wanted;
}

/*
 * Returns 1 if a close was requested and clears the pending flag.
 */
int
sftp_link_take_close(
	struct sftp_link *link)
{
	int wanted;

	/* Checks and claims a pending close request under the relay lock. */
	wanted = 0;
	kl_mutex_lock(&link->lock);

	if (link->close_wanted) {
		wanted = 1;
		link->close_wanted = 0;
		link->channel_active = 0;
		link->open_in_flight = 0;
		link->channel_closed = 1;
		kl_cond_broadcast(&link->worker_cond);
	}

	kl_mutex_unlock(&link->lock);

	/* Returns whether a close request was claimed. */
	return wanted;
}

/*
 * Delivers a subsystem open reply to the relay.
 */
void
sftp_link_push_reply(
	struct sftp_link *link,
	uint32_t channel,
	int accepted,
	uint32_t code,
	const char *reason)
{
	/* Stores the subsystem reply and wakes the worker under the lock. */
	kl_mutex_lock(&link->lock);

	link->open_in_flight = 0;
	link->reply_ready = 1;
	link->reply_accepted = accepted;
	link->reply_code = code;
	if (reason != NULL)
		(void)snprintf(link->reply_reason, sizeof(link->reply_reason), "%s", reason);
	else
		link->reply_reason[0] = '\0';
	if (accepted) {
		link->channel = channel;
		link->channel_active = 1;
	} else {
		link->channel_active = 0;
		link->channel = 0U;
		link->channel_closed = 1;
	}

	/* Signals the worker thread that the reply is ready. */
	kl_cond_broadcast(&link->worker_cond);

	kl_mutex_unlock(&link->lock);
}

/*
 * Pops up to capacity bytes from the outgoing ring for a pipe relay.
 */
size_t
sftp_link_take_out(
	struct sftp_link *link,
	uint8_t *bytes,
	size_t capacity)
{
	size_t take;

	/* Pops outgoing bytes and wakes the worker if space freed up. */
	kl_mutex_lock(&link->lock);

	if (capacity < link->out_length)
		take = capacity;
	else
		take = link->out_length;
	if (take != 0U) {
		ring_pop(link->out_buf, SFTP_LINK_OUT_MAX, &link->out_start, &link->out_length, bytes, take);
		kl_cond_broadcast(&link->worker_cond);
	}

	kl_mutex_unlock(&link->lock);

	/* Returns the number of bytes popped. */
	return take;
}

/*
 * Pushes incoming bytes from a pipe relay into the incoming ring.
 */
void
sftp_link_push_in(
	struct sftp_link *link,
	const uint8_t *bytes,
	size_t size)
{
	size_t room;
	size_t copy_size;

	/* Appends incoming bytes to the ring and wakes the worker. */
	kl_mutex_lock(&link->lock);

	room = SFTP_LINK_IN_MAX - link->in_length;
	if (size < room)
		copy_size = size;
	else
		copy_size = room;
	if (copy_size != 0U)
		ring_push(link->in_buf, SFTP_LINK_IN_MAX, &link->in_start, &link->in_length, bytes, copy_size);
	kl_cond_broadcast(&link->worker_cond);

	kl_mutex_unlock(&link->lock);
}

/* Appends count bytes to a ring buffer (caller ensures enough room). */
static void
ring_push(
	uint8_t *buf,
	size_t cap,
	size_t *start,
	size_t *length,
	const uint8_t *src,
	size_t count)
{
	size_t tail;
	size_t first;

	/* Copies the first contiguous span at tail, then wraps around if needed. */
	tail = (*start + *length) % cap;
	first = cap - tail;
	if (first > count)
		first = count;
	memcpy(buf + tail, src, first);
	if (count > first)
		memcpy(buf, src + first, count - first);
	*length += count;
}

/* Copies and removes count bytes from the front of a ring buffer. */
static void
ring_pop(
	const uint8_t *buf,
	size_t cap,
	size_t *start,
	size_t *length,
	uint8_t *dst,
	size_t count)
{
	size_t first;

	/* Copies the leading span and any wrapped tail, then advances start. */
	first = cap - *start;
	if (first > count)
		first = count;
	memcpy(dst, buf + *start, first);
	if (count > first)
		memcpy(dst + first, buf, count - first);
	*start = (*start + count) % cap;
	*length -= count;
}

/* Returns the longest contiguous span at the front of a ring buffer. */
static void
ring_peek_span(
	const uint8_t *buf,
	size_t cap,
	size_t start,
	size_t length,
	const uint8_t **span,
	size_t *span_size)
{
	size_t first;

	/* Bounds the contiguous span by the ring wrap point. */
	first = cap - start;
	if (first > length)
		first = length;
	*span = buf + start;
	*span_size = first;
}

/* Drops count bytes from the front of a ring buffer. */
static void
ring_drop(
	size_t cap,
	size_t *start,
	size_t *length,
	size_t count)
{
	*start = (*start + count) % cap;
	*length -= count;
}

/* Wakes the network thread when a waker is attached. */
static void
wake_network(
	struct sftp_link *link)
{
	if (link->network_waker != NULL)
		kl_socket_waker_ring(link->network_waker);
}
