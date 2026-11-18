/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video, sequence and picture parameter sets of H.265 and their store
 * (7.3.2, 7.3.3, 7.4.3, 7.4.4).
 */

#include "h265sw-ps.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The bits of profile_tier_level() that follow the four source flags of one profile part: 43 constraint and reserved bits and one more (7.3.3). */
#define PS_PROFILE_SKIPPED_BITS 44U

/* The bits of one sub-layer's profile part (7.3.3: 2 + 1 + 5 + 32 + 4 + 43 + 1) and level (u(8)). */
#define PS_SUB_PROFILE_BITS 88U
#define PS_SUB_LEVEL_BITS 8U

/* The sub-layer entries profile_tier_level() codes reserved bits for (7.3.3: i up to 7). */
#define PS_PTL_RESERVED_ENTRIES 8U

/* The largest values of the ids (7.4.3.2.1, 7.4.3.3.1). */
#define PS_SPS_ID_MAX 15U
#define PS_PPS_ID_MAX 63U

/* The largest vps_num_layer_sets_minus1 (7.4.3.1) and the layer sets the duplicate check of hrd_layer_set_idx tracks, in 32-bit words. */
#define PS_VPS_LAYER_SETS_MAX 1023U
#define PS_VPS_LAYER_SET_WORDS 32U

/* The scratch HRD parameters a VPS read alternates between for the ones it drops. */
#define PS_VPS_SCRATCH_HRDS 2U

/* The largest bit depth (7.4.3.2.1: bit_depth_luma_minus8 0..8) and the largest log2_max_pic_order_cnt_lsb_minus4. */
#define PS_BIT_DEPTH_MINUS8_MAX 8U
#define PS_POC_LSB_MINUS4_MAX 12U

/* The largest log2 of a size of the tools that stop at 32 (7.4.3.2.1: Min( CtbLog2SizeY, 5 )). */
#define PS_LOG2_TOOL_MAX 5U

/* The number of flags of sps_range_extension() (7.3.2.2.2). */
#define PS_SPS_RANGE_FLAGS 9U

/* The profile_idc values whose SPS may lose a misplaced VUI: Main, Main 10 and Main Still Picture (A.3.2 to A.3.4). */
#define PS_PROFILE_MAIN_FIRST 1U
#define PS_PROFILE_MAIN_LAST 3U

/* The range of init_qp_minus26 the PPS alone bounds: -( 26 + 48 ) to 25 (7.4.3.3.1, QpBdOffsetY up to 48), and of init_qp. */
#define PS_INIT_QP_MINUS26_MIN (-74)
#define PS_INIT_QP_MINUS26_MAX 25
#define PS_INIT_QP_MAX 51

/* The range of the chroma QP offsets (7.4.3.3.1, 7.4.3.3.2) and of the deblocking offsets. */
#define PS_QP_OFFSET_MAX 12
#define PS_DEBLOCK_OFFSET_MAX 6

/* The largest num_ref_idx_lX_default_active_minus1 (7.4.3.3.1). */
#define PS_NUM_REF_IDX_MINUS1_MAX 14U

/* The widest tile column and tallest tile row, in CTBs: the largest picture side over the smallest CTB size (16). */
#define PS_MIN_CTB_SIZE 16U
#define PS_TILE_SIZE_MAX (H265SW_MAX_SIDE / PS_MIN_CTB_SIZE)

/* The range extension limits of the PPS that need no SPS (7.4.3.3.2). */
#define PS_PPS_TS_SIZE_MINUS2_MAX 3U
#define PS_PPS_CHROMA_QP_LIST_MAX 5U
#define PS_PPS_SAO_SCALE_MAX 6U

/* The largest values the PPS bounds with the widest SPS (7.4.3.3.1: CtbLog2SizeY - MinCbLog2SizeY at most 6 - 3, CtbLog2SizeY - 2 at most 4). */
#define PS_PPS_DEPTH_MAX 3U
#define PS_PPS_MERGE_LEVEL_MINUS2_MAX 4U

/* Static function declarations. */
static unsigned ps_min(unsigned first, unsigned second);
static void ps_skip_to_stop(struct vcodec_bitreader *bits);
static int ps_read_sub_layer_ordering(struct vcodec_bitreader *bits, unsigned max_sub_layers_minus1, int *ordering_info_present, unsigned *max_dec_pic_buffering_minus1, unsigned *max_num_reorder_pics, uint32_t *max_latency_increase_plus1);
static int ps_parse_vps(struct h265sw_vps *work, struct h265sw_hrd *scratch, struct vcodec_bitreader *bits);
static int ps_read_vps_timing(struct h265sw_vps *work, struct h265sw_hrd *scratch, struct vcodec_bitreader *bits);
static int ps_parse_sps(struct h265sw_sps *work, struct vcodec_bitreader *bits);
static int ps_sps_format(struct h265sw_sps *work, struct vcodec_bitreader *bits);
static int ps_sps_blocks(struct h265sw_sps *work, struct vcodec_bitreader *bits);
static int ps_sps_picture_size(struct h265sw_sps *work);
static int ps_sps_pcm(struct h265sw_sps *work, struct vcodec_bitreader *bits);
static int ps_sps_references(struct h265sw_sps *work, struct vcodec_bitreader *bits);
static int ps_sps_extensions(struct h265sw_sps *work, struct vcodec_bitreader *bits);
static int ps_parse_pps(struct h265sw_pps *work, struct vcodec_bitreader *bits);
static int ps_pps_tiles(struct h265sw_pps *work, struct vcodec_bitreader *bits);
static int ps_pps_extensions(struct h265sw_pps *work, struct vcodec_bitreader *bits);

/*
 * Empties a store (every flag and counter 0).
 *
 * The whole structure is cleared, so the bytes between members are 0 and
 * the comparison of a set with the one stored before it is a plain memcmp.
 */
void
h265sw_ps_init(
	struct h265sw_ps_store *store)
{
	/* Clears every flag, counter and set. */
	memset(store, 0, sizeof(*store));
}

/*
 * Reads a profile_tier_level( 1, max_sub_layers_minus1 ) (7.3.3) into
 * *ptl.
 *
 * The general profile, tier and level are kept; the constraint bits and
 * every sub-layer part are read over.
 */
