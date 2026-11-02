/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8-header.c: frames built with the test's own boolean
 * writer in the order of RFC 6386 19.2 and read back, dequantization
 * factors worked out by hand from RFC 6386 14.1, the token partitions of
 * 9.5 against a model kept here, and cut-off and random inputs.
 */

#include "vp8-header.h"
#include "boolwriter.h"
#include "check.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most token probability updates one test header carries. */
#define SPEC_UPDATES_MAX	64U

/* The number of token probabilities (4 x 8 x 3 x 11). */
#define PROB_COUNT		1056U

/* One token probability update: the position in the order of the format and the new value. */
struct update {
	unsigned index;
	unsigned value;
};

/*
 * What to write into one test frame: every field of a key frame's header
 * with, for the optional ones, a presence flag and a value.  Fields that the
 * format only sends under another flag are left out of the frame when that
 * flag is 0, and check_header expects them to read as 0.
 */
struct spec {
	unsigned version;
	unsigned width;
	unsigned height;
	unsigned horizontal_scale;
	unsigned vertical_scale;
	int color_space;
	int clamping_type;
	int seg_enabled;
	int seg_update_map;
	int seg_update_data;
	int seg_absolute;
	int quant_present[4];
	int quant[4];
	int level_present[4];
	int level[4];
	int prob_present[3];
	unsigned prob[3];
	int simple;
	unsigned level_value;
	unsigned sharpness;
	int deltas_enabled;
	int deltas_update;
	int ref_present[4];
	int ref[4];
	int mode_present[4];
	int mode[4];
	unsigned log2_partitions;
	unsigned base_q;
	int dq_present[5];
	int dq[5];
	int refresh;
	unsigned update_count;
	struct update updates[SPEC_UPDATES_MAX];
	int skip_enabled;
	unsigned skip_prob;
	uint32_t part_table[8];
	size_t part_bytes[8];
	long first_size_adjust;
};

/* The frame being built; large, so not on the stack. */
static uint8_t frame[BOOLWRITER_BYTES + 4096U];

/* The first partition being written. */
static struct boolwriter writer;

/* The random generator's state. */
static unsigned long random_state = 0x2f6e2b1UL;

static void spec_default(struct spec *spec);
static void spec_partitions(struct spec *spec, unsigned log2_partitions, size_t base_bytes);
static void write_first(const struct spec *spec);
static size_t build_frame(const struct spec *spec, size_t *first_size);
static uint8_t *copy_exact(const uint8_t *source, size_t size);
static unsigned random_below(unsigned limit);
static int32_t expect_value(int present, int value);
static void expect_dequant(long q, const struct webpc_vp8_header *header, struct webpc_vp8_dequant *out);
static void check_header(const struct spec *spec, const struct webpc_vp8_header *header);
static int run_case(const struct spec *spec);
static void test_tag(void);
static void test_tag_errors(void);
static void test_header_basic(void);
static void test_segmentation(void);
static void test_filter(void);
static void test_partitions(void);
static void test_quant_and_probs(void);
static void test_dequant_hand(void);
static void test_dequant_clamp(void);
static void test_dequant_segments(void);
static void test_first_size(void);
static void test_truncation(void);
static void test_random_specs(void);
static void test_random_garbage(void);

/*
 * Runs the tests of vp8-header.c.
 */
int
main(
	void)
{
	int status;

	/* Each helper in turn. */
	test_tag();
	test_tag_errors();
	test_header_basic();
	test_segmentation();
	test_filter();
	test_partitions();
	test_quant_and_probs();
	test_dequant_hand();
	test_dequant_clamp();
	test_dequant_segments();
	test_first_size();
	test_truncation();
	test_random_specs();
	test_random_garbage();

	/* The summary line. */
	status = check_finish("vp8_header");
	return status;
}

/* Gives a pseudo-random number below limit (limit is at least 1). */
static unsigned
random_below(
	unsigned limit)
{
	unsigned long number;

	/* One step of the generator, reduced to the limit. */
	number = check_random(&random_state);
	return (unsigned)(number % (unsigned long)limit);
}

/*
 * Copies size bytes into a block of exactly that size, so a read past the
 * end is an error the sanitizer sees; a size of 0 gives NULL.
 */
static uint8_t *
copy_exact(
	const uint8_t *source,
	size_t size)
{
	uint8_t *copy;

	/* An empty frame has no buffer. */
	if (size == 0U) {
		return NULL;
	}

	/* A block of exactly the frame's size. */
	copy = malloc(size);
	if (copy == NULL) {
		return NULL;
	}
	memcpy(copy, source, size);
	return copy;
}

/* Fills a spec with a plain header: 16x16, normal filter, one partition, quantizer 40, nothing optional. */
static void
spec_default(
	struct spec *spec)
{
	/* Everything absent or 0 first. */
	memset(spec, 0, sizeof(*spec));

	/* The values of a plain frame. */
	spec->width = 16U;
	spec->height = 16U;
	spec->level_value = 20U;
	spec->sharpness = 2U;
	spec->base_q = 40U;
	spec->refresh = 1;
	spec_partitions(spec, 0U, 7U);
}

/* Sets the partition count and gives partition i base_bytes + i bytes, written in the table as they are. */
static void
spec_partitions(
	struct spec *spec,
	unsigned log2_partitions,
	size_t base_bytes)
{
	unsigned index;

	/* The count, and the size of each partition. */
	spec->log2_partitions = log2_partitions;
	for (index = 0U; index < 8U; index++) {
		spec->part_bytes[index] = base_bytes + index;
		spec->part_table[index] = (uint32_t)(base_bytes + index);
	}
}

/* Writes an optional signed value: the flag, then the magnitude and the sign. */
static void
write_optional(
	int present,
	int value,
	unsigned bits)
{
	/* The flag says whether the value follows. */
	boolwriter_put(&writer, 128U, present != 0);
	if (present != 0) {
		boolwriter_signed(&writer, (int32_t)value, bits);
	}
}

