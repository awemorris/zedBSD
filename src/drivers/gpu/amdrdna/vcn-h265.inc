/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.265 part of the video engine's decode messages (included by
 * vcn-format.c, whose writers it uses): the H.265 body of a decode
 * message, the tiles and the scaling lists a picture decodes with, and
 * the sizes of an H.265 session's hidden DPB and decoder context.
 *
 * As for H.264, the firmware keeps the reference pictures in a DPB of its
 * own and knows each by a tag: the picture's DPB slot, or
 * AMDRDNA_VCN_H265_LOOSE_TAG for a picture that has none.  The message
 * lists the references' tags with their order counts, and names the
 * references of the picture's three current sets by their places in
 * that list.
 */

/*
 * The H.265 body's fields, in bytes from its start: the sequence and
 * picture flags; the chroma format, the bit depths and the order count's
 * size; the DPB size; the coding, transform and PCM block sizes and
 * depths; the slice header's extra bits; the sequence's reference set
 * counts and the default active references; the chroma and deblocking
 * offsets; the quantizer's depth, the tiles and the merge level; the
 * tiles' column widths and row heights; the initial quantizer, the size
 * of the slice header's own short-term set and the picture's tag; the
 * picture's order count; the references' tags and order counts; the three
 * current sets; the scaling lists' DC coefficients; the highest
 * sub-layer (highestTid); the bit-depth conversion fields; the direct
 * reference lists; the bits of the slice header's own short-term set.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h (the H.265
 * body of a decode message).
 * derived: the offsets of the body's fields in their declared order with
 * their natural alignment; the body is 296 bytes.
 */
#define H265_BODY_BYTES			296U
#define H265_SPS_FLAGS_AT		0U
#define H265_PPS_FLAGS_AT		4U
#define H265_CHROMA_FORMAT_AT		8U
#define H265_LUMA_DEPTH_AT		9U
#define H265_CHROMA_DEPTH_AT		10U
#define H265_POC_BITS_AT		11U
#define H265_DPB_SIZE_AT		12U
#define H265_MIN_CB_AT			13U
#define H265_DIFF_CB_AT			14U
#define H265_MIN_TB_AT			15U
#define H265_DIFF_TB_AT			16U
#define H265_DEPTH_INTER_AT		17U
#define H265_DEPTH_INTRA_AT		18U
#define H265_PCM_LUMA_AT		19U
#define H265_PCM_CHROMA_AT		20U
#define H265_PCM_MIN_AT			21U
#define H265_PCM_DIFF_AT		22U
#define H265_EXTRA_BITS_AT		23U
#define H265_SHORT_TERM_SETS_AT		24U
#define H265_LONG_TERM_SPS_AT		25U
#define H265_L0_ACTIVE_AT		26U
#define H265_L1_ACTIVE_AT		27U
#define H265_CB_OFFSET_AT		28U
#define H265_CR_OFFSET_AT		29U
#define H265_BETA_AT			30U
#define H265_TC_AT			31U
#define H265_QP_DEPTH_AT		32U
#define H265_TILE_COLUMNS_AT		33U
#define H265_TILE_ROWS_AT		34U
#define H265_MERGE_LEVEL_AT		35U
#define H265_COLUMN_WIDTHS_AT		36U
#define H265_ROW_HEIGHTS_AT		74U
#define H265_INIT_QP_AT			116U
#define H265_DELTA_POCS_AT		117U
#define H265_TAG_AT			118U
#define H265_ORDER_AT			120U
#define H265_TAGS_AT			124U
#define H265_ORDERS_AT			140U
#define H265_BEFORE_AT			204U
#define H265_AFTER_AT			212U
#define H265_LONG_TERM_AT		220U
#define H265_DC16_AT			228U
#define H265_DC32_AT			234U
#define H265_HIGHEST_TID_AT		236U
#define H265_DIRECT_AT			244U
#define H265_RPS_BITS_AT		276U

/*
 * The index kind of an H.265 body in the message's header.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h
 * (RDECODE_MESSAGE_HEVC).
 */
#define H265_INDEX			0x0000000dU

