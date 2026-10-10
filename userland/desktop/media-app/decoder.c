/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Video Player and Music own optional software fallback; native media remains independent of codec libraries. */
#include "private.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One application decoder retains its track so a deferred first-packet profile refusal can open the optional fallback. */
struct app_decoder {
	struct media_decoder *native;
	void *fallback;
	struct media_track track;
	unsigned char *private_data;
	unsigned accepted;
};

/* One application frame retains either a public native CPU picture or an optional software frame independently of decoder close. */
struct app_frame {
	struct media_frame *native;
	void *fallback;
};

/* A presentation scaler can be reused across backend switches but owns only the backend-specific scaler state. */
struct app_scaler {
	struct media_scaler *native;
	void *fallback;
};

static int app_fallback_open(struct app_decoder *decoder, int native_problem);
static int app_fallback_problem(int problem);

/*
 * Reports built-in decoding readiness without loading an optional codec library at application startup.
 */
int
app_codec_load(void)
{
	/* Succeeded: the application's optional fallback is selected only for an unsupported track. */
	return 0;
}

/*
 * Reports the optional fallback loader's most recent refusal text.
 */
const char *
app_codec_reason(void)
{
	const char *reason;

	/* Loader diagnostics are requested only after a native track has been refused. */
	reason = app_avcodec_ops.reason();

	/* Succeeded: diagnostic text remains owned by the application codec module. */
	return reason;
}

/*
 * Opens native media first and uses optional software only for an unsupported codec, profile or device.
 */
int
app_decoder_open(
	const struct media_track *track,
	struct app_decoder **result)
{
	struct app_decoder *decoder;
	int problem;
	int fallback;

	/* Retain codec configuration independently of the media file reader's lifetime. */
	*result = NULL;
	decoder = calloc(1U, sizeof(*decoder));
	if (decoder == NULL)
		return ENOMEM;
	decoder->track = *track;
	if (track->private_size != 0U) {
		decoder->private_data = malloc(track->private_size);
		if (decoder->private_data == NULL) {
			free(decoder);
			return ENOMEM;
		}
		memcpy(decoder->private_data, track->private_data, track->private_size);
		decoder->track.private_data = decoder->private_data;
	}
	problem = media_decoder_open(&decoder->track, &decoder->native);
	if (problem != 0) {
		fallback = app_fallback_problem(problem);
		if (fallback)
			problem = app_fallback_open(decoder, problem);
		if (problem != 0) {
			app_decoder_close(decoder);
			return problem;
		}
	}

	/* Succeeded: one backend owns the track and the application owns its optional-library boundary. */
	*result = decoder;
	app_codec_log("MEDIA backend=%s codec=%s", app_decoder_backend(decoder), app_decoder_name(decoder));
	return 0;
}

/*
 * Reports the codec selected for the track.
 */
const char *
app_decoder_name(
	const struct app_decoder *decoder)
{
	const char *name;

	/* A closed or refused track has no decoder identity. */
	if (decoder == NULL)
		return "none";
	if (decoder->native != NULL)
		name = media_decoder_name(decoder->native);
	else
		name = app_avcodec_ops.decoder_name(decoder->fallback);

	/* Succeeded: the backend owns a stable codec name. */
	return name;
}

/*
 * Reports the execution backend separately from its codec.
 */
const char *
app_decoder_backend(
	const struct app_decoder *decoder)
{
	const char *name;

	/* A refused track has no execution backend. */
	if (decoder == NULL)
		return "none";
	if (decoder->native != NULL) {
		name = media_decoder_backend(decoder->native);
		return name;
	}

	/* Succeeded: optional codec execution belongs to the application. */
	return "libavcodec";
}

/*
 * Sends compressed data, permitting only a first-packet unsupported-profile fallback before native output exists.
 */
