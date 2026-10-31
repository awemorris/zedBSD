/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The storage thread of an instance and its queue.
 */

#include "storage/queue.h"
#include "base/base.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
int
wb_storage_queue_open(
	const struct browser_storage_calls *calls,
	void *context,
	struct wb_storage_queue **queue)
{
	UNUSED_PARAMETER(calls);
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(queue);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_queue_write(
	struct wb_storage_queue *queue,
	unsigned space,
	const char *key,
	const void *bytes,
	size_t length)
{
	UNUSED_PARAMETER(queue);
	UNUSED_PARAMETER(space);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_queue_remove(
	struct wb_storage_queue *queue,
	unsigned space,
	const char *key)
{
	UNUSED_PARAMETER(queue);
	UNUSED_PARAMETER(space);
	UNUSED_PARAMETER(key);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_queue_clear(
	struct wb_storage_queue *queue,
	unsigned space)
{
	UNUSED_PARAMETER(queue);
	UNUSED_PARAMETER(space);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_queue_read(
	struct wb_storage_queue *queue,
	unsigned space,
	const char *key,
	struct wb_buffer *out,
	int *found)
{
	UNUSED_PARAMETER(queue);
	UNUSED_PARAMETER(space);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(found);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_queue_list(
	struct wb_storage_queue *queue,
	unsigned space,
	struct wb_buffer *keys,
	size_t *count)
{
	UNUSED_PARAMETER(queue);
	UNUSED_PARAMETER(space);
	UNUSED_PARAMETER(keys);
	UNUSED_PARAMETER(count);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void
wb_storage_queue_flush(
	struct wb_storage_queue *queue)
{
	UNUSED_PARAMETER(queue);
}

/* Not implemented yet. */
int
wb_storage_queue_take_error(
	struct wb_storage_queue *queue)
{
	UNUSED_PARAMETER(queue);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void
wb_storage_queue_close(
	struct wb_storage_queue *queue)
{
	UNUSED_PARAMETER(queue);
}
