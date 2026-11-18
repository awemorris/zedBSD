/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * inflate: the decompression of libz-compat (ws071-p010, the decode half of
 * ws035-p040).  A deflate stream (RFC 1951) -- stored, fixed-Huffman and
 * dynamic-Huffman blocks -- in a zlib wrapper (RFC 1950, its Adler-32
 * checked) or raw.
 *
 * It is written to be read, not to be fast.  inflate keeps every byte of
 * input it is given; once the stream is whole it decodes it into an output
 * buffer of its own in one go and hands that out as the caller makes room.
 * A stream given a few bytes at a time is decoded again from its start at
 * each call until it is whole, which only costs time.
 */

#include <compat/zlib/zlib.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The longest Huffman code, and how many literal/length and distance codes there are. */
#define INFLATE_MAX_BITS	15
#define INFLATE_LITERALS	288
#define INFLATE_DISTANCES	30
#define INFLATE_CODE_LENGTHS	19

/* The results of one attempt at the whole stream. */
#define INFLATE_WHOLE		0
#define INFLATE_SHORT		1
#define INFLATE_BAD		2
#define INFLATE_NO_MEMORY	3

/*
 * The state of one stream: the wrapper expected, the input kept so far,
 * the output of the whole stream once it has been decoded, and how much of
 * it the caller has taken.
 */
struct internal_state {
	int wrapped;
	unsigned char *input;
	size_t input_length;
	size_t input_capacity;
	unsigned char *output;
	size_t output_length;
	size_t output_capacity;
	size_t output_given;
	int whole;
};

/*
 * The bits of the input, least significant first: the bytes, the next
 * byte's place, the bits read ahead and how many there are, and whether
 * the input ran out.
 */
struct inflate_bits {
	const unsigned char *data;
	size_t length;
	size_t position;
	uint32_t buffer;
	unsigned count;
	int short_input;
};

/*
 * A canonical Huffman code: how many codes each length has, and the
 * symbols in the order of their codes.
 */
struct inflate_huffman {
	unsigned short counts[INFLATE_MAX_BITS + 1];
	unsigned short symbols[INFLATE_LITERALS];
};

/* The base lengths and their extra bits of the length codes 257 to 285. */
static const unsigned short inflate_length_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char inflate_length_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/* The base distances and their extra bits of the distance codes 0 to 29. */
static const unsigned short inflate_distance_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char inflate_distance_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* The order the code lengths' code lengths come in (RFC 1951 3.2.7). */
static const unsigned char inflate_order[INFLATE_CODE_LENGTHS] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int inflate_setup(z_streamp strm, int windowBits);
static int inflate_keep(struct internal_state *state, const Bytef *data, uInt length);
static int inflate_whole(struct internal_state *state);
static int inflate_bits_need(struct inflate_bits *bits, unsigned count);
static uint32_t inflate_bits_take(struct inflate_bits *bits, unsigned count);
static int inflate_put(struct internal_state *state, unsigned char byte);
static int inflate_stored(struct internal_state *state, struct inflate_bits *bits);
static int inflate_codes(struct internal_state *state, struct inflate_bits *bits, const struct inflate_huffman *literals, const struct inflate_huffman *distances);
static int inflate_fixed(struct internal_state *state, struct inflate_bits *bits);
static int inflate_dynamic(struct internal_state *state, struct inflate_bits *bits);
static int inflate_build(struct inflate_huffman *huffman, const unsigned char *lengths, unsigned count, int incomplete_ok);
static int inflate_symbol(struct inflate_bits *bits, const struct inflate_huffman *huffman, unsigned *symbol);

/*
 * Reports the version of the library that was loaded.
 */
const char *
zlibVersion(
	void)
{
	/* The version zlib's interface is named by. */
	return ZLIB_VERSION;
}

/*
 * Starts decompressing a zlib stream.
 */
int
inflateInit_(
	z_streamp strm,
	const char *version,
	int stream_size)
{
	int error;

	/* The zlib wrapper, with the largest window. */
	error = inflateInit2_(strm, MAX_WBITS, version, stream_size);
	if (error != Z_OK)
		return error;

	/* Succeeded. */
	return Z_OK;
}

/*
 * Starts decompressing a stream: zlib-wrapped for windowBits 8 to 15 (and
 * 0), raw deflate for -8 to -15.  The gzip wrapper (windowBits + 16) is
 * not read.
 */