int
h265sw_ps_read_ptl(
	struct h265sw_ptl *ptl,
	struct vcodec_bitreader *bits,
	unsigned max_sub_layers_minus1)
{
	unsigned profile_present;
	unsigned level_present;
	unsigned index;

	/* Refuses arguments no valid caller passes and sub-layer counts above 6 (7.4.3.1). */
	if (ptl == NULL || bits == NULL)
		return EINVAL;
	if (max_sub_layers_minus1 >= H265SW_MAX_SUB_LAYERS)
		return EINVAL;
	memset(ptl, 0, sizeof(*ptl));

	/* Reads the general profile, its 32 compatibility flags (flag j is bit j) and the four source flags. */
	ptl->profile_space = vcodec_bitreader_u(bits, 2U);
	ptl->tier = vcodec_bitreader_u1(bits);
	ptl->profile_idc = vcodec_bitreader_u(bits, 5U);
	for (index = 0U; index < 32U; index++)
		ptl->compatibility |= vcodec_bitreader_u1(bits) << index;
	ptl->progressive = (int)vcodec_bitreader_u1(bits);
	ptl->interlaced = (int)vcodec_bitreader_u1(bits);
	ptl->non_packed = (int)vcodec_bitreader_u1(bits);
	ptl->frame_only = (int)vcodec_bitreader_u1(bits);

	/* Reads over the constraint and reserved bits, which are 44 whichever branch of 7.3.3 applies, then reads the level. */
	vcodec_bitreader_skip(bits, PS_PROFILE_SKIPPED_BITS);
	ptl->level_idc = vcodec_bitreader_u(bits, 8U);

	/* Reads which sub-layers have a profile part and a level (7.3.3). */
	profile_present = 0U;
	level_present = 0U;
	for (index = 0U; index < max_sub_layers_minus1; index++) {
		profile_present |= vcodec_bitreader_u1(bits) << index;
		level_present |= vcodec_bitreader_u1(bits) << index;
	}

	/* Reads over the reserved pairs that fill the entries up to eight, present when there are sub-layers. */
	if (max_sub_layers_minus1 > 0U)
		vcodec_bitreader_skip(bits, 2U * (PS_PTL_RESERVED_ENTRIES - max_sub_layers_minus1));

	/* Reads over the parts of the sub-layers that have them. */
	for (index = 0U; index < max_sub_layers_minus1; index++) {
		if (((profile_present >> index) & 1U) != 0U)
			vcodec_bitreader_skip(bits, PS_SUB_PROFILE_BITS);
		if (((level_present >> index) & 1U) != 0U)
			vcodec_bitreader_skip(bits, PS_SUB_LEVEL_BITS);
	}

	/* A read past the end makes the whole structure unusable. */
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the general part is read and the rest is passed. */
	return 0;
}

/*
 * Reads a video parameter set RBSP and keeps it in store (7.3.2.1).
 *
 * The set is built in memory taken for the call, so a failure leaves the
 * store as it was.
 */
int
h265sw_ps_read_vps(
	struct h265sw_ps_store *store,
	struct vcodec_bitreader *bits)
{
	struct h265sw_vps *work;
	struct h265sw_hrd *scratch;
	unsigned id;
	int error;

	/* Refuses arguments no valid caller passes. */
	if (store == NULL || bits == NULL)
		return EINVAL;

	/* Takes the set under construction and the scratch HRD parameters. */
	work = malloc(sizeof(*work));
	if (work == NULL)
		return ENOMEM;
	scratch = malloc(PS_VPS_SCRATCH_HRDS * sizeof(*scratch));
	if (scratch == NULL) {
		free(work);
		return ENOMEM;
	}
	memset(work, 0, sizeof(*work));
	memset(scratch, 0, PS_VPS_SCRATCH_HRDS * sizeof(*scratch));

	/* Reads and checks the set. */
	error = ps_parse_vps(work, scratch, bits);
	if (error != 0) {
		free(scratch);
		free(work);
		return error;
	}

	/* Keeps the set, counting it when its content differs from the one stored before. */
	id = work->id;
	if (store->vps_present[id] == 0 || memcmp(&store->vps[id], work, sizeof(*work)) != 0) {
		memcpy(&store->vps[id], work, sizeof(*work));
		store->generation++;
	}
	store->vps_present[id] = 1;
	store->vps_refused[id] = 0;

	/* Gives the memory back. */
	free(scratch);
	free(work);

	/* Succeeded: the set is stored. */
	return 0;
}

/*
 * Reads a sequence parameter set RBSP and keeps it in store (7.3.2.2).
 *
 * The set is built in memory taken for the call, so a failure leaves the
 * store as it was, except that a set refused as ENOTSUP marks its id.
 */
int
h265sw_ps_read_sps(
	struct h265sw_ps_store *store,
	struct vcodec_bitreader *bits)
{
	struct h265sw_sps *work;
	unsigned id;
	int error;

	/* Refuses arguments no valid caller passes. */
	if (store == NULL || bits == NULL)
		return EINVAL;

	/* Takes the set under construction. */
	work = malloc(sizeof(*work));
	if (work == NULL)
		return ENOMEM;
	memset(work, 0, sizeof(*work));

	/* Reads and checks the set. */
	error = ps_parse_sps(work, bits);
	if (error == ENOTSUP) {
		/* Notes that the set was refused for its range, not broken; the id was checked before any such refusal. */
		if (work->id < H265SW_SPS_IDS)
			store->sps_refused[work->id] = 1;
		free(work);
		return ENOTSUP;
	}
	if (error != 0) {
		free(work);
		return error;
	}

	/* Keeps the set, counting it when its content differs from the one stored before. */
	id = work->id;
	if (store->sps_present[id] == 0 || memcmp(&store->sps[id], work, sizeof(*work)) != 0) {
		memcpy(&store->sps[id], work, sizeof(*work));
		store->generation++;
		store->sps_generation[id]++;
	}
	store->sps_present[id] = 1;
	store->sps_refused[id] = 0;

	/* Gives the memory back. */
	free(work);

	/* Succeeded: the set is stored. */
	return 0;
}

/*
 * Reports whether the software decoder takes a sequence.
 *
 * The software decoder reads 8-bit 4:2:0 frame sequences with no range
 * extension tools.
 */
int
h265sw_ps_sps_supported(
	const struct h265sw_sps *sps)
{
	/* A missing set is a caller's mistake. */
	if (sps == NULL)
		return EINVAL;

	/* Only 4:2:0 is decoded. */
	if (sps->chroma_format_idc != 1U)
		return ENOTSUP;

	/* Only 8 bits per sample are decoded. */
	if (sps->bit_depth_luma != H265SW_BIT_DEPTH || sps->bit_depth_chroma != H265SW_BIT_DEPTH)
		return ENOTSUP;

	/* The range extension tools are not decoded. */
	if (sps->range_extension != 0U)
		return ENOTSUP;

	/* Field coding is not decoded. */
	if (sps->vui_present != 0 && sps->vui.field_seq != 0)
		return ENOTSUP;

	/* Succeeded: the sequence is in range. */
	return 0;
}

/*
 * Reads a picture parameter set RBSP and keeps it in store (7.3.2.3).
 *
 * The set is built in a local structure, so a failure leaves the store as
 * it was, except that a set refused as ENOTSUP marks its id.
 */
int
h265sw_ps_read_pps(
	struct h265sw_ps_store *store,
	struct vcodec_bitreader *bits)
{
	struct h265sw_pps work;
	unsigned id;
	int error;

	/* Refuses arguments no valid caller passes. */
	if (store == NULL || bits == NULL)
		return EINVAL;
	memset(&work, 0, sizeof(work));

