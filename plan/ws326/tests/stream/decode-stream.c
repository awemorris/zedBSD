/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The whole-stream test of the software H.264 decoder: decodes an Annex B
 * file NAL unit by NAL unit and prints the MD5 of each output frame,
 * cropped, as I420.
 *
 *   decode-stream FILE
 *     prints "frame <n> <width>x<height> <md5>" for each output frame and
 *     a last line "stats ..." with the decoder's counters.
 *   decode-stream --fuzz FILE SEED COUNT
 *     decodes COUNT damaged copies of FILE (bytes changed, a range cut
 *     out, or the end cut off, chosen by SEED) and checks that the
 *     decoder survives each and that, from the first IDR picture after
 *     the damage, its output equals the output of the whole file.
 *   decode-stream --start FILE
 *     starts decoding at each picture of FILE in turn (as after a seek),
 *     and once more after a reset in the middle, and checks that the
 *     output from the first IDR or I picture on equals the output of the
 *     whole file, without the pictures that predict from before the
 *     start.
 *
 * The time given with each NAL unit is its byte offset in the data, so
 * an output frame tells which picture it was (the offset of its first
 * slice).  The last line is "decode-stream: PASS" or "decode-stream:
 * FAIL (...)".
 */

#include "userland/desktop/libmedia/h264sw-decoder.h"
#include "userland/desktop/libmedia/h264sw-nal.h"
#include "userland/desktop/libmedia/vcodec-bitreader.h"
#include "userland/desktop/libmedia/h264sw-slice.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most output frames one decode keeps for comparison. */
#define STREAM_MAX_FRAMES 4096U

/* The largest file read. */
#define STREAM_MAX_FILE (64UL * 1024UL * 1024UL)

/* The nal_unit_type bits of the first byte of a NAL unit. */
#define STREAM_NAL_TYPE_MASK 0x1fU

/*
 * One output frame as the comparisons need it: the MD5 of its cropped
 * I420 samples (as text), its cropped size, and the time it came with
 * (the byte offset of its picture's first slice).
 */
struct stream_frame {
	char md5[33];
	unsigned width;
	unsigned height;
	int64_t time_us;
	int idr;
};

/*
 * The output of one decode: the frames in output order and the
 * decoder's counters at the end.  Lives for one comparison.
 */
struct stream_output {
	struct stream_frame frame[STREAM_MAX_FRAMES];
	unsigned count;
	unsigned lost;
	struct h264sw_decoder_stats stats;
	unsigned errors;
};

/*
 * The state of an MD5 computation (RFC 1321): the four words, the bytes
 * waiting for a whole block, and the length so far.
 */
struct stream_md5 {
	uint32_t state[4];
	uint8_t block[64];
	unsigned used;
	uint64_t length;
};

/* The constants of the MD5 rounds, floor( abs( sin( i + 1 ) ) * 2^32 ), filled once by md5_setup. */
static uint32_t md5_k[64];

/* The left rotations of the MD5 steps, four per round. */
static const unsigned md5_shift[16] = {
	7U, 12U, 17U, 22U,
	5U, 9U, 14U, 20U,
	4U, 11U, 16U, 23U,
	6U, 10U, 15U, 21U
};

/* The checks made and failed, for the last line. */
static unsigned stream_checks;
static unsigned stream_failures;

/* The state of the pseudo-random generator of the damage, from the seed; the same seed gives the same damage. */
static uint32_t stream_rng;

