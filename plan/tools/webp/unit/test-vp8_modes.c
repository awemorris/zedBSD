/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8-modes.c: macroblock headers written with the test's
 * boolean encoder (from vp8-tables' trees and probabilities, and the
 * subblock contexts the test works out itself from a grid of every
 * subblock mode of the frame), read back one macroblock at a time with the
 * above and left contexts carried from one to the next, over single
 * macroblocks, a 3x2 frame and random frames; then partitions that are
 * empty, cut short or random, and contexts that hold values no mode has.
 */

#include "vp8-modes.h"
#include "check.h"
#include "boolwriter.h"

#include <string.h>

/* The frame the tests write: macroblocks across and down, and the subblocks across and down. */
#define FRAME_MB_WIDTH		3U
#define FRAME_MB_HEIGHT		2U
#define GRID_WIDTH		(FRAME_MB_WIDTH * 4U)
#define GRID_HEIGHT		(FRAME_MB_HEIGHT * 4U)

/* The random generator's state. */
static unsigned long random_state = 0x2468ace1UL;

/* The partition the tests write, kept out of the stack (it is 64 KiB). */
static struct boolwriter writer;

static unsigned random_below(unsigned limit);
static void fill_header(struct webpc_vp8_header *header, int update_map, int skip_enabled);
static void random_header(struct webpc_vp8_header *header);
static unsigned grid_context(const unsigned grid[GRID_HEIGHT][GRID_WIDTH], const unsigned border_above[GRID_WIDTH], const unsigned border_left[GRID_HEIGHT], int row, int column);
static void write_macroblock(struct boolwriter *out, const struct webpc_vp8_header *header, const struct webpc_vp8_modes *want, const unsigned above[4], const unsigned left[4]);
static void make_modes(struct webpc_vp8_modes *want, const struct webpc_vp8_header *header, unsigned ymode);
static void check_modes_equal(const struct webpc_vp8_modes *got, const struct webpc_vp8_modes *want);
static void check_one(const struct webpc_vp8_header *header, const struct webpc_vp8_modes *want, const unsigned above[4], const unsigned left[4]);
static void run_frame(const struct webpc_vp8_header *header, const unsigned border_above[GRID_WIDTH], const unsigned border_left[GRID_HEIGHT], unsigned bpred_percent);
static void test_single_modes(void);
static void test_flags(void);
static void test_contexts_of_one(void);
static void test_frame_edges(void);
static void test_frame_3x2(void);
static void test_frame_random(void);
static void test_not_read_fields(void);
static void test_ended(void);
static void test_random_bytes(void);
static void test_bad_contexts(void);

/*
 * Runs the tests of vp8-modes.c.
 */
int
main(
	void)
{
	int status;

	/* Each helper in turn. */
	test_single_modes();
	test_flags();
	test_contexts_of_one();
	test_frame_edges();
	test_frame_3x2();
	test_frame_random();
	test_not_read_fields();
	test_ended();
	test_random_bytes();
	test_bad_contexts();

	/* The summary line. */
	status = check_finish("vp8_modes");
	return status;
}

/*
 * Gives a pseudo-random number below limit (limit is at least 1).
 */
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
 * Clears header and sets the fields the macroblock header reads, with
 * probabilities that differ from each other so a swapped one shows.
 */
static void
fill_header(
	struct webpc_vp8_header *header,
	int update_map,
	int skip_enabled)
{
	/* Everything else is zero. */
	memset(header, 0, sizeof(*header));

	/* The segment map, when it is updated, and its probabilities. */
	header->segmentation.enabled = update_map;
	header->segmentation.update_map = update_map;
	header->segmentation.tree_probs[0] = 70U;
	header->segmentation.tree_probs[1] = 190U;
	header->segmentation.tree_probs[2] = 25U;

	/* The skip flag and its probability. */
	header->skip_enabled = skip_enabled;
	header->skip_prob = skip_enabled ? 90U : 0U;
}

