/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * deflate: the compression of libz-compat (ws175-p006, plan/ws175/
 * phase001/design.md D4 and section 5.2; libpdf compresses the images and
 * the content it writes with it).  A deflate stream (RFC 1951) in a zlib
 * wrapper (RFC 1950, its Adler-32 last) or raw: LZ77 over a 32 KB window
 * (hash chains of three bytes, a lazy match), each block written stored,
 * with the fixed Huffman codes or with dynamic ones, whichever is the
 * shortest.
 *
 * It is written to be read, not to be fast, like inflate.c: deflate keeps
 * every byte of input it is given, and at Z_FINISH compresses the whole
 * of it into an output buffer of its own, which it hands out as the caller
 * makes room.  Z_NO_FLUSH and the other flushes only take input.
 */

#include <compat/zlib/zlib.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The window, the longest and shortest match, and the hash's size. */
#define DEFLATE_WINDOW		32768U
#define DEFLATE_MATCH_MAX	258U
#define DEFLATE_MATCH_MIN	3U
#define DEFLATE_HASH_BITS	15U
#define DEFLATE_HASH_SIZE	(1U << DEFLATE_HASH_BITS)

/* The symbols of one block, the literal/length and distance alphabets, and the code-length alphabet. */
#define DEFLATE_BLOCK_SYMBOLS	16384U
#define DEFLATE_LITERALS	286U
#define DEFLATE_DISTANCES	30U
#define DEFLATE_LENGTH_CODES	19U

/* The longest code of the two alphabets, and of the code-length alphabet. */
#define DEFLATE_CODE_MAX	15U
#define DEFLATE_LENGTH_CODE_MAX	7U

/* The most bytes one stored block holds. */
#define DEFLATE_STORED_MAX	65535U

/* No position in the hash chains. */
#define DEFLATE_NONE		0xffffffffU

/*
 * One symbol of a block: a literal (distance 0, value the byte) or a match
 * (value its length, distance its distance).
 */
struct deflate_symbol {
	unsigned short value;
	unsigned short distance;
};

/*
 * The state of one stream: the wrapper, the level, the input kept, the
 * output of the whole stream once it has been compressed, and how much of
 * it the caller has taken.
 */
struct deflate_state {
	int wrapped;
	int level;
	unsigned char *input;
	size_t input_length;
	size_t input_capacity;
	unsigned char *output;
	size_t output_length;
	size_t output_capacity;
	size_t output_given;
	int whole;
};

/* The bits written, least significant first, and whether memory ran out. */
struct deflate_bits {
	unsigned char *data;
	size_t length;
	size_t capacity;
	uint32_t buffer;
	unsigned count;
	int failed;
};

/* A Huffman code: each symbol's length and its code (bits reversed, ready to write). */
struct deflate_code {
	unsigned char lengths[DEFLATE_LITERALS + 2U];
	unsigned short codes[DEFLATE_LITERALS + 2U];
};

/* The base lengths and their extra bits of the length codes 257 to 285. */
static const unsigned short deflate_length_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char deflate_length_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/* The base distances and their extra bits of the distance codes 0 to 29. */
static const unsigned short deflate_distance_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
	6145, 8193, 12289, 16385, 24577
};
static const unsigned char deflate_distance_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* The order the code-length code's lengths are written in. */
static const unsigned char deflate_length_order[DEFLATE_LENGTH_CODES] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int deflate_setup(z_streamp strm, int level, int windowBits);
static int deflate_keep(struct deflate_state *state, const unsigned char *data, size_t length);
static int deflate_whole(struct deflate_state *state);
static int deflate_block(struct deflate_bits *bits, const unsigned char *data, size_t start, size_t end, const struct deflate_symbol *symbols, size_t count, int last);
static void deflate_stored(struct deflate_bits *bits, const unsigned char *data, size_t start, size_t end, int last);
static size_t deflate_symbols_cost(const struct deflate_symbol *symbols, size_t count, const struct deflate_code *literals, const struct deflate_code *distances);
static void deflate_write_symbols(struct deflate_bits *bits, const struct deflate_symbol *symbols, size_t count, const struct deflate_code *literals, const struct deflate_code *distances);
static void deflate_fixed(struct deflate_code *literals, struct deflate_code *distances);
static void deflate_lengths(const unsigned long *frequencies, unsigned count, unsigned limit, unsigned char *lengths);
static void deflate_canonical(struct deflate_code *code, unsigned count);
static unsigned deflate_length_code(unsigned length);
static unsigned deflate_distance_code(unsigned distance);
static void deflate_put(struct deflate_bits *bits, uint32_t value, unsigned count);
static void deflate_flush_bits(struct deflate_bits *bits);
static void deflate_byte(struct deflate_bits *bits, unsigned char byte);