/* Writes the first partition of a spec as RFC 6386 19.2 orders a key frame header, then a marker. */
static void
write_first(
	const struct spec *spec)
{
	unsigned index;
	unsigned type;
	unsigned band;
	unsigned context;
	unsigned node;
	unsigned position;
	unsigned update_index;
	int updated;
	unsigned new_value;

	/* color_space, clamping_type and segmentation_enabled. */
	boolwriter_init(&writer);
	boolwriter_literal(&writer, (uint32_t)spec->color_space, 1U);
	boolwriter_literal(&writer, (uint32_t)spec->clamping_type, 1U);
	boolwriter_literal(&writer, (uint32_t)spec->seg_enabled, 1U);

	/* update_segmentation(). */
	if (spec->seg_enabled != 0) {
		boolwriter_literal(&writer, (uint32_t)spec->seg_update_map, 1U);
		boolwriter_literal(&writer, (uint32_t)spec->seg_update_data, 1U);
		if (spec->seg_update_data != 0) {
			boolwriter_literal(&writer, (uint32_t)spec->seg_absolute, 1U);
			for (index = 0U; index < 4U; index++) {
				write_optional(spec->quant_present[index], spec->quant[index], 7U);
			}
			for (index = 0U; index < 4U; index++) {
				write_optional(spec->level_present[index], spec->level[index], 6U);
			}
		}
		if (spec->seg_update_map != 0) {
			for (index = 0U; index < 3U; index++) {
				boolwriter_put(&writer, 128U, spec->prob_present[index] != 0);
				if (spec->prob_present[index] != 0) {
					boolwriter_literal(&writer, spec->prob[index], 8U);
				}
			}
		}
	}

	/* filter_type, loop_filter_level, sharpness_level. */
	boolwriter_literal(&writer, (uint32_t)spec->simple, 1U);
	boolwriter_literal(&writer, spec->level_value, 6U);
	boolwriter_literal(&writer, spec->sharpness, 3U);

	/* mb_lf_adjustments(). */
	boolwriter_literal(&writer, (uint32_t)spec->deltas_enabled, 1U);
	if (spec->deltas_enabled != 0) {
		boolwriter_literal(&writer, (uint32_t)spec->deltas_update, 1U);
		if (spec->deltas_update != 0) {
			for (index = 0U; index < 4U; index++) {
				write_optional(spec->ref_present[index], spec->ref[index], 6U);
			}
			for (index = 0U; index < 4U; index++) {
				write_optional(spec->mode_present[index], spec->mode[index], 6U);
			}
		}
	}

	/* log2_nbr_of_dct_partitions and quant_indices(). */
	boolwriter_literal(&writer, spec->log2_partitions, 2U);
	boolwriter_literal(&writer, spec->base_q, 7U);
	for (index = 0U; index < 5U; index++) {
		write_optional(spec->dq_present[index], spec->dq[index], 4U);
	}

	/* refresh_entropy_probs. */
	boolwriter_literal(&writer, (uint32_t)spec->refresh, 1U);

	/* token_prob_update(): every probability's flag with its update probability, in the order of the format. */
	position = 0U;
	for (type = 0U; type < 4U; type++) {
		for (band = 0U; band < 8U; band++) {
			for (context = 0U; context < 3U; context++) {
				for (node = 0U; node < 11U; node++) {
					updated = 0;
					new_value = 0U;
					for (update_index = 0U; update_index < spec->update_count; update_index++) {
						if (spec->updates[update_index].index == position) {
							updated = 1;
							new_value = spec->updates[update_index].value;
						}
					}
					boolwriter_put(&writer, webpc_vp8_coeff_update[type][band][context][node], updated);
					if (updated != 0) {
						boolwriter_literal(&writer, new_value, 8U);
					}
					position++;
				}
			}
		}
	}

	/* mb_no_skip_coeff and prob_skip_false. */
	boolwriter_literal(&writer, (uint32_t)spec->skip_enabled, 1U);
	if (spec->skip_enabled != 0) {
		boolwriter_literal(&writer, spec->skip_prob, 8U);
	}

	/* A marker where the first macroblock header would start. */
	boolwriter_literal(&writer, 0x5aU, 8U);
}

/*
 * Builds a whole frame in frame[]: the tag, the start code, the size, the
 * first partition, the partition table and the partitions.  Gives the
 * frame's size and the first partition's real size.
 */
static size_t
build_frame(
	const struct spec *spec,
	size_t *first_size)
{
	size_t first;
	size_t total;
	size_t part_size;
	uint32_t tag;
	unsigned count;
	unsigned index;
	unsigned byte;
	long field;

	/* The first partition, closed so a decoder never needs more bytes. */
	write_first(spec);
	first = boolwriter_finish(&writer);
	CHECK(writer.overflow == 0);
	*first_size = first;

	/* The tag: key frame bit 0, version, show_frame 1 and the size, which a test may misstate. */
	field = (long)first + spec->first_size_adjust;
	tag = ((uint32_t)field << 5) | (1U << 4) | (spec->version << 1);
	frame[0] = (uint8_t)(tag & 255U);
	frame[1] = (uint8_t)((tag >> 8) & 255U);
	frame[2] = (uint8_t)((tag >> 16) & 255U);

	/* The start code and the 14 bit sizes with their scale bits above. */
	frame[3] = 0x9dU;
	frame[4] = 0x01U;
	frame[5] = 0x2aU;
	frame[6] = (uint8_t)(spec->width & 255U);
	frame[7] = (uint8_t)(((spec->width >> 8) & 63U) | (spec->horizontal_scale << 6));
	frame[8] = (uint8_t)(spec->height & 255U);
	frame[9] = (uint8_t)(((spec->height >> 8) & 63U) | (spec->vertical_scale << 6));

	/* The first partition's bytes. */
	memcpy(frame + 10, writer.bytes, first);
	total = 10U + first;

	/* The table of sizes of all partitions but the last, 3 bytes little-endian each. */
	count = 1U << spec->log2_partitions;
	for (index = 0U; index + 1U < count; index++) {
		frame[total] = (uint8_t)(spec->part_table[index] & 255U);
		frame[total + 1U] = (uint8_t)((spec->part_table[index] >> 8) & 255U);
		frame[total + 2U] = (uint8_t)((spec->part_table[index] >> 16) & 255U);
		total += 3U;
	}

	/* The partitions: each filled with a byte that names it. */
	for (index = 0U; index < count; index++) {
		part_size = spec->part_bytes[index];
		for (byte = 0U; byte < part_size; byte++) {
			frame[total + byte] = (uint8_t)(0xa0U + index);
		}
		total += part_size;
	}
	return total;
}

/* Gives the value a header reads for an optional field: the value when sent, else 0. */
static int32_t
expect_value(
	int present,
	int value)
{
	/* An unsent value is 0. */
	if (present == 0) {
		return 0;
	}
	return (int32_t)value;
}

/*
 * Works out the factors of RFC 6386 14.1 for index q (any value) and the
 * deltas of header, with long arithmetic and the table lookups, apart from
 * the library's code.
 */
static void
expect_dequant(
	long q,
	const struct webpc_vp8_header *header,
	struct webpc_vp8_dequant *out)
{
	long index[6];
	unsigned which;
	long delta[6];
	long y2_ac;

	/* The deltas by the order Y1 DC, Y1 AC (none), Y2 DC, Y2 AC, UV DC, UV AC. */
	delta[0] = header->dq_y1_dc;
	delta[1] = 0;
	delta[2] = header->dq_y2_dc;
	delta[3] = header->dq_y2_ac;
	delta[4] = header->dq_uv_dc;
	delta[5] = header->dq_uv_ac;

	/* Each index is q plus its delta limited to 0..127. */
	for (which = 0U; which < 6U; which++) {
		index[which] = q + delta[which];
		if (index[which] < 0) {
			index[which] = 0;
		}
		if (index[which] > 127) {
			index[which] = 127;
		}
	}

	/* The factors; Y2 DC doubled, Y2 AC by 155/100 not below 8, UV DC not above 132. */
	out->y1[0] = webpc_vp8_dc_q[index[0]];
	out->y1[1] = webpc_vp8_ac_q[index[1]];
	out->y2[0] = webpc_vp8_dc_q[index[2]] * 2;
	y2_ac = (long)webpc_vp8_ac_q[index[3]] * 155L / 100L;
	if (y2_ac < 8) {
		y2_ac = 8;
	}
	out->y2[1] = (int32_t)y2_ac;
	out->uv[0] = webpc_vp8_dc_q[index[4]];
	if (out->uv[0] > 132) {
		out->uv[0] = 132;
	}
	out->uv[1] = webpc_vp8_ac_q[index[5]];
}

