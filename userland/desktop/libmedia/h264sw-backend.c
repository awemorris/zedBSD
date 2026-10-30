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
#include "h264sw-nal.h"
#include "picture.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The pictures of the pool kept idle beyond the ones in the queue. */
#define H264SW_BACKEND_SPARE 4U

/* The largest side of a picture the library's pictures hold, in luma samples. */
#define H264SW_BACKEND_SIDE_MAX 8192U

/*
 * One picture waiting to be received, with the time of its frame.
 */
struct h264sw_backend_entry {
	struct media_picture *picture;
	int64_t time_us;
};

/*
 * The state of one open back end: the bitstream conversion, the decoder,
 * the pool the pictures come from, the queue of finished pictures, and the
 * rest of a packet that did not fit the queue yet.
 *
 * pool and pool_width, pool_height: the pictures' pool and the cropped size
 * it was made for; a frame of another size makes a new pool.  queue, head
 * and count: the pictures in output order, at most H264SW_OUTPUT_MAX.
 * current: the picture received last (the caller may take a reference to it
 * until the next receive).
 *
 * data, size and cursor: the converted packet and how far its NAL units were
 * given to the decoder; data_time its presentation time.  rest is an owned
 * copy of the packet's bytes, made when the packet could not be given to the
 * decoder completely at once (the caller's bytes are valid only until it
 * reads the next packet).  draining: the end of the stream was announced;
 * flushed: the decoder was flushed for it.
 *
 * failed: a lasting errno (the back end stops).  refused: ENOTSUP when the
 * stream turned out to be outside the decoder's range; send answers it once.
 *
 * aspect_num, aspect_den, colour and full_range: what the track says, used
 * where the stream says nothing.
 */
struct h264sw_backend {
	struct media_bitstream bitstream;
	struct h264sw_decoder *decoder;
	struct media_picture_pool *pool;
	uint32_t pool_width;
	uint32_t pool_height;
	struct h264sw_backend_entry queue[H264SW_OUTPUT_MAX];
	unsigned head;
	unsigned count;
	struct media_picture *current;
	const uint8_t *data;
	size_t size;
	size_t cursor;
	int64_t data_time;
	uint8_t *rest;
	size_t rest_capacity;
	int draining;
	int flushed;
	int failed;
	int refused;
	uint32_t aspect_num;
	uint32_t aspect_den;
	unsigned colour;
	int full_range;
};

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
static void h264sw_backend_pump(struct h264sw_backend *backend);
static void h264sw_backend_take(struct h264sw_backend *backend);
static void h264sw_backend_keep(struct h264sw_backend *backend);
static void h264sw_backend_note(struct h264sw_backend *backend, int error);
static int h264sw_backend_copy(struct h264sw_backend *backend, const struct h264sw_frame *frame, struct media_picture **result);
static int h264sw_backend_pool(struct h264sw_backend *backend, uint32_t width, uint32_t height);
static void h264sw_backend_clear(struct h264sw_backend *backend);

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

/*
 * Opens a decoder for a track: the bitstream conversion, the decoder and
 * the parameter sets of the track's private data.  Answers
 * MEDIA_PROBLEM_FORMAT for another codec and MEDIA_PROBLEM_PROFILE when a
 * parameter set is outside the decoder's range.
 */