int
inflateInit2_(
	z_streamp strm,
	int windowBits,
	const char *version,
	int stream_size)
{
	int error;

	/* A caller built against another stream layout, or with no stream. */
	if (version == NULL || version[0] != ZLIB_VERSION[0] || stream_size != (int)sizeof(z_stream))
		return Z_VERSION_ERROR;
	if (strm == NULL)
		return Z_STREAM_ERROR;

	/* The state for the wrapper asked for. */
	error = inflate_setup(strm, windowBits);
	if (error != Z_OK)
		return error;

	/* Succeeded: the stream is ready for input. */
	return Z_OK;
}

/*
 * Decompresses as much as the input and the room for output allow.  The
 * input is taken whole; the output comes once the stream is complete.
 * Returns Z_STREAM_END once all of it has been handed out, Z_OK while
 * there is more to come, Z_BUF_ERROR when nothing could be done (more
 * input or more room is needed), Z_DATA_ERROR for a stream that is not a
 * valid one, Z_MEM_ERROR, or Z_STREAM_ERROR.
 */
int
inflate(
	z_streamp strm,
	int flush)
{
	struct internal_state *state;
	size_t left;
	size_t count;
	int result;
	int error;

	/* A stream that was set up. */
	if (strm == NULL || strm->state == NULL)
		return Z_STREAM_ERROR;
	state = strm->state;

	/* The input, kept whole. */
	count = strm->avail_in;
	if (count > 0U) {
		error = inflate_keep(state, strm->next_in, strm->avail_in);
		if (error != Z_OK)
			return error;
		strm->next_in += count;
		strm->avail_in = 0;
		strm->total_in += count;
	}

	/* The stream decoded once it is whole. */
	if (!state->whole) {
		result = inflate_whole(state);
		if (result == INFLATE_BAD) {
			strm->msg = "invalid deflate stream";
			return Z_DATA_ERROR;
		}

		/* Memory ran out. */
		if (result == INFLATE_NO_MEMORY)
			return Z_MEM_ERROR;

		/* The stream is not whole yet: more input is waited for (none came: nothing could be done). */
		if (result == INFLATE_SHORT) {
			if (count > 0U && flush != Z_FINISH)
				return Z_OK;
			return Z_BUF_ERROR;
		}

		/* The output is ready to be handed out. */
		state->whole = 1;
	}

	/* The output, as much as there is room for. */
	left = state->output_length - state->output_given;
	count = left;
	if (count > strm->avail_out)
		count = strm->avail_out;
	if (count > 0U) {
		memcpy(strm->next_out, state->output + state->output_given, count);
		state->output_given += count;
		strm->next_out += count;
		strm->avail_out -= (uInt)count;
		strm->total_out += count;
	}

	/* All handed out: the end. */
	if (state->output_given == state->output_length)
		return Z_STREAM_END;

	/* No room at all: nothing could be done. */
	if (count == 0U)
		return Z_BUF_ERROR;

	/* More to come. */
	return Z_OK;
}

/*
 * Frees a stream's state.
 */
int
inflateEnd(
	z_streamp strm)
{
	/* A stream that was set up. */
	if (strm == NULL || strm->state == NULL)
		return Z_STREAM_ERROR;

	/* The kept input and output, then the state. */
	free(strm->state->input);
	free(strm->state->output);
	free(strm->state);
	strm->state = NULL;

	/* Succeeded. */
	return Z_OK;
}

/*
 * Starts the stream again, keeping its wrapper.
 */
int
inflateReset(
	z_streamp strm)
{
	struct internal_state *state;

	/* A stream that was set up. */
	if (strm == NULL || strm->state == NULL)
		return Z_STREAM_ERROR;

	/* Nothing kept, nothing decoded. */
	state = strm->state;
	state->input_length = 0;
	state->output_length = 0;
	state->output_given = 0;
	state->whole = 0;
	strm->total_in = 0;
	strm->total_out = 0;
	strm->msg = NULL;

	/* Succeeded. */
	return Z_OK;
}

/*
 * Decompresses a whole zlib stream into a buffer of a size, which it
 * changes to the size decompressed.  Returns Z_OK, Z_BUF_ERROR when the
 * buffer is too small, Z_DATA_ERROR or Z_MEM_ERROR.
 */
