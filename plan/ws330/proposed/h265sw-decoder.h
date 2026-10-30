/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The software H.265 decoder as a whole: it takes NAL units one at a
 * time and gives decoded frames in output order (8.1.3, C.5.2).
 *
 * The flow for each NAL unit:
 *
 *  1. h265sw_nal_parse into the decoder's RBSP buffer.  NAL units with
 *     nuh_layer_id above 0 are skipped (after step 2's check of VCL or
 *     not, 7.4.2.4.4 NOTE 3: they never start an access unit).
 *  2. A NAL unit that starts a new access unit (h265sw_nal_starts_access_unit,
 *     or a slice segment with first_slice_segment_in_pic_flag 1), and an
 *     end of sequence or end of bitstream, first finish the picture in
 *     progress (step 6).
 *  3. SPS and PPS: h265sw_ps_read_sps and _pps.  Suffix SEI: the decoded
 *     picture hash of the picture in progress (h265sw_sei_read_hash).
 *     End of sequence: the DPB is flushed with output
 *     (h265sw_dpb_flush) and the next picture has NoRaslOutputFlag 1.
 *     VPS, AUD, prefix SEI, filler data, reserved and unspecified types
 *     are skipped.
 *  4. A slice segment of a decodable type (h265sw_nal_is_decodable):
 *     h265sw_slice_read_start; the PPS and SPS (absent: EINVAL; refused:
 *     ENOTSUP).  A first segment starts a picture: the SPS is activated
 *     at an IRAP picture (the copy kept in sps with its generation; a
 *     change of size or DPB size makes a new pool, DPB and picture
 *     context after the old DPB was flushed or cleared by
 *     NoOutputOfPriorPicsFlag), and any other picture whose PPS names
 *     another SPS (or generation) is dropped with EINVAL (7.4.3.2.1: the
 *     SPS changes only at an IRAP picture with NoRaslOutputFlag 1);
 *     h265sw_ps_check_pair; h265sw_slice_read_rest; NoRaslOutputFlag of an
 *     IRAP picture (IDR, BLA, the first picture, after end of sequence or
 *     a reset: 1; else a CRA 0); RASL pictures associated with an IRAP
 *     picture with NoRaslOutputFlag 1 are skipped (8.1.3: not output and
 *     not decoded, counted as dropped); the order count
 *     (h265sw_poc_compute), the reference picture set (h265sw_poc_rps,
 *     h265sw_dpb_apply_rps), generated pictures (h265sw_dpb_generate:
 *     the Foll entries for BLA and CRA with NoRaslOutputFlag 1, every
 *     missing entry otherwise, counted as concealed), PicOutputFlag
 *     (8.1.3), NoOutputOfPriorPicsFlag and h265sw_dpb_before_picture, a
 *     frame from the pool with the display fields of the SPS's VUI, the
 *     picture context (h265sw_pic_start), the scaling factors
 *     (h265sw_scaling_derive of h265sw_ps_scaling), the CABAC store
 *     cleared.  A later segment of the same picture whose PPS differs
 *     from the first's, whose address is not after the previous segment's
 *     in tile scan, or that would exceed H265SW_MAX_SLICE_SEGMENTS is
 *     dropped with EINVAL.  A dependent segment uses the picture's last
 *     independent header (none: EINVAL).
 *  5. Each segment: its struct h265sw_segment recorded in the picture
 *     context, the reference lists (h265sw_reflist_build) and the motion
 *     context, then h265sw_slice_data on the RBSP from data_offset.
 *  6. Finishing a picture: CTBs no segment decoded are concealed (their
 *     samples copied from the same place of RefPicList0[ 0 ] of the last
 *     P or B slice when there was one, else from the frame output last,
 *     else grey; their blocks intra with filtering off; counted), the
 *     deblocking (h265sw_deblock_picture), SAO (h265sw_sao_picture), the
 *     motion stored in the frame (h265sw_tmvp_store), the decoded picture
 *     hash compared when there was one (stats), h265sw_poc_finish, and
 *     h265sw_dpb_store (which may output frames).
 *
 * Start and recovery: at the start and after h265sw_decoder_reset the
 * decoder waits for an IRAP picture (pictures before it are dropped and
 * counted); a CRA picture there gets NoRaslOutputFlag 1 (HandleCraAsBla
 * by the start, 8.1.3), so its RASL pictures are skipped.  A P or B
 * slice whose references are missing is decoded with generated stand-ins
 * and its frame's concealed count says so.
 *
 * An error inside a slice segment ends that segment; the decoder goes on
 * with the next and conceals what is left.  An error that leaves no
 * picture to decode drops the segment.  No error leaves the decoder
 * unable to take the next IRAP picture; only h265sw_decoder_destroy frees
 * it.
 */

#ifndef LIBMEDIA_H265SW_DECODER_H
#define LIBMEDIA_H265SW_DECODER_H

#include "h265sw.h"
#include "h265sw-ctu.h"
#include "h265sw-dpb.h"
#include "h265sw-pic.h"
#include "h265sw-poc.h"
#include "h265sw-ps.h"
#include "h265sw-recon.h"
#include "h265sw-reflist.h"
#include "h265sw-sao.h"
#include "h265sw-scaling.h"
#include "h265sw-sei.h"
#include "h265sw-slice.h"
#include "h265sw-slicedata.h"
#include "vcodec-frame.h"
#include "vcodec-output.h"

/*
 * What a decoder has done, for the log and the tests: pictures decoded,
 * frames output, CTBs concealed, generated reference pictures, slice
 * segments that ended in an error, pictures dropped (before the first
 * IRAP picture, and skipped RASL pictures), NAL units refused as
 * unsupported, and the decoded picture hashes that matched, did not
 * match, or were absent.
 */
