/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The WebP container: the RIFF header, the VP8X chunk, the optional chunks
 * and the bitstream chunk.
 */

#include "riff.h"

#include <errno.h>

/* The size of the RIFF header: "RIFF", File Size and "WEBP" (RFC 9649 2.4). */
#define RIFF_HEADER_SIZE		12U

/* The size of a chunk's header: its FourCC and its payload size (RFC 9649 2.3). */
#define CHUNK_HEADER_SIZE		8U

/* The payload size of the VP8X chunk (RFC 9649 2.7, Figure 7). */
#define VP8X_PAYLOAD_SIZE		10U

/*
 * What the chunks of an extended file add up to before any optional chunk:
 * "WEBP" (4 bytes) and the whole VP8X chunk (8 plus 10 bytes).
 */
#define EXTENDED_BASE_SIZE		22U

/* The bytes a lossy frame's header takes: the tag, the start code and the size (RFC 6386 9.1). */
#define VP8_HEADER_SIZE			10U

/* The bytes a lossless stream's header takes: the signature and the 32 bits after it (RFC 9649 3.4). */
#define VP8L_HEADER_SIZE		5U

/* The signature byte that starts a lossless stream (RFC 9649 3.4). */
#define VP8L_SIGNATURE			0x2fU

/*
 * Where the parse stands: the bytes it may use and how far it has gone.
 *
 * It lives in the one call of webpc_riff_parse that makes it.
 *
 *   data       The caller's bytes.
 *   size       How many of them the parse may use: the data's size, cut
 *              to File Size + 8 when the RIFF header says the file ends
 *              before the data does.
 *   pos        The next byte to read, never past size.
 *   have_riff  1 when the data starts with a RIFF header.
 *   riff_size  The File Size field (valid when have_riff is 1).
 *   complete   1 when the data is the whole file.
 */
struct riff_state {
	const uint8_t *data;
	size_t size;
	size_t pos;
	int have_riff;
	uint32_t riff_size;
	int complete;
};

static int tag_is(const uint8_t *bytes, const char *tag);
static int parse_riff_header(struct riff_state *state);
static int parse_vp8x(struct riff_state *state, struct webpc_container *out);
static int parse_optional_chunks(struct riff_state *state, struct webpc_container *out);
static int parse_bitstream_chunk(struct riff_state *state, struct webpc_container *out, size_t *chunk_size);
static int read_lossy_header(const struct riff_state *state, size_t chunk_size, unsigned *width, unsigned *height);
static int read_lossless_header(const struct riff_state *state, unsigned *width, unsigned *height, int *has_alpha);
static int soften(int error, const struct riff_state *state, struct webpc_container *out);

/*
 * Parses a file's container.
 */
int
webpc_riff_parse(
	const uint8_t *data,
	size_t size,
	int complete,
	struct webpc_container *out)
{
	struct riff_state state;
	size_t chunk_size;
	unsigned width;
	unsigned height;
	int has_alpha;
	int error;

	/* Clears the answer so every field has a meaning on every path. */
	out->format = WEBPC_FORMAT_UNDEFINED;
	out->width = 0;
	out->height = 0;
	out->has_alpha = 0;
	out->has_animation = 0;
	out->extended = 0;
	out->bitstream = NULL;
	out->bitstream_size = 0;
	out->available_size = 0;
	out->alpha = NULL;
	out->alpha_size = 0;

	/* Starts the parse at the first byte, with all the data usable. */
	state.data = data;
	state.size = size;
	state.pos = 0;
	state.have_riff = 0;
	state.riff_size = 0;
	state.complete = complete;

	/* Step 1: a file shorter than a RIFF header cannot be read, whatever it is. */
	if (size < RIFF_HEADER_SIZE)
		return ENOBUFS;

	/* Step 2: the RIFF header, when there is one. */
	error = parse_riff_header(&state);
	if (error != 0)
		return error;

	/* Step 3: the VP8X chunk, when there is one. */
	error = parse_vp8x(&state, out);
	if (error != 0)
		return error;

	/* An animation is not read further: the decoding layer refuses it. */
	if (out->has_animation != 0)
		return 0;

	/* Step 4: a tag has to follow; a file still arriving is reported by its VP8X chunk. */
	if (state.size - state.pos < 4U) {
		error = soften(ENOBUFS, &state, out);
		return error;
	}

	/* Step 5: the chunks of an extended file that come before the bitstream. */
	if (out->extended != 0) {
		error = parse_optional_chunks(&state, out);
		if (error != 0) {
			error = soften(error, &state, out);
			return error;
		}
	}

