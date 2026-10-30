/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sequence and picture parameter sets of the software H.264 decoder
 * (7.3.2.1.1, 7.3.2.2) and their scaling lists (7.3.2.1.1.1).
 */

#include "h264sw-ps.h"
#include "h264sw-tables.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* The aspect_ratio_idc that says the sample aspect ratio follows as sar_width and sar_height (Table E-1, Extended_SAR). */
#define PS_EXTENDED_SAR 255U

/* The scaling list sizes (7.3.2.1.1.1): a 4x4 list has 16 entries and an 8x8 list 64. */
#define PS_LIST_SIZE_4X4 16U
#define PS_LIST_SIZE_8X8 64U

/* How many scaling lists the SPS and the PPS of a 4:2:0 stream may send: 6 of 4x4 and 2 of 8x8 (7.3.2.1.1, 7.3.2.2). */
#define PS_LISTS_4X4 6U
#define PS_LISTS_8X8 2U
#define PS_LISTS_ALL 8U

/* The level_idc that means level 1b in every profile taken here (A.3.1, A.3.2). */
#define PS_LEVEL_1B 9U

/* The level_idc of level 1.1, which is level 1b when constraint_set3_flag is 1 in profiles 66, 77 and 88 (A.3.1). */
#define PS_LEVEL_1_1 11U

/* The bit of constraint_flags that holds constraint_set3_flag. */
#define PS_CONSTRAINT_SET3 0x08U

/* The matrix_coefficients value of an unspecified colour description (E.2.1, Table E-5). */
#define PS_MATRIX_UNSPECIFIED 2U

/* The largest values the SPS syntax allows for its ue(v) fields, from the ranges of 7.4.2.1.1. */
#define PS_MAX_LOG2_MINUS4 12U
#define PS_MAX_POC_TYPE 2U
#define PS_MAX_CHROMA_FORMAT_IDC 3U
#define PS_MAX_BIT_DEPTH_MINUS8 6U
#define PS_MAX_SPS_ID 31U

/* The largest PPS values from the ranges of 7.4.2.2. */
#define PS_MAX_PPS_ID 255U
#define PS_MAX_REF_IDX_DEFAULT_MINUS1 31U
#define PS_MAX_BIPRED_IDC 2U
#define PS_MAX_QP_DELTA 25
#define PS_MIN_QP_DELTA (-26)
#define PS_MAX_CHROMA_OFFSET 12
#define PS_MIN_CHROMA_OFFSET (-12)

/* The range of delta_scale (7.4.2.1.1.1). */
#define PS_MAX_DELTA_SCALE 127
#define PS_MIN_DELTA_SCALE (-128)

/* The largest values of the VUI fields that have a range (E.2.1, E.2.2). */
#define PS_MAX_CHROMA_LOC_TYPE 5U
#define PS_MAX_CPB_CNT_MINUS1 31U
#define PS_MAX_BYTES_PER_PIC_DENOM 16U
#define PS_MAX_BITS_PER_MB_DENOM 16U
#define PS_MAX_LOG2_MV_LENGTH 15U

/*
 * One sample aspect ratio of Table E-1: the horizontal and the vertical
 * size of a luma sample for one aspect_ratio_idc.
 */
struct ps_aspect_ratio {
	unsigned width;
	unsigned height;
};

/*
 * One level of Table A-1 with the largest decoded picture buffer it
 * allows, in macroblocks.
 */
struct ps_level_limit {
	unsigned level_idc;
	unsigned max_dpb_mbs;
};

/*
 * The sample aspect ratios of aspect_ratio_idc 1..16, indexed by idc - 1.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table E-1
 * (1:1, 12:11, 10:11, 16:11, 40:33, 24:11, 20:11, 32:11, 80:33, 18:11,
 * 15:11, 64:33, 160:99, 4:3, 3:2, 2:1).
 * It is read-only and lives as long as the program.
 */
static const struct ps_aspect_ratio ps_aspect_ratios[16] = {
	{ 1U, 1U },
	{ 12U, 11U },
	{ 10U, 11U },
	{ 16U, 11U },
	{ 40U, 33U },
	{ 24U, 11U },
	{ 20U, 11U },
	{ 32U, 11U },
	{ 80U, 33U },
	{ 18U, 11U },
	{ 15U, 11U },
	{ 64U, 33U },
	{ 160U, 99U },
	{ 4U, 3U },
	{ 3U, 2U },
	{ 2U, 1U }
};

/*
 * The decoded picture buffer size of each level, in macroblocks (MaxDpbMbs).
 *
 * Values referenced: ITU-T H.264 (08/2021) Table A-1, the Max decoded
 * picture buffer size column, level numbers 1 to 6.2 as level_idc 10 to 62.
 * Level 1b is not a row here: ps_max_dpb_mbs answers 396 for it.
 * It is read-only and lives as long as the program.
 */
static const struct ps_level_limit ps_level_limits[] = {
	{ 10U, 396U },
	{ 11U, 900U },
	{ 12U, 2376U },
	{ 13U, 2376U },
	{ 20U, 2376U },
	{ 21U, 4752U },
	{ 22U, 8100U },
	{ 30U, 8100U },
	{ 31U, 18000U },
	{ 32U, 20480U },
	{ 40U, 32768U },
	{ 41U, 32768U },
	{ 42U, 34816U },
	{ 50U, 110400U },
	{ 51U, 184320U },
	{ 52U, 184320U },
	{ 60U, 696320U },
	{ 61U, 696320U },
	{ 62U, 696320U }
};