/*
 * The sequence flags of the body (scaling lists, asymmetric motion
 * partitions, sample adaptive offset, PCM and its loop filter's absence,
 * long-term references, temporal motion vector prediction, strong intra
 * smoothing, separate colour planes, and a short-term set the slice
 * header carries itself) and the picture flags (dependent slices, the
 * output flag, sign data hiding, CABAC initialization, constrained intra,
 * transform skip, quantizer deltas, slice chroma offsets, weighted
 * prediction and bi-prediction, transquant bypass, tiles, wavefronts,
 * uniform spacing, the loop filter across tiles and across slices, the
 * deblocking override, deblocking off, list modification and slice header
 * extensions).
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (get_h265_msg: the bits of sps_info_flags and pps_info_flags).
 */
#define H265_SPS_SCALING		(1U << 0)
#define H265_SPS_AMP			(1U << 1)
#define H265_SPS_SAO			(1U << 2)
#define H265_SPS_PCM			(1U << 3)
#define H265_SPS_PCM_FILTER_OFF		(1U << 4)
#define H265_SPS_LONG_TERM		(1U << 5)
#define H265_SPS_TEMPORAL_MVP		(1U << 6)
#define H265_SPS_STRONG_INTRA		(1U << 7)
#define H265_SPS_SEPARATE_PLANES	(1U << 8)
#define H265_SPS_SLICE_RPS		(1U << 11)
#define H265_PPS_LOW_FLAGS		0x0000ffffU
#define H265_PPS_DEBLOCK_OVERRIDE	(1U << 16)
#define H265_PPS_DEBLOCK_OFF		(1U << 17)
#define H265_PPS_LIST_CHANGES		(1U << 18)
#define H265_PPS_HEADER_EXTENSION	(1U << 19)

/*
 * A reference list entry that names no reference, and a direct
 * reference list entry that names none.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (get_h265_msg: ref_pic_list filled with 0x7f, direct_reflist with
 * 0xff).
 */
#define H265_NO_TAG			0x7fU
#define H265_NO_DIRECT			0xffU
#define H265_DIRECT_ENTRIES		30U

/*
 * The scaling table the inverse transform reads: the six 4x4 lists from
 * byte 0, the six 8x8 lists from byte 96, the six 16x16 lists from byte
 * 480 and the two 32x32 lists from byte 864, each in the order the
 * standard's syntax carries its coefficients (the up-right diagonal
 * scan), as Vulkan's StdVideoH265ScalingLists holds them.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (update_h265_scaling: the lists copied into it_ptr as given) and
 * src/gallium/frontends/va/picture_hevc.c (the lists VA-API gives in
 * raster order put back into the diagonal scan for the same table).
 */
#define H265_TABLE_4X4_AT		0U
#define H265_TABLE_8X8_AT		96U
#define H265_TABLE_16X16_AT		480U
#define H265_TABLE_32X32_AT		864U
#define H265_TABLE_BYTES		992U

/* The 8x8 and larger lists' places: matrixId 0 to 2 are intra, 3 to 5 inter; of the two 32x32 lists, the first intra. */
#define H265_INTRA_LISTS		3U

/* The flat matrix's coefficient, and the DC coefficient of a default list. */
#define H265_FLAT			16U

/*
 * The coding tree block's largest side (as a power of two), and the
 * smallest coding block's.
 *
 * Values referenced: Recommendation ITU-T H.265 (7.4.3.2.1, CtbLog2SizeY
 * of 4 to 6, MinCbLog2SizeY of at least 3).
 */
#define H265_CTB_LOG2_MAX		6U
#define H265_MIN_CB_LOG2_BASE		3U

/*
 * The hidden DPB of an H.265 session: the sides first on 16 samples, then
 * each picture its luma and chroma samples with the sides on 32 samples,
 * on 256 bytes; as many pictures as the session's slots and one more, and
 * at least 17, or at least 8 for a picture of 4096 by 2000 samples or
 * more.
 *
 * Values referenced: Mesa 25.0.7
 * src/gallium/drivers/radeonsi/radeon_vcn_dec.c (calc_dpb_size: the
 * picture's size and the count for HEVC) and src/amd/vulkan/radv_video.c
 * (radv_video_get_db_alignment: 32 for H.265 Main).
 */
#define H265_SIDE_ALIGN			16U
#define H265_DPB_PICTURE_ALIGN		256U
#define H265_DPB_PICTURES_MIN		17U
#define H265_DPB_PICTURES_LARGE		8U
#define H265_LARGE_WIDTH		4096U
#define H265_LARGE_HEIGHT		2000U

