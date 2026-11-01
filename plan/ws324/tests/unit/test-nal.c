/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests of h265sw-nal: the NAL unit header (7.3.1.2, 7.4.2.2), the RBSP
 * copy (7.3.1.1, 7.4.2.1), the copy of the first bytes of a payload, the
 * type classes of Table 7-1 and the access unit starts of 7.4.2.4.4.
 *
 * The expected values come from the clauses and tables named above, as sets
 * of nal_unit_type values written out by hand and as bytes arranged by hand.
 * The inputs of the header checks are made with the bit writer, the others
 * with the NAL unit writer of h265write.h; the first bytes of a payload are
 * checked against a second, simpler removal of the emulation prevention
 * bytes written in this file.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "h265write.h"
#include "userland/desktop/libmedia/h265sw-nal.h"

/* The number of random inputs of the robustness check. */
#define RANDOM_INPUTS 100000U

/* The largest NAL unit the checks build, in bytes. */
#define NAL_BUFFER 512U

/* The writer, too large for the stack of a check. */
static struct bitwriter writer;

/* The state of the pseudo-random generator, fixed so a failure repeats. */
static uint32_t random_state = 0x2545F491U;

/* Static function declarations. */
static uint32_t random_next(void);
static int in_list(unsigned type, const unsigned *list, unsigned count);
static unsigned header_bytes(unsigned type, unsigned layer, unsigned temporal_plus1, uint8_t *out);
static int header_expected(unsigned type, unsigned temporal_plus1);
static unsigned unescape_simple(const uint8_t *payload, size_t size, uint8_t *out);
static void check_header(void);
static void check_header_errors(void);
static void check_classes(void);
static void check_rbsp(void);
static void check_rbsp_errors(void);
static void check_capacity(void);
static void check_prefix_one(const uint8_t *nal, size_t size);
static void check_prefix(void);
static void check_random(void);

/*
 * The types of each class, written out from Table 7-1 and 7.4.2.2 and
 * 7.4.2.4.4.
 */
static const unsigned list_vcl[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
};
static const unsigned list_irap[] = {16, 17, 18, 19, 20, 21, 22, 23};
static const unsigned list_idr[] = {19, 20};
static const unsigned list_bla[] = {16, 17, 18};
static const unsigned list_rasl[] = {8, 9};
static const unsigned list_radl[] = {6, 7};
static const unsigned list_slnr[] = {0, 2, 4, 6, 8, 10, 12, 14};
static const unsigned list_decodable[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21};
static const unsigned list_starts[] = {32, 33, 34, 35, 39, 41, 42, 43, 44, 48, 49, 50, 51, 52, 53, 54, 55};

/* Returns the next pseudo-random number (xorshift32). */
static uint32_t
random_next(void)
{
	/* The three shifts of the generator. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;

	/* Succeeded: the next number. */
	return random_state;
}

/* Reports whether type is one of the count entries of list. */
static int
in_list(
	unsigned type,
	const unsigned *list,
	unsigned count)
{
	unsigned index;

	/* Looks for the type in the list. */
	for (index = 0U; index < count; index++) {
		if (list[index] == type)
			return 1;
	}

	/* The type is not in the list. */
	return 0;
}

/*
 * Writes the two header bytes of 7.3.1.2 with the bit writer: a zero bit,
 * the type in 6 bits, the layer id in 6 and nuh_temporal_id_plus1 in 3.
 */
static unsigned
header_bytes(
	unsigned type,
	unsigned layer,
	unsigned temporal_plus1,
	uint8_t *out)
{
	/* Writes the fields in the order of the syntax. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 0U, 1U);
	bitwriter_u(&writer, type, 6U);
	bitwriter_u(&writer, layer, 6U);
	bitwriter_u(&writer, temporal_plus1, 3U);

	/* Takes the two bytes. */
	out[0] = writer.data[0];
	out[1] = writer.data[1];

	/* Succeeded: two bytes. */
	return 2U;
}

/*
 * Reports whether a header of this type and nuh_temporal_id_plus1 is valid
 * by 7.4.2.2: the plus-1 value is not 0, an IRAP type (16..23) has TemporalId
 * 0 and a TSA type (2, 3) has a TemporalId other than 0.
 */
