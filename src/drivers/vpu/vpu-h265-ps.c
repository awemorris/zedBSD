/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.265 video, sequence and picture parameter sets written back from
 * their StdVideo fields (see vpu-codec.h), for a decoder core that parses
 * them from the bitstream itself.
 *
 * What the decoding of a picture reads is written as given; what it does
 * not read is left out or written as its absence: the VPS has no layer
 * sets beyond the base, no timing and no HRD; the SPS has no VUI; the
 * extensions other than the range extension are absent.  A short-term
 * set is always written in its explicit form (the derived values a
 * predicted set stands for), which a slice header's own predicted set
 * reads the same way.
 *
 * Procedure referenced: ITU-T H.265 (V11) 7.3.1.2 (the NAL unit header),
 * 7.3.2.1 (the video parameter set syntax), 7.3.2.2.1 and 7.3.2.2.2 (the
 * sequence parameter set syntax and its range extension), 7.3.2.3.1 and
 * 7.3.2.3.2 (the picture parameter set syntax and its range extension),
 * 7.3.3 (profile_tier_level), 7.3.4 (scaling_list_data), 7.3.7
 * (st_ref_pic_set), A.4 Table A.8 (general_level_idc of each level).
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/vpu/vpu-bits.h"
#include "drivers/vpu/vpu-codec.h"

/*
 * The most RBSP bytes a set takes here, on the stack: real sets take a
 * few hundred; one past it (an SPS of 64 sets of 16 entries with large
 * differences and every list) is refused with ENOSPC.
 */
#define RBSP_MAX			4096U

/* The NAL unit headers: forbidden bit 0, the type, nuh_layer_id 0, nuh_temporal_id_plus1 1. */
#define HEADER_VPS			((32U << 9) | 1U)
#define HEADER_SPS			((33U << 9) | 1U)
#define HEADER_PPS			((34U << 9) | 1U)

/* The id ranges. */
#define VPS_IDS				16U
#define SPS_IDS				16U
#define PPS_IDS				64U

/* The most StdVideoH265LevelIdc enumerant (6.2). */
#define LEVEL_LAST			12U

/* The Main profile, whose streams are also Main 10 streams (A.3.2). */
#define PROFILE_MAIN			1U
#define PROFILE_MAIN_10			2U

static int check_ptl(const struct vpu_h265_ptl *ptl);
static int check_ordering(const struct vpu_h265_ordering *ordering, uint32_t max_sub_layers_minus1);
static int check_sps(const struct vpu_h265_sps *sps);
static int check_pps(const struct vpu_h265_pps *pps);
static void write_ptl(struct vpu_bits *bits, const struct vpu_h265_ptl *ptl, uint32_t max_sub_layers_minus1);
static void write_ordering(struct vpu_bits *bits, const struct vpu_h265_ordering *ordering, int every, uint32_t max_sub_layers_minus1);
static void write_scaling(struct vpu_bits *bits, const struct vpu_h265_scaling *scaling);
static void write_list(struct vpu_bits *bits, const uint8_t *list, uint32_t count, int dc, uint32_t dc_value);
static void write_st_rps(struct vpu_bits *bits, const struct vpu_h265_st_rps *rps, uint32_t index);
static int finish(struct vpu_bits *bits, uint32_t header, uint8_t *output, size_t capacity, size_t *written);

/*
 * Writes a VPS NAL unit (7.3.2.1): the base layer only, its sub-layers'
 * ordering information, one layer set, no timing, no extension.
 */