/*
 * The decoder context of an H.265 Main session: 16 bytes for each 16 by
 * 16 block of the largest picture grown by 255 samples each way, for each
 * of as many pictures as its DPB (counted as above), and 52 KiB; in whole
 * pages.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (calc_ctx_size_h265_main, and the memory requirement on 4096 bytes in
 * radv_GetVideoSessionMemoryRequirementsKHR).
 */
#define H265_CONTEXT_GROW		255U
#define H265_CONTEXT_BLOCK_BYTES	16U
#define H265_CONTEXT_EXTRA_BYTES	(52U * 1024U)
#define H265_CONTEXT_PAGE		4096U

static void h265_body_sets(uint8_t *body, const struct amdrdna_vcn_h265_sps *sps, const struct amdrdna_vcn_h265_pps *pps);
static void h265_body_picture(uint8_t *body, const struct amdrdna_vcn_h265_decode *h265);
static void h265_spacing(uint32_t total, uint32_t parts, bool uniform, const uint32_t *given, uint32_t *minus1, bool *fits);
static void h265_list_copy(uint8_t *table, size_t at, const uint8_t *source, uint32_t count);
static uint32_t h265_pictures(uint32_t width, uint32_t height, uint32_t slots);
static uint32_t h265_align(uint32_t value, uint32_t boundary);

/*
 * The default 8x8, 16x16 and 32x32 lists, in the order the standard's
 * syntax carries the coefficients (the up-right diagonal scan).
 *
 * Values referenced: Recommendation ITU-T H.265 (Table 7-6, the default
 * values of ScalingList[1..3][matrixId][i] for intra and inter).
 */
static const uint8_t h265_default_intra[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18,
	17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
	24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31,
	29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115
};

/* The default list for the inter lists, read the same way. */
static const uint8_t h265_default_inter[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18,
	18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28,
	28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91
};

/*
 * Writes an H.265 decode message: the header and the decode body from
 * frame, then the H.265 body from h265; and the scaling table the
 * inverse transform reads (scaling, scaling_bytes long; the rest of it
 * zeroed).
 *
 * Returns 0, or EINVAL for a buffer too small, more references or a
 * larger tag than the message holds, a current set's entry that names no
 * reference, or tiles the picture or the message cannot hold.
 */
int
amdrdna_vcn_message_decode_h265(
	uint8_t *message,
	size_t bytes,
	const struct amdrdna_vcn_frame *frame,
	const struct amdrdna_vcn_h265_decode *h265,
	uint8_t *scaling,
	size_t scaling_bytes)
{
	uint32_t columns[AMDRDNA_VCN_H265_TILE_COLUMNS];
	uint32_t rows[AMDRDNA_VCN_H265_TILE_ROWS];
	uint8_t dc16[6];
	uint8_t dc32[2];
	uint8_t *body;
	uint32_t index;
	size_t at;
	int error;

	/* The references and the tags the body holds, a table that holds the lists. */
	if (h265->reference_count > AMDRDNA_VCN_H265_REFERENCES || h265->tag > AMDRDNA_VCN_H265_LOOSE_TAG)
		return EINVAL;
	for (index = 0U; index < h265->reference_count; index++) {
		if (h265->references[index].tag >= AMDRDNA_VCN_H265_LOOSE_TAG)
			return EINVAL;
	}
	if (scaling_bytes < H265_TABLE_BYTES)
		return EINVAL;

	/* Each entry of the three current sets names one of the references, or none. */
	for (index = 0U; index < AMDRDNA_VCN_H265_SET_ENTRIES; index++) {
		if (h265->before[index] != AMDRDNA_VCN_H265_NO_REFERENCE && h265->before[index] >= h265->reference_count)
			return EINVAL;
		if (h265->after[index] != AMDRDNA_VCN_H265_NO_REFERENCE && h265->after[index] >= h265->reference_count)
			return EINVAL;
		if (h265->long_term[index] != AMDRDNA_VCN_H265_NO_REFERENCE && h265->long_term[index] >= h265->reference_count)
			return EINVAL;
	}

	/* The tiles the picture is cut into, which must fit the picture and the body. */
	error = amdrdna_vcn_h265_tiles(h265->sps, h265->pps, columns, rows);
	if (error != 0)
		return error;

	/* The header and the decode body. */
	error = format_message_decode(message, bytes, frame, AMDRDNA_VCN_STREAM_H265, H265_INDEX, H265_BODY_BYTES);
	if (error != 0)
		return error;

	/* What the sets say. */
	body = message + FORMAT_CODEC_AT;
	h265_body_sets(body, h265->sps, h265->pps);

	/* The tiles' column widths and row heights, 16 bits each. */
	for (index = 0U; index < AMDRDNA_VCN_H265_TILE_COLUMNS; index++)
		format_put16(body, H265_COLUMN_WIDTHS_AT + index * 2U, columns[index]);
	for (index = 0U; index < AMDRDNA_VCN_H265_TILE_ROWS; index++)
		format_put16(body, H265_ROW_HEIGHTS_AT + index * 2U, rows[index]);

	/* The scaling lists in the table, their DC coefficients in the body. */
	for (at = 0U; at < scaling_bytes; at++)
		scaling[at] = 0U;
	amdrdna_vcn_h265_scaling(h265->sps, h265->pps, scaling, dc16, dc32);
	for (index = 0U; index < 6U; index++)
		body[H265_DC16_AT + index] = dc16[index];
	for (index = 0U; index < 2U; index++)
		body[H265_DC32_AT + index] = dc32[index];

	/* The picture, its tag and its references. */
	h265_body_picture(body, h265);

	/* No direct reference lists. */
	for (index = 0U; index < H265_DIRECT_ENTRIES; index++)
		body[H265_DIRECT_AT + index] = H265_NO_DIRECT;

	/* Succeeded: the message and the table are written. */
	return 0;
}