static int
header_expected(
	unsigned type,
	unsigned temporal_plus1)
{
	/* nuh_temporal_id_plus1 shall not be 0. */
	if (temporal_plus1 == 0U)
		return 0;

	/* An IRAP type has TemporalId 0. */
	if (type >= 16U && type <= 23U && temporal_plus1 != 1U)
		return 0;

	/* A TSA type has a TemporalId other than 0. */
	if ((type == 2U || type == 3U) && temporal_plus1 == 1U)
		return 0;

	/* Succeeded: a valid header. */
	return 1;
}

/*
 * Removes each 0x03 that follows two zero bytes, which is all 7.3.1.1 does,
 * and returns the number of bytes kept.
 */
static unsigned
unescape_simple(
	const uint8_t *payload,
	size_t size,
	uint8_t *out)
{
	size_t index;
	unsigned zeros;
	unsigned kept;

	/* Walks the bytes counting the zeros just before. */
	zeros = 0U;
	kept = 0U;
	for (index = 0U; index < size; index++) {
		if (zeros >= 2U && payload[index] == 3U) {
			zeros = 0U;
			continue;
		}
		out[kept] = payload[index];
		kept++;
		if (payload[index] == 0U) {
			zeros++;
		} else {
			zeros = 0U;
		}
	}

	/* Succeeded: the RBSP's length. */
	return kept;
}

/* Checks the fields of every valid header: all types, layers and temporal ids. */
static void
check_header(void)
{
	uint8_t nal[3];
	uint8_t buffer[4];
	struct h265sw_nal out;
	unsigned type;
	unsigned layer;
	unsigned plus1;
	unsigned expected_ok;
	int error;

	/* Every combination of the three header fields, with one payload byte. */
	for (type = 0U; type < 64U; type++) {
		for (layer = 0U; layer < 64U; layer++) {
			for (plus1 = 1U; plus1 < 8U; plus1++) {
				header_bytes(type, layer, plus1, nal);
				nal[2] = 0x80U;
				memset(&out, 0xA5, sizeof(out));
				error = h265sw_nal_parse(nal, 3U, buffer, sizeof(buffer), &out);
				expected_ok = (unsigned)header_expected(type, plus1);
				if (expected_ok != 0U) {
					CHECK(error == 0);
					CHECK(out.type == type);
					CHECK(out.layer_id == layer);
					CHECK(out.temporal_id == plus1 - 1U);
					CHECK(out.rbsp == buffer);
					CHECK(out.rbsp_size == 1U);
					CHECK(buffer[0] == 0x80U);
					CHECK(out.complete == 1);
				} else {
					CHECK(error == EINVAL);
				}
			}
		}
	}
}

/* Checks the header's refusals and the sizes below a header. */
static void
check_header_errors(void)
{
	uint8_t nal[8];
	uint8_t buffer[8];
	struct h265sw_nal out;
	struct h265sw_nal sentinel;
	int error;

	/* A set forbidden_zero_bit is refused. */
	nal[0] = 0x80U | (1U << 1);
	nal[1] = 1U;
	nal[2] = 0x80U;
	error = h265sw_nal_parse(nal, 3U, buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);

	/* nuh_temporal_id_plus1 0 is refused for every type. */
	nal[0] = (uint8_t)(1U << 1);
	nal[1] = 0U;
	error = h265sw_nal_parse(nal, 3U, buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);

	/* A size of 0 and of 1 hold no header. */
	error = h265sw_nal_parse(NULL, 0U, buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);
	error = h265sw_nal_parse(nal, 0U, buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);
	error = h265sw_nal_parse(nal, 1U, buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);
	error = h265sw_nal_parse_prefix(nal, 1U, 1U, buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);

	/* A missing NAL unit pointer with a size, and a missing output, are refused. */
	nal[0] = (uint8_t)(1U << 1);
	nal[1] = 1U;
	error = h265sw_nal_parse(NULL, 3U, buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);
	error = h265sw_nal_parse(nal, 2U, buffer, sizeof(buffer), NULL);
	CHECK(error == EINVAL);

	/* A NAL unit above the largest taken is refused (the pointer is never read). */
	error = h265sw_nal_parse(nal, (size_t)H265SW_MAX_NAL_BYTES + 1U, buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);

	/* A header alone is an empty RBSP, and needs no buffer. */
	memset(&out, 0, sizeof(out));
	error = h265sw_nal_parse(nal, 2U, NULL, 0U, &out);
	CHECK(error == 0);
	CHECK(out.rbsp_size == 0U);
	CHECK(out.complete == 1);
	CHECK(out.type == 1U);

	/* An error leaves the output as it was. */
	memset(&out, 0x5A, sizeof(out));
	memset(&sentinel, 0x5A, sizeof(sentinel));
	nal[0] = 0x80U;
	error = h265sw_nal_parse(nal, 3U, buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);
	CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0);
	nal[0] = (uint8_t)(1U << 1);
	nal[2] = 0U;
	nal[3] = 0U;
	nal[4] = 1U;
	error = h265sw_nal_parse(nal, 5U, buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);
	CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0);
}