/* Static function declarations. */
static int ps_profile_taken(const struct h264sw_sps *sps);
static int ps_read_scaling_list(struct vcodec_bitreader *bits, uint8_t *list, unsigned size, const uint8_t *default_list, unsigned *state);
static void ps_fallback_rule_a(unsigned index, uint8_t list4x4[6][16], uint8_t list8x8[2][64]);
static int ps_read_scaling_matrix(struct vcodec_bitreader *bits, unsigned count, unsigned state[8], uint8_t list4x4[6][16], uint8_t list8x8[2][64], int apply_fallback);
static int ps_read_sps_profile(struct vcodec_bitreader *bits, struct h264sw_sps *sps, int *unsupported);
static int ps_read_sps_order(struct vcodec_bitreader *bits, struct h264sw_sps *sps);
static int ps_read_sps_size(struct vcodec_bitreader *bits, struct h264sw_sps *sps, int *unsupported);
static int ps_read_hrd(struct vcodec_bitreader *bits);
static int ps_read_vui(struct vcodec_bitreader *bits, struct h264sw_sps *sps);
static void ps_vui_reset(struct h264sw_sps *sps);
static unsigned ps_max_dpb_mbs(const struct h264sw_sps *sps);
static unsigned ps_dpb_frames(const struct h264sw_sps *sps);

/*
 * Empties a store: no set present, every generation 0.
 */
void
h264sw_ps_init(
	struct h264sw_ps_store *store)
{
	unsigned index;

	/* A store that is not there has nothing to empty. */
	if (store == NULL)
		return;

	/* Marks every sequence parameter set absent with generation 0. */
	for (index = 0U; index < H264SW_SPS_IDS; index++) {
		store->sps_present[index] = 0;
		store->sps_generation[index] = 0U;
		memset(&store->sps[index], 0, sizeof(store->sps[index]));
	}

	/* Marks every picture parameter set absent. */
	for (index = 0U; index < H264SW_PPS_IDS; index++) {
		store->pps_present[index] = 0;
		memset(&store->pps[index], 0, sizeof(store->pps[index]));
	}
}

/*
 * Reads a sequence parameter set RBSP and keeps it in store (7.3.2.1.1).
 *
 * The set is read into a local copy.  A syntax or range error wins over
 * a valid-but-unsupported choice found earlier in the same set, so a
 * damaged set is never taken for a merely unsupported one.  The VUI is
 * the one part allowed to be wrong: it is dropped and the set kept.
 */
int
h264sw_ps_read_sps(
	struct h264sw_ps_store *store,
	struct vcodec_bitreader *bits)
{
	struct h264sw_sps sps;
	int unsupported;
	int error;
	int vui_ok;
	int different;
	uint32_t flag;
	unsigned id;

	/* Without a store or a reader nothing can be read or kept. */
	if (store == NULL ||
	    bits == NULL)
		return EINVAL;

	/* Starts from an empty set whose scaling lists are flat and whose VUI is absent. */
	memset(&sps, 0, sizeof(sps));
	memset(sps.scaling4x4, 16, sizeof(sps.scaling4x4));
	memset(sps.scaling8x8, 16, sizeof(sps.scaling8x8));
	ps_vui_reset(&sps);
	unsupported = 0;

	/* Reads the profile, the level, the id and the fields only the High profiles send. */
	error = ps_read_sps_profile(bits, &sps, &unsupported);
	if (error != 0)
		return error;

	/* Reads the frame number length and the picture order count parameters. */
	error = ps_read_sps_order(bits, &sps);
	if (error != 0)
		return error;

	/* Reads the reference frame count, the picture size and the cropping window. */
	error = ps_read_sps_size(bits, &sps, &unsupported);
	if (error != 0)
		return error;

	/* Reads vui_parameters_present_flag, which ends the part that cannot be dropped. */
	flag = vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Reads the VUI when it is present; a VUI that does not read is dropped without failing the set. */
	vui_ok = 0;
	if (flag != 0U) {
		error = ps_read_vui(bits, &sps);
		if (error == 0) {
			sps.vui_present = 1;
			vui_ok = 1;
		} else {
			ps_vui_reset(&sps);
		}
	}

	/* Checks the trailing bits; after a dropped VUI the position is not trustworthy, so they are not checked. */
	if (flag == 0U ||
	    vui_ok != 0) {
		error = vcodec_bitreader_trailing(bits);
		if (error != 0)
			return EINVAL;
	}

	/* Refuses a valid set that uses a tool or a format the decoder does not have. */
	if (unsupported != 0)
		return ENOTSUP;

	/* Derives the decoded picture buffer size from the level and the picture size. */
	sps.dpb_frames = ps_dpb_frames(&sps);

	/* Compares the new set with the one it replaces to know whether the sequence changed. */
	id = sps.id;
	different = 0;
	if (store->sps_present[id] != 0) {
		different = memcmp(&store->sps[id], &sps, sizeof(sps));
	}

	/* Keeps the set; a replacement with other content moves the generation on. */
	if (different != 0)
		store->sps_generation[id]++;
	store->sps[id] = sps;
	store->sps_present[id] = 1;

	/* Succeeded: the set is stored. */
	return 0;
}

/*
 * Reads a picture parameter set RBSP and keeps it in store (7.3.2.2).
 *
 * The set is read into a local copy and stored only when it is whole.
 */
int
h264sw_ps_read_pps(
	struct h264sw_ps_store *store,
	struct vcodec_bitreader *bits)
{
	struct h264sw_pps pps;
	uint32_t id;
	uint32_t sps_id;
	uint32_t slice_groups;
	uint32_t count;
	uint32_t value;
	int32_t signed_value;
	int error;
	int more;
	unsigned lists;

	/* Without a store or a reader nothing can be read or kept. */
	if (store == NULL ||
	    bits == NULL)
		return EINVAL;

