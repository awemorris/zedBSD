/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan Video StdVideoH265 structures of the H.265 decode path: the
 * video, sequence and picture parameter sets of a session's parameters
 * (vulkan_video_codec_h265std.h) and the picture and reference
 * information of one decode (vulkan_video_codec_h265std_decode.h), made
 * from the parse (h265sw-ps.h, h265-stream.h) and the DPB plan
 * (h265-dpb.h).
 *
 * The structures carry everything the parse read, so a driver can use
 * them either way a Vulkan driver does: hand the values to a decoder that
 * parses the slice headers itself (NVDEC), or rebuild the parameter set
 * NAL units for a decoder that reads the bitstream (Wave5).  Where the
 * Vulkan text leaves a choice, this module makes one, which the zedBSD
 * drivers rely on:
 *
 * - A short-term set (StdVideoH265ShortTermRefPicSet) carries both forms:
 *   with inter_ref_pic_set_prediction_flag 1, the prediction syntax
 *   (delta_idx_minus1, delta_rps_sign, abs_delta_rps_minus1,
 *   used_by_curr_pic_flag and use_delta_flag as bit masks) and also the
 *   derived set written as an explicit one (num_negative_pics,
 *   num_positive_pics, delta_poc_s0_minus1[], delta_poc_s1_minus1[] and
 *   the used_by_curr_pic_s0_flag and _s1_flag masks from DeltaPocS0,
 *   DeltaPocS1 and the used flags of 7.4.8); with it 0, the explicit set
 *   as signalled (both forms are then the same).
 * - NumDeltaPocsOfRefRpsIdx is NumDeltaPocs[ RefRpsIdx ] when the slice
 *   header carries its own set (short_term_ref_pic_set_sps_flag 0) and
 *   that set is predicted; 0 otherwise.  (The Vulkan text names
 *   short_term_ref_pic_set_sps_flag 1, where RefRpsIdx is not defined;
 *   the value is needed only to parse a predicted set in the slice
 *   header.)  NumBitsForSTRefPicSetInSlice is the bits of that set when
 *   short_term_ref_pic_set_sps_flag is 0, else 0.
 * - flags.IsReference is always 1: every decoded picture is marked "used
 *   for short-term reference" (8.1.3 step 4; 3.132), and a later
 *   picture's RPS, not the NAL unit type, decides whether it is read.
 * - pScalingLists of an SPS points at the lists in use when
 *   scaling_list_enabled_flag is 1 (the signalled lists, or the default
 *   ones when sps_scaling_list_data_present_flag is 0), NULL otherwise;
 *   of a PPS, at its lists when pps_scaling_list_data_present_flag is 1,
 *   NULL otherwise.
 * - The DecPicBufMgr arrays hold every sub-layer up to
 *   max_sub_layers_minus1 with the inferred values copied down (7.4.3.1,
 *   7.4.3.2.1); entries above are 0.
 * - A VPS carries at most one HRD structure, the first of the VPS
 *   (struct h265sw_vps), and pHrdParameters is NULL without it.
 * - The range extension flags are copied: sps_range_extension_flag and
 *   the nine flags of sps_range_extension() (bit n of
 *   struct h265sw_sps range_extension, from transform_skip_rotation_enabled_flag
 *   as bit 0, to the flag of the same name), and pps_range_extension_flag
 *   (the PPS range extension's tools are refused by the parse, so their
 *   fields are 0).  The screen content (palette, curr_pic_ref, ACT,
 *   motion vector resolution) and other extension fields are 0: the parse
 *   refuses the streams that use them.
 * - The VUI's sar_width and sar_height are the signalled values
 *   (struct h265sw_vui signalled_sar_width and signalled_sar_height, 0
 *   unless aspect_ratio_idc is EXTENDED_SAR), not the derived ratio.
 *
 * Each output structure owns the arrays its pointers name and points into
 * itself: it must not be copied or moved after it is made (make it where
 * it lives, and make it again rather than copying it).
 */