/* Checks every field of a header read back against what the spec wrote. */
static void
check_header(
	const struct spec *spec,
	const struct webpc_vp8_header *header)
{
	static uint8_t probs[PROB_COUNT];
	struct webpc_vp8_dequant want;
	unsigned index;
	unsigned count;
	int sent_data;
	int sent_map;
	int sent_deltas;
	int32_t quantizer;
	long q;
	int mismatch;

	/* The tag fields. */
	CHECK(header->version == spec->version);
	CHECK(header->width == spec->width);
	CHECK(header->height == spec->height);
	CHECK(header->horizontal_scale == spec->horizontal_scale);
	CHECK(header->vertical_scale == spec->vertical_scale);
	CHECK(header->color_space == spec->color_space);
	CHECK(header->clamping_type == spec->clamping_type);

	/* The segmentation: what is only sent under a flag reads as 0 (255 for the probabilities) otherwise. */
	sent_data = (spec->seg_enabled != 0 && spec->seg_update_data != 0);
	sent_map = (spec->seg_enabled != 0 && spec->seg_update_map != 0);
	CHECK(header->segmentation.enabled == (spec->seg_enabled != 0));
	CHECK(header->segmentation.update_map == sent_map);
	CHECK(header->segmentation.update_data == sent_data);
	CHECK(header->segmentation.absolute == (sent_data && spec->seg_absolute != 0));
	for (index = 0U; index < 4U; index++) {
		quantizer = expect_value(sent_data && spec->quant_present[index], spec->quant[index]);
		CHECK(header->segmentation.quantizer[index] == quantizer);
		quantizer = expect_value(sent_data && spec->level_present[index], spec->level[index]);
		CHECK(header->segmentation.filter_level[index] == quantizer);
	}
	for (index = 0U; index < 3U; index++) {
		if (sent_map && spec->prob_present[index] != 0) {
			CHECK(header->segmentation.tree_probs[index] == spec->prob[index]);
		} else {
			CHECK(header->segmentation.tree_probs[index] == 255U);
		}
	}

	/* The filter controls and the deltas. */
	sent_deltas = (spec->deltas_enabled != 0 && spec->deltas_update != 0);
	CHECK(header->filter.simple == (spec->simple != 0));
	CHECK((unsigned)header->filter.level == spec->level_value);
	CHECK((unsigned)header->filter.sharpness == spec->sharpness);
	CHECK(header->filter.deltas_enabled == (spec->deltas_enabled != 0));
	for (index = 0U; index < 4U; index++) {
		CHECK(header->filter.ref_delta[index] == expect_value(sent_deltas && spec->ref_present[index], spec->ref[index]));
		CHECK(header->filter.mode_delta[index] == expect_value(sent_deltas && spec->mode_present[index], spec->mode[index]));
	}

	/* The quantizer indices. */
	CHECK((unsigned)header->base_q == spec->base_q);
	CHECK(header->dq_y1_dc == expect_value(spec->dq_present[0], spec->dq[0]));
	CHECK(header->dq_y2_dc == expect_value(spec->dq_present[1], spec->dq[1]));
	CHECK(header->dq_y2_ac == expect_value(spec->dq_present[2], spec->dq[2]));
	CHECK(header->dq_uv_dc == expect_value(spec->dq_present[3], spec->dq[3]));
	CHECK(header->dq_uv_ac == expect_value(spec->dq_present[4], spec->dq[4]));

	/* The factors of each segment: from base_q, or from the segment's value, absolute or added. */
	for (index = 0U; index < 4U; index++) {
		q = (long)spec->base_q;
		if (spec->seg_enabled != 0) {
			quantizer = expect_value(sent_data && spec->quant_present[index], spec->quant[index]);
			if (sent_data && spec->seg_absolute != 0) {
				q = quantizer;
			} else {
				q = (long)spec->base_q + quantizer;
			}
		}
		expect_dequant(q, header, &want);
		CHECK(header->dequant[index].y1[0] == want.y1[0]);
		CHECK(header->dequant[index].y1[1] == want.y1[1]);
		CHECK(header->dequant[index].y2[0] == want.y2[0]);
		CHECK(header->dequant[index].y2[1] == want.y2[1]);
		CHECK(header->dequant[index].uv[0] == want.uv[0]);
		CHECK(header->dequant[index].uv[1] == want.uv[1]);
	}

	/* The token probabilities: the defaults with the updates applied. */
	memcpy(probs, webpc_vp8_coeff_default, PROB_COUNT);
	for (index = 0U; index < spec->update_count; index++) {
		probs[spec->updates[index].index] = (uint8_t)spec->updates[index].value;
	}
	mismatch = memcmp(probs, header->coeff_probs, PROB_COUNT);
	CHECK(mismatch == 0);

	/* The skip flag and probability. */
	CHECK(header->skip_enabled == (spec->skip_enabled != 0));
	if (spec->skip_enabled != 0) {
		CHECK(header->skip_prob == spec->skip_prob);
	} else {
		CHECK(header->skip_prob == 0U);
	}

	/* The partitions: the count and, in the frame as built, the sizes the spec's bytes give. */
	count = 1U << spec->log2_partitions;
	CHECK(header->partition_count == count);
	for (index = 0U; index < count; index++) {
		CHECK(header->partitions[index] != NULL);
	}
}

/*
 * Builds the frame of a spec, reads it from an exactly sized copy, checks
 * the header and the marker after it; gives 1 when every read succeeded.
 */
static int
run_case(
	const struct spec *spec)
{
	static struct webpc_vp8_header header;
	struct webpc_bool first;
	size_t first_size;
	size_t total;
	uint8_t *copy;
	unsigned index;
	unsigned count;
	size_t expected_offset;
	int error;

	/* Builds the frame and copies it so the reads are bounds-checked. */
	total = build_frame(spec, &first_size);
	copy = copy_exact(frame, total);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return 0;
	}

	/* Reads the tag, then the rest of the header. */
	error = webpc_vp8_read_tag(copy, total, &header);
	CHECK(error == 0);
	if (error != 0) {
		free(copy);
		return 0;
	}
	error = webpc_vp8_read_header(copy, total, &header, &first);
	CHECK(error == 0);
	if (error != 0) {
		free(copy);
		return 0;
	}

	/* Every field, and the bool decoder standing at the marker. */
	check_header(spec, &header);
	CHECK(webpc_bool_literal(&first, 8U) == 0x5aUL);

	/* The partitions lie in the copy, one after another, each with its own fill byte. */
	count = 1U << spec->log2_partitions;
	expected_offset = 10U + first_size + 3U * (count - 1U);
	for (index = 0U; index < count; index++) {
		CHECK(header.partitions[index] == copy + expected_offset);
		CHECK(header.partition_sizes[index] == spec->part_bytes[index]);
		if (header.partition_sizes[index] > 0U) {
			CHECK(header.partitions[index][0] == (uint8_t)(0xa0U + index));
		}
		expected_offset += header.partition_sizes[index];
	}
	free(copy);
	return 1;
}