	/* Step 6: the bitstream chunk, or a bare bitstream. */
	error = parse_bitstream_chunk(&state, out, &chunk_size);
	if (error != 0) {
		error = soften(error, &state, out);
		return error;
	}

	/* Step 7: the bitstream's own header gives the size of the image. */
	width = 0;
	height = 0;
	has_alpha = out->has_alpha;
	if (out->format == WEBPC_FORMAT_LOSSY)
		error = read_lossy_header(&state, chunk_size, &width, &height);
	else
		error = read_lossless_header(&state, &width, &height, &has_alpha);
	if (error != 0) {
		error = soften(error, &state, out);
		return error;
	}

	/* Step 8: the canvas of an extended file is the bitstream's size. */
	if (out->extended != 0) {
		/* The canvas must be exactly as wide and as high as the bitstream's image. */
		if (out->width != width)
			return EINVAL;
		if (out->height != height)
			return EINVAL;
	} else {
		out->width = width;
		out->height = height;
	}

	/* A lossless stream's alpha_is_used bit replaces the VP8X flag. */
	if (out->format == WEBPC_FORMAT_LOSSLESS)
		out->has_alpha = has_alpha;

	/* An ALPH chunk makes the image have alpha whatever the flags said. */
	if (out->alpha != NULL)
		out->has_alpha = 1;

	/* Hands over where the bitstream is and how much of it the data holds. */
	out->bitstream = state.data + state.pos;
	out->bitstream_size = chunk_size;
	out->available_size = state.size - state.pos;

	/* Succeeded: the container's parts are found. */
	return 0;
}

/* Tells whether four bytes are a tag's four characters. */
static int
tag_is(
	const uint8_t *bytes,
	const char *tag)
{
	unsigned index;

	/* Compares the four characters one by one. */
	for (index = 0; index < 4U; index++) {
		if (bytes[index] != (uint8_t)tag[index])
			return 0;
	}

	/* All four characters matched. */
	return 1;
}

/*
 * Reads the RIFF header when the data starts with "RIFF" (step 2): the
 * "WEBP" form type, File Size, and the cut of the usable data to File Size
 * plus 8.  The caller has checked that 12 bytes are there.
 *
 * Values referenced: RFC 9649 2.4 (the RIFF header: "RIFF", File Size as a
 * 32-bit little-endian value, "WEBP"; File Size covers everything after
 * its own field, so the file is File Size + 8 bytes long, and bytes after
 * that are not part of the file).
 *
 * Procedure referenced: libwebp 1.5.0 src/dec/webp_dec.c ParseHeadersInternal
 * and the parsers it calls (BSD-3-Clause), the order of the checks and the
 * statuses.
 */
static int
parse_riff_header(
	struct riff_state *state)
{
	uint64_t whole_size;
	uint32_t riff_size;
	int is_riff;
	int is_webp;

	/* Data without the RIFF tag is parsed from its first byte. */
	is_riff = tag_is(state->data, "RIFF");
	if (!is_riff)
		return 0;

	/* The form type must be "WEBP". */
	is_webp = tag_is(state->data + 8, "WEBP");
	if (!is_webp)
		return EINVAL;

	/* File Size covers at least "WEBP" and one chunk header, and at most the format's limit. */
	riff_size = webpc_le32(state->data + 4);
	if (riff_size < RIFF_HEADER_SIZE)
		return EINVAL;
	if (riff_size > WEBPC_RIFF_SIZE_MAX)
		return EINVAL;

	/*
	 * The whole file is File Size + 8 bytes, computed wide so a File Size
	 * near 2^32 does not wrap.
	 */
	whole_size = (uint64_t)riff_size + 8U;

	/* Data meant to be the whole file must hold it; longer data is cut where the file ends. */
	if (whole_size > (uint64_t)state->size) {
		/* Refuses a file cut short when the data was meant to be all of it. */
		if (state->complete != 0)
			return ENOBUFS;
	} else {
		state->size = (size_t)whole_size;
	}

	/* Remembers the header and moves past it. */
	state->have_riff = 1;
	state->riff_size = riff_size;
	state->pos = RIFF_HEADER_SIZE;

	/* Succeeded: the parse continues after the RIFF header. */
	return 0;
}

/*
 * Reads the VP8X chunk when one is next (step 3) and fills the canvas
 * size, the flags and the extended marker of out.
 *
 * Values referenced: RFC 9649 2.7 Figure 7 (VP8X: the flag byte with
 * Animation 0x02, XMP 0x04, EXIF 0x08, Alpha 0x10, ICC 0x20; Canvas Width
 * Minus One and Canvas Height Minus One as 24-bit little-endian values at
 * payload bytes 4 and 7; the product of the canvas sides must fit in 32
 * bits).
 *
 * Procedure referenced: libwebp 1.5.0 src/dec/webp_dec.c ParseHeadersInternal
 * and the parsers it calls (BSD-3-Clause), the order of the checks and the
 * statuses.
 */
