/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of riff.c: files built by hand from RFC 9649's figures (the
 * RIFF header, the VP8X chunk, optional chunks, the bitstream chunks) and
 * RFC 6386 9.1's frame tag, the errors of each step, files cut at every
 * length, and random bytes.  Every call parses a copy made at exactly the
 * input's size, so a read outside it is caught by the address sanitizer.
 */

#include "check.h"
#include "riff.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The file under construction: its bytes and their count. */
static uint8_t file[4096];
static size_t file_len;

static void put_le16(uint8_t *bytes, unsigned value);
static void put_le24(uint8_t *bytes, unsigned long value);
static void put_le32(uint8_t *bytes, unsigned long value);
static void file_start(void);
static void file_chunk(const char *tag, const uint8_t *payload, unsigned long size, unsigned long declared);
static void file_end(void);
static size_t make_vp8_frame(uint8_t *bytes, unsigned tag_bits, unsigned width, unsigned height);
static void make_vp8x(uint8_t *payload, unsigned flags, unsigned long width, unsigned long height);
static void make_vp8l(uint8_t *bytes, unsigned width, unsigned height, unsigned alpha, unsigned version);
static int parse_exact(const uint8_t *data, size_t size, int complete, struct webpc_container *out);
static int parse_both(const uint8_t *data, size_t size);
static void check_invariants(const uint8_t *data, size_t size, int complete, int error, const struct webpc_container *out);
static void test_simple(void);
static void test_bare(void);
static void test_extended(void);
static void test_alpha_rules(void);
static void test_riff_errors(void);
static void test_vp8x_errors(void);
static void test_chunk_errors(void);
static void test_lossy_header_errors(void);
static void test_lossless_header_errors(void);
static void test_animation(void);
static void test_incomplete(void);
static void test_cut_sweep(void);
static void test_random(void);

/*
 * Runs the tests of riff.c.
 */
int
main(
	void)
{
	int status;

	/* Each group in turn. */
	test_simple();
	test_bare();
	test_extended();
	test_alpha_rules();
	test_riff_errors();
	test_vp8x_errors();
	test_chunk_errors();
	test_lossy_header_errors();
	test_lossless_header_errors();
	test_animation();
	test_incomplete();
	test_cut_sweep();
	test_random();

	/* The summary line. */
	status = check_finish("riff");
	return status;
}

