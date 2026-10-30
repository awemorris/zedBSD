/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slice layer the software H.264 encoder writes: the NAL unit header
 * byte (7.3.1), the slice header (7.3.3 with 7.3.3.1, 7.3.3.2 and
 * 7.3.3.3) from the decoder's own struct h264sw_slice_header, the
 * cabac_zero_word count that keeps a CABAC picture within the bin limit
 * (7.4.2.10, 9.3.4.6), and the filler data RBSP (7.3.2.7) that a
 * constant bit rate needs.
 *
 * The header the encoder fills is the one the decoder's reader gives
 * back (h264sw_slice_read): writing and reading round-trip every field
 * the encoder uses.
 */

#ifndef LIBMEDIA_H264ENC_SLICE_H
#define LIBMEDIA_H264ENC_SLICE_H

#include "h264enc.h"
#include "h264sw.h"
#include "h264sw-ps.h"
#include "h264sw-slice.h"
#include "vcodec-bitwriter.h"

/* The nal_unit_type of filler data (Table 7-1). */
#define H264ENC_NAL_FILLER 12U

/*
 * Gives the NAL unit header byte: forbidden_zero_bit 0, nal_ref_idc
 * (0..3) in bits 5..6, nal_unit_type (1..23) in bits 0..4.  Returns the
 * byte, or 0 for a value out of range (a caller's mistake; 0 is never a
 * valid header).
 */
uint8_t h264enc_nal_header(unsigned nal_ref_idc, unsigned nal_unit_type);

/*
 * Writes slice_header() (7.3.3) into writer.
 *
 * header holds what the encoder chose, in the decoder's units: nal_type
 * (1 or 5), nal_ref_idc, idr, first_mb, slice_type (H264SW_SLICE_I, _P
 * or _B; written as that value, never + 5), pps_id, frame_num, idr_pic_id,
 * poc_lsb, direct_spatial, num_ref_idx_active, the modifications, the
 * marking fields, cabac_init_idc, qp (SliceQPY), disable_deblocking and
 * the filter offsets (each even, -12..12).  sps and pps are the sets the
 * header refers to, as the decoder's reader gave them.
 *
 * Fields: first_mb_in_slice ue; slice_type ue; pic_parameter_set_id ue;
 * frame_num u(log2_max_frame_num); idr_pic_id ue (IDR); pic_order_cnt_lsb
 * u(log2_max_poc_lsb) (type 0; delta_pic_order_cnt_bottom is absent
 * because the PPS says so); direct_spatial_mv_pred_flag (B);
 * num_ref_idx_active_override_flag (P and B), 1 exactly when
 * num_ref_idx_active differs from the PPS's defaults for the lists the
 * slice type uses, followed by the minus1 values; ref_pic_list_modification()
 * from modification_count and modification (flag 0 when a count is 0;
 * else each idc and value, then idc 3); pred_weight_table() never (the
 * PPS has no weighted prediction; header->weighted must be 0);
 * dec_ref_pic_marking() when nal_ref_idc is above 0 (IDR:
 * no_output_of_prior_pics_flag and long_term_reference_flag; otherwise
 * adaptive_ref_pic_marking_mode_flag and, when 1, the operations and the
 * final 0); cabac_init_idc ue (CABAC, not I); slice_qp_delta se (qp -
 * pic_init_qp); disable_deblocking_filter_idc ue and, when it is not 1,
 * slice_alpha_c0_offset_div2 and slice_beta_offset_div2 se (the PPS has
 * deblocking_filter_control_present_flag 1).
 *
 * Checks every field against its range and against sps and pps (7.4.3)
 * before writing anything.  Returns 0, EINVAL, or the writer's error.
 * header->data_position is not used.
 */
int h264enc_slice_header_write(struct vcodec_bitwriter *writer, const struct h264sw_slice_header *header, const struct h264sw_sps *sps, const struct h264sw_pps *pps);

/*
 * Gives the number of cabac_zero_word to append to the last slice NAL
 * unit of a CABAC picture (9.3.4.6): k = Ceil( ( Ceil( 3 * ( 32 * bins -
 * RawMbBits * size_mbs ) / 1024 ) - vcl_bytes ) / 3 ) with RawMbBits
 * 3072, or 0 when k is not above 0.  bins is BinCountsInNALunits of the
 * picture, vcl_bytes NumBytesInVclNALunits (every slice NAL unit's bytes
 * after the start code, before the words).  Computed in int64_t; every
 * Ceil of a possibly negative numerator is the exact integer ceiling.
 */
unsigned h264enc_cabac_zero_words(uint64_t bins, uint64_t vcl_bytes, unsigned size_mbs);

/*
 * Writes a filler data RBSP (7.3.2.7) whose NAL unit, once appended with
 * its start code and header, takes total_bytes bytes: total_bytes - 6
 * ff_byte (0xFF) then rbsp_trailing_bits (one byte 0x80).  total_bytes
 * below 6 writes nothing and returns ERANGE (no filler that small
 * exists); 0 otherwise, or the writer's error.  The 0xFF bytes need no
 * emulation prevention.  Its NAL unit header has nal_ref_idc 0 (7.4.1:
 * 0 for nal_unit_type 12).
 */
int h264enc_filler_write(struct vcodec_bitwriter *writer, size_t total_bytes);

#endif