static int
parse_vp8x(
	struct riff_state *state,
	struct webpc_container *out)
{
	const uint8_t *chunk;
	uint64_t area;
	uint32_t payload_size;
	uint32_t canvas_width;
	uint32_t canvas_height;
	size_t left;
	int is_vp8x;

	/* A chunk header has to be there to tell what is next. */
	left = state->size - state->pos;
	if (left < CHUNK_HEADER_SIZE)
		return ENOBUFS;

	/* Anything but a VP8X chunk leaves the file a simple one. */
	chunk = state->data + state->pos;
	is_vp8x = tag_is(chunk, "VP8X");
	if (!is_vp8x)
		return 0;

	/* The VP8X payload has one size only. */
	payload_size = webpc_le32(chunk + 4);
	if (payload_size != VP8X_PAYLOAD_SIZE)
		return EINVAL;

	/* The whole chunk has to be there. */
	if (left < CHUNK_HEADER_SIZE + VP8X_PAYLOAD_SIZE)
		return ENOBUFS;

	/* The canvas sides are stored minus one. */
	canvas_width = webpc_le24(chunk + CHUNK_HEADER_SIZE + 4U) + 1U;
	canvas_height = webpc_le24(chunk + CHUNK_HEADER_SIZE + 7U) + 1U;

	/* The area is computed wide: two 24-bit sides overflow 32 bits. */
	area = (uint64_t)canvas_width * (uint64_t)canvas_height;
	if (area > (uint64_t)WEBPC_CANVAS_AREA_MAX)
		return EINVAL;

	/* A VP8X chunk is only valid inside a RIFF container. */
	if (!state->have_riff)
		return EINVAL;

	/* Records the canvas, the file being an extended one, and its flags. */
	out->extended = 1;
	out->width = canvas_width;
	out->height = canvas_height;

	/* The flag byte says whether the file has alpha and whether it is an animation. */
	if ((chunk[CHUNK_HEADER_SIZE] & WEBPC_VP8X_ALPHA) != 0U)
		out->has_alpha = 1;
	if ((chunk[CHUNK_HEADER_SIZE] & WEBPC_VP8X_ANIMATION) != 0U)
		out->has_animation = 1;

	/* Moves past the chunk. */
	state->pos += CHUNK_HEADER_SIZE + VP8X_PAYLOAD_SIZE;

	/* Succeeded: the file is an extended one. */
	return 0;
}

/*
 * Walks the chunks of an extended file that come before the bitstream
 * (step 5), keeping the last ALPH chunk, and stops at the bitstream chunk's
 * header.
 *
 * Values referenced: RFC 9649 2.3 (a chunk is a FourCC, a 32-bit
 * little-endian payload size, the payload, and one pad byte when the size
 * is odd) and 2.7.1.6 (a reader skips chunks it does not know).
 *
 * Procedure referenced: libwebp 1.5.0 src/dec/webp_dec.c ParseHeadersInternal
 * and the parsers it calls (BSD-3-Clause), the order of the checks and the
 * statuses.
 */
static int
parse_optional_chunks(
	struct riff_state *state,
	struct webpc_container *out)
{
	const uint8_t *chunk;
	uint64_t padded_size;
	uint64_t total;
	uint32_t payload_size;
	size_t left;
	int is_vp8;
	int is_vp8l;
	int is_alph;

	/* The chunks counted so far are "WEBP" and the VP8X chunk. */
	total = EXTENDED_BASE_SIZE;

	/* Visits each chunk until the bitstream's. */
	for (;;) {
		/* A chunk header has to be there. */
		left = state->size - state->pos;
		if (left < CHUNK_HEADER_SIZE)
			return ENOBUFS;

		/* The size field is bounded by the format's limit. */
		chunk = state->data + state->pos;
		payload_size = webpc_le32(chunk + 4);
		if (payload_size > WEBPC_CHUNK_SIZE_MAX)
			return EINVAL;

		/* The chunk takes its header, its payload and the pad byte of an odd size. */
		padded_size = ((uint64_t)CHUNK_HEADER_SIZE + (uint64_t)payload_size + 1U) & ~(uint64_t)1U;

		/* The chunks may not add up to more than File Size says. */
		total += padded_size;
		if (total > (uint64_t)state->riff_size)
			return EINVAL;

		/* The bitstream's chunk ends the walk, left for the next step. */
		is_vp8 = tag_is(chunk, "VP8 ");
		if (is_vp8)
			return 0;
		is_vp8l = tag_is(chunk, "VP8L");
		if (is_vp8l)
			return 0;

		/* Any other chunk has to be wholly there to be skipped. */
		if ((uint64_t)left < padded_size)
			return ENOBUFS;

		/* An ALPH chunk is kept; a later one replaces an earlier one. */
		is_alph = tag_is(chunk, "ALPH");
		if (is_alph) {
			out->alpha = chunk + CHUNK_HEADER_SIZE;
			out->alpha_size = (size_t)payload_size;
		}

		/* Moves past the chunk. */
		state->pos += (size_t)padded_size;
	}
}