/* Stores a 16-bit little-endian value. */
static void
put_le16(
	uint8_t *bytes,
	unsigned value)
{
	/* The low byte first. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)((value >> 8) & 0xffU);
}

/* Stores a 24-bit little-endian value. */
static void
put_le24(
	uint8_t *bytes,
	unsigned long value)
{
	/* The low byte first. */
	bytes[0] = (uint8_t)(value & 0xffUL);
	bytes[1] = (uint8_t)((value >> 8) & 0xffUL);
	bytes[2] = (uint8_t)((value >> 16) & 0xffUL);
}

/* Stores a 32-bit little-endian value. */
static void
put_le32(
	uint8_t *bytes,
	unsigned long value)
{
	/* The low byte first. */
	bytes[0] = (uint8_t)(value & 0xffUL);
	bytes[1] = (uint8_t)((value >> 8) & 0xffUL);
	bytes[2] = (uint8_t)((value >> 16) & 0xffUL);
	bytes[3] = (uint8_t)((value >> 24) & 0xffUL);
}

/* Starts a RIFF file: "RIFF", a File Size of 4 for now, "WEBP". */
static void
file_start(void)
{
	/* RFC 9649 Figure 1: the RIFF header. */
	memcpy(file, "RIFF", 4);
	put_le32(file + 4, 4UL);
	memcpy(file + 8, "WEBP", 4);
	file_len = 12;
}

/*
 * Appends a chunk: its tag, the declared size (not necessarily the real
 * one), the payload, and the pad byte of an odd real size.
 */
static void
file_chunk(
	const char *tag,
	const uint8_t *payload,
	unsigned long size,
	unsigned long declared)
{
	/* RFC 9649 Figure 2: FourCC, size, payload, padding. */
	memcpy(file + file_len, tag, 4);
	put_le32(file + file_len + 4, declared);
	file_len += 8;
	if (size != 0UL)
		memcpy(file + file_len, payload, (size_t)size);
	file_len += (size_t)size;
	if ((size & 1UL) != 0UL) {
		file[file_len] = 0;
		file_len++;
	}
}

/* Sets File Size to what the file holds: everything after its own field. */
static void
file_end(void)
{
	/* The file is File Size + 8 bytes long. */
	put_le32(file + 4, (unsigned long)(file_len - 8));
}

/*
 * Writes a lossy frame's first 10 bytes: the 3-byte tag from tag_bits
 * (RFC 6386 9.1: bit 0 key frame is 0, bits 1..3 version, bit 4 show_frame,
 * bits 5..23 first partition size), the start code 9d 01 2a, and the two
 * 16-bit sizes as given.
 */
static size_t
make_vp8_frame(
	uint8_t *bytes,
	unsigned tag_bits,
	unsigned width,
	unsigned height)
{
	/* The tag, then the start code, then the sizes. */
	put_le24(bytes, (unsigned long)tag_bits);
	bytes[3] = 0x9d;
	bytes[4] = 0x01;
	bytes[5] = 0x2a;
	put_le16(bytes + 6, width);
	put_le16(bytes + 8, height);

	/* Ten bytes were written. */
	return 10;
}

/* Writes a VP8X payload (RFC 9649 Figure 7): the flag byte, 3 reserved bytes, the canvas sides minus one. */
static void
make_vp8x(
	uint8_t *payload,
	unsigned flags,
	unsigned long width,
	unsigned long height)
{
	/* The flags and the reserved bytes. */
	payload[0] = (uint8_t)flags;
	payload[1] = 0;
	payload[2] = 0;
	payload[3] = 0;

	/* The canvas sides, each stored minus one. */
	put_le24(payload + 4, width - 1UL);
	put_le24(payload + 7, height - 1UL);
}

/*
 * Writes a lossless stream's 5-byte header (RFC 9649 3.4): the signature
 * 0x2f, then width-1 in 14 bits, height-1 in 14 bits, alpha_is_used in 1
 * bit and the version in 3 bits, from the least significant bit.
 */
static void
make_vp8l(
	uint8_t *bytes,
	unsigned width,
	unsigned height,
	unsigned alpha,
	unsigned version)
{
	unsigned long fields;

	/* The packed fields. */
	fields = (unsigned long)(width - 1U);
	fields |= (unsigned long)(height - 1U) << 14;
	fields |= (unsigned long)alpha << 28;
	fields |= (unsigned long)version << 29;

	/* The signature, then the fields. */
	bytes[0] = 0x2f;
	put_le32(bytes + 1, fields);
}

/*
 * Parses a copy of the data made at exactly its size, so the sanitizer sees
 * any read outside it; a zero size passes NULL.
 */
static int
parse_exact(
	const uint8_t *data,
	size_t size,
	int complete,
	struct webpc_container *out)
{
	uint8_t *copy;
	int error;

	/* A zero-size input is passed as NULL, as the interface allows. */
	if (size == 0) {
		error = webpc_riff_parse(NULL, 0, complete, out);
		check_invariants(NULL, 0, complete, error, out);
		return error;
	}

	/* Copies the input into a block of exactly its size. */
	copy = (uint8_t *)malloc(size);
	if (copy == NULL)
		abort();
	memcpy(copy, data, size);

	/* Parses the copy and checks the answer's pointers lie in it. */
	error = webpc_riff_parse(copy, size, complete, out);
	check_invariants(copy, size, complete, error, out);

	/* Releases the copy; the answer's pointers are not used after this. */
	free(copy);

	/* Reports the parse's status. */
	return error;
}

/* Parses with complete 1 and 0 and expects the same status from both. */
static int
parse_both(
	const uint8_t *data,
	size_t size)
{
	struct webpc_container out;
	int whole;
	int part;

	/* Both ways. */
	whole = parse_exact(data, size, 1, &out);
	part = parse_exact(data, size, 0, &out);
	CHECK(whole == part);

	/* The status they gave. */
	return whole;
}

/* Checks what every success promises about the answer's pointers and sizes. */
static void
check_invariants(
	const uint8_t *data,
	size_t size,
	int complete,
	int error,
	const struct webpc_container *out)
{
	size_t left;

	/* Only a failure leaves the answer unspecified, and only EINVAL and ENOBUFS are used. */
	if (error != 0) {
		CHECK(error == EINVAL || error == ENOBUFS);
		return;
	}

	/* A success has a size, and a format once the bitstream was reached. */
	CHECK(out->width >= 1U);
	CHECK(out->height >= 1U);
	CHECK(out->has_alpha == 0 || out->has_alpha == 1);

	/* The bitstream lies inside the data. */
	if (out->bitstream != NULL) {
		CHECK(out->format == WEBPC_FORMAT_LOSSY || out->format == WEBPC_FORMAT_LOSSLESS);
		CHECK(out->bitstream >= data);
		CHECK(out->bitstream <= data + size);
		left = (size_t)((data + size) - out->bitstream);
		CHECK(out->available_size <= left);
		if (complete != 0)
			CHECK(out->bitstream_size <= out->available_size);
	}

	/* The alpha payload lies inside the data. */
	if (out->alpha != NULL) {
		CHECK(out->alpha >= data);
		left = (size_t)((data + size) - out->alpha);
		CHECK(out->alpha_size <= left);
	}
}

/* Checks the simple lossy and lossless files (RFC 9649 2.5, 2.6). */
static void
test_simple(void)
{
	uint8_t frame[16];
	uint8_t stream[8];
	uint8_t *copy;
	struct webpc_container out;
	int error;

	/* A simple lossy file, 100x50: tag 0x30 is show_frame with a first partition size of 1. */
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	CHECK(file_len == 30U);
	CHECK(webpc_le32(file + 4) == 22UL);
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSY);
	CHECK(out.width == 100U);
	CHECK(out.height == 50U);
	CHECK(out.has_alpha == 0);
	CHECK(out.has_animation == 0);
	CHECK(out.extended == 0);
	CHECK(out.alpha == NULL);
	CHECK(out.alpha_size == 0U);
	CHECK(out.bitstream_size == 10U);
	CHECK(out.available_size == 10U);

	/* The same bytes read as features only. */
	error = parse_exact(file, file_len, 0, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSY);
	CHECK(out.width == 100U);
	CHECK(out.height == 50U);

	/* The bitstream is where the frame is, at byte 20 (12 plus the chunk header). */
	copy = (uint8_t *)malloc(file_len);
	if (copy == NULL)
		abort();
	memcpy(copy, file, file_len);
	error = webpc_riff_parse(copy, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.bitstream == copy + 20);
	free(copy);

	/* The scale bits above the 14 size bits are read and dropped: 0x4000 | 100 is 100. */
	make_vp8_frame(frame, 0x30U, 0x4000U | 100U, 0xc000U | 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.width == 100U);
	CHECK(out.height == 50U);

	/* The largest lossy size, 16383, and version 3 (bits 1..3 = 3, tag 0x36). */
	make_vp8_frame(frame, 0x36U, 16383U, 16383U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.width == 16383U);
	CHECK(out.height == 16383U);

	/* A simple lossless file, 3x2 with alpha: the hand-worked header is 2f 02 40 00 10. */
	make_vp8l(stream, 3U, 2U, 1U, 0U);
	CHECK(stream[0] == 0x2fU);
	CHECK(stream[1] == 0x02U);
	CHECK(stream[2] == 0x40U);
	CHECK(stream[3] == 0x00U);
	CHECK(stream[4] == 0x10U);
	file_start();
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_end();
	CHECK(file_len == 26U);
	CHECK(webpc_le32(file + 4) == 18UL);
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSLESS);
	CHECK(out.width == 3U);
	CHECK(out.height == 2U);
	CHECK(out.has_alpha == 1);
	CHECK(out.extended == 0);
	CHECK(out.bitstream_size == 5U);
	CHECK(out.available_size == 6U);
	error = parse_exact(file, file_len, 0, &out);
	CHECK(error == 0);
	CHECK(out.has_alpha == 1);

	/* Without the alpha bit, and at the largest lossless size, 16384. */
	make_vp8l(stream, 16384U, 16384U, 0U, 0U);
	CHECK(stream[1] == 0xffU);
	CHECK(stream[2] == 0xffU);
	CHECK(stream[3] == 0xffU);
	CHECK(stream[4] == 0x0fU);
	file_start();
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.width == 16384U);
	CHECK(out.height == 16384U);
	CHECK(out.has_alpha == 0);

	/* Bytes after File Size + 8 are never used: the bitstream is cut to the file's end. */
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	memcpy(file + file_len, "junk!", 5);
	error = parse_exact(file, file_len + 5U, 1, &out);
	CHECK(error == 0);
	CHECK(out.available_size == 10U);
	CHECK(out.bitstream_size == 10U);
	error = parse_exact(file, file_len + 5U, 0, &out);
	CHECK(error == 0);
	CHECK(out.available_size == 10U);
}

