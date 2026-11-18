/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The image data of a GIF (GIF89a §22 and Appendix F): the LZW minimum
 * code size, then sub-blocks of variable-length codes packed from the
 * low bit up.  The codes start one bit longer than the minimum size and
 * grow a bit each time the table reaches the next power of two, up to 12
 * bits; a clear code starts the table again and the end code ends the
 * data.  Each code stands for a string of pixels; a new code is the
 * previous string followed by the first pixel of the next one.
 */

#include "internal.h"

#include <string.h>

static int gif_lzw_next_byte(GifFileType *gif);
static int gif_lzw_read_code(GifFileType *gif, int size);
static void gif_lzw_add(struct gif_compat_private *state, int next, int previous, int pixel);
static size_t gif_lzw_emit(const struct gif_compat_private *state, int code, GifByteType *out, size_t room);
static int gif_lzw_skip(GifFileType *gif);

/*
 * Decodes an image's data into count pixels (in the order the file gives
 * them) and reads the data to its end.  Returns D_GIF_SUCCEEDED, or the
 * error that stopped it (the pixels not reached stay as they were).
 */
int
gif_compat_decode_raster(
	GifFileType *gif,
	GifByteType *pixels,
	size_t count)
{
	struct gif_compat_private *state;
	GifByteType minimum;
	size_t done;
	int status;
	int clear;
	int end;
	int size;
	int next;
	int previous;
	int code;
	int index;
	int error;

	/* The minimum code size, which the root codes are one pixel each of. */
	state = gif->Private;
	status = gif_compat_read(gif, &minimum, 1);
	if (status != 0)
		return D_GIF_ERR_READ_FAILED;
	if (minimum < 1 || minimum > 8)
		return D_GIF_ERR_IMAGE_DEFECT;

	/* The root codes, and the clear and end codes after them. */
	clear = 1 << minimum;
	end = clear + 1;
	for (index = 0; index < clear; index++) {
		state->prefix[index] = 0;
		state->suffix[index] = (uint8_t)index;
		state->first[index] = (uint8_t)index;
		state->length[index] = 1;
	}

	/* No sub-block and no bits yet. */
	state->block_length = 0;
	state->block_position = 0;
	state->blocks_ended = 0;
	state->read_failed = 0;
	state->bits = 0;
	state->bit_count = 0;

	/* The codes, until the pixels are all there, the end code, or bad data. */
	size = minimum + 1;
	next = end + 1;
	previous = -1;
	done = 0;
	error = D_GIF_SUCCEEDED;
	while (done < count) {
		code = gif_lzw_read_code(gif, size);
		if (code < 0 && state->read_failed) {
			/* The file ended inside the data. */
			error = D_GIF_ERR_READ_FAILED;
			break;
		} else if (code < 0) {
			/* The data's sub-blocks ended before the pixels. */
			error = D_GIF_ERR_EOF_TOO_SOON;
			break;
		}

		/* A clear code starts the table again. */
		if (code == clear) {
			size = minimum + 1;
			next = end + 1;
			previous = -1;
			continue;
		}

		/* The end code before the last pixel: the image is short. */
		if (code == end) {
			error = D_GIF_ERR_EOF_TOO_SOON;
			break;
		}

		/* The first code after a clear is a root code, and adds nothing to the table. */
		if (previous < 0) {
			if (code >= clear) {
				error = D_GIF_ERR_IMAGE_DEFECT;
				break;
			}

			/* The code's pixels, and the next code to read. */
			done += gif_lzw_emit(state, code, pixels + done, count - done);
			previous = code;
			continue;
		}

		/* A known code: the previous string and this one's first pixel become a new code. */
		if (code < next) {
			if (next < GIF_LZW_CODES)
				gif_lzw_add(state, next, previous, state->first[code]);
		} else if (code == next && next < GIF_LZW_CODES) {
			/* The code being made: the previous string and its own first pixel. */
			gif_lzw_add(state, next, previous, state->first[previous]);
		} else {
			/* A code the table cannot have yet. */
			error = D_GIF_ERR_IMAGE_DEFECT;
			break;
		}

		/* The code's pixels, and one code more in the table. */
		if (next < GIF_LZW_CODES)
			next++;
		done += gif_lzw_emit(state, code, pixels + done, count - done);
		previous = code;

		/* A full width of codes makes them a bit longer, up to 12 bits. */
		if (next == (1 << size) && size < GIF_LZW_BITS)
			size++;
	}

	/* The rest of the data is not read as pixels. */
	status = gif_lzw_skip(gif);
	if (error == D_GIF_SUCCEEDED && status != 0)
		error = D_GIF_ERR_READ_FAILED;

	/* Reports why the data stopped short. */
	if (error != D_GIF_SUCCEEDED)
		return error;

	/* Succeeded: every pixel is decoded. */
	return D_GIF_SUCCEEDED;
}

