/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Bounded-reader endpoints, normative AAC syntax vectors and actual ADTS frame headers. */

#include "userland/desktop/libmedia/aac-input.h"
#include "userland/desktop/libmedia/aac-codec.h"
#include "userland/desktop/libmedia/aac-huffman.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One hand-authored syntax vector; the test owns and bounds all emitted bits. */
struct syntax_writer {
	uint8_t data[512];
	size_t position;
};

/* One independently specified normative tuple, including the transmitted unsigned sign suffix. */
struct tuple_vector {
	unsigned book;
	unsigned symbol;
	unsigned signs;
	unsigned sign_count;
	int16_t coefficients[4];
};

static void expect(uint64_t actual, uint64_t expected, const char *meaning);
static void put(struct syntax_writer *writer, uint64_t field, unsigned width);
static void golomb(struct syntax_writer *writer, uint64_t number);
static void test_bits(void);
static void test_rbsp(void);
static void asc(struct syntax_writer *writer, unsigned object, unsigned rate, unsigned channels);
static void pce(struct syntax_writer *writer, unsigned duplicate, unsigned comment);
static void test_config(void);
static void adts(struct syntax_writer *writer, unsigned protected_frame, unsigned blocks, unsigned length);
static void test_adts(void);
static void *test_symbols(void *unused);
static void test_spectral(void);
static void test_tuples(void);
static void test_file(const char *path, unsigned rate, unsigned channels);

/*
 * Verify the production reader, input parser and original Huffman runtime.
 */
int
main(
	int argc,
	char **argv)
{
	pthread_t threads[4];
	unsigned index;
	int error;

	/* Race the first production lookup through pthread_once before any serial lookup. */
	for (index = 0U; index < 4U; index++) {
		error = pthread_create(&threads[index], NULL, test_symbols, NULL);
		expect(error, 0U, "start concurrent codebook reader");
	}

	/* No codebook reader may observe a partially initialized tree. */
	for (index = 0U; index < 4U; index++) {
		error = pthread_join(threads[index], NULL);
		expect(error, 0U, "join concurrent codebook reader");
	}

	/* Exercise exact endpoints and independently encoded valid and invalid grammar. */
	test_bits();
	test_rbsp();
	test_config();
	test_adts();
	test_spectral();
	test_tuples();

	/* Real encoder output checks transport parsing independently of the vector writer. */
	if (argc == 3) {
		test_file(argv[1], 44100U, 2U);
		test_file(argv[2], 48000U, 1U);
	}

	/* Succeeded: all selected functional checks passed. */
	puts("WS202 bits/AAC input/Huffman PASS");
	return 0;
}

/* Stop at the first mismatched functional result, naming the contract that failed. */
static void
expect(
	uint64_t actual,
	uint64_t expected,
	const char *meaning)
{
	/* The diagnostic keeps the observed and required numbers next to their meaning. */
	if (actual != expected) {
		fprintf(stderr, "%s: got %" PRIu64 ", expected %" PRIu64 "\n", meaning,
			actual, expected);
		exit(1);
	}

	/* Succeeded: the expected contract holds at this checkpoint. */
	return;
}

/* Encode a syntax field independently of the production bit reader. */
static void
put(
	struct syntax_writer *writer,
	uint64_t field,
	unsigned width)
{
	unsigned remaining;
	unsigned bit;

	/* Writer bounds are part of the fixture, never permission to truncate test vectors. */
	if (writer->position + width > sizeof(writer->data) * 8U)
		exit(2);

	/* Place each field in standard most-significant-bit wire order. */
	for (remaining = width; remaining != 0U; remaining--) {
		bit = (unsigned)((field >> (remaining - 1U)) & 1U);
		writer->data[writer->position / 8U] |= (uint8_t)(bit << (7U - writer->position % 8U));
		writer->position++;
	}

	/* Succeeded: the entire fixture field was emitted. */
	return;
}

/* Encode an Exp-Golomb number using wide fixture arithmetic. */
static void
golomb(
	struct syntax_writer *writer,
	uint64_t number)
{
	uint64_t code;
	uint64_t prefix;
	unsigned width;

	/* The code is a unary zero prefix followed by the binary representation of number plus one. */
	code = number + 1U;
	prefix = code;
	width = 0U;
	while (prefix > 1U) {
		prefix >>= 1U;
		width++;
	}

	/* Emit the entire endpoint code, including its thirty-two leading zeros when needed. */
	put(writer, 0U, width);
	put(writer, code, width + 1U);

	/* Succeeded: the fixture holds one complete Golomb code. */
	return;
}

