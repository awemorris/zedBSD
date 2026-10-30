/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The conformance check of the software H.264 decoder: decodes one
 * conformance bitstream (Annex B) NAL unit by NAL unit and compares each
 * output frame, cropped and as I420, with the next frame of the
 * reference decoder's output (8-bit I420 in output order).
 *
 *   conform [--aso] BITSTREAM REFERENCE [FRAMES]
 *
 * --aso gives the slices of each picture in the reverse order (arbitrary
 * slice order, 7.4.1.2.3; the decoded picture does not depend on the
 * order of its slices, so the reference stays right).  It is meant for
 * Baseline bitstreams whose pictures have several slices in order; a
 * picture's slices are the slice NAL units from one whose
 * first_mb_in_slice is 0 to the next such one or the next NAL unit of
 * another type.
 *
 * FRAMES, when given, is the number of frames the bitstream gives when
 * the reference holds more (a reference made from a longer bitstream):
 * the frames after it are not compared.
 *
 * Prints for each output frame
 *   "frame <n> <width>x<height> <md5 ours> <md5 reference> ok"
 * or, when they differ, "DIFF" with the first differing sample (plane,
 * x, y, its macroblock address) and the count of differing macroblocks.
 * When the decoder refuses a NAL unit as unsupported (ENOTSUP), prints
 * "unsupported <reason>" once per reason, the reason read here from the
 * NAL unit the decoder refused: data-partitioning, interlace (an SPS with
 * frame_mbs_only_flag 0), format (4:0:0, 4:2:2, 4:4:4, bit depth above
 * 8), lossless, size, profile-<profile_idc>, slice-groups, sp-slices, or
 * other.
 *
 * The last line is one of
 *   "conform: PASS (<n> frames)"      every frame equal, none missing
 *   "conform: OUT (<reasons>)"        the decoder refused the stream
 *   "conform: FAIL (<what>)"          a difference, a missing or an extra
 *                                     frame, or an error of the decoder
 */

#include "userland/desktop/libmedia/h264sw-decoder.h"
#include "userland/desktop/libmedia/h264sw-nal.h"
#include "userland/desktop/libmedia/vcodec-bitreader.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The largest bitstream read. */
#define CONFORM_MAX_FILE (1024UL * 1024UL * 1024UL)

/* The size the bitstream buffer grows by while the file is read. */
#define CONFORM_READ_STEP (16UL * 1024UL * 1024UL)

/* The nal_unit_type bits of the first byte of a NAL unit. */
#define CONFORM_NAL_TYPE_MASK 0x1fU

/* The most distinct reasons of refusal kept for the last line. */
#define CONFORM_MAX_REASONS 8U

/* The longest reason text. */
#define CONFORM_REASON_SIZE 32U

/* The most frames whose differences are printed in detail. */
#define CONFORM_MAX_DIFF_DETAILS 8U

/* The most slices of one picture --aso reorders; a picture with more is given in order. */
#define CONFORM_MAX_SLICES 1024U

/*
 * The state of an MD5 computation (RFC 1321): the four words, the bytes
 * waiting for a whole block, and the length so far.
 */
struct conform_md5 {
	uint32_t state[4];
	uint8_t block[64];
	unsigned used;
	uint64_t length;
};

/*
 * One run: the reference file and the frame buffer read from it, the
 * frames compared and their results, the number of frames expected
 * when the reference holds more (limit, 0 when it does not), the
 * pictures whose slices --aso gave in reverse (reordered), the reasons of refusal seen, and
 * the decoder's errors other than ENOTSUP.  Lives for one run of main.
 */