/*
 * Fills header with random flags and probabilities (0..255, 0 included).
 */
static void
random_header(
	struct webpc_vp8_header *header)
{
	/* The flags, then probabilities from the whole range. */
	fill_header(header, (int)random_below(2U), (int)random_below(2U));
	header->segmentation.tree_probs[0] = (uint8_t)random_below(256U);
	header->segmentation.tree_probs[1] = (uint8_t)random_below(256U);
	header->segmentation.tree_probs[2] = (uint8_t)random_below(256U);
	if (header->skip_enabled != 0) {
		header->skip_prob = random_below(256U);
	}
}

/*
 * Gives the mode of the subblock at (row, column) of the frame's grid, where
 * a row or column of -1 is the border (the contexts the first macroblocks
 * start from).  The grid holds the modes of the macroblocks read so far.
 */
static unsigned
grid_context(
	const unsigned grid[GRID_HEIGHT][GRID_WIDTH],
	const unsigned border_above[GRID_WIDTH],
	const unsigned border_left[GRID_HEIGHT],
	int row,
	int column)
{
	/* Above the frame, left of the frame, or a subblock already read. */
	if (row < 0) {
		return border_above[column];
	}
	if (column < 0) {
		return border_left[row];
	}
	return grid[row][column];
}

/*
 * Writes one macroblock header for the modes in want, in RFC 6386 19.3's
 * order, with the subblock contexts above[4] and left[4] given by the test.
 */
static void
write_macroblock(
	struct boolwriter *out,
	const struct webpc_vp8_header *header,
	const struct webpc_vp8_modes *want,
	const unsigned above[4],
	const unsigned left[4])
{
	unsigned index;
	unsigned above_mode;
	unsigned left_mode;
	int failed;

	/* The segment when the map is updated. */
	if (header->segmentation.update_map != 0) {
		failed = boolwriter_tree(out, webpc_vp8_segment_tree, 6U, header->segmentation.tree_probs, 0U, (int)want->segment);
		CHECK(failed == 0);
	}

	/* The skip flag when there is one. */
	if (header->skip_enabled != 0) {
		boolwriter_put(out, header->skip_prob, want->skip);
	}

	/* The luma mode. */
	failed = boolwriter_tree(out, webpc_vp8_kf_ymode_tree, 8U, webpc_vp8_kf_ymode_prob, 0U, (int)want->ymode);
	CHECK(failed == 0);

	/* The 16 subblock modes of a B_PRED macroblock, each after its above and left. */
	if (want->ymode == WEBPC_VP8_B_PRED) {
		for (index = 0U; index < 16U; index++) {
			if (index < 4U) {
				above_mode = above[index];
			} else {
				above_mode = want->bmodes[index - 4U];
			}
			if ((index % 4U) == 0U) {
				left_mode = left[index / 4U];
			} else {
				left_mode = want->bmodes[index - 1U];
			}
			failed = boolwriter_tree(out, webpc_vp8_bmode_tree, 18U, webpc_vp8_kf_bmode_prob[above_mode][left_mode], 0U, (int)want->bmodes[index]);
			CHECK(failed == 0);
		}
	}

	/* The chroma mode. */
	failed = boolwriter_tree(out, webpc_vp8_uv_mode_tree, 6U, webpc_vp8_kf_uv_mode_prob, 0U, (int)want->uvmode);
	CHECK(failed == 0);
}

/*
 * Fills want with a macroblock of luma mode ymode, random other fields (a
 * segment and a skip only when the header reads them) and, unless ymode is
 * B_PRED, the subblock modes that the luma mode implies by RFC 6386 11.3's
 * list (worked out here from its text).
 */