static int md5_setup(void);
static void md5_init(struct stream_md5 *md5);
static void md5_block(struct stream_md5 *md5, const uint8_t *block);
static void md5_update(struct stream_md5 *md5, const uint8_t *data, size_t size);
static void md5_text(struct stream_md5 *md5, char *text);
static uint32_t rng_next(void);
static void check(int passed, const char *what, unsigned case_number);
static uint8_t *read_file(const char *path, size_t *size);
static void frame_md5(const struct h264sw_frame *frame, char *text);
static void take_output(struct h264sw_decoder *decoder, struct stream_output *output);
static void feed(struct h264sw_decoder *decoder, const uint8_t *data, size_t size, size_t from, size_t to, struct stream_output *output);
static void feed_parameter_sets(struct h264sw_decoder *decoder, const uint8_t *data, size_t size, size_t to, struct stream_output *output);
static void decode(const uint8_t *data, size_t size, size_t from, struct stream_output *output);
static int first_picture_at(const uint8_t *data, size_t size, size_t from, int idr_only, size_t *offset);
static unsigned frames_from(const struct stream_output *output, int64_t time_us);
static int compare_tail(const struct stream_output *whole, int64_t whole_time, const struct stream_output *part, int64_t part_time);
static int run_plain(const uint8_t *data, size_t size);
static int run_fuzz(const uint8_t *data, size_t size, uint32_t seed, unsigned count);
static int run_start(const uint8_t *data, size_t size);
static unsigned slice_type_at(const uint8_t *data, size_t size, size_t offset);

/*
 * Runs the mode the arguments name.
 */
int
main(
	int argc,
	char **argv)
{
	uint8_t *data;
	size_t size;
	int difference;
	int status;

	/* Checks the MD5 against its known answers before trusting it. */
	status = md5_setup();
	if (status != 0) {
		printf("decode-stream: FAIL (md5 self-test)\n");
		return 1;
	}

	/* Decodes one file and prints its frames. */
	if (argc == 2) {
		data = read_file(argv[1], &size);
		if (data == NULL)
			return 1;
		status = run_plain(data, size);
		free(data);
		return status;
	}

	/* Decodes damaged copies of a file. */
	difference = 1;
	if (argc == 5)
		difference = strcmp(argv[1], "--fuzz");
	if (difference == 0) {
		data = read_file(argv[2], &size);
		if (data == NULL)
			return 1;
		status = run_fuzz(data, size, (uint32_t)strtoul(argv[3], NULL, 0), (unsigned)strtoul(argv[4], NULL, 0));
		free(data);
		return status;
	}

	/* Starts a file at each picture. */
	difference = 1;
	if (argc == 3)
		difference = strcmp(argv[1], "--start");
	if (difference == 0) {
		data = read_file(argv[2], &size);
		if (data == NULL)
			return 1;
		status = run_start(data, size);
		free(data);
		return status;
	}

	/* Anything else is a usage mistake. */
	fprintf(stderr, "usage: decode-stream FILE | --fuzz FILE SEED COUNT | --start FILE\n");
	return 2;
}

/*
 * Fills the MD5 constants and checks the MD5 of two known strings (RFC 1321, A.5).
 */
static int
md5_setup(void)
{
	struct stream_md5 md5;
	char text[33];
	unsigned index;
	int difference;

	/* The constants are the integer parts of 2^32 times the sines (RFC 1321, 3.4). */
	for (index = 0U;
	     index < 64U;
	     index++)
		md5_k[index] = (uint32_t)floor(fabs(sin((double)(index + 1U))) * 4294967296.0);

	/* The empty string. */
	md5_init(&md5);
	md5_text(&md5, text);
	difference = strcmp(text, "d41d8cd98f00b204e9800998ecf8427e");
	if (difference != 0)
		return 1;

	/* "message digest", which needs no second block. */
	md5_init(&md5);
	md5_update(&md5, (const uint8_t *)"message digest", 14U);
	md5_text(&md5, text);
	difference = strcmp(text, "f96b697d7cb7938d525a2f31aaf161d0");
	if (difference != 0)
		return 1;

	/* Succeeded: the MD5 gives the known answers. */
	return 0;
}

/*
 * Starts an MD5 computation (RFC 1321, 3.3).
 */
static void
md5_init(
	struct stream_md5 *md5)
{
	/* The initial words A, B, C, D. */
	md5->state[0] = 0x67452301U;
	md5->state[1] = 0xefcdab89U;
	md5->state[2] = 0x98badcfeU;
	md5->state[3] = 0x10325476U;
	md5->used = 0U;
	md5->length = 0U;
}

/*
 * Processes one 64-byte block (RFC 1321, 3.4).
 */
