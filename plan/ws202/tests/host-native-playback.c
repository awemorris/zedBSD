/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Actual container/parser/AAC/app integration with a video execution stand-in; physical pixels remain a separate acceptance. */
#include "userland/desktop/media-app/private.h"
#include "userland/desktop/libmedia/vkvideo-runtime.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A host runtime instance exists from one standard capability admission until its backend closes. */
struct vkvideo_runtime {
	unsigned decodes;
};

/* These variables belong only to the host stand-ins and never alter production implementation behavior. */
static int no_video;
static unsigned fallback_opens;
static unsigned completed_decodes;

static void host_nocts(const char *path);
static void host_seek(struct media_file *file, struct app_decoder *sound, unsigned track, int64_t target);
static int host_fallback_open(const struct media_track *track, void **state);

/* This optional application codec stand-in is intentionally absent; native AAC must never attempt to load it. */
const struct app_decoder_ops app_avcodec_ops = {
	"absent-host-codec", NULL, NULL, host_fallback_open, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

/*
 * Runs an MP4 through application-facing native decoders, then verifies unsupported-device fallback policy.
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
	struct app_decoder *refused;
	struct app_frame *frame;
	struct app_frame *retained;
	struct app_scaler *scaler;
	uint32_t pixels[160U * 90U];
	int16_t samples[4096];
	int64_t time_us;
	int64_t last_time;
	unsigned index;
	unsigned track_count;
	unsigned video_track;
	unsigned sound_track;
	unsigned pictures;
	size_t frames;
	size_t converted;
	int error;
	int read;
	int received;
	int width;
	int height;

	/* The independent fixture owns no external copyrighted media. */
	assert(argc == 3);
	error = media_file_open(argv[1], &file);
	assert(error == 0);
	video = NULL;
	sound = NULL;
	retained = NULL;
	scaler = NULL;
	video_track = 0U;
	sound_track = 0U;
	track_count = media_file_track_count(file);
	for (index = 0U; index < track_count; index++) {
		track = media_file_track(file, index);
		if (track->kind == MEDIA_TRACK_VIDEO) {
			error = app_decoder_open(track, &video);
			assert(error == 0);
			video_track = index;
		} else if (track->kind == MEDIA_TRACK_AUDIO) {
			error = app_decoder_open(track, &sound);
			assert(error == 0);
			sound_track = index;
			assert(track->end_us == 2000000);
		}
	}

	/* Require both native tracks before processing interleaved packets. */
	assert(video != NULL);
	assert(sound != NULL);
	assert(strcmp(app_decoder_backend(video), "vulkan-video") == 0);
	assert(strcmp(app_decoder_backend(sound), "libmedia") == 0);
	pictures = 0U;
	frames = 0U;
	last_time = -1;
	for (;;) {
		read = media_file_read(file, &packet);
		if (read != 0)
			break;
		if (packet.track == video_track) {
			error = app_decoder_send(video, &packet);
			assert(error == 0);
			for (;;) {
				received = app_decoder_receive(video, &time_us);
				assert(received >= 0);
				if (received == 0)
					break;
				assert(time_us >= last_time);
				last_time = time_us;
				frame = app_decoder_picture(video);
				assert(frame != NULL);
				pictures++;
				if (retained == NULL)
					retained = frame;
				else
					app_frame_free(&frame);
			}
		} else if (packet.track == sound_track) {
			error = app_decoder_send(sound, &packet);
			assert(error == 0);
			for (;;) {
				received = app_decoder_receive(sound, &time_us);
				assert(received >= 0);
				if (received == 0)
					break;
				for (;;) {
					converted = app_decoder_sound(sound, samples, 2048U, 48000U);
					frames += converted;
					if (converted == 0U)
						break;
				}
			}
		}
	}

	/* Drain both backends, preserving display PTS and the presentation edit's exact audio end. */
	error = app_decoder_send(video, NULL);
	assert(error == 0);
	for (;;) {
		received = app_decoder_receive(video, &time_us);
		assert(received >= 0);
		if (received == 0)
			break;
		assert(time_us >= last_time);
		last_time = time_us;
		frame = app_decoder_picture(video);
		assert(frame != NULL);
		app_frame_free(&frame);
		pictures++;
	}

	/* Drain AAC lookahead after the complete container reached end of data. */
	error = app_decoder_send(sound, NULL);
	assert(error == 0);
	for (;;) {
		received = app_decoder_receive(sound, &time_us);
		assert(received >= 0);
		if (received == 0)
			break;
		for (;;) {
			converted = app_decoder_sound(sound, samples, 2048U, 48000U);
			frames += converted;
			if (converted == 0U)
				break;
		}
	}

	/* Check complete two-second output and absence of software fallback. */
	assert(pictures == 50U);
	assert(completed_decodes == 50U);
	assert(frames == 96000U);
	assert(fallback_opens == 0U);
	/* Seek uses real AAC overlap preroll and trims exact PCM samples, including the last millisecond. */
	host_seek(file, sound, sound_track, 1250000);
	host_seek(file, sound, sound_track, 0);
	host_seek(file, sound, sound_track, 1999000);
	app_decoder_close(video);
	app_decoder_close(sound);
	app_frame_size(retained, &width, &height);
	assert(width == 320 && height == 180);
	error = app_frame_scale(retained, &scaler, pixels, 160U * sizeof(uint32_t), 160, 90);
	assert(error == 0);
	app_frame_free(&retained);
	app_scaler_free(scaler);

	/* Picture order consumes the container clock sequence when composition timestamps are absent. */
	host_nocts(argv[2]);

	/* An unsupported video device invokes optional software only at the app boundary and remains unavailable when absent. */
	no_video = 1;
	track = media_file_track(file, video_track);
	error = app_decoder_open(track, &refused);
	assert(error == MEDIA_PROBLEM_DEVICE);
	assert(refused == NULL);
	assert(fallback_opens == 1U);
	media_file_close(file);
	printf("Native playback host PASS pictures=%u audio_frames=%lu, retained picture, native-only library, app no-codec refusal\n", pictures, (unsigned long)frames);
	return 0;
}