/*
 * Sets a stream up for compression with a zlib wrapper.
 */
int
deflateInit_(
	z_streamp strm,
	int level,
	const char *version,
	int stream_size)
{
	/* The default window with its wrapper. */
	return deflateInit2_(strm, level, Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY, version, stream_size);
}

/*
 * Sets a stream up for compression: windowBits 8 to 15 for a zlib wrapper,
 * -8 to -15 for raw deflate (the window is always 32 KB; a smaller one is
 * only a stream's promise to its reader, and 32 KB is always good).
 * Returns Z_OK, Z_STREAM_ERROR, Z_MEM_ERROR or Z_VERSION_ERROR.
 */
int
deflateInit2_(
	z_streamp strm,
	int level,
	int method,
	int windowBits,
	int memLevel,
	int strategy,
	const char *version,
	int stream_size)
{
	/* A caller built against another stream layout, or with no stream. */
	(void)memLevel;
	(void)strategy;
	if (version == NULL || version[0] != ZLIB_VERSION[0] || stream_size != (int)sizeof(z_stream))
		return Z_VERSION_ERROR;
	if (strm == NULL || method != Z_DEFLATED)
		return Z_STREAM_ERROR;

	/* A level from 0 to 9, or the default. */
	if (level == Z_DEFAULT_COMPRESSION)
		level = 6;
	if (level < 0 || level > 9)
		return Z_STREAM_ERROR;

	/* The state. */
	return deflate_setup(strm, level, windowBits);
}

/*
 * Compresses: takes the input, and at Z_FINISH compresses the whole stream
 * and hands it out as the room for output allows.  Returns Z_STREAM_END
 * once all of it has been handed out, Z_OK while there is more to come or
 * more input is taken, Z_BUF_ERROR when nothing could be done, Z_MEM_ERROR,
 * or Z_STREAM_ERROR.
 */
int
deflate(
	z_streamp strm,
	int flush)
{
	struct deflate_state *state;
	size_t left;
	size_t count;
	int error;

	/* A stream that was set up. */
	if (strm == NULL || strm->state == NULL)
		return Z_STREAM_ERROR;
	state = (struct deflate_state *)strm->state;

	/* The input, kept whole. */
	count = strm->avail_in;
	if (count > 0U) {
		if (state->whole)
			return Z_STREAM_ERROR;
		error = deflate_keep(state, strm->next_in, count);
		if (error != Z_OK)
			return error;
		strm->next_in += count;
		strm->avail_in = 0;
		strm->total_in += count;
	}

	/* Until Z_FINISH only input is taken. */
	if (flush != Z_FINISH) {
		if (count > 0U)
			return Z_OK;
		return Z_BUF_ERROR;
	}

	/* The whole stream compressed once. */
	if (!state->whole) {
		error = deflate_whole(state);
		if (error != Z_OK)
			return error;
		state->whole = 1;
	}

	/* As much of the output as there is room for. */
	left = state->output_length - state->output_given;
	count = left;
	if (count > strm->avail_out)
		count = strm->avail_out;
	if (count > 0U) {
		memcpy(strm->next_out, state->output + state->output_given, count);
		strm->next_out += count;
		strm->avail_out -= (uInt)count;
		strm->total_out += count;
		state->output_given += count;
	}

	/* All of it given: the end. */
	if (state->output_given == state->output_length)
		return Z_STREAM_END;
	if (count == 0U)
		return Z_BUF_ERROR;
	return Z_OK;
}

/*
 * Ends a stream.
 */
int
deflateEnd(
	z_streamp strm)
{
	struct deflate_state *state;

	/* A stream that was set up. */
	if (strm == NULL || strm->state == NULL)
		return Z_STREAM_ERROR;
	state = (struct deflate_state *)strm->state;

	/* The kept input and output, then the state. */
	free(state->input);
	free(state->output);
	free(state);
	strm->state = NULL;

	/* Succeeded. */
	return Z_OK;
}

/*
 * Reports the most bytes compress2 may write for sourceLen bytes: stored
 * blocks of the whole, with the wrapper.
 */
uLong
compressBound(
	uLong sourceLen)
{
	/*
	 * Stored pieces: one at least for each block of symbols (a block covers
	 * DEFLATE_BLOCK_SYMBOLS bytes or more) and one more for each 65535
	 * bytes; each piece's five bytes and the byte its header may pad, the
	 * wrapper's six, and a little more.
	 */
	return sourceLen + (sourceLen / DEFLATE_BLOCK_SYMBOLS + sourceLen / DEFLATE_STORED_MAX + 2UL) * 6UL + 6UL + 16UL;
}

/*
 * Compresses a buffer into another with a zlib wrapper at the default
 * level.
 */
