/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoding add-in of the player (WS122 p004): FFmpeg's libavcodec,
 * libavutil and libswscale, when the system has them, opened with dlopen
 * and called through dlsym.  The player is built without FFmpeg's headers
 * (the 2026-10-05 user decisions: libavcodec is an add-in a dynamic link
 * may use when it is there; the add-in uses only the layout of the public
 * structures it touches and copies no code).
 *
 * What the add-in relies on, all of FFmpeg's public interface:
 *   - functions found by name with dlsym, their declarations written here
 *     (the arguments are pointers and integers);
 *   - codecs chosen by their names (avcodec_find_decoder_by_name), pixel
 *     and sample formats by their names (av_get_pix_fmt,
 *     av_get_sample_fmt_name), the decoder's settings by their option names
 *     (av_opt_set_int, av_opt_get_int, av_opt_get): no enumeration value
 *     and no field of the decoder's context is written down;
 *   - the first fields of AVPacket (pts, dts, data, size, flags) and of
 *     AVFrame (data, linesize, extended_data, width, height, nb_samples,
 *     format), whose layout is checked for each major version the add-in
 *     knows (plan/tools/media/host-layout.c compiles the check against
 *     each version's headers).  A major version not in add_versions is
 *     refused: playing then says the version is not supported.
 *
 * The decoders get no extradata (that is a field of the context): the
 * configuration travels in the stream (bitstream.c).  Pictures keep their
 * order of presentation by the times of the packets sent (the smallest
 * time waiting goes with the next picture), so no time field of AVFrame is
 * read.  The sound is turned into 16-bit stereo at the stream's rate here.
 */

#include "private.h"
#include "avcodec-layout.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The packet flag of a key frame (AV_PKT_FLAG_KEY), and swscale's bilinear filter (SWS_BILINEAR). */
#define CODEC_PACKET_KEY	0x1
#define CODEC_SCALE_BILINEAR	0x2

/* AVERROR_EOF: the tag 'E', 'O', 'F', ' ' made negative. */
#define CODEC_ERROR_EOF		(-(int)(0x45U | (0x4fU << 8) | (0x46U << 16) | (0x20U << 24)))

/* The zeros FFmpeg wants after extradata (AV_INPUT_BUFFER_PADDING_SIZE). */
#define CODEC_PADDING		64U

/* How many packet times a video decoder may hold before pictures come out. */
#define CODEC_PENDING_MAX	64U

/* The longest text kept of why the add-in could not load. */
#define CODEC_REASON_MAX	160U

/*
 * One set of FFmpeg's libraries the add-in knows: their sonames' major
 * versions, whose structure heads host-layout.c has checked.
 */
struct codec_version {
	unsigned avcodec;
	unsigned avutil;
	unsigned swscale;
};

/*
 * The libraries loaded and the functions found in them, for the program's
 * life once loaded (they are never closed).  Filled once by codec_load
 * under codec_once; error is 0 when the add-in can decode.
 */
struct codec_library {
	void *avcodec;
	void *avutil;
	void *swscale;
	unsigned major;
	int error;
	int bgra;
	char reason[CODEC_REASON_MAX];
	unsigned (*avcodec_version)(void);
	const void *(*avcodec_find_decoder_by_name)(const char *name);
	void *(*avcodec_alloc_context3)(const void *codec);
	int (*avcodec_open2)(void *context, const void *codec, void **options);
	int (*avcodec_send_packet)(void *context, const void *packet);
	int (*avcodec_receive_frame)(void *context, void *frame);
	void (*avcodec_flush_buffers)(void *context);
	void (*avcodec_free_context)(void **context);
	void *(*av_packet_alloc)(void);
	void (*av_packet_free)(void **packet);
	int (*av_new_packet)(void *packet, int size);
	void (*av_packet_unref)(void *packet);
	void *(*av_frame_alloc)(void);
	void (*av_frame_free)(void **frame);
	void (*av_frame_unref)(void *frame);
	void *(*av_frame_clone)(const void *frame);
	int (*av_opt_set_int)(void *object, const char *name, int64_t value, int flags);
	int (*av_opt_set)(void *object, const char *name, const char *value, int flags);
	int (*av_opt_get_int)(void *object, const char *name, int flags, int64_t *value);
	int (*av_opt_get)(void *object, const char *name, int flags, uint8_t **value);
	void (*av_free)(void *pointer);
	int (*av_get_pix_fmt)(const char *name);
	const char *(*av_get_sample_fmt_name)(int format);
	void *(*sws_getCachedContext)(void *context, int source_width, int source_height, int source_format,
	    int width, int height, int format, int flags, void *source_filter, void *filter, const double *parameters);
	int (*sws_scale)(void *context, const uint8_t *const source[], const int source_stride[], int slice_y, int slice_height,
	    uint8_t *const destination[], const int destination_stride[]);
	void (*sws_freeContext)(void *context);
	void *(*avcodec_parameters_alloc)(void);
	void (*avcodec_parameters_free)(void **parameters);
	int (*avcodec_parameters_to_context)(void *context, const void *parameters);
	void *(*av_mallocz)(size_t size);
};

/*
 * One track's decoder: the context, the packet and the frame worked on, the
 * stream's conversion, the kind of track, the times of the packets sent and
 * not yet given to a picture (sorted, smallest first), and for the sound
 * the time of its first sample after an open or a flush, the samples made
 * since, the container's rate and channels, and the resampler's place
 * between the last two samples.
 */
struct addin_decoder {
	void *context;
	void *packet;
	void *frame;
	struct app_bitstream bitstream;
	const char *name;
	unsigned kind;
	int64_t pending[CODEC_PENDING_MAX];
	unsigned pending_count;
	int sound_started;
	int64_t sound_start_us;
	uint64_t sound_samples;
	uint32_t track_rate;
	uint32_t track_channels;
	double resample_place;
	int16_t resample_last[2];
	int resample_primed;
	char own_name[MEDIA_CODEC_NAME_MAX];
};

/*
 * A codec of mediafile and FFmpeg's decoder for it, by name.
 */
struct codec_name {
	unsigned codec;
	const char *decoder;
};

/* The library sets the add-in knows, newest first: FFmpeg 9 (the package's) and FFmpeg 7 (Debian 13's). */
static const struct codec_version add_versions[] = {
	{ 63U, 61U, 10U },
	{ 61U, 59U, 8U }
};

/* FFmpeg's decoders by the codec a track carries (AV1's native decoder is GPU-only; dav1d's is tried first). */
static const struct codec_name codec_names[] = {
	{ MEDIA_CODEC_H264, "h264" },
	{ MEDIA_CODEC_HEVC, "hevc" },
	{ MEDIA_CODEC_AV1, "libdav1d" },
	{ MEDIA_CODEC_AV1, "av1" },
	{ MEDIA_CODEC_VP9, "vp9" },
	{ MEDIA_CODEC_VP8, "vp8" },
	{ MEDIA_CODEC_MPEG4, "mpeg4" },
	{ MEDIA_CODEC_AAC, "aac" },
	{ MEDIA_CODEC_OPUS, "opus" },
	{ MEDIA_CODEC_MP3, "mp3float" },
	{ MEDIA_CODEC_MP3, "mp3" },
	{ MEDIA_CODEC_VORBIS, "vorbis" },
	{ MEDIA_CODEC_THEORA, "theora" },
	{ MEDIA_CODEC_MJPEG, "mjpeg" }
};

/*
 * The libraries and their functions, loaded once for the program's life by
 * codec_load (under codec_once); read-only after that.
 */
static struct codec_library codec;

/* Makes codec_load run once, from whichever thread asks first. */
static pthread_once_t codec_once = PTHREAD_ONCE_INIT;

static void codec_load(void);
static int codec_open_set(const struct codec_version *version);
static void *codec_open_library(const char *name, unsigned major);
static int codec_find(void *library, const char *name, void *pointer);
static int codec_find_all(void);
static void codec_pending_add(struct addin_decoder *decoder, int64_t time_us);
static int codec_channels(struct addin_decoder *decoder);
static uint32_t codec_rate(struct addin_decoder *decoder);
static int codec_sample(const struct codec_frame *frame, const char *format, int channel, int planar, int channels, int index, int16_t *value);
static int addin_extradata(struct addin_decoder *decoder, const void *found, const struct media_track *track);
static int addin_load(void);
static const char *addin_reason(void);
static int addin_open(const struct media_track *track, void **result);
static const char *addin_name(const void *state);
static int addin_send(void *state, const struct media_packet *packet);
static int addin_receive(void *state, int64_t *time_us);
static void *addin_picture(void *state);
static size_t addin_sound(void *state, int16_t *samples, size_t capacity, uint32_t rate);
static void addin_flush(void *state);
static void addin_close(void *state);
static void addin_picture_free(void *picture);
static void addin_picture_size(const void *frame, int *width, int *height);
static int addin_picture_scale(const void *frame, void **scaler, uint32_t *pixels, size_t stride, int width, int height);
static void addin_scaler_free(void *scaler);

/* The add-in as libmedia's decoder.c calls it: the software decoding back end. */
const struct app_decoder_ops app_avcodec_ops = {
	"libavcodec",
	addin_load,
	addin_reason,
	addin_open,
	addin_name,
	addin_send,
	addin_receive,
	addin_picture,
	addin_sound,
	addin_flush,
	addin_close,
	addin_picture_free,
	addin_picture_size,
	addin_picture_scale,
	addin_scaler_free,
	NULL,
	NULL,
	NULL,
};

/*
 * Loads the add-in once.  Returns 0 when it can decode, MEDIA_PROBLEM_MISSING
 * when libavcodec is not installed, MEDIA_PROBLEM_VERSION for a version it does
 * not know (addin_reason says more).
 */
static int
addin_load(void)
{
	/* Once for the program. */
	(void)pthread_once(&codec_once, codec_load);

	/* Reports whether it can decode. */
	if (codec.error != 0)
		return codec.error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports why the add-in could not load ("" when it loaded).
 */
static const char *
addin_reason(void)
{
	/* The text kept by the load. */
	return codec.reason;
}

/*
 * Opens a decoder for a track.  Returns 0, MEDIA_PROBLEM_MISSING or
 * MEDIA_PROBLEM_VERSION (the add-in did not load), MEDIA_PROBLEM_FORMAT (no decoder
 * for the track's codec, or it would not open), or ENOMEM.
 */
static int
addin_open(
	const struct media_track *track,
	void **result)
{
	struct addin_decoder *decoder;
	const void *found;
	unsigned index;
	int needs_extradata;
	int status;

	/* The add-in. */
	*result = NULL;
	status = addin_load();
	if (status != 0)
		return status;

	/* The decoder of the track's codec, by name. */
	found = NULL;
	decoder = calloc(1, sizeof(*decoder));
	if (decoder == NULL)
		return ENOMEM;
	for (index = 0; index < sizeof(codec_names) / sizeof(codec_names[0]); index++) {
		if (codec_names[index].codec != track->codec)
			continue;
		found = codec.avcodec_find_decoder_by_name(codec_names[index].decoder);
		if (found != NULL) {
			decoder->name = codec_names[index].decoder;
			break;
		}
	}

	/* PCM's decoder is named by its samples (the container's name for the codec: pcm_s16le, pcm_u8, ...). */
	if (track->codec == MEDIA_CODEC_PCM) {
		(void)snprintf(decoder->own_name, sizeof(decoder->own_name), "%s", track->codec_name);
		found = codec.avcodec_find_decoder_by_name(decoder->own_name);
		decoder->name = decoder->own_name;
	}

	/* No decoder for the codec. */
	if (found == NULL) {
		free(decoder);
		return MEDIA_PROBLEM_FORMAT;
	}

	/* The stream's conversion (the configuration in the stream instead of extradata). */
	status = app_bitstream_open(&decoder->bitstream, track->codec, track->private_data, track->private_size);
	if (status != 0) {
		free(decoder);
		return MEDIA_PROBLEM_FORMAT;
	}

	/* The context, set by option names: threads chosen by FFmpeg, and the sound's rate and layout when known. */
	decoder->kind = track->kind;
	decoder->track_rate = track->sample_rate;
	decoder->track_channels = track->channels;
	decoder->context = codec.avcodec_alloc_context3(found);
	if (decoder->context == NULL) {
		addin_close(decoder);
		return ENOMEM;
	}

	/* Vorbis and Theora read their three headers from the extradata, which the stream does not repeat. */
	needs_extradata = 0;
	if (track->codec == MEDIA_CODEC_VORBIS || track->codec == MEDIA_CODEC_THEORA)
		needs_extradata = 1;
	if (needs_extradata && track->private_size != 0) {
		status = addin_extradata(decoder, found, track);
		if (status != 0) {
			addin_close(decoder);
			return MEDIA_PROBLEM_FORMAT;
		}
	}

	/* Its options: the threads FFmpeg chooses, and the sound's rate and layout from the container. */
	(void)codec.av_opt_set_int(decoder->context, "threads", 0, 0);
	if (track->kind == MEDIA_TRACK_AUDIO && track->sample_rate != 0U)
		(void)codec.av_opt_set_int(decoder->context, "ar", (int64_t)track->sample_rate, 0);
	if (track->kind == MEDIA_TRACK_AUDIO && track->channels == 1U)
		(void)codec.av_opt_set(decoder->context, "ch_layout", "mono", 0);
	if (track->kind == MEDIA_TRACK_AUDIO && track->channels == 2U)
		(void)codec.av_opt_set(decoder->context, "ch_layout", "stereo", 0);

	/* Opened, with its packet and frame. */
	status = codec.avcodec_open2(decoder->context, found, NULL);
	if (status < 0) {
		addin_close(decoder);
		return MEDIA_PROBLEM_FORMAT;
	}

	/* The packet sent each time. */
	decoder->packet = codec.av_packet_alloc();
	if (decoder->packet == NULL) {
		addin_close(decoder);
		return ENOMEM;
	}

	/* The frame received each time. */
	decoder->frame = codec.av_frame_alloc();
	if (decoder->frame == NULL) {
		addin_close(decoder);
		return ENOMEM;
	}

	/* Succeeded. */
	*result = decoder;
	return 0;
}

/*
 * Reports the name of the decoder FFmpeg gave (the log names it).
 */
static const char *
addin_name(
	const void *state)
{
	const struct addin_decoder *decoder;

	/* The add-in's decoder. */
	decoder = state;

	/* None without a decoder. */
	if (decoder == NULL)
		return "none";

	/* The decoder's name. */
	return decoder->name;
}

/*
 * Sends a packet to the decoder (NULL to drain it at the end).  Returns 0,
 * EAGAIN when its pictures or sound must be received first (the packet is
 * not taken; send it again), or EINVAL for a packet that cannot be decoded.
 */
static int
addin_send(
	void *state,
	const struct media_packet *packet)
{
	struct addin_decoder *decoder;
	struct codec_packet *head;
	const unsigned char *bytes;
	size_t size;
	int status;

	/* The add-in's decoder. */
	decoder = state;

	/* The end: the decoder gives what it holds. */
	if (packet == NULL) {
		status = codec.avcodec_send_packet(decoder->context, NULL);
		if (status == -EAGAIN)
			return EAGAIN;
		return 0;
	}

	/* The bytes as the decoder reads them. */
	status = app_bitstream_convert(&decoder->bitstream, packet->data, packet->size, packet->keyframe, &bytes, &size);
	if (status != 0)
		return EINVAL;
	if (size == 0U || size > (size_t)0x7fffffff)
		return EINVAL;

	/* The packet: its bytes, its time and whether it is a key frame. */
	codec.av_packet_unref(decoder->packet);
	status = codec.av_new_packet(decoder->packet, (int)size);
	if (status < 0)
		return EINVAL;
	head = decoder->packet;
	memcpy(head->data, bytes, size);
	head->pts = packet->pts_us;
	head->dts = packet->dts_us;
	if (packet->keyframe)
		head->flags |= CODEC_PACKET_KEY;

	/* Sent; a full decoder keeps it for later. */
	status = codec.avcodec_send_packet(decoder->context, decoder->packet);
	if (status == -EAGAIN)
		return EAGAIN;
	if (status < 0)
		return EINVAL;

	/* The time waits for its picture; the sound's first time anchors the samples. */
	if (decoder->kind == MEDIA_TRACK_VIDEO)
		codec_pending_add(decoder, packet->pts_us);
	if (decoder->kind == MEDIA_TRACK_AUDIO && !decoder->sound_started) {
		decoder->sound_started = 1;
		decoder->sound_start_us = packet->pts_us;
		decoder->sound_samples = 0;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Receives the next picture or sound into the decoder's frame, with its
 * time.  Returns 1 with one, 0 when the decoder needs another packet or has
 * ended.
 */
static int
addin_receive(
	void *state,
	int64_t *time_us)
{
	struct addin_decoder *decoder;
	const struct codec_frame *frame;
	uint32_t rate;
	int status;

	/* The add-in's decoder. */
	decoder = state;

	/* The next one. */
	codec.av_frame_unref(decoder->frame);
	status = codec.avcodec_receive_frame(decoder->context, decoder->frame);
	if (status < 0)
		return 0;

	/* A picture takes the smallest time waiting. */
	*time_us = 0;
	if (decoder->kind == MEDIA_TRACK_VIDEO) {
		if (decoder->pending_count != 0U) {
			*time_us = decoder->pending[0];
			memmove(decoder->pending, decoder->pending + 1, (decoder->pending_count - 1U) * sizeof(decoder->pending[0]));
			decoder->pending_count--;
		}

		/* Succeeded: the picture. */
		return 1;
	}

	/* The sound's time is its first sample's plus the samples before it. */
	frame = decoder->frame;
	rate = codec_rate(decoder);
	*time_us = decoder->sound_start_us;
	if (rate != 0U)
		*time_us += (int64_t)(decoder->sound_samples * 1000000U / rate);
	if (frame->nb_samples > 0)
		decoder->sound_samples += (uint64_t)frame->nb_samples;

	/* Succeeded: the sound. */
	return 1;
}

/*
 * Takes a reference of the picture received, for the window; NULL when it
 * cannot be made.
 */
static void *
addin_picture(
	void *state)
{
	struct addin_decoder *decoder;
	void *copy;

	/* The add-in's decoder. */
	decoder = state;

	/* A new reference to the same picture. */
	copy = codec.av_frame_clone(decoder->frame);
	return copy;
}

/*
 * Converts the sound received into 16-bit stereo at a rate (linear
 * resampling, the first two channels), up to capacity frames.  Returns how
 * many frames were written (0 for a format the add-in does not convert).
 */
static size_t
addin_sound(
	void *state,
	int16_t *samples,
	size_t capacity,
	uint32_t rate)
{
	struct addin_decoder *decoder;
	const struct codec_frame *frame;
	const char *format;
	int16_t current[2];
	double step;
	size_t written;
	uint32_t source_rate;
	int channels;
	int planar;
	int index;
	int status;
	size_t length;

	/* The add-in's decoder. */
	decoder = state;

	/* The frame's format, by its name. */
	frame = decoder->frame;
	format = codec.av_get_sample_fmt_name(frame->format);
	if (format == NULL || frame->nb_samples <= 0 || rate == 0U)
		return 0;
	length = strlen(format);
	planar = 0;
	if (length > 0U && format[length - 1U] == 'p')
		planar = 1;
	channels = codec_channels(decoder);
	source_rate = codec_rate(decoder);
	if (channels <= 0 || source_rate == 0U)
		return 0;

	/* Each source sample: the output samples between the last one and it, interpolated. */
	step = (double)source_rate / (double)rate;
	written = 0;
	for (index = 0; index < frame->nb_samples; index++) {
		status = codec_sample(frame, format, 0, planar, channels, index, &current[0]);
		if (status != 0)
			return 0;
		current[1] = current[0];
		if (channels > 1) {
			status = codec_sample(frame, format, 1, planar, channels, index, &current[1]);
			if (status != 0)
				return 0;
		}

		/* The first sample of a stream starts the line. */
		if (!decoder->resample_primed) {
			decoder->resample_last[0] = current[0];
			decoder->resample_last[1] = current[1];
			decoder->resample_primed = 1;
			decoder->resample_place = 0.0;
		}

		/* The outputs that fall between the last sample and this one. */
		while (decoder->resample_place <= 1.0 && written < capacity) {
			samples[written * 2U] = (int16_t)((double)decoder->resample_last[0] + ((double)current[0] - (double)decoder->resample_last[0]) * decoder->resample_place);
			samples[written * 2U + 1U] = (int16_t)((double)decoder->resample_last[1] + ((double)current[1] - (double)decoder->resample_last[1]) * decoder->resample_place);
			written++;
			decoder->resample_place += step;
		}

		/* This sample becomes the last one. */
		decoder->resample_place -= 1.0;
		decoder->resample_last[0] = current[0];
		decoder->resample_last[1] = current[1];
	}

	/* Succeeded: the frames written. */
	return written;
}

/*
 * Empties the decoder (after a seek): what it held is dropped, the times
 * waiting are forgotten and the sound starts its count again.
 */
static void
addin_flush(
	void *state)
{
	struct addin_decoder *decoder;

	/* The add-in's decoder. */
	decoder = state;

	/* The decoder's own state. */
	codec.avcodec_flush_buffers(decoder->context);

	/* Ours. */
	decoder->pending_count = 0;
	decoder->sound_started = 0;
	decoder->sound_samples = 0;
	decoder->resample_primed = 0;
	decoder->resample_place = 0.0;
}

/*
 * Closes a decoder.
 */
static void
addin_close(
	void *state)
{
	struct addin_decoder *decoder;

	/* The add-in's decoder. */
	decoder = state;

	/* Nothing to close. */
	if (decoder == NULL)
		return;

	/* FFmpeg's parts, then ours. */
	if (decoder->frame != NULL)
		codec.av_frame_free(&decoder->frame);
	if (decoder->packet != NULL)
		codec.av_packet_free(&decoder->packet);
	if (decoder->context != NULL)
		codec.avcodec_free_context(&decoder->context);
	app_bitstream_close(&decoder->bitstream);
	free(decoder);
}

/*
 * Frees a picture's reference (an AVFrame).
 */
static void
addin_picture_free(
	void *picture)
{
	void *reference;

	/* The reference. */
	reference = picture;
	codec.av_frame_free(&reference);
}

/*
 * Reports a picture's size.
 */
static void
addin_picture_size(
	const void *frame,
	int *width,
	int *height)
{
	const struct codec_frame *head;

	/* The fields at the head of the frame. */
	head = (const void *)frame;
	*width = head->width;
	*height = head->height;
}

/*
 * Scales a picture into 32-bit BGRA pixels (the canvas's 0xAARRGGBB) of a
 * size, through a scaler kept between calls (remade when the sizes
 * change).  Returns 0, or EINVAL when it cannot be scaled.
 */
static int
addin_picture_scale(
	const void *frame,
	void **scaler,
	uint32_t *pixels,
	size_t stride,
	int width,
	int height)
{
	const struct codec_frame *head;
	uint8_t *planes[4];
	int strides[4];

	/* The scaler for these sizes and formats. */
	head = (const void *)frame;
	if (codec.bgra < 0 || head->width <= 0 || head->height <= 0)
		return EINVAL;
	*scaler = codec.sws_getCachedContext(*scaler, head->width, head->height, head->format, width, height, codec.bgra,
	    CODEC_SCALE_BILINEAR, NULL, NULL, NULL);
	if (*scaler == NULL)
		return EINVAL;

	/* Straight into the pixels. */
	memset(planes, 0, sizeof(planes));
	memset(strides, 0, sizeof(strides));
	planes[0] = (uint8_t *)pixels;
	strides[0] = (int)stride;
	(void)codec.sws_scale(*scaler, (const uint8_t *const *)head->data, head->linesize, 0, head->height, planes, strides);

	/* Succeeded. */
	return 0;
}

/*
 * Frees a scaler (NULL is left alone).
 */
static void
addin_scaler_free(
	void *scaler)
{
	/* Only one there is, with the library loaded. */
	if (scaler == NULL || codec.sws_freeContext == NULL)
		return;
	codec.sws_freeContext(scaler);
}

/* Loads the first library set that is there and known; keeps why not. */
static void
codec_load(void)
{
	unsigned index;
	int status;

	/* Each known set, newest first; the first whose libavcodec is there decides. */
	codec.error = MEDIA_PROBLEM_MISSING;
	(void)snprintf(codec.reason, sizeof(codec.reason), "libavcodec is not installed");
	for (index = 0; index < sizeof(add_versions) / sizeof(add_versions[0]); index++) {
		status = codec_open_set(&add_versions[index]);
		if (status == ENOENT)
			continue;
		codec.error = status;
		break;
	}

	/* The pixel format the window takes, by name. */
	if (codec.error == 0)
		codec.bgra = codec.av_get_pix_fmt("bgra");
	app_codec_log("CODEC load error=%d major=%u reason=%s", codec.error, codec.major, codec.reason);
}

/*
 * Opens one library set: ENOENT when its libavcodec is not there, 0 when
 * every library and function is there and the version is the one named,
 * MEDIA_PROBLEM_VERSION or MEDIA_PROBLEM_MISSING otherwise.
 */
static int
codec_open_set(
	const struct codec_version *version)
{
	unsigned found;
	int status;

	/* libavcodec of this major version. */
	codec.avcodec = codec_open_library("libavcodec", version->avcodec);
	if (codec.avcodec == NULL)
		return ENOENT;

	/* Its version must be the one its name says (a renamed library is not trusted). */
	status = codec_find(codec.avcodec, "avcodec_version", &codec.avcodec_version);
	if (status != 0)
		return MEDIA_PROBLEM_VERSION;
	found = codec.avcodec_version() >> 16;
	codec.major = found;
	if (found != version->avcodec) {
		(void)snprintf(codec.reason, sizeof(codec.reason), "libavcodec %u is not a version this player knows", found);
		return MEDIA_PROBLEM_VERSION;
	}

	/* libavutil and libswscale of the same release. */
	codec.avutil = codec_open_library("libavutil", version->avutil);
	codec.swscale = codec_open_library("libswscale", version->swscale);
	if (codec.avutil == NULL || codec.swscale == NULL) {
		(void)snprintf(codec.reason, sizeof(codec.reason), "libavutil %u or libswscale %u is not installed", version->avutil, version->swscale);
		return MEDIA_PROBLEM_MISSING;
	}

	/* Every function. */
	status = codec_find_all();
	if (status != 0)
		return MEDIA_PROBLEM_VERSION;

	/* Succeeded: the add-in decodes. */
	codec.reason[0] = '\0';
	return 0;
}

/* Opens a library by its soname (lib<name>.so.<major>); NULL when it is not there. */
static void *
codec_open_library(
	const char *name,
	unsigned major)
{
	char soname[64];
	void *library;

	/* The soname, then the library. */
	(void)snprintf(soname, sizeof(soname), "%s.so.%u", name, major);
	library = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
	return library;
}

/* Finds one function; 0, or ENOENT with the reason kept. */
static int
codec_find(
	void *library,
	const char *name,
	void *pointer)
{
	void *found;

	/* The symbol. */
	found = dlsym(library, name);
	if (found == NULL) {
		(void)snprintf(codec.reason, sizeof(codec.reason), "FFmpeg has no %s", name);
		return ENOENT;
	}

	/* Stored through the member's address (a data pointer copied into a function pointer). */
	memcpy(pointer, &found, sizeof(found));
	return 0;
}

/* Finds every function the add-in calls; 0 or ENOENT. */
static int
codec_find_all(void)
{
	int status;

	/* libavcodec's. */
	status = codec_find(codec.avcodec, "avcodec_find_decoder_by_name", &codec.avcodec_find_decoder_by_name);
	if (status == 0)
		status = codec_find(codec.avcodec, "avcodec_alloc_context3", &codec.avcodec_alloc_context3);
	if (status == 0)
		status = codec_find(codec.avcodec, "avcodec_open2", &codec.avcodec_open2);
	if (status == 0)
		status = codec_find(codec.avcodec, "avcodec_send_packet", &codec.avcodec_send_packet);
	if (status == 0)
		status = codec_find(codec.avcodec, "avcodec_receive_frame", &codec.avcodec_receive_frame);
	if (status == 0)
		status = codec_find(codec.avcodec, "avcodec_flush_buffers", &codec.avcodec_flush_buffers);
	if (status == 0)
		status = codec_find(codec.avcodec, "avcodec_free_context", &codec.avcodec_free_context);
	if (status == 0)
		status = codec_find(codec.avcodec, "av_packet_alloc", &codec.av_packet_alloc);
	if (status == 0)
		status = codec_find(codec.avcodec, "av_packet_free", &codec.av_packet_free);
	if (status == 0)
		status = codec_find(codec.avcodec, "av_new_packet", &codec.av_new_packet);
	if (status == 0)
		status = codec_find(codec.avcodec, "av_packet_unref", &codec.av_packet_unref);

	/* libavutil's. */
	if (status == 0)
		status = codec_find(codec.avutil, "av_frame_alloc", &codec.av_frame_alloc);
	if (status == 0)
		status = codec_find(codec.avutil, "av_frame_free", &codec.av_frame_free);
	if (status == 0)
		status = codec_find(codec.avutil, "av_frame_unref", &codec.av_frame_unref);
	if (status == 0)
		status = codec_find(codec.avutil, "av_frame_clone", &codec.av_frame_clone);
	if (status == 0)
		status = codec_find(codec.avutil, "av_opt_set_int", &codec.av_opt_set_int);
	if (status == 0)
		status = codec_find(codec.avutil, "av_opt_set", &codec.av_opt_set);
	if (status == 0)
		status = codec_find(codec.avutil, "av_opt_get_int", &codec.av_opt_get_int);
	if (status == 0)
		status = codec_find(codec.avutil, "av_opt_get", &codec.av_opt_get);
	if (status == 0)
		status = codec_find(codec.avutil, "av_free", &codec.av_free);
	if (status == 0)
		status = codec_find(codec.avutil, "av_get_pix_fmt", &codec.av_get_pix_fmt);
	if (status == 0)
		status = codec_find(codec.avutil, "av_get_sample_fmt_name", &codec.av_get_sample_fmt_name);

	/* libswscale's. */
	if (status == 0)
		status = codec_find(codec.swscale, "sws_getCachedContext", &codec.sws_getCachedContext);
	if (status == 0)
		status = codec_find(codec.swscale, "sws_scale", &codec.sws_scale);
	if (status == 0)
		status = codec_find(codec.swscale, "sws_freeContext", &codec.sws_freeContext);

	/* The codec parameters that carry a decoder's extradata (ws177-p031: Vorbis, Theora). */
	if (status == 0)
		status = codec_find(codec.avcodec, "avcodec_parameters_alloc", &codec.avcodec_parameters_alloc);
	if (status == 0)
		status = codec_find(codec.avcodec, "avcodec_parameters_free", &codec.avcodec_parameters_free);
	if (status == 0)
		status = codec_find(codec.avcodec, "avcodec_parameters_to_context", &codec.avcodec_parameters_to_context);
	if (status == 0)
		status = codec_find(codec.avutil, "av_mallocz", &codec.av_mallocz);

	/* Reports a function missing. */
	if (status != 0)
		return status;

	/* Succeeded. */
	return 0;
}

/* Keeps a packet's time among those waiting for their pictures, in order (the oldest dropped when full). */
static void
codec_pending_add(
	struct addin_decoder *decoder,
	int64_t time_us)
{
	unsigned place;

	/* A full list drops its smallest (a decoder that held that many lost one). */
	if (decoder->pending_count == CODEC_PENDING_MAX) {
		memmove(decoder->pending, decoder->pending + 1, (CODEC_PENDING_MAX - 1U) * sizeof(decoder->pending[0]));
		decoder->pending_count--;
	}

	/* Its place, after the smaller ones. */
	place = decoder->pending_count;
	while (place > 0U && decoder->pending[place - 1U] > time_us) {
		decoder->pending[place] = decoder->pending[place - 1U];
		place--;
	}

	/* Kept there. */
	decoder->pending[place] = time_us;
	decoder->pending_count++;
}

/*
 * Reports the sound's channels: the decoder's layout by its option's name
 * ("mono", "stereo", "N channels", "5.1" and the like), else the
 * container's.
 */
static int
codec_channels(
	struct addin_decoder *decoder)
{
	static const struct {
		const char *name;
		int channels;
	} layouts[] = {
		{ "mono", 1 }, { "stereo", 2 }, { "2.1", 3 }, { "3.0", 3 }, { "quad", 4 }, { "4.0", 4 },
		{ "5.0", 5 }, { "5.1", 6 }, { "6.0", 6 }, { "6.1", 7 }, { "7.0", 7 }, { "7.1", 8 }
	};
	const char *named;
	uint8_t *text;
	size_t length;
	unsigned index;
	int channels;
	int status;
	int same;

	/* The layout's name. */
	channels = (int)decoder->track_channels;
	text = NULL;
	status = codec.av_opt_get(decoder->context, "ch_layout", 0, &text);
	if (status < 0 || text == NULL)
		return channels;

	/* "N channels", or a name of the list (before a "(side)" and the like). */
	if (text[0] >= '1' && text[0] <= '9') {
		channels = atoi((const char *)text);
		named = strstr((const char *)text, "channels");
		if (named == NULL)
			channels = (int)decoder->track_channels;
	}

	/* A name of the list. */
	for (index = 0; index < sizeof(layouts) / sizeof(layouts[0]); index++) {
		length = strlen(layouts[index].name);
		same = strncmp((const char *)text, layouts[index].name, length);
		if (same == 0 && (text[length] == '\0' || text[length] == '(')) {
			channels = layouts[index].channels;
			break;
		}
	}

	/* The text is FFmpeg's to free. */
	codec.av_free(text);

	/* The channels found. */
	return channels;
}

/* Reports the sound's rate: the decoder's, else the container's. */
static uint32_t
codec_rate(
	struct addin_decoder *decoder)
{
	int64_t rate;
	int status;

	/* The decoder's rate by its option's name. */
	rate = 0;
	status = codec.av_opt_get_int(decoder->context, "ar", 0, &rate);
	if (status < 0 || rate <= 0 || rate > 768000)
		return decoder->track_rate;

	/* The decoder's. */
	return (uint32_t)rate;
}

/* Reads one sample of a channel as 16 bits; 0, or EINVAL for a format not converted. */
static int
codec_sample(
	const struct codec_frame *frame,
	const char *format,
	int channel,
	int planar,
	int channels,
	int index,
	int16_t *value)
{
	const uint8_t *plane;
	double real;
	size_t place;
	int same;

	/* The plane and the place of the sample in it. */
	if (planar) {
		plane = frame->extended_data[channel];
		place = (size_t)index;
	} else {
		plane = frame->extended_data[0];
		place = (size_t)index * (size_t)channels + (size_t)channel;
	}

	/* Each format the add-in converts, by its name's start. */
	same = strncmp(format, "s16", 3);
	if (same == 0) {
		*value = ((const int16_t *)(const void *)plane)[place];
		return 0;
	}

	/* 32-bit integers keep their top 16 bits. */
	same = strncmp(format, "s32", 3);
	if (same == 0) {
		*value = (int16_t)(((const int32_t *)(const void *)plane)[place] >> 16);
		return 0;
	}

	/* Unsigned bytes are centred on 128. */
	same = strncmp(format, "u8", 2);
	if (same == 0) {
		*value = (int16_t)(((int)plane[place] - 128) << 8);
		return 0;
	}

	/* Floats and doubles are -1 to 1. */
	same = strncmp(format, "flt", 3);
	if (same == 0) {
		real = (double)((const float *)(const void *)plane)[place];
	} else {
		same = strncmp(format, "dbl", 3);
		if (same != 0)
			return EINVAL;
		real = ((const double *)(const void *)plane)[place];
	}

	/* A real sample, clipped. */
	if (real > 1.0)
		real = 1.0;
	if (real < -1.0)
		real = -1.0;
	*value = (int16_t)(real * 32767.0);
	return 0;
}

/*
 * Gives a decoder's context the track's private data as its extradata,
 * through codec parameters (their first fields, avcodec-layout.h's, with the
 * decoder's own type and ID from the head of its AVCodec): the context
 * copies them.  Returns 0, ENOMEM, or EINVAL when FFmpeg refuses them.
 */
static int
addin_extradata(
	struct addin_decoder *decoder,
	const void *found,
	const struct media_track *track)
{
	struct codec_parameters *parameters;
	const struct codec_head *head;
	void *allocated;
	uint8_t *extradata;
	int status;

	/* Refuses private data too large for FFmpeg's int. */
	if (track->private_size > (size_t)0x7fffff00)
		return EINVAL;

	/* The parameters. */
	allocated = codec.avcodec_parameters_alloc();
	if (allocated == NULL)
		return ENOMEM;
	parameters = allocated;

	/* The extradata, with FFmpeg's padding of zeros after it. */
	extradata = codec.av_mallocz(track->private_size + CODEC_PADDING);
	if (extradata == NULL) {
		codec.avcodec_parameters_free(&allocated);
		return ENOMEM;
	}

	/* The track's private data in it. */
	memcpy(extradata, track->private_data, track->private_size);

	/* The decoder's type and ID, and the extradata (the parameters own it now). */
	head = found;
	parameters->codec_type = head->type;
	parameters->codec_id = head->id;
	parameters->extradata = extradata;
	parameters->extradata_size = (int)track->private_size;

	/* Copied into the context; the parameters go. */
	status = codec.avcodec_parameters_to_context(decoder->context, parameters);
	codec.avcodec_parameters_free(&allocated);
	if (status < 0)
		return EINVAL;

	/* Succeeded: the context has the extradata. */
	return 0;
}