/* Checks read_tag on good tags: versions, sizes, the scale bits not entering the size, the first size. */
static void
test_tag(void)
{
	static struct webpc_vp8_header header;
	struct spec spec;
	size_t first_size;
	size_t total;
	unsigned version;
	int error;

	/* Each version, a plain size. */
	for (version = 0U; version < 4U; version++) {
		spec_default(&spec);
		spec.version = version;
		spec.width = 300U;
		spec.height = 200U;
		total = build_frame(&spec, &first_size);
		error = webpc_vp8_read_tag(frame, total, &header);
		CHECK(error == 0);
		CHECK(header.version == version);
		CHECK(header.width == 300U);
		CHECK(header.height == 200U);
		CHECK(header.horizontal_scale == 0U);
		CHECK(header.vertical_scale == 0U);
		CHECK(header.first_size == first_size);
	}

	/* The extreme sizes, with the scale bits set: they are read apart and do not change the size. */
	spec_default(&spec);
	spec.width = 16383U;
	spec.height = 1U;
	spec.horizontal_scale = 3U;
	spec.vertical_scale = 2U;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	CHECK(header.width == 16383U);
	CHECK(header.height == 1U);
	CHECK(header.horizontal_scale == 3U);
	CHECK(header.vertical_scale == 2U);

	/* A width of 1 and the tallest height, scales 1 and 0. */
	spec.width = 1U;
	spec.height = 16383U;
	spec.horizontal_scale = 1U;
	spec.vertical_scale = 0U;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	CHECK(header.width == 1U);
	CHECK(header.height == 16383U);
	CHECK(header.horizontal_scale == 1U);
	CHECK(header.vertical_scale == 0U);

	/* A tag of exactly 10 bytes is enough for read_tag; the first size is as written (19 bits, here the largest). */
	spec.first_size_adjust = 524287L - (long)first_size;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, 10U, &header);
	CHECK(error == 0);
	CHECK(header.first_size == 524287U);
}

/* Checks read_tag's errors, each against a good tag changed in one place. */
static void
test_tag_errors(void)
{
	static struct webpc_vp8_header header;
	struct spec spec;
	uint8_t tag[10];
	size_t first_size;
	size_t total;
	unsigned length;
	unsigned index;
	int error;

	/* A good tag to start from. */
	spec_default(&spec);
	total = build_frame(&spec, &first_size);
	CHECK(total >= 10U);
	memcpy(tag, frame, 10U);
	error = webpc_vp8_read_tag(tag, 10U, &header);
	CHECK(error == 0);

	/* Fewer than 10 bytes is ENOBUFS, from 0 to 9. */
	for (length = 0U; length < 10U; length++) {
		error = webpc_vp8_read_tag(tag, length, &header);
		CHECK(error == ENOBUFS);
	}

	/* An interframe (bit 0 set) is EINVAL. */
	tag[0] = (uint8_t)(tag[0] | 1U);
	error = webpc_vp8_read_tag(tag, 10U, &header);
	CHECK(error == EINVAL);
	tag[0] = frame[0];

	/* Versions 4 to 7 are EINVAL. */
	for (index = 4U; index < 8U; index++) {
		tag[0] = (uint8_t)((frame[0] & ~0x0eU) | (index << 1));
		error = webpc_vp8_read_tag(tag, 10U, &header);
		CHECK(error == EINVAL);
	}
	tag[0] = frame[0];

	/* show_frame 0 is EINVAL. */
	tag[0] = (uint8_t)(tag[0] & ~0x10U);
	error = webpc_vp8_read_tag(tag, 10U, &header);
	CHECK(error == EINVAL);
	tag[0] = frame[0];

	/* Each byte of the start code wrong is EINVAL. */
	for (index = 3U; index < 6U; index++) {
		tag[index] = (uint8_t)(tag[index] ^ 0x01U);
		error = webpc_vp8_read_tag(tag, 10U, &header);
		CHECK(error == EINVAL);
		tag[index] = frame[index];
	}

	/* A width of 0, with and without scale bits, is EINVAL. */
	tag[6] = 0U;
	tag[7] = 0U;
	error = webpc_vp8_read_tag(tag, 10U, &header);
	CHECK(error == EINVAL);
	tag[7] = 0xc0U;
	error = webpc_vp8_read_tag(tag, 10U, &header);
	CHECK(error == EINVAL);
	tag[6] = frame[6];
	tag[7] = frame[7];

	/* A height of 0, with and without scale bits, is EINVAL. */
	tag[8] = 0U;
	tag[9] = 0U;
	error = webpc_vp8_read_tag(tag, 10U, &header);
	CHECK(error == EINVAL);
	tag[9] = 0x40U;
	error = webpc_vp8_read_tag(tag, 10U, &header);
	CHECK(error == EINVAL);
}

/* Checks plain headers: the default, the colour bits, version, sizes and a simple filter. */
static void
test_header_basic(void)
{
	struct spec spec;

	/* The default frame. */
	spec_default(&spec);
	CHECK(run_case(&spec) == 1);

	/* color_space and clamping_type set, and each alone. */
	spec.color_space = 1;
	CHECK(run_case(&spec) == 1);
	spec.clamping_type = 1;
	CHECK(run_case(&spec) == 1);
	spec.color_space = 0;
	CHECK(run_case(&spec) == 1);

	/* refresh_entropy_probs 0 reads and leaves nothing behind. */
	spec_default(&spec);
	spec.refresh = 0;
	CHECK(run_case(&spec) == 1);

	/* A large frame with scale bits and a version 3. */
	spec_default(&spec);
	spec.version = 3U;
	spec.width = 16383U;
	spec.height = 12345U;
	spec.horizontal_scale = 2U;
	spec.vertical_scale = 1U;
	CHECK(run_case(&spec) == 1);
}