int
compress(
	Bytef *dest,
	uLongf *destLen,
	const Bytef *source,
	uLong sourceLen)
{
	/* The default level. */
	return compress2(dest, destLen, source, sourceLen, Z_DEFAULT_COMPRESSION);
}

/*
 * Compresses a buffer into another with a zlib wrapper.  destLen is the
 * room and becomes the length written.  Returns Z_OK, Z_BUF_ERROR when the
 * room is too small, Z_MEM_ERROR, or Z_STREAM_ERROR for a bad level.
 */
int
compress2(
	Bytef *dest,
	uLongf *destLen,
	const Bytef *source,
	uLong sourceLen,
	int level)
{
	z_stream stream;
	int error;

	/* The stream over the two buffers. */
	if (dest == NULL || destLen == NULL || (source == NULL && sourceLen != 0UL))
		return Z_STREAM_ERROR;
	memset(&stream, 0, sizeof(stream));
	error = deflateInit2_(&stream, level, Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY, ZLIB_VERSION, (int)sizeof(stream));
	if (error != Z_OK)
		return error;
	stream.next_in = source;
	stream.avail_in = (uInt)sourceLen;
	stream.next_out = dest;
	stream.avail_out = (uInt)*destLen;

	/* All of it, at once. */
	error = deflate(&stream, Z_FINISH);
	*destLen = stream.total_out;
	(void)deflateEnd(&stream);
	if (error == Z_STREAM_END)
		return Z_OK;
	if (error == Z_OK)
		return Z_BUF_ERROR;
	return error;
}

/* Makes a stream's state for a level and a wrapper. */
static int
deflate_setup(
	z_streamp strm,
	int level,
	int windowBits)
{
	struct deflate_state *state;

	/* The wrapper asked for. */
	if (!((windowBits >= 8 && windowBits <= 15) || (windowBits >= -15 && windowBits <= -8)))
		return Z_STREAM_ERROR;

	/* The state, empty. */
	state = calloc(1, sizeof(*state));
	if (state == NULL)
		return Z_MEM_ERROR;
	state->wrapped = windowBits > 0;
	state->level = level;

	/* Succeeded: the stream is ready for input. */
	strm->state = (struct internal_state *)state;
	strm->total_in = 0;
	strm->total_out = 0;
	strm->msg = NULL;
	strm->adler = 1;
	return Z_OK;
}

/* Keeps input until the stream is finished.  Returns Z_OK or Z_MEM_ERROR. */
static int
deflate_keep(
	struct deflate_state *state,
	const unsigned char *data,
	size_t length)
{
	unsigned char *grown;
	size_t capacity;

	/* Room, doubled when short. */
	if (state->input_length + length > state->input_capacity) {
		capacity = state->input_capacity * 2U + length + 1024U;
		grown = realloc(state->input, capacity);
		if (grown == NULL)
			return Z_MEM_ERROR;
		state->input = grown;
		state->input_capacity = capacity;
	}

	/* Succeeded: kept. */
	memcpy(state->input + state->input_length, data, length);
	state->input_length += length;
	return Z_OK;
}

/*
 * Compresses the whole input: the wrapper's header, the blocks (each of up
 * to DEFLATE_BLOCK_SYMBOLS symbols), the wrapper's Adler-32.  Returns Z_OK
 * or Z_MEM_ERROR.
 */
static int
deflate_whole(
	struct deflate_state *state)
{
	struct deflate_bits bits;
	struct deflate_symbol *symbols;
	uint32_t *head;
	uint32_t *previous;
	const unsigned char *data;
	size_t length;
	size_t position;
	size_t block_start;
	size_t count;
	size_t candidate;
	size_t best_length;
	size_t best_distance;
	size_t next_length;
	size_t match;
	size_t same;
	size_t limit;
	unsigned chain;
	unsigned chain_max;
	unsigned hash;
	uLong check;
	int last;
	int error;

	/* The bits, the hash chains and a block's symbols. */
	memset(&bits, 0, sizeof(bits));
	data = state->input;
	length = state->input_length;
	head = malloc(DEFLATE_HASH_SIZE * sizeof(*head));
	previous = malloc(DEFLATE_WINDOW * sizeof(*previous));
	symbols = malloc(DEFLATE_BLOCK_SYMBOLS * sizeof(*symbols));
	if (head == NULL || previous == NULL || symbols == NULL) {
		free(head);
		free(previous);
		free(symbols);
		return Z_MEM_ERROR;
	}

	/* No position in any chain yet. */
	memset(head, 0xff, DEFLATE_HASH_SIZE * sizeof(*head));

