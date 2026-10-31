/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The storage thread of an instance (queue.c): the one thread that calls
 * the instance's backend, and the queue other threads hand it work by.
 *
 * Writes, removes and clears are queued and the caller goes on at once
 * (write-behind): the page's thread and the network threads never wait
 * for the disk.  Reads and lists are queued too and the caller waits for
 * the answer; because the queue runs in order, a read sees every write the
 * same queue was given before it, by any thread.  A queue without a
 * backend runs no thread: writes are dropped, reads find nothing.
 *
 * Locking: the queue's lock is the innermost lock of the engine.  Under it
 * the queue takes no other lock and calls neither the backend nor the
 * engine; callers may hold their own locks (the loader's, the cookie
 * jar's) when they call a queue function that does not wait (write,
 * remove, clear, take_error), and must hold none when they call one that
 * waits (read, list, flush, close).
 */

#ifndef KEILAND_BROWSER_STORAGE_QUEUE_H
#define KEILAND_BROWSER_STORAGE_QUEUE_H

#include "base/base.h"

#include <browser/storage.h>

#include <stddef.h>

/*
 * The most bytes of queued writes not yet done (64 MiB).  A write that
 * would pass it waits until the thread has done enough (the only time a
 * write waits).
 */
#define WB_STORAGE_QUEUE_LIMIT		(64UL * 1024UL * 1024UL)

/* The name of the storage thread (wb_os_thread_name). */
#define WB_STORAGE_QUEUE_THREAD		"browser-storage"

/*
 * One instance's queue and thread.  wb_storage_queue_open creates it and
 * wb_storage_queue_close destroys it; between the two any thread may use
 * it.
 */
struct wb_storage_queue;

/*
 * Creates a queue over a backend and starts its thread.
 *
 * calls:   the backend's table, or NULL for no backend.  When not NULL its
 *          version must be BROWSER_STORAGE_VERSION and read, write, remove,
 *          list and clear set.  Borrowed until close.
 * context: the backend's context.
 * queue:   receives the queue, or NULL on failure.
 * Returns 0, EINVAL (queue NULL), ENOTSUP (another version or a member
 * missing), ENOMEM, EAGAIN (no thread could be started).  On failure the
 * backend's close is not called here (the caller, which owns the context
 * then, calls it).
 */
int wb_storage_queue_open(const struct browser_storage_calls *calls, void *context, struct wb_storage_queue **queue);

/*
 * Queues a write of a copy of bytes under space and key.
 *
 * key:   a valid key (wb_storage_key_valid), copied.
 * bytes: length bytes, copied; NULL only when length is 0.
 * Returns 0 (queued, or dropped by a queue without a backend), EINVAL
 * (NULL arguments or an invalid key), EFBIG (length above
 * WB_STORAGE_QUEUE_LIMIT), ENOMEM.  The backend's error, if the write later
 * fails, is kept for wb_storage_queue_take_error.
 * A later write of the same space and key that is queued before the thread
 * has started the earlier one may replace it in the queue (only the last
 * bytes reach the backend).
 */
int wb_storage_queue_write(struct wb_storage_queue *queue, unsigned space, const char *key, const void *bytes, size_t length);

/* Queues a remove; arguments and returns as wb_storage_queue_write (without bytes). */
int wb_storage_queue_remove(struct wb_storage_queue *queue, unsigned space, const char *key);

/* Queues a clear of a whole space; returns 0, EINVAL (queue NULL), ENOMEM. */
int wb_storage_queue_clear(struct wb_storage_queue *queue, unsigned space);

/*
 * Reads a record, waiting for the queue to reach it.
 *
 * out:   the record's bytes are appended (nothing when it is absent or the
 *        read fails).
 * found: receives 1 when the record exists and was read, 0 otherwise.
 * Returns 0 (found or not), EINVAL (NULL arguments, an invalid key), ENOMEM,
 * or the backend's error (found 0).  A queue without a backend finds 0.
 * Must not be called from the storage thread (from a backend's call).
 */
int wb_storage_queue_read(struct wb_storage_queue *queue, unsigned space, const char *key, struct wb_buffer *out, int *found);

/*
 * Lists a space's keys, waiting for the queue to reach the list.
 *
 * keys: receives the keys appended one after another, each followed by a
 *       NUL byte ("key\0key\0"); nothing for an empty space.
 * count: receives how many keys were appended.
 * Only valid keys (wb_storage_key_valid) are kept; others from the backend
 * are skipped.  Returns 0, EINVAL, ENOMEM, or the backend's error (keys and
 * count as before the call).
 */
int wb_storage_queue_list(struct wb_storage_queue *queue, unsigned space, struct wb_buffer *keys, size_t *count);

/* Waits until everything queued before the call has been done. */
void wb_storage_queue_flush(struct wb_storage_queue *queue);

/*
 * Reports the first error a queued write, remove or clear met since the
 * last call (0 for none) and forgets it, for the engine to tell the page's
 * console once.  Does not wait.
 */
int wb_storage_queue_take_error(struct wb_storage_queue *queue);

/*
 * Destroys a queue: does everything queued, stops the thread, then calls
 * the backend's close (when the table has one) exactly once.  NULL does
 * nothing.  No other call on the queue may be in progress or follow.
 */
void wb_storage_queue_close(struct wb_storage_queue *queue);

#endif