	/* Starts from an empty set. */
	memset(&pps, 0, sizeof(pps));

	/* Reads the two ids, the entropy coding mode and the bottom field order flag. */
	id = vcodec_bitreader_ue(bits);
	sps_id = vcodec_bitreader_ue(bits);
	pps.cabac = (int)vcodec_bitreader_u1(bits);
	pps.bottom_field_poc_present = (int)vcodec_bitreader_u1(bits);
	slice_groups = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Checks the ids against their ranges. */
	if (id > PS_MAX_PPS_ID ||
	    sps_id > PS_MAX_SPS_ID)
		return EINVAL;
	pps.id = id;
	pps.sps_id = sps_id;

	/* Refuses slice groups, whose map syntax this decoder does not read. */
	if (slice_groups != 0U)
		return ENOTSUP;

	/* Reads the default reference list lengths of both lists. */
	count = vcodec_bitreader_ue(bits);
	if (count > PS_MAX_REF_IDX_DEFAULT_MINUS1)
		return EINVAL;
	pps.num_ref_idx_default[0] = count + 1U;
	count = vcodec_bitreader_ue(bits);
	if (count > PS_MAX_REF_IDX_DEFAULT_MINUS1)
		return EINVAL;
	pps.num_ref_idx_default[1] = count + 1U;

	/* Reads the weighted prediction modes. */
	pps.weighted_pred = (int)vcodec_bitreader_u1(bits);
	value = vcodec_bitreader_u(bits, 2U);
	if (value > PS_MAX_BIPRED_IDC)
		return EINVAL;
	pps.weighted_bipred_idc = value;

	/* Reads the initial quantization parameter, and skips the one for SP and SI slices after checking it. */
	signed_value = vcodec_bitreader_se(bits);
	if (signed_value < PS_MIN_QP_DELTA ||
	    signed_value > PS_MAX_QP_DELTA)
		return EINVAL;
	pps.pic_init_qp = 26 + (int)signed_value;
	signed_value = vcodec_bitreader_se(bits);
	if (signed_value < PS_MIN_QP_DELTA ||
	    signed_value > PS_MAX_QP_DELTA)
		return EINVAL;

	/* Reads the chroma quantization offset of the Cb component. */
	signed_value = vcodec_bitreader_se(bits);
	if (signed_value < PS_MIN_CHROMA_OFFSET ||
	    signed_value > PS_MAX_CHROMA_OFFSET)
		return EINVAL;
	pps.chroma_qp_offset = (int)signed_value;

	/* Reads the three flags that change how slices are decoded. */
	pps.deblocking_control_present = (int)vcodec_bitreader_u1(bits);
	pps.constrained_intra_pred = (int)vcodec_bitreader_u1(bits);
	pps.redundant_pic_cnt_present = (int)vcodec_bitreader_u1(bits);

	/* A read that ran past the end of the RBSP is a truncated set. */
	if (bits->error != 0)
		return EINVAL;

	/* The Cr offset follows the Cb one unless the High profile fields say otherwise. */
	pps.second_chroma_qp_offset = pps.chroma_qp_offset;

	/* Reads the fields only the High profile sends, which are present when more data follows. */
	more = vcodec_bitreader_more_rbsp_data(bits);
	if (more != 0) {
		pps.transform_8x8_mode = (int)vcodec_bitreader_u1(bits);
		pps.scaling_matrix_present = (int)vcodec_bitreader_u1(bits);
		if (bits->error != 0)
			return EINVAL;

		/* Reads the scaling lists the picture sends; the absent ones are resolved later against the SPS. */
		if (pps.scaling_matrix_present != 0) {
			lists = PS_LISTS_4X4 + PS_LISTS_8X8 * (unsigned)pps.transform_8x8_mode;
			error = ps_read_scaling_matrix(bits, lists, pps.scaling_state, pps.scaling4x4, pps.scaling8x8, 0);
			if (error != 0)
				return error;
		}

		/* Reads the chroma quantization offset of the Cr component. */
		signed_value = vcodec_bitreader_se(bits);
		if (bits->error != 0)
			return EINVAL;
		if (signed_value < PS_MIN_CHROMA_OFFSET ||
		    signed_value > PS_MAX_CHROMA_OFFSET)
			return EINVAL;
		pps.second_chroma_qp_offset = (int)signed_value;
	}

	/* Checks the trailing bits. */
	error = vcodec_bitreader_trailing(bits);
	if (error != 0)
		return EINVAL;

	/* Keeps the set, replacing the earlier one of the same id. */
	store->pps[id] = pps;
	store->pps_present[id] = 1;

	/* Succeeded: the set is stored. */
	return 0;
}

/*
 * Resolves the scaling lists a picture uses from its SPS and PPS (7.4.2.1.1, 7.4.2.2, Table 7-2).
 *
 * The lists are produced in the order of Table 7-2: index 0..5 are the
 * 4x4 lists and index 6 and 7 the two 8x8 lists.
 */
void
h264sw_ps_scaling(
	const struct h264sw_sps *sps,
	const struct h264sw_pps *pps,
	struct h264sw_scaling *scaling)
{
	unsigned index;
	unsigned size;
	uint8_t *destination;
	const uint8_t *sent;
	const uint8_t *sequence;
	const uint8_t *default_list;
	const uint8_t *previous;

	/* Without the sets or a place for the answer nothing can be resolved. */
	if (sps == NULL ||
	    pps == NULL ||
	    scaling == NULL)
		return;

	/* Without a matrix in either set every list is flat. */
	if (sps->scaling_matrix_present == 0 &&
	    pps->scaling_matrix_present == 0) {
		memset(scaling->list4x4, 16, sizeof(scaling->list4x4));
		memset(scaling->list8x8, 16, sizeof(scaling->list8x8));
		return;
	}