/* Checks every classification function on all 64 types against the hand-written sets. */
static void
check_classes(void)
{
	unsigned type;

	/* Each function over every type, and a few values above 63. */
	for (type = 0U; type < 70U; type++) {
		CHECK((h265sw_nal_is_vcl(type) != 0) == in_list(type, list_vcl, sizeof(list_vcl) / sizeof(list_vcl[0])));
		CHECK((h265sw_nal_is_irap(type) != 0) == in_list(type, list_irap, sizeof(list_irap) / sizeof(list_irap[0])));
		CHECK((h265sw_nal_is_idr(type) != 0) == in_list(type, list_idr, sizeof(list_idr) / sizeof(list_idr[0])));
		CHECK((h265sw_nal_is_bla(type) != 0) == in_list(type, list_bla, sizeof(list_bla) / sizeof(list_bla[0])));
		CHECK((h265sw_nal_is_rasl(type) != 0) == in_list(type, list_rasl, sizeof(list_rasl) / sizeof(list_rasl[0])));
		CHECK((h265sw_nal_is_radl(type) != 0) == in_list(type, list_radl, sizeof(list_radl) / sizeof(list_radl[0])));
		CHECK((h265sw_nal_is_slnr(type) != 0) == in_list(type, list_slnr, sizeof(list_slnr) / sizeof(list_slnr[0])));
		CHECK((h265sw_nal_is_decodable(type) != 0) == in_list(type, list_decodable, sizeof(list_decodable) / sizeof(list_decodable[0])));
		CHECK((h265sw_nal_starts_access_unit(type) != 0) == in_list(type, list_starts, sizeof(list_starts) / sizeof(list_starts[0])));
	}

	/* The results are 0 or 1 only, and the macros of the header name the same types. */
	CHECK(h265sw_nal_is_irap(H265SW_NAL_CRA) == 1);
	CHECK(h265sw_nal_is_idr(H265SW_NAL_IDR_N_LP) == 1);
	CHECK(h265sw_nal_is_vcl(H265SW_NAL_SPS) == 0);
	CHECK(h265sw_nal_starts_access_unit(H265SW_NAL_EOS) == 0);
	CHECK(h265sw_nal_starts_access_unit(H265SW_NAL_SUFFIX_SEI) == 0);
}

/* Checks the removal of emulation prevention bytes on arranged payloads. */
static void
check_rbsp(void)
{
	/* Each case: the escaped payload and the RBSP it gives, arranged by hand from 7.3.1.1. */
	static const uint8_t in0[] = {0x00, 0x00, 0x03, 0x01};
	static const uint8_t out0[] = {0x00, 0x00, 0x01};
	static const uint8_t in1[] = {0x11, 0x00, 0x00, 0x03, 0x02, 0x22};
	static const uint8_t out1[] = {0x11, 0x00, 0x00, 0x02, 0x22};
	static const uint8_t in2[] = {0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x01};
	static const uint8_t out2[] = {0x00, 0x00, 0x00, 0x00, 0x01};
	static const uint8_t in3[] = {0x00, 0x00, 0x03, 0x03};
	static const uint8_t out3[] = {0x00, 0x00, 0x03};
	static const uint8_t in4[] = {0x00, 0x00, 0x03};
	static const uint8_t out4[] = {0x00, 0x00};
	static const uint8_t in5[] = {0x80, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03};
	static const uint8_t out5[] = {0x80, 0x00, 0x00, 0x00, 0x00};
	static const uint8_t in6[] = {0x00, 0x00, 0x03, 0x00, 0x03};
	static const uint8_t out6[] = {0x00, 0x00, 0x00, 0x03};
	static const uint8_t in7[] = {0x01, 0x02, 0x03, 0x04};
	static const uint8_t out7[] = {0x01, 0x02, 0x03, 0x04};
	static const uint8_t *const inputs[] = {in0, in1, in2, in3, in4, in5, in6, in7};
	static const uint8_t *const outputs[] = {out0, out1, out2, out3, out4, out5, out6, out7};
	static const size_t in_sizes[] = {sizeof(in0), sizeof(in1), sizeof(in2), sizeof(in3), sizeof(in4), sizeof(in5), sizeof(in6), sizeof(in7)};
	static const size_t out_sizes[] = {sizeof(out0), sizeof(out1), sizeof(out2), sizeof(out3), sizeof(out4), sizeof(out5), sizeof(out6), sizeof(out7)};
	uint8_t nal[NAL_BUFFER];
	uint8_t buffer[NAL_BUFFER];
	struct h265sw_nal out;
	unsigned index;
	int error;

	/* Each arranged payload behind a valid header. */
	for (index = 0U; index < sizeof(inputs) / sizeof(inputs[0]); index++) {
		header_bytes(1U, 0U, 1U, nal);
		memcpy(nal + 2, inputs[index], in_sizes[index]);
		memset(buffer, 0xEE, sizeof(buffer));
		error = h265sw_nal_parse(nal, in_sizes[index] + 2U, buffer, in_sizes[index], &out);
		CHECK(error == 0);
		CHECK(out.rbsp_size == out_sizes[index]);
		CHECK(memcmp(out.rbsp, outputs[index], out_sizes[index]) == 0);
		CHECK(out.complete == 1);
	}
}