/*
 * Reports the tiles' column widths and row heights a picture decodes
 * with, minus 1, in coding tree blocks: the ones the picture set gives
 * with the last made up to the picture, or, with uniform spacing, the
 * standard's even split; all zero when the set has no tiles.  The entries
 * past the tiles are zero.  Returns 0, or EINVAL for a coding tree block
 * larger than the standard's, more tiles than the body holds or the
 * picture has blocks for, or given sizes the picture cannot hold.
 *
 * Procedure referenced: Recommendation ITU-T H.265 (6.5.1, the column
 * widths and row heights; 7.4.3.2.1, CtbLog2SizeY and PicWidthInCtbsY).
 */
int
amdrdna_vcn_h265_tiles(
	const struct amdrdna_vcn_h265_sps *sps,
	const struct amdrdna_vcn_h265_pps *pps,
	uint32_t *columns,
	uint32_t *rows)
{
	uint32_t ctb_log2;
	uint32_t ctb;
	uint32_t width_ctbs;
	uint32_t height_ctbs;
	uint32_t column_count;
	uint32_t row_count;
	uint32_t index;
	bool uniform;
	bool fits;

	/* Nothing until the tiles say otherwise. */
	for (index = 0U; index < AMDRDNA_VCN_H265_TILE_COLUMNS; index++)
		columns[index] = 0U;
	for (index = 0U; index < AMDRDNA_VCN_H265_TILE_ROWS; index++)
		rows[index] = 0U;

	/* A picture set without tiles has one, which the body does not size. */
	if ((pps->flags & AMDRDNA_VCN_H265_PPS_TILES) == 0U)
		return 0;

	/* The coding tree block's side and the picture's size in whole blocks. */
	ctb_log2 = H265_MIN_CB_LOG2_BASE + sps->log2_min_luma_coding_block_size_minus3 + sps->log2_diff_max_min_luma_coding_block_size;
	if (ctb_log2 > H265_CTB_LOG2_MAX)
		return EINVAL;
	ctb = 1U << ctb_log2;
	width_ctbs = (sps->width + ctb - 1U) / ctb;
	height_ctbs = (sps->height + ctb - 1U) / ctb;

	/* As many columns and rows as the body holds and the picture has blocks for. */
	if (pps->num_tile_columns_minus1 >= AMDRDNA_VCN_H265_TILE_COLUMNS || pps->num_tile_rows_minus1 >= AMDRDNA_VCN_H265_TILE_ROWS)
		return EINVAL;
	column_count = pps->num_tile_columns_minus1 + 1U;
	row_count = pps->num_tile_rows_minus1 + 1U;
	if (column_count > width_ctbs || row_count > height_ctbs)
		return EINVAL;

	/* The columns and the rows, given or split evenly. */
	uniform = (pps->flags & AMDRDNA_VCN_H265_PPS_UNIFORM) != 0U;
	h265_spacing(width_ctbs, column_count, uniform, pps->column_width_minus1, columns, &fits);
	if (!fits)
		return EINVAL;
	h265_spacing(height_ctbs, row_count, uniform, pps->row_height_minus1, rows, &fits);
	if (!fits)
		return EINVAL;

	/* Succeeded: the tiles fit. */
	return 0;
}