	/* With a matrix only in the SPS the picture uses the SPS's lists, whose fall-back is already applied. */
	if (pps->scaling_matrix_present == 0) {
		memcpy(scaling->list4x4, sps->scaling4x4, sizeof(scaling->list4x4));
		memcpy(scaling->list8x8, sps->scaling8x8, sizeof(scaling->list8x8));
		return;
	}

	/* Resolves each of the eight lists the PPS may have changed. */
	for (index = 0U; index < PS_LISTS_ALL; index++) {
		/* Picks the arrays of this list: a 4x4 list is index 0..5, an 8x8 list index 6 or 7. */
		previous = NULL;
		if (index < PS_LISTS_4X4) {
			size = PS_LIST_SIZE_4X4;
			destination = scaling->list4x4[index];
			sent = pps->scaling4x4[index];
			sequence = sps->scaling4x4[index];
			default_list = h264sw_default4x4[index / 3U];

			/* Lists 1, 2, 4 and 5 fall back to the list before them in the same group of three. */
			if (index % 3U != 0U)
				previous = scaling->list4x4[index - 1U];
		} else {
			size = PS_LIST_SIZE_8X8;
			destination = scaling->list8x8[index - PS_LISTS_4X4];
			sent = pps->scaling8x8[index - PS_LISTS_4X4];
			sequence = sps->scaling8x8[index - PS_LISTS_4X4];
			default_list = h264sw_default8x8[index - PS_LISTS_4X4];
		}

		/* Takes the list as the PPS sent it, as the default list, or by a fall-back rule. */
		if (pps->scaling_state[index] == H264SW_LIST_EXPLICIT) {
			memcpy(destination, sent, size);
		} else if (pps->scaling_state[index] == H264SW_LIST_DEFAULT) {
			memcpy(destination, default_list, size);
		} else if (previous != NULL) {
			/* Both rule sets say the list before it. */
			memcpy(destination, previous, size);
		} else if (sps->scaling_matrix_present != 0) {
			/* Rule B: the sequence-level list. */
			memcpy(destination, sequence, size);
		} else {
			/* Rule A: the default list. */
			memcpy(destination, default_list, size);
		}
	}
}

/*
 * Says whether the profile_idc, with its constraint flags, is one this decoder takes.
 *
 * Baseline, Main and the High family are taken; Extended is taken only
 * when constraint_set0_flag or constraint_set1_flag is 1 (A.2.1, A.2.2).
 */
static int
ps_profile_taken(
	const struct h264sw_sps *sps)
{
	/* Baseline, Main and the High family are taken as they are. */
	if (sps->profile_idc == H264SW_PROFILE_BASELINE ||
	    sps->profile_idc == H264SW_PROFILE_MAIN ||
	    sps->profile_idc == H264SW_PROFILE_HIGH ||
	    sps->profile_idc == H264SW_PROFILE_HIGH10 ||
	    sps->profile_idc == H264SW_PROFILE_HIGH422 ||
	    sps->profile_idc == H264SW_PROFILE_HIGH444)
		return 1;

	/* Extended also conforms to Baseline or Main when constraint_set0_flag or constraint_set1_flag is 1. */
	if (sps->profile_idc == H264SW_PROFILE_EXTENDED &&
	    (sps->constraint_flags & 0x03U) != 0U)
		return 1;

	/* Every other profile_idc is refused. */
	return 0;
}

/*
 * Reads one scaling_list() (7.3.2.1.1.1) into list.
 *
 * state receives H264SW_LIST_EXPLICIT, or H264SW_LIST_DEFAULT with the
 * default list copied into list when useDefaultScalingMatrixFlag is set.
 * Returns EINVAL for a truncated list or a delta_scale outside -128..127.
 */
static int
ps_read_scaling_list(
	struct vcodec_bitreader *bits,
	uint8_t *list,
	unsigned size,
	const uint8_t *default_list,
	unsigned *state)
{
	unsigned last_scale;
	unsigned next_scale;
	unsigned index;
	int use_default;
	int32_t delta_scale;

	/* Starts the running values as the syntax does: both 8, and no default requested. */
	last_scale = 8U;
	next_scale = 8U;
	use_default = 0;

	/* Reads the entries; once nextScale is 0 the last value repeats to the end of the list. */
	for (index = 0U; index < size; index++) {
		if (next_scale != 0U) {
			delta_scale = vcodec_bitreader_se(bits);
			if (bits->error != 0)
				return EINVAL;
			if (delta_scale < PS_MIN_DELTA_SCALE ||
			    delta_scale > PS_MAX_DELTA_SCALE)
				return EINVAL;

			/* The next value is the last plus the delta, modulo 256 (the delta is at least -128, so the sum stays positive). */
			next_scale = (unsigned)((int)last_scale + (int)delta_scale + 256) % 256U;

			/* A first value of 0 asks for the default list. */
			if (index == 0U &&
			    next_scale == 0U)
				use_default = 1;
		}

		/* Keeps the value read, or repeats the last one when the list ended early. */
		if (next_scale == 0U) {
			list[index] = (uint8_t)last_scale;
		} else {
			list[index] = (uint8_t)next_scale;
		}

		/* The entry just kept is the base of the next delta. */
		last_scale = list[index];
	}

	/* Replaces the list with the default one when the first entry asked for it. */
	if (use_default != 0) {
		memcpy(list, default_list, size);
		*state = H264SW_LIST_DEFAULT;
	} else {
		*state = H264SW_LIST_EXPLICIT;
	}

	/* Succeeded: the list is read. */
	return 0;
}

