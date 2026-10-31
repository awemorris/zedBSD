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
 * it. The native table contains standard Vulkan Video H.264, the software
 * H.264 decoder and original AAC-LC reconstruction.  A back end that
 * refuses a stream after opening it is replaced by the next one that takes
 * the track (see media_decoder_send).
 */

#include "h264sw-backend.h"
#include "media-private.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most pictures kept from a back end that is replaced, while it is drained. */
#define DECODER_HELD_MAX 64U

/* The most rounds of receiving and sending the end of the stream that draining a back end takes. */
#define DECODER_DRAIN_ROUNDS 16U

/*
 * A picture drained from a back end that was replaced, waiting to be
 * received: the back end whose picture it is, the picture and its time.
 */
struct decoder_held {
	const struct media_decoder_ops *ops;
	void *picture;
	int64_t time_us;
};

/*
 * A track's decoder: the back end that opened it and that back end's
 * state, and what is needed to replace the back end when it refuses the
 * stream after opening.
 *
 * track: a copy of the track, its private data in private_data (owned).
 * index: the back end's place in the table.  accepted: the packets the back
 * end took since it was opened or the last flush.  dropping: 1 while the
 * packets are dropped until a key frame after a switch; dropped counts
 * them, and refusal is the error that caused the switch (reported when the
 * stream ends before a key frame).
 *
 * held and held_count: the pictures drained from replaced back ends, in
 * output order, which come out before the new back end's.  picked and
 * picked_ops: the held picture the last receive handed out (taken by
 * media_decoder_picture or freed by the next receive); picked_last is 1
 * when the last receive was of a held picture.
 */
struct media_decoder {
	const struct media_decoder_ops *ops;
	void *state;
	struct media_track track;
	unsigned char *private_data;
	size_t index;
	uint64_t accepted;
	int dropping;
	uint64_t dropped;
	int refusal;
	struct decoder_held held[DECODER_HELD_MAX];
	unsigned held_count;
	void *picked;
	const struct media_decoder_ops *picked_ops;
	int picked_last;
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
 * The pictures a program keeps at once that it asked the GPU's decoders
 * for (media_decoder_gpu_pictures), or zero for pictures in the CPU's
 * memory.  The program sets it before opening its decoders; each decoder
 * reads it once, when it makes its context.
 */
static unsigned decoder_gpu_hold;

/*
 * The decoding back ends, tried in this order for each track (a back end
 * that cannot work, or does not take the codec, passes it on).
 */
static const struct media_decoder_ops *const decoder_backends[] = {
	&media_vkvideo_ops,
	&media_h264sw_ops,
	&media_aac_ops,
};

static int decoder_try(const struct media_track *track, size_t start, size_t *index, void **state);
static int decoder_refusal(int status);
static int decoder_switch(struct media_decoder *decoder, const struct media_packet *packet, int status, int *resend);
static void decoder_collect(struct media_decoder *decoder);
static void decoder_hold(struct media_decoder *decoder, const struct media_decoder_ops *ops, void *picture, int64_t time_us);
static void decoder_release(struct media_decoder *decoder);

/*
 * Reports built-in decoder availability without loading any optional software codec.
 * Track-specific GPU and profile capabilities are checked by media_decoder_open.
 */
int
media_codec_load(
	void)
{
	/* Succeeded: built-in codecs have no optional software-library prerequisite. */
	return 0;
}

/*
 * Reports no process-wide failure; native admission errors are returned for each track.
 */
const char *
media_codec_reason(
	void)
{
	/* Native capability problems belong to the selected track, not process startup. */
	return "";
}

/*
 * Opens a decoder for a track: the first back end that takes its codec.
 * A back end that cannot open it now (MEDIA_PROBLEM_DEVICE, _PROFILE or
 * _BUSY) passes it to the next one.  Returns 0, the first such problem
 * (or MEDIA_PROBLEM_FORMAT) when no back end opens it, or an allocation
 * errno.
 */
int
media_decoder_open(
	const struct media_track *track,
	struct media_decoder **decoder)
{
	struct media_decoder *opened;
	void *state;
	size_t index;
	int status;

