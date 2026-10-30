/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The speed measurement of the software H.264 decoder: decodes an Annex
 * B file from memory, NAL unit by NAL unit as the libmedia back end does,
 * and times it.
 *
 *   speed-bench FILE RUNS
 *     decodes FILE RUNS times, each with a new decoder, and prints one
 *     line per run "run <n> frames <f> wall <s> cpu <s> fps <r>" and the
 *     last line "speed-bench: frames <f> best_fps <r> median_fps <r>
 *     concealed <c> errors <e>".
 *   speed-bench --yuv FILE
 *     decodes FILE once and writes every output frame, cropped, as I420
 *     to the standard output (for a comparison with another decoder).
 *
 * The time covers the NAL unit search, the decoding and taking the
 * output frames; reading the file does not count.
 */

#include "userland/desktop/libmedia/h264sw-decoder.h"
#include "userland/desktop/libmedia/vcodec-nal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The largest file read. */
#define SPEED_MAX_FILE (1024UL * 1024UL * 1024UL)

/* The most runs of one measurement. */
#define SPEED_MAX_RUNS 64U

struct speed_run;

static uint8_t *read_file(const char *path, size_t *size);
static int write_frame(const struct h264sw_frame *frame);
static int take_output(struct h264sw_decoder *decoder, int write, unsigned long *frames);
static double clock_seconds(clockid_t id);
static int decode_once(const uint8_t *data, size_t size, int write, struct speed_run *run);
static int compare_fps(const void *a, const void *b);
static int measure(const uint8_t *data, size_t size, unsigned runs);

/*
 * The result of one decode: frames output, the wall and CPU seconds, the
 * concealed macroblocks and the NAL units refused.
 */
struct speed_run {
	unsigned long frames;
	double wall;
	double cpu;
	unsigned long concealed;
	unsigned long errors;
};

/*
 * Reads a whole file into memory.  Returns the bytes (the caller frees
 * them) and *size, or NULL with a message.
 */
static uint8_t *
read_file(
	const char *path,
	size_t *size)
{
	FILE *file;
	uint8_t *data;
	long length;
	size_t got;
	int result;

	/* Opens the file and finds its length. */
	file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "speed-bench: cannot open %s\n", path);
		return NULL;
	}
	result = fseek(file, 0L, SEEK_END);
	if (result != 0) {
		fprintf(stderr, "speed-bench: cannot seek %s\n", path);
		fclose(file);
		return NULL;
	}
	length = ftell(file);
	if (length <= 0L || (unsigned long)length > SPEED_MAX_FILE) {
		fprintf(stderr, "speed-bench: %s is empty or too large\n", path);
		fclose(file);
		return NULL;
	}
	rewind(file);

	/* Reads the bytes. */
	data = malloc((size_t)length);
	if (data == NULL) {
		fprintf(stderr, "speed-bench: out of memory\n");
		fclose(file);
		return NULL;
	}
	got = fread(data, 1U, (size_t)length, file);
	fclose(file);
	if (got != (size_t)length) {
		fprintf(stderr, "speed-bench: short read of %s\n", path);
		free(data);
		return NULL;
	}
	*size = (size_t)length;
	return data;
}

/*
 * Writes the cropped I420 samples of a frame to the standard output.
 * Returns 0, or -1 when the write failed.
 */
static int
write_frame(
	const struct h264sw_frame *frame)
{
	unsigned plane;
	unsigned row;
	unsigned x;
	unsigned y;
	unsigned width;
	unsigned height;
	size_t wrote;

	/* Writes the window of each plane; chroma is half the luma window (4:2:0). */
	for (plane = 0U;
	     plane < 3U;
	     plane++) {
		/* The window of this plane. */
		x = frame->crop_x;
		y = frame->crop_y;
		width = frame->crop_width;
		height = frame->crop_height;
		if (plane != 0U) {
			x /= 2U;
			y /= 2U;
			width /= 2U;
			height /= 2U;
		}

		/* Writes each row of the window. */
		for (row = 0U;
		     row < height;
		     row++) {
			wrote = fwrite(frame->plane[plane] + (size_t)(y + row) * frame->stride[plane] + x, 1U, width, stdout);
			if (wrote != width)
				return -1;
		}
	}
	return 0;
}

/*
 * Takes every output frame; with write set, writes each out.  Counts
 * them into *frames.  Returns 0, or -1 when a write failed.
 */
static int
take_output(
	struct h264sw_decoder *decoder,
	int write,
	unsigned long *frames)
{
	struct h264sw_frame *frame;
	int written;
	int result;

	/* Takes frames until none is ready. */
	result = 0;
	for (;;) {
		frame = h264sw_decoder_output(decoder);
		if (frame == NULL)
			break;

		/* Writes the frame when asked; a failed write is remembered and the frames are still taken. */
		if (write) {
			written = write_frame(frame);
			if (written != 0)
				result = -1;
		}
		(*frames)++;
		h264sw_frame_unref(frame);
	}
	return result;
}

/*
 * The seconds of a clock.
 */