int
app_decoder_send(
	struct app_decoder *decoder,
	const struct media_packet *packet)
{
	int error;
	int problem;

	/* A native capability refusal deferred to the first packet is still an admission decision. */
	if (decoder->native != NULL) {
		error = media_decoder_send(decoder->native, packet);
		if (decoder->accepted == 0U && packet != NULL) {
			problem = 0;
			if (error == ENOTSUP)
				problem = MEDIA_PROBLEM_PROFILE;
			else if (error == ENODEV)
				problem = MEDIA_PROBLEM_DEVICE;
			else if (error == EBUSY)
				problem = MEDIA_PROBLEM_BUSY;
			if (problem != 0) {
				problem = app_fallback_open(decoder, problem);
				if (problem == 0) {
					media_decoder_close(decoder->native);
					decoder->native = NULL;
					error = app_avcodec_ops.send(decoder->fallback, packet);
					app_codec_log("MEDIA deferred backend=libavcodec codec=%s", app_decoder_name(decoder));
				}
			}
		}
	} else {
		error = app_avcodec_ops.send(decoder->fallback, packet);
	}
	if (error != 0)
		return error;
	if (packet != NULL)
		decoder->accepted++;

	/* Succeeded: later execution failures are reported, without switching an already-running GPU decode stream. */
	return 0;
}

/*
 * Receives one decoded output or a negative runtime error from the chosen backend.
 */
int
app_decoder_receive(
	struct app_decoder *decoder,
	int64_t *time_us)
{
	int received;

	/* Runtime failure remains distinct from needing another packet. */
	if (decoder->native != NULL)
		received = media_decoder_receive(decoder->native, time_us);
	else
		received = app_avcodec_ops.receive(decoder->fallback, time_us);
	if (received < 0)
		return received;

	/* Succeeded: a positive count publishes output; zero asks the application for more input. */
	return received;
}

/*
 * Retains the current decoded picture in an application-owned backend-neutral wrapper.
 */
struct app_frame *
app_decoder_picture(
	struct app_decoder *decoder)
{
	struct app_frame *frame;

	/* Allocate one wrapper before exporting a backend reference. */
	frame = calloc(1U, sizeof(*frame));
	if (frame == NULL)
		return NULL;
	if (decoder->native != NULL) {
		frame->native = media_decoder_picture(decoder->native);
		if (frame->native == NULL) {
			free(frame);
			return NULL;
		}
	} else {
		frame->fallback = app_avcodec_ops.picture(decoder->fallback);
		if (frame->fallback == NULL) {
			free(frame);
			return NULL;
		}
	}

	/* Succeeded: the picture remains valid after its decoder is closed or seeks. */
	return frame;
}

/*
 * Converts current audio through the selected backend's stereo output contract.
 */
size_t
app_decoder_sound(
	struct app_decoder *decoder,
	int16_t *samples,
	size_t capacity,
	uint32_t rate)
{
	size_t count;

	/* Only the active backend owns audio conversion history. */
	if (decoder->native != NULL)
		count = media_decoder_sound(decoder->native, samples, capacity, rate);
	else
		count = app_avcodec_ops.sound(decoder->fallback, samples, capacity, rate);

	/* Succeeded: count is the number of converted stereo frames in the caller's storage. */
	return count;
}

/*
 * Trims native audio exactly at the desired source-sample seek position.
 */
int
app_decoder_trim(
	struct app_decoder *decoder,
	int64_t before_us)
{
	int error;

	/* Optional software retains the existing whole-frame skipping contract. */
	if (decoder->native == NULL)
		return ENOTSUP;
	error = media_decoder_trim(decoder->native, before_us);
	if (error != 0)
		return error;

	/* Succeeded: output before this source-sample boundary will be excluded. */
	return 0;
}

/*
 * Reports the codec overlap preroll duration required before an exact audio seek.
 */
int64_t
app_decoder_frame_us(
	const struct app_decoder *decoder)
{
	int64_t duration;

	/* Optional backends preserve their previous container seek behavior. */
	if (decoder->native == NULL)
		return 0;
	duration = media_decoder_frame_us(decoder->native);

	/* Succeeded: this duration precedes the trimmed output target. */
	return duration;
}

/*
 * Flushes decode history for seek without changing the already-chosen backend.
 */
void
app_decoder_flush(
	struct app_decoder *decoder)
{
	/* A seek never reopens native admission or enables an execution-time fallback. */
	if (decoder->native != NULL)
		media_decoder_flush(decoder->native);
	else
		app_avcodec_ops.flush(decoder->fallback);
}

/*
 * Closes decoder ownership while keeping independently retained pictures alive.
 */
void
app_decoder_close(
	struct app_decoder *decoder)
{
	/* Partial admission failures may own only track configuration. */
	if (decoder == NULL)
		return;
	if (decoder->native != NULL)
		media_decoder_close(decoder->native);
	if (decoder->fallback != NULL)
		app_avcodec_ops.close(decoder->fallback);
	free(decoder->private_data);
	free(decoder);
}

