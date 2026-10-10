/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the application's multi-codec/container regression with actual
 * optional libavcodec decoding and built-in AAC. Native GPU admission is
 * deliberately unavailable on the host; this test proves no hardware pixels.
 * Every receive/send failure is terminal, including after drain and seek.
 */
#include "userland/desktop/media-app/private.h"
#include "userland/desktop/libmedia/media-private.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define HOST_RATE 48000U

/* One file's test owns its reader, decoders and scaler until main's cleanup. */
struct host_playback {
	struct media_file *file;
	struct app_decoder *video;
	struct app_decoder *sound;
	struct app_scaler *scaler;
	unsigned video_track;
	unsigned sound_track;
	unsigned video_packets;
	unsigned pictures;
	unsigned drawn;
	int64_t last_us;
	uint64_t sound_frames;
};

/* The sole runner thread counts failed expectations for this process. */
static unsigned failures;

static int host_video_open(const struct media_track *track, void **result);

/* The native registry's GPU entry refuses host admission; only AAC is executed natively here. */
const struct media_decoder_ops media_vkvideo_ops = {
    "unsupported-host-video", NULL, NULL, host_video_open, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static void check(int condition, const char *what);
static int host_open(struct host_playback *run, const char *path);
static int host_receive(struct host_playback *run, struct app_decoder *decoder);
static int host_send(struct host_playback *run, struct app_decoder *decoder, const struct media_packet *packet);
static int host_packets(struct host_playback *run);
static int host_seek(struct host_playback *run);

/*
 * Provides the native library's diagnostic sink in this source-linked host test.
 */
void
media_log(
	const char *format,
	...)
{
	va_list arguments;

	/* Preserve ordinary production diagnostics without changing codec selection. */
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/*
 * Checks complete playback and middle seek, releasing all owners on every outcome.
 */
int
main(
	int argc,
	char **argv)
{
	struct host_playback run;
	int error;
	int seek_error;
	int64_t duration;

	/* One argument names a container with video and optional audio. */
	if (argc != 2)
		return 2;
	memset(&run, 0, sizeof(run));
	run.last_us = -1;

	/* Test the optional host library and the app's real native-first boundary. */
	error = app_avcodec_ops.load();
	check(error == 0, "the optional application library loads");
	if (error == 0) {
		error = host_open(&run, argv[1]);
		check(error == 0, "the file and all selected decoders open");
		if (error == 0) {
			error = host_packets(&run);
			check(error == 0, "packets and both decoder drains succeed");
			if (error == 0) {
				/* Compare complete output, including AAC's final resampling tail. */
				printf("video: packets=%u pictures=%u drawn=%u\n", run.video_packets, run.pictures, run.drawn);
				check(run.pictures > 0U, "video output is nonempty");
				check(run.pictures + 4U >= run.video_packets && run.pictures <= run.video_packets, "about as many pictures as packets");
				check(run.drawn > 1000U, "the scaled picture contains visible pixels");
				if (run.sound != NULL) {
					duration = media_file_duration_us(run.file);
					printf("sound: frames=%llu seconds=%.2f length=%.2f\n", (unsigned long long)run.sound_frames, (double)run.sound_frames / HOST_RATE, (double)duration / 1000000.0);
					check((double)run.sound_frames / HOST_RATE > (double)duration / 1000000.0 * 0.9, "sound converts for about the file's length");
				}

				/* Reusing the same owners after flush must yield a picture. */
				seek_error = host_seek(&run);
				check(seek_error == 0, "a picture comes after middle seek");
			}
		}
	}

	/* Every acquisition is optional until its successful open. */
	app_scaler_free(run.scaler);
	app_decoder_close(run.video);
	app_decoder_close(run.sound);
	if (run.file != NULL)
		media_file_close(run.file);
	if (failures != 0U) {
		printf("host-codec: FAIL %s failures=%u\n", argv[1], failures);
		return 1;
	}

	/* Succeeded: all generated codec/container cases use this same protocol. */
	printf("host-codec: PASS %s\n", argv[1]);
	return 0;
}

/* Refuses GPU execution without a production switch or a software fake pixel. */
static int
host_video_open(
	const struct media_track *track,
	void **result)
{
	/* No Vulkan driver is substituted into the host's native registry. */
	(void)track;
	*result = NULL;
	return MEDIA_PROBLEM_DEVICE;
}

/* Reports one expectation without obscuring a decoder's error as success. */
static void
check(
	int condition,
	const char *what)
{
	/* Failed expectations remain visible at the final verdict. */
	if (!condition) {
		printf("FAIL %s\n", what);
		failures++;
		return;
	}

	/* Succeeded: name the checked behavior beside its evidence. */
	printf("ok %s\n", what);
}

/* Opens the container and its first video/audio tracks; main cleans partial ownership. */
static int
host_open(
	struct host_playback *run,
	const char *path)
{
	const struct media_track *track;
	unsigned count;
	unsigned index;
	int error;

	/* Reader ownership is retained even when a later decoder rejects the track. */
	error = media_file_open(path, &run->file);
	if (error != 0)
		return error;

	/* Exercise every selected stream's actual app adapter. */
	count = media_file_track_count(run->file);
	for (index = 0U; index < count; index++) {
		track = media_file_track(run->file, index);
		if (track->kind == MEDIA_TRACK_VIDEO && run->video == NULL) {
			error = app_decoder_open(track, &run->video);
			if (error != 0)
				return error;
			run->video_track = index;
		}

		/* Audio may stay native while the selected video uses optional software. */
		if (track->kind == MEDIA_TRACK_AUDIO && run->sound == NULL) {
			error = app_decoder_open(track, &run->sound);
			if (error != 0)
				return error;
			run->sound_track = index;
		}
	}

	/* A zero-picture fixture cannot satisfy a video codec regression. */
	if (run->video == NULL)
		return ENOTSUP;
	printf("decoders: video=%s audio=%s\n", app_decoder_name(run->video), app_decoder_name(run->sound));

	/* Succeeded: the runner owns all selected track state. */
	return 0;
}

/* Receives all available output, distinguishing negative errors from positive output. */
static int
host_receive(
	struct host_playback *run,
	struct app_decoder *decoder)
{
	int16_t samples[16384U * 2U];
	uint32_t pixels[160U * 90U];
	struct app_frame *picture;
	int64_t time_us;
	size_t frames;
	unsigned index;
	int received;
	int error;

	/* Consumption makes room for compressed input and for the final drain. */
	for (;;) {
		received = app_decoder_receive(decoder, &time_us);
		if (received < 0)
			return -received;
		if (received == 0)
			break;
		if (decoder == run->sound) {
			frames = app_decoder_sound(decoder, samples, 16384U, HOST_RATE);
			run->sound_frames += frames;
			continue;
		}

		/* Display timestamps cannot regress, including reordered B pictures. */
		check(time_us >= run->last_us, "pictures come in increasing time");
		run->last_us = time_us;
		run->pictures++;
		if (run->pictures == 10U) {
			picture = app_decoder_picture(decoder);
			if (picture == NULL)
				return ENOMEM;

			/* Sample a real decoded frame, then release it even if conversion fails. */
			memset(pixels, 0, sizeof(pixels));
			error = app_frame_scale(picture, &run->scaler, pixels, 160U * sizeof(uint32_t), 160, 90);
			app_frame_free(&picture);
			if (error != 0)
				return error;

			/* A BGRA image containing only clear pixels is not successful decoding. */
			for (index = 0U; index < 160U * 90U; index++) {
				if ((pixels[index] & 0x00ffffffU) != 0U)
					run->drawn++;
			}
		}
	}

	/* Succeeded: the decoder currently needs input. */
	return 0;
}

/* Retries bounded backpressure only after consuming available output. */
static int
host_send(
	struct host_playback *run,
	struct app_decoder *decoder,
	const struct media_packet *packet)
{
	unsigned retry;
	int error;

	/* A broken send/receive cycle must fail rather than spin until the timeout. */
	for (retry = 0U; retry < 4U; retry++) {
		error = app_decoder_send(decoder, packet);
		if (error == 0) {
			error = host_receive(run, decoder);
			if (error != 0)
				return error;

			/* Succeeded: the packet, including a drain marker, was accepted. */
			return 0;
		}

		/* Only backpressure permits retry; actual decoder failures remain terminal. */
		if (error != EAGAIN)
			return error;
		error = host_receive(run, decoder);
		if (error != 0)
			return error;
	}

	/* Failed: bounded backpressure did not make progress. */
	return EAGAIN;
}

/* Decodes every container packet and drains both selected tracks at clean EOF. */
static int
host_packets(
	struct host_playback *run)
{
	struct media_packet packet;
	int error;

	/* Container errors are not interchangeable with its ENODATA end marker. */
	for (;;) {
		error = media_file_read(run->file, &packet);
		if (error == ENODATA)
			break;
		if (error != 0)
			return error;
		if (packet.track == run->video_track) {
			run->video_packets++;
			error = host_send(run, run->video, &packet);
			if (error != 0)
				return error;
		} else if (run->sound != NULL && packet.track == run->sound_track) {
			error = host_send(run, run->sound, &packet);
			if (error != 0)
				return error;
		}
	}

	/* Video reorder and audio lookahead both have pending output at EOF. */
	error = host_send(run, run->video, NULL);
	if (error != 0)
		return error;
	if (run->sound != NULL) {
		error = host_send(run, run->sound, NULL);
		if (error != 0)
			return error;
	}

	/* Succeeded: both tracks ended without a hidden decoding error. */
	return 0;
}

/* Checks decoder reuse at the key frame preceding the middle of the file. */
static int
host_seek(
	struct host_playback *run)
{
	struct media_packet packet;
	int64_t target;
	int64_t first_us;
	int error;
	int received;
	int drained;

	/* Discard output state before feeding the reader's new key frame. */
	target = media_file_duration_us(run->file) / 2;
	error = media_file_seek(run->file, target);
	if (error != 0)
		return error;
	app_decoder_flush(run->video);
	drained = 0;

	/* Feed until actual output, allowing codec reorder to drain at the new EOF. */
	for (;;) {
		error = media_file_read(run->file, &packet);
		if (error == ENODATA) {
			if (drained)
				return ENODATA;
			drained = 1;
			error = app_decoder_send(run->video, NULL);
		} else {
			if (error != 0)
				return error;
			if (packet.track != run->video_track)
				continue;
			error = app_decoder_send(run->video, &packet);
		}

		/* An unaccepted compressed packet invalidates this seek test. */
		if (error != 0)
			return error;
		received = app_decoder_receive(run->video, &first_us);
		if (received < 0)
			return -received;
		if (received == 0)
			continue;
		printf("seek: first picture=%lld middle=%lld\n", (long long)first_us, (long long)target);
		if (first_us < 0 || first_us > target + 500000)
			return ERANGE;

		/* Succeeded: the decoder produces a timestamp near the selected key frame. */
		return 0;
	}
}
