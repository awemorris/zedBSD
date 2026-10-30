/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Decodes the H.264 track of a media file through libmedia's decoder layer
 * (decoder.c with the software back end; the Vulkan Video back end stands in
 * as a device that is not there, as on a host) and writes the pictures in
 * output order as planar 4:2:0 (Y, then U, then V, cropped) to a file, for
 * comparison with FFmpeg's decoding of the same file.  Prints the back end
 * in use and the number of pictures.
 *
 *   host-decode <input> <output.yuv>
 */

#include "userland/desktop/libmedia/media-private.h"
#include "userland/desktop/libmedia/picture.h"
#include "userland/desktop/mediafile/mediafile.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The Vulkan Video back end of a host without a device. */
static int
host_video_open(
	const struct media_track *track,
	void **state)
{
	(void)track;
	*state = NULL;
	return MEDIA_PROBLEM_DEVICE;
}

const struct media_decoder_ops media_vkvideo_ops = {
	"host-no-device", NULL, NULL, host_video_open, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* The sound back end is not under test: it takes no track. */
static int
host_sound_open(
	const struct media_track *track,
	void **state)
{
	(void)track;
	*state = NULL;
	return MEDIA_PROBLEM_FORMAT;
}

const struct media_decoder_ops media_aac_ops = {
	"host-no-sound", NULL, NULL, host_sound_open, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* The library's log goes to the error output. */
void
media_log(
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/* Writes one picture's cropped planes as Y, U, V; returns 0 or 1 on a write error. */
static int
write_picture(
	FILE *out,
	const struct media_picture *picture)
{
	unsigned x;
	unsigned y;
	unsigned chroma_width;
	unsigned chroma_height;
	int plane;

	/* Luma rows. */
	for (y = 0U; y < picture->height; y++) {
		if (fwrite(picture->luma + (size_t)y * picture->pitch, 1U, picture->width, out) != picture->width)
			return 1;
	}

	/* U then V, deinterleaved from the chroma plane. */
	chroma_width = (picture->width + 1U) / 2U;
	chroma_height = (picture->height + 1U) / 2U;
	for (plane = 0; plane < 2; plane++) {
		for (y = 0U; y < chroma_height; y++) {
			for (x = 0U; x < chroma_width; x++) {
				if (fputc(picture->chroma[(size_t)y * picture->pitch + 2U * x + (unsigned)plane], out) == EOF)
					return 1;
			}
		}
	}
	return 0;
}

/* Takes every picture the decoder has; returns 0, or a nonzero error. */
static int
take_pictures(
	struct media_decoder *decoder,
	FILE *out,
	unsigned *count)
{
	struct media_frame *frame;
	int64_t time_us;
	int received;

	/* Each picture in output order. */
	for (;;) {
		received = media_decoder_receive(decoder, &time_us);
		if (received < 0)
			return -received;
		if (received == 0)
			return 0;
		frame = media_decoder_picture(decoder);
		if (frame == NULL)
			return ENOMEM;
		if (write_picture(out, (const struct media_picture *)frame->picture) != 0) {
			media_frame_free(&frame);
			return EIO;
		}
		media_frame_free(&frame);
		(*count)++;
	}
}

int
main(
	int argc,
	char **argv)
{
	struct media_file *file;
	const struct media_track *track;
	struct media_decoder *decoder;
	struct media_packet packet;
	FILE *out;
	unsigned index;
	unsigned video;
	unsigned count;
	int error;
	int status;

	if (argc != 3)
		return 2;
	file = NULL;
	error = media_file_open(argv[1], &file);
	if (error != 0) {
		fprintf(stderr, "host-decode: cannot open %s (%d)\n", argv[1], error);
		return 1;
	}

	/* The first H.264 track. */
	video = media_file_track_count(file);
	for (index = 0U; index < media_file_track_count(file); index++) {
		track = media_file_track(file, index);
		if (track->codec == MEDIA_CODEC_H264 && video == media_file_track_count(file))
			video = index;
	}
	if (video == media_file_track_count(file)) {
		fprintf(stderr, "host-decode: no H.264 track\n");
		media_file_close(file);
		return 1;
	}
	track = media_file_track(file, video);
	decoder = NULL;
	error = media_decoder_open(track, &decoder);
	if (error != 0) {
		fprintf(stderr, "host-decode: decoder open failed (%d)\n", error);
		media_file_close(file);
		return 1;
	}
	out = fopen(argv[2], "wb");
	if (out == NULL) {
		media_decoder_close(decoder);
		media_file_close(file);
		return 1;
	}

	/* Packets of the track, with the pictures taken whenever the decoder asks. */
	count = 0U;
	error = 0;
	while (error == 0) {
		status = media_file_read(file, &packet);
		if (status != 0)
			break;
		if (packet.track != video)
			continue;
		for (;;) {
			status = media_decoder_send(decoder, &packet);
			if (status != EAGAIN)
				break;
			error = take_pictures(decoder, out, &count);
			if (error != 0)
				break;
		}
		if (error == 0 && status != 0)
			error = status;
		if (error == 0)
			error = take_pictures(decoder, out, &count);
	}

	/* The end of the stream drains the decoder. */
	if (error == 0) {
		for (;;) {
			status = media_decoder_send(decoder, NULL);
			if (status != EAGAIN)
				break;
			error = take_pictures(decoder, out, &count);
			if (error != 0)
				break;
		}
		if (error == 0)
			error = status;
		if (error == 0)
			error = take_pictures(decoder, out, &count);
	}
	printf("host-decode: backend=%s pictures=%u error=%d\n", media_decoder_backend(decoder), count, error);
	media_decoder_close(decoder);
	media_file_close(file);
	fclose(out);
	if (error != 0)
		return 1;
	return 0;
}