static double
clock_seconds(
	clockid_t id)
{
	struct timespec now;
	int result;

	/* A clock that cannot be read counts as 0; the wall clock always can. */
	result = clock_gettime(id, &now);
	if (result != 0)
		return 0.0;
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/*
 * Decodes the data once with a new decoder.  Returns 0 and *run, or -1
 * with a message.
 */
static int
decode_once(
	const uint8_t *data,
	size_t size,
	int write,
	struct speed_run *run)
{
	struct h264sw_decoder *decoder;
	const uint8_t *nal;
	size_t nal_size;
	size_t cursor;
	double wall_start;
	double cpu_start;
	int found;
	int taken;
	int error;
	int failed;

	/* Makes the decoder. */
	memset(run, 0, sizeof(*run));
	error = h264sw_decoder_create(&decoder);
	if (error != 0) {
		fprintf(stderr, "speed-bench: cannot make a decoder (%d)\n", error);
		return -1;
	}

	/* Decodes every NAL unit, taking the output before each so the decoder is never busy. */
	failed = 0;
	wall_start = clock_seconds(CLOCK_MONOTONIC);
	cpu_start = clock_seconds(CLOCK_PROCESS_CPUTIME_ID);
	cursor = 0U;
	for (;;) {
		found = vcodec_nal_find(data, size, &cursor, &nal, &nal_size);
		if (!found)
			break;
		taken = take_output(decoder, write, &run->frames);
		if (taken != 0)
			failed = 1;
		error = h264sw_decoder_nal(decoder, nal, nal_size, (int64_t)(nal - data));

		/* A refused NAL unit is counted; the decoder goes on as it does for the back end. */
		if (error != 0)
			run->errors++;
	}

	/* Takes what the last NAL unit output (a flush refuses to run before), ends the stream and takes the last frames. */
	taken = take_output(decoder, write, &run->frames);
	if (taken != 0)
		failed = 1;
	error = h264sw_decoder_flush(decoder);
	if (error != 0)
		run->errors++;
	taken = take_output(decoder, write, &run->frames);
	if (taken != 0)
		failed = 1;
	run->wall = clock_seconds(CLOCK_MONOTONIC) - wall_start;
	run->cpu = clock_seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
	run->concealed = (unsigned long)decoder->stats.concealed_mbs;
	h264sw_decoder_destroy(decoder);

	/* A failed write ends the --yuv mode with an error. */
	if (failed) {
		fprintf(stderr, "speed-bench: writing the frames failed\n");
		return -1;
	}
	return 0;
}

/*
 * Orders two frame rates for qsort.
 */
static int
compare_fps(
	const void *a,
	const void *b)
{
	double x;
	double y;

	/* Ascending. */
	x = *(const double *)a;
	y = *(const double *)b;
	if (x < y)
		return -1;
	if (x > y)
		return 1;
	return 0;
}

/*
 * Measures RUNS decodes of a file and prints the summary.  Returns the
 * exit status.
 */
static int
measure(
	const uint8_t *data,
	size_t size,
	unsigned runs)
{
	struct speed_run run;
	double fps[SPEED_MAX_RUNS];
	unsigned long frames;
	unsigned long concealed;
	unsigned long errors;
	unsigned i;
	int result;

	/* Decodes the file runs times; every run must give the same number of frames. */
	frames = 0UL;
	concealed = 0UL;
	errors = 0UL;
	for (i = 0U;
	     i < runs;
	     i++) {
		result = decode_once(data, size, 0, &run);
		if (result != 0)
			return 1;
		if (i == 0U)
			frames = run.frames;
		if (run.frames != frames || run.frames == 0UL) {
			printf("speed-bench: FAIL (run %u gave %lu frames, the first %lu)\n", i, run.frames, frames);
			return 1;
		}
		concealed = run.concealed;
		errors = run.errors;

		/* A run too short to time counts as infinitely fast; such a file is useless for a measurement. */
		fps[i] = 0.0;
		if (run.wall > 0.0)
			fps[i] = (double)run.frames / run.wall;
		printf("run %u frames %lu wall %.3f cpu %.3f fps %.2f\n", i, run.frames, run.wall, run.cpu, fps[i]);
		fflush(stdout);
	}

	/* The best and the median frame rate. */
	qsort(fps, runs, sizeof(fps[0]), compare_fps);
	printf("speed-bench: frames %lu best_fps %.2f median_fps %.2f concealed %lu errors %lu\n", frames,
	    fps[runs - 1U], fps[runs / 2U], concealed, errors);
	return 0;
}

int
main(
	int argc,
	char **argv)
{
	struct speed_run run;
	uint8_t *data;
	size_t size;
	long runs;
	int yuv;
	int result;
	int status;

	/* --yuv FILE: one decode written out. */
	yuv = 0;
	if (argc == 3)
		yuv = strcmp(argv[1], "--yuv") == 0;
	if (yuv) {
		data = read_file(argv[2], &size);
		if (data == NULL)
			return 1;
		status = 0;
		result = decode_once(data, size, 1, &run);
		if (result != 0)
			status = 1;
		free(data);
		result = fflush(stdout);
		if (result != 0)
			status = 1;
		return status;
	}

	/* FILE RUNS: the measurement. */
	if (argc != 3) {
		fprintf(stderr, "usage: speed-bench FILE RUNS | speed-bench --yuv FILE\n");
		return 2;
	}
	runs = strtol(argv[2], NULL, 10);
	if (runs < 1L || runs > (long)SPEED_MAX_RUNS) {
		fprintf(stderr, "speed-bench: RUNS must be 1 to %u\n", SPEED_MAX_RUNS);
		return 2;
	}
	data = read_file(argv[1], &size);
	if (data == NULL)
		return 1;
	status = measure(data, size, (unsigned)runs);
	free(data);
	return status;
}