static void
md5_block(
	struct stream_md5 *md5,
	const uint8_t *block)
{
	uint32_t word[16];
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;
	uint32_t f;
	uint32_t held;
	uint32_t sum;
	unsigned index;
	unsigned g;
	unsigned shift;

	/* Reads the block as sixteen little-endian words. */
	for (index = 0U;
	     index < 16U;
	     index++) {
		word[index] = (uint32_t)block[4U * index] |
			((uint32_t)block[4U * index + 1U] << 8) |
			((uint32_t)block[4U * index + 2U] << 16) |
			((uint32_t)block[4U * index + 3U] << 24);
	}

	/* Runs the 64 steps of the four rounds. */
	a = md5->state[0];
	b = md5->state[1];
	c = md5->state[2];
	d = md5->state[3];
	for (index = 0U;
	     index < 64U;
	     index++) {
		/* The round's function and the word it reads. */
		if (index < 16U) {
			f = (b & c) | (~b & d);
			g = index;
		} else if (index < 32U) {
			f = (d & b) | (~d & c);
			g = (5U * index + 1U) % 16U;
		} else if (index < 48U) {
			f = b ^ c ^ d;
			g = (3U * index + 5U) % 16U;
		} else {
			f = c ^ (b | ~d);
			g = (7U * index) % 16U;
		}

		/* Rotates the sum into B and moves the words along. */
		shift = md5_shift[(index / 16U) * 4U + index % 4U];
		sum = a + f + md5_k[index] + word[g];
		held = d;
		d = c;
		c = b;
		b = b + ((sum << shift) | (sum >> (32U - shift)));
		a = held;
	}

	/* Adds the block's result to the state. */
	md5->state[0] += a;
	md5->state[1] += b;
	md5->state[2] += c;
	md5->state[3] += d;
}

/*
 * Adds bytes to an MD5 computation.
 */
static void
md5_update(
	struct stream_md5 *md5,
	const uint8_t *data,
	size_t size)
{
	size_t index;

	/* Collects the bytes into blocks and processes each full one. */
	md5->length += size;
	for (index = 0U;
	     index < size;
	     index++) {
		md5->block[md5->used] = data[index];
		md5->used++;
		if (md5->used == 64U) {
			md5_block(md5, md5->block);
			md5->used = 0U;
		}
	}
}

/*
 * Ends an MD5 computation and writes the digest as 32 hexadecimal digits (RFC 1321, 3.1, 3.2, 3.5).
 */
static void
md5_text(
	struct stream_md5 *md5,
	char *text)
{
	static const char digits[] = "0123456789abcdef";
	uint8_t length[8];
	uint8_t pad;
	uint64_t bits;
	unsigned index;
	unsigned byte;

	/* The length in bits, little-endian, taken before the padding. */
	bits = md5->length * 8U;
	for (index = 0U;
	     index < 8U;
	     index++)
		length[index] = (uint8_t)(bits >> (8U * index));

	/* Pads with a one bit and zeros up to 56 bytes of a block, then the length. */
	pad = 0x80U;
	md5_update(md5, &pad, 1U);
	pad = 0U;
	while (md5->used != 56U)
		md5_update(md5, &pad, 1U);
	md5_update(md5, length, 8U);

	/* Writes the four words, little-endian, as hexadecimal. */
	for (index = 0U;
	     index < 16U;
	     index++) {
		byte = (unsigned)((md5->state[index / 4U] >> (8U * (index % 4U))) & 0xffU);
		text[2U * index] = digits[byte >> 4];
		text[2U * index + 1U] = digits[byte & 15U];
	}

	/* Ends the digest text. */
	text[32] = '\0';
}

/*
 * Gives the next pseudo-random number (a linear congruential generator).
 */
static uint32_t
rng_next(void)
{
	/* Moves the state and gives its upper bits, which vary most. */
	stream_rng = stream_rng * 1103515245U + 12345U;
	return stream_rng >> 8;
}

/*
 * Records one check; a failure prints what failed and in which case.
 */