	/* The zlib header: deflate with a 32 KB window, the level's hint, its check. */
	if (state->wrapped) {
		deflate_byte(&bits, 0x78U);
		if (state->level <= 1)
			deflate_byte(&bits, 0x01U);
		else if (state->level < 6)
			deflate_byte(&bits, 0x5eU);
		else if (state->level == 6)
			deflate_byte(&bits, 0x9cU);
		else
			deflate_byte(&bits, 0xdaU);
	}

	/* How long the chains are searched: longer for a higher level. */
	chain_max = 8U;
	if (state->level >= 4)
		chain_max = 64U;
	if (state->level >= 7)
		chain_max = 512U;

	/* The blocks: symbols gathered until a block is full or the input ends. */
	position = 0;
	block_start = 0;
	count = 0;
	error = 0;

	/* Level 0 stores the whole input (an empty one as one empty stored block). */
	if (state->level == 0)
		deflate_stored(&bits, data, 0, length, 1);
	while (state->level != 0 && position < length) {
		/* The longest match at this position (and, lazily, at the next one). */
		best_length = 0;
		best_distance = 0;
		next_length = 0;
		for (match = 0; match < 2U && position + match + DEFLATE_MATCH_MIN <= length; match++) {
			/* The chain of earlier positions with the same three bytes. */
			hash = ((unsigned)data[position + match] << 10 ^ (unsigned)data[position + match + 1U] << 5 ^ data[position + match + 2U]) & (DEFLATE_HASH_SIZE - 1U);
			candidate = head[hash];
			for (chain = 0; chain < chain_max && candidate != DEFLATE_NONE && position + match - candidate <= DEFLATE_WINDOW; chain++) {
				/* How far the candidate matches (at most the longest match, and what is left). */
				limit = length - (position + match);
				if (limit > DEFLATE_MATCH_MAX)
					limit = DEFLATE_MATCH_MAX;
				same = 0;
				while (same < limit && data[candidate + same] == data[position + match + same])
					same++;

				/* The longest so far. */
				if (match == 0 && same > best_length) {
					best_length = same;
					best_distance = position - candidate;
				} else if (match == 1 && same > next_length) {
					next_length = same;
				}

				/* The next older candidate. */
				candidate = previous[candidate % DEFLATE_WINDOW];
			}

			/* This position joins its chain (once it has been searched). */
			if (match == 0) {
				previous[position % DEFLATE_WINDOW] = head[hash];
				head[hash] = (uint32_t)position;
			}

			/* Without a match here there is nothing to be lazy about. */
			if (best_length < DEFLATE_MATCH_MIN)
				break;
		}

		/* A literal when no match, or a better one starts at the next byte; else the match. */
		if (best_length < DEFLATE_MATCH_MIN || next_length > best_length) {
			symbols[count].value = data[position];
			symbols[count].distance = 0;
			count++;
			position++;
		} else {
			symbols[count].value = (unsigned short)best_length;
			symbols[count].distance = (unsigned short)best_distance;
			count++;

			/* The matched bytes join the chains too. */
			for (match = 1; match < best_length; match++) {
				if (position + match + DEFLATE_MATCH_MIN > length)
					break;
				hash = ((unsigned)data[position + match] << 10 ^ (unsigned)data[position + match + 1U] << 5 ^ data[position + match + 2U]) & (DEFLATE_HASH_SIZE - 1U);
				previous[(position + match) % DEFLATE_WINDOW] = head[hash];
				head[hash] = (uint32_t)(position + match);
			}

			/* Past the match. */
			position += best_length;
		}

		/* The block when it is full or the input ended. */
		if (count == DEFLATE_BLOCK_SYMBOLS || position >= length) {
			last = position >= length;
			error = deflate_block(&bits, data, block_start, position, symbols, count, last);
			if (error != 0)
				break;
			block_start = position;
			count = 0;
		}
	}

	/* An empty input is one empty fixed block. */
	if (length == 0 && state->level != 0 && error == 0)
		error = deflate_block(&bits, data, 0, 0, symbols, 0, 1);
	deflate_flush_bits(&bits);
	free(head);
	free(previous);
	free(symbols);

	/* The wrapper's Adler-32 of the input, the highest byte first. */
	if (state->wrapped) {
		check = adler32(1UL, Z_NULL, 0);
		position = 0;
		while (position < length) {
			/* In pieces an unsigned int holds. */
			match = length - position;
			if (match > 0x40000000U)
				match = 0x40000000U;
			check = adler32(check, data + position, (uInt)match);
			position += match;
		}

		/* Its four bytes. */
		deflate_byte(&bits, (unsigned char)(check >> 24));
		deflate_byte(&bits, (unsigned char)(check >> 16));
		deflate_byte(&bits, (unsigned char)(check >> 8));
		deflate_byte(&bits, (unsigned char)check);
	}

	/* Memory ran out somewhere. */
	if (bits.failed || error != 0) {
		free(bits.data);
		return Z_MEM_ERROR;
	}