/*
 * Fills the list of an SPS that was not sent by fall-back rule set A (Table 7-2).
 *
 * Lists 0, 3, 6 and 7 become the default list of their kind; lists 1, 2,
 * 4 and 5 copy the list before them.  index is the scaling list index 0..7.
 */
static void
ps_fallback_rule_a(
	unsigned index,
	uint8_t list4x4[6][16],
	uint8_t list8x8[2][64])
{
	/* Chooses the source by the list index of Table 7-2. */
	switch (index) {
	case 0:
		memcpy(list4x4[0], h264sw_default4x4[0], PS_LIST_SIZE_4X4);
		break;
	case 1:
	case 2:
	case 4:
	case 5:
		memcpy(list4x4[index], list4x4[index - 1U], PS_LIST_SIZE_4X4);
		break;
	case 3:
		memcpy(list4x4[3], h264sw_default4x4[1], PS_LIST_SIZE_4X4);
		break;
	case 6:
		memcpy(list8x8[0], h264sw_default8x8[0], PS_LIST_SIZE_8X8);
		break;
	default:
		memcpy(list8x8[1], h264sw_default8x8[1], PS_LIST_SIZE_8X8);
		break;
	}
}

/*
 * Reads count scaling list presence flags and the lists they announce.
 *
 * Lists not sent are marked H264SW_LIST_ABSENT; when apply_fallback is
 * set (the SPS) they also receive their fall-back rule A values.
 */
static int
ps_read_scaling_matrix(
	struct vcodec_bitreader *bits,
	unsigned count,
	unsigned state[8],
	uint8_t list4x4[6][16],
	uint8_t list8x8[2][64],
	int apply_fallback)
{
	unsigned index;
	uint32_t present;
	int error;

	/* Reads the presence flag of each list and the list itself when it is present. */
	for (index = 0U; index < count; index++) {
		present = vcodec_bitreader_u1(bits);
		if (bits->error != 0)
			return EINVAL;

		/* A sent list is read in its own size, with its own default list. */
		if (present != 0U && index < PS_LISTS_4X4) {
			error = ps_read_scaling_list(bits, list4x4[index], PS_LIST_SIZE_4X4, h264sw_default4x4[index / 3U], &state[index]);
			if (error != 0)
				return error;
		} else if (present != 0U) {
			error = ps_read_scaling_list(bits, list8x8[index - PS_LISTS_4X4], PS_LIST_SIZE_8X8, h264sw_default8x8[index - PS_LISTS_4X4], &state[index]);
			if (error != 0)
				return error;
		} else {
			/* A list that is not sent is marked, and in the SPS filled by rule A. */
			state[index] = H264SW_LIST_ABSENT;
			if (apply_fallback != 0)
				ps_fallback_rule_a(index, list4x4, list8x8);
		}
	}

	/* Succeeded: every announced list is read. */
	return 0;
}

/*
 * Reads profile_idc to seq_parameter_set_id and the fields of the High profiles.
 *
 * Returns ENOTSUP at once for a profile or a 4:4:4 format the rest of
 * the syntax cannot follow; other unsupported choices set *unsupported
 * and reading goes on, so a damaged set still reports EINVAL.
 */