static void
check(
	int passed,
	const char *what,
	unsigned case_number)
{
	/* Every check counts. */
	stream_checks++;
	if (passed)
		return;

	/* A failure says what and where. */
	stream_failures++;
	printf("FAIL case %u: %s\n", case_number, what);
}

/*
 * Reads a whole file into memory.
 */
static uint8_t *
read_file(
	const char *path,
	size_t *size)
{
	FILE *file;
	uint8_t *data;
	size_t length;

	/* Opens the file. */
	file = fopen(path, "rb");
	if (file == NULL) {
		printf("decode-stream: FAIL (cannot open %s)\n", path);
		return NULL;
	}

	/* Reads up to the limit; a file at the limit is refused. */
	data = malloc(STREAM_MAX_FILE);
	if (data == NULL) {
		fclose(file);
		printf("decode-stream: FAIL (no memory)\n");
		return NULL;
	}

	/* Reads the whole file and refuses an empty or oversized one. */
	length = fread(data, 1U, STREAM_MAX_FILE, file);
	fclose(file);
	if (length == 0U || length >= STREAM_MAX_FILE) {
		free(data);
		printf("decode-stream: FAIL (size of %s)\n", path);
		return NULL;
	}

	/* Succeeded: the caller frees the data. */
	*size = length;
	return data;
}

/*
 * Computes the MD5 of a frame's cropped samples as I420 (Y, then Cb, then Cr, rows without padding).
 */
static void
frame_md5(
	const struct h264sw_frame *frame,
	char *text)
{
	struct stream_md5 md5;
	unsigned plane;
	unsigned row;
	unsigned x;
	unsigned y;
	unsigned width;
	unsigned height;

	/* Hashes the window of each plane; chroma is half the luma window (4:2:0). */
	md5_init(&md5);
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

		/* Hashes each row of the window. */
		for (row = 0U;
		     row < height;
		     row++)
			md5_update(&md5, frame->plane[plane] + (size_t)(y + row) * frame->stride[plane] + x, width);
	}

	/* Finishes the digest of the frame. */
	md5_text(&md5, text);
}

/*
 * Takes every frame the decoder has output and records it.
 */
static void
take_output(
	struct h264sw_decoder *decoder,
	struct stream_output *output)
{
	struct h264sw_frame *frame;
	struct stream_frame *record;

	/* Takes frames until none is ready. */
	for (;;) {
		frame = h264sw_decoder_output(decoder);
		if (frame == NULL)
			break;

		/* Records the frame, or counts it when the record is full. */
		if (output->count < STREAM_MAX_FRAMES) {
			record = &output->frame[output->count];
			frame_md5(frame, record->md5);
			record->width = frame->crop_width;
			record->height = frame->crop_height;
			record->time_us = frame->time_us;
			record->idr = frame->idr;
			output->count++;
		} else {
			output->lost++;
		}

		/* The caller's reference goes. */
		h264sw_frame_unref(frame);
	}
}

/*
 * Gives a decoder the NAL units whose bytes begin in data[from..to), each with its offset as its time.
 *
 * from is 0 or the offset of a NAL unit (just after its start code).
 */
static void
feed(
	struct h264sw_decoder *decoder,
	const uint8_t *data,
	size_t size,
	size_t from,
	size_t to,
	struct stream_output *output)
{
	const uint8_t *nal;
	size_t nal_size;
	size_t cursor;
	int found;
	int error;

	/* Walks the NAL units of the range; a start at a NAL unit's offset backs up over its start code, which the search looks for. */
	cursor = 0U;
	if (from >= 3U)
		cursor = from - 3U;
	for (;;) {
		found = vcodec_nal_find(data, size, &cursor, &nal, &nal_size);
		if (!found)
			break;
		if ((size_t)(nal - data) >= to)
			break;

		/* Takes the output before each NAL unit, so the decoder is never busy. */
		take_output(decoder, output);
		error = h264sw_decoder_nal(decoder, nal, nal_size, (int64_t)(nal - data));
		if (error == EBUSY) {
			printf("decoder busy after the output was taken\n");
			output->errors++;
		} else if (error != 0) {
			/* A refused NAL unit is part of a damaged stream's life; the decoder goes on. */
			output->errors++;
		}
	}