#ifndef LIBMEDIA_H265_STD_H
#define LIBMEDIA_H265_STD_H

#include <vulkan/vulkan.h>

#include "h265sw.h"
#include "h265sw-ps.h"
#include "h265-dpb.h"
#include "h265-stream.h"

/* A VPS in StdVideo form with the structures its pointers name. */
struct h265_std_vps {
	StdVideoH265VideoParameterSet vps;
	StdVideoH265ProfileTierLevel ptl;
	StdVideoH265DecPicBufMgr dpb;
	StdVideoH265HrdParameters hrd;
	StdVideoH265SubLayerHrdParameters hrd_nal[STD_VIDEO_H265_SUBLAYERS_LIST_SIZE];
	StdVideoH265SubLayerHrdParameters hrd_vcl[STD_VIDEO_H265_SUBLAYERS_LIST_SIZE];
};

/* An SPS in StdVideo form with the structures its pointers name. */
struct h265_std_sps {
	StdVideoH265SequenceParameterSet sps;
	StdVideoH265ProfileTierLevel ptl;
	StdVideoH265DecPicBufMgr dpb;
	StdVideoH265ScalingLists scaling;
	StdVideoH265ShortTermRefPicSet st_rps[STD_VIDEO_H265_MAX_SHORT_TERM_REF_PIC_SETS];
	StdVideoH265LongTermRefPicsSps lt;
	StdVideoH265SequenceParameterSetVui vui;
	StdVideoH265HrdParameters hrd;
	StdVideoH265SubLayerHrdParameters hrd_nal[STD_VIDEO_H265_SUBLAYERS_LIST_SIZE];
	StdVideoH265SubLayerHrdParameters hrd_vcl[STD_VIDEO_H265_SUBLAYERS_LIST_SIZE];
};

/* A PPS in StdVideo form with the scaling lists its pointer names. */
struct h265_std_pps {
	StdVideoH265PictureParameterSet pps;
	StdVideoH265ScalingLists scaling;
};

/*
 * Makes the StdVideo form of a profile, tier and level.
 *
 * general_profile_idc is the StdVideoH265ProfileIdc of the same value
 * when it is 1, 2, 3, 4 or 9; otherwise the lowest j of 1, 2 and 3 whose
 * general_profile_compatibility_flag[ j ] is 1 (a stream that names
 * another profile but is compatible with Main, Main 10 or Main Still
 * Picture, 7.4.4); otherwise ENOTSUP.  general_level_idc (30 times the
 * level) maps to the StdVideoH265LevelIdc of the same level (Table A.8's
 * levels 1 to 6.2: 30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180,
 * 183, 186); a value between them maps to the next defined level above
 * it, and a value above 186 (for example 255, the unconstrained level
 * 8.5) or 0 maps to 6.2 or 1.0 respectively, so an off-table value does
 * not stop the decode (the level is only a capability check for the
 * driver).  The flags are copied.  Returns 0 and *out; ENOTSUP as above
 * (out is then not written).
 */
int h265_std_ptl(const struct h265sw_ptl *ptl, StdVideoH265ProfileTierLevel *out);

/*
 * Makes the StdVideo form of a VPS (every field of struct h265sw_vps that
 * StdVideoH265VideoParameterSet names; the reserved fields 0).  out is
 * written in full and points into itself.  Returns 0; ENOTSUP from
 * h265_std_ptl (the profile only).
 */
int h265_std_vps(const struct h265sw_vps *vps, struct h265_std_vps *out);