int
vpu_h265_write_vps(
	const struct vpu_h265_vps *vps,
	uint8_t *output,
	size_t capacity,
	size_t *written)
{
	uint8_t rbsp[RBSP_MAX];
	struct vpu_bits bits;
	int every;
	int error;

	/* Refuses fields H.265 cannot code. */
	if (vps->id >= VPS_IDS || vps->max_sub_layers_minus1 >= VPU_H265_SUB_LAYERS || !vps->has_ptl || !vps->has_ordering)
		return EINVAL;
	error = check_ptl(&vps->ptl);
	if (error == 0)
		error = check_ordering(&vps->ordering, vps->max_sub_layers_minus1);
	if (error != 0)
		return error;

	/* The id, the base layer internal and available, one layer, the sub-layers, the reserved 16 bits. */
	vpu_bits_start(&bits, rbsp, sizeof(rbsp));
	vpu_bits_put(&bits, vps->id, 4);
	vpu_bits_put(&bits, 1, 1);
	vpu_bits_put(&bits, 1, 1);
	vpu_bits_put(&bits, 0, 6);
	vpu_bits_put(&bits, vps->max_sub_layers_minus1, 3);
	vpu_bits_flag(&bits, vps->flags, VPU_H265_VPS_TEMPORAL_NESTING);
	vpu_bits_put(&bits, 0xffffU, 16);

	/* The profile, tier and level, and the ordering information. */
	write_ptl(&bits, &vps->ptl, vps->max_sub_layers_minus1);
	every = (vps->flags & VPU_H265_VPS_ORDERING_INFO) != 0;
	vpu_bits_put(&bits, (uint32_t)every, 1);
	write_ordering(&bits, &vps->ordering, every, vps->max_sub_layers_minus1);

	/* vps_max_layer_id 0, one layer set, no timing, no extension. */
	vpu_bits_put(&bits, 0, 6);
	vpu_bits_ue(&bits, 0);
	vpu_bits_put(&bits, 0, 1);
	vpu_bits_put(&bits, 0, 1);

	/* The trailing bits and the unit. */
	return finish(&bits, HEADER_VPS, output, capacity, written);
}

/*
 * Writes an SPS NAL unit (7.3.2.2.1), field by field in the syntax's
 * order; no VUI, and of the extensions only the range extension.
 */
int
vpu_h265_write_sps(
	const struct vpu_h265_sps *sps,
	uint8_t *output,
	size_t capacity,
	size_t *written)
{
	uint8_t rbsp[RBSP_MAX];
	struct vpu_bits bits;
	uint32_t index;
	int every;
	int error;

	/* Refuses fields H.265 cannot code. */
	error = check_sps(sps);
	if (error != 0)
		return error;

	/* The VPS, the sub-layers, the profile, the id and the chroma format. */
	vpu_bits_start(&bits, rbsp, sizeof(rbsp));
	vpu_bits_put(&bits, sps->vps_id, 4);
	vpu_bits_put(&bits, sps->max_sub_layers_minus1, 3);
	vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_TEMPORAL_NESTING);
	write_ptl(&bits, &sps->ptl, sps->max_sub_layers_minus1);
	vpu_bits_ue(&bits, sps->id);
	vpu_bits_ue(&bits, sps->chroma_format_idc);
	if (sps->chroma_format_idc == 3U)
		vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_SEPARATE_PLANES);

	/* The size and the conformance window. */
	vpu_bits_ue(&bits, sps->width);
	vpu_bits_ue(&bits, sps->height);
	vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_CONFORMANCE_WINDOW);
	if ((sps->flags & VPU_H265_SPS_CONFORMANCE_WINDOW) != 0) {
		for (index = 0; index < 4U; index++)
			vpu_bits_ue(&bits, sps->window[index]);
	}

	/* The bit depths, the order count's bits and the ordering information. */
	vpu_bits_ue(&bits, sps->bit_depth_luma_minus8);
	vpu_bits_ue(&bits, sps->bit_depth_chroma_minus8);
	vpu_bits_ue(&bits, sps->log2_max_poc_lsb_minus4);
	every = (sps->flags & VPU_H265_SPS_ORDERING_INFO) != 0;
	vpu_bits_put(&bits, (uint32_t)every, 1);
	write_ordering(&bits, &sps->ordering, every, sps->max_sub_layers_minus1);

	/* The block sizes and the transform depths. */
	vpu_bits_ue(&bits, sps->log2_min_cb_minus3);
	vpu_bits_ue(&bits, sps->log2_diff_cb);
	vpu_bits_ue(&bits, sps->log2_min_tb_minus2);
	vpu_bits_ue(&bits, sps->log2_diff_tb);
	vpu_bits_ue(&bits, sps->depth_inter);
	vpu_bits_ue(&bits, sps->depth_intra);

	/* The scaling lists, coded when the SPS has its own. */
	vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_SCALING);
	if ((sps->flags & VPU_H265_SPS_SCALING) != 0) {
		vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_SCALING_DATA);
		if ((sps->flags & VPU_H265_SPS_SCALING_DATA) != 0)
			write_scaling(&bits, &sps->scaling);
	}

	/* The motion partitions, the offsets, the PCM coding. */
	vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_AMP);
	vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_SAO);
	vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_PCM);
	if ((sps->flags & VPU_H265_SPS_PCM) != 0) {
		vpu_bits_put(&bits, sps->pcm_luma_minus1, 4);
		vpu_bits_put(&bits, sps->pcm_chroma_minus1, 4);
		vpu_bits_ue(&bits, sps->log2_min_pcm_minus3);
		vpu_bits_ue(&bits, sps->log2_diff_pcm);
		vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_PCM_LOOP_FILTER_OFF);
	}

	/* The short-term sets, each explicit. */
	vpu_bits_ue(&bits, sps->st_count);
	for (index = 0; index < sps->st_count; index++)
		write_st_rps(&bits, &sps->st[index], index);

	/* The long-term pictures, each order count of the order count's bits. */
	vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_LONG_TERM);
	if ((sps->flags & VPU_H265_SPS_LONG_TERM) != 0) {
		vpu_bits_ue(&bits, sps->lt_count);
		for (index = 0; index < sps->lt_count; index++) {
			vpu_bits_put(&bits, sps->lt_lsb[index], sps->log2_max_poc_lsb_minus4 + 4U);
			vpu_bits_put(&bits, (sps->lt_used >> index) & 1U, 1);
		}
	}

	/* The motion vector prediction, the smoothing, no VUI. */
	vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_TEMPORAL_MVP);
	vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_STRONG_SMOOTHING);
	vpu_bits_put(&bits, 0, 1);

	/* The range extension, the one extension written: its flag, three absent ones, four zero bits, its nine flags. */
	vpu_bits_flag(&bits, sps->flags, VPU_H265_SPS_RANGE_EXTENSION);
	if ((sps->flags & VPU_H265_SPS_RANGE_EXTENSION) != 0) {
		vpu_bits_put(&bits, 1, 1);
		vpu_bits_put(&bits, 0, 3);
		vpu_bits_put(&bits, 0, 4);
		for (index = 0; index < VPU_H265_SPS_RANGE_FLAGS; index++)
			vpu_bits_put(&bits, (sps->flags >> (VPU_H265_SPS_RANGE_FIRST + index)) & 1U, 1);
	}

	/* The trailing bits and the unit. */
	return finish(&bits, HEADER_SPS, output, capacity, written);
}

