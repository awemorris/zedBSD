/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scaling lists of H.265: scaling_list_data() and the default lists
 * (7.3.4, 7.4.5).
 */

#include "h265sw-scaling.h"

#include <errno.h>
#include <string.h>

/* The number of sizeId values (Table 7-3) and of matrixId values (Table 7-4). */
#define SCALING_SIZE_IDS 4U
#define SCALING_MATRIX_IDS 6U

/* The first matrixId of the inter lists (Table 7-4: matrixId 3..5 are MODE_INTER). */
#define SCALING_FIRST_INTER 3U

/* The entries of a list of sizeId 0 (a 4x4 matrix) and of a larger one (an 8x8 base matrix, 7.3.4: Min( 64, ... )). */
#define SCALING_SMALL_ENTRIES 16U
#define SCALING_LARGE_ENTRIES 64U

/* The value of the DC coefficient of a default list (7.4.5: scaling_list_dc_coef_minus8 inferred 8). */
#define SCALING_DEFAULT_DC 16U

/* The range of scaling_list_dc_coef_minus8 and scaling_list_delta_coef (7.4.5). */
#define SCALING_DC_MINUS8_MIN (-7)
#define SCALING_DC_MINUS8_MAX 247
#define SCALING_DELTA_MIN (-128)
#define SCALING_DELTA_MAX 127

/*
 * The default list of sizeId 1..3 for an intra matrixId (0..2).
 *
 * Values referenced: ITU-T H.265 (01/2026) Table 7-6, the row
 * ScalingList[ 1..3 ][ 0..2 ][ i ] with i = 0..63.  Taken from the text of
 * the table and checked against an image of the page.
 */
static const uint8_t scaling_default_intra[SCALING_LARGE_ENTRIES] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18,
	17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
	24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31,
	29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115
};

/*
 * The default list of sizeId 1..3 for an inter matrixId (3..5).
 *
 * Values referenced: ITU-T H.265 (01/2026) Table 7-6, the row
 * ScalingList[ 1..3 ][ 3..5 ][ i ] with i = 0..63.  Taken from the text of
 * the table and checked against an image of the page.
 */
static const uint8_t scaling_default_inter[SCALING_LARGE_ENTRIES] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18,
	18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28,
	28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91
};

/* Static function declarations. */
static unsigned scaling_entries(unsigned size_id);
static void scaling_default_copy(uint8_t *destination, unsigned size_id, unsigned matrix_id);

/*
 * Sets list to the default lists (Tables 7-5 and 7-6).
 *
 * Only the lists the syntax codes are set (sizeId 3 has matrixId 0 and 3);
 * everything else stays 0.
 */
void
h265sw_scaling_default(
	struct h265sw_scaling_list *list)
{
	unsigned size_id;
	unsigned matrix_id;
	unsigned step;

	/* Clears every entry, so the entries outside the lists are 0. */
	memset(list, 0, sizeof(*list));

	/* Fills each list the syntax codes with its default and, for sizeId 2 and 3, the default DC. */
	for (size_id = 0U; size_id < SCALING_SIZE_IDS; size_id++) {
		step = 1U;
		if (size_id == 3U)
			step = 3U;

		for (matrix_id = 0U; matrix_id < SCALING_MATRIX_IDS; matrix_id += step) {
			scaling_default_copy(list->list[size_id][matrix_id], size_id, matrix_id);
			if (size_id > 1U)
				list->dc[size_id - 2U][matrix_id] = SCALING_DEFAULT_DC;
		}
	}
}

/*
 * Reads scaling_list_data() (7.3.4) into list.
 *
 * The reference of a predicted list is always an earlier matrixId of the
 * same sizeId, which is already read, so each list is complete when the
 * next one starts.
 */
int
h265sw_scaling_read(
	struct h265sw_scaling_list *list,
	struct vcodec_bitreader *bits)
{
	unsigned size_id;
	unsigned matrix_id;
	unsigned step;
	unsigned entries;
	unsigned entry;
	unsigned pred_mode;
	unsigned max_delta;
	unsigned reference;
	uint32_t matrix_id_delta;
	int32_t dc_minus8;
	int32_t delta_coef;
	int32_t next_coef;

	/* Clears every entry first, so the whole structure is written even for the lists the syntax skips. */
	memset(list, 0, sizeof(*list));