int
uncompress(
	Bytef *dest,
	uLongf *destLen,
	const Bytef *source,
	uLong sourceLen)
{
	z_stream stream;
	int result;
	int error;

	/* The stream over the source. */
	memset(&stream, 0, sizeof(stream));
	error = inflateInit(&stream);
	if (error != Z_OK)
		return error;
	stream.next_in = source;
	stream.avail_in = (uInt)sourceLen;
	stream.next_out = dest;
	stream.avail_out = (uInt)*destLen;

	/* All of it at once. */
	result = inflate(&stream, Z_FINISH);
	*destLen = stream.total_out;
	(void)inflateEnd(&stream);

	/* A stream that did not fit, or was short or broken. */
	if (result == Z_OK)
		return Z_BUF_ERROR;
	if (result != Z_STREAM_END)
		return result;

	/* Succeeded. */
	return Z_OK;
}

/* Allocates a stream's state for a wrapper (windowBits as inflateInit2 takes it). */
static int
inflate_setup(
	z_streamp strm,
	int windowBits)
{
	struct internal_state *state;
	int wrapped;

	/* The wrapper: zlib (0, or 8 to 15) or raw (-8 to -15). */
	if (windowBits == 0 || (windowBits >= 8 && windowBits <= 15)) {
		wrapped = 1;
	} else if (windowBits >= -15 && windowBits <= -8) {
		wrapped = 0;
	} else {
		return Z_STREAM_ERROR;
	}

	/* The state. */
	state = calloc(1, sizeof(*state));
	if (state == NULL)
		return Z_MEM_ERROR;
	state->wrapped = wrapped;

	/* The stream starts empty. */
	strm->state = state;
	strm->total_in = 0;
	strm->total_out = 0;
	strm->msg = NULL;
	strm->adler = 1;

	/* Succeeded. */
	return Z_OK;
}

/* Keeps a piece of input after the pieces before it. */
static int
inflate_keep(
	struct internal_state *state,
	const Bytef *data,
	uInt length)
{
	unsigned char *grown;
	size_t capacity;

	/* Room for it, twice what is needed when the buffer must grow. */
	if (state->input_length + length > state->input_capacity) {
		capacity = (state->input_length + length) * 2U;
		grown = realloc(state->input, capacity);
		if (grown == NULL)
			return Z_MEM_ERROR;
		state->input = grown;
		state->input_capacity = capacity;
	}

	/* The bytes, after those kept. */
	memcpy(state->input + state->input_length, data, length);
	state->input_length += length;

	/* Succeeded. */
	return Z_OK;
}

/*
 * Decodes the kept input as a whole stream into the output buffer.
 * Returns INFLATE_WHOLE, INFLATE_SHORT when the input ends before the
 * stream does, INFLATE_BAD or INFLATE_NO_MEMORY.
 */
static int
inflate_whole(
	struct internal_state *state)
{
	int have;
	struct inflate_bits bits;
	uint32_t header;
	uint32_t checksum;
	uint32_t expected;
	uint32_t final;
	uint32_t type;
	unsigned index;
	int result;

	/* The bits of the kept input, and an empty output. */
	memset(&bits, 0, sizeof(bits));
	bits.data = state->input;
	bits.length = state->input_length;
	state->output_length = 0;

	/* The zlib header: deflate (8), a window of 32 KiB at most, a check that divides by 31, no dictionary. */
	if (state->wrapped) {
		have = inflate_bits_need(&bits, 16U);
		if (!have)
			return INFLATE_SHORT;
		header = inflate_bits_take(&bits, 8U) << 8;
		header |= inflate_bits_take(&bits, 8U);
		if ((header >> 8 & 0x0fU) != 8U || (header >> 12) > 7U || header % 31U != 0U || (header & 0x20U) != 0U)
			return INFLATE_BAD;
	}

	/* Each block until the last. */
	do {
		have = inflate_bits_need(&bits, 3U);
		if (!have)
			return INFLATE_SHORT;
		final = inflate_bits_take(&bits, 1U);
		type = inflate_bits_take(&bits, 2U);

		/* The block by its type: stored, fixed or dynamic Huffman codes. */
		if (type == 0U) {
			result = inflate_stored(state, &bits);
		} else if (type == 1U) {
			result = inflate_fixed(state, &bits);
		} else if (type == 2U) {
			result = inflate_dynamic(state, &bits);
		} else {
			result = INFLATE_BAD;
		}

		/* A block that is short, broken or out of memory ends the attempt. */
		if (result != INFLATE_WHOLE)
			return result;
	} while (final == 0U);

	/* A raw stream ends there. */
	if (!state->wrapped)
		return INFLATE_WHOLE;