	/* Takes what the last NAL unit output. */
	take_output(decoder, output);
}

/*
 * Gives a decoder the SPS and PPS NAL units before an offset, as a player that starts in the middle of a stream has them from the container.
 */
static void
feed_parameter_sets(
	struct h264sw_decoder *decoder,
	const uint8_t *data,
	size_t size,
	size_t to,
	struct stream_output *output)
{
	const uint8_t *nal;
	size_t nal_size;
	size_t cursor;
	unsigned type;
	int found;
	int error;

	/* Walks the NAL units before the offset and gives only the parameter sets. */
	cursor = 0U;
	for (;;) {
		found = vcodec_nal_find(data, size, &cursor, &nal, &nal_size);
		if (!found)
			break;
		if ((size_t)(nal - data) >= to)
			break;
		type = nal[0] & STREAM_NAL_TYPE_MASK;
		if (type != H264SW_NAL_SPS && type != H264SW_NAL_PPS)
			continue;

		/* A parameter set produces no output and is never refused in a valid stream. */
		error = h264sw_decoder_nal(decoder, nal, nal_size, (int64_t)(nal - data));
		if (error != 0)
			output->errors++;
	}
}

/*
 * Decodes data from offset from to the end, then flushes.
 *
 * A start after offset 0 first gives the parameter sets before it.
 */
static void
decode(
	const uint8_t *data,
	size_t size,
	size_t from,
	struct stream_output *output)
{
	struct h264sw_decoder *decoder;
	int error;

	/* Starts with an empty record. */
	memset(output, 0, sizeof(*output));

	/* Makes the decoder. */
	error = h264sw_decoder_create(&decoder);
	if (error != 0) {
		output->errors++;
		return;
	}

	/* Decodes, flushes and takes the last frames. */
	if (from > 0U)
		feed_parameter_sets(decoder, data, size, from, output);
	feed(decoder, data, size, from, size, output);
	error = h264sw_decoder_flush(decoder);
	if (error != 0)
		output->errors++;
	take_output(decoder, output);

	/* Keeps the counters and frees the decoder. */
	output->stats = decoder->stats;
	h264sw_decoder_destroy(decoder);
}

/*
 * Finds the first NAL unit at or after from that is a slice with first_mb_in_slice 0 (an IDR slice when idr_only).
 *
 * first_mb_in_slice 0 is the first bit of the slice header being 1
 * (ue(v) of 0).  Returns 1 and *offset, or 0 when there is none.
 */
static int
first_picture_at(
	const uint8_t *data,
	size_t size,
	size_t from,
	int idr_only,
	size_t *offset)
{
	const uint8_t *nal;
	size_t nal_size;
	size_t cursor;
	unsigned type;
	int found;

	/* Walks the NAL units from the offset. */
	cursor = from;
	for (;;) {
		found = vcodec_nal_find(data, size, &cursor, &nal, &nal_size);
		if (!found)
			return 0;
		if (nal_size < 2U)
			continue;

		/* Keeps to the slices that begin a picture. */
		type = nal[0] & STREAM_NAL_TYPE_MASK;
		if (type != H264SW_NAL_IDR && (idr_only || type != H264SW_NAL_SLICE))
			continue;
		if ((nal[1] & 0x80U) == 0U)
			continue;

		/* Found the picture. */
		*offset = (size_t)(nal - data);
		return 1;
	}
}

/*
 * Finds the index of the first output frame with a time at or after time_us, in output order, after which every frame also has one.
 *
 * Returns output->count when there is none.
 */
static unsigned
frames_from(
	const struct stream_output *output,
	int64_t time_us)
{
	unsigned index;

	/* Walks back from the end while the frames are at or after the time. */
	index = output->count;
	while (index > 0U && output->frame[index - 1U].time_us >= time_us)
		index--;

	/* Reports where the tail begins. */
	return index;
}

/*
 * Compares the tails of two outputs: the frames from the picture at whole_time in the whole decode and from part_time in the other.
 *
 * Returns 1 when the tails have the same frames in the same order.
 */