	/* Reads every list the syntax codes: six per size, two for the 32x32 size. */
	for (size_id = 0U; size_id < SCALING_SIZE_IDS; size_id++) {
		step = 1U;
		if (size_id == 3U)
			step = 3U;
		entries = scaling_entries(size_id);

		for (matrix_id = 0U; matrix_id < SCALING_MATRIX_IDS; matrix_id += step) {
			/* Reads whether the list is signalled or predicted. */
			pred_mode = vcodec_bitreader_u1(bits);
			if (bits->error != 0)
				return EINVAL;

			if (pred_mode == 0U) {
				/* Reads which earlier list, or the default, this list copies. */
				matrix_id_delta = vcodec_bitreader_ue(bits);
				if (bits->error != 0)
					return EINVAL;

				/* Refuses a reference before the first list (7.4.5). */
				max_delta = matrix_id;
				if (size_id == 3U)
					max_delta = matrix_id / 3U;
				if (matrix_id_delta > max_delta)
					return EINVAL;

				if (matrix_id_delta == 0U) {
					/* Copies the default list and, for the larger sizes, the default DC. */
					scaling_default_copy(list->list[size_id][matrix_id], size_id, matrix_id);
					if (size_id > 1U)
						list->dc[size_id - 2U][matrix_id] = SCALING_DEFAULT_DC;
				} else {
					/* Copies the referenced list (7-42, 7-43) and its DC. */
					reference = matrix_id - (unsigned)matrix_id_delta * step;
					memcpy(list->list[size_id][matrix_id], list->list[size_id][reference], entries);
					if (size_id > 1U)
						list->dc[size_id - 2U][matrix_id] = list->dc[size_id - 2U][reference];
				}

				continue;
			}

			/* A signalled list starts from 8, or from the DC value for the larger sizes (7.3.4). */
			next_coef = 8;
			if (size_id > 1U) {
				dc_minus8 = vcodec_bitreader_se(bits);
				if (bits->error != 0)
					return EINVAL;

				/* Refuses a DC outside -7..247 (7.4.5). */
				if (dc_minus8 < SCALING_DC_MINUS8_MIN || dc_minus8 > SCALING_DC_MINUS8_MAX)
					return EINVAL;

				list->dc[size_id - 2U][matrix_id] = (uint8_t)(dc_minus8 + 8);
				next_coef = dc_minus8 + 8;
			}

			/* Reads the entries as differences that wrap modulo 256. */
			for (entry = 0U; entry < entries; entry++) {
				delta_coef = vcodec_bitreader_se(bits);
				if (bits->error != 0)
					return EINVAL;

				/* Refuses a difference outside -128..127 (7.4.5). */
				if (delta_coef < SCALING_DELTA_MIN || delta_coef > SCALING_DELTA_MAX)
					return EINVAL;

				next_coef = (next_coef + delta_coef + 256) % 256;

				/* Refuses an entry of 0: the scaling factor shall be greater than 0 (7.4.5). */
				if (next_coef == 0)
					return EINVAL;

				list->list[size_id][matrix_id][entry] = (uint8_t)next_coef;
			}
		}
	}

	/* Succeeded: every list is read. */
	return 0;
}

/*
 * Reports how many entries a list of sizeId has: 16 for sizeId 0, else 64
 * (coefNum of 7.3.4).
 */
static unsigned
scaling_entries(
	unsigned size_id)
{
	/* The 4x4 matrix has 16 entries. */
	if (size_id == 0U)
		return SCALING_SMALL_ENTRIES;

	/* Larger matrices are coded as 64 entries of an 8x8 base. */
	return SCALING_LARGE_ENTRIES;
}

/*
 * Copies the default list of a sizeId and matrixId into destination (Table
 * 7-5 for sizeId 0, Table 7-6 otherwise).
 */
static void
scaling_default_copy(
	uint8_t *destination,
	unsigned size_id,
	unsigned matrix_id)
{
	/*
	 * Every default list of sizeId 0 is flat.
	 *
	 * Values referenced: ITU-T H.265 (01/2026) Table 7-5, all values 16.
	 */
	if (size_id == 0U) {
		memset(destination, 16, SCALING_SMALL_ENTRIES);
		return;
	}

	/* The intra matrixIds share one default list. */
	if (matrix_id < SCALING_FIRST_INTER) {
		memcpy(destination, scaling_default_intra, SCALING_LARGE_ENTRIES);
		return;
	}

	/* The inter matrixIds share the other. */
	memcpy(destination, scaling_default_inter, SCALING_LARGE_ENTRIES);
}