	/* Succeeded: the whole stream. */
	state->output = bits.data;
	state->output_length = bits.length;
	state->output_capacity = bits.capacity;
	state->output_given = 0;
	return Z_OK;
}

/*
 * Writes one block of symbols the shortest way: stored (the bytes it
 * covers), with the fixed codes, or with dynamic codes (and their
 * description).  Returns 0.
 */
static int
deflate_block(
	struct deflate_bits *bits,
	const unsigned char *data,
	size_t start,
	size_t end,
	const struct deflate_symbol *symbols,
	size_t count,
	int last)
{
	static struct deflate_code fixed_literals;
	static struct deflate_code fixed_distances;
	static int fixed_made;
	struct deflate_code literals;
	struct deflate_code distances;
	struct deflate_code lengths_code;
	unsigned long frequencies[DEFLATE_LITERALS + 2U];
	unsigned long distance_frequencies[DEFLATE_DISTANCES];
	unsigned long length_frequencies[DEFLATE_LENGTH_CODES];
	unsigned char all[DEFLATE_LITERALS + DEFLATE_DISTANCES];
	unsigned char runs[DEFLATE_LITERALS + DEFLATE_DISTANCES];
	unsigned char repeats[DEFLATE_LITERALS + DEFLATE_DISTANCES];
	size_t run_count;
	size_t fixed_cost;
	size_t dynamic_cost;
	size_t stored_cost;
	size_t at;
	unsigned literal_count;
	unsigned distance_count;
	unsigned length_count;
	unsigned total;
	unsigned value;
	unsigned repeat;
	unsigned symbol;
	uint32_t final;

	/* The fixed codes, made once. */
	if (!fixed_made) {
		deflate_fixed(&fixed_literals, &fixed_distances);
		fixed_made = 1;
	}

	/* The symbols' frequencies, the end of the block among them. */
	memset(frequencies, 0, sizeof(frequencies));
	memset(distance_frequencies, 0, sizeof(distance_frequencies));
	for (at = 0; at < count; at++) {
		if (symbols[at].distance == 0) {
			frequencies[symbols[at].value]++;
		} else {
			frequencies[257U + deflate_length_code(symbols[at].value)]++;
			distance_frequencies[deflate_distance_code(symbols[at].distance)]++;
		}
	}

	/* The end of the block, once. */
	frequencies[256]++;

	/* The dynamic codes (at least two distance codes, so that a reader takes a block of literals alone). */
	memset(&literals, 0, sizeof(literals));
	memset(&distances, 0, sizeof(distances));
	deflate_lengths(frequencies, DEFLATE_LITERALS, DEFLATE_CODE_MAX, literals.lengths);
	if (distance_frequencies[0] == 0)
		distance_frequencies[0] = 1;
	if (distance_frequencies[1] == 0)
		distance_frequencies[1] = 1;
	deflate_lengths(distance_frequencies, DEFLATE_DISTANCES, DEFLATE_CODE_MAX, distances.lengths);
	deflate_canonical(&literals, DEFLATE_LITERALS);
	deflate_canonical(&distances, DEFLATE_DISTANCES);

	/* How many of each are written (the trailing zeros left out). */
	literal_count = DEFLATE_LITERALS;
	while (literal_count > 257U && literals.lengths[literal_count - 1U] == 0U)
		literal_count--;
	distance_count = DEFLATE_DISTANCES;
	while (distance_count > 1U && distances.lengths[distance_count - 1U] == 0U)
		distance_count--;

	/* The two lists of lengths run-length coded with the code-length alphabet (16 a repeat, 17 and 18 zeros). */
	memcpy(all, literals.lengths, literal_count);
	memcpy(all + literal_count, distances.lengths, distance_count);
	total = literal_count + distance_count;
	run_count = 0;
	memset(length_frequencies, 0, sizeof(length_frequencies));
	for (at = 0; at < total; at += repeat) {
		/* How many times the length repeats from here. */
		value = all[at];
		repeat = 1;
		while (at + repeat < total && all[at + repeat] == value && repeat < 138U)
			repeat++;

		/* Zeros by 17 or 18; another length once, then by 16; a short run as it is. */
		if (value == 0U && repeat >= 11U) {
			symbol = 18U;
		} else if (value == 0U && repeat >= 3U) {
			if (repeat > 10U)
				repeat = 10U;
			symbol = 17U;
		} else if (value != 0U && at > 0U && all[at - 1U] == value && repeat >= 3U) {
			if (repeat > 6U)
				repeat = 6U;
			symbol = 16U;
		} else {
			repeat = 1;
			symbol = value;
		}

		/* The run, counted. */
		runs[run_count] = (unsigned char)symbol;
		repeats[run_count] = (unsigned char)repeat;
		run_count++;
		length_frequencies[symbol]++;
	}

	/* The code-length code, and how many of its lengths are written (at least four). */
	memset(&lengths_code, 0, sizeof(lengths_code));
	deflate_lengths(length_frequencies, DEFLATE_LENGTH_CODES, DEFLATE_LENGTH_CODE_MAX, lengths_code.lengths);
	deflate_canonical(&lengths_code, DEFLATE_LENGTH_CODES);
	length_count = DEFLATE_LENGTH_CODES;
	while (length_count > 4U && lengths_code.lengths[deflate_length_order[length_count - 1U]] == 0U)
		length_count--;

	/* The three costs in bits. */
	fixed_cost = 3U + deflate_symbols_cost(symbols, count, &fixed_literals, &fixed_distances) + 7U;
	dynamic_cost = 3U + 14U + 3U * length_count + deflate_symbols_cost(symbols, count, &literals, &distances) + literals.lengths[256];
	for (at = 0; at < run_count; at++) {
		dynamic_cost += lengths_code.lengths[runs[at]];
		if (runs[at] == 16U)
			dynamic_cost += 2U;
		if (runs[at] == 17U)
			dynamic_cost += 3U;
		if (runs[at] == 18U)
			dynamic_cost += 7U;
	}

	/* Stored: every byte, and each piece's header. */
	stored_cost = (end - start) * 8U + ((end - start) / DEFLATE_STORED_MAX + 1U) * 40U + 8U;

	/* Stored, when it is the shortest (and there are bytes to store). */
	if (end > start && stored_cost <= fixed_cost && stored_cost <= dynamic_cost) {
		deflate_stored(bits, data, start, end, last);
		return 0;
	}

	/* Whether this is the last block. */
	final = 0U;
	if (last)
		final = 1U;

	/* The fixed codes. */
	if (fixed_cost <= dynamic_cost) {
		deflate_put(bits, final, 1U);
		deflate_put(bits, 1U, 2U);
		deflate_write_symbols(bits, symbols, count, &fixed_literals, &fixed_distances);
		deflate_put(bits, fixed_literals.codes[256], fixed_literals.lengths[256]);
		return 0;
	}

	/* The dynamic codes: the header, the code-length code, the run-length coded lengths. */
	deflate_put(bits, final, 1U);
	deflate_put(bits, 2U, 2U);
	deflate_put(bits, literal_count - 257U, 5U);
	deflate_put(bits, distance_count - 1U, 5U);
	deflate_put(bits, length_count - 4U, 4U);
	for (at = 0; at < length_count; at++)
		deflate_put(bits, lengths_code.lengths[deflate_length_order[at]], 3U);
	for (at = 0; at < run_count; at++) {
		/* The symbol, and a repeat's extra bits. */
		deflate_put(bits, lengths_code.codes[runs[at]], lengths_code.lengths[runs[at]]);
		if (runs[at] == 16U)
			deflate_put(bits, repeats[at] - 3U, 2U);
		if (runs[at] == 17U)
			deflate_put(bits, repeats[at] - 3U, 3U);
		if (runs[at] == 18U)
			deflate_put(bits, repeats[at] - 11U, 7U);
	}

	/* The symbols and the end of the block. */
	deflate_write_symbols(bits, symbols, count, &literals, &distances);
	deflate_put(bits, literals.codes[256], literals.lengths[256]);
	return 0;
}