static void
make_modes(
	struct webpc_vp8_modes *want,
	const struct webpc_vp8_header *header,
	unsigned ymode)
{
	unsigned index;
	unsigned implied;

	/* The fields the header can code. */
	memset(want, 0, sizeof(*want));
	if (header->segmentation.update_map != 0) {
		want->segment = random_below(4U);
	}
	if (header->skip_enabled != 0) {
		want->skip = (int)random_below(2U);
	}
	want->ymode = ymode;
	want->uvmode = random_below(4U);

	/* B_PRED codes ten kinds of subblock modes; the others imply one by 11.3's list. */
	if (ymode == WEBPC_VP8_B_PRED) {
		for (index = 0U; index < 16U; index++) {
			want->bmodes[index] = (uint8_t)random_below(10U);
		}
		return;
	}
	if (ymode == WEBPC_VP8_DC_PRED) {
		implied = 0U;
	} else if (ymode == WEBPC_VP8_V_PRED) {
		implied = 2U;
	} else if (ymode == WEBPC_VP8_H_PRED) {
		implied = 3U;
	} else {
		implied = 1U;
	}
	for (index = 0U; index < 16U; index++) {
		want->bmodes[index] = (uint8_t)implied;
	}
}

/*
 * Checks every field of got against want.
 */
static void
check_modes_equal(
	const struct webpc_vp8_modes *got,
	const struct webpc_vp8_modes *want)
{
	unsigned index;

	/* The scalar fields. */
	CHECK(got->segment == want->segment);
	CHECK(got->skip == want->skip);
	CHECK(got->ymode == want->ymode);
	CHECK(got->uvmode == want->uvmode);

	/* The 16 subblock modes. */
	for (index = 0U; index < 16U; index++) {
		CHECK(got->bmodes[index] == want->bmodes[index]);
	}
}

/*
 * Writes one macroblock with the given contexts, reads it back, and checks
 * the modes, that the partition did not end, and that the contexts became
 * the macroblock's bottom row and right column.
 */
static void
check_one(
	const struct webpc_vp8_header *header,
	const struct webpc_vp8_modes *want,
	const unsigned above[4],
	const unsigned left[4])
{
	struct webpc_bool decoder;
	struct webpc_vp8_modes got;
	uint8_t read_above[4];
	uint8_t read_left[4];
	size_t size;
	unsigned index;

	/* The partition holding the one header. */
	boolwriter_init(&writer);
	write_macroblock(&writer, header, want, above, left);
	size = boolwriter_finish(&writer);
	CHECK(writer.overflow == 0);

	/* The contexts the reader starts from. */
	for (index = 0U; index < 4U; index++) {
		read_above[index] = (uint8_t)above[index];
		read_left[index] = (uint8_t)left[index];
	}

	/* Reads it back. */
	memset(&got, 0xa5, sizeof(got));
	webpc_bool_init(&decoder, writer.bytes, size);
	webpc_vp8_read_modes(&decoder, header, read_above, read_left, &got);
	CHECK(webpc_bool_ended(&decoder) == 0);
	check_modes_equal(&got, want);

	/* The contexts are now this macroblock's bottom row and right column. */
	for (index = 0U; index < 4U; index++) {
		CHECK(read_above[index] == want->bmodes[12U + index]);
		CHECK(read_left[index] == want->bmodes[(index * 4U) + 3U]);
	}
}

/*
 * Runs a frame of FRAME_MB_WIDTH x FRAME_MB_HEIGHT macroblocks: writes every
 * macroblock in raster order into one partition (a macroblock is B_PRED
 * with probability bpred_percent, else a random 16x16 mode), with the
 * contexts taken from the grid of all subblock modes and the given borders,
 * then reads it all back carrying the above row and the left column as a
 * decoder would, and checks every macroblock and the final contexts.
 */