/*
 * Reads the bitstream chunk's header or recognises a bare bitstream
 * (step 6), sets out's format, moves the position to the bitstream's first
 * byte and gives the bitstream's declared size.
 *
 * Values referenced: RFC 9649 2.5 (the simple lossy file: a "VP8 " chunk),
 * 2.6 (the simple lossless file: a "VP8L" chunk) and 3.4 (the lossless
 * stream starts with the signature byte 0x2f).
 *
 * Procedure referenced: libwebp 1.5.0 src/dec/webp_dec.c ParseHeadersInternal
 * and the parsers it calls (BSD-3-Clause), the order of the checks and the
 * statuses.
 */
static int
parse_bitstream_chunk(
	struct riff_state *state,
	struct webpc_container *out,
	size_t *chunk_size)
{
	const uint8_t *chunk;
	uint64_t room;
	uint32_t payload_size;
	size_t left;
	int is_vp8;
	int is_vp8l;

	/* A chunk header has to be there. */
	left = state->size - state->pos;
	if (left < CHUNK_HEADER_SIZE)
		return ENOBUFS;

	/* Tells which chunk is next, if it is a bitstream's. */
	chunk = state->data + state->pos;
	is_vp8 = tag_is(chunk, "VP8 ");
	is_vp8l = tag_is(chunk, "VP8L");

	/* A bare bitstream is not in a chunk. */
	if (!is_vp8 && !is_vp8l) {
		/*
		 * It is lossless when its first bytes are a lossless header's
		 * (the signature and a version field that fits), else lossy,
		 * and it runs to the end of the data.
		 */
		*chunk_size = left;
		out->format = WEBPC_FORMAT_LOSSY;

		/* Enough bytes for a lossless header, starting with the signature. */
		if (left >= VP8L_HEADER_SIZE) {
			if (chunk[0] == VP8L_SIGNATURE) {
				/* A version number of 0 makes it a lossless stream. */
				if ((chunk[4] >> 5) == 0U)
					out->format = WEBPC_FORMAT_LOSSLESS;
			}
		}

		/* Succeeded: the bitstream starts where the data stands. */
		return 0;
	}

	/* The chunk's declared size. */
	payload_size = webpc_le32(chunk + 4);

	/* With a RIFF header the chunk must fit in what File Size leaves after "WEBP". */
	if (state->have_riff) {
		room = (uint64_t)state->riff_size - RIFF_HEADER_SIZE;
		if ((uint64_t)payload_size > room)
			return EINVAL;
	}

	/* Data that is meant to be the whole file must hold the chunk. */
	if (state->complete != 0) {
		room = (uint64_t)(left - CHUNK_HEADER_SIZE);
		if ((uint64_t)payload_size > room)
			return ENOBUFS;
	}

	/* A size over the format's limit cannot be a real chunk. */
	if (payload_size > WEBPC_CHUNK_SIZE_MAX)
		return EINVAL;

	/* The bitstream starts after the chunk's header. */
	*chunk_size = (size_t)payload_size;
	state->pos += CHUNK_HEADER_SIZE;

	/* The chunk's FourCC tells the format. */
	if (is_vp8l)
		out->format = WEBPC_FORMAT_LOSSLESS;
	else
		out->format = WEBPC_FORMAT_LOSSY;

	/* Succeeded: the chunk is the bitstream's. */
	return 0;
}

/*
 * Checks a lossy frame's header and reads the image size from it.  The
 * header is at the position; the bytes the data holds (not the declared
 * chunk size) must reach 10.
 *
 * Values referenced: RFC 6386 9.1 (the 3-byte frame tag: key_frame bit 0 is
 * 0 for a key frame, version bits 1..3, show_frame bit 4, first_part_size
 * bits 5..23; the start code 9d 01 2a; the 16-bit width and height whose
 * low 14 bits are the size and high 2 bits the scale) and 19.1 (the same
 * fields).
 *
 * Procedure referenced: libwebp 1.5.0 src/dec/webp_dec.c ParseHeadersInternal
 * and the parsers it calls (BSD-3-Clause), the checks and their order.
 */
