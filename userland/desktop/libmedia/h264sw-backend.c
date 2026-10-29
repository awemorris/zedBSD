/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libmedia's back end for the software H.264 decoder.
 */

#include "h264sw-backend.h"
#include "h264sw-decoder.h"
#include "picture.h"

#include <errno.h>
#include <stddef.h>

static int h264sw_backend_load(void);
static const char *h264sw_backend_reason(void);
static int h264sw_backend_open(const struct media_track *track, void **state);
static const char *h264sw_backend_name(const void *state);
static int h264sw_backend_send(void *state, const struct media_packet *packet);
static int h264sw_backend_receive(void *state, int64_t *time_us);
static void *h264sw_backend_picture(void *state);
static size_t h264sw_backend_sound(void *state, int16_t *samples, size_t capacity, uint32_t rate);
static void h264sw_backend_flush(void *state);
static void h264sw_backend_close(void *state);

/*
 * The back end's operations, in the order of struct media_decoder_ops;
 * the pictures are picture.h's, in the CPU's memory.
 */
const struct media_decoder_ops media_h264sw_ops = {
	"h264-software",
	h264sw_backend_load,
	h264sw_backend_reason,
	h264sw_backend_open,
	h264sw_backend_name,
	h264sw_backend_send,
	h264sw_backend_receive,
	h264sw_backend_picture,
	h264sw_backend_sound,
	h264sw_backend_flush,
	h264sw_backend_close,
	media_picture_unref,
	media_picture_size,
	media_picture_scale,
	media_picture_scaler_free,
	NULL,
	NULL,
	media_picture_aspect,
	NULL
};

/* Reports that the back end needs nothing loaded. */
static int
h264sw_backend_load(
	void)
{
	/* Succeeded: the decoder is part of the library. */
	return 0;
}

/* Reports no process-wide problem. */
static const char *
h264sw_backend_reason(
	void)
{
	/* The back end has no process-wide failure to describe. */
	return "";
}

/* Opens a decoder for a track; not implemented yet, so every track passes to the next back end. */
static int
h264sw_backend_open(
	const struct media_track *track,
	void **state)
{
	UNUSED_PARAMETER(track);

	/* Passes the track on until the decoder is implemented. */
	*state = NULL;
	return MEDIA_PROBLEM_FORMAT;
}

/* Names the codec the back end decodes. */
static const char *
h264sw_backend_name(
	const void *state)
{
	UNUSED_PARAMETER(state);

	/* The codec's name, as the other back ends report it. */
	return "h264";
}

/* Sends a packet; not implemented yet. */
static int
h264sw_backend_send(
	void *state,
	const struct media_packet *packet)
{
	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(packet);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/* Receives a picture; not implemented yet. */
static int
h264sw_backend_receive(
	void *state,
	int64_t *time_us)
{
	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(time_us);

	/* Reports that the function is not implemented yet. */
	return -ENOSYS;
}

/* Hands out the current picture; not implemented yet. */
static void *
h264sw_backend_picture(
	void *state)
{
	UNUSED_PARAMETER(state);

	/* No picture is made yet. */
	return NULL;
}

/* Gives no sound: H.264 is video. */
static size_t
h264sw_backend_sound(
	void *state,
	int16_t *samples,
	size_t capacity,
	uint32_t rate)
{
	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(samples);
	UNUSED_PARAMETER(capacity);
	UNUSED_PARAMETER(rate);

	/* A video decoder has no samples. */
	return 0U;
}

/* Drops the decoding state for a seek; not implemented yet. */
static void
h264sw_backend_flush(
	void *state)
{
	UNUSED_PARAMETER(state);

	/* Does nothing yet. */
	return;
}

/* Frees the back end's state; not implemented yet. */
static void
h264sw_backend_close(
	void *state)
{
	UNUSED_PARAMETER(state);

	/* Does nothing yet. */
	return;
}