static void
run_frame(
	const struct webpc_vp8_header *header,
	const unsigned border_above[GRID_WIDTH],
	const unsigned border_left[GRID_HEIGHT],
	unsigned bpred_percent)
{
	struct webpc_vp8_modes want[FRAME_MB_HEIGHT][FRAME_MB_WIDTH];
	struct webpc_vp8_modes got;
	struct webpc_bool decoder;
	unsigned grid[GRID_HEIGHT][GRID_WIDTH];
	unsigned above_context[4];
	unsigned left_context[4];
	uint8_t above_row[FRAME_MB_WIDTH][4];
	uint8_t left_column[4];
	unsigned mb_row;
	unsigned mb_column;
	unsigned ymode;
	unsigned index;
	size_t size;

	/* Writes the macroblocks in raster order, each with the contexts the grid gives. */
	boolwriter_init(&writer);
	memset(grid, 0, sizeof(grid));
	for (mb_row = 0U; mb_row < FRAME_MB_HEIGHT; mb_row++) {
		for (mb_column = 0U; mb_column < FRAME_MB_WIDTH; mb_column++) {
			if (random_below(100U) < bpred_percent) {
				ymode = WEBPC_VP8_B_PRED;
			} else {
				ymode = random_below(4U);
			}
			make_modes(&want[mb_row][mb_column], header, ymode);

			/* The four subblocks above and to the left of this macroblock. */
			for (index = 0U; index < 4U; index++) {
				above_context[index] = grid_context(grid, border_above, border_left, (int)(mb_row * 4U) - 1, (int)((mb_column * 4U) + index));
				left_context[index] = grid_context(grid, border_above, border_left, (int)((mb_row * 4U) + index), (int)(mb_column * 4U) - 1);
			}
			write_macroblock(&writer, header, &want[mb_row][mb_column], above_context, left_context);

			/* The macroblock's modes join the grid. */
			for (index = 0U; index < 16U; index++) {
				grid[(mb_row * 4U) + (index / 4U)][(mb_column * 4U) + (index % 4U)] = want[mb_row][mb_column].bmodes[index];
			}
		}
	}
	size = boolwriter_finish(&writer);
	CHECK(writer.overflow == 0);

	/* The row above the frame and the left column start from the borders. */
	for (mb_column = 0U; mb_column < FRAME_MB_WIDTH; mb_column++) {
		for (index = 0U; index < 4U; index++) {
			above_row[mb_column][index] = (uint8_t)border_above[(mb_column * 4U) + index];
		}
	}
	webpc_bool_init(&decoder, writer.bytes, size);

	/* Reads the macroblocks in the same order, the left column restarting at each row. */
	for (mb_row = 0U; mb_row < FRAME_MB_HEIGHT; mb_row++) {
		for (index = 0U; index < 4U; index++) {
			left_column[index] = (uint8_t)border_left[(mb_row * 4U) + index];
		}
		for (mb_column = 0U; mb_column < FRAME_MB_WIDTH; mb_column++) {
			memset(&got, 0xa5, sizeof(got));
			webpc_vp8_read_modes(&decoder, header, above_row[mb_column], left_column, &got);
			check_modes_equal(&got, &want[mb_row][mb_column]);
		}

		/* After the row, the left column is the last macroblock's right column (the next row resets it). */
		for (index = 0U; index < 4U; index++) {
			CHECK(left_column[index] == want[mb_row][FRAME_MB_WIDTH - 1U].bmodes[(index * 4U) + 3U]);
		}
	}
	CHECK(webpc_bool_ended(&decoder) == 0);

	/* The row above holds the bottom macroblock row's bottom subblocks. */
	for (mb_column = 0U; mb_column < FRAME_MB_WIDTH; mb_column++) {
		for (index = 0U; index < 4U; index++) {
			CHECK(above_row[mb_column][index] == want[FRAME_MB_HEIGHT - 1U][mb_column].bmodes[12U + index]);
		}
	}
}

/*
 * Reads each luma mode with each chroma mode, at the frame's edge (every
 * context B_DC_PRED), with and without B_PRED's 16 modes.
 */