/* Exercise cursor bounds, scalar reads and the full signed and unsigned Golomb domains. */
static void
test_bits(
	void)
{
	const uint8_t bytes[5] = {0xabU, 0xcdU, 0xefU, 0x01U, 0x23U};
	const uint64_t numbers[7] = {0U, 1U, 2U, 127U, 2147483646U, 4294967295U, 4294967296U};
	struct syntax_writer writer;
	struct media_bits bits;
	uint32_t scalar;
	int32_t signed_number;
	size_t position;
	unsigned index;

	/* Scalar fields preserve wire order even when a full-width field starts mid-byte. */
	media_bits_init(&bits, bytes, sizeof(bytes));
	scalar = media_bits_read(&bits, 4U);
	expect(scalar, 10U, "first nibble");
	scalar = media_bits_read(&bits, 32U);
	expect(scalar, 0xbcdef012U, "unaligned thirty-two-bit field");
	media_bits_align(&bits);
	expect(bits.position, 40U, "alignment reaches exact end");
	position = bits.position;
	scalar = media_bits_read1(&bits);
	expect(bits.error, 1U, "end read fails");
	expect(bits.position, position, "failed field does not advance");
	media_bits_skip(&bits, 0U);
	expect(bits.error, 1U, "failure remains sticky");

	/* Empty borrowed spans and overflowing bit counts have distinct initialization results. */
	media_bits_init(&bits, NULL, 0U);
	expect(bits.error, 0U, "empty span");
	media_bits_init(&bits, NULL, 1U);
	expect(bits.error, 1U, "missing nonempty span");
	media_bits_init(&bits, bytes, SIZE_MAX / 8U + 1U);
	expect(bits.error, 1U, "bit count overflow");

	/* Unsigned Golomb includes UINT32_MAX but refuses the next mathematical value. */
	for (index = 0U; index < 7U; index++) {
		memset(&writer, 0, sizeof(writer));
		golomb(&writer, numbers[index]);
		media_bits_init(&bits, writer.data, (writer.position + 7U) / 8U);
		bits.count = writer.position;
		scalar = media_bits_ue(&bits);

		/* Only the final fixture exceeds the unsigned result domain. */
		if (index == 6U) {
			expect(bits.error, 1U, "unsigned Golomb rejects 2^32");
		} else {
			expect(bits.error, 0U, "unsigned Golomb complete code");
			expect(scalar, numbers[index], "unsigned Golomb value");
		}
	}

	/* Signed decoding supports INT32_MIN without accepting positive 2^31. */
	memset(&writer, 0, sizeof(writer));
	golomb(&writer, (uint64_t)UINT32_MAX + 1U);
	media_bits_init(&bits, writer.data, (writer.position + 7U) / 8U);
	signed_number = media_bits_se(&bits);
	expect((uint32_t)signed_number, (uint32_t)INT32_MIN, "signed Golomb negative endpoint");
	expect(bits.error, 0U, "negative endpoint is valid");
	memset(&writer, 0, sizeof(writer));
	golomb(&writer, UINT32_MAX);
	media_bits_init(&bits, writer.data, (writer.position + 7U) / 8U);
	signed_number = media_bits_se(&bits);
	expect(bits.error, 1U, "positive signed overflow refused");

	/* The positive signed endpoint uses the largest representable odd code below the overflow case. */
	memset(&writer, 0, sizeof(writer));
	golomb(&writer, (uint64_t)INT32_MAX * 2U - 1U);
	media_bits_init(&bits, writer.data, (writer.position + 7U) / 8U);
	signed_number = media_bits_se(&bits);
	expect(signed_number, INT32_MAX, "signed Golomb positive endpoint");
	expect(bits.error, 0U, "positive endpoint is valid");

	/* A valid unary prefix with a missing suffix cannot return a partial value. */
	media_bits_init(&bits, writer.data, (writer.position + 7U) / 8U);
	bits.count = writer.position - 1U;
	scalar = media_bits_ue(&bits);
	expect(bits.error, 1U, "truncated Golomb suffix");

	/* Succeeded: the integer and cursor contracts hold. */
	return;
}