static int
compare_tail(
	const struct stream_output *whole,
	int64_t whole_time,
	const struct stream_output *part,
	int64_t part_time)
{
	unsigned whole_index;
	unsigned part_index;
	int difference;

	/* Finds where each tail begins. */
	whole_index = frames_from(whole, whole_time);
	part_index = frames_from(part, part_time);

	/* The tails have the same length. */
	if (whole->count - whole_index != part->count - part_index) {
		printf("tail lengths %u and %u\n", whole->count - whole_index, part->count - part_index);
		return 0;
	}

	/* Each frame of one tail equals the frame at the same place of the other. */
	while (whole_index < whole->count) {
		difference = strcmp(whole->frame[whole_index].md5, part->frame[part_index].md5);
		if (difference != 0) {
			printf("frame %u differs\n", whole_index);
			return 0;
		}

		/* Moves both tails to their next frame. */
		whole_index++;
		part_index++;
	}

	/* Succeeded: the tails are equal. */
	return 1;
}

/*
 * Decodes a whole file and prints each frame's MD5 and the decoder's counters.
 */
static int
run_plain(
	const uint8_t *data,
	size_t size)
{
	static struct stream_output output;
	unsigned index;

	/* Decodes the whole file. */
	decode(data, size, 0U, &output);

	/* Prints each frame. */
	for (index = 0U;
	     index < output.count;
	     index++)
		printf("frame %u %ux%u %s\n", index, output.frame[index].width, output.frame[index].height, output.frame[index].md5);

	/* Prints the counters. */
	printf("stats pictures %lu output %lu concealed %lu slice_errors %lu dropped %lu unsupported %lu refused %u lost %u\n",
	       (unsigned long)output.stats.pictures,
	       (unsigned long)output.stats.output,
	       (unsigned long)output.stats.concealed_mbs,
	       (unsigned long)output.stats.slice_errors,
	       (unsigned long)output.stats.dropped,
	       (unsigned long)output.stats.unsupported,
	       output.errors,
	       output.lost);

	/* Reports a stream that was not decoded cleanly. */
	if (output.errors != 0U || output.lost != 0U || output.count == 0U) {
		printf("decode-stream: FAIL (errors)\n");
		return 1;
	}

	/* Succeeded: every frame is printed. */
	printf("decode-stream: PASS (%u frames)\n", output.count);
	return 0;
}

/*
 * Decodes damaged copies of a file and checks the recovery at the next IDR picture.
 *
 * Each case damages the middle six eighths of the file: some bytes
 * changed in a short range, a range cut out, or the end cut off.
 */