struct h265sw_decoder_stats {
	uint64_t pictures;
	uint64_t output;
	uint64_t concealed_ctbs;
	uint64_t generated;
	uint64_t segment_errors;
	uint64_t dropped;
	uint64_t unsupported;
	uint64_t hash_match;
	uint64_t hash_mismatch;
	uint64_t hash_absent;
};

/*
 * One decoder.
 *
 * ps the parameter sets; rbsp the RBSP buffer (rbsp_capacity bytes, grown
 * up to H265SW_MAX_NAL_BYTES).
 *
 * The sequence: has_sequence once an SPS was activated; sps the copy of
 * the active SPS and sps_generation its store generation; pps the copy
 * of the picture's PPS; pool, dpb, output, poc; pic the picture
 * context; scaling the picture's matrices; sao the SAO scratch planes;
 * ctu the scratch of one CTU; store the CABAC store; qp the QP state.
 *
 * The picture in progress: in_picture; frame (with a reference);
 * first the header of its first segment, independent the header of its
 * last independent segment, header the segment being decoded; poc_val
 * its PicOrderCntVal; irap_no_rasl and no_rasl_output (NoRaslOutputFlag
 * of the current IRAP picture and of the IRAP picture the current
 * picture is associated with); pic_output PicOutputFlag; sets the
 * reference picture set; lists the current slice's lists; hash the
 * decoded picture hash received for it; time_us the time of its first
 * segment's NAL unit; last_ref_frame RefPicList0[ 0 ] of its last P or B
 * slice (the concealment's source).
 *
 * waiting: 1 at the start and after a reset, until an IRAP picture.
 * after_eos: 1 after an end of sequence (the next IRAP picture has
 * NoRaslOutputFlag 1).  first_picture: 1 until the first picture of the
 * stream has started (C.5.2.2's "picture 0").  last_output: the frame
 * output last (with a reference).  decode_order_times: set by the
 * caller when the times it gives are in decoding order
 * (h265sw_decoder_output).  verify_hash: compare decoded picture hashes
 * (on by default; the tests rely on it).
 */
struct h265sw_decoder {
	struct h265sw_ps_store ps;
	uint8_t *rbsp;
	size_t rbsp_capacity;

	int has_sequence;
	struct h265sw_sps sps;
	unsigned sps_generation;
	struct h265sw_pps pps;
	struct vcodec_frame_pool *pool;
	struct h265sw_dpb dpb;
	struct vcodec_output_queue output;
	struct h265sw_poc_state poc;
	struct h265sw_pic pic;
	struct h265sw_scaling_factors scaling;
	struct h265sw_sao_scratch sao;
	struct h265sw_ctu *ctu;
	struct h265sw_cabac_store store;
	struct h265sw_qp_state qp;

	int in_picture;
	struct vcodec_frame *frame;
	struct h265sw_slice_header first;
	struct h265sw_slice_header independent;
	struct h265sw_slice_header header;
	int32_t poc_val;
	int irap_no_rasl;
	int no_rasl_output;
	int pic_output;
	struct h265sw_ref_sets sets;
	struct h265sw_reflists lists;
	struct h265sw_picture_hash hash;
	int64_t time_us;
	struct vcodec_frame *last_ref_frame;

	int waiting;
	int after_eos;
	int first_picture;
	struct vcodec_frame *last_output;
	int decode_order_times;
	int verify_hash;
	struct h265sw_decoder_stats stats;
};

/* Makes a decoder in its start state.  Returns 0 and *decoder, or ENOMEM. */
int h265sw_decoder_create(struct h265sw_decoder **decoder);

/*
 * Frees a decoder: the picture in progress is dropped, the DPB and the
 * output queue cleared, the pool destroyed.  Frames the caller still
 * holds stay valid until it unrefs them.  NULL is ignored.
 */
void h265sw_decoder_destroy(struct h265sw_decoder *decoder);

/*
 * Decodes one NAL unit (the flow above).
 *
 * nal and size are the NAL unit's bytes after its start code
 * (vcodec_nal_find); time_us the presentation time of the packet it came
 * in.  The caller takes every output frame (h265sw_decoder_output)
 * before it gives the next NAL unit: when the output queue is not empty
 * this returns EBUSY and does nothing.
 *
 * Returns 0; EINVAL for a NAL unit or a segment that could not be used
 * (the decoder has recovered); ENOTSUP for a stream outside the
 * decoder's range (an SPS or PPS refused, a segment naming one); ENOMEM.
 */
int h265sw_decoder_nal(struct h265sw_decoder *decoder, const uint8_t *nal, size_t size, int64_t time_us);

/*
 * Finishes the picture in progress, for a caller certain that the access
 * unit has ended (a container whose packets are whole access units).
 * Same returns as h265sw_decoder_nal; 0 when no picture is in progress.
 */
int h265sw_decoder_end_access_unit(struct h265sw_decoder *decoder);

/*
 * Ends the stream: finishes the picture in progress and outputs every
 * frame still waiting in the DPB in output order; then waits for a new
 * IRAP picture as after a reset.
 */
int h265sw_decoder_flush(struct h265sw_decoder *decoder);

/*
 * Takes the next output frame with its reference (the caller unrefs it),
 * or NULL when none is ready.  With decode_order_times 1 the times are
 * moved into output order (vcodec_output_retime over the queue and the
 * DPB frames still needed for output).  The frame taken also becomes
 * last_output.
 */
struct vcodec_frame *h265sw_decoder_output(struct h265sw_decoder *decoder);

/*
 * Drops the picture in progress, the DPB, the output queue and the
 * order count state, for a seek; the parameter sets and the pool are
 * kept.  The decoder waits for an IRAP picture.
 */
void h265sw_decoder_reset(struct h265sw_decoder *decoder);

#endif