struct conform_run {
	FILE *reference;
	uint8_t *expected;
	size_t expected_capacity;
	unsigned frames;
	unsigned equal;
	unsigned different;
	unsigned missing;
	unsigned details;
	unsigned errors;
	unsigned limit;
	unsigned reordered;
	char reason[CONFORM_MAX_REASONS][CONFORM_REASON_SIZE];
	unsigned reasons;
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

/*
 * The profile_idc values whose SPS carries chroma_format_idc, the bit
 * depths and the scaling matrix (7.3.2.1.1).
 */
static const unsigned conform_format_profiles[] = {
	100U, 110U, 122U, 244U, 44U, 83U, 86U, 118U, 128U, 138U, 139U, 134U, 135U
};

static int md5_setup(void);
static void md5_init(struct conform_md5 *md5);
static void md5_block(struct conform_md5 *md5, const uint8_t *block);
static void md5_update(struct conform_md5 *md5, const uint8_t *data, size_t size);
static void md5_text(struct conform_md5 *md5, char *text);
static uint8_t *read_file(const char *path, size_t *size);
static void frame_md5(const struct h264sw_frame *frame, char *text);
static void compare_frame(struct conform_run *run, const struct h264sw_frame *frame);
static void describe_difference(struct conform_run *run, const struct h264sw_frame *frame);
static void take_output(struct h264sw_decoder *decoder, struct conform_run *run);
static void give(struct h264sw_decoder *decoder, struct conform_run *run, const uint8_t *data, const uint8_t *nal, size_t size);
static void give_reversed(struct h264sw_decoder *decoder, struct conform_run *run, const uint8_t *data, const uint8_t **slice, const size_t *slice_size, unsigned count);
static void add_reason(struct conform_run *run, const char *reason);
static void skip_scaling_lists(struct vcodec_bitreader *bits, unsigned count);
static void sps_reason(const struct h264sw_nal *unit, char *reason);
static void slice_reason(const struct h264sw_nal *unit, char *reason);
static void refusal_reason(struct conform_run *run, const uint8_t *nal, size_t size);
static int finish(struct conform_run *run, const struct h264sw_decoder_stats *stats);

/*
 * Decodes the bitstream and compares its frames with the reference.
 */
int
main(
	int argc,
	char **argv)
{
	static struct conform_run run;
	static const uint8_t *slice[CONFORM_MAX_SLICES];
	static size_t slice_size[CONFORM_MAX_SLICES];
	struct h264sw_decoder *decoder;
	struct h264sw_decoder_stats stats;
	const uint8_t *nal;
	uint8_t *data;
	size_t nal_size;
	size_t size;
	size_t cursor;
	unsigned slices;
	unsigned type;
	int aso;
	int difference;
	int first;
	int found;
	int status;
	int error;

	/* Takes --aso, the two paths and the optional frame count. */
	aso = 0;
	if (argc >= 2) {
		difference = strcmp(argv[1], "--aso");
		if (difference == 0) {
			aso = 1;
			argc--;
			argv++;
		}
	}

	/* Refuses a command line without the bitstream and the reference. */
	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: conform [--aso] BITSTREAM REFERENCE [FRAMES]\n");
		return 2;
	}

	/* An optional frame limit stops the comparison after that many frames. */
	if (argc == 4)
		run.limit = (unsigned)strtoul(argv[3], NULL, 10);

	/* Checks the MD5 against its known answers before trusting it. */
	status = md5_setup();
	if (status != 0) {
		printf("conform: FAIL (md5 self-test)\n");
		return 1;
	}

	/* Reads the bitstream and opens the reference. */
	data = read_file(argv[1], &size);
	if (data == NULL)
		return 1;
	run.reference = fopen(argv[2], "rb");
	if (run.reference == NULL) {
		free(data);
		printf("conform: FAIL (cannot open %s)\n", argv[2]);
		return 1;
	}

	/* Makes the decoder. */
	error = h264sw_decoder_create(&decoder);
	if (error != 0) {
		fclose(run.reference);
		free(data);
		printf("conform: FAIL (decoder create %d)\n", error);
		return 1;
	}

	/* Gives every NAL unit; with --aso the slices of a picture are held and given in reverse. */
	cursor = 0U;
	slices = 0U;
	for (;;) {
		found = vcodec_nal_find(data, size, &cursor, &nal, &nal_size);
		if (!found)
			break;

		/* Without --aso, every NAL unit goes as it comes. */
		if (aso == 0) {
			give(decoder, &run, data, nal, nal_size);
			continue;
		}

		/* A slice whose first_mb_in_slice is 0 (its first RBSP bit 1, ue(v) of 0) starts a picture: the held one goes first. */
		type = nal[0] & CONFORM_NAL_TYPE_MASK;
		if (type == H264SW_NAL_SLICE ||
		    type == H264SW_NAL_IDR) {
			first = 0;
			if (nal_size >= 2U && (nal[1] & 0x80U) != 0U)
				first = 1;
			if (first != 0 ||
			    slices == CONFORM_MAX_SLICES) {
				give_reversed(decoder, &run, data, slice, slice_size, slices);
				slices = 0U;
			}

			/* Keeps the slice of this picture for the group the reversed order needs. */
			slice[slices] = nal;
			slice_size[slices] = nal_size;
			slices++;
			continue;
		}

		/* Any other NAL unit ends the held picture and goes after it. */
		give_reversed(decoder, &run, data, slice, slice_size, slices);
		slices = 0U;
		give(decoder, &run, data, nal, nal_size);
	}