/*
 * Makes the StdVideo form of an SPS.
 *
 * Every field of StdVideoH265SequenceParameterSet from struct h265sw_sps:
 * the flags, chroma_format_idc, the sizes, the ids, the bit depths
 * (minus 8), the log2 sizes in the minus and diff forms of the syntax
 * (log2_min_luma_coding_block_size_minus3 = min_cb_log2 - 3 and so on),
 * the PCM fields when pcm is 1, the conformance window offsets as
 * signalled, num_short_term_ref_pic_sets and num_long_term_ref_pics_sps;
 * pProfileTierLevel (h265_std_ptl), pDecPicBufMgr, pScalingLists,
 * pShortTermRefPicSet (num_short_term_ref_pic_sets entries, the forms
 * the comment above says; NULL when there are none), pLongTermRefPicsSps
 * (used_by_curr_pic_lt_sps_flag as a bit mask and
 * lt_ref_pic_poc_lsb_sps; NULL without long-term pictures),
 * pSequenceParameterSetVui (NULL without a VUI) with its pHrdParameters
 * (NULL without HRD parameters) and their sub-layer arrays
 * (pSubLayerHrdParametersNal and _Vcl NULL when the matching present flag
 * is 0), pPredictorPaletteEntries NULL.
 *
 * Returns 0; ENOTSUP from h265_std_ptl; EINVAL when a derived short-term
 * set does not fit the explicit form (a difference of consecutive
 * DeltaPocS0 or DeltaPocS1 values outside 1..65536, which no valid stream
 * makes: 8-4 bounds every difference of order counts it uses).
 */
int h265_std_sps(const struct h265sw_sps *sps, struct h265_std_sps *out);

/*
 * Makes the StdVideo form of a PPS.  sps is the SPS the PPS names (its
 * vps_id is the PPS's sps_video_parameter_set_id, the third part of the
 * PPS's key in a session's parameters); NULL is EINVAL.  Every field of
 * StdVideoH265PictureParameterSet from struct h265sw_pps; the tile sizes
 * column_width_minus1[ i ] and row_height_minus1[ i ] for the signalled
 * entries when uniform_spacing_flag is 0, 0 otherwise; pScalingLists as
 * the comment above says.  Returns 0; EINVAL when pps->sps_id is not
 * sps->id.
 */
int h265_std_pps(const struct h265sw_pps *pps, const struct h265sw_sps *sps, struct h265_std_pps *out);

/*
 * Makes the picture information of one decode.
 *
 * picture from h265_stream_next and plan from h265_dpb_begin (the plan's
 * setup slot is not read).  flags.IrapPicFlag, IdrPicFlag, IsReference
 * (1) and short_term_ref_pic_set_sps_flag; the three ids;
 * NumDeltaPocsOfRefRpsIdx and NumBitsForSTRefPicSetInSlice as the comment
 * above says; PicOrderCntVal; RefPicSetStCurrBefore, StCurrAfter and
 * LtCurr from the plan's Curr sets, each slot as a uint8_t and
 * H265_DPB_NO_SLOT as STD_VIDEO_H265_NO_REFERENCE_PICTURE, the entries
 * past each count STD_VIDEO_H265_NO_REFERENCE_PICTURE.
 *
 * Returns 0 and *info; ENOTSUP when a Curr set has more than
 * STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE entries (A.4.1 keeps
 * NumPicTotalCurr at most 8 for every level up to 6.2).
 */
int h265_std_picture(const struct h265_picture *picture, const struct h265_dpb_plan *plan, StdVideoDecodeH265PictureInfo *info);

/*
 * Makes the reference information of the plan's references
 * (reference_slot order): info[i] for reference_slot[ i ], with
 * PicOrderCntVal reference_poc[ i ], flags.used_for_long_term_reference
 * reference_long[ i ] and flags.unused_for_reference 0.  info has room
 * for plan->reference_count entries.
 */
void h265_std_references(const struct h265_dpb_plan *plan, StdVideoDecodeH265ReferenceInfo *info);

/*
 * Makes the reference information of the picture being decoded (the
 * setup slot): PicOrderCntVal poc, both flags 0 (a new picture is a
 * short-term reference, 8.1.3 step 4).
 */
void h265_std_setup(int32_t poc, StdVideoDecodeH265ReferenceInfo *info);

#endif