	/* The zlib trailer: the Adler-32 of the output, most significant byte first, from a byte boundary. */
	bits.buffer = 0;
	bits.count = 0;
	expected = 0;
	for (index = 0; index < 4U; index++) {
		have = inflate_bits_need(&bits, 8U);
		if (!have)
			return INFLATE_SHORT;
		expected = (expected << 8) | inflate_bits_take(&bits, 8U);
	}

	/* The output's own Adler-32 must be the one given. */
	checksum = (uint32_t)adler32(adler32(0L, NULL, 0), state->output, (uInt)state->output_length);
	if (checksum != expected)
		return INFLATE_BAD;

	/* Succeeded: the stream is whole and checked. */
	return INFLATE_WHOLE;
}

/* Reads ahead until a number of bits are there; 0 when the input ends first. */
static int
inflate_bits_need(
	struct inflate_bits *bits,
	unsigned count)
{
	/* A byte at a time, above the bits already there. */
	while (bits->count < count) {
		if (bits->position == bits->length) {
			bits->short_input = 1;
			return 0;
		}

		/* The next byte above the bits there. */
		bits->buffer |= (uint32_t)bits->data[bits->position] << bits->count;
		bits->position++;
		bits->count += 8U;
	}

	/* They are there. */
	return 1;
}

/* Takes bits read ahead (inflate_bits_need made sure of them), least significant first. */
static uint32_t
inflate_bits_take(
	struct inflate_bits *bits,
	unsigned count)
{
	uint32_t value;

	/* The low bits, then the rest moved down. */
	value = bits->buffer & ((1U << count) - 1U);
	bits->buffer >>= count;
	bits->count -= count;
	return value;
}

/* Adds a byte to the output, growing it when full; 0 without memory. */
static int
inflate_put(
	struct internal_state *state,
	unsigned char byte)
{
	unsigned char *grown;
	size_t capacity;

	/* Room for one more, twice as much when the buffer must grow. */
	if (state->output_length == state->output_capacity) {
		capacity = state->output_capacity * 2U;
		if (capacity < 4096U)
			capacity = 4096U;
		grown = realloc(state->output, capacity);
		if (grown == NULL)
			return 0;
		state->output = grown;
		state->output_capacity = capacity;
	}

	/* The byte. */
	state->output[state->output_length] = byte;
	state->output_length++;
	return 1;
}

/* Copies a stored block: from a byte boundary, its length and that length's complement, then the bytes. */
static int
inflate_stored(
	struct internal_state *state,
	struct inflate_bits *bits)
{
	int have;
	uint32_t length;
	uint32_t complement;
	uint32_t index;
	int room;

	/* The rest of the byte is dropped. */
	bits->buffer = 0;
	bits->count = 0;

	/* The length and its complement. */
	have = inflate_bits_need(bits, 16U);
	if (!have)
		return INFLATE_SHORT;
	length = inflate_bits_take(bits, 16U);
	have = inflate_bits_need(bits, 16U);
	if (!have)
		return INFLATE_SHORT;
	complement = inflate_bits_take(bits, 16U);
	if ((length ^ 0xffffU) != complement)
		return INFLATE_BAD;

	/* The bytes as they are. */
	for (index = 0; index < length; index++) {
		have = inflate_bits_need(bits, 8U);
		if (!have)
			return INFLATE_SHORT;
		room = inflate_put(state, (unsigned char)inflate_bits_take(bits, 8U));
		if (!room)
			return INFLATE_NO_MEMORY;
	}

	/* Succeeded. */
	return INFLATE_WHOLE;
}

/* Decodes a block's literals and back-references with its codes, until its end code. */
static int
inflate_codes(
	struct internal_state *state,
	struct inflate_bits *bits,
	const struct inflate_huffman *literals,
	const struct inflate_huffman *distances)
{
	int have;
	unsigned symbol;
	uint32_t length;
	uint32_t distance;
	uint32_t index;
	int found;
	int room;

	/* Each symbol until the end of the block (256). */
	for (;;) {
		found = inflate_symbol(bits, literals, &symbol);
		if (found != INFLATE_WHOLE)
			return found;

		/* A literal byte. */
		if (symbol < 256U) {
			room = inflate_put(state, (unsigned char)symbol);
			if (!room)
				return INFLATE_NO_MEMORY;
			continue;
		}

		/* The end. */
		if (symbol == 256U)
			return INFLATE_WHOLE;

		/* A length (and its extra bits). */
		symbol -= 257U;
		if (symbol >= 29U)
			return INFLATE_BAD;
		have = inflate_bits_need(bits, inflate_length_extra[symbol]);
		if (!have)
			return INFLATE_SHORT;
		length = inflate_length_base[symbol] + inflate_bits_take(bits, inflate_length_extra[symbol]);

		/* Its distance (and its extra bits), back into the output so far. */
		found = inflate_symbol(bits, distances, &symbol);
		if (found != INFLATE_WHOLE)
			return found;
		if (symbol >= INFLATE_DISTANCES)
			return INFLATE_BAD;
		have = inflate_bits_need(bits, inflate_distance_extra[symbol]);
		if (!have)
			return INFLATE_SHORT;
		distance = inflate_distance_base[symbol] + inflate_bits_take(bits, inflate_distance_extra[symbol]);
		if (distance > state->output_length)
			return INFLATE_BAD;

		/* The bytes copied from that far back, one at a time (the copy may overlap itself). */
		for (index = 0; index < length; index++) {
			room = inflate_put(state, state->output[state->output_length - distance]);
			if (!room)
				return INFLATE_NO_MEMORY;
		}
	}
}