/* Checks a bitstream with no RIFF header, in a chunk and bare. */
static void
test_bare(void)
{
	uint8_t data[32];
	struct webpc_container out;
	int error;
	size_t index;

	/* A bare lossy frame: 10 bytes of header and 6 of zeros, to reach 16 bytes. */
	memset(data, 0, sizeof(data));
	make_vp8_frame(data, 0x30U, 7U, 9U);
	error = parse_exact(data, 16U, 1, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSY);
	CHECK(out.width == 7U);
	CHECK(out.height == 9U);
	CHECK(out.extended == 0);
	CHECK(out.bitstream_size == 16U);
	CHECK(out.available_size == 16U);
	error = parse_exact(data, 16U, 0, &out);
	CHECK(error == 0);
	CHECK(out.bitstream_size == 16U);

	/* A bare lossless stream: its header and 7 zero bytes. */
	memset(data, 0, sizeof(data));
	make_vp8l(data, 3U, 2U, 1U, 0U);
	error = parse_exact(data, 12U, 1, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSLESS);
	CHECK(out.width == 3U);
	CHECK(out.height == 2U);
	CHECK(out.has_alpha == 1);
	CHECK(out.bitstream_size == 12U);

	/* Fewer than 12 bytes is too few, even for a whole lossless header. */
	for (index = 0; index < 12U; index++) {
		error = parse_both(data, index);
		CHECK(error == ENOBUFS);
	}

	/* A bare lossless stream with a version field that is not 0 is not recognised, so is read as lossy and fails. */
	memset(data, 0, sizeof(data));
	make_vp8l(data, 3U, 2U, 1U, 1U);
	error = parse_both(data, 12U);
	CHECK(error == EINVAL);

	/* A chunk with no RIFF header: "VP8 " with a 10-byte frame. */
	memset(data, 0, sizeof(data));
	memcpy(data, "VP8 ", 4);
	put_le32(data + 4, 10UL);
	make_vp8_frame(data + 8, 0x30U, 20U, 30U);
	error = parse_exact(data, 18U, 1, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSY);
	CHECK(out.width == 20U);
	CHECK(out.height == 30U);
	CHECK(out.bitstream_size == 10U);
	CHECK(out.available_size == 10U);

	/* A "VP8L" chunk with no RIFF header. */
	memset(data, 0, sizeof(data));
	memcpy(data, "VP8L", 4);
	put_le32(data + 4, 5UL);
	make_vp8l(data + 8, 5U, 6U, 0U, 0U);
	error = parse_exact(data, 14U, 1, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSLESS);
	CHECK(out.width == 5U);
	CHECK(out.height == 6U);
	CHECK(out.has_alpha == 0);

	/* A chunk size beyond the data fails when the data is the whole file, and reads the header when it is not. */
	memset(data, 0, sizeof(data));
	memcpy(data, "VP8 ", 4);
	put_le32(data + 4, 100UL);
	make_vp8_frame(data + 8, 0x30U, 20U, 30U);
	error = parse_exact(data, 18U, 1, &out);
	CHECK(error == ENOBUFS);
	error = parse_exact(data, 18U, 0, &out);
	CHECK(error == 0);
	CHECK(out.bitstream_size == 100U);
	CHECK(out.available_size == 10U);

	/* A chunk size above the format's limit is a failure even for data that may be cut. */
	put_le32(data + 4, 0xfffffff7UL);
	error = parse_exact(data, 18U, 0, &out);
	CHECK(error == EINVAL);

	/* "RIFX" is not "RIFF": the bytes are read as a bare frame and fail on the start code. */
	memset(data, 0, sizeof(data));
	memcpy(data, "RIFX", 4);
	memcpy(data + 8, "WEBP", 4);
	error = parse_both(data, 20U);
	CHECK(error == EINVAL);
}