/* Checks the segmentation in every shape the format has. */
static void
test_segmentation(void)
{
	struct spec spec;
	unsigned index;

	/* Enabled with the map only: no values, so the dequantization stays at base_q (RFC 6386 20.4). */
	spec_default(&spec);
	spec.seg_enabled = 1;
	spec.seg_update_map = 1;
	spec.prob_present[0] = 1;
	spec.prob[0] = 77U;
	spec.prob_present[2] = 1;
	spec.prob[2] = 0U;
	CHECK(run_case(&spec) == 1);

	/* Enabled with neither update: still nothing sent. */
	spec.seg_update_map = 0;
	CHECK(run_case(&spec) == 1);

	/* Values only, in delta mode: positive, negative and absent quantizers and levels. */
	spec_default(&spec);
	spec.seg_enabled = 1;
	spec.seg_update_data = 1;
	spec.quant_present[0] = 1;
	spec.quant[0] = 10;
	spec.quant_present[1] = 1;
	spec.quant[1] = -10;
	spec.quant_present[3] = 1;
	spec.quant[3] = 127;
	spec.level_present[1] = 1;
	spec.level[1] = 63;
	spec.level_present[2] = 1;
	spec.level[2] = -63;
	spec.level_present[3] = 1;
	spec.level[3] = 1;
	CHECK(run_case(&spec) == 1);

	/* The same values in absolute mode. */
	spec.seg_absolute = 1;
	spec.quant[1] = 0;
	spec.quant_present[1] = 1;
	spec.quant[0] = 120;
	spec.quant[3] = 1;
	CHECK(run_case(&spec) == 1);

	/* Both updates, absolute, a negative absolute quantizer, and all probabilities. */
	spec.seg_update_map = 1;
	spec.quant[2] = -5;
	spec.quant_present[2] = 1;
	for (index = 0U; index < 3U; index++) {
		spec.prob_present[index] = 1;
		spec.prob[index] = 200U + index;
	}
	CHECK(run_case(&spec) == 1);

	/* Both updates in delta mode with no value sent at all (absent flags only). */
	spec_default(&spec);
	spec.seg_enabled = 1;
	spec.seg_update_map = 1;
	spec.seg_update_data = 1;
	CHECK(run_case(&spec) == 1);

	/* Absolute mode with no value sent: every segment is quantizer 0 (RFC 6386 20.4). */
	spec.seg_absolute = 1;
	CHECK(run_case(&spec) == 1);

	/* A disabled segmentation whose spec fields are all set: none of it is written, none is read. */
	spec_default(&spec);
	spec.seg_update_map = 1;
	spec.seg_update_data = 1;
	spec.seg_absolute = 1;
	for (index = 0U; index < 4U; index++) {
		spec.quant_present[index] = 1;
		spec.quant[index] = 5;
		spec.level_present[index] = 1;
		spec.level[index] = 5;
	}
	CHECK(run_case(&spec) == 1);
}

/* Checks the filter controls: types, the ends of the level and sharpness, the delta forms. */
static void
test_filter(void)
{
	struct spec spec;
	unsigned index;

	/* The simple filter at level 0, and at level 63 with sharpness 7. */
	spec_default(&spec);
	spec.simple = 1;
	spec.level_value = 0U;
	spec.sharpness = 0U;
	CHECK(run_case(&spec) == 1);
	spec.level_value = 63U;
	spec.sharpness = 7U;
	CHECK(run_case(&spec) == 1);

	/* The normal filter at the same ends. */
	spec.simple = 0;
	CHECK(run_case(&spec) == 1);
	spec.level_value = 0U;
	spec.sharpness = 0U;
	CHECK(run_case(&spec) == 1);

	/* Deltas enabled with no update: all 0. */
	spec.deltas_enabled = 1;
	CHECK(run_case(&spec) == 1);

	/* Deltas enabled and updated with none present. */
	spec.deltas_update = 1;
	CHECK(run_case(&spec) == 1);

	/* Each delta alone, positive then negative, so that a swapped order of reference and mode shows. */
	for (index = 0U; index < 4U; index++) {
		spec.ref_present[index] = 1;
		spec.ref[index] = (int)(index + 1U) * 3;
		CHECK(run_case(&spec) == 1);
		spec.ref[index] = -(int)(index + 1U) * 3;
		CHECK(run_case(&spec) == 1);
		spec.ref_present[index] = 0;
		spec.mode_present[index] = 1;
		spec.mode[index] = (int)(index + 1U) * 5;
		CHECK(run_case(&spec) == 1);
		spec.mode[index] = -63 + (int)index;
		CHECK(run_case(&spec) == 1);
		spec.mode_present[index] = 0;
	}

	/* All eight present with the largest magnitudes of both signs. */
	for (index = 0U; index < 4U; index++) {
		spec.ref_present[index] = 1;
		spec.ref[index] = (index % 2U == 0U) ? 63 : -63;
		spec.mode_present[index] = 1;
		spec.mode[index] = (index % 2U == 0U) ? -63 : 63;
	}
	CHECK(run_case(&spec) == 1);

	/* A set update flag with deltas disabled is not written. */
	spec.deltas_enabled = 0;
	CHECK(run_case(&spec) == 1);
}

/* Checks the token partitions: counts, sizes, a size that is too large, a missing or short table. */
static void
test_partitions(void)
{
	static struct webpc_vp8_header header;
	struct spec spec;
	struct webpc_bool first;
	size_t first_size;
	size_t total;
	size_t cut;
	unsigned log2;
	int error;

	/* 1, 2, 4 and 8 partitions with distinct sizes (a size of 0 in the middle is allowed). */
	for (log2 = 0U; log2 < 4U; log2++) {
		spec_default(&spec);
		spec_partitions(&spec, log2, 9U);
		CHECK(run_case(&spec) == 1);
		spec_partitions(&spec, log2, 0U);
		spec.part_bytes[(1U << log2) - 1U] = 4U;
		spec.part_table[(1U << log2) - 1U] = 4U;
		CHECK(run_case(&spec) == 1);
	}

	/* A size larger than the rest is cut to what is left and the next ones are empty. */
	spec_default(&spec);
	spec_partitions(&spec, 2U, 5U);
	spec.part_table[1] = 0xffffffU;
	spec.part_bytes[0] = 5U;
	spec.part_bytes[1] = 100U;
	spec.part_bytes[2] = 0U;
	spec.part_bytes[3] = 0U;
	spec.part_table[0] = 5U;
	spec.part_table[2] = 7U;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(frame, total, &header, &first);
	CHECK(error == ENOBUFS);

	/* A cut partition in the middle (partition 2) takes the rest and leaves the last empty. */
	spec.part_table[1] = 98U;
	spec.part_table[2] = 0xffffffU;
	spec.part_bytes[1] = 98U;
	spec.part_bytes[2] = 0U;
	spec.part_bytes[3] = 2U;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(frame, total, &header, &first);
	CHECK(error == ENOBUFS);

	/* A table size that passes the end in a middle partition: that partition takes the rest, the last is empty. */
	spec_default(&spec);
	spec_partitions(&spec, 1U, 5U);
	spec.part_table[0] = 999999U;
	spec.part_bytes[0] = 20U;
	spec.part_bytes[1] = 0U;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(frame, total, &header, &first);
	CHECK(error == ENOBUFS);

	/* A table size exactly one less than what is left leaves the last partition that one byte. */
	spec_partitions(&spec, 1U, 5U);
	spec.part_table[0] = 20U;
	spec.part_bytes[0] = 20U;
	spec.part_bytes[1] = 1U;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(frame, total, &header, &first);
	CHECK(error == 0);
	CHECK(header.partition_sizes[0] == 20U);
	CHECK(header.partition_sizes[1] == 1U);

	/* A table size equal to what is left takes it all and leaves the last empty: ENOBUFS. */
	spec.part_table[0] = 21U;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(frame, total, &header, &first);
	CHECK(error == ENOBUFS);

	/* An empty last partition is ENOBUFS, with one partition and with four. */
	spec_default(&spec);
	spec.part_bytes[0] = 0U;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(frame, total, &header, &first);
	CHECK(error == ENOBUFS);
	spec_partitions(&spec, 2U, 3U);
	spec.part_bytes[3] = 0U;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(frame, total, &header, &first);
	CHECK(error == ENOBUFS);

	/* A table that does not fit or leaves nothing: with 8 partitions, every cut from the first partition's end to one byte after the table. */
	spec_default(&spec);
	spec_partitions(&spec, 3U, 4U);
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	for (cut = 10U + first_size; cut < 10U + first_size + 21U + 1U; cut++) {
		error = webpc_vp8_read_header(frame, cut, &header, &first);
		CHECK(error == ENOBUFS);
	}

	/* The table plus one byte of partitions still fails: the first partition takes that byte and the last is empty. */
	error = webpc_vp8_read_header(frame, 10U + first_size + 21U + 1U, &header, &first);
	CHECK(error == ENOBUFS);
}