/*
 * Writes a PPS NAL unit (7.3.2.3.1), field by field in the syntax's
 * order; of the extensions only the range extension.
 */
int
vpu_h265_write_pps(
	const struct vpu_h265_pps *pps,
	uint8_t *output,
	size_t capacity,
	size_t *written)
{
	uint8_t rbsp[RBSP_MAX];
	struct vpu_bits bits;
	uint32_t index;
	int error;

	/* Refuses fields H.265 cannot code. */
	error = check_pps(pps);
	if (error != 0)
		return error;

	/* The ids and the slice header's fields. */
	vpu_bits_start(&bits, rbsp, sizeof(rbsp));
	vpu_bits_ue(&bits, pps->id);
	vpu_bits_ue(&bits, pps->sps_id);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_DEPENDENT_SLICES);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_OUTPUT_FLAG);
	vpu_bits_put(&bits, pps->extra_bits, 3);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_SIGN_HIDING);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_CABAC_INIT);
	vpu_bits_ue(&bits, pps->l0_minus1);
	vpu_bits_ue(&bits, pps->l1_minus1);

	/* The quantizers. */
	vpu_bits_se(&bits, pps->init_qp_minus26);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_CONSTRAINED_INTRA);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_TRANSFORM_SKIP);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_CU_QP_DELTA);
	if ((pps->flags & VPU_H265_PPS_CU_QP_DELTA) != 0)
		vpu_bits_ue(&bits, pps->qp_depth);
	vpu_bits_se(&bits, pps->cb_offset);
	vpu_bits_se(&bits, pps->cr_offset);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_CHROMA_QP_OFFSETS);

	/* The prediction weights, the bypass, the tiles and the wavefronts. */
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_WEIGHTED_PRED);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_WEIGHTED_BIPRED);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_TRANSQUANT_BYPASS);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_TILES);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_ENTROPY_SYNC);
	if ((pps->flags & VPU_H265_PPS_TILES) != 0) {
		vpu_bits_ue(&bits, pps->tile_columns_minus1);
		vpu_bits_ue(&bits, pps->tile_rows_minus1);
		vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_UNIFORM_SPACING);
		if ((pps->flags & VPU_H265_PPS_UNIFORM_SPACING) == 0) {
			for (index = 0; index < pps->tile_columns_minus1; index++)
				vpu_bits_ue(&bits, pps->column_minus1[index]);
			for (index = 0; index < pps->tile_rows_minus1; index++)
				vpu_bits_ue(&bits, pps->row_minus1[index]);
		}

		/* The filter across the tiles. */
		vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_FILTER_TILES);
	}

	/* The filter across slices and the deblocking control. */
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_FILTER_SLICES);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_DEBLOCKING_CONTROL);
	if ((pps->flags & VPU_H265_PPS_DEBLOCKING_CONTROL) != 0) {
		vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_DEBLOCKING_OVERRIDE);
		vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_DEBLOCKING_OFF);
		if ((pps->flags & VPU_H265_PPS_DEBLOCKING_OFF) == 0) {
			vpu_bits_se(&bits, pps->beta_div2);
			vpu_bits_se(&bits, pps->tc_div2);
		}
	}

	/* The picture's own scaling lists. */
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_SCALING_DATA);
	if ((pps->flags & VPU_H265_PPS_SCALING_DATA) != 0)
		write_scaling(&bits, &pps->scaling);

	/* The list modification, the merge level, the header extension. */
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_LISTS_MODIFICATION);
	vpu_bits_ue(&bits, pps->merge_level_minus2);
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_HEADER_EXTENSION);

	/* The range extension, the one extension written: its flag, three absent ones, four zero bits, its fields. */
	vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_RANGE_EXTENSION);
	if ((pps->flags & VPU_H265_PPS_RANGE_EXTENSION) != 0) {
		vpu_bits_put(&bits, 1, 1);
		vpu_bits_put(&bits, 0, 3);
		vpu_bits_put(&bits, 0, 4);
		if ((pps->flags & VPU_H265_PPS_TRANSFORM_SKIP) != 0)
			vpu_bits_ue(&bits, pps->skip_size_minus2);
		vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_CROSS_COMPONENT);
		vpu_bits_flag(&bits, pps->flags, VPU_H265_PPS_CHROMA_QP_LIST);
		if ((pps->flags & VPU_H265_PPS_CHROMA_QP_LIST) != 0) {
			vpu_bits_ue(&bits, pps->chroma_qp_depth);
			vpu_bits_ue(&bits, pps->chroma_qp_list_minus1);
			for (index = 0; index <= pps->chroma_qp_list_minus1; index++) {
				vpu_bits_se(&bits, pps->cb_list[index]);
				vpu_bits_se(&bits, pps->cr_list[index]);
			}
		}
		vpu_bits_ue(&bits, pps->sao_luma);
		vpu_bits_ue(&bits, pps->sao_chroma);
	}

	/* The trailing bits and the unit. */
	return finish(&bits, HEADER_PPS, output, capacity, written);
}

