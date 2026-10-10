/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Real application-owned dlopen decoding after a deterministic unsupported native video admission. */
#include "userland/desktop/media-app/private.h"
#include "userland/desktop/libmedia/media-private.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int host_video_open(const struct media_track *track, void **result);
static unsigned host_receive(struct app_decoder *decoder, uint64_t *sound_frames, struct app_frame **retained);

/* The test refuses native GPU admission; video pixels are decoded by the actual optional application library. */
const struct media_decoder_ops media_vkvideo_ops = {
    "unsupported-host-video", NULL, NULL, host_video_open, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

/*
 * Supplies the production native library's ordinary diagnostic sink to this source-linked host test.
 */
void
media_log(
	const char *format,
	...)
{
	va_list arguments;

	/* Keep runtime diagnostics available without changing decoder admission behavior. */
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/*
 * Decodes real H.264 through optional software and real AAC through the native backend, including seek and retained frames.
 */
int
main(
	int argc,
	char **argv)
{
	struct media_file *file;
	const struct media_track *track;
	struct media_packet packet;
	struct app_decoder *video;
	struct app_decoder *sound;
	struct app_frame *retained;
	struct app_scaler *scaler;
	uint32_t pixels[160U * 90U];
	uint64_t sound_frames;
	unsigned video_track;
	unsigned sound_track;
	unsigned count;
	unsigned index;
	unsigned pictures;
	unsigned received;
	int error;
	int comparison;

	/* Both tracks belong to the independently encoded two-second fixture. */
	assert(argc == 2);
	error = media_file_open(argv[1], &file);
	assert(error == 0);
	video = NULL;
	sound = NULL;
	retained = NULL;
	scaler = NULL;
	video_track = 0U;
	sound_track = 0U;
	count = media_file_track_count(file);
	for (index = 0U; index < count; index++) {
		track = media_file_track(file, index);
		if (track->kind == MEDIA_TRACK_VIDEO) {
			error = app_decoder_open(track, &video);
			assert(error == 0);
			video_track = index;
		} else if (track->kind == MEDIA_TRACK_AUDIO) {
			error = app_decoder_open(track, &sound);
			assert(error == 0);
			sound_track = index;
		}
	}

	/* Require the expected ownership boundary before consuming compressed packets. */
	assert(video != NULL);
	assert(sound != NULL);
	comparison = strcmp(app_decoder_backend(video), "libavcodec");
	assert(comparison == 0);
	comparison = strcmp(app_decoder_backend(sound), "libmedia");
	assert(comparison == 0);
	pictures = 0U;
	sound_frames = 0U;
	for (;;) {
		error = media_file_read(file, &packet);
		if (error != 0)
			break;
		if (packet.track == video_track) {
			error = app_decoder_send(video, &packet);
			assert(error == 0);
			received = host_receive(video, &sound_frames, &retained);
			pictures += received;
		} else if (packet.track == sound_track) {
			error = app_decoder_send(sound, &packet);
			assert(error == 0);
			(void)host_receive(sound, &sound_frames, &retained);
		}
	}

	/* EOF must drain both implementations rather than silently losing their final blocks. */
	assert(error == ENODATA);
	error = app_decoder_send(video, NULL);
	assert(error == 0);
	received = host_receive(video, &sound_frames, &retained);
	pictures += received;
	error = app_decoder_send(sound, NULL);
	assert(error == 0);
	(void)host_receive(sound, &sound_frames, &retained);
	assert(pictures == 50U);
	assert(sound_frames == 96000U);
	assert(retained != NULL);

	/* Seek reuses the optional decoder after flushing its old references and delayed pictures. */
	error = media_file_seek(file, 1000000);
	assert(error == 0);
	app_decoder_flush(video);
	received = 0U;
	while (received == 0U) {
		error = media_file_read(file, &packet);
		assert(error == 0);
		if (packet.track != video_track)
			continue;
		error = app_decoder_send(video, &packet);
		assert(error == 0);
		received = host_receive(video, &sound_frames, &retained);
	}

	/* The retained software picture remains scalable after decoder and container owners close. */
	app_decoder_close(video);
	app_decoder_close(sound);
	media_file_close(file);
	error = app_frame_scale(retained, &scaler, pixels, 160U * sizeof(uint32_t), 160, 90);
	assert(error == 0);
	app_frame_free(&retained);
	app_scaler_free(scaler);
	puts("Application fallback PASS (real dlopen H.264, native AAC, 50 pictures, 96000 audio frames, seek, retained software frame)");
	return 0;
}

/* Refuse only H.264 GPU admission while leaving other codec selection to the production backend table. */
static int
host_video_open(
	const struct media_track *track,
	void **result)
{
	/* This controlled unsupported device has no GPU state or physical-decoding claim. */
	*result = NULL;
	if (track->codec == MEDIA_CODEC_H264)
		return MEDIA_PROBLEM_DEVICE;

	/* Other codecs remain eligible for the native audio backend. */
	return MEDIA_PROBLEM_FORMAT;
}

/* Drain ordinary application receive operations, retaining one actual decoded software picture. */
static unsigned
host_receive(
	struct app_decoder *decoder,
	uint64_t *sound_frames,
	struct app_frame **retained)
{
	struct app_frame *frame;
	int16_t samples[4096];
	int64_t time_us;
	size_t converted;
	unsigned pictures;
	int ready;

	/* Receive all currently available output, rejecting negative runtime errors. */
	pictures = 0U;
	for (;;) {
		ready = app_decoder_receive(decoder, &time_us);
		assert(ready >= 0);
		if (ready == 0)
			break;
		frame = app_decoder_picture(decoder);
		if (frame != NULL) {
			pictures++;
			if (*retained == NULL)
				*retained = frame;
			else
				app_frame_free(&frame);
		} else {
			converted = app_decoder_sound(decoder, samples, 2048U, 48000U);
			*sound_frames += converted;
		}
	}

	/* Succeeded: output pictures and sound remain associated with their own backends. */
	return pictures;
}