	/* The first back end that opens the track. */
	*decoder = NULL;
	status = decoder_try(track, 0U, &index, &state);
	if (status != 0)
		return status;

	/* The decoder, keeping its back end. */
	opened = calloc(1U, sizeof(*opened));
	if (opened == NULL) {
		decoder_backends[index]->close(state);
		return ENOMEM;
	}

	/* A copy of the track, to open the next back end with when this one refuses the stream. */
	opened->track = *track;
	opened->track.private_data = NULL;
	if (track->private_data != NULL && track->private_size != 0U) {
		opened->private_data = malloc(track->private_size);
		if (opened->private_data == NULL) {
			decoder_backends[index]->close(state);
			free(opened);
			return ENOMEM;
		}

		/* The copy is what the next back end is opened with. */
		memcpy(opened->private_data, track->private_data, track->private_size);
		opened->track.private_data = opened->private_data;
	}

	/* The back end and its state. */
	opened->ops = decoder_backends[index];
	opened->state = state;
	opened->index = index;

	/* Succeeded: the track has a decoder. */
	*decoder = opened;
	return 0;
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
 * EAGAIN when its pictures or sound must be received first, or an error.
 *
 * A back end that refuses the stream (ENOTSUP, ENODEV or EBUSY) is replaced
 * by the next back end of the table that opens the track.  Before the old
 * one took any packet, the packet goes to the new one as it is.  After, the
 * old one is drained (its pictures come out first), and packets are dropped
 * until a key frame, which starts the new one; the number dropped is
 * logged.  A stream that ends before a key frame ends with the error.
 */
int
media_decoder_send(
	struct media_decoder *decoder,
	const struct media_packet *packet)
{
	int status;
	int resend;
	int refused;

	/* After a switch the packets wait for a key frame. */
	if (decoder->dropping) {
		/* The stream ends without one: the refusal stands. */
		if (packet == NULL) {
			media_log("media: %s refused the stream and no key frame followed (%lu packets dropped)", decoder->ops->name, (unsigned long)decoder->dropped);
			decoder->dropping = 0;
			return decoder->refusal;
		}

		/* A packet that cannot start the new back end is dropped. */
		if (!packet->keyframe) {
			decoder->dropped++;
			return 0;
		}

		/* A key frame starts it. */
		media_log("media: %s starts at a key frame after %lu packets dropped", decoder->ops->name, (unsigned long)decoder->dropped);
		decoder->dropping = 0;
	}

	/* The back end's, again with the next back end when one refuses the stream. */
	for (;;) {
		status = decoder->ops->send(decoder->state, packet);
		if (status == 0)
			break;
		if (packet == NULL)
			return status;

		/* Only a refusal of the stream passes it to the next back end. */
		refused = decoder_refusal(status);
		if (!refused)
			return status;

		/* The next back end takes over, or the refusal stands. */
		resend = 0;
		status = decoder_switch(decoder, packet, status, &resend);
		if (status != 0)
			return status;
		if (!resend)
			return 0;
	}

	/* The packet is taken. */
	if (packet != NULL)
		decoder->accepted++;

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
	struct decoder_held *held;
	unsigned index;
	int received;

	/* A drained picture that was not taken is let go. */
	if (decoder->picked != NULL) {
		decoder->picked_ops->picture_free(decoder->picked);
		decoder->picked = NULL;
	}

	/* Nothing is handed out yet. */
	decoder->picked_last = 0;

	/* The pictures drained from a replaced back end come first. */
	if (decoder->held_count != 0U) {
		held = &decoder->held[0];
		decoder->picked = held->picture;
		decoder->picked_ops = held->ops;
		*time_us = held->time_us;
		decoder->picked_last = 1;

		/* Moves the remaining held pictures up one place. */
		for (index = 1U; index < decoder->held_count; index++)
			decoder->held[index - 1U] = decoder->held[index];
		decoder->held_count--;

		/* Succeeded: one picture. */
		return 1;
	}

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
	const struct media_decoder_ops *ops;
	void *picture;

	/* The picture drained from a replaced back end is handed out once. */
	if (decoder->picked_last) {
		picture = decoder->picked;
		ops = decoder->picked_ops;
		decoder->picked = NULL;
		if (picture == NULL)
			return NULL;
	} else {
		/* The back end's picture. */
		ops = decoder->ops;
		picture = ops->picture(decoder->state);
		if (picture == NULL)
			return NULL;
	}

	/* Kept with its back end. */
	frame = malloc(sizeof(*frame));
	if (frame == NULL) {
		ops->picture_free(picture);
		return NULL;
	}

	/* The back end and its picture. */
	frame->ops = ops;
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
	/* The pictures drained from a replaced back end are let go. */
	decoder_release(decoder);

	/* The back end starts again: nothing was accepted, and no key frame is awaited. */
	decoder->accepted = 0U;
	decoder->dropping = 0;
	decoder->dropped = 0U;

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

	/* The pictures drained from a replaced back end, then the back end's state, then the decoder. */
	decoder_release(decoder);
	decoder->ops->close(decoder->state);
	free(decoder->private_data);
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

/*
 * Asks for pictures the GPU holds from the decoders opened from now on.
 */
void
media_decoder_gpu_pictures(
	unsigned hold)
{
	/* The decoders opened later read it. */
	decoder_gpu_hold = hold;
}

/*
 * Reports the pictures held at once a program asked the GPU's decoders for.
 */
unsigned
media_decoder_gpu_hold(
	void)
{
	/* Zero asks for pictures in the CPU's memory. */
	return decoder_gpu_hold;
}

/*
 * Describes a picture the GPU holds.
 */
int
media_frame_gpu(
	const struct media_frame *frame,
	struct media_gpu_picture *gpu)
{
	int error;

	/* A back end without GPU pictures has only pictures in the CPU's memory. */
	if (frame == NULL || frame->ops->picture_gpu == NULL)
		return ENOTSUP;

	/* The back end describes its picture. */
	error = frame->ops->picture_gpu(frame->picture, gpu);
	if (error != 0)
		return error;

	/* Succeeded: gpu describes the picture's image. */
	return 0;
}

/*
 * Opens the first back end of the table, from a place on, that opens the
 * track.  A back end that does not take the codec, or cannot open it now
 * (MEDIA_PROBLEM_DEVICE, _PROFILE or _BUSY), passes it on.  Returns 0 with
 * the back end's place and state, the first such problem (or
 * MEDIA_PROBLEM_FORMAT) when none opens, or an errno.
 */
static int
decoder_try(
	const struct media_track *track,
	size_t start,
	size_t *index,
	void **state)
{
	size_t position;
	int first;
	int status;

	/* Each back end in turn, the first problem kept. */
	first = MEDIA_PROBLEM_FORMAT;
	for (position = start; position < sizeof(decoder_backends) / sizeof(decoder_backends[0]); position++) {
		status = decoder_backends[position]->open(track, state);
		if (status == 0) {
			*index = position;
			return 0;
		}

		/* An allocation failure ends the search. */
		if (status == ENOMEM)
			return ENOMEM;

		/* A back end that does not take the codec says nothing about it. */
		if (status == MEDIA_PROBLEM_FORMAT)
			continue;

		/* A back end that cannot open the track now: the next may. */
		if (status == MEDIA_PROBLEM_DEVICE ||
		    status == MEDIA_PROBLEM_PROFILE ||
		    status == MEDIA_PROBLEM_BUSY) {
			if (first == MEDIA_PROBLEM_FORMAT)
				first = status;
			continue;
		}

		/* Any other error is the track's. */
		return status;
	}

	/* No back end opens the track. */
	return first;
}

/* Reports whether a send's error means the back end refuses the stream and another may take it. */
static int
decoder_refusal(
	int status)
{
	/* The stream is outside the back end's range, or its device is gone or busy. */
	if (status == ENOTSUP)
		return 1;
	if (status == ENODEV)
		return 1;
	if (status == EBUSY)
		return 1;

	/* Any other error is the stream's. */
	return 0;
}

/*
 * Replaces the back end that refused the stream with the next one that
 * opens the track.  Before the old back end took a packet, *resend is set to
 * send the same packet to the new one; after, the old one is drained and
 * the packets are dropped until a key frame (the packet itself is sent
 * when it is one).  Returns 0, the refusal when no back end follows, or an
 * errno.
 */
static int
decoder_switch(
	struct media_decoder *decoder,
	const struct media_packet *packet,
	int status,
	int *resend)
{
	const struct media_decoder_ops *old_ops;
	void *old_state;
	void *state;
	size_t index;
	uint64_t accepted;
	int error;

	/* The next back end that opens the track, the old one still open. */
	error = decoder_try(&decoder->track, decoder->index + 1U, &index, &state);
	if (error != 0) {
		/* An errno is the track's; a problem means no back end follows. */
		if (error == ENOMEM)
			return ENOMEM;
		if (error < MEDIA_PROBLEM_MISSING)
			return error;
		return status;
	}

	/* After packets were accepted, the pictures the old back end holds come out first. */
	accepted = decoder->accepted;
	if (accepted != 0U)
		decoder_collect(decoder);

	/* The old back end goes. */
	old_ops = decoder->ops;
	old_state = decoder->state;
	decoder->ops = decoder_backends[index];
	decoder->state = state;
	decoder->index = index;
	decoder->accepted = 0U;
	old_ops->close(old_state);
	media_log("media: %s refused the stream (%d); switched to %s", old_ops->name, status, decoder->ops->name);

	/* Nothing was output and no reference kept: the same packet goes to the new back end. */
	if (accepted == 0U) {
		*resend = 1;
		return 0;
	}

	/* No reference crosses the switch: a key frame starts the new back end. */
	decoder->refusal = status;
	decoder->dropped = 0U;
	if (packet->keyframe) {
		*resend = 1;
		return 0;
	}

	/* This packet is the first one dropped. */
	decoder->dropping = 1;
	decoder->dropped = 1U;
	*resend = 0;

	/* Succeeded: the new back end waits for a key frame. */
	return 0;
}

/*
 * Drains the pictures out of the back end in use into the held pictures
 * (their time with each), so they come out before the next back end's.
 */
static void
decoder_collect(
	struct media_decoder *decoder)
{
	void *picture;
	int64_t time_us;
	unsigned round;
	int received;
	int status;

	/* The end of the stream is announced; a back end that is full gives its pictures first. */
	status = decoder->ops->send(decoder->state, NULL);
	for (round = 0U; round < DECODER_DRAIN_ROUNDS; round++) {
		/* Each picture the back end has. */
		for (;;) {
			received = decoder->ops->receive(decoder->state, &time_us);
			if (received <= 0)
				break;
			picture = decoder->ops->picture(decoder->state);
			if (picture == NULL)
				continue;
			decoder_hold(decoder, decoder->ops, picture, time_us);
		}

		/* Done, or the back end is broken. */
		if (received < 0)
			return;
		if (status != EAGAIN)
			return;

		/* Announces the end again, now that the pictures were received. */
		status = decoder->ops->send(decoder->state, NULL);
	}
}

/* Keeps a drained picture, or frees it when too many are kept. */
static void
decoder_hold(
	struct media_decoder *decoder,
	const struct media_decoder_ops *ops,
	void *picture,
	int64_t time_us)
{
	/* Too many pictures: the surplus is let go. */
	if (decoder->held_count >= DECODER_HELD_MAX) {
		ops->picture_free(picture);
		return;
	}

	/* Appends the picture in output order. */
	decoder->held[decoder->held_count].ops = ops;
	decoder->held[decoder->held_count].picture = picture;
	decoder->held[decoder->held_count].time_us = time_us;
	decoder->held_count++;
}

/* Frees the drained pictures kept for receiving, and the one handed out last that was not taken. */
static void
decoder_release(
	struct media_decoder *decoder)
{
	unsigned index;

	/* Each picture still waiting. */
	for (index = 0U; index < decoder->held_count; index++)
		decoder->held[index].ops->picture_free(decoder->held[index].picture);
	decoder->held_count = 0U;

	/* The picture received last, if it was not taken. */
	if (decoder->picked != NULL) {
		decoder->picked_ops->picture_free(decoder->picked);
		decoder->picked = NULL;
	}

	/* Nothing is handed out any more. */
	decoder->picked_last = 0;
}