/*
 * Releases one retained application picture through its owning backend.
 */
void
app_frame_free(
	struct app_frame **frame)
{
	/* A caller can safely clear an already-empty picture slot. */
	if (*frame == NULL)
		return;
	if ((*frame)->native != NULL)
		media_frame_free(&(*frame)->native);
	else
		app_avcodec_ops.picture_free((*frame)->fallback);
	free(*frame);
	*frame = NULL;
}

/*
 * Reports the decoded visible picture size.
 */
void
app_frame_size(
	const struct app_frame *frame,
	int *width,
	int *height)
{
	/* Dimensions come from the decoded picture rather than potentially stale container metadata. */
	if (frame->native != NULL)
		media_frame_size(frame->native, width, height);
	else
		app_avcodec_ops.picture_size(frame->fallback, width, height);
}

/*
 * Reports native sample aspect ratio for application presentation.
 */
void
app_frame_aspect(
	const struct app_frame *frame,
	int *num,
	int *den)
{
	/* The existing optional software path uses square samples when no metadata contract exists. */
	*num = 1;
	*den = 1;
	if (frame->native != NULL)
		media_frame_aspect(frame->native, num, den);
}

/*
 * Draws a retained frame while reusing backend-specific scaler storage between pictures.
 */
int
app_frame_scale(
	const struct app_frame *frame,
	struct app_scaler **scaler,
	uint32_t *pixels,
	size_t stride,
	int width,
	int height)
{
	struct app_scaler *context;
	int error;

	/* A scaler owns no decoder or picture, so closing a session cannot invalidate a retained frame's drawing. */
	context = *scaler;
	if (context == NULL) {
		context = calloc(1U, sizeof(*context));
		if (context == NULL)
			return ENOMEM;
		*scaler = context;
	}
	if (frame->native != NULL)
		error = media_frame_scale(frame->native, &context->native, pixels, stride, width, height);
	else
		error = app_avcodec_ops.picture_scale(frame->fallback, &context->fallback, pixels, stride, width, height);
	if (error != 0)
		return error;

	/* Succeeded: the destination contains the selected picture at the requested dimensions. */
	return 0;
}

/*
 * Frees all scaler storage owned by an application presentation context.
 */
void
app_scaler_free(
	struct app_scaler *scaler)
{
	/* A context may retain one scaler for each backend encountered during its lifetime. */
	if (scaler == NULL)
		return;
	if (scaler->native != NULL)
		media_scaler_free(scaler->native);
	if (scaler->fallback != NULL)
		app_avcodec_ops.scaler_free(scaler->fallback);
	free(scaler);
}

/*
 * Writes codec diagnostics to the application log without calling a private libmedia symbol.
 */
void
app_codec_log(
	const char *format,
	...)
{
	va_list arguments;

	/* Optional-library diagnostics remain application-owned and never select library backends. */
	va_start(arguments, format);
	(void)vfprintf(stderr, format, arguments);
	va_end(arguments);
	(void)fputc('\n', stderr);
}

/* Select the exact class of native refusal eligible for an optional application codec. */
static int
app_fallback_problem(
	int problem)
{
	/* Allocation and malformed-input failures must remain visible rather than being hidden by fallback. */
	if (problem == MEDIA_PROBLEM_FORMAT || problem == MEDIA_PROBLEM_DEVICE)
		return 1;
	if (problem == MEDIA_PROBLEM_PROFILE || problem == MEDIA_PROBLEM_BUSY)
		return 1;

	/* This failure is outside the agreed codec/profile/device fallback policy. */
	return 0;
}

/* Open the optional software implementation only after a native admission refusal. */
static int
app_fallback_open(
	struct app_decoder *decoder,
	int native_problem)
{
	int problem;

	/* Keep a typed native capability failure when optional software is simply absent. */
	problem = app_avcodec_ops.open(&decoder->track, &decoder->fallback);
	if (problem != 0) {
		if (problem == MEDIA_PROBLEM_MISSING && native_problem != MEDIA_PROBLEM_FORMAT)
			return native_problem;
		return problem;
	}

	/* Succeeded: the application owns an optional software decoder for the refused native track. */
	return 0;
}