static int
read_lossy_header(
	const struct riff_state *state,
	size_t chunk_size,
	unsigned *width,
	unsigned *height)
{
	const uint8_t *frame;
	uint32_t tag;
	uint32_t partition_size;
	uint32_t frame_width;
	uint32_t frame_height;

	/* The tag, the start code and the sizes take 10 bytes. */
	if (state->size - state->pos < VP8_HEADER_SIZE)
		return ENOBUFS;

	/* The start code follows the tag. */
	frame = state->data + state->pos;
	if (frame[3] != 0x9dU)
		return EINVAL;
	if (frame[4] != 0x01U)
		return EINVAL;
	if (frame[5] != 0x2aU)
		return EINVAL;

	/* Reads the tag's fields and the sizes. */
	tag = webpc_le24(frame);
	partition_size = tag >> 5;
	frame_width = webpc_le16(frame + 6) & 0x3fffU;
	frame_height = webpc_le16(frame + 8) & 0x3fffU;

	/* Only a key frame starts an image. */
	if ((tag & 1U) != 0U)
		return EINVAL;

	/* Versions above 3 are not defined. */
	if (((tag >> 1) & 7U) > 3U)
		return EINVAL;

	/* A frame that is not to be shown is no image. */
	if (((tag >> 4) & 1U) == 0U)
		return EINVAL;

	/* The first partition has to fit in the chunk, with room for what follows. */
	if ((uint64_t)partition_size >= (uint64_t)chunk_size)
		return EINVAL;

	/* An image has a size. */
	if (frame_width == 0U)
		return EINVAL;
	if (frame_height == 0U)
		return EINVAL;

	/* Hands over the size. */
	*width = (unsigned)frame_width;
	*height = (unsigned)frame_height;

	/* Succeeded: the header is valid. */
	return 0;
}

/*
 * Checks a lossless stream's header and reads the image size and the
 * alpha_is_used bit from it.  The header is at the position; the bytes the
 * data holds must reach 5.
 *
 * Values referenced: RFC 9649 3.4 (signature 0x2f, then 14 bits of width
 * minus one, 14 bits of height minus one, 1 bit alpha_is_used, 3 bits
 * version_number, packed from the least significant bit; the version
 * must be 0).
 */
static int
read_lossless_header(
	const struct riff_state *state,
	unsigned *width,
	unsigned *height,
	int *has_alpha)
{
	const uint8_t *stream;
	uint32_t fields;
	uint32_t version;

	/* The signature and 32 bits of fields take 5 bytes. */
	if (state->size - state->pos < VP8L_HEADER_SIZE)
		return ENOBUFS;

	/* The first byte is the signature. */
	stream = state->data + state->pos;
	if (stream[0] != VP8L_SIGNATURE)
		return EINVAL;

	/* The next 32 bits hold the fields, the first one in the lowest bits. */
	fields = webpc_le32(stream + 1);

	/* The version number must be 0. */
	version = fields >> 29;
	if (version != 0U)
		return EINVAL;

	/* Hands over the size (stored minus one) and the alpha bit. */
	*width = (unsigned)(fields & 0x3fffU) + 1U;
	*height = (unsigned)((fields >> 14) & 0x3fffU) + 1U;
	*has_alpha = (int)((fields >> 28) & 1U);

	/* Succeeded: the header is valid. */
	return 0;
}

/*
 * Turns an "ends too soon" failure of an extended file that is still
 * arriving into a success that reports what is known so far; any other
 * failure stays one.
 *
 * Procedure referenced: libwebp 1.5.0 src/dec/webp_dec.c ParseHeadersInternal
 * (BSD-3-Clause), the status it gives for a VP8X file whose data ends
 * early when only the features are asked for.
 */
static int
soften(
	int error,
	const struct riff_state *state,
	struct webpc_container *out)
{
	/* Only a cut-short file, read for its features, is not a failure; every other failure is passed on. */
	if (error != ENOBUFS)
		return error;
	if (state->complete != 0)
		return error;

	/* Without a VP8X chunk nothing is known: it stays a failure. */
	if (out->extended == 0)
		return error;

	/* An ALPH chunk found so far makes the image have alpha. */
	if (out->alpha != NULL)
		out->has_alpha = 1;

	/* The bitstream was not reached, so it is not reported. */
	out->bitstream = NULL;
	out->bitstream_size = 0;
	out->available_size = 0;

	/* Succeeded: the canvas, the flags and what is known of the format are reported. */
	return 0;
}
