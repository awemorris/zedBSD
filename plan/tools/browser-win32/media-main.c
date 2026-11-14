/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A small program over libmedia, for comparing a host's run with a run
 * under Wine: "media-tool FILE" reads a media file with an H.264 track
 * twice and prints, one line each,
 *   media-tool: decoder WIDTHxHEIGHT          the track's picture size
 *   media-tool: picture N WIDTHxHEIGHT HASH   the first MEDIA_TOOL_PICTURES
 *                                             pictures of the decoder layer
 *   media-tool: pictures N                    how many it decoded
 *   media-tool: engine WIDTHxHEIGHT HASH      the engine's first picture
 *   media-tool: backend NAME                  the decoder's back end
 * where HASH is the FNV-1a 64 of the picture scaled to its own size
 * (0xAARRGGBB pixels, each taken as 4 little-endian bytes).  The first
 * read goes through the decoder layer (media_file_open and
 * media_decoder_*), the second through the engine (media_engine_open on
 * its own thread, paused, which shows its first picture at once).  The
 * library's log goes to the error output.  The exit status is 0 when both
 * reads gave their pictures.
 */

#include "userland/desktop/libmedia/media.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* The pictures of the decoder layer hashed and printed. */
#define MEDIA_TOOL_PICTURES	8U

/* How long the engine may take to open and give its first picture, in seconds. */
#define MEDIA_TOOL_SECONDS	30

/* The decoder layer's state while it reads. */
struct media_tool_read {
	struct media_decoder *decoder;
	struct media_scaler *scaler;
	unsigned pictures;
	int failed;
};

static int media_tool_decoder(const char *path);
static void media_tool_take(struct media_tool_read *read);
static int media_tool_engine(const char *path);
static uint64_t media_tool_hash(const uint32_t *pixels, size_t count);
static void media_tool_log(void *context, const char *line);
static void media_tool_pause(void);

int
main(
	int argc,
	char **argv)
{
	int failed;

	/* One argument: the file. */
	if (argc != 2) {
		fprintf(stderr, "usage: media-tool FILE\n");
		return 2;
	}

	/* The module's log goes to the tool. */
	media_set_log(media_tool_log, NULL);

	/* The two reads. */
	failed = media_tool_decoder(argv[1]);
	failed |= media_tool_engine(argv[1]);
	fflush(stdout);

	/* The verdict. */
	if (failed)
		return 1;
	return 0;
}

/*
 * Reads the file's first H.264 track through the decoder layer, printing
 * its size, the first pictures and the count.  Returns 0, or 1 on a
 * failure.
 */
static int
media_tool_decoder(
	const char *path)
{
	struct media_tool_read read;
	struct media_file *file;
	const struct media_track *track;
	struct media_packet packet;
	unsigned video;
	unsigned count;
	unsigned index;
	int status;
	int more;

	/* The file and its first H.264 track. */
	status = media_file_open(path, &file);
	if (status != 0) {
		printf("media-tool: cannot open %s (%d)\n", path, status);
		return 1;
	}

	/* The first H.264 track. */
	count = media_file_track_count(file);
	video = count;
	for (index = 0U; index < count && video == count; index++) {
		track = media_file_track(file, index);
		if (track != NULL && track->codec == MEDIA_CODEC_H264)
			video = index;
	}

	/* The file has no such track. */
	if (video == count) {
		printf("media-tool: no H.264 track\n");
		media_file_close(file);
		return 1;
	}

	/* Its decoder. */
	track = media_file_track(file, video);
	printf("media-tool: decoder %ux%u\n", (unsigned)track->width, (unsigned)track->height);
	read.decoder = NULL;
	read.scaler = NULL;
	read.pictures = 0U;
	read.failed = 0;
	status = media_decoder_open(track, &read.decoder);
	if (status != 0) {
		printf("media-tool: no decoder (%d)\n", status);
		media_file_close(file);
		return 1;
	}

	/* The track's packets, a full decoder giving its pictures first; then the drain. */
	for (;;) {
		more = 0;
		if (!read.failed)
			more = media_file_read(file, &packet) == 0;
		if (!more)
			break;
		if (packet.track != video)
			continue;
		status = media_decoder_send(read.decoder, &packet);
		while (status == EAGAIN && !read.failed) {
			media_tool_take(&read);
			status = media_decoder_send(read.decoder, &packet);
		}

		/* A send that failed ends the read. */
		if (status != 0)
			read.failed = 1;
		media_tool_take(&read);
	}

	/* Drain the decoder: with no packet it gives what it still holds. */
	status = media_decoder_send(read.decoder, NULL);
	while (status == EAGAIN && !read.failed) {
		media_tool_take(&read);
		status = media_decoder_send(read.decoder, NULL);
	}

	/* Take the pictures the drain gave. */
	media_tool_take(&read);

	/* The count and the back end, then the decoder and the file let go. */
	printf("media-tool: pictures %u\n", read.pictures);
	printf("media-tool: backend %s\n", media_decoder_backend(read.decoder));
	media_scaler_free(read.scaler);
	media_decoder_close(read.decoder);
	media_file_close(file);

	/* Succeeded when it decoded pictures and nothing failed. */
	if (read.failed || read.pictures == 0U)
		return 1;
	return 0;
}