static int
run_fuzz(
	const uint8_t *data,
	size_t size,
	uint32_t seed,
	unsigned count)
{
	static struct stream_output whole;
	static struct stream_output damaged;
	uint8_t *copy;
	size_t copy_size;
	size_t start;
	size_t length;
	size_t position;
	size_t idr;
	unsigned kind;
	unsigned changes;
	unsigned index;
	unsigned recovered;
	int has_idr;
	int same;

	/* Decodes the whole file once: the reference the tails are compared with. */
	decode(data, size, 0U, &whole);
	check(whole.errors == 0U && whole.count > 0U, "whole file decodes", 0U);

	/* The copy each case damages. */
	copy = malloc(size);
	if (copy == NULL) {
		printf("decode-stream: FAIL (no memory)\n");
		return 1;
	}

	/* Runs each case. */
	stream_rng = seed;
	recovered = 0U;
	for (index = 1U;
	     index <= count;
	     index++) {
		/* Chooses where the damage starts and what it is. */
		start = size / 8U + (size_t)rng_next() % (size * 6U / 8U);
		kind = rng_next() % 3U;
		memcpy(copy, data, size);
		copy_size = size;
		length = 0U;
		if (kind == 0U) {
			/* Changes one to eight bytes within 64 bytes. */
			length = 64U;
			if (start + length > size)
				length = size - start;
			changes = 1U + rng_next() % 8U;
			while (changes > 0U) {
				position = start + (size_t)rng_next() % length;
				copy[position] = (uint8_t)(copy[position] ^ (1U + rng_next() % 255U));
				changes--;
			}
		} else if (kind == 1U) {
			/* Cuts out up to 2000 bytes. */
			length = 1U + (size_t)rng_next() % 2000U;
			if (start + length > size)
				length = size - start;
			memmove(copy + start, copy + start + length, size - start - length);
			copy_size = size - length;
		} else {
			/* Cuts off the end. */
			length = size - start;
			copy_size = start;
		}

		/* Decodes the damaged copy; the sanitizers report any fault. */
		decode(copy, copy_size, 0U, &damaged);
		check(damaged.lost == 0U, "output within the record", index);

		/* From the first IDR picture after the damage, the output is the whole file's. */
		has_idr = first_picture_at(data, size, start + length, 1, &idr);
		if (has_idr && kind != 2U) {
			if (kind == 0U) {
				same = compare_tail(&whole, (int64_t)idr, &damaged, (int64_t)idr);
			} else {
				same = compare_tail(&whole, (int64_t)idr, &damaged, (int64_t)(idr - length));
			}

			/* Records whether the output recovered from the next IDR picture, and counts it. */
			check(same, "output from the next IDR picture", index);
			if (same)
				recovered++;
		}
	}

	/* Frees the damaged copy. */
	free(copy);

	/* Reports the cases. */
	if (stream_failures != 0U) {
		printf("decode-stream: FAIL (%u of %u)\n", stream_failures, stream_checks);
		return 1;
	}

	/* Succeeded: every damaged copy decoded, and recovered where an IDR picture followed. */
	printf("decode-stream: PASS (%u cases, %u recovered at an IDR picture, %u checks)\n", count, recovered, stream_checks);
	return 0;
}

/*
 * Starts the decoding at each picture of a file, and once after a reset, and checks the output from the start on.
 *
 * A start at an IDR picture gives every frame from it on.  A start at a
 * non-IDR I picture gives the frames from it on that do not come before
 * it in output order (the leading pictures, which predict from before
 * the start).  A start at a P or B picture drops pictures until the
 * first I or IDR picture, and from the first IDR picture on gives the
 * whole file's frames.
 */