/* Writes bytes as stored blocks (of at most 65535 bytes each), the last one marked when asked. */
static void
deflate_stored(
	struct deflate_bits *bits,
	const unsigned char *data,
	size_t start,
	size_t end,
	int last)
{
	uint32_t final;
	size_t piece;
	size_t at;

	/* Piece after piece (an empty one once for empty bytes). */
	do {
		/* The header on its own byte boundary, then the length and its complement. */
		piece = end - start;
		if (piece > DEFLATE_STORED_MAX)
			piece = DEFLATE_STORED_MAX;
		final = 0U;
		if (last && start + piece == end)
			final = 1U;
		deflate_put(bits, final, 1U);
		deflate_put(bits, 0U, 2U);
		deflate_flush_bits(bits);
		deflate_byte(bits, (unsigned char)piece);
		deflate_byte(bits, (unsigned char)(piece >> 8));
		deflate_byte(bits, (unsigned char)~piece);
		deflate_byte(bits, (unsigned char)(~piece >> 8));

		/* The bytes. */
		for (at = 0; at < piece; at++)
			deflate_byte(bits, data[start + at]);
		start += piece;
	} while (start < end);
}

/* Counts the bits the symbols take with codes (their extra bits too). */
static size_t
deflate_symbols_cost(
	const struct deflate_symbol *symbols,
	size_t count,
	const struct deflate_code *literals,
	const struct deflate_code *distances)
{
	unsigned length;
	unsigned distance;
	size_t cost;
	size_t at;

	/* Each symbol. */
	cost = 0;
	for (at = 0; at < count; at++) {
		if (symbols[at].distance == 0) {
			cost += literals->lengths[symbols[at].value];
			continue;
		}

		/* A match: its length's and distance's codes and extra bits. */
		length = deflate_length_code(symbols[at].value);
		distance = deflate_distance_code(symbols[at].distance);
		cost += literals->lengths[257U + length] + deflate_length_extra[length];
		cost += distances->lengths[distance] + deflate_distance_extra[distance];
	}

	/* The bits. */
	return cost;
}