/* Checks extended files: VP8X with optional chunks (RFC 9649 2.7). */
static void
test_extended(void)
{
	uint8_t vp8x[10];
	uint8_t frame[10];
	uint8_t stream[5];
	uint8_t alph[3];
	uint8_t icc[4];
	uint8_t odd[3];
	struct webpc_container out;
	uint8_t *copy;
	int error;

	/* VP8X (Alpha flag), ALPH of 3 bytes (padded), then a lossy frame, canvas 100x50. */
	make_vp8x(vp8x, WEBPC_VP8X_ALPHA, 100UL, 50UL);
	CHECK(vp8x[0] == 0x10U);
	CHECK(vp8x[4] == 99U);
	CHECK(vp8x[7] == 49U);
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	alph[0] = 0;
	alph[1] = 1;
	alph[2] = 2;
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ALPH", alph, 3UL, 3UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	CHECK(file_len == 12U + 18U + 12U + 18U);
	copy = (uint8_t *)malloc(file_len);
	if (copy == NULL)
		abort();
	memcpy(copy, file, file_len);
	error = webpc_riff_parse(copy, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSY);
	CHECK(out.extended == 1);
	CHECK(out.width == 100U);
	CHECK(out.height == 50U);
	CHECK(out.has_alpha == 1);
	CHECK(out.alpha == copy + 38);
	CHECK(out.alpha_size == 3U);
	CHECK(out.bitstream == copy + 12 + 18 + 12 + 8);
	CHECK(out.bitstream_size == 10U);
	free(copy);

	/* An ALPH chunk with no Alpha flag still gives has_alpha 1. */
	make_vp8x(vp8x, 0U, 100UL, 50UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ALPH", alph, 3UL, 3UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == 0);
	error = parse_exact(file, file_len, 1, &out);
	CHECK(out.has_alpha == 1);
	CHECK(out.alpha_size == 3U);

	/* Two ALPH chunks: the later one is kept. */
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ALPH", alph, 3UL, 3UL);
	file_chunk("ALPH", alph, 2UL, 2UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	copy = (uint8_t *)malloc(file_len);
	if (copy == NULL)
		abort();
	memcpy(copy, file, file_len);
	error = webpc_riff_parse(copy, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.alpha_size == 2U);
	CHECK(out.alpha == copy + 12 + 18 + 12 + 8);
	free(copy);

	/* An ALPH chunk of size 0 and of size 1 are kept as they are. */
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ALPH", alph, 0UL, 0UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.alpha != NULL);
	CHECK(out.alpha_size == 0U);
	CHECK(out.has_alpha == 1);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ALPH", alph, 1UL, 1UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.alpha_size == 1U);

	/* ICCP, an unknown chunk of odd size (padded), a lossless stream and a chunk after it. */
	make_vp8x(vp8x, WEBPC_VP8X_ICC | WEBPC_VP8X_EXIF, 3UL, 2UL);
	icc[0] = 1;
	icc[1] = 2;
	icc[2] = 3;
	icc[3] = 4;
	odd[0] = 7;
	odd[1] = 8;
	odd[2] = 9;
	make_vp8l(stream, 3U, 2U, 0U, 0U);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ICCP", icc, 4UL, 4UL);
	file_chunk("ABCD", odd, 3UL, 3UL);
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_chunk("EXIF", icc, 4UL, 4UL);
	file_end();
	copy = (uint8_t *)malloc(file_len);
	if (copy == NULL)
		abort();
	memcpy(copy, file, file_len);
	error = webpc_riff_parse(copy, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSLESS);
	CHECK(out.extended == 1);
	CHECK(out.width == 3U);
	CHECK(out.height == 2U);
	CHECK(out.has_alpha == 0);
	CHECK(out.alpha == NULL);
	CHECK(out.bitstream == copy + 12 + 18 + 12 + 12 + 8);
	CHECK(out.bitstream_size == 5U);
	CHECK(out.available_size == 6U + 12U);
	free(copy);

	/* A lossless file with an ALPH chunk: ALPH only sets has_alpha. */
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ALPH", alph, 3UL, 3UL);
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSLESS);
	CHECK(out.has_alpha == 1);
	CHECK(out.alpha_size == 3U);

	/* A canvas of the largest area under 2^32, 65536 x 65535, with a bitstream of another size, fails on the size. */
	make_vp8x(vp8x, 0U, 65536UL, 65535UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
}

/* Checks has_alpha: the VP8X flag, replaced by a lossless stream's bit, made 1 by ALPH. */
static void
test_alpha_rules(void)
{
	uint8_t vp8x[10];
	uint8_t frame[10];
	uint8_t stream[5];
	struct webpc_container out;
	unsigned flag;
	unsigned bit;
	int error;

	/* Every flag and bit combination of a lossless file: the bit decides. */
	for (flag = 0; flag < 2U; flag++) {
		for (bit = 0; bit < 2U; bit++) {
			make_vp8x(vp8x, flag != 0U ? WEBPC_VP8X_ALPHA : 0U, 3UL, 2UL);
			make_vp8l(stream, 3U, 2U, bit, 0U);
			file_start();
			file_chunk("VP8X", vp8x, 10UL, 10UL);
			file_chunk("VP8L", stream, 5UL, 5UL);
			file_end();
			error = parse_exact(file, file_len, 1, &out);
			CHECK(error == 0);
			CHECK(out.has_alpha == (int)bit);
		}
	}

	/* A lossy file: only the flag counts (the frame has no alpha bit). */
	make_vp8_frame(frame, 0x30U, 3U, 2U);
	make_vp8x(vp8x, 0U, 3UL, 2UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.has_alpha == 0);
	make_vp8x(vp8x, WEBPC_VP8X_ALPHA, 3UL, 2UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.has_alpha == 1);
}

/* Checks the errors of steps 1 and 2: the RIFF header. */
static void
test_riff_errors(void)
{
	uint8_t frame[10];
	struct webpc_container out;
	int error;
	size_t index;

	/* A valid simple file to damage. */
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();

	/* Shorter than 12 bytes of a RIFF file is ENOBUFS, and a zero size with NULL too. */
	for (index = 0; index < 12U; index++) {
		error = parse_both(file, index);
		CHECK(error == ENOBUFS);
	}
	error = webpc_riff_parse(NULL, 0, 1, &out);
	CHECK(error == ENOBUFS);
	error = webpc_riff_parse(NULL, 0, 0, &out);
	CHECK(error == ENOBUFS);

	/* RIFF without "WEBP": any other form type. */
	file[8] = 'W';
	file[9] = 'E';
	file[10] = 'B';
	file[11] = 'Q';
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	file[11] = 'P';
	error = parse_both(file, file_len);
	CHECK(error == 0);
	file[8] = 'X';
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	file[8] = 'W';

	/* File Size of 11 is below the 12 a "WEBP" and a chunk header need; 12 is the least. */
	put_le32(file + 4, 11UL);
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	put_le32(file + 4, 0UL);
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* File Size above 2^32 - 10: the limit is 0xfffffff6 and 0xfffffff7 breaks it. */
	put_le32(file + 4, 0xfffffff7UL);
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	put_le32(file + 4, 0xffffffffUL);
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* The limit itself passes this check: whole data is then missing for complete 1, while a part is read. */
	put_le32(file + 4, 0xfffffff6UL);
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == ENOBUFS);

	/* A File Size beyond the data is ENOBUFS for the whole file. */
	file_end();
	put_le32(file + 4, (unsigned long)(file_len - 8) + 1UL);
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == ENOBUFS);

	/* A File Size of exactly 12 with 12 bytes of data has no chunk: ENOBUFS (VP8X step). */
	file_start();
	put_le32(file + 4, 12UL);
	error = parse_both(file, 12U);
	CHECK(error == ENOBUFS);

	/* File Size 12 and a chunk header of 8 bytes: the bytes after the file's end are not used. */
	memset(file + 12, 0, 16);
	put_le32(file + 4, 12UL);
	error = parse_both(file, 28U);
	CHECK(error == ENOBUFS);
}