/* Exercise prevention-byte validation, capacity errors and supported in-place unescaping. */
static void
test_rbsp(
	void)
{
	const uint8_t escaped[8] = {0U, 0U, 3U, 0U, 0U, 3U, 3U, 0x80U};
	const uint8_t expected[6] = {0U, 0U, 0U, 0U, 3U, 0x80U};
	uint8_t destination[8];
	uint8_t malformed[4] = {0U, 0U, 3U, 4U};
	size_t written;
	int error;
	int comparison;

	/* Chained escapes remove only prevention bytes, preserving protected zero and three. */
	error = media_rbsp_unescape(escaped, sizeof(escaped), destination, sizeof(destination), &written);
	expect(error, 0U, "RBSP escaped input");
	expect(written, sizeof(expected), "RBSP output size");
	comparison = memcmp(destination, expected, sizeof(expected));
	expect(comparison, 0U, "RBSP output bytes");
	memcpy(destination, escaped, sizeof(escaped));
	error = media_rbsp_unescape(destination, sizeof(destination), destination, sizeof(destination), &written);
	expect(error, 0U, "RBSP in-place input");
	comparison = memcmp(destination, expected, sizeof(expected));
	expect(comparison, 0U, "RBSP in-place bytes");

	/* A small destination is reported distinctly from malformed prevention syntax. */
	error = media_rbsp_unescape(escaped, sizeof(escaped), destination, 2U, &written);
	expect(error, ENOBUFS, "RBSP short destination");
	expect(written, 0U, "RBSP no partial publication");
	error = media_rbsp_unescape(malformed, sizeof(malformed), destination, sizeof(destination), &written);
	expect(error, EINVAL, "invalid protected byte");
	error = media_rbsp_unescape(malformed, 3U, destination, sizeof(destination), &written);
	expect(error, EINVAL, "dangling prevention byte");
	malformed[2] = 1U;
	error = media_rbsp_unescape(malformed, 3U, destination, sizeof(destination), &written);
	expect(error, EINVAL, "missing prevention byte");

	/* Succeeded: escaped input never changes unrelated payload bytes. */
	return;
}

/* Emit the ordinary ASC header and the three LC GA flags for controlled fixtures. */
static void
asc(
	struct syntax_writer *writer,
	unsigned object,
	unsigned rate,
	unsigned channels)
{
	/* Fixed fields precede LC's zero frame-length, core-dependency and extension flags. */
	memset(writer, 0, sizeof(*writer));
	put(writer, object, 5U);
	put(writer, rate, 4U);
	put(writer, channels, 4U);
	put(writer, 0U, 3U);

	/* Succeeded: the ordinary header ends at its byte boundary. */
	return;
}

/* Emit a six-channel PCE with matrix metadata, reordered groups and an optional bad tag. */
static void
pce(
	struct syntax_writer *writer,
	unsigned duplicate,
	unsigned comment)
{
	unsigned padding;

	/* Program tag three uses LC at 44.1 kHz with front pair plus center, back pair and LFE. */
	put(writer, 3U, 4U);
	put(writer, 1U, 2U);
	put(writer, 4U, 4U);
	put(writer, 2U, 4U);
	put(writer, 0U, 4U);
	put(writer, 1U, 4U);
	put(writer, 1U, 2U);
	put(writer, 0U, 3U);
	put(writer, 0U, 4U);
	put(writer, 0U, 1U);
	put(writer, 0U, 1U);
	put(writer, 1U, 1U);
	put(writer, 2U, 2U);
	put(writer, 1U, 1U);

	/* Front CPE zero and SCE zero use distinct element namespaces. */
	put(writer, 1U, 1U);
	put(writer, 0U, 4U);
	put(writer, 0U, 1U);
	put(writer, 0U, 4U);
	put(writer, 1U, 1U);
	put(writer, 1U - duplicate, 4U);
	put(writer, 0U, 4U);

	/* PCE comments follow alignment relative to the containing syntax span. */
	padding = (unsigned)((8U - writer->position % 8U) % 8U);
	put(writer, 0U, padding);
	put(writer, comment, 8U);

	/* Succeeded: no comment payload is emitted, allowing a truncation fixture. */
	return;
}