	/* Reads and checks the set. */
	error = ps_parse_pps(&work, bits);
	if (error == ENOTSUP) {
		/* Notes that the set was refused for its range, not broken; the id was checked before any such refusal. */
		if (work.id < H265SW_PPS_IDS)
			store->pps_refused[work.id] = 1;
		return ENOTSUP;
	}
	if (error != 0)
		return error;

	/* Keeps the set, counting it when its content differs from the one stored before. */
	id = work.id;
	if (store->pps_present[id] == 0 || memcmp(&store->pps[id], &work, sizeof(work)) != 0) {
		memcpy(&store->pps[id], &work, sizeof(work));
		store->generation++;
	}
	store->pps_present[id] = 1;
	store->pps_refused[id] = 0;

	/* Succeeded: the set is stored. */
	return 0;
}

/*
 * Checks a PPS against the SPS it names, for the values whose ranges
 * depend on the SPS (7.4.3.3.1).
 */
int
h265sw_ps_check_pair(
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps)
{
	uint64_t total;
	int qp_bd_offset;
	unsigned entry;

	/* Refuses arguments no valid caller passes. */
	if (sps == NULL || pps == NULL)
		return EINVAL;

	/* The PPS must name this SPS. */
	if (sps->id != pps->sps_id)
		return EINVAL;

	/* Scaling lists in the PPS need scaling lists enabled in the SPS. */
	if (pps->scaling_list_present != 0 && sps->scaling_list_enabled == 0)
		return EINVAL;

	/* init_qp reaches down to -QpBdOffsetY and up to 51 (7.4.3.3.1). */
	qp_bd_offset = 6 * ((int)sps->bit_depth_luma - 8);
	if (pps->init_qp < -qp_bd_offset || pps->init_qp > PS_INIT_QP_MAX)
		return EINVAL;

	/* The depths are bounded by the CTB and coding block sizes (7.4.3.3.1). */
	if (pps->diff_cu_qp_delta_depth > sps->ctb_log2 - sps->min_cb_log2)
		return EINVAL;
	if (pps->log2_parallel_merge_level > sps->ctb_log2)
		return EINVAL;

	/* Without tiles there is nothing more to check. */
	if (pps->tiles == 0)
		return 0;

	/* Each tile column and row holds at least one CTB (7.4.3.3.1). */
	if (pps->tile_columns > sps->pic_width_ctbs || pps->tile_rows > sps->pic_height_ctbs)
		return EINVAL;

	/* Explicit column widths leave at least one CTB for the last column. */
	if (pps->uniform_spacing == 0) {
		total = 0U;
		for (entry = 0U; entry + 1U < pps->tile_columns; entry++)
			total += pps->column_width[entry];
		if (total >= sps->pic_width_ctbs)
			return EINVAL;

		/* Explicit row heights leave at least one CTB for the last row. */
		total = 0U;
		for (entry = 0U; entry + 1U < pps->tile_rows; entry++)
			total += pps->row_height[entry];
		if (total >= sps->pic_height_ctbs)
			return EINVAL;
	}

	/* Succeeded: the pair is consistent. */
	return 0;
}

/* Returns the smaller of two numbers. */
static unsigned
ps_min(
	unsigned first,
	unsigned second)
{
	/* The first is the smaller or equal. */
	if (first <= second)
		return first;

	/* The second is the smaller. */
	return second;
}

/*
 * Moves to the rbsp_stop_one_bit, reading over the extension data flags
 * that a decoder ignores (7.3.2.1, 7.3.2.2.1, 7.3.2.3.1: while
 * more_rbsp_data()).
 */
static void
ps_skip_to_stop(
	struct vcodec_bitreader *bits)
{
	/* Nothing to read over when the stop bit is next, and nothing readable after an error. */
	if (bits->error != 0 || bits->position >= bits->stop)
		return;

	/* Reads over everything before the stop bit. */
	vcodec_bitreader_skip(bits, bits->stop - bits->position);
}

/*
 * Reads the per-sub-layer buffering values of an SPS or a VPS (7.3.2.1,
 * 7.3.2.2.1) and the copies the semantics infer, checking each value
 * (7.4.3.1, 7.4.3.2.1).
 */
static int
ps_read_sub_layer_ordering(
	struct vcodec_bitreader *bits,
	unsigned max_sub_layers_minus1,
	int *ordering_info_present,
	unsigned *max_dec_pic_buffering_minus1,
	unsigned *max_num_reorder_pics,
	uint32_t *max_latency_increase_plus1)
{
	uint32_t ordering_present;
	uint32_t max_dec;
	uint32_t max_reorder;
	uint32_t max_latency;
	unsigned first;
	unsigned index;

	/* Reads whether every sub-layer has values or only the highest. */
	ordering_present = vcodec_bitreader_u1(bits);
	*ordering_info_present = (int)ordering_present;
	first = max_sub_layers_minus1;
	if (ordering_present != 0U)
		first = 0U;

	/* Reads and checks the values. */
	for (index = first; index <= max_sub_layers_minus1; index++) {
		max_dec = vcodec_bitreader_ue(bits);
		max_reorder = vcodec_bitreader_ue(bits);
		max_latency = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;

		/* The buffer holds at most the DPB size, and the reorder depth at most the buffer. */
		if (max_dec > H265SW_MAX_DPB - 1U)
			return EINVAL;
		if (max_reorder > max_dec)
			return EINVAL;

		max_dec_pic_buffering_minus1[index] = max_dec;
		max_num_reorder_pics[index] = max_reorder;
		max_latency_increase_plus1[index] = max_latency;
	}

	/* The values of the highest sub-layer stand for the lower ones when they are not sent (7.4.3.1, 7.4.3.2.1). */
	for (index = 0U; index < first; index++) {
		max_dec_pic_buffering_minus1[index] = max_dec_pic_buffering_minus1[max_sub_layers_minus1];
		max_num_reorder_pics[index] = max_num_reorder_pics[max_sub_layers_minus1];
		max_latency_increase_plus1[index] = max_latency_increase_plus1[max_sub_layers_minus1];
	}

	/* Succeeded: every sub-layer has values. */
	return 0;
}

/* Reads and checks a whole VPS RBSP into work (7.3.2.1, 7.4.3.1). */
static int
ps_parse_vps(
	struct h265sw_vps *work,
	struct h265sw_hrd *scratch,
	struct vcodec_bitreader *bits)
{
	uint32_t layer_sets_minus1;
	uint32_t layer_id_included_bits;
	int error;

	/* Reads the fixed fields up to the profile (vps_reserved_0xffff_16bits is read over: its value is ignored). */
	work->id = vcodec_bitreader_u(bits, 4U);
	work->base_layer_internal = (int)vcodec_bitreader_u1(bits);
	work->base_layer_available = (int)vcodec_bitreader_u1(bits);
	work->max_layers_minus1 = vcodec_bitreader_u(bits, 6U);
	work->max_sub_layers_minus1 = vcodec_bitreader_u(bits, 3U);
	work->temporal_id_nesting = (int)vcodec_bitreader_u1(bits);
	vcodec_bitreader_skip(bits, 16U);
	if (bits->error != 0)
		return EINVAL;
	if (work->max_sub_layers_minus1 >= H265SW_MAX_SUB_LAYERS)
		return EINVAL;