/* Reads the next byte of the image data (-1 once the sub-blocks end). */
static int
gif_lzw_next_byte(
	GifFileType *gif)
{
	struct gif_compat_private *state;
	GifByteType length;
	int status;
	int byte;

	/* A new sub-block when this one is used up: its length, then its bytes (0 ends the data). */
	state = gif->Private;
	if (state->block_position == state->block_length) {
		if (state->blocks_ended)
			return -1;
		status = gif_compat_read(gif, &length, 1);
		if (status != 0) {
			state->blocks_ended = 1;
			state->read_failed = 1;
			return -1;
		}

		/* An empty sub-block ends the data. */
		if (length == 0) {
			state->blocks_ended = 1;
			return -1;
		}

		/* The sub-block's bytes. */
		status = gif_compat_read(gif, state->block, length);
		if (status != 0) {
			state->blocks_ended = 1;
			state->read_failed = 1;
			return -1;
		}

		/* The sub-block is read from its start. */
		state->block_length = length;
		state->block_position = 0;
	}

	/* Succeeded: the byte. */
	byte = state->block[state->block_position];
	state->block_position++;
	return byte;
}

/* Reads one code of size bits (-1 when the data ends first). */
static int
gif_lzw_read_code(
	GifFileType *gif,
	int size)
{
	struct gif_compat_private *state;
	int byte;
	int code;

	/* Bytes above the bits in hand until there are enough. */
	state = gif->Private;
	while (state->bit_count < size) {
		byte = gif_lzw_next_byte(gif);
		if (byte < 0)
			return -1;
		state->bits |= (uint32_t)byte << state->bit_count;
		state->bit_count += 8;
	}

	/* Succeeded: the lowest size bits. */
	code = (int)(state->bits & ((1U << size) - 1U));
	state->bits >>= size;
	state->bit_count -= size;
	return code;
}

/* Makes a new code: the previous code's string followed by one pixel. */
static void
gif_lzw_add(
	struct gif_compat_private *state,
	int next,
	int previous,
	int pixel)
{
	/* The string it extends, the pixel, its first pixel and its length. */
	state->prefix[next] = (uint16_t)previous;
	state->suffix[next] = (uint8_t)pixel;
	state->first[next] = state->first[previous];
	state->length[next] = (uint16_t)(state->length[previous] + 1U);
}

/*
 * Writes a code's string of pixels (as much of it as room allows, from
 * its start) and returns how many were written.
 */
static size_t
gif_lzw_emit(
	const struct gif_compat_private *state,
	int code,
	GifByteType *out,
	size_t room)
{
	size_t length;
	size_t kept;
	size_t position;

	/* The pixels that fit. */
	length = state->length[code];
	kept = length;
	if (kept > room)
		kept = room;

	/* The string from its last pixel back to its first, the ones past the room dropped. */
	position = length;
	while (position > 0) {
		position--;
		if (position < kept)
			out[position] = state->suffix[code];
		code = state->prefix[code];
	}

	/* Succeeded: the pixels written. */
	return kept;
}

/* Reads the image data's sub-blocks to the one that ends them; nonzero when the file ends first. */
static int
gif_lzw_skip(
	GifFileType *gif)
{
	struct gif_compat_private *state;
	GifByteType length;
	GifByteType discard[255];
	int status;

	/* Already at the end, or the file ended. */
	state = gif->Private;
	if (state->read_failed)
		return 1;
	if (state->blocks_ended)
		return 0;

	/* Each sub-block left, until the empty one. */
	for (;;) {
		status = gif_compat_read(gif, &length, 1);
		if (status != 0)
			return 1;
		if (length == 0)
			break;
		status = gif_compat_read(gif, discard, length);
		if (status != 0)
			return 1;
	}

	/* Succeeded: the data is over. */
	state->blocks_ended = 1;
	return 0;
}