/* Exercise common LC ASC, unsupported extensions and PCE publication. */
static void
test_config(
	void)
{
	const uint8_t common[2] = {0x12U, 0x10U};
	const uint8_t extended_lc[5] = {0x12U, 0x10U, 0x56U, 0xe5U, 0U};
	struct syntax_writer writer;
	struct media_aac_config config;
	struct media_aac_config unchanged;
	int error;
	int comparison;

	/* Published LC vectors use the standard frequency and channel tables. */
	error = media_aac_config_parse(common, sizeof(common), &config);
	expect(error, 0U, "common LC ASC");
	expect(config.rate, 44100U, "ASC sampling frequency");
	expect(config.channels, 2U, "ASC stereo");
	error = media_aac_config_parse(extended_lc, sizeof(extended_lc), &config);
	expect(error, 0U, "explicit SBR-absent LC ASC");
	asc(&writer, 2U, 4U, 7U);
	error = media_aac_config_parse(writer.data, 2U, &config);
	expect(error, 0U, "configuration seven");
	expect(config.channels, 8U, "configuration seven is eight channels");

	/* Explicit sampling frequency is valid syntax and remains explicitly indexed. */
	memset(&writer, 0, sizeof(writer));
	put(&writer, 2U, 5U);
	put(&writer, 15U, 4U);
	put(&writer, 44100U, 24U);
	put(&writer, 2U, 4U);
	put(&writer, 0U, 3U);
	error = media_aac_config_parse(writer.data, 5U, &config);
	expect(error, 0U, "explicit ASC frequency");
	expect(config.rate_index, 15U, "explicit frequency marker retained");

	/* Non-LC profiles and enabled backward-compatible SBR are never degraded to LC. */
	asc(&writer, 5U, 4U, 2U);
	error = media_aac_config_parse(writer.data, 2U, &config);
	expect(error, ENOTSUP, "explicit HE-AAC refused");
	asc(&writer, 29U, 4U, 2U);
	error = media_aac_config_parse(writer.data, 2U, &config);
	expect(error, ENOTSUP, "explicit HE-AACv2 refused");
	asc(&writer, 2U, 4U, 2U);
	put(&writer, 0x2b7U, 11U);
	put(&writer, 5U, 5U);
	put(&writer, 1U, 1U);
	put(&writer, 4U, 4U);
	error = media_aac_config_parse(writer.data, (writer.position + 7U) / 8U, &config);
	expect(error, ENOTSUP, "backward-compatible SBR refused");
	asc(&writer, 2U, 13U, 2U);
	error = media_aac_config_parse(writer.data, 2U, &config);
	expect(error, EINVAL, "reserved frequency refused");
	error = media_aac_config_parse(common, 1U, &config);
	expect(error, EINVAL, "short ASC refused");

	/* PCE retains element identity, group ordering and matrix metadata. */
	asc(&writer, 2U, 4U, 0U);
	pce(&writer, 0U, 0U);
	error = media_aac_config_parse(writer.data, (writer.position + 7U) / 8U, &config);
	expect(error, 0U, "six-channel PCE");
	expect(config.channels, 6U, "PCE channel count");
	expect(config.element_count, 4U, "PCE element count");
	expect(config.elements[2].group, 2U, "PCE back group");
	expect(config.matrix_index, 2U, "PCE matrix metadata");
	expect(config.pseudo_surround, 1U, "PCE pseudo-surround metadata");
	unchanged = config;
	asc(&writer, 2U, 4U, 0U);
	pce(&writer, 1U, 0U);
	error = media_aac_config_parse(writer.data, (writer.position + 7U) / 8U, &config);
	expect(error, EINVAL, "duplicate PCE pair tag refused");
	comparison = memcmp(&config, &unchanged, sizeof(config));
	expect(comparison, 0U, "invalid ASC leaves output unchanged");
	asc(&writer, 2U, 4U, 0U);
	pce(&writer, 0U, 1U);
	error = media_aac_config_parse(writer.data, (writer.position + 7U) / 8U, &config);
	expect(error, EINVAL, "truncated PCE comment");

	/* Succeeded: accepted configurations contain no enabled unsupported tool. */
	return;
}