	/* Reads the profile, tier and level. */
	error = h265sw_ps_read_ptl(&work->ptl, bits, work->max_sub_layers_minus1);
	if (error != 0)
		return error;

	/* Reads the buffering values of the sub-layers. */
	error = ps_read_sub_layer_ordering(bits, work->max_sub_layers_minus1, &work->sub_layer_ordering_info_present, work->max_dec_pic_buffering_minus1, work->max_num_reorder_pics, work->max_latency_increase_plus1);
	if (error != 0)
		return error;

	/* Reads the layer sets and reads over their layer_id_included_flag bits. */
	work->max_layer_id = vcodec_bitreader_u(bits, 6U);
	layer_sets_minus1 = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (layer_sets_minus1 > PS_VPS_LAYER_SETS_MAX)
		return EINVAL;
	work->num_layer_sets_minus1 = layer_sets_minus1;
	layer_id_included_bits = layer_sets_minus1 * (work->max_layer_id + 1U);
	vcodec_bitreader_skip(bits, layer_id_included_bits);

	/* Reads the timing and the HRD parameters. */
	error = ps_read_vps_timing(work, scratch, bits);
	if (error != 0)
		return error;

	/* Reads the extension flag and reads over the extension data. */
	work->extension = (int)vcodec_bitreader_u1(bits);
	if (work->extension != 0)
		ps_skip_to_stop(bits);

	/* Reads the trailing bits. */
	error = vcodec_bitreader_trailing(bits);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the VPS is read and checked. */
	return 0;
}

/*
 * Reads the timing information of a VPS and its HRD parameters (7.3.2.1),
 * keeping the first HRD parameters and checking hrd_layer_set_idx.
 */
static int
ps_read_vps_timing(
	struct h265sw_vps *work,
	struct h265sw_hrd *scratch,
	struct vcodec_bitreader *bits)
{
	uint32_t seen[PS_VPS_LAYER_SET_WORDS];
	struct h265sw_hrd *current;
	const struct h265sw_hrd *previous;
	uint32_t num_hrd;
	uint32_t layer_set_idx;
	uint32_t common_present;
	unsigned lowest;
	unsigned index;
	int error;

	/* Without timing there are no HRD parameters. */
	work->timing_present = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (work->timing_present == 0)
		return 0;

	/* Reads the tick and its optional link to the picture order count. */
	work->num_units_in_tick = vcodec_bitreader_u(bits, 32U);
	work->time_scale = vcodec_bitreader_u(bits, 32U);
	work->poc_proportional_to_timing = (int)vcodec_bitreader_u1(bits);
	if (work->poc_proportional_to_timing != 0)
		work->num_ticks_poc_diff_one_minus1 = vcodec_bitreader_ue(bits);
	num_hrd = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Both timing values are non-zero (7.4.3.1), and there are at most as many HRD parameters as layer sets. */
	if (work->num_units_in_tick == 0U || work->time_scale == 0U)
		return EINVAL;
	if (num_hrd > work->num_layer_sets_minus1 + 1U)
		return EINVAL;
	work->num_hrd_parameters = num_hrd;

	/* The layer sets with HRD parameters, none twice, and the lowest set allowed (7.4.3.1). */
	memset(seen, 0, sizeof(seen));
	lowest = 1U;
	if (work->base_layer_internal != 0)
		lowest = 0U;

	/* Reads each HRD parameters with its layer set and, after the first, whether it has its own common part. */
	for (index = 0U; index < num_hrd; index++) {
		layer_set_idx = vcodec_bitreader_ue(bits);
		common_present = 1U;
		if (index > 0U)
			common_present = vcodec_bitreader_u1(bits);
		if (bits->error != 0)
			return EINVAL;

		/* The layer set is in range and not used by an earlier HRD parameters. */
		if (layer_set_idx < lowest || layer_set_idx > work->num_layer_sets_minus1)
			return EINVAL;
		if (((seen[layer_set_idx / 32U] >> (layer_set_idx % 32U)) & 1U) != 0U)
			return EINVAL;
		seen[layer_set_idx / 32U] |= (uint32_t)1U << (layer_set_idx % 32U);

		/* The first is kept; the others alternate between the scratch ones, each taking its common part from the one before it. */
		if (index == 0U) {
			current = &work->hrd;
			previous = NULL;
		} else {
			current = &scratch[(index - 1U) % PS_VPS_SCRATCH_HRDS];
			previous = &work->hrd;
			if (index > 1U)
				previous = &scratch[(index - 2U) % PS_VPS_SCRATCH_HRDS];
		}
		error = h265sw_vui_read_hrd(current, bits, (int)common_present, work->max_sub_layers_minus1, previous);
		if (error != 0)
			return error;
	}

	/* Succeeded: the timing and the HRD parameters are read. */
	return 0;
}

/* Reads and checks a whole SPS RBSP into work (7.3.2.2, 7.4.3.2). */
static int
ps_parse_sps(
	struct h265sw_sps *work,
	struct vcodec_bitreader *bits)
{
	int error;

	/* Reads the format, the size, the bit depths and the buffering. */
	error = ps_sps_format(work, bits);
	if (error != 0)
		return error;

	/* Reads the block sizes and checks the picture size against them. */
	error = ps_sps_blocks(work, bits);
	if (error != 0)
		return error;

	/* Reads the scaling lists, the tool flags and the PCM parameters. */
	work->scaling_list_enabled = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (work->scaling_list_enabled != 0) {
		work->scaling_list_data_present = (int)vcodec_bitreader_u1(bits);
		if (work->scaling_list_data_present != 0) {
			/* Reads the lists of the SPS. */
			error = h265sw_scaling_read(&work->scaling, bits);
			if (error != 0)
				return EINVAL;
		} else {
			/* The default lists stand when none are sent (7.4.3.2.1). */
			h265sw_scaling_default(&work->scaling);
		}
	}
	work->amp = (int)vcodec_bitreader_u1(bits);
	work->sao = (int)vcodec_bitreader_u1(bits);
	error = ps_sps_pcm(work, bits);
	if (error != 0)
		return error;

	/* Reads the short-term sets, the long-term pictures and the last tool flags. */
	error = ps_sps_references(work, bits);
	if (error != 0)
		return error;

	/* Reads the VUI and the extensions. */
	error = ps_sps_extensions(work, bits);
	if (error != 0)
		return error;

	/* Succeeded: the SPS is read and checked. */
	return 0;
}

/*
 * Reads the SPS from its first bit through the sub-layer buffering
 * (7.3.2.2.1), checking each value.
 */