/*
 * Writes the scaling lists a picture decodes with into the inverse
 * transform's table (in the order the standard's syntax carries them)
 * and reports their DC coefficients: the picture set's lists when it
 * carries them, else the sequence set's, else the standard's defaults
 * when the sequence enables scaling lists; flat lists when it does not.
 *
 * Procedure referenced: Recommendation ITU-T H.265 (7.3.2.2.1 and
 * 7.3.2.3.1, which set's scaling_list_data applies; 7.4.5, the default
 * lists of Tables 7-5 and 7-6 and the DC coefficient of 16 they imply).
 */
void
amdrdna_vcn_h265_scaling(
	const struct amdrdna_vcn_h265_sps *sps,
	const struct amdrdna_vcn_h265_pps *pps,
	uint8_t *table,
	uint8_t dc16[6],
	uint8_t dc32[2])
{
	const struct amdrdna_vcn_h265_scaling *source;
	const uint8_t *fallback;
	uint32_t list;
	uint32_t index;

	/* The picture set's lists, else the sequence set's, when the sequence enables them. */
	source = NULL;
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_SCALING) != 0U) {
		if ((pps->flags & AMDRDNA_VCN_H265_PPS_SCALING_DATA) != 0U && pps->has_scaling) {
			/* The picture set carries its own. */
			source = &pps->scaling;
		} else if ((sps->flags & AMDRDNA_VCN_H265_SPS_SCALING_DATA) != 0U && sps->has_scaling) {
			/* The sequence set carries its own. */
			source = &sps->scaling;
		}
	}

	/* A set's own lists go in as sent. */
	if (source != NULL) {
		for (list = 0U; list < 6U; list++) {
			h265_list_copy(table, H265_TABLE_4X4_AT + list * 16U, source->list4[list], 16U);
			h265_list_copy(table, H265_TABLE_8X8_AT + list * 64U, source->list8[list], 64U);
			h265_list_copy(table, H265_TABLE_16X16_AT + list * 64U, source->list16[list], 64U);
			dc16[list] = source->dc16[list];
		}
		for (list = 0U; list < 2U; list++) {
			h265_list_copy(table, H265_TABLE_32X32_AT + list * 64U, source->list32[list], 64U);
			dc32[list] = source->dc32[list];
		}
		return;
	}

	/* Otherwise every coefficient and every DC coefficient is flat. */
	for (index = 0U; index < H265_TABLE_BYTES; index++)
		table[index] = H265_FLAT;
	for (list = 0U; list < 6U; list++)
		dc16[list] = H265_FLAT;
	for (list = 0U; list < 2U; list++)
		dc32[list] = H265_FLAT;

	/* Scaling lists without a set's own are the defaults: flat 4x4 lists, the intra and inter ones for the larger. */
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_SCALING) != 0U) {
		for (list = 0U; list < 6U; list++) {
			fallback = h265_default_inter;
			if (list < H265_INTRA_LISTS)
				fallback = h265_default_intra;
			h265_list_copy(table, H265_TABLE_8X8_AT + list * 64U, fallback, 64U);
			h265_list_copy(table, H265_TABLE_16X16_AT + list * 64U, fallback, 64U);
		}
		h265_list_copy(table, H265_TABLE_32X32_AT, h265_default_intra, 64U);
		h265_list_copy(table, H265_TABLE_32X32_AT + 64U, h265_default_inter, 64U);
	}
}

/*
 * Reports the bytes of the hidden DPB of an H.265 session whose largest
 * picture is width by height samples and which has slots DPB slots.
 */
uint64_t
amdrdna_vcn_h265_dpb_bytes(
	uint32_t width,
	uint32_t height,
	uint32_t slots)
{
	uint64_t picture;
	uint32_t aligned_width;
	uint32_t aligned_height;

	/* One picture: the sides on 16 then on 32, half again for the chroma, on 256 bytes. */
	aligned_width = h265_align(h265_align(width, H265_SIDE_ALIGN), FORMAT_DPB_ALIGN);
	aligned_height = h265_align(h265_align(height, H265_SIDE_ALIGN), FORMAT_DPB_ALIGN);
	picture = (uint64_t)aligned_width * aligned_height;
	picture = picture * 3U / 2U;
	picture = (picture + H265_DPB_PICTURE_ALIGN - 1U) & ~((uint64_t)H265_DPB_PICTURE_ALIGN - 1U);

	/* Succeeded: the DPB's bytes. */
	return picture * h265_pictures(width, height, slots);
}