/* Writes the symbols with codes. */
static void
deflate_write_symbols(
	struct deflate_bits *bits,
	const struct deflate_symbol *symbols,
	size_t count,
	const struct deflate_code *literals,
	const struct deflate_code *distances)
{
	unsigned length;
	unsigned distance;
	size_t at;

	/* Each symbol: a literal's code, or a length's and a distance's codes with their extra bits. */
	for (at = 0; at < count; at++) {
		if (symbols[at].distance == 0) {
			deflate_put(bits, literals->codes[symbols[at].value], literals->lengths[symbols[at].value]);
			continue;
		}

		/* A match. */
		length = deflate_length_code(symbols[at].value);
		distance = deflate_distance_code(symbols[at].distance);
		deflate_put(bits, literals->codes[257U + length], literals->lengths[257U + length]);
		deflate_put(bits, symbols[at].value - deflate_length_base[length], deflate_length_extra[length]);
		deflate_put(bits, distances->codes[distance], distances->lengths[distance]);
		deflate_put(bits, symbols[at].distance - deflate_distance_base[distance], deflate_distance_extra[distance]);
	}
}

/* Makes the fixed codes of RFC 1951 section 3.2.6. */
static void
deflate_fixed(
	struct deflate_code *literals,
	struct deflate_code *distances)
{
	unsigned symbol;

	/* The literal/length code's lengths: 8, 9, 7, 8 by ranges; the distance code's: all 5. */
	memset(literals, 0, sizeof(*literals));
	memset(distances, 0, sizeof(*distances));
	for (symbol = 0; symbol < 288U; symbol++) {
		if (symbol < 144U)
			literals->lengths[symbol] = 8U;
		else if (symbol < 256U)
			literals->lengths[symbol] = 9U;
		else if (symbol < 280U)
			literals->lengths[symbol] = 7U;
		else
			literals->lengths[symbol] = 8U;
	}

	/* The distances. */
	for (symbol = 0; symbol < DEFLATE_DISTANCES; symbol++)
		distances->lengths[symbol] = 5U;

	/* Their codes. */
	deflate_canonical(literals, 288U);
	deflate_canonical(distances, DEFLATE_DISTANCES);
}

/*
 * Gives each symbol of an alphabet a code length from the frequencies (a
 * Huffman code, no length past limit: the frequencies are halved until the
 * longest fits).  A symbol that never comes has none; an alphabet with one
 * symbol gives it length 1.
 */
static void
deflate_lengths(
	const unsigned long *frequencies,
	unsigned count,
	unsigned limit,
	unsigned char *lengths)
{
	unsigned long weights[2U * (DEFLATE_LITERALS + 2U)];
	unsigned long scaled[DEFLATE_LITERALS + 2U];
	unsigned parents[2U * (DEFLATE_LITERALS + 2U)];
	unsigned nodes[2U * (DEFLATE_LITERALS + 2U)];
	unsigned node_count;
	unsigned used;
	unsigned symbol;
	unsigned first;
	unsigned second;
	unsigned at;
	unsigned depth;
	unsigned longest;
	unsigned next;

	/* The weights as given. */
	for (symbol = 0; symbol < count; symbol++)
		scaled[symbol] = frequencies[symbol];

	/* Until the longest length fits. */
	for (;;) {
		/* The leaves: the symbols that come. */
		memset(lengths, 0, count);
		node_count = 0;
		used = 0;
		for (symbol = 0; symbol < count; symbol++) {
			if (scaled[symbol] == 0)
				continue;
			weights[symbol] = scaled[symbol];
			nodes[used] = symbol;
			used++;
		}

		/* None or one symbol: one gets length 1. */
		if (used == 0)
			return;
		if (used == 1) {
			lengths[nodes[0]] = 1U;
			return;
		}

		/* Joins the two lightest nodes into a new one (numbered after the symbols) until one is left. */
		node_count = count;
		while (used > 1U) {
			/* The lightest two of the nodes left. */
			first = 0;
			for (at = 1; at < used; at++) {
				if (weights[nodes[at]] < weights[nodes[first]])
					first = at;
			}

			/* Taken out, then the next lightest. */
			symbol = nodes[first];
			nodes[first] = nodes[used - 1U];
			used--;
			second = 0;
			for (at = 1; at < used; at++) {
				if (weights[nodes[at]] < weights[nodes[second]])
					second = at;
			}

			/* Their parent in the second's place. */
			weights[node_count] = weights[symbol] + weights[nodes[second]];
			parents[symbol] = node_count;
			parents[nodes[second]] = node_count;
			nodes[second] = node_count;
			node_count++;
		}

		/* Each symbol's depth, and the longest. */
		longest = 0;
		for (symbol = 0; symbol < count; symbol++) {
			if (scaled[symbol] == 0)
				continue;
			depth = 0;
			for (next = symbol; next != node_count - 1U; next = parents[next])
				depth++;
			lengths[symbol] = (unsigned char)depth;
			if (depth > longest)
				longest = depth;
		}

		/* Fits: done; or the weights halved (each that comes keeps at least 1). */
		if (longest <= limit)
			return;
		for (symbol = 0; symbol < count; symbol++) {
			if (scaled[symbol] != 0)
				scaled[symbol] = scaled[symbol] / 2UL + 1UL;
		}
	}
}

