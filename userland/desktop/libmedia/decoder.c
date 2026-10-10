/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libmedia's decoders (ws177-p031): the boundary between the programs and
 * the decoding back ends.  A track's decoder is opened by the first back
 * end of the table that takes its codec; the decoder, its pictures and
 * their scaler keep the back end that made them, and every call goes to
 * it.  Today the table holds the add-in that opens FFmpeg's libavcodec
 * (avcodec.c); a GPU decoder (WS083's Vulkan Video) goes before it.
 */

#include "media-private.h"

#include <errno.h>
#include <stdlib.h>

/*
 * A track's decoder: the back end that opened it and that back end's
 * state.
 */
struct media_decoder {
	const struct media_decoder_ops *ops;
	void *state;
};

/*
 * A picture taken from a decoder: the back end's own object, freed and
 * scaled by that back end.
 */
struct media_frame {
	const struct media_decoder_ops *ops;
	void *picture;
};

/*
 * A scaler kept between a program's pictures: the back end whose pictures
 * it draws, and that back end's scaler (NULL until the first picture).
 */
struct media_scaler {
	const struct media_decoder_ops *ops;
	void *state;
};

/*
 * The decoding back ends, tried in this order for each track (a back end
 * that cannot work, or does not take the codec, passes it on).
 */
static const struct media_decoder_ops *const decoder_backends[] = {
	&media_vkvideo_ops,
	&media_aac_ops,
};

/*
 * Loads the software decoding add-in once.  Returns 0 when it can decode,
 * MEDIA_PROBLEM_MISSING when libavcodec is not installed, or
 * MEDIA_PROBLEM_VERSION for a version it does not know.
 */
int
media_codec_load(void)
{
	/* Succeeded: built-in codecs have no optional software-library prerequisite. */
	return 0;
}

/*
 * Reports why the add-in could not load ("" when it loaded).
 */
const char *
media_codec_reason(void)
{
	/* Native capability problems belong to the selected track, not process startup. */
	return "";
}

/*
 * Opens a decoder for a track: the first back end that takes its codec.
 * Returns 0, the first back end's problem when none takes it
 * (MEDIA_PROBLEM_MISSING, _VERSION or _FORMAT), or ENOMEM.
 */
int
media_decoder_open(
	const struct media_track *track,
	struct media_decoder **decoder)
{
	struct media_decoder *opened;
	void *state;
	size_t index;
	int first;
	int status;

	/* Each back end in turn, the first one's answer kept. */
	*decoder = NULL;
	first = MEDIA_PROBLEM_FORMAT;
	for (index = 0; index < sizeof(decoder_backends) / sizeof(decoder_backends[0]); index++) {
		/* The back end's decoder; one that does not take the track passes it on. */
		status = decoder_backends[index]->open(track, &state);
		if (status == ENOMEM)
			return ENOMEM;
		if (status == MEDIA_PROBLEM_FORMAT)
			continue;
		if (status != 0)
			return status;

		/* The decoder, keeping its back end. */
		opened = malloc(sizeof(*opened));
		if (opened == NULL) {
			decoder_backends[index]->close(state);
			return ENOMEM;
		}

		/* The back end and its state. */
		opened->ops = decoder_backends[index];
		opened->state = state;

		/* Succeeded: the track has a decoder. */
		*decoder = opened;
		return 0;
	}

	/* No back end takes the track. */
	return first;
}

/*
 * Reports the name of a decoder ("none" without one).
 */
const char *
media_decoder_name(
	const struct media_decoder *decoder)
{
	const char *name;

	/* None. */
	if (decoder == NULL)
		return "none";

	/* The back end's name for it. */
	name = decoder->ops->decoder_name(decoder->state);
	return name;
}

/*
 * Sends a packet to a decoder (NULL to drain it at the end).  Returns 0,
 * EAGAIN when its pictures or sound must be received first, or EINVAL.
 */
int
media_decoder_send(
	struct media_decoder *decoder,
	const struct media_packet *packet)
{
	int status;

	/* The back end's. */
	status = decoder->ops->send(decoder->state, packet);
	if (status != 0)
		return status;

	/* Succeeded: the packet is taken. */
	return 0;
}

/*
 * Receives the next picture or sound with its time.  Returns 1 with one,
 * 0 when the decoder needs another packet or has ended.
 */
int
media_decoder_receive(
	struct media_decoder *decoder,
	int64_t *time_us)
{
	int received;

	/* The back end's. */
	received = decoder->ops->receive(decoder->state, time_us);
	return received;
}

/*
 * Takes the picture received, for the caller to keep; NULL when it cannot
 * be taken.
 */
struct media_frame *
media_decoder_picture(
	struct media_decoder *decoder)
{
	struct media_frame *frame;
	void *picture;

	/* The back end's picture. */
	picture = decoder->ops->picture(decoder->state);
	if (picture == NULL)
		return NULL;

	/* Kept with its back end. */
	frame = malloc(sizeof(*frame));
	if (frame == NULL) {
		decoder->ops->picture_free(picture);
		return NULL;
	}

	/* The back end and its picture. */
	frame->ops = decoder->ops;
	frame->picture = picture;

	/* Succeeded: the picture. */
	return frame;
}

/*
 * Converts the sound received into 16-bit stereo at a rate, up to
 * capacity frames; returns how many were written.
 */
size_t
media_decoder_sound(
	struct media_decoder *decoder,
	int16_t *samples,
	size_t capacity,
	uint32_t rate)
{
	size_t written;

	/* The back end's. */
	written = decoder->ops->sound(decoder->state, samples, capacity, rate);
	return written;
}

/*
 * Empties a decoder (after a seek).
 */
void
media_decoder_flush(
	struct media_decoder *decoder)
{
	/* The back end's. */
	decoder->ops->flush(decoder->state);
}

/*
 * Closes a decoder (NULL is left alone).
 */
void
media_decoder_close(
	struct media_decoder *decoder)
{
	/* Nothing to close. */
	if (decoder == NULL)
		return;

	/* The back end's state, then the decoder. */
	decoder->ops->close(decoder->state);
	free(decoder);
}

/*
 * Frees a picture (NULL is left alone) and empties the caller's pointer.
 */
void
media_frame_free(
	struct media_frame **frame)
{
	/* Nothing to free. */
	if (*frame == NULL)
		return;

	/* The back end's picture, then the frame. */
	(*frame)->ops->picture_free((*frame)->picture);
	free(*frame);
	*frame = NULL;
}

/*
 * Reports a picture's size.
 */
void
media_frame_size(
	const struct media_frame *frame,
	int *width,
	int *height)
{
	/* The back end's. */
	frame->ops->picture_size(frame->picture, width, height);
}

/*
 * Scales a picture into 32-bit pixels (the canvas's 0xAARRGGBB) of a
 * size, through the caller's scaler (made at the first picture, remade
 * when the back end differs).  Returns 0, or EINVAL when it cannot be
 * scaled, or ENOMEM.
 */
int
media_frame_scale(
	const struct media_frame *frame,
	struct media_scaler **scaler,
	uint32_t *pixels,
	size_t stride,
	int width,
	int height)
{
	struct media_scaler *made;
	int status;

	/* A scaler of another back end is let go. */
	if (*scaler != NULL && (*scaler)->ops != frame->ops) {
		media_scaler_free(*scaler);
		*scaler = NULL;
	}

	/* A scaler for this back end's pictures. */
	if (*scaler == NULL) {
		made = calloc(1, sizeof(*made));
		if (made == NULL)
			return ENOMEM;

		/* It draws this back end's pictures. */
		made->ops = frame->ops;
		*scaler = made;
	}

	/* The back end's scaling. */
	status = frame->ops->picture_scale(frame->picture, &(*scaler)->state, pixels, stride, width, height);
	if (status != 0)
		return status;

	/* Succeeded: the pixels hold the picture. */
	return 0;
}

/*
 * Frees a scaler (NULL is left alone).
 */
void
media_scaler_free(
	struct media_scaler *scaler)
{
	/* Nothing to free. */
	if (scaler == NULL)
		return;

	/* The back end's scaler, then ours. */
	if (scaler->state != NULL)
		scaler->ops->scaler_free(scaler->state);
	free(scaler);
}

/*
 * Reports the selected native execution backend separately from the codec.
 */
const char *
media_decoder_backend(
	const struct media_decoder *decoder)
{
	/* A missing decoder has no selected backend. */
	if (decoder == NULL)
		return "none";

	/* Succeeded: the backend identity stays stable for this decoder's lifetime. */
	return decoder->ops->name;
}

/*
 * Trims native audio at the source-sample boundary following a seek.
 */
int
media_decoder_trim(
	struct media_decoder *decoder,
	int64_t before_us)
{
	int error;

	/* Only native audio backends with an exact trimming contract implement this operation. */
	if (decoder->ops->trim == NULL)
		return ENOTSUP;
	error = decoder->ops->trim(decoder->state, before_us);
	if (error != 0)
		return error;

	/* Succeeded: subsequent output will begin at the requested sample boundary. */
	return 0;
}

/*
 * Reports the native codec frame duration needed to reconstruct overlap before a seek target.
 */
int64_t
media_decoder_frame_us(
	const struct media_decoder *decoder)
{
	int64_t duration;

	/* Backends without overlap preroll report no required preceding frame. */
	if (decoder->ops->frame_us == NULL)
		return 0;
	duration = decoder->ops->frame_us(decoder->state);

	/* Succeeded: callers may seek this far before their desired trimmed position. */
	return duration;
}

/*
 * Reports a retained picture's sample aspect ratio for presentation geometry.
 */
void
media_frame_aspect(
	const struct media_frame *frame,
	int *num,
	int *den)
{
	/* Unspecified picture metadata uses square samples. */
	*num = 1;
	*den = 1;
	if (frame->ops->picture_aspect != NULL)
		frame->ops->picture_aspect(frame->picture, num, den);
}