static void
test_single_modes(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_modes want;
	unsigned zero[4];
	unsigned ymode;
	unsigned uvmode;
	unsigned round;
	unsigned index;

	/* No segment, no skip; every luma mode against every chroma mode. */
	fill_header(&header, 0, 0);
	memset(zero, 0, sizeof(zero));
	for (ymode = 0U; ymode < 5U; ymode++) {
		for (uvmode = 0U; uvmode < 4U; uvmode++) {
			for (round = 0U; round < 4U; round++) {
				make_modes(&want, &header, ymode);
				want.uvmode = uvmode;
				check_one(&header, &want, zero, zero);
			}
		}
	}

	/* B_PRED with every subblock the same mode, each of the ten modes. */
	for (uvmode = 0U; uvmode < 10U; uvmode++) {
		make_modes(&want, &header, WEBPC_VP8_B_PRED);
		for (index = 0U; index < 16U; index++) {
			want.bmodes[index] = (uint8_t)uvmode;
		}
		check_one(&header, &want, zero, zero);
	}
}

/*
 * Reads the segment and skip flag, whether the header has them or not.
 */
static void
test_flags(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_modes want;
	unsigned zero[4];
	unsigned update_map;
	unsigned skip_enabled;
	unsigned segment;
	unsigned skip;
	unsigned ymode;

	/* Every combination of the two flags with every segment and skip value. */
	memset(zero, 0, sizeof(zero));
	for (update_map = 0U; update_map < 2U; update_map++) {
		for (skip_enabled = 0U; skip_enabled < 2U; skip_enabled++) {
			fill_header(&header, (int)update_map, (int)skip_enabled);
			for (ymode = 0U; ymode < 5U; ymode++) {
				for (segment = 0U; segment < 4U; segment++) {
					for (skip = 0U; skip < 2U; skip++) {
						make_modes(&want, &header, ymode);
						if (update_map != 0U) {
							want.segment = segment;
						}
						if (skip_enabled != 0U) {
							want.skip = (int)skip;
						}
						check_one(&header, &want, zero, zero);
					}
				}
			}
		}
	}
}

/*
 * Reads a B_PRED macroblock whose above and left contexts are modes that
 * differ from each other, so a swapped [above][left] shows.
 */
static void
test_contexts_of_one(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_modes want;
	unsigned above[4];
	unsigned left[4];
	unsigned round;
	unsigned index;

	/* Fixed contexts, as an asymmetric pair of rows. */
	fill_header(&header, 1, 1);
	for (index = 0U; index < 4U; index++) {
		above[index] = 2U + index;
		left[index] = 9U - (index * 3U);
	}
	for (round = 0U; round < 20U; round++) {
		make_modes(&want, &header, WEBPC_VP8_B_PRED);
		check_one(&header, &want, above, left);
	}

	/* Random contexts from the ten modes. */
	for (round = 0U; round < 200U; round++) {
		for (index = 0U; index < 4U; index++) {
			above[index] = random_below(10U);
			left[index] = random_below(10U);
		}
		make_modes(&want, &header, WEBPC_VP8_B_PRED);
		check_one(&header, &want, above, left);
	}
}

/*
 * Reads 3x2 frames whose borders are B_DC_PRED (the frame's edge), mostly
 * B_PRED so the subblock contexts of a macroblock come from the ones above
 * and to the left.
 */
static void
test_frame_edges(void)
{
	struct webpc_vp8_header header;
	unsigned border_above[GRID_WIDTH];
	unsigned border_left[GRID_HEIGHT];
	unsigned round;

	/* Every context beyond the frame is B_DC_PRED, as the reader must start. */
	memset(border_above, 0, sizeof(border_above));
	memset(border_left, 0, sizeof(border_left));
	for (round = 0U; round < 100U; round++) {
		fill_header(&header, (int)(round % 2U), (int)((round / 2U) % 2U));
		run_frame(&header, border_above, border_left, 100U);
	}
}

/*
 * Reads 3x2 frames with a mix of B_PRED and 16x16 macroblocks, so the
 * implied subblock modes serve as contexts too.
 */
