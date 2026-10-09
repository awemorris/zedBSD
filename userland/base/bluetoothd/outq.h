/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The output queue of one client of bluetoothd's socket (ws197-p003,
 * plan/ws197/phase003/phase.md section 4.1): what the daemon writes to the
 * client is added here and sent as far as the client takes it, never
 * waiting, so a client that does not read cannot stop the daemon's loop.
 * A queue grows as it must, up to its limit; what would pass the limit is
 * refused whole (the daemon then closes the client, or for a subscriber
 * drops the event and says so later).
 *
 * Without system calls: the caller hands in the send.  The host tests
 * build it.
 */

#ifndef BLUETOOTHD_OUTQ_H
#define BLUETOOTHD_OUTQ_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The most a client's queue holds. */
#define BTD_OUTQ_MAX		262144U

/*
 * Sends bytes to the client: how many went (0 or more), or -1 with errno
 * (EAGAIN when the client takes nothing now).
 */
typedef ssize_t (*btd_outq_send_fn)(void *context, const uint8_t *data, size_t length);

/*
 * One client's queue: its buffer (allocated on the first add, freed by
 * btd_outq_clear), the bytes not sent yet from first to used, the
 * buffer's size, and the limit of what it holds.
 */
struct btd_outq {
	uint8_t *bytes;
	size_t first;
	size_t used;
	size_t capacity;
	size_t limit;
};

void btd_outq_init(struct btd_outq *queue, size_t limit);
int btd_outq_append(struct btd_outq *queue, const void *data, size_t length);
int btd_outq_flush(struct btd_outq *queue, btd_outq_send_fn send_fn, void *context);
size_t btd_outq_pending(const struct btd_outq *queue);
void btd_outq_clear(struct btd_outq *queue);

#endif