static int
ps_sps_format(
	struct h265sw_sps *work,
	struct vcodec_bitreader *bits)
{
	uint32_t id;
	uint32_t chroma_format_idc;
	uint32_t bit_depth_luma_minus8;
	uint32_t bit_depth_chroma_minus8;
	uint32_t log2_max_poc_lsb_minus4;
	unsigned index;
	int error;

	/* Reads the VPS id and the sub-layer count, then the profile, tier and level. */
	work->vps_id = vcodec_bitreader_u(bits, 4U);
	work->max_sub_layers_minus1 = vcodec_bitreader_u(bits, 3U);
	work->temporal_id_nesting = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (work->max_sub_layers_minus1 >= H265SW_MAX_SUB_LAYERS)
		return EINVAL;
	error = h265sw_ps_read_ptl(&work->ptl, bits, work->max_sub_layers_minus1);
	if (error != 0)
		return error;

	/* Reads the SPS id and the chroma format. */
	id = vcodec_bitreader_ue(bits);
	chroma_format_idc = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (id > PS_SPS_ID_MAX || chroma_format_idc > 3U)
		return EINVAL;
	work->id = id;
	work->chroma_format_idc = chroma_format_idc;
	if (chroma_format_idc == 3U)
		work->separate_colour_plane = (int)vcodec_bitreader_u1(bits);

	/* Reads the size and the conformance window; they are checked once the block sizes are known. */
	work->width = vcodec_bitreader_ue(bits);
	work->height = vcodec_bitreader_ue(bits);
	work->conformance_window = (int)vcodec_bitreader_u1(bits);
	if (work->conformance_window != 0) {
		for (index = 0U; index < 4U; index++)
			work->conf_win_offset[index] = vcodec_bitreader_ue(bits);
	}

	/* Reads the bit depths and the picture order count length. */
	bit_depth_luma_minus8 = vcodec_bitreader_ue(bits);
	bit_depth_chroma_minus8 = vcodec_bitreader_ue(bits);
	log2_max_poc_lsb_minus4 = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (bit_depth_luma_minus8 > PS_BIT_DEPTH_MINUS8_MAX || bit_depth_chroma_minus8 > PS_BIT_DEPTH_MINUS8_MAX)
		return EINVAL;
	if (log2_max_poc_lsb_minus4 > PS_POC_LSB_MINUS4_MAX)
		return EINVAL;
	work->bit_depth_luma = 8U + bit_depth_luma_minus8;
	work->bit_depth_chroma = 8U + bit_depth_chroma_minus8;
	work->log2_max_poc_lsb = 4U + log2_max_poc_lsb_minus4;

	/* Reads the buffering values of the sub-layers. */
	error = ps_read_sub_layer_ordering(bits, work->max_sub_layers_minus1, &work->sub_layer_ordering_info_present, work->max_dec_pic_buffering_minus1, work->max_num_reorder_pics, work->max_latency_increase_plus1);
	if (error != 0)
		return error;

	/* Succeeded: the format is read. */
	return 0;
}

/*
 * Reads the block sizes (7.3.2.2.1), checks them and the picture size
 * against them, and computes the derived sizes (7-10 to 7-19).
 */
static int
ps_sps_blocks(
	struct h265sw_sps *work,
	struct vcodec_bitreader *bits)
{
	uint32_t min_cb_minus3;
	uint32_t diff_cb;
	uint32_t min_tb_minus2;
	uint32_t diff_tb;
	uint32_t depth_inter;
	uint32_t depth_intra;
	unsigned tb_limit;
	int error;

	/* Reads the coding block sizes, each checked before any sum so none wraps. */
	min_cb_minus3 = vcodec_bitreader_ue(bits);
	diff_cb = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (min_cb_minus3 > H265SW_MAX_CTB_LOG2 - H265SW_MIN_CB_LOG2)
		return ENOTSUP;
	if (diff_cb > H265SW_MAX_CTB_LOG2 - H265SW_MIN_CB_LOG2)
		return ENOTSUP;
	work->min_cb_log2 = min_cb_minus3 + 3U;
	work->ctb_log2 = work->min_cb_log2 + diff_cb;

	/* Every profile of Annex A has CTBs of 16 to 64 samples (A.3.2 and following). */
	if (work->ctb_log2 < H265SW_MIN_CTB_LOG2 || work->ctb_log2 > H265SW_MAX_CTB_LOG2)
		return ENOTSUP;

	/* Reads the transform block sizes and the hierarchy depths. */
	min_tb_minus2 = vcodec_bitreader_ue(bits);
	diff_tb = vcodec_bitreader_ue(bits);
	depth_inter = vcodec_bitreader_ue(bits);
	depth_intra = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;

	/* The smallest transform block is below the smallest coding block (7.4.3.2.1). */
	if (min_tb_minus2 >= work->min_cb_log2 - 2U)
		return EINVAL;
	work->min_tb_log2 = min_tb_minus2 + 2U;

	/* The largest transform block is at most 32 and at most the CTB. */
	tb_limit = ps_min(work->ctb_log2, PS_LOG2_TOOL_MAX) - work->min_tb_log2;
	if (diff_tb > tb_limit)
		return EINVAL;
	work->max_tb_log2 = work->min_tb_log2 + diff_tb;

	/* The hierarchy depths reach from the CTB down to the smallest transform block. */
	if (depth_inter > work->ctb_log2 - work->min_tb_log2)
		return EINVAL;
	if (depth_intra > work->ctb_log2 - work->min_tb_log2)
		return EINVAL;
	work->max_th_depth_inter = depth_inter;
	work->max_th_depth_intra = depth_intra;

	/* Checks the picture size and the window against the block sizes and derives the sizes. */
	error = ps_sps_picture_size(work);
	if (error != 0)
		return error;

	/* Succeeded: the block sizes and the sizes derived from them are set. */
	return 0;
}

/*
 * Checks pic_width_in_luma_samples, pic_height_in_luma_samples and the
 * conformance window (7.4.3.2.1) and derives the cropping rectangle and
 * the sizes in blocks (7-14 to 7-19).
 */
static int
ps_sps_picture_size(
	struct h265sw_sps *work)
{
	uint64_t sub_width;
	uint64_t sub_height;
	uint64_t window_width;
	uint64_t window_height;
	unsigned min_cb_size;
	unsigned ctb_size;

	/* The size is non-zero, a multiple of the smallest coding block and within the limits of h265sw.h. */
	min_cb_size = 1U << work->min_cb_log2;
	if (work->width == 0U || work->height == 0U)
		return EINVAL;
	if ((work->width % min_cb_size) != 0U || (work->height % min_cb_size) != 0U)
		return EINVAL;
	if (work->width > H265SW_MAX_SIDE || work->height > H265SW_MAX_SIDE)
		return EINVAL;
	if ((uint64_t)work->width * work->height > H265SW_MAX_LUMA_PS)
		return EINVAL;