/* Compare display-order output times against the independently recorded no-ctts packet clock sequence. */
static void
host_nocts(
	const char *path)
{
	struct media_file *file;
	const struct media_track *track;
	struct media_packet packet;
	struct app_decoder *video;
	struct app_frame *frame;
	int64_t clock[128];
	int64_t time_us;
	unsigned clocks;
	unsigned pictures;
	int error;
	int read;
	int received;

	/* This video-only MP4 retains the exact H.264 access units but carries no composition offsets. */
	error = media_file_open(path, &file);
	assert(error == 0);
	track = media_file_track(file, 0U);
	assert(track->decode_order_times == 1);
	error = app_decoder_open(track, &video);
	assert(error == 0);
	clocks = 0U;
	pictures = 0U;
	for (;;) {
		read = media_file_read(file, &packet);
		if (read != 0)
			break;
		assert(clocks < 128U);
		clock[clocks] = packet.pts_us;
		clocks++;
		error = app_decoder_send(video, &packet);
		assert(error == 0);
		for (;;) {
			received = app_decoder_receive(video, &time_us);
			assert(received >= 0);
			if (received == 0)
				break;
			assert(pictures < clocks && time_us == clock[pictures]);
			pictures++;
			frame = app_decoder_picture(video);
			assert(frame != NULL);
			app_frame_free(&frame);
		}
	}

	/* The final reordered pictures consume the remaining clock positions exactly once. */
	assert(read == ENODATA);
	error = app_decoder_send(video, NULL);
	assert(error == 0);
	for (;;) {
		received = app_decoder_receive(video, &time_us);
		assert(received >= 0);
		if (received == 0)
			break;
		assert(pictures < clocks && time_us == clock[pictures]);
		pictures++;
		frame = app_decoder_picture(video);
		assert(frame != NULL);
		app_frame_free(&frame);
	}

	/* A complete bitstream must publish every original access unit despite missing ctts. */
	assert(pictures == 50U && pictures == clocks);
	app_decoder_close(video);
	media_file_close(file);
}

