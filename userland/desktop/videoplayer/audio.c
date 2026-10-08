/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The player's sound: one playback stream of libkeiland's
 * (kl_audio_stream_*, WS191) of 16-bit stereo at 48 kHz, which the desktop
 * plays through the system's sound service.  The samples go into the
 * stream's ring; the stream's own connection carries the controls.  A
 * stream whose service went goes on as a silent sink, so the player's
 * clock and its writing go on; the controls then answer EPIPE, which the
 * player takes as done (the sink took them).
 */

#include "videoplayer.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <string.h>

/* The stream: its rate, channels, and its ring of half a second, asked for in fiftieths. */
#define AUDIO_RATE		48000U
#define AUDIO_CHANNELS		2U
#define AUDIO_BUFFER		(AUDIO_RATE / 2U)
#define AUDIO_PERIOD		(AUDIO_RATE / 50U)

/*
 * Opens the playback stream (not started).  Returns 0, or an errno value
 * when there is no sound: the player then plays the picture alone.
 */
int
vp_audio_open(
	struct vp_audio *audio)
{
	struct kl_audio_format format;
	int error;

	/* Nothing yet. */
	memset(audio, 0, sizeof(*audio));

	/* The stream, as the player writes it. */
	memset(&format, 0, sizeof(format));
	format.format = KL_AUDIO_FORMAT_S16_LE;
	format.channels = AUDIO_CHANNELS;
	format.rate = AUDIO_RATE;
	format.buffer_frames = AUDIO_BUFFER;
	format.period_frames = AUDIO_PERIOD;
	error = kl_audio_stream_open(&format, &audio->stream);
	if (error != 0) {
		vp_log("AUDIO open error=%d", error);
		return error;
	}

	/* Succeeded: the stream is the player's. */
	audio->rate = AUDIO_RATE;
	audio->channels = AUDIO_CHANNELS;
	audio->capacity = kl_audio_stream_capacity(audio->stream);
	audio->created = 1;
	return 0;
}

/*
 * Ends the stream (no other thread uses it any more).
 */
void
vp_audio_close(
	struct vp_audio *audio)
{
	/* The stream, when there is one. */
	if (audio->stream != NULL)
		kl_audio_stream_close(audio->stream);
	audio->stream = NULL;
	audio->created = 0;
	audio->running = 0;
}

/* Starts the stream: the device plays what is written. */
int
vp_audio_start(
	struct vp_audio *audio)
{
	int error;

	/* Once. */
	if (!audio->created || audio->running)
		return 0;

	/* The control; a lost stream's sink took it as well. */
	error = kl_audio_stream_start(audio->stream);
	if (error != 0 && error != EPIPE)
		return error;

	/* Succeeded: running. */
	audio->running = 1;
	return 0;
}

/* Stops the stream where it is (a pause): what is written stays. */
int
vp_audio_stop(
	struct vp_audio *audio)
{
	int error;

	/* Only a running stream. */
	if (!audio->created || !audio->running)
		return 0;

	/* The control; a lost stream's sink took it as well. */
	error = kl_audio_stream_stop(audio->stream);
	if (error != 0 && error != EPIPE)
		return error;

	/* Succeeded: stopped. */
	audio->running = 0;
	return 0;
}

/* Drops what is written and not played (a seek); a running stream keeps running. */
int
vp_audio_flush(
	struct vp_audio *audio)
{
	int error;

	/* Only a stream. */
	if (!audio->created)
		return 0;

	/* The control; a lost stream's sink took it as well. */
	error = kl_audio_stream_flush(audio->stream);
	if (error != 0 && error != EPIPE)
		return error;

	/* Succeeded: the ring is empty. */
	return 0;
}

/* Reports how many frames the device side has taken from the stream. */
uint64_t
vp_audio_read_position(
	const struct vp_audio *audio)
{
	uint64_t consumed;

	/* None without a stream. */
	if (!audio->created)
		return 0;

	/* The device side's count. */
	consumed = kl_audio_stream_consumed(audio->stream);

	/* Succeeded: the frames consumed. */
	return consumed;
}

/* Reports how many frames the player has written. */
uint64_t
vp_audio_write_position(
	const struct vp_audio *audio)
{
	uint64_t written;

	/* None without a stream. */
	if (!audio->created)
		return 0;

	/* The player's count. */
	written = kl_audio_stream_written(audio->stream);

	/* Succeeded: the frames written. */
	return written;
}

/* Reports the position heard, the players' clock (never behind a flush). */
uint64_t
vp_audio_clock_position(
	const struct vp_audio *audio)
{
	uint64_t position;

	/* None without a stream. */
	if (!audio->created)
		return 0;

	/* The position heard. */
	position = kl_audio_stream_position(audio->stream, NULL);

	/* Succeeded: the frames heard. */
	return position;
}

/*
 * Writes up to a number of frames into the ring, as many as there is room
 * for; reports how many.
 */
size_t
vp_audio_write(
	struct vp_audio *audio,
	const int16_t *samples,
	size_t frames)
{
	size_t written;

	/* Nothing without a stream. */
	if (!audio->created)
		return 0;

	/* As many frames as fit. */
	written = kl_audio_stream_write(audio->stream, samples, frames);

	/* Succeeded: the frames that fit. */
	return written;
}

/*
 * Opens the stream again between files, when its sound service went (the
 * stream is a silent sink) or none was there when it was opened: a service
 * that came back is used.  No other thread uses the stream meanwhile.
 */
void
vp_audio_renew(
	struct vp_audio *audio)
{
	unsigned events;
	int error;

	/* A stream that still plays stays. */
	if (audio->created) {
		events = 0U;
		error = kl_audio_stream_dispatch(audio->stream, &events);
		if (error != EPIPE)
			return;

		/* Lost: closed, to be opened again. */
		vp_log("AUDIO lost: opening again");
		vp_audio_close(audio);
	}

	/* Opened again; without a service the player goes on without sound. */
	(void)vp_audio_open(audio);
}