	/* The size of a chroma sample in luma samples (Table 6-1); a monochrome or separately coded picture has 1 and 1. */
	sub_width = 1U;
	sub_height = 1U;
	if (work->chroma_format_idc == 1U) {
		sub_width = 2U;
		sub_height = 2U;
	} else if (work->chroma_format_idc == 2U) {
		sub_width = 2U;
	}

	/* The window leaves at least one sample in each direction (7.4.3.2.1), in 64 bits so the offsets cannot wrap. */
	window_width = sub_width * ((uint64_t)work->conf_win_offset[0] + work->conf_win_offset[1]);
	window_height = sub_height * ((uint64_t)work->conf_win_offset[2] + work->conf_win_offset[3]);
	if (window_width >= work->width || window_height >= work->height)
		return EINVAL;
	work->crop_x = (unsigned)(sub_width * work->conf_win_offset[0]);
	work->crop_y = (unsigned)(sub_height * work->conf_win_offset[2]);
	work->crop_width = work->width - (unsigned)window_width;
	work->crop_height = work->height - (unsigned)window_height;

	/* The sizes in coding blocks and CTBs (7-14 to 7-19). */
	ctb_size = 1U << work->ctb_log2;
	work->pic_width_min_cbs = work->width / min_cb_size;
	work->pic_height_min_cbs = work->height / min_cb_size;
	work->pic_width_ctbs = (work->width + ctb_size - 1U) / ctb_size;
	work->pic_height_ctbs = (work->height + ctb_size - 1U) / ctb_size;
	work->pic_size_ctbs = work->pic_width_ctbs * work->pic_height_ctbs;

	/* Succeeded: the size is valid. */
	return 0;
}

/* Reads the PCM parameters of an SPS (7.3.2.2.1) and checks them (7.4.3.2.1). */
static int
ps_sps_pcm(
	struct h265sw_sps *work,
	struct vcodec_bitreader *bits)
{
	uint32_t depth_luma_minus1;
	uint32_t depth_chroma_minus1;
	uint32_t min_minus3;
	uint32_t diff;
	unsigned low;
	unsigned high;

	/* Reads whether PCM is enabled. */
	work->pcm = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (work->pcm == 0)
		return 0;

	/* Reads the sample depths, the block sizes and the loop filter flag. */
	depth_luma_minus1 = vcodec_bitreader_u(bits, 4U);
	depth_chroma_minus1 = vcodec_bitreader_u(bits, 4U);
	min_minus3 = vcodec_bitreader_ue(bits);
	diff = vcodec_bitreader_ue(bits);
	work->pcm_loop_filter_disabled = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* The sample depths are at most the bit depths; the chroma one is ignored without chroma (7.4.3.2.1). */
	if (depth_luma_minus1 + 1U > work->bit_depth_luma)
		return EINVAL;
	if (work->chroma_format_idc != 0U && work->separate_colour_plane == 0 && depth_chroma_minus1 + 1U > work->bit_depth_chroma)
		return EINVAL;
	work->pcm_bit_depth_luma = depth_luma_minus1 + 1U;
	work->pcm_bit_depth_chroma = depth_chroma_minus1 + 1U;

	/* The smallest PCM block is within Min( MinCbLog2SizeY, 5 )..Min( CtbLog2SizeY, 5 ), checked before the sum. */
	low = ps_min(work->min_cb_log2, PS_LOG2_TOOL_MAX);
	high = ps_min(work->ctb_log2, PS_LOG2_TOOL_MAX);
	if (min_minus3 > high - 3U)
		return EINVAL;
	if (min_minus3 + 3U < low)
		return EINVAL;
	work->pcm_min_log2 = min_minus3 + 3U;

	/* The largest PCM block is at most Min( CtbLog2SizeY, 5 ), checked before the sum. */
	if (diff > high - work->pcm_min_log2)
		return EINVAL;
	work->pcm_max_log2 = work->pcm_min_log2 + diff;

	/* Succeeded: the PCM parameters are read. */
	return 0;
}

/*
 * Reads the short-term reference picture sets, the long-term pictures and
 * the two tool flags after them (7.3.2.2.1).
 */
static int
ps_sps_references(
	struct h265sw_sps *work,
	struct vcodec_bitreader *bits)
{
	uint32_t num_st_rps;
	uint32_t num_lt;
	unsigned max_pictures;
	unsigned index;
	int error;

	/* Reads how many short-term sets there are. */
	num_st_rps = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (num_st_rps > H265SW_MAX_ST_RPS - 1U)
		return EINVAL;
	work->num_st_rps = num_st_rps;

	/* Reads each set; a set may predict from the ones before it, which are already derived. */
	max_pictures = work->max_dec_pic_buffering_minus1[work->max_sub_layers_minus1];
	for (index = 0U; index < num_st_rps; index++) {
		error = h265sw_rps_read(bits, index, num_st_rps, work->st_rps, max_pictures, &work->st_rps[index], &work->st_syntax[index]);
		if (error != 0)
			return error;
	}

	/* Reads the long-term pictures the SPS offers. */
	work->long_term_present = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (work->long_term_present != 0) {
		num_lt = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (num_lt > H265SW_MAX_LT_SPS)
			return EINVAL;
		work->num_lt_sps = num_lt;

		/* Reads each picture's order count low bits and its use flag. */
		for (index = 0U; index < num_lt; index++) {
			work->lt_poc_lsb[index] = vcodec_bitreader_u(bits, work->log2_max_poc_lsb);
			work->lt_used[index] = (uint8_t)vcodec_bitreader_u1(bits);
		}
	}

	/* Reads the last two tool flags. */
	work->temporal_mvp = (int)vcodec_bitreader_u1(bits);
	work->strong_intra_smoothing = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the references are read. */
	return 0;
}

/*
 * Reads the VUI and the extensions of an SPS and its trailing bits
 * (7.3.2.2.1, 7.3.2.2.2).
 */
static int
ps_sps_extensions(
	struct h265sw_sps *work,
	struct vcodec_bitreader *bits)
{
	uint32_t range_flag;
	uint32_t multilayer_flag;
	uint32_t flag_3d;
	uint32_t scc_flag;
	uint32_t extension_4bits;
	unsigned index;
	int error;

	/* Reads whether a VUI follows. */
	work->vui_present = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Reads the VUI. */
	if (work->vui_present != 0) {
		error = h265sw_vui_read(&work->vui, bits, work->max_sub_layers_minus1);
		if (error != 0) {
			/* Some old encoders of Main, Main 10 and Main Still Picture streams misplace VUI fields; those profiles have no extensions, so the SPS keeps its other fields and loses the VUI (A.3.2 to A.3.4). */
			if (work->ptl.profile_idc < PS_PROFILE_MAIN_FIRST || work->ptl.profile_idc > PS_PROFILE_MAIN_LAST)
				return EINVAL;
			work->vui_present = 0;
			memset(&work->vui, 0, sizeof(work->vui));
			return 0;
		}
	}