/* Checks the byte sequences the payload may not hold. */
static void
check_rbsp_errors(void)
{
	/* Each case holds a three-byte sequence 7.4.2.1 forbids. */
	static const uint8_t bad0[] = {0x00, 0x00, 0x00};
	static const uint8_t bad1[] = {0x00, 0x00, 0x01};
	static const uint8_t bad2[] = {0x00, 0x00, 0x02};
	static const uint8_t bad3[] = {0x00, 0x00, 0x03, 0x04};
	static const uint8_t bad4[] = {0x55, 0x00, 0x00, 0x03, 0x80, 0x01};
	static const uint8_t bad5[] = {0x55, 0x00, 0x00, 0x03, 0x00, 0x00, 0x01};
	static const uint8_t bad6[] = {0x55, 0x66, 0x00, 0x00, 0x00, 0x66};
	static const uint8_t bad7[] = {0x00, 0x00, 0x03, 0xFF};
	static const uint8_t *const inputs[] = {bad0, bad1, bad2, bad3, bad4, bad5, bad6, bad7};
	static const size_t sizes[] = {sizeof(bad0), sizeof(bad1), sizeof(bad2), sizeof(bad3), sizeof(bad4), sizeof(bad5), sizeof(bad6), sizeof(bad7)};
	uint8_t nal[NAL_BUFFER];
	uint8_t buffer[NAL_BUFFER];
	struct h265sw_nal out;
	unsigned index;
	int error;

	/* Each refused payload behind a valid header. */
	for (index = 0U; index < sizeof(inputs) / sizeof(inputs[0]); index++) {
		header_bytes(1U, 0U, 1U, nal);
		memcpy(nal + 2, inputs[index], sizes[index]);
		error = h265sw_nal_parse(nal, sizes[index] + 2U, buffer, sizeof(buffer), &out);
		CHECK(error == EINVAL);
	}
}

/* Checks that the capacity is compared with the escaped size, not the RBSP size. */
static void
check_capacity(void)
{
	/* Five payload bytes with one emulation prevention byte: the RBSP is four bytes. */
	static const uint8_t payload[] = {0x01, 0x00, 0x00, 0x03, 0x02};
	uint8_t nal[NAL_BUFFER];
	uint8_t *buffer;
	struct h265sw_nal out;
	int error;

	/* Builds the NAL unit. */
	header_bytes(1U, 0U, 1U, nal);
	memcpy(nal + 2, payload, sizeof(payload));

	/* A buffer of the RBSP's own size is refused: the check does not depend on the escapes. */
	buffer = (uint8_t *)malloc(4U);
	CHECK(buffer != NULL);
	if (buffer == NULL)
		return;
	error = h265sw_nal_parse(nal, 7U, buffer, 4U, &out);
	CHECK(error == ENOBUFS);
	free(buffer);

	/* A buffer of size - 2 is enough. */
	buffer = (uint8_t *)malloc(5U);
	CHECK(buffer != NULL);
	if (buffer == NULL)
		return;
	error = h265sw_nal_parse(nal, 7U, buffer, 5U, &out);
	CHECK(error == 0);
	CHECK(out.rbsp_size == 4U);
	free(buffer);

	/* The prefix copy has the same rule with the cut size. */
	buffer = (uint8_t *)malloc(3U);
	CHECK(buffer != NULL);
	if (buffer == NULL)
		return;
	error = h265sw_nal_parse_prefix(nal, 7U, 4U, buffer, 3U, &out);
	CHECK(error == ENOBUFS);
	error = h265sw_nal_parse_prefix(nal, 7U, 3U, buffer, 3U, &out);
	CHECK(error == 0);
	CHECK(out.complete == 0);
	CHECK(out.rbsp_size == 3U);
	free(buffer);

	/* The ENOBUFS check comes after the header's: a broken header is EINVAL whatever the capacity. */
	nal[1] = 0U;
	error = h265sw_nal_parse(nal, 7U, NULL, 0U, &out);
	CHECK(error == EINVAL);
}