/* Checks a profile, tier and level: a level StdVideo names; EINVAL otherwise. */
static int
check_ptl(
	const struct vpu_h265_ptl *ptl)
{
	/* The profile in its five bits, the level among the enumerants. */
	if (ptl->profile_idc > 31U || ptl->level > LEVEL_LAST)
		return EINVAL;

	/* Succeeded: it can be written. */
	return 0;
}

/* Checks the ordering information of the sub-layers written; EINVAL for a value outside 7.4.3.2.1's ranges. */
static int
check_ordering(
	const struct vpu_h265_ordering *ordering,
	uint32_t max_sub_layers_minus1)
{
	uint32_t index;

	/* Each sub-layer's buffering (MaxDpbSize at most 16) and reordering (at most the buffering). */
	for (index = 0; index <= max_sub_layers_minus1; index++) {
		if (ordering->buffering_minus1[index] > 15U || ordering->reorder[index] > ordering->buffering_minus1[index])
			return EINVAL;
		if (ordering->latency_plus1[index] == UINT32_MAX)
			return EINVAL;
	}

	/* Succeeded: it can be written. */
	return 0;
}

/* Checks an SPS's fields against 7.4.3.2.1's ranges and what the writer codes; EINVAL for one outside. */
static int
check_sps(
	const struct vpu_h265_sps *sps)
{
	uint32_t index;
	uint32_t entries;
	int error;

	/* The ids, the sub-layers, the chroma format, the bit depths, the parts every SPS has. */
	if (sps->vps_id >= VPS_IDS || sps->id >= SPS_IDS || sps->max_sub_layers_minus1 >= VPU_H265_SUB_LAYERS)
		return EINVAL;
	if (sps->chroma_format_idc > 3U || sps->bit_depth_luma_minus8 > 8U || sps->bit_depth_chroma_minus8 > 8U)
		return EINVAL;
	if (!sps->has_ptl || !sps->has_ordering)
		return EINVAL;
	error = check_ptl(&sps->ptl);
	if (error == 0)
		error = check_ordering(&sps->ordering, sps->max_sub_layers_minus1);
	if (error != 0)
		return error;

	/* The order count's bits, the sizes in Exp-Golomb's reach. */
	if (sps->log2_max_poc_lsb_minus4 > 12U || sps->width == 0 || sps->height == 0 || sps->width > 0xffffU || sps->height > 0xffffU)
		return EINVAL;
	for (index = 0; index < 4U; index++) {
		if (sps->window[index] > 0xffffU)
			return EINVAL;
	}

	/* The block sizes (CtbLog2SizeY at most 6) and the PCM's bits. */
	if (sps->log2_min_cb_minus3 > 3U || sps->log2_diff_cb > 3U || sps->log2_min_tb_minus2 > 3U || sps->log2_diff_tb > 3U)
		return EINVAL;
	if (sps->depth_inter > 4U || sps->depth_intra > 4U)
		return EINVAL;
	if (sps->pcm_luma_minus1 > 15U || sps->pcm_chroma_minus1 > 15U || sps->log2_min_pcm_minus3 > 2U || sps->log2_diff_pcm > 2U)
		return EINVAL;

	/* Scaling data needs its lists; the screen content tools are not written. */
	if ((sps->flags & VPU_H265_SPS_SCALING_DATA) != 0 && (sps->flags & VPU_H265_SPS_SCALING) != 0 && !sps->has_scaling)
		return EINVAL;
	if ((sps->flags & VPU_H265_SPS_SCC_FLAGS) != 0)
		return EINVAL;

	/* The short-term sets, each of at most 16 entries, and the long-term pictures. */
	if (sps->st_count > VPU_H265_ST_SETS || sps->lt_count > VPU_H265_LT_PICTURES)
		return EINVAL;
	for (index = 0; index < sps->st_count; index++) {
		entries = sps->st[index].negative + sps->st[index].positive;
		if (sps->st[index].negative > VPU_H265_ST_ENTRIES || sps->st[index].positive > VPU_H265_ST_ENTRIES || entries > VPU_H265_ST_ENTRIES)
			return EINVAL;
	}
	for (index = 0; index < sps->lt_count; index++) {
		if ((sps->lt_lsb[index] >> (sps->log2_max_poc_lsb_minus4 + 4U)) != 0)
			return EINVAL;
	}

	/* Succeeded: the set can be written. */
	return 0;
}