/* Checks the quantizer indices, their deltas and the token probability updates. */
static void
test_quant_and_probs(void)
{
	struct spec spec;
	unsigned index;

	/* The base index at 0 and 127, no deltas. */
	spec_default(&spec);
	spec.base_q = 0U;
	CHECK(run_case(&spec) == 1);
	spec.base_q = 127U;
	CHECK(run_case(&spec) == 1);

	/* Each delta alone, in both signs and at its magnitude limit, so a swapped order shows. */
	for (index = 0U; index < 5U; index++) {
		spec_default(&spec);
		spec.dq_present[index] = 1;
		spec.dq[index] = 15;
		CHECK(run_case(&spec) == 1);
		spec.dq[index] = -15;
		CHECK(run_case(&spec) == 1);
		spec.dq[index] = (int)index + 1;
		CHECK(run_case(&spec) == 1);
	}

	/* All five at once with different values. */
	for (index = 0U; index < 5U; index++) {
		spec.dq_present[index] = 1;
		spec.dq[index] = (int)index * 3 - 6;
	}
	CHECK(run_case(&spec) == 1);

	/* A few updates: the first, the last, a pair that are neighbours and one in the middle of each type. */
	spec_default(&spec);
	spec.update_count = 6U;
	spec.updates[0].index = 0U;
	spec.updates[0].value = 1U;
	spec.updates[1].index = 1055U;
	spec.updates[1].value = 255U;
	spec.updates[2].index = 500U;
	spec.updates[2].value = 7U;
	spec.updates[3].index = 501U;
	spec.updates[3].value = 8U;
	spec.updates[4].index = 264U;
	spec.updates[4].value = 0U;
	spec.updates[5].index = 800U;
	spec.updates[5].value = 128U;
	CHECK(run_case(&spec) == 1);

	/* Updates with skip enabled at the largest probability. */
	spec.skip_enabled = 1;
	spec.skip_prob = 255U;
	CHECK(run_case(&spec) == 1);

	/* A skip probability of 0. */
	spec.skip_prob = 0U;
	CHECK(run_case(&spec) == 1);

	/* A skip probability in the middle. */
	spec.skip_prob = 83U;
	CHECK(run_case(&spec) == 1);

	/* A skip probability that is set in the spec but not enabled is not written. */
	spec.skip_enabled = 0;
	CHECK(run_case(&spec) == 1);

	/* Every probability updated: a header with the whole table rewritten. */
	spec_default(&spec);
	spec.update_count = SPEC_UPDATES_MAX;
	for (index = 0U; index < SPEC_UPDATES_MAX; index++) {
		spec.updates[index].index = index * 16U;
		spec.updates[index].value = 255U - index;
	}
	CHECK(run_case(&spec) == 1);
}

/* Checks dequantization factors worked out by hand from RFC 6386 14.1 and its tables. */
static void
test_dequant_hand(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant out;

	/* Index 0 with no deltas: dc_q and ac_q are 4; Y2 is 8 and 8 (4 * 155 / 100 = 6 is raised to 8); chroma 4 and 4. */
	memset(&header, 0, sizeof(header));
	webpc_vp8_dequant_build(0, &header, &out);
	CHECK(out.y1[0] == 4 && out.y1[1] == 4);
	CHECK(out.y2[0] == 8 && out.y2[1] == 8);
	CHECK(out.uv[0] == 4 && out.uv[1] == 4);

	/* Index 127: dc_q 157 and ac_q 284; Y2 is 314 and 284 * 155 / 100 = 440; chroma 132 (157 lowered) and 284. */
	webpc_vp8_dequant_build(127, &header, &out);
	CHECK(out.y1[0] == 157 && out.y1[1] == 284);
	CHECK(out.y2[0] == 314 && out.y2[1] == 440);
	CHECK(out.uv[0] == 132 && out.uv[1] == 284);

	/* Index 1: ac_q is 5 and 5 * 155 / 100 = 7 is raised to 8; index 2: 6 * 155 / 100 = 9 stays. */
	webpc_vp8_dequant_build(1, &header, &out);
	CHECK(out.y2[1] == 8);
	webpc_vp8_dequant_build(2, &header, &out);
	CHECK(out.y2[1] == 9);

	/* The chroma DC cap: dc_q 130 at index 116 stays, 132 at 117 stays, 134 at 118 is lowered to 132. */
	webpc_vp8_dequant_build(116, &header, &out);
	CHECK(out.uv[0] == 130);
	webpc_vp8_dequant_build(117, &header, &out);
	CHECK(out.uv[0] == 132);
	webpc_vp8_dequant_build(118, &header, &out);
	CHECK(out.uv[0] == 132);

	/* Index 60 with every delta different: Y1 DC at 65 is 60, UV DC at 56 is 51, Y2 AC at 62 is 74 * 155 / 100 = 114, UV AC at 57 is 64. */
	header.dq_y1_dc = 5;
	header.dq_y2_dc = 0;
	header.dq_y2_ac = 2;
	header.dq_uv_dc = -4;
	header.dq_uv_ac = -3;
	webpc_vp8_dequant_build(60, &header, &out);
	CHECK(out.y1[0] == 60);
	CHECK(out.y1[1] == 70);
	CHECK(out.y2[0] == 110);
	CHECK(out.y2[1] == 114);
	CHECK(out.uv[0] == 51);
	CHECK(out.uv[1] == 64);
}