/* Emit an ADTS header with independently chosen block positions relative to its first payload. */
static void
adts(
	struct syntax_writer *writer,
	unsigned protected_frame,
	unsigned blocks,
	unsigned length)
{
	unsigned block;

	/* MPEG-4 LC stereo at 44.1 kHz uses the seven-byte fixed and variable header. */
	memset(writer, 0, sizeof(*writer));
	put(writer, 0xfffU, 12U);
	put(writer, 0U, 1U);
	put(writer, 0U, 2U);
	put(writer, 1U - protected_frame, 1U);
	put(writer, 1U, 2U);
	put(writer, 4U, 4U);
	put(writer, 0U, 1U);
	put(writer, 2U, 3U);
	put(writer, 0U, 4U);
	put(writer, length, 13U);
	put(writer, 0x7ffU, 11U);
	put(writer, blocks - 1U, 2U);

	/* CRC-protected multi-block positions refer to the first raw data block, not the frame header. */
	if (protected_frame != 0U) {
		/* Each test block consists of an END byte and its two-byte CRC. */
		for (block = 1U; block < blocks; block++)
			put(writer, block * 3U, 16U);
		put(writer, 0U, 16U);
	}

	/* Succeeded: zero payload bytes suffice for header tests, without claiming decoded AAC. */
	return;
}

/* Exercise CRC header boundaries, multi-block relative positions and incomplete frames. */
static void
test_adts(
	void)
{
	const uint8_t ordinary[8] = {0xffU, 0xf1U, 0x50U, 0x80U, 1U, 0x1fU, 0xfcU, 0xe0U};
	struct syntax_writer writer;
	struct media_aac_adts frame;
	unsigned blocks;
	unsigned length;
	int error;

	/* This literal independent header describes one LC stereo END-only block. */
	error = media_aac_adts_parse(ordinary, sizeof(ordinary), &frame);
	expect(error, 0U, "literal ADTS header");
	expect(frame.frame_size, 8U, "literal frame length");
	expect(frame.header_size, 7U, "unprotected header length");

	/* All four protected block counts retain the correct relative-to-absolute offsets. */
	for (blocks = 1U; blocks <= 4U; blocks++) {
		length = 9U + 2U * (blocks - 1U) + 3U * blocks;
		adts(&writer, 1U, blocks, length);
		error = media_aac_adts_parse(writer.data, length, &frame);
		expect(error, 0U, "protected ADTS frame");
		expect(frame.blocks, blocks, "protected block count");
		expect(frame.block_offset[0], 9U + 2U * (blocks - 1U), "first protected payload");

		/* A later block starts three bytes after the preceding block's payload start. */
		if (blocks > 1U)
			expect(frame.block_offset[1], frame.header_size + 3U, "relative protected block position");
		error = media_aac_adts_parse(writer.data, length - 1U, &frame);
		expect(error, EINVAL, "truncated ADTS frame");
	}

	/* Nonmonotone protected positions and non-LC profiles are distinct rejections. */
	adts(&writer, 1U, 2U, 17U);
	writer.data[8] = 0U;
	error = media_aac_adts_parse(writer.data, 17U, &frame);
	expect(error, EINVAL, "zero protected block position");
	adts(&writer, 0U, 1U, 8U);
	writer.data[2] &= 0x3fU;
	error = media_aac_adts_parse(writer.data, 8U, &frame);
	expect(error, ENOTSUP, "ADTS Main profile refused");
	writer.data[0] = 0U;
	error = media_aac_adts_parse(writer.data, 8U, &frame);
	expect(error, EINVAL, "bad ADTS sync");

	/* Succeeded: transport never publishes a truncated or inconsistent frame. */
	return;
}