/*
 * Reports the bytes of the decoder context of an H.265 Main session whose
 * largest picture is width by height samples and which has slots DPB
 * slots.
 */
uint64_t
amdrdna_vcn_h265_context_bytes(
	uint32_t width,
	uint32_t height,
	uint32_t slots)
{
	uint64_t bytes;
	uint32_t aligned_width;
	uint32_t aligned_height;

	/* The sides on 16 samples. */
	aligned_width = h265_align(width, H265_SIDE_ALIGN);
	aligned_height = h265_align(height, H265_SIDE_ALIGN);

	/* 16 bytes a block of the grown picture for each picture of the DPB, and 52 KiB; in whole pages. */
	bytes = (uint64_t)((aligned_width + H265_CONTEXT_GROW) / H265_SIDE_ALIGN) * ((aligned_height + H265_CONTEXT_GROW) / H265_SIDE_ALIGN);
	bytes *= (uint64_t)H265_CONTEXT_BLOCK_BYTES * h265_pictures(width, height, slots);
	bytes += H265_CONTEXT_EXTRA_BYTES;
	bytes = (bytes + H265_CONTEXT_PAGE - 1U) & ~((uint64_t)H265_CONTEXT_PAGE - 1U);

	/* Succeeded: the context's bytes. */
	return bytes;
}