/* Checks the errors of step 3: the VP8X chunk. */
static void
test_vp8x_errors(void)
{
	uint8_t vp8x[10];
	uint8_t frame[10];
	uint8_t stream[5];
	struct webpc_container out;
	int error;
	size_t index;

	/* A good extended file, 100x50, to damage. */
	make_vp8x(vp8x, 0U, 100UL, 50UL);
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == 0);

	/* A VP8X size of 9 and of 11 (the payload is exactly 10). */
	put_le32(file + 16, 9UL);
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	put_le32(file + 16, 11UL);
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	put_le32(file + 16, 10UL);

	/* Cut inside the 18 bytes of the chunk: ENOBUFS, whether the data is the whole file or not. */
	for (index = 12U; index < 30U; index++) {
		error = parse_both(file, index);
		CHECK(error == ENOBUFS);
	}

	/* A canvas of 65536 x 65536: the area is 2^32, which is too large. */
	make_vp8x(vp8x, 0U, 65536UL, 65536UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A canvas of the largest sides, 2^24 x 2^24, too. */
	make_vp8x(vp8x, 0U, 16777216UL, 16777216UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* One short of the limit: 65536 x 65535 (area 4294901760) passes the area check and fails on the bitstream's size. */
	make_vp8x(vp8x, 0U, 65536UL, 65535UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A VP8X chunk with no RIFF header. */
	make_vp8x(vp8x, 0U, 100UL, 50UL);
	memset(file, 0, sizeof(file));
	memcpy(file, "VP8X", 4);
	put_le32(file + 4, 10UL);
	memcpy(file + 8, vp8x, 10);
	memcpy(file + 18, "VP8 ", 4);
	put_le32(file + 22, 10UL);
	memcpy(file + 26, frame, 10);
	error = parse_both(file, 36U);
	CHECK(error == EINVAL);

	/* A VP8X chunk with no RIFF header, cut inside its 18 bytes: it is too short first. */
	error = parse_both(file, 12U);
	CHECK(error == ENOBUFS);
	error = parse_both(file, 17U);
	CHECK(error == ENOBUFS);

	/* Chunk sizes of the sum of the chunks above File Size. */
	make_vp8x(vp8x, 0U, 3UL, 2UL);
	make_vp8l(stream, 3U, 2U, 0U, 0U);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ABCD", frame, 4UL, 4UL);
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == 0);
	put_le32(file + 4, (unsigned long)(file_len - 8) - 2UL);
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A chunk of odd size is padded to even, so the sum is the same, and one byte less breaks it. */
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ABCD", frame, 3UL, 3UL);
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == 0);
	put_le32(file + 4, (unsigned long)(file_len - 8) - 1UL);
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A File Size of 30 holds a chunk header after the VP8X chunk but not the chunk (22 + 14 > 30). */
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_end();
	put_le32(file + 4, 30UL);
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A File Size that ends the file 7 bytes after the VP8X chunk: no chunk header is there. */
	put_le32(file + 4, 29UL);
	error = webpc_riff_parse(file, file_len, 1, &out);
	CHECK(error == ENOBUFS);
	error = webpc_riff_parse(file, file_len, 0, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_UNDEFINED);
}