	/* Reads the extension flags. */
	work->extension_present = (int)vcodec_bitreader_u1(bits);
	range_flag = 0U;
	multilayer_flag = 0U;
	flag_3d = 0U;
	scc_flag = 0U;
	extension_4bits = 0U;
	if (work->extension_present != 0) {
		range_flag = vcodec_bitreader_u1(bits);
		multilayer_flag = vcodec_bitreader_u1(bits);
		flag_3d = vcodec_bitreader_u1(bits);
		scc_flag = vcodec_bitreader_u1(bits);
		extension_4bits = vcodec_bitreader_u(bits, 4U);
	}
	if (bits->error != 0)
		return EINVAL;

	/* Reads the nine range extension flags, bit n being the n-th flag. */
	work->range_extension_present = (int)range_flag;
	if (range_flag != 0U) {
		for (index = 0U; index < PS_SPS_RANGE_FLAGS; index++)
			work->range_extension |= vcodec_bitreader_u1(bits) << index;
		if (bits->error != 0)
			return EINVAL;
	}

	/* The multilayer, 3D and screen content extensions are not read. */
	if (multilayer_flag != 0U || flag_3d != 0U || scc_flag != 0U)
		return ENOTSUP;

	/* Reads over the extension data and reads the trailing bits. */
	if (extension_4bits != 0U)
		ps_skip_to_stop(bits);
	error = vcodec_bitreader_trailing(bits);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the SPS is complete. */
	return 0;
}

/* Reads and checks a whole PPS RBSP into work (7.3.2.3, 7.4.3.3). */
static int
ps_parse_pps(
	struct h265sw_pps *work,
	struct vcodec_bitreader *bits)
{
	uint32_t id;
	uint32_t sps_id;
	uint32_t default_l0_minus1;
	uint32_t default_l1_minus1;
	uint32_t depth;
	uint32_t merge_level_minus2;
	int32_t init_qp_minus26;
	int32_t cb_qp_offset;
	int32_t cr_qp_offset;
	int32_t beta_offset_div2;
	int32_t tc_offset_div2;
	int error;

	/* Reads the ids. */
	id = vcodec_bitreader_ue(bits);
	sps_id = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (id > PS_PPS_ID_MAX || sps_id > PS_SPS_ID_MAX)
		return EINVAL;
	work->id = id;
	work->sps_id = sps_id;

	/* Reads the slice header controls and the default reference list sizes. */
	work->dependent_slices = (int)vcodec_bitreader_u1(bits);
	work->output_flag_present = (int)vcodec_bitreader_u1(bits);
	work->num_extra_slice_header_bits = vcodec_bitreader_u(bits, 3U);
	work->sign_data_hiding = (int)vcodec_bitreader_u1(bits);
	work->cabac_init_present = (int)vcodec_bitreader_u1(bits);
	default_l0_minus1 = vcodec_bitreader_ue(bits);
	default_l1_minus1 = vcodec_bitreader_ue(bits);
	init_qp_minus26 = vcodec_bitreader_se(bits);
	if (bits->error != 0)
		return EINVAL;
	if (default_l0_minus1 > PS_NUM_REF_IDX_MINUS1_MAX || default_l1_minus1 > PS_NUM_REF_IDX_MINUS1_MAX)
		return EINVAL;
	if (init_qp_minus26 < PS_INIT_QP_MINUS26_MIN || init_qp_minus26 > PS_INIT_QP_MINUS26_MAX)
		return EINVAL;
	work->num_ref_idx_default[0] = default_l0_minus1 + 1U;
	work->num_ref_idx_default[1] = default_l1_minus1 + 1U;
	work->init_qp = 26 + init_qp_minus26;

	/* Reads the intra, transform skip and QP delta controls. */
	work->constrained_intra = (int)vcodec_bitreader_u1(bits);
	work->transform_skip = (int)vcodec_bitreader_u1(bits);
	work->cu_qp_delta = (int)vcodec_bitreader_u1(bits);
	if (work->cu_qp_delta != 0) {
		depth = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (depth > PS_PPS_DEPTH_MAX)
			return EINVAL;
		work->diff_cu_qp_delta_depth = depth;
	}
	cb_qp_offset = vcodec_bitreader_se(bits);
	cr_qp_offset = vcodec_bitreader_se(bits);
	if (bits->error != 0)
		return EINVAL;
	if (cb_qp_offset < -PS_QP_OFFSET_MAX || cb_qp_offset > PS_QP_OFFSET_MAX)
		return EINVAL;
	if (cr_qp_offset < -PS_QP_OFFSET_MAX || cr_qp_offset > PS_QP_OFFSET_MAX)
		return EINVAL;
	work->cb_qp_offset = cb_qp_offset;
	work->cr_qp_offset = cr_qp_offset;

	/* Reads the weighting, bypass, tile and synchronization flags. */
	work->slice_chroma_qp_offsets_present = (int)vcodec_bitreader_u1(bits);
	work->weighted_pred = (int)vcodec_bitreader_u1(bits);
	work->weighted_bipred = (int)vcodec_bitreader_u1(bits);
	work->transquant_bypass = (int)vcodec_bitreader_u1(bits);
	work->tiles = (int)vcodec_bitreader_u1(bits);
	work->entropy_sync = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Reads the tile layout; without tiles there is one tile, spaced uniformly, and the loop filter crosses tile boundaries by inference (7.4.3.3.1). */
	work->uniform_spacing = 1;
	work->loop_filter_across_tiles = 1;
	work->tile_columns = 1U;
	work->tile_rows = 1U;
	if (work->tiles != 0) {
		error = ps_pps_tiles(work, bits);
		if (error != 0)
			return error;
	}

	/* Reads the loop filter and deblocking controls. */
	work->loop_filter_across_slices = (int)vcodec_bitreader_u1(bits);
	work->deblocking_control_present = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (work->deblocking_control_present != 0) {
		work->deblocking_override_enabled = (int)vcodec_bitreader_u1(bits);
		work->deblocking_disabled = (int)vcodec_bitreader_u1(bits);
		if (work->deblocking_disabled == 0) {
			beta_offset_div2 = vcodec_bitreader_se(bits);
			tc_offset_div2 = vcodec_bitreader_se(bits);
			if (bits->error != 0)
				return EINVAL;
			if (beta_offset_div2 < -PS_DEBLOCK_OFFSET_MAX || beta_offset_div2 > PS_DEBLOCK_OFFSET_MAX)
				return EINVAL;
			if (tc_offset_div2 < -PS_DEBLOCK_OFFSET_MAX || tc_offset_div2 > PS_DEBLOCK_OFFSET_MAX)
				return EINVAL;
			work->beta_offset_div2 = beta_offset_div2;
			work->tc_offset_div2 = tc_offset_div2;
		}
	}

	/* Reads the scaling lists of the PPS. */
	work->scaling_list_present = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (work->scaling_list_present != 0) {
		error = h265sw_scaling_read(&work->scaling, bits);
		if (error != 0)
			return EINVAL;
	}