	/* Gives the picture held at the end and says how many pictures had their slices reversed. */
	give_reversed(decoder, &run, data, slice, slice_size, slices);
	if (aso != 0)
		printf("aso pictures %u\n", run.reordered);

	/* Takes what the last NAL unit output (the flush needs an empty queue), flushes and takes the last frames. */
	take_output(decoder, &run);
	error = h264sw_decoder_flush(decoder);
	if (error != 0) {
		printf("error %d at the flush\n", error);
		run.errors++;
	}

	/* Takes the frames the flush released. */
	take_output(decoder, &run);

	/* Keeps the counters and frees everything but the run. */
	stats = decoder->stats;
	h264sw_decoder_destroy(decoder);
	free(data);

	/* Judges the run. */
	status = finish(&run, &stats);
	fclose(run.reference);
	free(run.expected);
	return status;
}

/*
 * Fills the MD5 constants and checks the MD5 of two known strings (RFC 1321, A.5).
 */
static int
md5_setup(void)
{
	struct conform_md5 md5;
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
	struct conform_md5 *md5)
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
	struct conform_md5 *md5,
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
	struct conform_md5 *md5,
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
	struct conform_md5 *md5,
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
 * Reads a whole file into memory, growing the buffer as it goes.
 */
static uint8_t *
read_file(
	const char *path,
	size_t *size)
{
	FILE *file;
	uint8_t *data;
	uint8_t *grown;
	size_t length;
	size_t capacity;
	size_t got;

	/* Opens the file. */
	file = fopen(path, "rb");
	if (file == NULL) {
		printf("conform: FAIL (cannot open %s)\n", path);
		return NULL;
	}

	/* Reads step by step up to the limit; a file at the limit is refused. */
	data = NULL;
	length = 0U;
	capacity = 0U;
	for (;;) {
		/* Grows the buffer by one step when it is full. */
		if (length == capacity) {
			if (capacity >= CONFORM_MAX_FILE) {
				free(data);
				fclose(file);
				printf("conform: FAIL (size of %s)\n", path);
				return NULL;
			}

			/* Makes room for the next block of the file. */
			grown = realloc(data, capacity + CONFORM_READ_STEP);
			if (grown == NULL) {
				free(data);
				fclose(file);
				printf("conform: FAIL (no memory)\n");
				return NULL;
			}

			/* Takes the larger buffer. */
			data = grown;
			capacity += CONFORM_READ_STEP;
		}

		/* Reads what fits; nothing read is the end of the file. */
		got = fread(data + length, 1U, capacity - length, file);
		if (got == 0U)
			break;
		length += got;
	}

	/* Closes the file; its bytes are all in memory. */
	fclose(file);

	/* An empty file is not a bitstream. */
	if (length == 0U) {
		free(data);
		printf("conform: FAIL (%s is empty)\n", path);
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
	struct conform_md5 md5;
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
 * Reads the next reference frame of the output frame's size and compares the two.
 *
 * A frame the reference has no frame for is counted missing.
 */
static void
compare_frame(
	struct conform_run *run,
	const struct h264sw_frame *frame)
{
	struct conform_md5 md5;
	char ours[33];
	char theirs[33];
	uint8_t *grown;
	size_t frame_size;
	size_t got;
	int difference;

	/* Sizes the reference buffer for one cropped I420 frame. */
	frame_size = (size_t)frame->crop_width * frame->crop_height * 3U / 2U;
	if (frame_size > run->expected_capacity) {
		grown = realloc(run->expected, frame_size);
		if (grown == NULL) {
			printf("frame %u: no memory for the reference\n", run->frames);
			run->errors++;
			return;
		}

		/* Keeps the larger reference buffer. */
		run->expected = grown;
		run->expected_capacity = frame_size;
	}

	/* Reads the reference frame; a short read means the reference has fewer frames. */
	got = fread(run->expected, 1U, frame_size, run->reference);
	if (got != frame_size) {
		printf("frame %u %ux%u: not in the reference (%lu bytes left)\n", run->frames, frame->crop_width,
		       frame->crop_height, (unsigned long)got);
		run->missing++;
		return;
	}

	/* Hashes both frames. */
	frame_md5(frame, ours);
	md5_init(&md5);
	md5_update(&md5, run->expected, frame_size);
	md5_text(&md5, theirs);

	/* Prints the result; a difference is described for the first few frames. */
	difference = strcmp(ours, theirs);
	if (difference == 0) {
		printf("frame %u %ux%u %s %s ok\n", run->frames, frame->crop_width, frame->crop_height, ours, theirs);
		run->equal++;
	} else {
		printf("frame %u %ux%u %s %s DIFF poc %d frame_num %u idr %d concealed %u", run->frames,
		       frame->crop_width, frame->crop_height, ours, theirs, (int)frame->poc,
		       (unsigned)frame->frame_num, frame->idr, frame->concealed);
		describe_difference(run, frame);
		run->different++;
	}
}

/*
 * Prints where an output frame first differs from its reference frame and how many macroblocks differ.
 */
static void
describe_difference(
	struct conform_run *run,
	const struct h264sw_frame *frame)
{
	static uint8_t marks[H264SW_MAX_FRAME_MBS];
	const uint8_t *expected;
	const uint8_t *ours;
	unsigned plane;
	unsigned shift;
	unsigned width;
	unsigned height;
	unsigned x;
	unsigned y;
	unsigned mb;
	unsigned width_mbs;
	unsigned marked;
	int first;

	/* Only the first frames get the details: the rest of a broken stream adds nothing. */
	if (run->details >= CONFORM_MAX_DIFF_DETAILS) {
		printf("\n");
		return;
	}

	/* Counts the frame whose difference is described. */
	run->details++;

	/* Compares every sample, marking the macroblock (in the frame, before the crop) of each difference. */
	width_mbs = frame->width / 16U;
	memset(marks, 0, sizeof(marks));
	marked = 0U;
	first = 1;
	expected = run->expected;
	for (plane = 0U;
	     plane < 3U;
	     plane++) {
		/* The plane's window and its sampling. */
		shift = 0U;
		if (plane != 0U)
			shift = 1U;
		width = frame->crop_width >> shift;
		height = frame->crop_height >> shift;

		/* Walks the window's rows and samples. */
		for (y = 0U;
		     y < height;
		     y++) {
			ours = frame->plane[plane] + (size_t)((frame->crop_y >> shift) + y) * frame->stride[plane] +
				(frame->crop_x >> shift);

			/* Compares the samples of one row. */
			for (x = 0U;
			     x < width;
			     x++) {
				/* An equal sample needs nothing. */
				if (ours[x] == expected[x])
					continue;

				/* Notes the first difference and marks the macroblock once. */
				mb = (((frame->crop_y >> shift) + y) >> (4U - shift)) * width_mbs +
					(((frame->crop_x >> shift) + x) >> (4U - shift));
				if (first) {
					printf(" first plane %u x %u y %u mb %u ours %u ref %u", plane, x, y, mb,
					       (unsigned)ours[x], (unsigned)expected[x]);
					first = 0;
				}

				/* Marks the macroblock the differing sample lies in, once. */
				if (mb < H264SW_MAX_FRAME_MBS && marks[mb] == 0U) {
					marks[mb] = 1U;
					marked++;
				}
			}

			/* Moves to the next row of the reference. */
			expected += width;
		}
	}

	/* Prints how many macroblocks differ. */
	printf(" mbs %u\n", marked);
}

/*
 * Takes every frame the decoder has output and compares each with the reference.
 */
static void
take_output(
	struct h264sw_decoder *decoder,
	struct conform_run *run)
{
	struct h264sw_frame *frame;

	/* Takes frames until none is ready. */
	for (;;) {
		frame = h264sw_decoder_output(decoder);
		if (frame == NULL)
			break;

		/* Compares it, then the caller's reference goes. */
		compare_frame(run, frame);
		run->frames++;
		h264sw_frame_unref(frame);
	}
}

/*
 * Gives one NAL unit to the decoder, taking the output before it so that the decoder is never busy.
 */
static void
give(
	struct h264sw_decoder *decoder,
	struct conform_run *run,
	const uint8_t *data,
	const uint8_t *nal,
	size_t size)
{
	int error;

	/* Empties the output queue and decodes the NAL unit. */
	take_output(decoder, run);
	error = h264sw_decoder_nal(decoder, nal, size, (int64_t)(nal - data));
	if (error == ENOTSUP) {
		/* A refusal as unsupported puts the stream out of the range: its reason is recorded. */
		refusal_reason(run, nal, size);
	} else if (error != 0) {
		/* A conformance stream is valid: any other refusal is the decoder's mistake. */
		printf("error %d at offset %lu (nal_unit_type %u)\n", error, (unsigned long)(nal - data),
		       (unsigned)(nal[0] & CONFORM_NAL_TYPE_MASK));
		run->errors++;
	}
}

/*
 * Gives the held slices of one picture to the decoder, the last first; a picture of several slices is counted as reordered.
 */
static void
give_reversed(
	struct h264sw_decoder *decoder,
	struct conform_run *run,
	const uint8_t *data,
	const uint8_t **slice,
	const size_t *slice_size,
	unsigned count)
{
	unsigned index;

	/* Counts a picture whose order changes. */
	if (count > 1U)
		run->reordered++;

	/* Gives the slices from the last to the first. */
	for (index = count;
	     index > 0U;
	     index--)
		give(decoder, run, data, slice[index - 1U], slice_size[index - 1U]);
}

/*
 * Records a reason of refusal once.
 */
static void
add_reason(
	struct conform_run *run,
	const char *reason)
{
	unsigned index;
	int difference;

	/* A reason already seen is not repeated. */
	for (index = 0U;
	     index < run->reasons;
	     index++) {
		difference = strcmp(run->reason[index], reason);
		if (difference == 0)
			return;
	}

	/* Keeps and prints a new reason while there is room. */
	if (run->reasons >= CONFORM_MAX_REASONS)
		return;
	strncpy(run->reason[run->reasons], reason, CONFORM_REASON_SIZE - 1U);
	run->reason[run->reasons][CONFORM_REASON_SIZE - 1U] = '\0';
	run->reasons++;
	printf("unsupported %s\n", reason);
}

/*
 * Skips the scaling lists of an SPS (7.3.2.1.1.1): count lists, the first six of 16 and the rest of 64 coefficients.
 */
static void
skip_scaling_lists(
	struct vcodec_bitreader *bits,
	unsigned count)
{
	unsigned list;
	unsigned index;
	unsigned size;
	int32_t next;
	int32_t last;
	int32_t delta;
	uint32_t present;

	/* Reads each list's present flag and, when it is there, its deltas until next_scale is 0. */
	for (list = 0U;
	     list < count;
	     list++) {
		present = vcodec_bitreader_u1(bits);
		if (present == 0U)
			continue;
		size = 64U;
		if (list < 6U)
			size = 16U;
		last = 8;
		next = 8;

		/* Reads the deltas of one list; delta_scale is in -128..127 (7.4.2.1.1.1), clamped so a broken one cannot overflow. */
		for (index = 0U;
		     index < size;
		     index++) {
			if (next != 0) {
				delta = vcodec_bitreader_se(bits);
				if (delta < -128)
					delta = -128;
				if (delta > 127)
					delta = 127;
				next = (last + delta + 256) % 256;
			}

			/* A scale of zero ends the deltas of the list: the last nonzero scale stays in force for the rest. */
			if (next != 0)
				last = next;
		}
	}
}

/*
 * Finds why an SPS was refused (7.3.2.1.1): the first unsupported field.
 */
static void
sps_reason(
	const struct h264sw_nal *unit,
	char *reason)
{
	struct vcodec_bitreader bits;
	uint32_t profile;
	uint32_t chroma;
	uint32_t depth_luma;
	uint32_t depth_chroma;
	uint32_t lossless;
	uint32_t matrix;
	uint32_t poc_type;
	uint32_t count;
	uint32_t index;
	uint32_t frame_mbs_only;
	unsigned format_fields;
	unsigned taken;

	/* Reads profile_idc, skips the constraint flags, level_idc and the id. */
	vcodec_bitreader_init(&bits, unit->rbsp, unit->rbsp_size);
	profile = vcodec_bitreader_u(&bits, 8U);
	vcodec_bitreader_skip(&bits, 16U);
	(void)vcodec_bitreader_ue(&bits);

	/* Finds whether the profile carries the format fields. */
	format_fields = 0U;
	for (index = 0U;
	     index < sizeof(conform_format_profiles) / sizeof(conform_format_profiles[0]);
	     index++) {
		if (conform_format_profiles[index] == profile)
			format_fields = 1U;
	}

	/* The High family carries the format fields; the others imply 4:2:0 with 8 bits. */
	if (format_fields != 0U) {
		chroma = vcodec_bitreader_ue(&bits);
		if (chroma == 3U)
			(void)vcodec_bitreader_u1(&bits);
		depth_luma = vcodec_bitreader_ue(&bits);
		depth_chroma = vcodec_bitreader_ue(&bits);
		lossless = vcodec_bitreader_u1(&bits);
		if (chroma != 1U ||
		    depth_luma != 0U ||
		    depth_chroma != 0U) {
			strcpy(reason, "format");
			return;
		}

		/* Refuses a lossless profile. */
		if (lossless != 0U) {
			strcpy(reason, "lossless");
			return;
		}

		/* A stream with a scaling matrix has its lists skipped. */
		matrix = vcodec_bitreader_u1(&bits);
		if (matrix != 0U)
			skip_scaling_lists(&bits, 8U);
	}

	/* Skips log2_max_frame_num_minus4 and the order count fields of its type (7.3.2.1.1). */
	(void)vcodec_bitreader_ue(&bits);
	poc_type = vcodec_bitreader_ue(&bits);
	if (poc_type == 0U) {
		(void)vcodec_bitreader_ue(&bits);
	} else if (poc_type == 1U) {
		(void)vcodec_bitreader_u1(&bits);
		(void)vcodec_bitreader_se(&bits);
		(void)vcodec_bitreader_se(&bits);
		count = vcodec_bitreader_ue(&bits);
		for (index = 0U;
		     index < count && index < 256U;
		     index++)
			(void)vcodec_bitreader_se(&bits);
	}

	/* Skips max_num_ref_frames, the gaps flag and the size, then reads frame_mbs_only_flag. */
	(void)vcodec_bitreader_ue(&bits);
	(void)vcodec_bitreader_u1(&bits);
	(void)vcodec_bitreader_ue(&bits);
	(void)vcodec_bitreader_ue(&bits);
	frame_mbs_only = vcodec_bitreader_u1(&bits);
	if (bits.error == 0 && frame_mbs_only == 0U) {
		strcpy(reason, "interlace");
		return;
	}

	/* A taken profile (Baseline, Main, the High family) that got this far was refused for its size. */
	taken = 0U;
	if (profile == 66U ||
	    profile == 77U ||
	    profile == 100U)
		taken = 1U;
	if (profile == 110U ||
	    profile == 122U ||
	    profile == 244U)
		taken = 1U;
	if (taken != 0U) {
		strcpy(reason, "size");
		return;
	}

	/* Any other profile (Extended without constraint_set0 or 1, SVC, MVC) is refused for itself. */
	sprintf(reason, "profile-%u", (unsigned)profile);
}

/*
 * Finds why a slice was refused (7.3.3): an SP or SI slice, else other.
 */
static void
slice_reason(
	const struct h264sw_nal *unit,
	char *reason)
{
	struct vcodec_bitreader bits;
	uint32_t slice_type;

	/* Reads first_mb_in_slice and slice_type; types 3, 4, 8 and 9 are SP and SI (Table 7-6). */
	vcodec_bitreader_init(&bits, unit->rbsp, unit->rbsp_size);
	(void)vcodec_bitreader_ue(&bits);
	slice_type = vcodec_bitreader_ue(&bits) % 5U;
	if (bits.error == 0 &&
	    (slice_type == 3U ||
	     slice_type == 4U)) {
		strcpy(reason, "sp-slices");
		return;
	}

	/* Any other reason. */
	strcpy(reason, "other");
}

/*
 * Records why the decoder refused a NAL unit as unsupported.
 */
static void
refusal_reason(
	struct conform_run *run,
	const uint8_t *nal,
	size_t size)
{
	static uint8_t buffer[CONFORM_MAX_FILE / 64U];
	struct h264sw_nal unit;
	char reason[CONFORM_REASON_SIZE];
	int error;

	/* Reads the NAL unit's header and RBSP again (a NAL unit too large for the buffer is "other"). */
	strcpy(reason, "other");
	if (size > sizeof(buffer)) {
		add_reason(run, reason);
		return;
	}

	/* Parses the NAL unit; a unit that cannot be parsed has the reason found so far. */
	error = h264sw_nal_parse(nal, size, buffer, sizeof(buffer), &unit);
	if (error != 0) {
		add_reason(run, reason);
		return;
	}

	/* Names the reason from the kind of NAL unit. */
	if (unit.type == H264SW_NAL_PARTITION_A ||
	    unit.type == H264SW_NAL_PARTITION_B ||
	    unit.type == H264SW_NAL_PARTITION_C) {
		strcpy(reason, "data-partitioning");
	} else if (unit.type == H264SW_NAL_SPS) {
		sps_reason(&unit, reason);
	} else if (unit.type == H264SW_NAL_PPS) {
		strcpy(reason, "slice-groups");
	} else if (unit.type == H264SW_NAL_SLICE || unit.type == H264SW_NAL_IDR) {
		slice_reason(&unit, reason);
	}

	/* Records the reason the unit lies outside the decoder's range, if any. */
	add_reason(run, reason);
}

/*
 * Prints the counters and the last line, and gives the exit status.
 */
static int
finish(
	struct conform_run *run,
	const struct h264sw_decoder_stats *stats)
{
	uint8_t byte;
	size_t got;
	unsigned extra;
	unsigned index;
	int failed;
	const char *reference_state;

	/* Counts the reference frames left over: any byte left is a frame the decoder did not output, unless a limit says the reference is longer. */
	extra = 0U;
	got = fread(&byte, 1U, 1U, run->reference);
	if (got == 1U)
		extra = 1U;
	if (run->limit != 0U) {
		extra = 0U;
		if (run->frames != run->limit)
			extra = 1U;
	}

	/* Prints the counters. */
	printf("stats frames %u equal %u different %u missing %u reference_left %u errors %u pictures %lu concealed %lu "
	       "slice_errors %lu dropped %lu unsupported %lu\n",
	       run->frames, run->equal, run->different, run->missing, extra, run->errors,
	       (unsigned long)stats->pictures, (unsigned long)stats->concealed_mbs,
	       (unsigned long)stats->slice_errors, (unsigned long)stats->dropped,
	       (unsigned long)stats->unsupported);

	/* A refused stream is out of the decoder's range, whatever it output. */
	if (run->reasons != 0U) {
		printf("conform: OUT (");
		for (index = 0U;
		     index < run->reasons;
		     index++) {
			/* The reasons are separated by commas. */
			if (index != 0U)
				printf(",");
			printf("%s", run->reason[index]);
		}

		/* Closes the list of reasons. */
		printf(")\n");
		return 3;
	}

	/* Every frame equal, none missing or left over, no error: the stream conforms. */
	failed = 0;
	if (run->different != 0U || run->missing != 0U)
		failed = 1;
	if (extra != 0U ||
	    run->errors != 0U ||
	    run->frames == 0U)
		failed = 1;
	if (failed == 0) {
		printf("conform: PASS (%u frames)\n", run->frames);
		return 0;
	}

	/* Anything else fails. */
	reference_state = "ended";
	if (extra != 0U)
		reference_state = "has more frames";
	printf("conform: FAIL (%u of %u frames differ, %u not in the reference, reference %s, %u errors)\n",
	       run->different, run->frames, run->missing, reference_state,
	       run->errors);
	return 1;
}