/* Writes what a picture's two sets say into the H.265 body: the flags, the formats, the sizes, the counts and the offsets. */
static void
h265_body_sets(
	uint8_t *body,
	const struct amdrdna_vcn_h265_sps *sps,
	const struct amdrdna_vcn_h265_pps *pps)
{
	uint32_t flags;
	bool pcm;

	/* The sequence flags. */
	flags = 0U;
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_SCALING) != 0U)
		flags |= H265_SPS_SCALING;
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_AMP) != 0U)
		flags |= H265_SPS_AMP;
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_SAO) != 0U)
		flags |= H265_SPS_SAO;
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_PCM) != 0U)
		flags |= H265_SPS_PCM;
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_PCM_FILTER_OFF) != 0U)
		flags |= H265_SPS_PCM_FILTER_OFF;
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_LONG_TERM) != 0U)
		flags |= H265_SPS_LONG_TERM;
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_TEMPORAL_MVP) != 0U)
		flags |= H265_SPS_TEMPORAL_MVP;
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_STRONG_INTRA) != 0U)
		flags |= H265_SPS_STRONG_INTRA;
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_SEPARATE_PLANES) != 0U)
		flags |= H265_SPS_SEPARATE_PLANES;
	format_put32(body, H265_SPS_FLAGS_AT, flags);

	/* The picture flags: the first sixteen in the wire's order, then the four the body places on their own. */
	flags = pps->flags & H265_PPS_LOW_FLAGS;
	if ((pps->flags & AMDRDNA_VCN_H265_PPS_DEBLOCK_OVERRIDE) != 0U)
		flags |= H265_PPS_DEBLOCK_OVERRIDE;
	if ((pps->flags & AMDRDNA_VCN_H265_PPS_DEBLOCK_OFF) != 0U)
		flags |= H265_PPS_DEBLOCK_OFF;
	if ((pps->flags & AMDRDNA_VCN_H265_PPS_LIST_CHANGES) != 0U)
		flags |= H265_PPS_LIST_CHANGES;
	if ((pps->flags & AMDRDNA_VCN_H265_PPS_HEADER_EXTENSION) != 0U)
		flags |= H265_PPS_HEADER_EXTENSION;
	format_put32(body, H265_PPS_FLAGS_AT, flags);

	/* The sequence's formats, DPB size and block sizes, a byte each. */
	body[H265_CHROMA_FORMAT_AT] = (uint8_t)sps->chroma_format_idc;
	body[H265_LUMA_DEPTH_AT] = (uint8_t)sps->bit_depth_luma_minus8;
	body[H265_CHROMA_DEPTH_AT] = (uint8_t)sps->bit_depth_chroma_minus8;
	body[H265_POC_BITS_AT] = (uint8_t)sps->log2_max_pic_order_cnt_lsb_minus4;
	body[H265_DPB_SIZE_AT] = (uint8_t)sps->max_dec_pic_buffering_minus1;
	/*
	 * The highest sub-layer the sequence has.  Mesa leaves it 0; the
	 * engine then treats a sub-layer non-reference picture (TRAIL_N,
	 * TSA_N, ...) of every sub-layer as unused for reference, though a
	 * picture of a higher sub-layer may still reference it (H.265 defines
	 * such a picture as one its own sub-layer does not reference); with
	 * the sequence's sps_max_sub_layers_minus1 the engine keeps it.
	 */
	body[H265_HIGHEST_TID_AT] = (uint8_t)sps->max_sub_layers_minus1;
	body[H265_MIN_CB_AT] = (uint8_t)sps->log2_min_luma_coding_block_size_minus3;
	body[H265_DIFF_CB_AT] = (uint8_t)sps->log2_diff_max_min_luma_coding_block_size;
	body[H265_MIN_TB_AT] = (uint8_t)sps->log2_min_luma_transform_block_size_minus2;
	body[H265_DIFF_TB_AT] = (uint8_t)sps->log2_diff_max_min_luma_transform_block_size;
	body[H265_DEPTH_INTER_AT] = (uint8_t)sps->max_transform_hierarchy_depth_inter;
	body[H265_DEPTH_INTRA_AT] = (uint8_t)sps->max_transform_hierarchy_depth_intra;

	/* The PCM sizes, only when the sequence uses PCM. */
	pcm = (sps->flags & AMDRDNA_VCN_H265_SPS_PCM) != 0U;
	if (pcm) {
		body[H265_PCM_LUMA_AT] = (uint8_t)sps->pcm_sample_bit_depth_luma_minus1;
		body[H265_PCM_CHROMA_AT] = (uint8_t)sps->pcm_sample_bit_depth_chroma_minus1;
		body[H265_PCM_MIN_AT] = (uint8_t)sps->log2_min_pcm_luma_coding_block_size_minus3;
		body[H265_PCM_DIFF_AT] = (uint8_t)sps->log2_diff_max_min_pcm_luma_coding_block_size;
	}

	/* The counts: the slice header's extra bits, the sequence's reference sets, the default active references. */
	body[H265_EXTRA_BITS_AT] = (uint8_t)pps->num_extra_slice_header_bits;
	body[H265_SHORT_TERM_SETS_AT] = (uint8_t)sps->num_short_term_ref_pic_sets;
	body[H265_LONG_TERM_SPS_AT] = (uint8_t)sps->num_long_term_ref_pics_sps;
	body[H265_L0_ACTIVE_AT] = (uint8_t)pps->num_ref_idx_l0_default_active_minus1;
	body[H265_L1_ACTIVE_AT] = (uint8_t)pps->num_ref_idx_l1_default_active_minus1;

	/* The picture set's offsets, signed bytes. */
	body[H265_CB_OFFSET_AT] = (uint8_t)pps->pps_cb_qp_offset;
	body[H265_CR_OFFSET_AT] = (uint8_t)pps->pps_cr_qp_offset;
	body[H265_BETA_AT] = (uint8_t)pps->pps_beta_offset_div2;
	body[H265_TC_AT] = (uint8_t)pps->pps_tc_offset_div2;

	/* The quantizer's depth, the tiles when the set has them, the merge level and the initial quantizer. */
	body[H265_QP_DEPTH_AT] = (uint8_t)pps->diff_cu_qp_delta_depth;
	if ((pps->flags & AMDRDNA_VCN_H265_PPS_TILES) != 0U) {
		body[H265_TILE_COLUMNS_AT] = (uint8_t)pps->num_tile_columns_minus1;
		body[H265_TILE_ROWS_AT] = (uint8_t)pps->num_tile_rows_minus1;
	}
	body[H265_MERGE_LEVEL_AT] = (uint8_t)pps->log2_parallel_merge_level_minus2;
	body[H265_INIT_QP_AT] = (uint8_t)pps->init_qp_minus26;
}

/*
 * Writes the picture into the H.265 body: whether the slice header
 * carries its own short-term set (and its size in references and in
 * bits), the picture's tag and order count, each reference's tag and
 * order count (the entries past them naming none), and the three current
 * sets as places in the references.
 */