/* Gives a code's canonical codes from its lengths (RFC 1951 section 3.2.2), bits reversed for writing. */
static void
deflate_canonical(
	struct deflate_code *code,
	unsigned count)
{
	unsigned short counts[DEFLATE_CODE_MAX + 1U];
	unsigned short next[DEFLATE_CODE_MAX + 2U];
	unsigned symbol;
	unsigned bits;
	unsigned value;
	unsigned reversed;
	unsigned at;

	/* How many codes each length has. */
	memset(counts, 0, sizeof(counts));
	for (symbol = 0; symbol < count; symbol++)
		counts[code->lengths[symbol]]++;
	counts[0] = 0;

	/* The first code of each length. */
	value = 0;
	next[0] = 0;
	for (bits = 1; bits <= DEFLATE_CODE_MAX; bits++) {
		value = (value + counts[bits - 1U]) << 1;
		next[bits] = (unsigned short)value;
	}

	/* Each symbol's code, reversed (deflate writes a code's first bit first). */
	for (symbol = 0; symbol < count; symbol++) {
		bits = code->lengths[symbol];
		if (bits == 0U)
			continue;
		value = next[bits];
		next[bits]++;
		reversed = 0;
		for (at = 0; at < bits; at++)
			reversed |= ((value >> at) & 1U) << (bits - 1U - at);
		code->codes[symbol] = (unsigned short)reversed;
	}
}

/* Gives the length code (0 to 28, for 257 to 285) of a match's length. */
static unsigned
deflate_length_code(
	unsigned length)
{
	unsigned code;

	/* The last base not past the length. */
	code = 28U;
	while (code > 0U && deflate_length_base[code] > length)
		code--;
	return code;
}

/* Gives the distance code (0 to 29) of a match's distance. */
static unsigned
deflate_distance_code(
	unsigned distance)
{
	unsigned code;

	/* The last base not past the distance. */
	code = 29U;
	while (code > 0U && deflate_distance_base[code] > distance)
		code--;
	return code;
}

/* Writes count bits of value, least significant first. */
static void
deflate_put(
	struct deflate_bits *bits,
	uint32_t value,
	unsigned count)
{
	/* The bits join those waiting; whole bytes go out. */
	bits->buffer |= (value & ((1UL << count) - 1UL)) << bits->count;
	bits->count += count;
	while (bits->count >= 8U) {
		deflate_byte(bits, (unsigned char)bits->buffer);
		bits->buffer >>= 8;
		bits->count -= 8U;
	}
}

/* Writes the bits waiting, the last byte padded with zeros. */
static void
deflate_flush_bits(
	struct deflate_bits *bits)
{
	/* A partial byte. */
	if (bits->count > 0U)
		deflate_byte(bits, (unsigned char)bits->buffer);
	bits->buffer = 0;
	bits->count = 0;
}

/* Appends a byte to the output (memory gone is noted, and later bytes are dropped). */
static void
deflate_byte(
	struct deflate_bits *bits,
	unsigned char byte)
{
	unsigned char *grown;
	size_t capacity;

	/* Room, doubled when short. */
	if (bits->failed)
		return;
	if (bits->length == bits->capacity) {
		capacity = bits->capacity * 2U + 4096U;
		grown = realloc(bits->data, capacity);
		if (grown == NULL) {
			bits->failed = 1;
			return;
		}

		/* The larger room. */
		bits->data = grown;
		bits->capacity = capacity;
	}

	/* The byte. */
	bits->data[bits->length] = byte;
	bits->length++;
}