/* Check source-time trimming after flushing decoder overlap and revisiting actual container packets. */
static void
host_seek(
	struct media_file *file,
	struct app_decoder *sound,
	unsigned track,
	int64_t target)
{
	struct media_packet packet;
	int16_t samples[4096];
	int64_t time_us;
	int64_t start;
	size_t frames;
	size_t converted;
	int error;
	int received;
	int read;

	/* The preceding compressed AAC block initializes overlap while trim excludes it from output. */
	start = target - app_decoder_frame_us(sound);
	if (start < 0)
		start = 0;
	error = media_file_seek(file, start);
	assert(error == 0);
	app_decoder_flush(sound);
	error = app_decoder_trim(sound, target);
	assert(error == 0);
	frames = 0U;
	for (;;) {
		read = media_file_read(file, &packet);
		if (read != 0)
			break;
		if (packet.track != track)
			continue;
		error = app_decoder_send(sound, &packet);
		assert(error == 0);
		for (;;) {
			received = app_decoder_receive(sound, &time_us);
			assert(received >= 0);
			if (received == 0)
				break;
			for (;;) {
				converted = app_decoder_sound(sound, samples, 2048U, 48000U);
				frames += converted;
				if (converted == 0U)
					break;
			}
		}
	}

	/* Require a genuine end of data before draining the seeked decoder. */
	assert(read == ENODATA);

	/* The final lookahead is drained through the same app API as the player. */
	error = app_decoder_send(sound, NULL);
	assert(error == 0);
	for (;;) {
		received = app_decoder_receive(sound, &time_us);
		assert(received >= 0);
		if (received == 0)
			break;
		for (;;) {
			converted = app_decoder_sound(sound, samples, 2048U, 48000U);
			frames += converted;
			if (converted == 0U)
				break;
		}
	}

	/* Compare the trimmed output against the exact remaining source sample count. */
	assert(frames == (size_t)((2000000 - target) * 48000 / 1000000));
}

/*
 * Supplies capability admission without implementing or claiming hardware decoding.
 */
int
media_vkvideo_runtime_open(
	const StdVideoH264SequenceParameterSet *sps,
	struct vkvideo_runtime **runtime)
{
	(void)sps;
	*runtime = NULL;
	if (no_video)
		return MEDIA_PROBLEM_DEVICE;
	*runtime = calloc(1U, sizeof(**runtime));
	if (*runtime == NULL)
		return ENOMEM;
	return 0;
}

/*
 * Observes production parameter publication at the native boundary.
 */
int
media_vkvideo_runtime_parameters(
	struct vkvideo_runtime *runtime,
	const struct h264_stream *stream)
{
	(void)runtime;
	assert(stream->parameter_generation != 0U);
	return 0;
}

/*
 * Produces a host-only NV12 stand-in after verifying that native admission selected only real references.
 */
int
media_vkvideo_runtime_decode(
	struct vkvideo_runtime *runtime,
	const struct h264_stream *stream,
	const struct h264_picture *picture,
	const struct h264_dpb_plan *plan,
	struct media_picture *output)
{
	(void)stream;
	assert(picture->slice_count > 0U);
	assert(plan->decode == 1);
	memset(output->luma, 80, (size_t)output->pitch * output->height);
	memset(output->chroma, 128, (size_t)output->pitch * ((output->height + 1U) / 2U));
	runtime->decodes++;
	completed_decodes++;
	return 0;
}

/*
 * Ends the host execution owner's lifetime independently of retained CPU pictures.
 */
void
media_vkvideo_runtime_close(
	struct vkvideo_runtime *runtime)
{
	free(runtime);
}

/*
 * Observes a seek coding reset without changing production reference policy.
 */
void
media_vkvideo_runtime_reset(
	struct vkvideo_runtime *runtime)
{
	(void)runtime;
}

/*
 * Supplies diagnostics without a desktop engine dependency in the host test.
 */
void
media_log(
	const char *format,
	...)
{
	(void)format;
}

/* The application-only fallback stand-in records the attempt and reports an absent optional codec library. */
static int
host_fallback_open(
	const struct media_track *track,
	void **state)
{
	(void)track;
	*state = NULL;
	fallback_opens++;
	return MEDIA_PROBLEM_MISSING;
}
