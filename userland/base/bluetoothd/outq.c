/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The output queue of a client of bluetoothd's socket (ws197-p003, see
 * outq.h).
 */

#include "userland/base/bluetoothd/outq.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The first size of a queue's buffer. */
#define OUTQ_FIRST_SIZE		4096U

/* Prepares an empty queue that holds limit bytes at most. */
void
btd_outq_init(
	struct btd_outq *queue,
	size_t limit)
{
	/* Nothing held, no buffer yet. */
	memset(queue, 0, sizeof(*queue));
	queue->limit = limit;
}

/*
 * Adds bytes at the queue's end.  Returns 0, ENOBUFS when the queue would
 * hold more than its limit (nothing is added), or ENOMEM.
 */
int
btd_outq_append(
	struct btd_outq *queue,
	const void *data,
	size_t length)
{
	uint8_t *grown;
	size_t pending;
	size_t wanted;
	size_t size;

	/* Nothing to add. */
	if (length == 0U)
		return 0;

	/* Within the limit, with what is held. */
	pending = queue->used - queue->first;
	if (length > queue->limit || pending > queue->limit - length)
		return ENOBUFS;

	/* The bytes sent already make room at the front. */
	if (queue->first != 0U) {
		memmove(queue->bytes, queue->bytes + queue->first, pending);
		queue->first = 0U;
		queue->used = pending;
	}

	/* A buffer large enough, doubled from its first size. */
	wanted = pending + length;
	if (wanted > queue->capacity) {
		size = OUTQ_FIRST_SIZE;
		while (size < wanted)
			size *= 2U;

		/* Grown, keeping what it holds. */
		grown = realloc(queue->bytes, size);
		if (grown == NULL)
			return ENOMEM;
		queue->bytes = grown;
		queue->capacity = size;
	}

	/* Succeeded: added at the end. */
	memcpy(queue->bytes + queue->used, data, length);
	queue->used += length;
	return 0;
}

/*
 * Sends what the queue holds as far as the client takes it now.  Returns
 * 0 (all sent, or the client takes no more now), or the errno value of a
 * send that failed (the client is gone).
 */
int
btd_outq_flush(
	struct btd_outq *queue,
	btd_outq_send_fn send_fn,
	void *context)
{
	ssize_t sent;
	int error;

	/* Each send until all went or the client takes no more. */
	while (queue->first < queue->used) {
		sent = send_fn(context, queue->bytes + queue->first, queue->used - queue->first);
		if (sent > 0) {
			queue->first += (size_t)sent;
			continue;
		}

		/* An interrupted send is tried again. */
		error = errno;
		if (sent < 0 && error == EINTR)
			continue;

		/* The client takes no more now. */
		if (sent < 0 && (error == EAGAIN || error == EWOULDBLOCK))
			return 0;

		/* A send that took nothing without saying why is a client gone. */
		if (sent == 0)
			return EPIPE;

		/* The client is gone. */
		return error;
	}

	/* All sent: the queue starts again at its front. */
	queue->first = 0U;
	queue->used = 0U;

	/* Succeeded. */
	return 0;
}

/* Gives how many bytes the queue holds not sent yet. */
size_t
btd_outq_pending(
	const struct btd_outq *queue)
{
	/* The bytes from first to used. */
	return queue->used - queue->first;
}

/* Empties the queue and frees its buffer (its limit stays). */
void
btd_outq_clear(
	struct btd_outq *queue)
{
	size_t limit;

	/* The buffer freed. */
	free(queue->bytes);

	/* Succeeded: empty, with its limit. */
	limit = queue->limit;
	memset(queue, 0, sizeof(*queue));
	queue->limit = limit;
}
