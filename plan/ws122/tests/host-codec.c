/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws122-p004: the host test of the player's decoding add-in
 * (userland/desktop/videoplayer/codec.c and bitstream.c) with the reader
 * (mediafile), against the host's FFmpeg (Debian 13, libavcodec 61) opened
 * by dlopen.  For a file: the video and audio tracks' decoders open, every
 * packet decodes, the pictures come out in increasing time and as many as
 * the packets (less a few a decoder may hold), a picture scales to BGRA
 * with something drawn, the sound converts to 16-bit stereo at 48 kHz for
 * about the file's length, and after a seek to the middle the first
 * picture is at or after the key frame before it.
 *
 *   host-codec FILE          prints "host-codec: PASS ..." or FAIL lines
 */

#include "userland/desktop/libmedia/media-private.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The output rate the sound is converted to. */
#define HOST_RATE	48000U

static int failures;

/* libmedia's log (the library's own function; the test links the sources), to standard error. */
void
media_log(
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	fprintf(stderr, "VIDEOPLAYER ");
	vfprintf(stderr, format, arguments);
	fprintf(stderr, "\n");
	va_end(arguments);
}

static void check(int condition, const char *what);

int
main(
	int argc,
	char **argv)
{
	static int16_t samples[16384 * 2];
	static uint32_t pixels[160 * 90];
	struct media_decoder *video;
	struct media_decoder *sound;
	const struct media_track *track;
	struct media_file *file;
	struct media_packet packet;
	struct media_frame *picture;
	struct media_scaler *scaler;
	unsigned video_track;
	unsigned sound_track;
	unsigned index;
	unsigned pictures;
	unsigned video_packets;
	unsigned drawn;
	unsigned out_of_order;
	uint64_t sound_frames;
	int64_t time_us;
	int64_t last_us;
	int64_t first_after_seek;
	int status;
	int width;
	int height;

	/* The file and its tracks' decoders. */
	if (argc != 2)
		return 2;
	status = media_codec_load();
	check(status == 0, "the add-in loads the host's FFmpeg");
	if (status != 0) {
		printf("reason: %s\n", media_codec_reason());
		return 1;
	}
	status = media_file_open(argv[1], &file);
	check(status == 0, "the file opens");
	if (status != 0)
		return 1;
	video = NULL;
	sound = NULL;
	video_track = 0;
	sound_track = 0;
	for (index = 0; index < media_file_track_count(file); index++) {
		track = media_file_track(file, index);
		if (track->kind == MEDIA_TRACK_VIDEO && video == NULL) {
			status = media_decoder_open(track, &video);
			if (status == 0)
				video_track = index;
		}
		if (track->kind == MEDIA_TRACK_AUDIO && sound == NULL) {
			status = media_decoder_open(track, &sound);
			if (status == 0)
				sound_track = index;
		}
	}
	check(video != NULL, "a video decoder opens");
	if (video == NULL)
		return 1;
	printf("decoders: video=%s audio=%s\n", media_decoder_name(video), media_decoder_name(sound));

	/* Every packet, decoded. */
	pictures = 0;
	video_packets = 0;
	drawn = 0;
	out_of_order = 0;
	sound_frames = 0;
	last_us = -1;
	scaler = NULL;
	for (;;) {
		status = media_file_read(file, &packet);
		if (status != 0)
			break;
		if (packet.track == video_track) {
			video_packets++;
			while (media_decoder_send(video, &packet) == EAGAIN) {
				while (media_decoder_receive(video, &time_us)) {
					pictures++;
				}
			}
			while (media_decoder_receive(video, &time_us)) {
				if (time_us < last_us)
					out_of_order++;
				last_us = time_us;
				pictures++;
				if (pictures == 10U) {
					picture = media_decoder_picture(video);
					media_frame_size(picture, &width, &height);
					printf("picture: %dx%d\n", width, height);
					memset(pixels, 0, sizeof(pixels));
					status = media_frame_scale(picture, &scaler, pixels, 160 * sizeof(uint32_t), 160, 90);
					check(status == 0, "a picture scales to BGRA");
					for (index = 0; index < 160U * 90U; index++) {
						if ((pixels[index] & 0x00ffffffU) != 0U)
							drawn++;
					}
					media_frame_free(&picture);
				}
			}
		} else if (sound != NULL && packet.track == sound_track) {
			(void)media_decoder_send(sound, &packet);
			while (media_decoder_receive(sound, &time_us))
				sound_frames += media_decoder_sound(sound, samples, 16384, HOST_RATE);
		}
	}

	/* The ends drained. */
	(void)media_decoder_send(video, NULL);
	while (media_decoder_receive(video, &time_us)) {
		if (time_us < last_us)
			out_of_order++;
		last_us = time_us;
		pictures++;
	}
	printf("video: packets=%u pictures=%u out_of_order=%u drawn=%u\n", video_packets, pictures, out_of_order, drawn);
	check(pictures + 4U >= video_packets && pictures <= video_packets, "about as many pictures as packets");
	check(out_of_order == 0U, "the pictures come in increasing time");
	check(drawn > 1000U, "the scaled picture has something drawn");
	if (sound != NULL) {
		printf("sound: frames=%llu seconds=%.2f length=%.2f\n", (unsigned long long)sound_frames, (double)sound_frames / HOST_RATE,
		    (double)media_file_duration_us(file) / 1000000.0);
		check((double)sound_frames / HOST_RATE > (double)media_file_duration_us(file) / 1000000.0 * 0.9, "the sound converts for about the length");
	}

	/* A seek to the middle: the first picture after it. */
	status = media_file_seek(file, media_file_duration_us(file) / 2);
	check(status == 0, "the seek works");
	media_decoder_flush(video);
	first_after_seek = -1;
	while (first_after_seek < 0) {
		status = media_file_read(file, &packet);
		if (status != 0)
			break;
		if (packet.track != video_track)
			continue;
		(void)media_decoder_send(video, &packet);
		if (media_decoder_receive(video, &time_us))
			first_after_seek = time_us;
	}
	printf("seek: first picture at %lld us (middle %lld)\n", (long long)first_after_seek, (long long)(media_file_duration_us(file) / 2));
	check(first_after_seek >= 0 && first_after_seek <= media_file_duration_us(file) / 2 + 500000, "a picture comes after the seek, at the key frame before the middle");

	/* The end. */
	media_scaler_free(scaler);
	media_decoder_close(video);
	media_decoder_close(sound);
	media_file_close(file);
	if (failures != 0) {
		printf("host-codec: FAIL %s failures=%d\n", argv[1], failures);
		return 1;
	}
	printf("host-codec: PASS %s\n", argv[1]);
	return 0;
}

/* Reports one check. */
static void
check(
	int condition,
	const char *what)
{
	/* A failure is counted. */
	if (!condition) {
		printf("FAIL %s\n", what);
		failures++;
		return;
	}
	printf("ok %s\n", what);
}
