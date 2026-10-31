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

/* Forward declarations of the static functions below. */
static int host_video_open(const struct media_track *track, void **state);
static int host_sound_open(const struct media_track *track, void **state);
static int write_picture(FILE *out, const struct media_picture *picture);
static int take_pictures(struct media_decoder *decoder, FILE *out, unsigned *count);

/*
 * The Vulkan Video back end of a host without a device: its open always
 * answers that there is no device, so the decoder layer falls through to the
 * software back end.
 */
const struct media_decoder_ops media_vkvideo_ops = {
	"host-no-device", NULL, NULL, host_video_open, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/*
 * The sound back end the decoder layer links against: it refuses every
 * track, as this tool decodes video only.
 */
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

	/* Forwards the library's message to the error output, one line each. */
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/* Decodes the first H.264 track of a file to a planar 4:2:0 file; exits 1 on failure. */
int
main(
	int argc,
	char **argv)
{
	struct media_file *file;
	const struct media_track *track;
	struct media_decoder *decoder;
	struct media_packet packet;
	const char *backend;
	FILE *out;
	unsigned index;
	unsigned video;
	unsigned count;
	unsigned track_count;
	int error;
	int status;

	/* Refuses a command line without the input and the output. */
	if (argc != 3)
		return 2;

	/* Opens the media file. */
	file = NULL;
	error = media_file_open(argv[1], &file);
	if (error != 0) {
		fprintf(stderr, "host-decode: cannot open %s (%d)\n", argv[1], error);
		return 1;
	}

	/* Finds the first H.264 track; the track count itself means there is none. */
	track_count = media_file_track_count(file);
	video = track_count;
	for (index = 0U; index < track_count; index++) {
		track = media_file_track(file, index);
		if (track->codec == MEDIA_CODEC_H264 && video == track_count)
			video = index;
	}

	/* Refuses a file that has no H.264 track. */
	if (video == track_count) {
		fprintf(stderr, "host-decode: no H.264 track\n");
		media_file_close(file);
		return 1;
	}

	/* Opens a decoder for that track. */
	track = media_file_track(file, video);
	decoder = NULL;
	error = media_decoder_open(track, &decoder);
	if (error != 0) {
		fprintf(stderr, "host-decode: decoder open failed (%d)\n", error);
		media_file_close(file);
		return 1;
	}

	/* Opens the output file. */
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

		/* Skips the packets of other tracks. */
		if (packet.track != video)
			continue;

		/* Sends the packet, taking pictures while the decoder is full. */
		for (;;) {
			status = media_decoder_send(decoder, &packet);
			if (status != EAGAIN)
				break;

			/* The decoder is full: takes its pictures so that it can accept the packet again. */
			error = take_pictures(decoder, out, &count);
			if (error != 0)
				break;
		}

		/* A refused packet ends the run with its status. */
		if (error == 0 && status != 0)
			error = status;

		/* Takes what the packet made ready. */
		if (error == 0)
			error = take_pictures(decoder, out, &count);
	}

	/* The end of the stream drains the decoder. */
	if (error == 0) {
		for (;;) {
			status = media_decoder_send(decoder, NULL);
			if (status != EAGAIN)
				break;

			/* The decoder is full: takes its pictures so that it can accept the packet again. */
			error = take_pictures(decoder, out, &count);
			if (error != 0)
				break;
		}

		/* A refused drain ends the run with its status. */
		if (error == 0)
			error = status;

		/* Takes the pictures the drain released. */
		if (error == 0)
			error = take_pictures(decoder, out, &count);
	}

	/* Reports the back end in use and how many pictures were written. */
	backend = media_decoder_backend(decoder);
	printf("host-decode: backend=%s pictures=%u error=%d\n", backend, count, error);

	/* Releases the decoder, the file and the output. */
	media_decoder_close(decoder);
	media_file_close(file);
	fclose(out);

	/* A failed run exits nonzero. */
	if (error != 0)
		return 1;

	/* Succeeded: every picture was written. */
	return 0;
}

/* The Vulkan Video back end of a host without a device. */
static int
host_video_open(
	const struct media_track *track,
	void **state)
{
	(void)track;

	/* Leaves no state and answers that the device is missing. */
	*state = NULL;
	return MEDIA_PROBLEM_DEVICE;
}

/* The sound back end is not under test: it takes no track. */
static int
host_sound_open(
	const struct media_track *track,
	void **state)
{
	(void)track;

	/* Leaves no state and answers that the format is not taken. */
	*state = NULL;
	return MEDIA_PROBLEM_FORMAT;
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
	size_t offset;
	size_t written;
	int put;
	int plane;

	/* Luma rows. */
	for (y = 0U; y < picture->height; y++) {
		written = fwrite(picture->luma + (size_t)y * picture->pitch, 1U, picture->width, out);
		if (written != picture->width)
			return 1;
	}

	/* U then V, deinterleaved from the chroma plane. */
	chroma_width = (picture->width + 1U) / 2U;
	chroma_height = (picture->height + 1U) / 2U;
	for (plane = 0; plane < 2; plane++) {
		for (y = 0U; y < chroma_height; y++) {
			for (x = 0U; x < chroma_width; x++) {
				offset = (size_t)y * picture->pitch + 2U * x + (unsigned)plane;
				put = fputc(picture->chroma[offset], out);
				if (put == EOF)
					return 1;
			}
		}
	}

	/* Succeeded: every plane was written. */
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
	int error;

	/* Each picture in output order, until the decoder has no more. */
	for (;;) {
		received = media_decoder_receive(decoder, &time_us);
		if (received < 0)
			return -received;
		if (received == 0)
			return 0;

		/* Takes the picture that is ready. */
		frame = media_decoder_picture(decoder);
		if (frame == NULL)
			return ENOMEM;

		/* Writes the picture and gives the frame back. */
		error = write_picture(out, (const struct media_picture *)frame->picture);
		if (error != 0) {
			media_frame_free(&frame);
			return EIO;
		}

		/* Releases the frame and counts the picture written. */
		media_frame_free(&frame);
		(*count)++;
	}
}