static int
ps_read_sps_profile(
	struct vcodec_bitreader *bits,
	struct h264sw_sps *sps,
	int *unsupported)
{
	uint32_t value;
	uint32_t flag;
	unsigned index;
	unsigned lists;
	int taken;
	int error;

	/* Reads profile_idc. */
	sps->profile_idc = vcodec_bitreader_u(bits, 8U);

	/* Reads constraint_set0..5_flag; bit n of constraint_flags is constraint_setn_flag. */
	sps->constraint_flags = 0U;
	for (index = 0U; index < 6U; index++) {
		flag = vcodec_bitreader_u1(bits);
		sps->constraint_flags |= (unsigned)flag << index;
	}

	/* Skips reserved_zero_2bits, which decoders ignore, and reads level_idc and the id. */
	vcodec_bitreader_skip(bits, 2U);
	sps->level_idc = vcodec_bitreader_u(bits, 8U);
	value = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Refuses a profile the decoder does not take before reading its profile-specific fields. */
	taken = ps_profile_taken(sps);
	if (taken == 0)
		return ENOTSUP;

	/* Checks the id against its range. */
	if (value > PS_MAX_SPS_ID)
		return EINVAL;
	sps->id = value;

	/* Profiles below High have no chroma or bit depth fields: 4:2:0 with 8 bits is implied. */
	if (sps->profile_idc != H264SW_PROFILE_HIGH &&
	    sps->profile_idc != H264SW_PROFILE_HIGH10 &&
	    sps->profile_idc != H264SW_PROFILE_HIGH422 &&
	    sps->profile_idc != H264SW_PROFILE_HIGH444)
		return 0;

	/* Reads chroma_format_idc, which must be 4:2:0 for this decoder. */
	value = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (value > PS_MAX_CHROMA_FORMAT_IDC)
		return EINVAL;
	if (value == 3U)
		return ENOTSUP;
	if (value != 1U)
		*unsupported = 1;

	/* Reads the luma and chroma bit depths, which must be 8 bits. */
	value = vcodec_bitreader_ue(bits);
	if (value > PS_MAX_BIT_DEPTH_MINUS8)
		return EINVAL;
	if (value != 0U)
		*unsupported = 1;
	value = vcodec_bitreader_ue(bits);
	if (value > PS_MAX_BIT_DEPTH_MINUS8)
		return EINVAL;
	if (value != 0U)
		*unsupported = 1;

	/* Reads qpprime_y_zero_transform_bypass_flag, whose lossless coding is not decoded. */
	flag = vcodec_bitreader_u1(bits);
	if (flag != 0U)
		*unsupported = 1;

	/* Reads seq_scaling_matrix_present_flag. */
	flag = vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	sps->scaling_matrix_present = (int)flag;

	/* Reads the eight scaling lists of a 4:2:0 stream, applying rule A to those not sent. */
	if (flag != 0U) {
		lists = PS_LISTS_ALL;
		error = ps_read_scaling_matrix(bits, lists, sps->scaling_state, sps->scaling4x4, sps->scaling8x8, 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the profile fields are read. */
	return 0;
}

/*
 * Reads the frame number length and the picture order count fields (7.3.2.1.1).
 *
 * Fills log2_max_frame_num, max_frame_num, poc_type and the fields of
 * the type that is used, checking each against its range in 7.4.2.1.1.
 */
static int
ps_read_sps_order(
	struct vcodec_bitreader *bits,
	struct h264sw_sps *sps)
{
	uint32_t value;
	uint32_t flag;
	unsigned index;
	int32_t offset;

	/* Reads log2_max_frame_num_minus4 and derives MaxFrameNum (7-10). */
	value = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (value > PS_MAX_LOG2_MINUS4)
		return EINVAL;
	sps->log2_max_frame_num = value + 4U;
	sps->max_frame_num = (uint32_t)1U << sps->log2_max_frame_num;

	/* Reads pic_order_cnt_type. */
	value = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (value > PS_MAX_POC_TYPE)
		return EINVAL;
	sps->poc_type = value;

	/* Type 0 sends the length of the order count's least significant bits. */
	if (sps->poc_type == 0U) {
		value = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (value > PS_MAX_LOG2_MINUS4)
			return EINVAL;
		sps->log2_max_poc_lsb = value + 4U;

		/* Succeeded: type 0 has no further fields. */
		return 0;
	}

	/* Type 2 sends nothing more. */
	if (sps->poc_type == 2U)
		return 0;

	/* Type 1 sends the two offsets, the cycle length and the offsets of one cycle. */
	flag = vcodec_bitreader_u1(bits);
	sps->delta_pic_order_always_zero = (int)flag;
	sps->offset_for_non_ref_pic = vcodec_bitreader_se(bits);
	sps->offset_for_top_to_bottom_field = vcodec_bitreader_se(bits);
	value = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (value > H264SW_MAX_POC_CYCLE)
		return EINVAL;
	sps->poc_cycle_length = value;

	/* Reads the offset of each reference frame of the cycle and adds them up (7-12). */
	sps->expected_delta_per_poc_cycle = 0;
	for (index = 0U; index < sps->poc_cycle_length; index++) {
		offset = vcodec_bitreader_se(bits);
		sps->offset_for_ref_frame[index] = offset;
		sps->expected_delta_per_poc_cycle += (int64_t)offset;
	}

	/* A read that ran past the end of the RBSP is a truncated set. */
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the order count fields are read. */
	return 0;
}

/*
 * Reads the reference frame count, the picture size, the direct inference flag and the cropping window.
 *
 * Sizes above the decoder's limits and interlaced streams set
 * *unsupported; the cropping window is checked against the picture it
 * crops (7.4.2.1.1) with the units of a 4:2:0 picture.
 */
static int
ps_read_sps_size(
	struct vcodec_bitreader *bits,
	struct h264sw_sps *sps,
	int *unsupported)
{
	uint32_t value;
	uint32_t flag;
	uint32_t frame_mbs_only;
	uint32_t crop_left;
	uint32_t crop_right;
	uint32_t crop_top;
	uint32_t crop_bottom;
	uint64_t width_mbs;
	uint64_t height_mbs;
	uint64_t size_mbs;
	unsigned crop_unit_y;

	/* Reads max_num_ref_frames, which cannot exceed the 16 frames a decoded picture buffer holds. */
	value = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (value > H264SW_MAX_REF_FRAMES)
		return EINVAL;
	sps->max_num_ref_frames = value;

	/* Reads gaps_in_frame_num_value_allowed_flag. */
	flag = vcodec_bitreader_u1(bits);
	sps->gaps_allowed = (int)flag;

	/* Reads the picture width and the height in map units, in 64 bits so the largest codes cannot overflow. */
	value = vcodec_bitreader_ue(bits);
	width_mbs = (uint64_t)value + 1U;
	value = vcodec_bitreader_ue(bits);
	height_mbs = (uint64_t)value + 1U;

	/* Reads frame_mbs_only_flag; interlaced coding is outside the decoder, and its frame height is twice the map unit height (7-18). */
	frame_mbs_only = vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	crop_unit_y = 2U;
	if (frame_mbs_only == 0U) {
		*unsupported = 1;
		height_mbs = height_mbs * 2U;
		crop_unit_y = 4U;

		/* Reads mb_adaptive_frame_field_flag, which is not used. */
		flag = vcodec_bitreader_u1(bits);
	}

	/* Reads direct_8x8_inference_flag, which must be 1 when frames and fields can both occur. */
	flag = vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (frame_mbs_only == 0U &&
	    flag == 0U)
		return EINVAL;
	sps->direct_8x8_inference = (int)flag;

	/* Refuses a picture wider or taller than the decoder's limit, or with more macroblocks than it allows. */
	if (width_mbs > H264SW_MAX_SIDE_MBS ||
	    height_mbs > H264SW_MAX_SIDE_MBS) {
		*unsupported = 1;
	} else {
		sps->width_mbs = (unsigned)width_mbs;
		sps->height_mbs = (unsigned)height_mbs;
		size_mbs = width_mbs * height_mbs;
		if (size_mbs > H264SW_MAX_FRAME_MBS) {
			*unsupported = 1;
		} else {
			sps->size_mbs = (unsigned)size_mbs;
		}
	}

	/* Reads frame_cropping_flag. */
	flag = vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (flag == 0U)
		return 0;

	/* Reads the four crop offsets, each in units of 2 luma columns or crop_unit_y luma rows. */
	crop_left = vcodec_bitreader_ue(bits);
	crop_right = vcodec_bitreader_ue(bits);
	crop_top = vcodec_bitreader_ue(bits);
	crop_bottom = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;

	/* The window must keep at least one unit of columns and of rows: left + right + 1 units must fit in the picture. */
	if ((uint64_t)crop_left + (uint64_t)crop_right + 1U > width_mbs * 8U)
		return EINVAL;
	if ((uint64_t)crop_top + (uint64_t)crop_bottom + 1U > height_mbs * 16U / crop_unit_y)
		return EINVAL;

	/* Keeps the cropping window in luma samples; the fit above bounds the products. */
	sps->crop_left = (unsigned)(crop_left * 2U);
	sps->crop_right = (unsigned)(crop_right * 2U);
	sps->crop_top = (unsigned)(crop_top * crop_unit_y);
	sps->crop_bottom = (unsigned)(crop_bottom * crop_unit_y);

	/* Succeeded: the size fields are read. */
	return 0;
}

/*
 * Reads hrd_parameters() (E.1.2) only to skip it, checking cpb_cnt_minus1 (E.2.2).
 */
static int
ps_read_hrd(
	struct vcodec_bitreader *bits)
{
	uint32_t count;
	unsigned index;

	/* Reads cpb_cnt_minus1, which bounds the loop below. */
	count = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (count > PS_MAX_CPB_CNT_MINUS1)
		return EINVAL;

	/* Skips bit_rate_scale and cpb_size_scale. */
	vcodec_bitreader_skip(bits, 8U);

	/* Skips the bit rate, the size and the constant bit rate flag of each CPB. */
	for (index = 0U; index <= count; index++) {
		(void)vcodec_bitreader_ue(bits);
		(void)vcodec_bitreader_ue(bits);
		vcodec_bitreader_skip(bits, 1U);
		if (bits->error != 0)
			return EINVAL;
	}

	/* Skips the four 5-bit length fields. */
	vcodec_bitreader_skip(bits, 20U);
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the parameters are skipped. */
	return 0;
}

/*
 * Reads vui_parameters() (E.1.1) into the VUI fields of sps.
 *
 * Returns EINVAL when the syntax is cut short or a value is outside the
 * range E.2.1 gives; the caller then drops the whole VUI.
 */
static int
ps_read_vui(
	struct vcodec_bitreader *bits,
	struct h264sw_sps *sps)
{
	uint32_t flag;
	uint32_t value;
	uint32_t sar_width;
	uint32_t sar_height;
	uint32_t units;
	uint32_t scale;
	uint32_t nal_hrd;
	uint32_t vcl_hrd;
	int error;

	/* Reads aspect_ratio_info_present_flag and the sample aspect ratio it announces. */
	flag = vcodec_bitreader_u1(bits);
	if (flag != 0U) {
		value = vcodec_bitreader_u(bits, 8U);
		if (value == PS_EXTENDED_SAR) {
			sar_width = vcodec_bitreader_u(bits, 16U);
			sar_height = vcodec_bitreader_u(bits, 16U);

			/* A ratio with a zero side is unspecified. */
			if (sar_width != 0U &&
			    sar_height != 0U) {
				sps->sar_width = sar_width;
				sps->sar_height = sar_height;
			}
		} else if (value >= 1U && value <= 16U) {
			sps->sar_width = ps_aspect_ratios[value - 1U].width;
			sps->sar_height = ps_aspect_ratios[value - 1U].height;
		} else if (value != 0U) {
			/* The values 17 to 254 are reserved. */
			return EINVAL;
		}
	}

	/* Skips overscan_info_present_flag and overscan_appropriate_flag. */
	flag = vcodec_bitreader_u1(bits);
	if (flag != 0U)
		vcodec_bitreader_skip(bits, 1U);

	/* Reads video_signal_type_present_flag, video_format, video_full_range_flag and the colour description. */
	flag = vcodec_bitreader_u1(bits);
	if (flag != 0U) {
		sps->signal_present = 1;
		vcodec_bitreader_skip(bits, 3U);
		value = vcodec_bitreader_u1(bits);
		sps->full_range = (int)value;
		value = vcodec_bitreader_u1(bits);
		if (value != 0U) {
			sps->colour_present = 1;
			vcodec_bitreader_skip(bits, 16U);
			sps->matrix_coefficients = vcodec_bitreader_u(bits, 8U);
		}
	}

	/* Reads chroma_loc_info_present_flag and checks the two location types. */
	flag = vcodec_bitreader_u1(bits);
	if (flag != 0U) {
		value = vcodec_bitreader_ue(bits);
		if (value > PS_MAX_CHROMA_LOC_TYPE)
			return EINVAL;
		value = vcodec_bitreader_ue(bits);
		if (value > PS_MAX_CHROMA_LOC_TYPE)
			return EINVAL;
	}

	/* Reads timing_info_present_flag; both the tick and the scale must be positive. */
	flag = vcodec_bitreader_u1(bits);
	if (flag != 0U) {
		units = vcodec_bitreader_u(bits, 32U);
		scale = vcodec_bitreader_u(bits, 32U);
		vcodec_bitreader_skip(bits, 1U);
		if (bits->error != 0)
			return EINVAL;
		if (units == 0U ||
		    scale == 0U)
			return EINVAL;
	}

	/* Reads the presence of the NAL HRD parameters and skips them. */
	nal_hrd = vcodec_bitreader_u1(bits);
	if (nal_hrd != 0U) {
		error = ps_read_hrd(bits);
		if (error != 0)
			return error;
	}

	/* Reads the presence of the VCL HRD parameters and skips them. */
	vcl_hrd = vcodec_bitreader_u1(bits);
	if (vcl_hrd != 0U) {
		error = ps_read_hrd(bits);
		if (error != 0)
			return error;
	}

	/* Skips low_delay_hrd_flag, present with either HRD, and pic_struct_present_flag. */
	if (nal_hrd != 0U ||
	    vcl_hrd != 0U)
		vcodec_bitreader_skip(bits, 1U);
	vcodec_bitreader_skip(bits, 1U);

	/* Reads bitstream_restriction_flag and the restrictions it announces. */
	flag = vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;
	if (flag != 0U) {
		/* Skips motion_vectors_over_pic_boundaries_flag and checks the four limits that have a range. */
		vcodec_bitreader_skip(bits, 1U);
		value = vcodec_bitreader_ue(bits);
		if (value > PS_MAX_BYTES_PER_PIC_DENOM)
			return EINVAL;
		value = vcodec_bitreader_ue(bits);
		if (value > PS_MAX_BITS_PER_MB_DENOM)
			return EINVAL;
		value = vcodec_bitreader_ue(bits);
		if (value > PS_MAX_LOG2_MV_LENGTH)
			return EINVAL;
		value = vcodec_bitreader_ue(bits);
		if (value > PS_MAX_LOG2_MV_LENGTH)
			return EINVAL;

		/* Reads the reorder depth and the buffer size, which are limited by each other and by the 16 frames a buffer holds. */
		sps->max_num_reorder_frames = vcodec_bitreader_ue(bits);
		sps->max_dec_frame_buffering = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (sps->max_dec_frame_buffering > H264SW_MAX_DPB_FRAMES)
			return EINVAL;
		if (sps->max_dec_frame_buffering < sps->max_num_ref_frames)
			return EINVAL;
		if (sps->max_num_reorder_frames > sps->max_dec_frame_buffering)
			return EINVAL;
		sps->restriction_present = 1;
	}

	/* A read that ran past the end of the RBSP is a truncated VUI. */
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the VUI is read and in range. */
	return 0;
}

/*
 * Puts the VUI fields of sps at their values for an absent VUI.
 */
static void
ps_vui_reset(
	struct h264sw_sps *sps)
{
	/* An unknown aspect ratio is 0:0, and nothing about the signal or the restrictions is known. */
	sps->vui_present = 0;
	sps->sar_width = 0U;
	sps->sar_height = 0U;
	sps->signal_present = 0;
	sps->full_range = 0;
	sps->colour_present = 0;
	sps->matrix_coefficients = PS_MATRIX_UNSPECIFIED;
	sps->restriction_present = 0;
	sps->max_num_reorder_frames = 0U;
	sps->max_dec_frame_buffering = 0U;
}

/*
 * Returns MaxDpbMbs of the set's level (Table A-1), or 0 when the level is not in the table.
 *
 * Level 1b is level_idc 9 in every profile, and level_idc 11 with
 * constraint_set3_flag 1 in profiles 66, 77 and 88 (A.3.1, A.3.2).
 */
static unsigned
ps_max_dpb_mbs(
	const struct h264sw_sps *sps)
{
	unsigned index;
	unsigned count;

	/* Level 1b by its own level_idc. */
	if (sps->level_idc == PS_LEVEL_1B)
		return 396U;

	/* Level 1b by level 1.1 with constraint_set3_flag, in the profiles that define it that way. */
	if (sps->level_idc == PS_LEVEL_1_1 &&
	    (sps->constraint_flags & PS_CONSTRAINT_SET3) != 0U &&
	    (sps->profile_idc == H264SW_PROFILE_BASELINE ||
	     sps->profile_idc == H264SW_PROFILE_MAIN ||
	     sps->profile_idc == H264SW_PROFILE_EXTENDED))
		return 396U;

	/* Looks the level up in Table A-1. */
	count = (unsigned)(sizeof(ps_level_limits) / sizeof(ps_level_limits[0]));
	for (index = 0U; index < count; index++) {
		if (ps_level_limits[index].level_idc == sps->level_idc)
			return ps_level_limits[index].max_dpb_mbs;
	}

	/* A level that is not in the table has no known limit. */
	return 0U;
}

/*
 * Returns the decoded picture buffer size in frames of a set whose other fields are known (A.3.1 h, E.2.1).
 */
static unsigned
ps_dpb_frames(
	const struct h264sw_sps *sps)
{
	unsigned frames;
	unsigned max_dpb_mbs;

	/* The VUI's max_dec_frame_buffering is the size when it is sent. */
	if (sps->restriction_present != 0) {
		frames = sps->max_dec_frame_buffering;
	} else {
		/* Otherwise the level's limit decides; a level the table does not hold allows 16. */
		max_dpb_mbs = ps_max_dpb_mbs(sps);
		if (max_dpb_mbs == 0U) {
			frames = H264SW_MAX_DPB_FRAMES;
		} else {
			frames = max_dpb_mbs / sps->size_mbs;
		}
	}

	/* The buffer must hold every reference frame. */
	if (frames < sps->max_num_ref_frames)
		frames = sps->max_num_ref_frames;

	/* A picture needs at least one buffer. */
	if (frames < 1U)
		frames = 1U;

	/* The buffer never holds more than 16 frames. */
	if (frames > H264SW_MAX_DPB_FRAMES)
		frames = H264SW_MAX_DPB_FRAMES;

	/* Succeeded: the size in frames. */
	return frames;
}