/* Decode every normative symbol with an exact bit endpoint, also during once initialization. */
static void *
test_symbols(
	void *unused)
{
	struct syntax_writer writer;
	struct media_bits bits;
	const struct media_aac_codeword *word;
	uint16_t symbol;
	unsigned book;
	unsigned index;
	int error;

	/* The thread argument carries no test-only production control. */
	(void)unused;

	/* All twelve books must return the normative index and consume exactly one codeword. */
	for (book = 0U; book < 12U; book++) {
		/* Generated numeric facts are independent of the original tree construction. */
		for (index = 0U; index < media_aac_huffman_books[book].count; index++) {
			word = &media_aac_huffman_books[book].words[index];
			memset(&writer, 0, sizeof(writer));
			put(&writer, word->word, word->length);
			media_bits_init(&bits, writer.data, (writer.position + 7U) / 8U);
			bits.count = writer.position;
			error = media_aac_huffman_symbol(&bits, book, &symbol);
			expect(error, 0U, "normative codeword lookup");
			expect(symbol, word->symbol, "normative symbol index");
			expect(bits.position, word->length, "no lookup read-ahead");

			/* Removing the final bit must fail, even for codes ending in zero. */
			media_bits_init(&bits, writer.data, (writer.position + 7U) / 8U);
			bits.count = writer.position - 1U;
			error = media_aac_huffman_symbol(&bits, book, &symbol);
			expect(error, EINVAL, "short normative codeword");
		}
	}

	/* Succeeded: this reader observed only complete immutable lookup trees. */
	return NULL;
}

/* Exercise fixed normative zero tuples, signed tuple order and both escape sign positions. */
static void
test_spectral(
	void)
{
	struct syntax_writer writer;
	struct media_bits bits;
	int16_t coefficients[4];
	unsigned count;
	unsigned index;
	int difference;
	int error;
	const struct media_aac_codeword *word;

	/* Scalefactor zero difference and signed-book-one zero quad both use the single zero bit. */
	memset(&writer, 0, sizeof(writer));
	media_bits_init(&bits, writer.data, 1U);
	error = media_aac_scalefactor(&bits, &difference);
	expect(error, 0U, "zero scalefactor code");
	expect(difference, 0U, "zero scalefactor difference");
	media_bits_init(&bits, writer.data, 1U);
	error = media_aac_spectral(&bits, 1U, coefficients, &count);
	expect(error, 0U, "zero signed quad");
	expect(count, 4U, "quad dimension");
	expect(bits.position, 1U, "zero quad has no sign suffix");

	/* Book eleven's index 288 denotes sixteen/sixteen before sign and escape extensions. */
	word = NULL;
	for (index = 0U; index < media_aac_huffman_books[11].count; index++) {
		/* Find only the normative literal; the fixture writes signs and escapes itself. */
		if (media_aac_huffman_books[11].words[index].symbol == 288U)
			word = &media_aac_huffman_books[11].words[index];
	}

	/* The fixture requires the complete escape pair from the standard codebook. */
	if (word == NULL)
		exit(2);
	memset(&writer, 0, sizeof(writer));
	put(&writer, word->word, word->length);
	put(&writer, 2U, 2U);
	put(&writer, 0U, 1U);
	put(&writer, 1U, 4U);
	put(&writer, 0xffU, 8U);
	put(&writer, 0U, 1U);
	put(&writer, 0xfffU, 12U);
	media_bits_init(&bits, writer.data, (writer.position + 7U) / 8U);
	bits.count = writer.position;
	error = media_aac_spectral(&bits, 11U, coefficients, &count);
	expect(error, 0U, "two escapes with signs before payloads");
	expect((uint16_t)coefficients[0], (uint16_t)-17, "negative escaped first coefficient");
	expect(coefficients[1], 8191U, "positive normative magnitude endpoint");
	expect(coefficients[2], 0U, "pair clears unused coefficients");
	expect(bits.position, writer.position, "escape pair exact endpoint");

	/* A ninth unary one would require a coefficient above the allowed 8191. */
	memset(&writer, 0, sizeof(writer));
	put(&writer, word->word, word->length);
	put(&writer, 0U, 2U);
	put(&writer, 0x1ffU, 9U);
	media_bits_init(&bits, writer.data, (writer.position + 7U) / 8U);
	error = media_aac_spectral(&bits, 11U, coefficients, &count);
	expect(error, EINVAL, "excessive escape magnitude refused");

	/* Succeeded: tuple order and suffix syntax preserve the normative coefficient domain. */
	return;
}

