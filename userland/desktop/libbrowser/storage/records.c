/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The records of the storage: keys, cookies, cache entries and localStorage areas.
 */

#include "storage/records.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
void
wb_storage_sink_init(
	struct browser_storage_sink *sink,
	struct wb_buffer *buffer)
{
	UNUSED_PARAMETER(sink);
	UNUSED_PARAMETER(buffer);
}

/* Not implemented yet. */
int
wb_storage_sink_error(
	const struct browser_storage_sink *sink)
{
	UNUSED_PARAMETER(sink);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
uint64_t
wb_storage_hash(
	const void *bytes,
	size_t length)
{
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);

	/* A skeleton: the function is not written yet. */
	return 0;
}

/* Not implemented yet. */
int
wb_storage_cookie_original_key(
	const struct wb_storage_cookie *cookie,
	struct wb_buffer *out)
{
	UNUSED_PARAMETER(cookie);
	UNUSED_PARAMETER(out);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_key_text(
	const void *original,
	size_t length,
	unsigned collision,
	char *text)
{
	UNUSED_PARAMETER(original);
	UNUSED_PARAMETER(length);
	UNUSED_PARAMETER(collision);
	UNUSED_PARAMETER(text);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_key_valid(
	const char *text)
{
	UNUSED_PARAMETER(text);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_cookie_encode(
	const struct wb_storage_cookie *cookie,
	struct wb_buffer *out)
{
	UNUSED_PARAMETER(cookie);
	UNUSED_PARAMETER(out);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_cookie_decode(
	const void *bytes,
	size_t length,
	struct wb_storage_cookie *cookie,
	const unsigned char **original,
	size_t *original_length)
{
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);
	UNUSED_PARAMETER(cookie);
	UNUSED_PARAMETER(original);
	UNUSED_PARAMETER(original_length);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_cache_encode(
	const struct wb_storage_cache_entry *entry,
	struct wb_buffer *out)
{
	UNUSED_PARAMETER(entry);
	UNUSED_PARAMETER(out);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_cache_decode(
	const void *bytes,
	size_t length,
	struct wb_storage_cache_entry *entry)
{
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);
	UNUSED_PARAMETER(entry);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_local_encode(
	const char *origin,
	size_t origin_length,
	const struct wb_storage_local_item *items,
	size_t count,
	struct wb_buffer *out)
{
	UNUSED_PARAMETER(origin);
	UNUSED_PARAMETER(origin_length);
	UNUSED_PARAMETER(items);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(out);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_storage_local_decode(
	const void *bytes,
	size_t length,
	const unsigned char **origin,
	size_t *origin_length,
	wb_storage_local_each each,
	void *context)
{
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);
	UNUSED_PARAMETER(origin);
	UNUSED_PARAMETER(origin_length);
	UNUSED_PARAMETER(each);
	UNUSED_PARAMETER(context);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
browser_storage_sink_append(
	struct browser_storage_sink *sink,
	const void *bytes,
	size_t length)
{
	UNUSED_PARAMETER(sink);
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}