static void
h265_body_picture(
	uint8_t *body,
	const struct amdrdna_vcn_h265_decode *h265)
{
	uint32_t flags;
	uint32_t index;

	/* A short-term set of the slice header's own is a sequence flag of the body, with its size. */
	if ((h265->picture.flags & AMDRDNA_VCN_H265_PICTURE_SPS_RPS) == 0U) {
		flags = format_get32(body, H265_SPS_FLAGS_AT) | H265_SPS_SLICE_RPS;
		format_put32(body, H265_SPS_FLAGS_AT, flags);
	}
	format_put32(body, H265_RPS_BITS_AT, h265->picture.num_bits_for_st_ref_pic_set_in_slice);
	body[H265_DELTA_POCS_AT] = (uint8_t)h265->picture.num_delta_pocs_of_ref_rps_idx;

	/* The picture's tag and order count. */
	body[H265_TAG_AT] = (uint8_t)h265->tag;
	format_put32(body, H265_ORDER_AT, (uint32_t)h265->picture.poc);

	/* No entry names a reference until one does. */
	for (index = 0U; index < AMDRDNA_VCN_H265_SLOTS; index++)
		body[H265_TAGS_AT + index] = H265_NO_TAG;

	/* Each reference: its tag and its order count. */
	for (index = 0U; index < h265->reference_count; index++) {
		body[H265_TAGS_AT + index] = (uint8_t)h265->references[index].tag;
		format_put32(body, H265_ORDERS_AT + index * 4U, (uint32_t)h265->references[index].poc);
	}

	/* The three current sets, as places in the references. */
	for (index = 0U; index < AMDRDNA_VCN_H265_SET_ENTRIES; index++) {
		body[H265_BEFORE_AT + index] = h265->before[index];
		body[H265_AFTER_AT + index] = h265->after[index];
		body[H265_LONG_TERM_AT + index] = h265->long_term[index];
	}
}

/*
 * Works out the sizes, minus 1, of parts tiles of one direction across
 * total coding tree blocks: the standard's even split, or the given
 * sizes with the last made up to the total.  *fits is clear when given
 * sizes leave nothing for the last part.
 */
static void
h265_spacing(
	uint32_t total,
	uint32_t parts,
	bool uniform,
	const uint32_t *given,
	uint32_t *minus1,
	bool *fits)
{
	uint32_t index;
	uint32_t used;
	uint32_t size;

	/* The even split: each part the difference of two rounded-down shares. */
	*fits = true;
	if (uniform) {
		for (index = 0U; index < parts; index++)
			minus1[index] = ((index + 1U) * total) / parts - (index * total) / parts - 1U;
		return;
	}

	/* The given sizes, each at most what is left for itself and the parts after it. */
	used = 0U;
	for (index = 0U; index + 1U < parts; index++) {
		size = given[index] + 1U;
		if (size >= total - used) {
			*fits = false;
			return;
		}
		used += size;
		minus1[index] = given[index];
	}

	/* The last part takes the rest. */
	minus1[parts - 1U] = total - used - 1U;
}

/* Copies count bytes of one scaling list into the table at a byte offset. */
static void
h265_list_copy(
	uint8_t *table,
	size_t at,
	const uint8_t *source,
	uint32_t count)
{
	uint32_t index;

	/* Each coefficient as given. */
	for (index = 0U; index < count; index++)
		table[at + index] = source[index];
}

/*
 * Reports how many pictures the hidden DPB of an H.265 session holds: its
 * slots and one more, and at least 17, or at least 8 for a largest
 * picture of 4096 by 2000 samples or more.
 */
static uint32_t
h265_pictures(
	uint32_t width,
	uint32_t height,
	uint32_t slots)
{
	uint32_t pictures;
	uint32_t least;

	/* The fewest for the picture's size. */
	least = H265_DPB_PICTURES_MIN;
	if ((uint64_t)width * height >= (uint64_t)H265_LARGE_WIDTH * H265_LARGE_HEIGHT)
		least = H265_DPB_PICTURES_LARGE;

	/* The slots and one more, at least the fewest. */
	pictures = slots + 1U;
	if (pictures < least)
		pictures = least;

	/* Succeeded: the count. */
	return pictures;
}

/* Rounds a value up to a boundary that is a power of two. */
static uint32_t
h265_align(
	uint32_t value,
	uint32_t boundary)
{
	/* Succeeded: the rounded value. */
	return (value + boundary - 1U) & ~(boundary - 1U);
}