/* Checks the errors of steps 5 and 6: the optional chunks and the bitstream chunk's size. */
static void
test_chunk_errors(void)
{
	uint8_t vp8x[10];
	uint8_t frame[10];
	uint8_t stream[5];
	struct webpc_container out;
	int error;

	/* An optional chunk of size 0xfffffff7 breaks the limit. */
	make_vp8x(vp8x, 0U, 3UL, 2UL);
	make_vp8l(stream, 3U, 2U, 0U, 0U);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ABCD", frame, 0UL, 0xfffffff7UL);
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A size of the limit itself, 0xfffffff6, fails on the sum instead. */
	put_le32(file + 12 + 18 + 4, 0xfffffff6UL);
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A bitstream chunk whose size is above File Size - 12 (here 11 against 10). */
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 11UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* In an extended file a huge bitstream chunk already fails on the sum of the chunks. */
	make_vp8x(vp8x, 0U, 100UL, 50UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 0xfffffff0UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A lossless chunk's size is checked the same way. */
	file_start();
	file_chunk("VP8L", stream, 5UL, 100UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A file with no bitstream chunk: after the optional chunks the data ends. */
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ABCD", frame, 4UL, 4UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == ENOBUFS);
	error = parse_exact(file, file_len, 0, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_UNDEFINED);
	CHECK(out.bitstream == NULL);

	/* A simple file whose first chunk is not a bitstream chunk: read as a bare frame, it fails. */
	file_start();
	file_chunk("ABCD", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A canvas that is not the bitstream's size, in each side. */
	make_vp8x(vp8x, 0U, 101UL, 50UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	make_vp8x(vp8x, 0U, 100UL, 51UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	make_vp8x(vp8x, 0U, 100UL, 50UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == 0);
}

/* Checks the errors of a lossy frame's header (step 7). */
static void
test_lossy_header_errors(void)
{
	uint8_t frame[16];
	struct webpc_container out;
	int error;
	unsigned bits;

	/* A frame with the tag bits given, in a simple file, parsed with both settings. */
	memset(frame, 0, sizeof(frame));

	/* An interframe (bit 0 set). */
	make_vp8_frame(frame, 0x31U, 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* Version 4 (bits 1..3 = 4: 0x30 | 8) fails, and the versions 0..3 pass. */
	make_vp8_frame(frame, 0x38U, 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	for (bits = 0; bits < 4U; bits++) {
		make_vp8_frame(frame, 0x30U | (bits << 1), 100U, 50U);
		file_start();
		file_chunk("VP8 ", frame, 10UL, 10UL);
		file_end();
		error = parse_both(file, file_len);
		CHECK(error == 0);
	}
	for (bits = 4U; bits < 8U; bits++) {
		make_vp8_frame(frame, 0x30U | (bits << 1), 100U, 50U);
		file_start();
		file_chunk("VP8 ", frame, 10UL, 10UL);
		file_end();
		error = parse_both(file, file_len);
		CHECK(error == EINVAL);
	}

	/* show_frame 0. */
	make_vp8_frame(frame, 0x20U, 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A first partition of 9 bytes fits a 10-byte chunk; 10 does not (it must be below the size). */
	make_vp8_frame(frame, 0x10U | (9U << 5), 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == 0);
	make_vp8_frame(frame, 0x10U | (10U << 5), 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A first partition size that uses the top of the 19 bits (the tag's bits 5..23). */
	make_vp8_frame(frame, 0x10U, 100U, 50U);
	put_le24(frame, 0x10UL | (0x7ffffUL << 5));
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A width of 0 and a height of 0, also when only the scale bits are set. */
	make_vp8_frame(frame, 0x30U, 0U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	make_vp8_frame(frame, 0x30U, 100U, 0U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	make_vp8_frame(frame, 0x30U, 0x4000U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	make_vp8_frame(frame, 0x30U, 100U, 0xc000U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A start code that differs in each of its three bytes. */
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	frame[3] = 0x9c;
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	frame[4] = 0x00;
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	frame[5] = 0x2b;
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A frame cut inside its 10 bytes: the chunk holds 8. */
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 8UL, 8UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == ENOBUFS);

	/*
	 * The header is read from the bytes the data holds, not the declared
	 * size: 5 declared, 10 held, and a first partition of 5 which does
	 * not fit the declared size (reading only 5 bytes would be ENOBUFS).
	 */
	make_vp8_frame(frame, 0x10U | (5U << 5), 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 5UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == EINVAL);
}

/* Checks the errors of a lossless stream's header (step 7). */
static void
test_lossless_header_errors(void)
{
	uint8_t stream[5];
	struct webpc_container out;
	int error;
	unsigned version;

	/* A wrong signature byte. */
	make_vp8l(stream, 3U, 2U, 0U, 0U);
	stream[0] = 0x2e;
	file_start();
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);

	/* A version of 1 and of every other nonzero value. */
	for (version = 1; version < 8U; version++) {
		make_vp8l(stream, 3U, 2U, 0U, version);
		file_start();
		file_chunk("VP8L", stream, 5UL, 5UL);
		file_end();
		error = parse_both(file, file_len);
		CHECK(error == EINVAL);
	}

	/* A stream with 4 bytes in a chunk of 4: the header needs 5. */
	make_vp8l(stream, 3U, 2U, 0U, 0U);
	file_start();
	file_chunk("VP8L", stream, 4UL, 4UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == ENOBUFS);

	/* The right header with every dimension bit set decodes to 16384 x 16384. */
	stream[0] = 0x2f;
	put_le32(stream + 1, 0x0fffffffUL);
	file_start();
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_end();
	error = parse_exact(file, file_len, 1, &out);
	CHECK(error == 0);
	CHECK(out.width == 16384U);
	CHECK(out.height == 16384U);
	CHECK(out.has_alpha == 0);
}

/* Checks an animation: it ends the parse after the VP8X chunk (RFC 9649 2.7). */
static void
test_animation(void)
{
	uint8_t vp8x[10];
	struct webpc_container out;
	int error;
	int complete;

	/* VP8X with only the Animation flag, and nothing after it. */
	make_vp8x(vp8x, WEBPC_VP8X_ANIMATION, 400UL, 300UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_end();
	for (complete = 0; complete < 2; complete++) {
		error = parse_exact(file, file_len, complete, &out);
		CHECK(error == 0);
		CHECK(out.format == WEBPC_FORMAT_UNDEFINED);
		CHECK(out.has_animation == 1);
		CHECK(out.extended == 1);
		CHECK(out.width == 400U);
		CHECK(out.height == 300U);
		CHECK(out.has_alpha == 0);
		CHECK(out.bitstream == NULL);
		CHECK(out.alpha == NULL);
	}

	/* With the Alpha flag too, and chunks after it that are not read. */
	make_vp8x(vp8x, WEBPC_VP8X_ANIMATION | WEBPC_VP8X_ALPHA, 400UL, 300UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ANIM", vp8x, 6UL, 6UL);
	file_end();
	for (complete = 0; complete < 2; complete++) {
		error = parse_exact(file, file_len, complete, &out);
		CHECK(error == 0);
		CHECK(out.has_animation == 1);
		CHECK(out.has_alpha == 1);
		CHECK(out.format == WEBPC_FORMAT_UNDEFINED);
	}

	/* An animation's canvas must still satisfy the VP8X checks. */
	make_vp8x(vp8x, WEBPC_VP8X_ANIMATION, 65536UL, 65536UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_end();
	error = parse_both(file, file_len);
	CHECK(error == EINVAL);
}

/* Checks a file cut short with complete 0: what is known is reported (RFC 9649 2.7). */
static void
test_incomplete(void)
{
	uint8_t vp8x[10];
	uint8_t frame[10];
	uint8_t stream[5];
	uint8_t alph[3];
	uint8_t longer[16];
	struct webpc_container out;
	int error;

	/* VP8X (no flags) and nothing else, File Size claims more: the canvas and an UNDEFINED format. */
	make_vp8x(vp8x, 0U, 100UL, 50UL);
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_exact(file, 30U, 0, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_UNDEFINED);
	CHECK(out.width == 100U);
	CHECK(out.height == 50U);
	CHECK(out.extended == 1);
	CHECK(out.has_alpha == 0);
	CHECK(out.bitstream == NULL);
	error = parse_exact(file, 30U, 1, &out);
	CHECK(error == ENOBUFS);

	/* The same with the Alpha flag. */
	make_vp8x(vp8x, WEBPC_VP8X_ALPHA, 100UL, 50UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_exact(file, 30U, 0, &out);
	CHECK(error == 0);
	CHECK(out.has_alpha == 1);

	/* The VP8 chunk header and 5 bytes of the frame: the format is known, the frame's size is not. */
	make_vp8x(vp8x, 0U, 100UL, 50UL);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_exact(file, 30U + 8U + 5U, 0, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSY);
	CHECK(out.width == 100U);
	CHECK(out.height == 50U);
	CHECK(out.bitstream == NULL);

	/* The same for a lossless stream cut at 4 bytes, with the flag's alpha kept. */
	make_vp8x(vp8x, WEBPC_VP8X_ALPHA, 3UL, 2UL);
	make_vp8l(stream, 3U, 2U, 0U, 0U);
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("VP8L", stream, 5UL, 5UL);
	file_end();
	error = parse_exact(file, 30U + 8U + 4U, 0, &out);
	CHECK(error == 0);
	CHECK(out.format == WEBPC_FORMAT_LOSSLESS);
	CHECK(out.has_alpha == 1);
	CHECK(out.bitstream == NULL);

	/* An ALPH chunk read before the cut counts toward has_alpha, with a bare cut after it. */
	make_vp8x(vp8x, 0U, 100UL, 50UL);
	alph[0] = 0;
	alph[1] = 0;
	alph[2] = 0;
	file_start();
	file_chunk("VP8X", vp8x, 10UL, 10UL);
	file_chunk("ALPH", alph, 3UL, 3UL);
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_exact(file, 30U + 12U + 3U, 0, &out);
	CHECK(error == 0);
	CHECK(out.has_alpha == 1);
	CHECK(out.format == WEBPC_FORMAT_UNDEFINED);
	CHECK(out.bitstream == NULL);

	/* A cut inside the ALPH chunk: it is not found, so the alpha stays as the flag says. */
	error = parse_exact(file, 30U + 11U, 0, &out);
	CHECK(error == 0);
	CHECK(out.has_alpha == 0);

	/* Without a VP8X chunk a cut is a failure. */
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	file_start();
	file_chunk("VP8 ", frame, 10UL, 10UL);
	file_end();
	error = parse_exact(file, 12U + 8U + 9U, 0, &out);
	CHECK(error == ENOBUFS);

	/* A simple file cut inside a longer frame: the header is there, so the features are read. */
	memset(longer, 0, sizeof(longer));
	make_vp8_frame(longer, 0x30U, 100U, 50U);
	file_start();
	file_chunk("VP8 ", longer, 16UL, 16UL);
	file_end();
	error = parse_exact(file, 12U + 8U + 10U, 0, &out);
	CHECK(error == 0);
	CHECK(out.bitstream_size == 16U);
	CHECK(out.available_size == 10U);
	CHECK(out.width == 100U);
	error = parse_exact(file, 12U + 8U + 10U, 1, &out);
	CHECK(error == ENOBUFS);
}

/* Cuts valid files at every length and checks both settings on exactly sized copies. */
static void
test_cut_sweep(void)
{
	uint8_t vp8x[10];
	uint8_t frame[16];
	uint8_t stream[5];
	uint8_t alph[3];
	uint8_t other[5];
	struct webpc_container out;
	static const size_t first_ok[4] = { 30U, 25U, 30U, 30U };
	unsigned kind;
	size_t length;
	size_t whole;
	int error;

	/* A lossy frame of 16 bytes (10 of header) and a plain payload for the other chunks. */
	memset(frame, 0, sizeof(frame));
	make_vp8_frame(frame, 0x30U, 100U, 50U);
	make_vp8l(stream, 3U, 2U, 1U, 0U);
	alph[0] = 0;
	alph[1] = 1;
	alph[2] = 2;
	memset(other, 5, sizeof(other));

	/* Four files: simple lossy, simple lossless, extended lossy with ALPH, extended lossless with chunks. */
	for (kind = 0; kind < 4U; kind++) {
		file_start();
		if (kind == 0U) {
			file_chunk("VP8 ", frame, 16UL, 16UL);
		} else if (kind == 1U) {
			file_chunk("VP8L", stream, 5UL, 5UL);
		} else if (kind == 2U) {
			make_vp8x(vp8x, 0U, 100UL, 50UL);
			file_chunk("VP8X", vp8x, 10UL, 10UL);
			file_chunk("ALPH", alph, 3UL, 3UL);
			file_chunk("VP8 ", frame, 16UL, 16UL);
		} else {
			make_vp8x(vp8x, 0U, 3UL, 2UL);
			file_chunk("VP8X", vp8x, 10UL, 10UL);
			file_chunk("ICCP", other, 5UL, 5UL);
			file_chunk("VP8L", stream, 5UL, 5UL);
			file_chunk("EXIF", other, 5UL, 5UL);
		}
		file_end();
		whole = file_len;

		/* The whole file parses with both settings. */
		error = parse_both(file, whole);
		CHECK(error == 0);

		/* Every shorter length: the whole file's check fails with ENOBUFS, the features check never crashes. */
		for (length = 0; length < whole; length++) {
			error = parse_exact(file, length, 1, &out);
			CHECK(error == ENOBUFS);
			error = parse_exact(file, length, 0, &out);

			/*
			 * The features are read once the bitstream's header is there
			 * (a simple file) or the VP8X chunk is (an extended file).
			 */
			if (length >= first_ok[kind])
				CHECK(error == 0);
			else
				CHECK(error == ENOBUFS);
		}
	}
}

/* Parses random bytes, some made to look like the start of a WebP file, and checks nothing crashes or reads outside. */
static void
test_random(void)
{
	static const char *const tags[] = { "ALPH", "VP8 ", "VP8L", "ICCP", "VP8X", "EXIF", "ANIM" };
	uint8_t data[96];
	unsigned long state;
	unsigned long value;
	unsigned long count;
	size_t length;
	size_t index;
	unsigned mode;
	int error;
	struct webpc_container out;

	/* A fixed seed makes a failure repeatable. */
	state = 0x2468aceUL;

	/* Ten thousand inputs of random length. */
	for (count = 0; count < 10000UL; count++) {
		/* Random bytes, a length of 0 to 95. */
		value = check_random(&state);
		length = (size_t)(value % 96UL);
		for (index = 0; index < length; index++) {
			value = check_random(&state);
			data[index] = (uint8_t)(value >> 8);
		}

		/* Half of them start with a RIFF header and "WEBP", with a File Size near the data's. */
		mode = (unsigned)(count % 4UL);
		if (mode != 0U && length >= 12U) {
			memcpy(data, "RIFF", 4);
			memcpy(data + 8, "WEBP", 4);
			value = check_random(&state);
			put_le32(data + 4, (unsigned long)length - 8UL + (value % 7UL) - 3UL);
			if ((value % 11UL) == 0UL)
				put_le32(data + 4, value);
		}

		/* Some then have a VP8X chunk of the right size, so the chunk walk is reached. */
		if (mode >= 2U && length >= 30U) {
			memcpy(data + 12, "VP8X", 4);
			put_le32(data + 16, 10UL);
		}

		/* Some have a known tag with a small size at the next position. */
		if (mode == 3U && length >= 40U) {
			value = check_random(&state);
			memcpy(data + 30, tags[value % 7UL], 4);
			value = check_random(&state);
			put_le32(data + 34, value % 24UL);
		}

		/* Both settings, with the answer's pointers checked inside parse_exact. */
		error = parse_exact(data, length, 1, &out);
		CHECK(error == 0 || error == EINVAL || error == ENOBUFS);
		error = parse_exact(data, length, 0, &out);
		CHECK(error == 0 || error == EINVAL || error == ENOBUFS);
	}
}
