/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.265 elementary stream reader of the Vulkan Video path.
 */

#include "h265-stream.h"

#include <errno.h>

/*
 * Makes an empty reader: no buffer, no parameter sets, waiting for an IRAP
 * picture.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-stream.h.
 */
void
h265_stream_init(
	struct h265_stream *stream)
{
	UNUSED_PARAMETER(stream);

	/* Does nothing yet. */
	return;
}

/*
 * Frees what the reader allocated (slice_rbsp); the reader may be
 * initialized again.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-stream.h.
 */
void
h265_stream_destroy(
	struct h265_stream *stream)
{
	UNUSED_PARAMETER(stream);

	/* Does nothing yet. */
	return;
}

/*
 * Gives the reader a new buffer of whole access units (data is NULL only
 * when size is 0) and moves the cursor to its start.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-stream.h.
 */
void
h265_stream_input(
	struct h265_stream *stream,
	const uint8_t *data,
	size_t size)
{
	UNUSED_PARAMETER(stream);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);

	/* Does nothing yet. */
	return;
}

/*
 * Reads the parameter sets of a buffer that holds no picture into the
 * store.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-stream.h.
 */
int
h265_stream_parameters(
	struct h265_stream *stream,
	const uint8_t *data,
	size_t size)
{
	UNUSED_PARAMETER(stream);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads the next picture to decode from the buffer.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-stream.h.
 */
int
h265_stream_next(
	struct h265_stream *stream,
	struct h265_picture *picture)
{
	UNUSED_PARAMETER(stream);
	UNUSED_PARAMETER(picture);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Starts again from a random access point (a seek).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-stream.h.
 */
void
h265_stream_reset(
	struct h265_stream *stream)
{
	UNUSED_PARAMETER(stream);

	/* Does nothing yet. */
	return;
}