/* Verify lexical order, signed books and zero-skipping sign syntax for every spectral book. */
static void
test_tuples(
	void)
{
	static const struct tuple_vector vectors[11] = {
	    {1U, 0U, 0U, 0U, {-1, -1, -1, -1}},
	    {2U, 80U, 0U, 0U, {1, 1, 1, 1}},
	    {3U, 65U, 2U, 3U, {2, -1, 0, 2}},
	    {4U, 1U, 1U, 1U, {0, 0, 0, -1}},
	    {5U, 0U, 0U, 0U, {-4, -4, 0, 0}},
	    {6U, 80U, 0U, 0U, {4, 4, 0, 0}},
	    {7U, 58U, 1U, 2U, {7, -2, 0, 0}},
	    {8U, 7U, 1U, 1U, {0, -7, 0, 0}},
	    {9U, 158U, 1U, 2U, {12, -2, 0, 0}},
	    {10U, 13U, 1U, 1U, {-1, 0, 0, 0}},
	    {11U, 16U, 0U, 1U, {0, 16, 0, 0}}};
	struct syntax_writer writer;
	struct media_bits bits;
	const struct media_aac_codeword *word;
	const struct tuple_vector *vector;
	int16_t coefficients[4];
	unsigned count;
	unsigned fixture;
	unsigned index;
	int error;

	/* Each fixture supplies its own expected coefficients rather than deriving them from the runtime radix. */
	for (fixture = 0U; fixture < 11U; fixture++) {
		vector = &vectors[fixture];
		word = NULL;

		/* Match the normative symbol to its numeric codeword, without reusing tuple conversion. */
		for (index = 0U; index < media_aac_huffman_books[vector->book].count; index++) {
			/* Only the requested normative entry supplies the Huffman prefix. */
			if (media_aac_huffman_books[vector->book].words[index].symbol == vector->symbol)
				word = &media_aac_huffman_books[vector->book].words[index];
		}

		/* A missing requested symbol makes the fixture invalid, never a skipped assertion. */
		if (word == NULL)
			exit(2);

		/* Append all signs and the minimum escape for the book-eleven sixteen coefficient. */
		memset(&writer, 0, sizeof(writer));
		put(&writer, word->word, word->length);
		put(&writer, vector->signs, vector->sign_count);

		/* The fixture with an escape appends separator zero and four zero suffix bits. */
		if (vector->book == 11U)
			put(&writer, 0U, 5U);

		/* Exact bit counts expose accidental sign consumption for zero coefficients. */
		media_bits_init(&bits, writer.data, (writer.position + 7U) / 8U);
		bits.count = writer.position;
		error = media_aac_spectral(&bits, vector->book, coefficients, &count);
		expect(error, 0U, "normative spectral tuple");
		expect(bits.position, writer.position, "tuple consumes only its signs and escapes");

		/* Compare transmitted frequency order and cleared unused pair entries independently. */
		for (index = 0U; index < 4U; index++)
			expect((uint16_t)coefficients[index], (uint16_t)vector->coefficients[index], "normative tuple coefficient");
	}

	/* Succeeded: every spectral book uses its own normative tuple domain. */
	return;
}

/* Parse every frame in encoder-produced ADTS without interpreting its AAC payload. */
static void
test_file(
	const char *path,
	unsigned rate,
	unsigned channels)
{
	FILE *file;
	uint8_t data[65536];
	struct media_aac_adts frame;
	size_t size;
	size_t offset;
	unsigned frames;
	int error;

	/* Read only the small independently generated fixture supplied by the runner. */
	file = fopen(path, "rb");
	if (file == NULL)
		exit(2);
	size = fread(data, 1U, sizeof(data), file);
	error = fclose(file);
	expect(error, 0U, "close ADTS fixture");

	/* Consume each complete frame and verify all emitted metadata, not just the first header. */
	offset = 0U;
	frames = 0U;
	while (offset < size) {
		error = media_aac_adts_parse(data + offset, size - offset, &frame);
		expect(error, 0U, "real ADTS frame");
		expect(frame.config.rate, rate, "real ADTS sample rate");
		expect(frame.config.channels, channels, "real ADTS channel count");
		offset += frame.frame_size;
		frames++;
	}

	/* An empty fixture cannot make the parsing loop vacuously pass. */
	if (frames < 4U)
		exit(2);
	expect(offset, size, "real ADTS consumed exactly");

	/* Succeeded: the entire real fixture was framed without trailing-byte loss. */
	return;
}