/* Checks the clamping of q plus a delta, and q values far outside 0..127. */
static void
test_dequant_clamp(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_dequant out;
	struct webpc_vp8_dequant want;
	static const long values[] = { 0L, 1L, 3L, 50L, 112L, 120L, 127L, 128L, 142L, 143L, 200L, 255L, 256L, 257L,
		1000L, 100000L, 2147483647L, -1L, -3L, -15L, -16L, -127L, -300L, -100000L, -2147483647L - 1L };
	unsigned index;
	unsigned delta_index;
	int32_t *deltas[5];

	/* q = 120 with a delta of 15 reaches 135, which is clamped to 127 for every delta in turn. */
	memset(&header, 0, sizeof(header));
	header.dq_y1_dc = 15;
	webpc_vp8_dequant_build(120, &header, &out);
	CHECK(out.y1[0] == 157);
	CHECK(out.y1[1] == 249);

	/* q = 3 with a delta of -15 reaches -12, which is clamped to 0: dc_q[0] = 4. */
	memset(&header, 0, sizeof(header));
	header.dq_y1_dc = -15;
	header.dq_uv_ac = -15;
	header.dq_y2_dc = -15;
	webpc_vp8_dequant_build(3, &header, &out);
	CHECK(out.y1[0] == 4);
	CHECK(out.uv[1] == 4);
	CHECK(out.y2[0] == 8);
	CHECK(out.y1[1] == 7);

	/* The other deltas at the top: q = 120 and +15 gives 127 in every factor but Y1 AC, which is at 120. */
	memset(&header, 0, sizeof(header));
	header.dq_y1_dc = 15;
	header.dq_y2_dc = 15;
	header.dq_y2_ac = 15;
	header.dq_uv_dc = 15;
	header.dq_uv_ac = 15;
	webpc_vp8_dequant_build(120, &header, &out);
	CHECK(out.y1[0] == 157);
	CHECK(out.y1[1] == 249);
	CHECK(out.y2[0] == 314);
	CHECK(out.y2[1] == 440);
	CHECK(out.uv[0] == 132);
	CHECK(out.uv[1] == 284);

	/* Every q of the list against every delta alone at 15 and -15, compared with the model. */
	deltas[0] = &header.dq_y1_dc;
	deltas[1] = &header.dq_y2_dc;
	deltas[2] = &header.dq_y2_ac;
	deltas[3] = &header.dq_uv_dc;
	deltas[4] = &header.dq_uv_ac;
	for (index = 0U; index < sizeof(values) / sizeof(values[0]); index++) {
		for (delta_index = 0U; delta_index < 10U; delta_index++) {
			memset(&header, 0, sizeof(header));
			*deltas[delta_index % 5U] = (delta_index < 5U) ? 15 : -15;
			webpc_vp8_dequant_build((int32_t)values[index], &header, &out);
			expect_dequant(values[index], &header, &want);
			CHECK(out.y1[0] == want.y1[0] && out.y1[1] == want.y1[1]);
			CHECK(out.y2[0] == want.y2[0] && out.y2[1] == want.y2[1]);
			CHECK(out.uv[0] == want.uv[0] && out.uv[1] == want.uv[1]);
		}
	}
}

/* Checks the segment index rules of RFC 6386 20.4: absolute values and deltas, q itself not clamped. */
static void
test_dequant_segments(void)
{
	static struct webpc_vp8_header header;
	struct spec spec;
	struct webpc_bool first;
	size_t first_size;
	size_t total;
	int error;
	unsigned index;

	/* Segmentation off: all four segments have the factors of base_q (60: 55, 70). */
	spec_default(&spec);
	spec.base_q = 60U;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(frame, total, &header, &first);
	CHECK(error == 0);
	for (index = 0U; index < 4U; index++) {
		CHECK(header.dequant[index].y1[0] == 55);
		CHECK(header.dequant[index].y1[1] == 70);
	}

	/* Delta mode: base 60 plus +4, -2, none and -127 gives 64, 58, 60 and -67 (clamped only when looked up): ac_q is 78, 66, 70 and 4. */
	spec.seg_enabled = 1;
	spec.seg_update_data = 1;
	spec.quant_present[0] = 1;
	spec.quant[0] = 4;
	spec.quant_present[1] = 1;
	spec.quant[1] = -2;
	spec.quant_present[3] = 1;
	spec.quant[3] = -127;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(frame, total, &header, &first);
	CHECK(error == 0);
	CHECK(header.dequant[0].y1[1] == 78);
	CHECK(header.dequant[1].y1[1] == 66);
	CHECK(header.dequant[2].y1[1] == 70);
	CHECK(header.dequant[3].y1[1] == 4);

	/* Absolute mode: the values themselves, base_q ignored (ac_q[4] is 8, ac_q[127] 284, a negative index looks up entry 0). */
	spec.seg_absolute = 1;
	spec.quant[0] = 4;
	spec.quant[1] = 100;
	spec.quant_present[2] = 1;
	spec.quant[2] = 127;
	spec.quant[3] = -3;
	total = build_frame(&spec, &first_size);
	error = webpc_vp8_read_tag(frame, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(frame, total, &header, &first);
	CHECK(error == 0);
	CHECK(header.dequant[0].y1[1] == 8);
	CHECK(header.dequant[1].y1[1] == webpc_vp8_ac_q[100]);
	CHECK(header.dequant[2].y1[1] == 284);
	CHECK(header.dequant[3].y1[1] == 4);
	CHECK(header.segmentation.quantizer[3] == -3);
}

/* Checks the first partition's size against the data: it must fit, to the byte. */
static void
test_first_size(void)
{
	static struct webpc_vp8_header header;
	struct spec spec;
	struct webpc_bool first;
	size_t first_size;
	size_t total;
	uint8_t *copy;
	int error;

	/* A frame whose first partition ends exactly where the data does, with one partition of a byte after it. */
	spec_default(&spec);
	spec.part_bytes[0] = 1U;
	total = build_frame(&spec, &first_size);
	copy = copy_exact(frame, total);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}
	error = webpc_vp8_read_tag(copy, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(copy, total, &header, &first);
	CHECK(error == 0);

	/* The data cut at the first partition's end: nothing is left for the last partition. */
	error = webpc_vp8_read_header(copy, 10U + first_size, &header, &first);
	CHECK(error == ENOBUFS);

	/* The data cut one byte inside the first partition: it does not fit. */
	error = webpc_vp8_read_header(copy, 10U + first_size - 1U, &header, &first);
	CHECK(error == ENOBUFS);

	/* A size field larger than the data, by one and by a lot. */
	free(copy);
	spec.first_size_adjust = 1L;
	total = build_frame(&spec, &first_size);
	copy = copy_exact(frame, total);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}
	error = webpc_vp8_read_tag(copy, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(copy, total, &header, &first);
	CHECK(error == ENOBUFS);
	free(copy);
	spec.first_size_adjust = 400000L;
	total = build_frame(&spec, &first_size);
	copy = copy_exact(frame, total);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}
	error = webpc_vp8_read_tag(copy, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(copy, total, &header, &first);
	CHECK(error == ENOBUFS);
	free(copy);

	/* A first partition too short for its header: the decoder runs past the end and the header is ENOBUFS. */
	spec_default(&spec);
	total = build_frame(&spec, &first_size);
	spec.first_size_adjust = 2L - (long)first_size;
	total = build_frame(&spec, &first_size);
	copy = copy_exact(frame, total);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}
	error = webpc_vp8_read_tag(copy, total, &header);
	CHECK(error == 0);
	CHECK(header.first_size == 2U);
	error = webpc_vp8_read_header(copy, total, &header, &first);
	CHECK(error == ENOBUFS);
	free(copy);

	/* A first partition of 0 bytes: ENOBUFS as well. */
	spec.first_size_adjust = 0L - (long)first_size;
	total = build_frame(&spec, &first_size);
	copy = copy_exact(frame, total);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}
	error = webpc_vp8_read_tag(copy, total, &header);
	CHECK(error == 0);
	error = webpc_vp8_read_header(copy, total, &header, &first);
	CHECK(error == ENOBUFS);
	free(copy);
}

/*
 * Cuts a frame of 4 partitions at every length and compares read_header with
 * a model of RFC 6386 9.5 kept here.
 */