/* Checks a PPS's fields against 7.4.3.3's ranges and what the writer codes; EINVAL for one outside. */
static int
check_pps(
	const struct vpu_h265_pps *pps)
{
	/* The ids, the extra bits, the default references. */
	if (pps->id >= PPS_IDS || pps->sps_id >= SPS_IDS || pps->extra_bits > 7U)
		return EINVAL;
	if (pps->l0_minus1 > 14U || pps->l1_minus1 > 14U)
		return EINVAL;

	/* The quantizers and the deblocking offsets. */
	if (pps->init_qp_minus26 < -74 || pps->init_qp_minus26 > 25 || pps->qp_depth > 6U)
		return EINVAL;
	if (pps->cb_offset < -12 || pps->cb_offset > 12 || pps->cr_offset < -12 || pps->cr_offset > 12)
		return EINVAL;
	if (pps->beta_div2 < -6 || pps->beta_div2 > 6 || pps->tc_div2 < -6 || pps->tc_div2 > 6)
		return EINVAL;

	/* The tiles within the arrays, the merge level. */
	if (pps->tile_columns_minus1 > VPU_H265_TILE_COLUMNS || pps->tile_rows_minus1 > VPU_H265_TILE_ROWS || pps->merge_level_minus2 > 4U)
		return EINVAL;

	/* The range extension's fields, the screen content tools not written, scaling data with its lists. */
	if (pps->skip_size_minus2 > 3U || pps->chroma_qp_depth > 6U || pps->chroma_qp_list_minus1 >= VPU_H265_CHROMA_QP_LIST)
		return EINVAL;
	if (pps->sao_luma > 6U || pps->sao_chroma > 6U)
		return EINVAL;
	if ((pps->flags & VPU_H265_PPS_SCC_FLAGS) != 0)
		return EINVAL;
	if ((pps->flags & VPU_H265_PPS_SCALING_DATA) != 0 && !pps->has_scaling)
		return EINVAL;

	/* Succeeded: the set can be written. */
	return 0;
}