static void
test_frame_3x2(void)
{
	struct webpc_vp8_header header;
	unsigned border_above[GRID_WIDTH];
	unsigned border_left[GRID_HEIGHT];
	unsigned percent;
	unsigned round;

	/* Frames from all 16x16 macroblocks to all B_PRED. */
	memset(border_above, 0, sizeof(border_above));
	memset(border_left, 0, sizeof(border_left));
	for (percent = 0U; percent <= 100U; percent += 25U) {
		for (round = 0U; round < 40U; round++) {
			fill_header(&header, 1, 1);
			run_frame(&header, border_above, border_left, percent);
		}
	}
}

/*
 * Reads random frames with random probabilities and random borders.
 */
static void
test_frame_random(void)
{
	struct webpc_vp8_header header;
	unsigned border_above[GRID_WIDTH];
	unsigned border_left[GRID_HEIGHT];
	unsigned round;
	unsigned index;

	/* The borders are random modes here (they are zero in a real frame). */
	for (round = 0U; round < 1000U; round++) {
		random_header(&header);
		for (index = 0U; index < GRID_WIDTH; index++) {
			border_above[index] = random_below(10U);
		}
		for (index = 0U; index < GRID_HEIGHT; index++) {
			border_left[index] = random_below(10U);
		}
		run_frame(&header, border_above, border_left, random_below(101U));
	}
}

/*
 * A header that has no segment map and no skip flag leaves them 0 whatever
 * the structure held before and whatever probabilities the header carries.
 */
static void
test_not_read_fields(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_modes got;
	struct webpc_bool decoder;
	uint8_t above[4];
	uint8_t left[4];
	uint8_t data[64];
	unsigned round;
	unsigned index;

	/* Random data, with a header that reads neither field but has probabilities set. */
	fill_header(&header, 0, 0);
	header.segmentation.enabled = 1;
	header.segmentation.update_data = 1;
	header.skip_prob = 33U;
	for (round = 0U; round < 50U; round++) {
		for (index = 0U; index < sizeof(data); index++) {
			data[index] = (uint8_t)random_below(256U);
		}
		memset(above, 0, sizeof(above));
		memset(left, 0, sizeof(left));
		memset(&got, 0xff, sizeof(got));
		got.segment = 99U;
		got.skip = 77;
		webpc_bool_init(&decoder, data, sizeof(data));
		webpc_vp8_read_modes(&decoder, &header, above, left, &got);
		CHECK(got.segment == 0U);
		CHECK(got.skip == 0);
	}
}

/*
 * Reads from an empty partition and from partitions cut short: nothing
 * outside the data is read, every value is in its range, and the decoder
 * says it has ended.
 */
static void
test_ended(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_modes got;
	struct webpc_vp8_modes want;
	struct webpc_bool decoder;
	uint8_t above[4];
	uint8_t left[4];
	unsigned zero[4];
	unsigned cut;
	size_t size;
	unsigned index;

	/* An empty partition: every bool reads 0, so the first value of every tree. */
	fill_header(&header, 1, 1);
	memset(above, 0, sizeof(above));
	memset(left, 0, sizeof(left));
	webpc_bool_init(&decoder, NULL, 0U);
	webpc_vp8_read_modes(&decoder, &header, above, left, &got);
	CHECK(webpc_bool_ended(&decoder) == 1);
	CHECK(got.segment < 4U);
	CHECK(got.ymode <= WEBPC_VP8_B_PRED);
	CHECK(got.uvmode <= WEBPC_VP8_TM_PRED);
	for (index = 0U; index < 16U; index++) {
		CHECK(got.bmodes[index] < 10U);
	}

	/* A B_PRED macroblock written whole, then every cut of its partition. */
	memset(zero, 0, sizeof(zero));
	make_modes(&want, &header, WEBPC_VP8_B_PRED);
	boolwriter_init(&writer);
	write_macroblock(&writer, &header, &want, zero, zero);
	size = boolwriter_finish(&writer);
	CHECK(size > 8U);
	for (cut = 0U; cut < size; cut++) {
		memset(above, 0, sizeof(above));
		memset(left, 0, sizeof(left));
		webpc_bool_init(&decoder, writer.bytes, cut);
		webpc_vp8_read_modes(&decoder, &header, above, left, &got);
		CHECK(got.ymode <= WEBPC_VP8_B_PRED);
		CHECK(got.uvmode <= WEBPC_VP8_TM_PRED);
		if (cut == 0U) {
			CHECK(webpc_bool_ended(&decoder) == 1);
		}
	}

	/* The whole partition reads back and has not ended. */
	memset(above, 0, sizeof(above));
	memset(left, 0, sizeof(left));
	webpc_bool_init(&decoder, writer.bytes, size);
	webpc_vp8_read_modes(&decoder, &header, above, left, &got);
	CHECK(webpc_bool_ended(&decoder) == 0);
	check_modes_equal(&got, &want);
}