	/* Reads the list modification flag, the parallel merge level and the slice header extension flag. */
	work->lists_modification_present = (int)vcodec_bitreader_u1(bits);
	merge_level_minus2 = vcodec_bitreader_ue(bits);
	work->slice_header_extension_present = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (merge_level_minus2 > PS_PPS_MERGE_LEVEL_MINUS2_MAX)
		return EINVAL;
	work->log2_parallel_merge_level = merge_level_minus2 + 2U;

	/* Reads the extensions and the trailing bits. */
	error = ps_pps_extensions(work, bits);
	if (error != 0)
		return error;

	/* Succeeded: the PPS is read and checked. */
	return 0;
}

/*
 * Reads the tile layout of a PPS (7.3.2.3.1): the counts, the uniform
 * flag, the explicit sizes and the loop filter flag.
 */
static int
ps_pps_tiles(
	struct h265sw_pps *work,
	struct vcodec_bitreader *bits)
{
	uint32_t columns_minus1;
	uint32_t rows_minus1;
	uint32_t size_minus1;
	unsigned index;

	/* Reads the counts; more tiles than the tables of Annex A allow are outside the range. */
	columns_minus1 = vcodec_bitreader_ue(bits);
	rows_minus1 = vcodec_bitreader_ue(bits);
	work->uniform_spacing = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (columns_minus1 >= H265SW_MAX_TILE_COLS || rows_minus1 >= H265SW_MAX_TILE_ROWS)
		return ENOTSUP;

	/* Tiles enabled means more than one tile (7.4.3.3.1). */
	if (columns_minus1 == 0U && rows_minus1 == 0U)
		return EINVAL;
	work->tile_columns = columns_minus1 + 1U;
	work->tile_rows = rows_minus1 + 1U;

	/* Reads the explicit widths and heights; the last column and row take the rest. */
	if (work->uniform_spacing == 0) {
		for (index = 0U; index < columns_minus1; index++) {
			size_minus1 = vcodec_bitreader_ue(bits);
			if (bits->error != 0)
				return EINVAL;
			if (size_minus1 >= PS_TILE_SIZE_MAX)
				return EINVAL;
			work->column_width[index] = size_minus1 + 1U;
		}
		for (index = 0U; index < rows_minus1; index++) {
			size_minus1 = vcodec_bitreader_ue(bits);
			if (bits->error != 0)
				return EINVAL;
			if (size_minus1 >= PS_TILE_SIZE_MAX)
				return EINVAL;
			work->row_height[index] = size_minus1 + 1U;
		}
	}

	/* Reads whether the loop filter crosses tile boundaries. */
	work->loop_filter_across_tiles = (int)vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the tile layout is read. */
	return 0;
}

/*
 * Reads the extensions of a PPS and its trailing bits (7.3.2.3.1,
 * 7.3.2.3.2), refusing the tools outside the range after reading the
 * range extension in full.
 */
static int
ps_pps_extensions(
	struct h265sw_pps *work,
	struct vcodec_bitreader *bits)
{
	uint32_t range_flag;
	uint32_t multilayer_flag;
	uint32_t flag_3d;
	uint32_t scc_flag;
	uint32_t extension_4bits;
	uint32_t transform_skip_size_minus2;
	uint32_t cross_component;
	uint32_t chroma_list_enabled;
	uint32_t chroma_depth;
	uint32_t chroma_list_len_minus1;
	uint32_t sao_scale_luma;
	uint32_t sao_scale_chroma;
	int32_t qp_offset;
	unsigned entry;
	int unsupported;
	int error;

	/* Reads the extension flags. */
	work->extension_present = (int)vcodec_bitreader_u1(bits);
	range_flag = 0U;
	multilayer_flag = 0U;
	flag_3d = 0U;
	scc_flag = 0U;
	extension_4bits = 0U;
	if (work->extension_present != 0) {
		range_flag = vcodec_bitreader_u1(bits);
		multilayer_flag = vcodec_bitreader_u1(bits);
		flag_3d = vcodec_bitreader_u1(bits);
		scc_flag = vcodec_bitreader_u1(bits);
		extension_4bits = vcodec_bitreader_u(bits, 4U);
	}
	if (bits->error != 0)
		return EINVAL;
	work->range_extension_present = (int)range_flag;

	/* Reads the range extension, remembering whether it turns on a tool that is outside the range. */
	unsupported = 0;
	if (range_flag != 0U) {
		if (work->transform_skip != 0) {
			transform_skip_size_minus2 = vcodec_bitreader_ue(bits);
			if (bits->error != 0)
				return EINVAL;
			if (transform_skip_size_minus2 > PS_PPS_TS_SIZE_MINUS2_MAX)
				return EINVAL;
			if (transform_skip_size_minus2 > 0U)
				unsupported = 1;
		}
		cross_component = vcodec_bitreader_u1(bits);
		chroma_list_enabled = vcodec_bitreader_u1(bits);
		if (bits->error != 0)
			return EINVAL;
		if (cross_component != 0U)
			unsupported = 1;

		/* Reads the chroma QP offset lists, which are checked even though they are refused. */
		if (chroma_list_enabled != 0U) {
			unsupported = 1;
			chroma_depth = vcodec_bitreader_ue(bits);
			chroma_list_len_minus1 = vcodec_bitreader_ue(bits);
			if (bits->error != 0)
				return EINVAL;
			if (chroma_depth > PS_PPS_DEPTH_MAX || chroma_list_len_minus1 > PS_PPS_CHROMA_QP_LIST_MAX)
				return EINVAL;
			for (entry = 0U; entry <= chroma_list_len_minus1; entry++) {
				qp_offset = vcodec_bitreader_se(bits);
				if (bits->error != 0)
					return EINVAL;
				if (qp_offset < -PS_QP_OFFSET_MAX || qp_offset > PS_QP_OFFSET_MAX)
					return EINVAL;
				qp_offset = vcodec_bitreader_se(bits);
				if (bits->error != 0)
					return EINVAL;
				if (qp_offset < -PS_QP_OFFSET_MAX || qp_offset > PS_QP_OFFSET_MAX)
					return EINVAL;
			}
		}

		/* Reads the SAO offset scales. */
		sao_scale_luma = vcodec_bitreader_ue(bits);
		sao_scale_chroma = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (sao_scale_luma > PS_PPS_SAO_SCALE_MAX || sao_scale_chroma > PS_PPS_SAO_SCALE_MAX)
			return EINVAL;
		if (sao_scale_luma > 0U || sao_scale_chroma > 0U)
			unsupported = 1;
	}

	/* The multilayer, 3D and screen content extensions are not read, and a range tool in use is not decoded. */
	if (multilayer_flag != 0U || flag_3d != 0U || scc_flag != 0U)
		return ENOTSUP;
	if (unsupported != 0)
		return ENOTSUP;

	/* Reads over the extension data and reads the trailing bits. */
	if (extension_4bits != 0U)
		ps_skip_to_stop(bits);
	error = vcodec_bitreader_trailing(bits);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the PPS is complete. */
	return 0;
}