/* Decodes a block with the fixed codes (RFC 1951 3.2.6). */
static int
inflate_fixed(
	struct internal_state *state,
	struct inflate_bits *bits)
{
	struct inflate_huffman literals;
	struct inflate_huffman distances;
	unsigned char lengths[INFLATE_LITERALS];
	unsigned index;
	int result;

	/* The literal/length code: 8, 9, 7 and 8 bits over its four ranges. */
	for (index = 0; index < 144U; index++)
		lengths[index] = 8;
	for (; index < 256U; index++)
		lengths[index] = 9;
	for (; index < 280U; index++)
		lengths[index] = 7;
	for (; index < INFLATE_LITERALS; index++)
		lengths[index] = 8;
	result = inflate_build(&literals, lengths, INFLATE_LITERALS, 0);
	if (result != INFLATE_WHOLE)
		return result;

	/* The distance code: 5 bits each. */
	for (index = 0; index < INFLATE_DISTANCES; index++)
		lengths[index] = 5;
	result = inflate_build(&distances, lengths, INFLATE_DISTANCES, 1);
	if (result != INFLATE_WHOLE)
		return result;

	/* The block. */
	result = inflate_codes(state, bits, &literals, &distances);
	if (result != INFLATE_WHOLE)
		return result;

	/* Succeeded. */
	return INFLATE_WHOLE;
}

/* Decodes a block with codes of its own, which it describes first (RFC 1951 3.2.7). */
static int
inflate_dynamic(
	struct internal_state *state,
	struct inflate_bits *bits)
{
	int have;
	struct inflate_huffman lengths_code;
	struct inflate_huffman literals;
	struct inflate_huffman distances;
	unsigned char lengths[INFLATE_LITERALS + INFLATE_DISTANCES];
	unsigned literal_count;
	unsigned distance_count;
	unsigned code_count;
	unsigned symbol;
	unsigned repeat;
	unsigned index;
	unsigned char previous;
	int result;

	/* How many literal/length, distance and code-length codes there are. */
	have = inflate_bits_need(bits, 14U);
	if (!have)
		return INFLATE_SHORT;
	literal_count = inflate_bits_take(bits, 5U) + 257U;
	distance_count = inflate_bits_take(bits, 5U) + 1U;
	code_count = inflate_bits_take(bits, 4U) + 4U;
	if (literal_count > 286U || distance_count > INFLATE_DISTANCES)
		return INFLATE_BAD;

	/* The code lengths' code, three bits a length in its order. */
	memset(lengths, 0, sizeof(lengths));
	for (index = 0; index < code_count; index++) {
		have = inflate_bits_need(bits, 3U);
		if (!have)
			return INFLATE_SHORT;
		lengths[inflate_order[index]] = (unsigned char)inflate_bits_take(bits, 3U);
	}

	/* That code. */
	result = inflate_build(&lengths_code, lengths, INFLATE_CODE_LENGTHS, 0);
	if (result != INFLATE_WHOLE)
		return result;