static int
run_start(
	const uint8_t *data,
	size_t size)
{
	static struct stream_output whole;
	static struct stream_output part;
	struct h264sw_decoder *decoder;
	size_t cursor;
	size_t picture;
	size_t idr;
	size_t middle;
	unsigned type;
	unsigned slice_type;
	unsigned starts;
	unsigned index;
	unsigned leading;
	unsigned leading_total;
	int difference;
	int found;
	int has_idr;
	int same;
	int error;

	/* Decodes the whole file once. */
	decode(data, size, 0U, &whole);
	check(whole.errors == 0U && whole.count > 0U, "whole file decodes", 0U);

	/* Starts at every picture's first slice. */
	starts = 0U;
	leading_total = 0U;
	cursor = 0U;
	for (;;) {
		found = first_picture_at(data, size, cursor, 0, &picture);
		if (!found)
			break;
		cursor = picture + 1U;
		starts++;

		/* Decodes from the picture. */
		decode(data, size, picture, &part);
		check(part.errors == 0U, "no refused NAL unit after a start", starts);

		/* An IDR or I picture starts the output at once; the others are dropped until one. */
		type = data[picture] & STREAM_NAL_TYPE_MASK;
		slice_type = slice_type_at(data, size, picture);
		if (type == H264SW_NAL_IDR || slice_type == H264SW_SLICE_I) {
			/* The whole decode's frames from the start picture's place in output order on; those before it are the leading ones. */
			index = 0U;
			while (index < whole.count && whole.frame[index].time_us != (int64_t)picture)
				index++;
			check(index < whole.count, "start picture is output", starts);
			same = 0;
			if (index < whole.count && whole.count - index == part.count) {
				same = 1;
				while (same && index < whole.count) {
					difference = strcmp(whole.frame[index].md5, part.frame[part.count - (whole.count - index)].md5);
					if (difference != 0)
						same = 0;
					index++;
				}
			}

			/* Records whether the output from the start picture matched the whole decode's. */
			check(same, "output from a start at an IDR or I picture", starts);
			if (type == H264SW_NAL_IDR)
				check(part.stats.dropped == 0U, "nothing dropped at an IDR start", starts);

			/* The pictures dropped are the leading ones: decoded after the start but output before it in the whole decode. */
			leading = 0U;
			for (index = 0U;
			     index < whole.count && whole.frame[index].time_us != (int64_t)picture;
			     index++) {
				if (whole.frame[index].time_us > (int64_t)picture)
					leading++;
			}

			/* Records that exactly the leading pictures were dropped. */
			check(part.stats.dropped == leading, "the leading pictures are dropped", starts);
			leading_total += leading;
		} else {
			/* A P or B start: the pictures before the next IDR picture may differ, the rest may not. */
			check(part.stats.dropped > 0U, "pictures dropped before the start", starts);
			has_idr = first_picture_at(data, size, picture + 1U, 1, &idr);
			if (has_idr) {
				same = compare_tail(&whole, (int64_t)idr, &part, (int64_t)idr);
				check(same, "output from the IDR picture after a P or B start", starts);
			}
		}
	}

	/* A reset in the middle of the file: the first half, a reset, then the file again from a picture after the middle. */
	memset(&part, 0, sizeof(part));
	error = h264sw_decoder_create(&decoder);
	check(error == 0, "decoder for the reset", 0U);
	if (error == 0) {
		middle = size / 2U;
		feed(decoder, data, size, 0U, middle, &part);
		h264sw_decoder_reset(decoder);
		part.count = 0U;
		found = first_picture_at(data, size, middle + size / 8U, 0, &picture);
		if (found) {
			feed(decoder, data, size, picture, size, &part);
			error = h264sw_decoder_flush(decoder);
			check(error == 0, "flush after the reset", 0U);
			take_output(decoder, &part);
			has_idr = first_picture_at(data, size, picture, 1, &idr);
			if (has_idr) {
				same = compare_tail(&whole, (int64_t)idr, &part, (int64_t)idr);
				check(same, "output from the IDR picture after a reset", 0U);
			}
		}

		/* Frees the decoder of this start. */
		h264sw_decoder_destroy(decoder);
	}

	/* Reports the cases. */
	if (stream_failures != 0U) {
		printf("decode-stream: FAIL (%u of %u)\n", stream_failures, stream_checks);
		return 1;
	}

	/* Succeeded: every start gave the frames it must. */
	printf("decode-stream: PASS (%u starts, %u leading pictures dropped, %u checks)\n", starts, leading_total, stream_checks);
	return 0;
}

/*
 * Reads the slice type (slice_type % 5) of the slice NAL unit at an offset, or 99 when it does not read.
 */
static unsigned
slice_type_at(
	const uint8_t *data,
	size_t size,
	size_t offset)
{
	static uint8_t rbsp[STREAM_MAX_FILE / 16U];
	struct h264sw_nal unit;
	struct vcodec_bitreader bits;
	const uint8_t *nal;
	size_t nal_size;
	size_t cursor;
	uint32_t slice_type;
	int found;
	int error;

	/* Finds and unescapes the NAL unit, backing up over its start code, which the search looks for. */
	cursor = 0U;
	if (offset >= 3U)
		cursor = offset - 3U;
	found = vcodec_nal_find(data, size, &cursor, &nal, &nal_size);
	if (!found)
		return 99U;
	if (nal_size > sizeof(rbsp))
		return 99U;
	error = h264sw_nal_parse(nal, nal_size, rbsp, sizeof(rbsp), &unit);
	if (error != 0)
		return 99U;

	/* Reads first_mb_in_slice, then slice_type (7.3.3). */
	vcodec_bitreader_init(&bits, unit.rbsp, unit.rbsp_size);
	(void)vcodec_bitreader_ue(&bits);
	slice_type = vcodec_bitreader_ue(&bits);
	if (bits.error)
		return 99U;

	/* Reports the type without the "all slices alike" offset. */
	return (unsigned)(slice_type % 5U);
}
