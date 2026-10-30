/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video usability information of the software H.265 decoder:
 * vui_parameters() (E.2.1, E.3.1) with the hypothetical reference decoder
 * parameters inside it (hrd_parameters() E.2.2 and
 * sub_layer_hrd_parameters() E.2.3), which are read only to be skipped:
 * the SPS fields after the VUI (the extension flags) can be reached only
 * by reading the whole VUI.
 *
 * The decoder keeps what it uses to show a picture (the sample aspect
 * ratio, the range and the colour description) and the one field that
 * puts a sequence outside its range (field_seq_flag).
 */

#ifndef LIBMEDIA_H265SW_VUI_H
#define LIBMEDIA_H265SW_VUI_H

#include "h265sw.h"
#include "vcodec-bitreader.h"

/*
 * What the decoder keeps of a VUI.
 *
 * sar_width and sar_height: the sample aspect ratio of aspect_ratio_idc
 * (Table E.1, 1..16, or sar_width and sar_height of EXTENDED_SAR 255),
 * 0 and 0 when absent, unspecified (0), reserved (17..254) or with a
 * zero term.  signal_present is video_signal_type_present_flag with
 * full_range video_full_range_flag; colour_present
 * colour_description_present_flag with colour_primaries,
 * transfer_characteristics and matrix_coeffs.  field_seq is
 * field_seq_flag.  timing_present with num_units_in_tick and time_scale
 * (vui_timing_info).
 */
struct h265sw_vui {
	unsigned sar_width;
	unsigned sar_height;
	int signal_present;
	int full_range;
	int colour_present;
	unsigned colour_primaries;
	unsigned transfer_characteristics;
	unsigned matrix_coeffs;
	int field_seq;
	int timing_present;
	uint32_t num_units_in_tick;
	uint32_t time_scale;
};

/*
 * Reads vui_parameters() into vui (E.2.1).
 *
 * max_sub_layers_minus1 is sps_max_sub_layers_minus1 (0..6), the
 * maxNumSubLayersMinus1 of hrd_parameters( 1, ... ).  Every field is
 * read in order, including the HRD parameters (E.2.2 with the
 * inferences of E.3.2 for fixed_pic_rate_within_cvs_flag and
 * low_delay_hrd_flag; cpb_cnt_minus1 0..31; sub_layer_hrd_parameters
 * for each of NAL and VCL present, CpbCnt = cpb_cnt_minus1 + 1) and the
 * bitstream restriction, so bits ends just after the VUI.  The aspect
 * ratios of Table E.1 are data of this module.
 *
 * Returns 0; EINVAL for a read error or a value out of its range
 * (cpb_cnt_minus1 above 31, a ue(v) above its stated range).  The caller
 * decides what a failed VUI means for the SPS (h265sw-ps.h).
 */
int h265sw_vui_read(struct h265sw_vui *vui, struct vcodec_bitreader *bits, unsigned max_sub_layers_minus1);

#endif