	/* The literal/length and distance codes' lengths, with its repeats. */
	index = 0;
	while (index < literal_count + distance_count) {
		result = inflate_symbol(bits, &lengths_code, &symbol);
		if (result != INFLATE_WHOLE)
			return result;

		/* A length as it is. */
		if (symbol < 16U) {
			lengths[index] = (unsigned char)symbol;
			index++;
			continue;
		}

		/* The previous length 3 to 6 times (16), or zero 3 to 10 (17) or 11 to 138 times (18). */
		previous = 0;
		if (symbol == 16U) {
			if (index == 0U)
				return INFLATE_BAD;
			previous = lengths[index - 1U];
			have = inflate_bits_need(bits, 2U);
			if (!have)
				return INFLATE_SHORT;
			repeat = 3U + inflate_bits_take(bits, 2U);
		} else if (symbol == 17U) {
			have = inflate_bits_need(bits, 3U);
			if (!have)
				return INFLATE_SHORT;
			repeat = 3U + inflate_bits_take(bits, 3U);
		} else {
			have = inflate_bits_need(bits, 7U);
			if (!have)
				return INFLATE_SHORT;
			repeat = 11U + inflate_bits_take(bits, 7U);
		}

		/* The repeat must stay inside the lengths. */
		if (index + repeat > literal_count + distance_count)
			return INFLATE_BAD;
		while (repeat > 0U) {
			lengths[index] = previous;
			index++;
			repeat--;
		}
	}

	/* A block must be able to end. */
	if (lengths[256] == 0U)
		return INFLATE_BAD;

	/* The two codes. */
	result = inflate_build(&literals, lengths, literal_count, 0);
	if (result != INFLATE_WHOLE)
		return result;
	result = inflate_build(&distances, lengths + literal_count, distance_count, 1);
	if (result != INFLATE_WHOLE)
		return result;

	/* The block. */
	result = inflate_codes(state, bits, &literals, &distances);
	if (result != INFLATE_WHOLE)
		return result;

	/* Succeeded. */
	return INFLATE_WHOLE;
}

/*
 * Builds a canonical Huffman code from its symbols' code lengths (0 for
 * a symbol not used).  A code with more codes than its lengths allow is
 * refused; one with fewer only where incomplete_ok (the distance code may
 * have a single code).
 */
static int
inflate_build(
	struct inflate_huffman *huffman,
	const unsigned char *lengths,
	unsigned count,
	int incomplete_ok)
{
	unsigned short offsets[INFLATE_MAX_BITS + 1];
	unsigned symbol;
	unsigned length;
	int left;

	/* How many codes each length has. */
	memset(huffman->counts, 0, sizeof(huffman->counts));
	for (symbol = 0; symbol < count; symbol++)
		huffman->counts[lengths[symbol]]++;

	/* No code at all is only a distance code's (a block of literals only). */
	if (huffman->counts[0] == count) {
		if (!incomplete_ok)
			return INFLATE_BAD;
		return INFLATE_WHOLE;
	}

	/* Not more codes than the lengths leave room for. */
	left = 1;
	for (length = 1; length <= INFLATE_MAX_BITS; length++) {
		left <<= 1;
		left -= huffman->counts[length];
		if (left < 0)
			return INFLATE_BAD;
	}

	/* A code with room left over, only where allowed. */
	if (left > 0 && !incomplete_ok)
		return INFLATE_BAD;

	/* Where each length's symbols start in the table. */
	offsets[1] = 0;
	for (length = 1; length < INFLATE_MAX_BITS; length++)
		offsets[length + 1] = (unsigned short)(offsets[length] + huffman->counts[length]);

	/* The symbols by length, then in their order. */
	for (symbol = 0; symbol < count; symbol++) {
		if (lengths[symbol] != 0U) {
			huffman->symbols[offsets[lengths[symbol]]] = (unsigned short)symbol;
			offsets[lengths[symbol]]++;
		}
	}

	/* Succeeded. */
	return INFLATE_WHOLE;
}

/*
 * Decodes one symbol a bit at a time: a code of each length is tried, the
 * first bit read being the most significant of the code.
 */
static int
inflate_symbol(
	struct inflate_bits *bits,
	const struct inflate_huffman *huffman,
	unsigned *symbol)
{
	int have;
	int code;
	int first;
	int index;
	int count;
	unsigned length;

	/* The code grows a bit at a time until it is one of its length's. */
	code = 0;
	first = 0;
	index = 0;
	for (length = 1; length <= INFLATE_MAX_BITS; length++) {
		have = inflate_bits_need(bits, 1U);
		if (!have)
			return INFLATE_SHORT;
		code |= (int)inflate_bits_take(bits, 1U);
		count = huffman->counts[length];
		if (code - first < count) {
			*symbol = huffman->symbols[index + (code - first)];
			return INFLATE_WHOLE;
		}

		/* Past this length's codes: one more bit. */
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}

	/* No code matched. */
	return INFLATE_BAD;
}