/*
 * Writes profile_tier_level( 1, maxNumSubLayersMinus1 ) (7.3.3): the
 * general profile with its own compatibility flag (and Main 10's for
 * Main), the source and constraint flags, the reserved bits zero, the
 * level; no sub-layer profile or level.
 */
static void
write_ptl(
	struct vpu_bits *bits,
	const struct vpu_h265_ptl *ptl,
	uint32_t max_sub_layers_minus1)
{
	/* general_level_idc of each StdVideoH265LevelIdc: 30 times the level. */
	static const uint8_t levels[LEVEL_LAST + 1U] = {
		30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186
	};
	uint32_t compatibility;
	uint32_t index;

	/* The profile space 0, the tier, the profile. */
	vpu_bits_put(bits, 0, 2);
	vpu_bits_flag(bits, ptl->flags, VPU_H265_PTL_TIER);
	vpu_bits_put(bits, ptl->profile_idc, 5);

	/* The compatibility flags, flag j first for j from 0. */
	compatibility = 1U << ptl->profile_idc;
	if (ptl->profile_idc == PROFILE_MAIN)
		compatibility |= 1U << PROFILE_MAIN_10;
	for (index = 0; index < 32U; index++)
		vpu_bits_put(bits, (compatibility >> index) & 1U, 1);

	/* The source and constraint flags, the 43 reserved bits and the one after them. */
	vpu_bits_flag(bits, ptl->flags, VPU_H265_PTL_PROGRESSIVE);
	vpu_bits_flag(bits, ptl->flags, VPU_H265_PTL_INTERLACED);
	vpu_bits_flag(bits, ptl->flags, VPU_H265_PTL_NON_PACKED);
	vpu_bits_flag(bits, ptl->flags, VPU_H265_PTL_FRAME_ONLY);
	vpu_bits_put(bits, 0, 32);
	vpu_bits_put(bits, 0, 11);
	vpu_bits_put(bits, 0, 1);

	/* The level. */
	vpu_bits_put(bits, levels[ptl->level], 8);

	/* No sub-layer profile or level, then the reserved two bits up to eight entries. */
	for (index = 0; index < max_sub_layers_minus1; index++)
		vpu_bits_put(bits, 0, 2);
	if (max_sub_layers_minus1 > 0) {
		for (index = max_sub_layers_minus1; index < 8U; index++)
			vpu_bits_put(bits, 0, 2);
	}
}

/*
 * Writes the ordering information of a VPS or an SPS: each sub-layer's
 * when every is set, else the highest one's.
 */