/*
 * Checks the first-bytes copy of one NAL unit for every limit against the
 * whole copy and against the simple removal of this file.
 */
static void
check_prefix_one(
	const uint8_t *nal,
	size_t size)
{
	uint8_t whole_buffer[NAL_BUFFER];
	uint8_t cut_buffer[NAL_BUFFER];
	uint8_t expected[NAL_BUFFER];
	struct h265sw_nal whole;
	struct h265sw_nal cut;
	size_t payload;
	size_t limit;
	unsigned expected_size;
	int whole_error;
	int cut_error;

	/* The whole copy, which these inputs are valid for. */
	payload = size - 2U;
	whole_error = h265sw_nal_parse(nal, size, whole_buffer, sizeof(whole_buffer), &whole);
	CHECK(whole_error == 0);
	if (whole_error != 0)
		return;

	/* Every limit from 1 to one above the payload's length, and a large one. */
	for (limit = 1U; limit <= payload + 2U; limit++) {
		memset(&cut, 0, sizeof(cut));
		cut_error = h265sw_nal_parse_prefix(nal, size, limit, cut_buffer, sizeof(cut_buffer), &cut);
		CHECK(cut_error == 0);
		if (cut_error != 0)
			continue;

		/* Complete exactly when the payload fits the limit. */
		CHECK((cut.complete != 0) == (payload <= limit));
		CHECK(cut.type == whole.type);
		CHECK(cut.layer_id == whole.layer_id);
		CHECK(cut.temporal_id == whole.temporal_id);
		CHECK(cut.rbsp == cut_buffer);

		/* The copy is the start of the whole RBSP, and all of it when complete. */
		CHECK(cut.rbsp_size <= whole.rbsp_size);
		CHECK(memcmp(cut.rbsp, whole.rbsp, cut.rbsp_size) == 0);
		if (cut.complete != 0)
			CHECK(cut.rbsp_size == whole.rbsp_size);

		/* The length matches the simple removal on the first limit payload bytes. */
		expected_size = unescape_simple(nal + 2, (limit < payload) ? limit : payload, expected);
		CHECK(cut.rbsp_size == expected_size);
		CHECK(memcmp(cut.rbsp, expected, expected_size) == 0);
	}

	/* A limit of 0 is refused. */
	cut_error = h265sw_nal_parse_prefix(nal, size, 0U, cut_buffer, sizeof(cut_buffer), &cut);
	CHECK(cut_error == EINVAL);
}