static void
test_truncation(void)
{
	static struct webpc_vp8_header header;
	struct spec spec;
	struct webpc_bool first;
	size_t first_size;
	size_t total;
	size_t cut;
	size_t rest;
	size_t remaining;
	size_t part;
	size_t sizes[4];
	uint8_t *copy;
	unsigned index;
	int expect_ok;
	int error;

	/* A frame of 4 partitions of 6, 7, 8 and 9 bytes. */
	spec_default(&spec);
	spec_partitions(&spec, 2U, 6U);
	total = build_frame(&spec, &first_size);
	copy = copy_exact(frame, total);
	CHECK(copy != NULL);
	if (copy == NULL) {
		return;
	}

	/* Each length: read_tag needs 10 bytes; read_header follows the model. */
	for (cut = 0U; cut <= total; cut++) {
		error = webpc_vp8_read_tag(copy, cut, &header);
		if (cut < 10U) {
			CHECK(error == ENOBUFS);
			continue;
		}
		CHECK(error == 0);

		/* The model: the first partition and the table must fit, sizes are cut to the rest, the last is not empty. */
		expect_ok = 0;
		if (cut >= 10U + first_size) {
			rest = cut - 10U - first_size;
			if (rest >= 9U) {
				remaining = rest - 9U;
				for (index = 0U; index < 3U; index++) {
					part = spec.part_table[index];
					if (part > remaining) {
						part = remaining;
					}
					sizes[index] = part;
					remaining -= part;
				}
				sizes[3] = remaining;
				if (remaining > 0U) {
					expect_ok = 1;
				}
			}
		}
		error = webpc_vp8_read_header(copy, cut, &header, &first);
		if (expect_ok != 0) {
			CHECK(error == 0);
			for (index = 0U; index < 4U; index++) {
				CHECK(header.partition_sizes[index] == sizes[index]);
			}
		} else {
			CHECK(error == ENOBUFS);
		}
	}
	free(copy);
}

/* Reads random specs: every field random within its range, each frame read back and compared. */
static void
test_random_specs(void)
{
	struct spec spec;
	unsigned round;
	unsigned index;

	/* 300 random headers. */
	for (round = 0U; round < 300U; round++) {
		spec_default(&spec);
		spec.version = random_below(4U);
		spec.width = 1U + random_below(16383U);
		spec.height = 1U + random_below(16383U);
		spec.horizontal_scale = random_below(4U);
		spec.vertical_scale = random_below(4U);
		spec.color_space = (int)random_below(2U);
		spec.clamping_type = (int)random_below(2U);
		spec.seg_enabled = (int)random_below(2U);
		spec.seg_update_map = (int)random_below(2U);
		spec.seg_update_data = (int)random_below(2U);
		spec.seg_absolute = (int)random_below(2U);
		for (index = 0U; index < 4U; index++) {
			spec.quant_present[index] = (int)random_below(2U);
			spec.quant[index] = (int)random_below(255U) - 127;
			spec.level_present[index] = (int)random_below(2U);
			spec.level[index] = (int)random_below(127U) - 63;
			spec.ref_present[index] = (int)random_below(2U);
			spec.ref[index] = (int)random_below(127U) - 63;
			spec.mode_present[index] = (int)random_below(2U);
			spec.mode[index] = (int)random_below(127U) - 63;
		}
		for (index = 0U; index < 3U; index++) {
			spec.prob_present[index] = (int)random_below(2U);
			spec.prob[index] = random_below(256U);
		}
		spec.simple = (int)random_below(2U);
		spec.level_value = random_below(64U);
		spec.sharpness = random_below(8U);
		spec.deltas_enabled = (int)random_below(2U);
		spec.deltas_update = (int)random_below(2U);
		spec_partitions(&spec, random_below(4U), 1U + random_below(20U));
		spec.base_q = random_below(128U);
		for (index = 0U; index < 5U; index++) {
			spec.dq_present[index] = (int)random_below(2U);
			spec.dq[index] = (int)random_below(31U) - 15;
		}
		spec.refresh = (int)random_below(2U);
		spec.update_count = random_below(SPEC_UPDATES_MAX);
		for (index = 0U; index < spec.update_count; index++) {
			spec.updates[index].index = random_below(PROB_COUNT);
			spec.updates[index].value = random_below(256U);
		}
		spec.skip_enabled = (int)random_below(2U);
		spec.skip_prob = random_below(256U);
		CHECK(run_case(&spec) == 1);
	}
}

/*
 * Reads random bytes behind a valid tag, and valid frames with random bytes
 * flipped: read_header gives 0 or ENOBUFS, never anything else, and the
 * partitions it reports lie inside the buffer.
 */
static void
test_random_garbage(void)
{
	static struct webpc_vp8_header header;
	static uint8_t bytes[400];
	struct spec spec;
	struct webpc_bool first;
	size_t first_size;
	size_t total;
	size_t size;
	size_t sum;
	uint8_t *copy;
	unsigned round;
	unsigned index;
	unsigned count;
	int error;

	/* 2000 buffers of random bytes and sizes behind a tag with a random first size. */
	for (round = 0U; round < 2000U; round++) {
		size = 10U + random_below(390U);
		for (index = 0U; index < size; index++) {
			bytes[index] = (uint8_t)random_below(256U);
		}
		bytes[0] = (uint8_t)((bytes[0] & ~0x1fU) | 0x10U);
		bytes[3] = 0x9dU;
		bytes[4] = 0x01U;
		bytes[5] = 0x2aU;
		bytes[6] |= 1U;
		bytes[8] |= 1U;
		copy = copy_exact(bytes, size);
		CHECK(copy != NULL);
		if (copy == NULL) {
			return;
		}
		error = webpc_vp8_read_tag(copy, size, &header);
		CHECK(error == 0);
		error = webpc_vp8_read_header(copy, size, &header, &first);
		CHECK(error == 0 || error == ENOBUFS);
		if (error == 0) {
			count = header.partition_count;
			CHECK(count == 1U || count == 2U || count == 4U || count == 8U);
			sum = 0U;
			for (index = 0U; index < count; index++) {
				CHECK(header.partitions[index] >= copy);
				CHECK(header.partitions[index] + header.partition_sizes[index] <= copy + size);
				sum += header.partition_sizes[index];
			}
			CHECK(sum > 0U);
		}
		free(copy);
	}

	/* Valid frames with one to three bytes of the first partition or the table replaced at random. */
	spec_default(&spec);
	spec_partitions(&spec, 2U, 5U);
	total = build_frame(&spec, &first_size);
	for (round = 0U; round < 2000U; round++) {
		copy = copy_exact(frame, total);
		CHECK(copy != NULL);
		if (copy == NULL) {
			return;
		}
		for (index = 0U; index < 1U + random_below(3U); index++) {
			copy[10U + random_below((unsigned)(first_size + 9U))] = (uint8_t)random_below(256U);
		}
		error = webpc_vp8_read_tag(copy, total, &header);
		CHECK(error == 0);
		error = webpc_vp8_read_header(copy, total, &header, &first);
		CHECK(error == 0 || error == ENOBUFS);
		free(copy);
	}
}