static void
write_ordering(
	struct vpu_bits *bits,
	const struct vpu_h265_ordering *ordering,
	int every,
	uint32_t max_sub_layers_minus1)
{
	uint32_t index;

	/* From sub-layer 0, or only the highest. */
	index = max_sub_layers_minus1;
	if (every)
		index = 0;
	for (; index <= max_sub_layers_minus1; index++) {
		vpu_bits_ue(bits, ordering->buffering_minus1[index]);
		vpu_bits_ue(bits, ordering->reorder[index]);
		vpu_bits_ue(bits, ordering->latency_plus1[index]);
	}
}

/*
 * Writes scaling_list_data() (7.3.4) with every list explicit
 * (scaling_list_pred_mode_flag 1): the 4x4, 8x8, 16x16 and 32x32 lists in
 * the syntax's order, the 32x32 ones for matrixId 0 and 3.
 */
static void
write_scaling(
	struct vpu_bits *bits,
	const struct vpu_h265_scaling *scaling)
{
	uint32_t matrix;

	/* sizeId 0 to 2, six lists each; sizeId 3, two. */
	for (matrix = 0; matrix < 6U; matrix++)
		write_list(bits, scaling->list4[matrix], 16U, 0, 0);
	for (matrix = 0; matrix < 6U; matrix++)
		write_list(bits, scaling->list8[matrix], 64U, 0, 0);
	for (matrix = 0; matrix < 6U; matrix++)
		write_list(bits, scaling->list16[matrix], 64U, 1, scaling->dc16[matrix]);
	for (matrix = 0; matrix < 2U; matrix++)
		write_list(bits, scaling->list32[matrix], 64U, 1, scaling->dc32[matrix]);
}

/*
 * Writes one explicit list: the mode flag, the DC coefficient (16x16 and
 * 32x32 lists), then each coefficient as its difference from the one
 * before, wrapped to -128..127.
 */
static void
write_list(
	struct vpu_bits *bits,
	const uint8_t *list,
	uint32_t count,
	int dc,
	uint32_t dc_value)
{
	uint32_t index;
	int32_t next;
	int32_t delta;

	/* Explicit, starting from 8 or from the DC coefficient. */
	vpu_bits_put(bits, 1, 1);
	next = 8;
	if (dc) {
		vpu_bits_se(bits, (int32_t)dc_value - 8);
		next = (int32_t)dc_value;
	}

	/* Each coefficient. */
	for (index = 0; index < count; index++) {
		delta = ((int32_t)list[index] - next + 256) % 256;
		if (delta > 127)
			delta -= 256;
		vpu_bits_se(bits, delta);
		next = list[index];
	}
}

/*
 * Writes st_ref_pic_set( stRpsIdx ) (7.3.7) in the explicit form: the
 * prediction flag 0 (after the first set), the counts, each difference
 * with its used flag.
 */
static void
write_st_rps(
	struct vpu_bits *bits,
	const struct vpu_h265_st_rps *rps,
	uint32_t index)
{
	uint32_t entry;

	/* Not predicted; the first set has no flag. */
	if (index != 0)
		vpu_bits_put(bits, 0, 1);

	/* The counts, then the pictures before and after. */
	vpu_bits_ue(bits, rps->negative);
	vpu_bits_ue(bits, rps->positive);
	for (entry = 0; entry < rps->negative; entry++) {
		vpu_bits_ue(bits, rps->delta_s0_minus1[entry]);
		vpu_bits_put(bits, (rps->used_s0 >> entry) & 1U, 1);
	}
	for (entry = 0; entry < rps->positive; entry++) {
		vpu_bits_ue(bits, rps->delta_s1_minus1[entry]);
		vpu_bits_put(bits, (rps->used_s1 >> entry) & 1U, 1);
	}
}

/* Ends the RBSP and writes the NAL unit; ENOSPC when either does not fit. */
static int
finish(
	struct vpu_bits *bits,
	uint32_t header,
	uint8_t *output,
	size_t capacity,
	size_t *written)
{
	int error;

	/* The trailing bits. */
	vpu_bits_trailing(bits);
	if (bits->error != 0)
		return ENOSPC;

	/* The start code, the two-byte header and the escaped body. */
	error = vpu_bits_nal(header, 2U, bits->data, bits->position / 8U, output, capacity, written);
	if (error != 0)
		return error;

	/* Succeeded: output holds the unit. */
	return 0;
}