/*
 * Reads random bytes with random headers: every value is in its range and
 * the contexts stay modes.
 */
static void
test_random_bytes(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_modes got;
	struct webpc_bool decoder;
	uint8_t above[4];
	uint8_t left[4];
	uint8_t data[200];
	unsigned round;
	unsigned step;
	unsigned index;

	/* Several macroblocks per partition, so the contexts carry on random modes. */
	for (round = 0U; round < 2000U; round++) {
		random_header(&header);
		for (index = 0U; index < sizeof(data); index++) {
			data[index] = (uint8_t)random_below(256U);
		}
		memset(above, 0, sizeof(above));
		memset(left, 0, sizeof(left));
		webpc_bool_init(&decoder, data, sizeof(data));
		for (step = 0U; step < 6U; step++) {
			webpc_vp8_read_modes(&decoder, &header, above, left, &got);
			CHECK(got.segment < 4U);
			CHECK(got.skip == 0 || got.skip == 1);
			CHECK(got.ymode <= WEBPC_VP8_B_PRED);
			CHECK(got.uvmode <= WEBPC_VP8_TM_PRED);
			for (index = 0U; index < 16U; index++) {
				CHECK(got.bmodes[index] < 10U);
			}
			for (index = 0U; index < 4U; index++) {
				CHECK(above[index] == got.bmodes[12U + index]);
				CHECK(left[index] == got.bmodes[(index * 4U) + 3U]);
			}
		}
	}
}

/*
 * Contexts that hold a value no subblock mode has are read as B_DC_PRED, so
 * the probability table is never indexed outside; the macroblock then reads
 * as it would with B_DC_PRED there.
 */
static void
test_bad_contexts(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_modes want;
	struct webpc_vp8_modes got;
	struct webpc_bool decoder;
	uint8_t above[4];
	uint8_t left[4];
	unsigned zero[4];
	unsigned values[4];
	size_t size;
	unsigned round;
	unsigned index;

	/* A B_PRED macroblock written with B_DC_PRED contexts, read with bad ones. */
	fill_header(&header, 0, 0);
	memset(zero, 0, sizeof(zero));
	values[0] = 10U;
	values[1] = 11U;
	values[2] = 200U;
	values[3] = 255U;
	for (round = 0U; round < 20U; round++) {
		make_modes(&want, &header, WEBPC_VP8_B_PRED);
		boolwriter_init(&writer);
		write_macroblock(&writer, &header, &want, zero, zero);
		size = boolwriter_finish(&writer);
		for (index = 0U; index < 4U; index++) {
			above[index] = (uint8_t)values[index];
			left[index] = (uint8_t)values[3U - index];
		}
		webpc_bool_init(&decoder, writer.bytes, size);
		webpc_vp8_read_modes(&decoder, &header, above, left, &got);
		check_modes_equal(&got, &want);
	}
}