/* Takes every picture the decoder has ready, printing the first ones. */
static void
media_tool_take(
	struct media_tool_read *read)
{
	struct media_frame *frame;
	uint32_t *pixels;
	int64_t time_us;
	int width;
	int height;
	int status;
	unsigned long long hash;
	int more;

	/* Each picture (sound is not asked for). */
	for (;;) {
		more = 0;
		if (!read->failed)
			more = media_decoder_receive(read->decoder, &time_us) == 1;
		if (!more)
			break;
		frame = media_decoder_picture(read->decoder);
		if (frame == NULL) {
			read->failed = 1;
			return;
		}

		/* Count the picture. */
		read->pictures++;

		/* One of the first: scaled to its own size and hashed. */
		if (read->pictures <= MEDIA_TOOL_PICTURES) {
			media_frame_size(frame, &width, &height);
			pixels = calloc((size_t)width * (size_t)height, sizeof(*pixels));
			status = ENOMEM;
			if (pixels != NULL)
				status = media_frame_scale(frame, &read->scaler, pixels, (size_t)width * sizeof(*pixels), width, height);
			if (status == 0) {
				hash = media_tool_hash(pixels, (size_t)width * (size_t)height);
				printf("media-tool: picture %u %dx%d %016llx\n", read->pictures, width, height, hash);
			} else {
				read->failed = 1;
			}

			/* The pixels are not needed after the hash. */
			free(pixels);
		}

		/* Give the picture back. */
		media_frame_free(&frame);
	}
}

/*
 * Opens the file in the engine, paused, and prints the size and the hash
 * of the first picture it shows.  Returns 0, or 1 on a failure.
 */
static int
media_tool_engine(
	const char *path)
{
	struct media_engine *engine;
	struct media_status status;
	uint32_t *pixels;
	double next;
	int drawn;
	int waits;
	int error;
	unsigned long long hash;

	/* Opened on the engine's thread, without a wake (the program asks). */
	error = media_engine_open(path, NULL, 0U, NULL, NULL, NULL, &engine);
	if (error != 0) {
		printf("media-tool: engine open failed (%d)\n", error);
		return 1;
	}

	/* Wait while the engine opens. */
	media_engine_status(engine, &status);
	for (waits = 0; status.state == MEDIA_OPENING && waits < MEDIA_TOOL_SECONDS * 100; waits++) {
		media_tool_pause();
		media_engine_status(engine, &status);
	}

	/* The engine must be paused and have a size. */
	if (status.state != MEDIA_PAUSED || status.width <= 0 || status.height <= 0) {
		printf("media-tool: engine state %u error %d problem %d\n", status.state, status.error, status.problem);
		media_engine_close(engine);
		return 1;
	}

	/* Its first picture, shown at once even while paused. */
	pixels = calloc((size_t)status.width * (size_t)status.height, sizeof(*pixels));
	drawn = 0;
	for (waits = 0; pixels != NULL && !drawn && waits < MEDIA_TOOL_SECONDS * 100; waits++) {
		drawn = media_engine_picture(engine, pixels, (size_t)status.width, status.width, status.height, &next);
		if (!drawn)
			media_tool_pause();
	}

	/* Print the hash of the picture, or say that none came. */
	if (drawn) {
		hash = media_tool_hash(pixels, (size_t)status.width * (size_t)status.height);
		printf("media-tool: engine %dx%d %016llx\n", status.width, status.height, hash);
	} else {
		printf("media-tool: engine gave no picture\n");
	}

	/* Release the pixels and the engine. */
	free(pixels);
	media_engine_close(engine);

	/* Succeeded when it drew. */
	if (!drawn)
		return 1;
	return 0;
}

/* The FNV-1a 64 of the pixels, each as its 4 bytes from the lowest. */
static uint64_t
media_tool_hash(
	const uint32_t *pixels,
	size_t count)
{
	uint64_t hash;
	size_t index;
	unsigned shift;

	/* Every byte of every pixel. */
	hash = 0xcbf29ce484222325ULL;
	for (index = 0U; index < count; index++) {
		for (shift = 0U; shift < 32U; shift += 8U) {
			hash ^= (pixels[index] >> shift) & 0xffU;
			hash *= 0x100000001b3ULL;
		}
	}

	/* The hash. */
	return hash;
}

/* The library's log, to the error output. */
static void
media_tool_log(
	void *context,
	const char *line)
{
	(void)context;
	fprintf(stderr, "%s\n", line);
}

/* Waits 10 ms (Sleep on Windows: no POSIX thread layer there). */
static void
media_tool_pause(void)
{
#ifdef _WIN32
	Sleep(10);
#else
	struct timespec wait;

	/* Wait ten milliseconds. */
	wait.tv_sec = 0;
	wait.tv_nsec = 10000000L;
	nanosleep(&wait, NULL);
#endif
}