static int
h264sw_backend_open(
	const struct media_track *track,
	void **state)
{
	struct h264sw_backend *backend;
	const uint8_t *nal;
	size_t nal_size;
	size_t cursor;
	int found;
	int error;

	/* Another codec is a normal back end selection refusal. */
	*state = NULL;
	if (track->codec != MEDIA_CODEC_H264)
		return MEDIA_PROBLEM_FORMAT;

	/* Allocates the back end's state. */
	backend = calloc(1U, sizeof(*backend));
	if (backend == NULL)
		return ENOMEM;

	/* The track's presentation hints, used where the stream says nothing. */
	backend->aspect_num = track->aspect_num;
	backend->aspect_den = track->aspect_den;
	backend->full_range = track->full_range;
	backend->colour = MEDIA_COLOUR_601;
	if (track->width >= 1280U || track->height > 576U)
		backend->colour = MEDIA_COLOUR_709;

	/* The container's colour matrix, when it has one, overrides the size's guess. */
	if (track->colour_present) {
		if (track->colour_matrix == 1U)
			backend->colour = MEDIA_COLOUR_709;
		else if (track->colour_matrix == 9U || track->colour_matrix == 10U)
			backend->colour = MEDIA_COLOUR_2020;
		else
			backend->colour = MEDIA_COLOUR_601;
	}

	/* Prepares the conversion of the container's NAL units to a byte stream. */
	error = media_bitstream_open(&backend->bitstream, track->codec, track->private_data, track->private_size);
	if (error != 0) {
		h264sw_backend_close(backend);
		return error;
	}

	/* Makes the decoder in its start state. */
	error = h264sw_decoder_create(&backend->decoder);
	if (error != 0) {
		backend->decoder = NULL;
		h264sw_backend_close(backend);

		/* A decoder that is not built does not take the track: the next back end may. */
		if (error == ENOSYS)
			return MEDIA_PROBLEM_FORMAT;
		return error;
	}

	/* A container without composition times gives times in decoding order. */
	backend->decoder->decode_order_times = track->decode_order_times;

	/* Gives each parameter set of the private data to the decoder. */
	cursor = 0U;
	while (backend->bitstream.prefix != NULL) {
		found = vcodec_nal_find(backend->bitstream.prefix, backend->bitstream.prefix_size, &cursor, &nal, &nal_size);
		if (found == 0)
			break;

		/* One parameter set. */
		error = h264sw_decoder_nal(backend->decoder, nal, nal_size, 0);
		if (error == ENOTSUP) {
			h264sw_backend_close(backend);
			return MEDIA_PROBLEM_PROFILE;
		}
		if (error == ENOMEM) {
			h264sw_backend_close(backend);
			return ENOMEM;
		}
	}

	/* Succeeded: the first packet decides what the private data did not say. */
	*state = backend;
	return 0;
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

/*
 * Sends a packet (NULL starts the drain): its NAL units are given to the
 * decoder and the frames that come out are copied into pictures.
 */
static int
h264sw_backend_send(
	void *state,
	const struct media_packet *packet)
{
	struct h264sw_backend *backend;
	const unsigned char *data;
	size_t size;
	unsigned worth;
	int error;

	backend = state;

	/* A lasting error stays until a flush. */
	if (backend->failed != 0)
		return backend->failed;

	/* The end of the stream: the decoder is flushed when its turn comes. */
	if (packet == NULL) {
		backend->refused = 0;
		backend->draining = 1;
		h264sw_backend_pump(backend);
		if (backend->failed != 0)
			return backend->failed;

		/* Succeeded: the drain is under way. */
		return 0;
	}

	/* A stream that turned out to be outside the range is reported once. */
	if (backend->refused != 0) {
		error = backend->refused;
		backend->refused = 0;
		return error;
	}

	/* No packet follows the end of the stream. */
	if (backend->draining)
		return EINVAL;

	/* The packet before gives what it holds first. */
	h264sw_backend_pump(backend);
	if (backend->failed != 0)
		return backend->failed;
	if (backend->cursor < backend->size)
		return EAGAIN;
	if (backend->decoder->output.count != 0U)
		return EAGAIN;

	/* The caller receives first when the queue cannot hold another picture's output (every frame of the DPB and the current one). */
	worth = backend->decoder->dpb.size + 1U;
	if (backend->count + worth > H264SW_OUTPUT_MAX)
		return EAGAIN;

	/* A packet with bytes missing cannot be read. */
	if (packet->data == NULL && packet->size != 0U)
		return EINVAL;

	/* Converts the packet's NAL units to a byte stream. */
	error = media_bitstream_convert(&backend->bitstream, packet->data, packet->size, packet->keyframe, &data, &size);
	if (error != 0)
		return error;

	/* The converted packet waits for the decoder, NAL unit by NAL unit. */
	backend->data = data;
	backend->size = size;
	backend->cursor = 0U;
	backend->data_time = packet->pts_us;
	h264sw_backend_pump(backend);
	if (backend->failed != 0)
		return backend->failed;

	/* A stream outside the decoder's range: the decoder layer switches back ends. */
	if (backend->refused != 0) {
		error = backend->refused;
		backend->refused = 0;
		return error;
	}

	/* Succeeded: the packet is taken, perhaps with its end still to be given. */
	return 0;
}

/*
 * Hands out the next picture in output order, after giving the decoder what
 * is left of the packet when the queue is empty.
 */
static int
h264sw_backend_receive(
	void *state,
	int64_t *time_us)
{
	struct h264sw_backend *backend;
	struct h264sw_backend_entry *entry;

	backend = state;

	/* The previous receive's picture stays available until this receive replaces it. */
	if (backend->current != NULL) {
		media_picture_unref(backend->current);
		backend->current = NULL;
	}

	/* The decoder works on while the queue is empty. */
	if (backend->count == 0U)
		h264sw_backend_pump(backend);

	/* The oldest picture of the queue. */
	if (backend->count != 0U) {
		entry = &backend->queue[backend->head];
		backend->current = entry->picture;
		*time_us = entry->time_us;
		entry->picture = NULL;
		backend->head = (backend->head + 1U) % H264SW_OUTPUT_MAX;
		backend->count--;

		/* Succeeded: one picture. */
		return 1;
	}

	/* A lasting error is reported when no picture is left. */
	if (backend->failed != 0)
		return -backend->failed;

	/* Nothing is ready: the back end needs a packet. */
	return 0;
}

/* Hands out the current picture with a reference of the caller's. */
static void *
h264sw_backend_picture(
	void *state)
{
	struct h264sw_backend *backend;

	backend = state;

	/* No picture was received. */
	if (backend->current == NULL)
		return NULL;

	/* The caller's reference is independent of the next receive. */
	media_picture_ref(backend->current);
	return backend->current;
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

/* Drops the decoding state for a seek: the decoder is reset and the pictures waiting are released. */
static void
h264sw_backend_flush(
	void *state)
{
	struct h264sw_backend *backend;

	backend = state;

	/* The pictures waiting and the one received last go. */
	h264sw_backend_clear(backend);
	if (backend->current != NULL) {
		media_picture_unref(backend->current);
		backend->current = NULL;
	}

	/* The decoder waits for a new start; the parameter sets stay. */
	h264sw_decoder_reset(backend->decoder);

	/* What was left of a packet, the end of the stream and an error are forgotten. */
	backend->data = NULL;
	backend->size = 0U;
	backend->cursor = 0U;
	backend->draining = 0;
	backend->flushed = 0;
	backend->failed = 0;
	backend->refused = 0;
}

/* Frees everything; pictures handed out stay valid, as their pool lives until the last is released. */
static void
h264sw_backend_close(
	void *state)
{
	struct h264sw_backend *backend;

	backend = state;

	/* A failed open leaves nothing to close. */
	if (backend == NULL)
		return;

	/* The pictures held, then the decoder, the pool and the conversion. */
	h264sw_backend_clear(backend);
	if (backend->current != NULL)
		media_picture_unref(backend->current);
	h264sw_decoder_destroy(backend->decoder);
	media_picture_pool_close(backend->pool);
	media_bitstream_close(&backend->bitstream);
	free(backend->rest);
	free(backend);
}

/*
 * Works the decoder as far as the queue allows: frames are copied into the
 * queue, then the next NAL unit of the packet is given, and at the end of the
 * stream the decoder is flushed.  It stops when the queue is full, when
 * nothing is left to do, or after an error.
 */
static void
h264sw_backend_pump(
	struct h264sw_backend *backend)
{
	const uint8_t *nal;
	size_t nal_size;
	int found;
	int error;

	/* Repeats until there is nothing more to do or the queue is full. */
	for (;;) {
		/* A lasting error stops the work. */
		if (backend->failed != 0)
			return;

		/* The frames the decoder has ready go into the queue. */
		h264sw_backend_take(backend);
		if (backend->failed != 0)
			return;

		/* The queue is full: the rest waits for the caller to receive. */
		if (backend->decoder->output.count != 0U) {
			h264sw_backend_keep(backend);
			return;
		}

		/* A stream outside the range gives up the rest of the packet. */
		if (backend->refused != 0)
			backend->cursor = backend->size;

		/* The next NAL unit of the packet. */
		if (backend->cursor < backend->size) {
			found = vcodec_nal_find(backend->data, backend->size, &backend->cursor, &nal, &nal_size);
			if (found == 0) {
				backend->cursor = backend->size;
				continue;
			}

			/* The decoder takes it with the packet's time. */
			error = h264sw_decoder_nal(backend->decoder, nal, nal_size, backend->data_time);
			h264sw_backend_note(backend, error);
			continue;
		}

		/* The end of the stream, once the packet is given: the decoder outputs what it holds. */
		if (backend->draining && !backend->flushed) {
			backend->flushed = 1;
			error = h264sw_decoder_flush(backend->decoder);
			h264sw_backend_note(backend, error);
			continue;
		}

		/* Nothing is left to do. */
		return;
	}
}

/*
 * Records what the decoder answered for a NAL unit or a flush: a stream
 * error was concealed by the decoder and goes unreported, ENOTSUP and ENOMEM
 * are kept, and anything else (EBUSY cannot occur, the queue being empty) is
 * a lasting I/O error.
 */
static void
h264sw_backend_note(
	struct h264sw_backend *backend,
	int error)
{
	/* The decoder took it, or concealed a stream error. */
	if (error == 0)
		return;
	if (error == EINVAL)
		return;

	/* The stream is outside the decoder's range. */
	if (error == ENOTSUP) {
		backend->refused = ENOTSUP;
		return;
	}

	/* An allocation failure is lasting. */
	if (error == ENOMEM) {
		backend->failed = ENOMEM;
		return;
	}

	/* Anything else would loop forever and is a lasting error. */
	backend->failed = EIO;
}

/*
 * Takes the frames the decoder has output, as many as the queue holds, copies
 * each into a picture at once and releases the frame.
 */
static void
h264sw_backend_take(
	struct h264sw_backend *backend)
{
	struct h264sw_frame *frame;
	struct media_picture *picture;
	unsigned place;
	int64_t time_us;
	int error;

	/* Each frame the decoder has ready, while the queue has room. */
	while (backend->count < H264SW_OUTPUT_MAX) {
		frame = h264sw_decoder_output(backend->decoder);
		if (frame == NULL)
			return;

		/* The frame's samples go into a picture of the pool. */
		time_us = frame->time_us;
		picture = NULL;
		error = h264sw_backend_copy(backend, frame, &picture);
		h264sw_frame_unref(frame);

		/* A picture too large for the library's pictures is outside the range. */
		if (error == ENOTSUP) {
			backend->refused = ENOTSUP;
			continue;
		}

		/* A frame that cannot be shown is dropped. */
		if (error == EINVAL)
			continue;

		/* The pool or the memory failed: lasting. */
		if (error != 0) {
			backend->failed = error;
			return;
		}

		/* Appends the picture to the queue in output order. */
		place = (backend->head + backend->count) % H264SW_OUTPUT_MAX;
		backend->queue[place].picture = picture;
		backend->queue[place].time_us = time_us;
		backend->count++;
	}
}

/*
 * Keeps an owned copy of the converted packet when part of it is still to be
 * given to the decoder, because the caller's bytes are valid only until it
 * reads the next packet.
 */
static void
h264sw_backend_keep(
	struct h264sw_backend *backend)
{
	uint8_t *bytes;

	/* Nothing is left of the packet, or the copy already holds it. */
	if (backend->cursor >= backend->size)
		return;
	if (backend->data == backend->rest)
		return;

	/* Makes room for the packet's bytes. */
	if (backend->rest_capacity < backend->size) {
		bytes = realloc(backend->rest, backend->size);
		if (bytes == NULL) {
			backend->failed = ENOMEM;
			return;
		}
		backend->rest = bytes;
		backend->rest_capacity = backend->size;
	}

	/* The packet's bytes, at the same offsets, so the cursor stays right. */
	memcpy(backend->rest, backend->data, backend->size);
	backend->data = backend->rest;
}

/*
 * Copies a frame's cropped samples into a picture of the pool: the luma, and
 * Cb and Cr interleaved into the chroma plane.  Sets the picture's aspect
 * ratio, colour and range.  Returns 0 and *result, ENOTSUP for a size the
 * pictures cannot hold, EINVAL for a frame that cannot be shown, or ENOMEM.
 */
static int
h264sw_backend_copy(
	struct h264sw_backend *backend,
	const struct h264sw_frame *frame,
	struct media_picture **result)
{
	struct media_picture *picture;
	const uint8_t *source;
	const uint8_t *cb;
	const uint8_t *cr;
	uint8_t *target;
	unsigned chroma_width;
	unsigned chroma_height;
	unsigned x;
	unsigned y;
	int error;

	*result = NULL;

	/* A cropping window outside the frame cannot be shown. */
	if (frame->crop_width == 0U || frame->crop_height == 0U)
		return EINVAL;
	if (frame->crop_x + frame->crop_width > frame->width)
		return EINVAL;
	if (frame->crop_y + frame->crop_height > frame->height)
		return EINVAL;

	/* The library's pictures are limited in size. */
	if (frame->crop_width > H264SW_BACKEND_SIDE_MAX || frame->crop_height > H264SW_BACKEND_SIDE_MAX)
		return ENOTSUP;

	/* The chroma window, half of the luma window in each direction. */
	chroma_width = (frame->crop_width + 1U) / 2U;
	chroma_height = (frame->crop_height + 1U) / 2U;
	if (frame->crop_x / 2U + chroma_width > frame->width / 2U)
		return EINVAL;
	if (frame->crop_y / 2U + chroma_height > frame->height / 2U)
		return EINVAL;

	/* A pool of the sequence's cropped size. */
	error = h264sw_backend_pool(backend, frame->crop_width, frame->crop_height);
	if (error != 0)
		return error;

	/* A picture of the pool. */
	picture = media_picture_pool_get(backend->pool);
	if (picture == NULL)
		return ENOMEM;

	/* The luma window, row by row. */
	for (y = 0U; y < frame->crop_height; y++) {
		source = frame->plane[0] + (size_t)(frame->crop_y + y) * frame->stride[0] + frame->crop_x;
		target = picture->luma + (size_t)y * picture->pitch;
		memcpy(target, source, frame->crop_width);
	}

	/* The chroma window, Cb and Cr interleaved in each row. */
	for (y = 0U; y < chroma_height; y++) {
		cb = frame->plane[1] + (size_t)(frame->crop_y / 2U + y) * frame->stride[1] + frame->crop_x / 2U;
		cr = frame->plane[2] + (size_t)(frame->crop_y / 2U + y) * frame->stride[2] + frame->crop_x / 2U;
		target = picture->chroma + (size_t)y * picture->pitch;
		for (x = 0U; x < chroma_width; x++) {
			target[2U * x] = cb[x];
			target[2U * x + 1U] = cr[x];
		}
	}

	/* The aspect ratio: the frame's when it is known, else the track's. */
	picture->aspect_num = 1U;
	picture->aspect_den = 1U;
	if (frame->sar_width != 0U && frame->sar_height != 0U) {
		picture->aspect_num = frame->sar_width;
		picture->aspect_den = frame->sar_height;
	} else if (backend->aspect_num != 0U && backend->aspect_den != 0U) {
		picture->aspect_num = backend->aspect_num;
		picture->aspect_den = backend->aspect_den;
	}

	/* The range and the colour: the track's, then the stream's own. */
	picture->colour = backend->colour;
	picture->full_range = backend->full_range;
	if (frame->signal_present)
		picture->full_range = frame->full_range;
	if (frame->colour_present) {
		if (frame->matrix_coefficients == 1U)
			picture->colour = MEDIA_COLOUR_709;
		else if (frame->matrix_coefficients == 9U || frame->matrix_coefficients == 10U)
			picture->colour = MEDIA_COLOUR_2020;
	}

	/* Succeeded: the picture holds the frame's cropped samples. */
	*result = picture;
	return 0;
}

/*
 * Makes the pool for pictures of a cropped size, unless the current one is
 * of that size.  The old pool is closed; its pictures stay valid until they
 * are released.  Returns 0 or ENOMEM.
 */
static int
h264sw_backend_pool(
	struct h264sw_backend *backend,
	uint32_t width,
	uint32_t height)
{
	struct media_picture_pool *pool;

	/* The pool of the sequence's size is kept. */
	if (backend->pool != NULL && backend->pool_width == width && backend->pool_height == height)
		return 0;

	/* A pool of the new size. */
	pool = media_picture_pool_create(width, height, H264SW_OUTPUT_MAX + H264SW_BACKEND_SPARE);
	if (pool == NULL)
		return ENOMEM;

	/* The old pool no longer gives pictures. */
	media_picture_pool_close(backend->pool);
	backend->pool = pool;
	backend->pool_width = width;
	backend->pool_height = height;

	/* Succeeded: pictures of the new size can be made. */
	return 0;
}

/* Releases every picture waiting in the queue. */
static void
h264sw_backend_clear(
	struct h264sw_backend *backend)
{
	unsigned index;
	unsigned place;

	/* Each picture still waiting, oldest first. */
	for (index = 0U; index < backend->count; index++) {
		place = (backend->head + index) % H264SW_OUTPUT_MAX;
		media_picture_unref(backend->queue[place].picture);
		backend->queue[place].picture = NULL;
	}
	backend->head = 0U;
	backend->count = 0U;
}