/* Checks the first-bytes copy on arranged and random valid NAL units. */
static void
check_prefix(void)
{
	/* A payload with escapes at the cut points: 00 00 03 01 | 00 00 03 00 00 03 | 02 00 00 03 03. */
	static const uint8_t arranged[] = {
		0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00, 0x03, 0x03, 0x80
	};
	static const uint8_t ending[] = {0x55, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03};
	uint8_t nal[NAL_BUFFER];
	uint8_t rbsp[NAL_BUFFER];
	uint8_t buffer[NAL_BUFFER];
	struct h265sw_nal whole;
	size_t size;
	size_t rbsp_size;
	size_t index;
	unsigned round;
	unsigned zero_bias;
	int error;

	/* The arranged payloads. */
	header_bytes(1U, 0U, 1U, nal);
	memcpy(nal + 2, arranged, sizeof(arranged));
	check_prefix_one(nal, sizeof(arranged) + 2U);
	memcpy(nal + 2, ending, sizeof(ending));
	check_prefix_one(nal, sizeof(ending) + 2U);

	/* Random RBSPs rich in zeros, made into NAL units by the writer (a valid stream, zero pairs at the end). */
	for (round = 0U; round < 400U; round++) {
		rbsp_size = 1U + (size_t)(random_next() % 40U);
		zero_bias = 1U + random_next() % 3U;
		for (index = 0U; index < rbsp_size; index++) {
			if ((random_next() % zero_bias) == 0U) {
				rbsp[index] = (uint8_t)(random_next() % 4U);
			} else {
				rbsp[index] = (uint8_t)random_next();
			}
		}

		/* An RBSP ends with a non-zero byte, then perhaps cabac_zero_word pairs. */
		rbsp[rbsp_size - 1U] |= 0x80U;
		if ((random_next() % 3U) == 0U) {
			rbsp[rbsp_size] = 0U;
			rbsp[rbsp_size + 1U] = 0U;
			rbsp_size += 2U;
		}

		size = h265w_nal(1U, 0U, 1U, rbsp, rbsp_size, nal, sizeof(nal));
		CHECK(size != 0U);
		if (size == 0U)
			continue;

		/* The whole copy is the RBSP the writer started from, then the cuts. */
		error = h265sw_nal_parse(nal, size, buffer, sizeof(buffer), &whole);
		CHECK(error == 0);
		CHECK(whole.rbsp_size == rbsp_size);
		CHECK(memcmp(whole.rbsp, rbsp, rbsp_size) == 0);
		check_prefix_one(nal, size);
	}
}

/* Checks that random bytes never crash the readers and only the documented errors come back. */
static void
check_random(void)
{
	uint8_t *nal;
	uint8_t *buffer;
	struct h265sw_nal out;
	size_t size;
	size_t capacity;
	size_t limit;
	size_t index;
	unsigned round;
	unsigned bias;
	int error;
	int allowed;

	/* Each input in an exactly sized heap block, so an out-of-bounds access is caught. */
	for (round = 0U; round < RANDOM_INPUTS; round++) {
		size = (size_t)(random_next() % 40U);
		nal = (uint8_t *)malloc(size + 1U);
		CHECK(nal != NULL);
		if (nal == NULL)
			return;
		bias = 1U + random_next() % 4U;
		for (index = 0U; index < size; index++) {
			if ((random_next() % bias) == 0U) {
				nal[index] = (uint8_t)(random_next() % 5U);
			} else {
				nal[index] = (uint8_t)random_next();
			}
		}

		/* Half of the inputs get a valid header. */
		if (size >= 2U && (random_next() % 2U) == 0U) {
			nal[0] = (uint8_t)((nal[0] & 0x7FU) & ~0x7EU);
			nal[0] = (uint8_t)(nal[0] | (1U << 1));
			nal[1] = (uint8_t)((nal[1] & 0xF8U) | 1U);
		}

		/* A capacity near the size, sometimes short. */
		capacity = size;
		if (size > 2U && (random_next() % 4U) == 0U)
			capacity = (size_t)(random_next() % size);
		buffer = (uint8_t *)malloc(capacity + 1U);
		CHECK(buffer != NULL);
		if (buffer == NULL) {
			free(nal);
			return;
		}

		/* The whole copy: 0, EINVAL or ENOBUFS only, and never more than the capacity. */
		error = h265sw_nal_parse(nal, size, buffer, capacity, &out);
		allowed = (error == 0 || error == EINVAL || error == ENOBUFS);
		CHECK(allowed);
		if (error == 0)
			CHECK(out.rbsp_size <= capacity);

		/* The first-bytes copy with a random limit. */
		limit = 1U + (size_t)(random_next() % 45U);
		error = h265sw_nal_parse_prefix(nal, size, limit, buffer, capacity, &out);
		allowed = (error == 0 || error == EINVAL || error == ENOBUFS);
		CHECK(allowed);
		if (error == 0)
			CHECK(out.rbsp_size <= capacity);

		free(buffer);
		free(nal);
	}
}

int
main(void)
{
	/* Runs every group of checks. */
	check_header();
	check_header_errors();
	check_classes();
	check_rbsp();
	check_rbsp_errors();
	check_capacity();
	check_prefix();
	check_random();

	/* Reports the summary line. */
	return check_finish("nal");
}
