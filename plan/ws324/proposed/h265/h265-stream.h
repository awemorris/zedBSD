/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.265 elementary stream reader of the Vulkan Video path.
 *
 * Vulkan Video leaves the bitstream to the application: it hands the
 * decoder the parameter sets and each picture's information as StdVideo
 * structures, and the slice segments as offsets into a buffer.  This
 * reader splits an Annex B byte stream into its NAL units, keeps the
 * parameter sets, groups the slice segments into pictures (7.4.2.4.4,
 * 7.4.2.4.5), and works out each picture's NoRaslOutputFlag, picture
 * order count, reference picture set order counts and PicOutputFlag
 * (8.1.3, 8.3.1, 8.3.2), from its slice segment headers.  The pictures
 * it returns are those to decode, in decoding order; h265-dpb.h plans
 * their references and output, h265-std.h makes the Vulkan structures.
 *
 * Pictures it does not return (counted in struct h265_stream_counts):
 * every picture before the first IRAP picture of the stream or after a
 * reset (8.1.3 cannot decode them), the RASL pictures associated with
 * an IRAP picture with NoRaslOutputFlag 1 (8.1.3: not output, and no
 * picture that is output reads them), pictures with a reserved VCL type
 * (7.4.2.2), and broken pictures (a missing or refused parameter set, a
 * slice segment header that does not read, segments that disagree);
 * NAL units of nuh_layer_id above 0 are skipped.
 *
 * A caller gives whole access units: h265_stream_input hands the reader
 * a buffer that holds one or more complete access units (a container's
 * packet, or a whole Annex B file); the last picture of the buffer ends
 * at its end.  The reader borrows the buffer until the next
 * h265_stream_input; the offsets of a picture point into it.
 */

#ifndef LIBMEDIA_H265_STREAM_H
#define LIBMEDIA_H265_STREAM_H

#include "h265sw.h"
#include "h265sw-nal.h"
#include "h265sw-poc.h"
#include "h265sw-ps.h"
#include "h265sw-slice.h"

/*
 * The largest RBSP copy the reader makes of one NAL unit: a whole
 * parameter set (a larger one is refused, counted in refused_sets), or
 * the start of a slice segment NAL unit, enough for its header (a header
 * that runs past it is read again from a copy of the whole NAL unit in
 * slice_rbsp, up to H265_STREAM_SLICE_BYTES; a longer one breaks the
 * picture).  64 KiB holds any SPS within the limits of h265sw.h (64
 * short-term sets, 32 long-term pictures, the scaling lists and the HRD
 * parameters of 7 sub-layers with 32 CPBs each).
 */
#define H265_STREAM_RBSP_BYTES 65536U

/*
 * The longest slice segment header the reader takes: the header of a
 * picture of level 6.2 with every entry point (139 264 CTBs of 16x16 at
 * most, 32 bits each) fits in 1 MiB.
 */
#define H265_STREAM_SLICE_BYTES (1024U * 1024U)

/* The pictures and NAL units the reader did not return, by reason. */
struct h265_stream_counts {
	uint64_t before_irap;
	uint64_t rasl_skipped;
	uint64_t reserved_type;
	uint64_t other_layer;
	uint64_t refused_sets;
	uint64_t broken_pictures;
};

/*
 * One picture to decode.
 *
 * nal_type and temporal_id are those of its VCL NAL units (every segment
 * has the same nal_type, 7.4.2.4.5); vps_id, sps_id and pps_id the
 * parameter sets it uses (sps_id that of the PPS, vps_id that of the
 * SPS); generation the store's generation when it was read (struct
 * h265sw_ps_store), so a caller rebuilds its session parameters when it
 * changes.
 *
 * irap and idr say what the type is; no_rasl_output is NoRaslOutputFlag
 * (8.1.3: 1 for an IDR or BLA picture, the first picture of the stream
 * or after h265_stream_reset, the first picture after an end of sequence
 * NAL unit, and a CRA picture handled as BLA after a reset; 0 for other
 * IRAP pictures; 0 for non-IRAP pictures); first is 1 for the first
 * picture returned after h265_stream_init or h265_stream_reset (picture
 * 0 for C.5.2.2); after_eos is 1 when an end of sequence NAL unit came
 * after the previous picture returned (the caller flushes its DPB with
 * output first, see the design: the reference decoder's output does so);
 * no_output_of_prior_pics is NoOutputOfPriorPicsFlag for an IRAP picture
 * with NoRaslOutputFlag 1 that is not first (C.5.2.2 step 1: 1 for a
 * CRA picture, else no_output_of_prior_pics_flag; the reader never takes
 * the "may" of a changed SPS), 0 otherwise.
 *
 * poc is PicOrderCntVal (8.3.1) and rps the order counts of its
 * reference picture set (8.3.2, 8-5); pic_output is PicOutputFlag (the
 * header's pic_output_flag: the RASL pictures that would get 0 are not
 * returned).  header is the first slice segment's header (with
 * st_syntax, the slice header's short-term set as signalled, for the
 * Vulkan picture information).
 *
 * slice_count segments, each slice_offset[i] and slice_size[i] the bytes
 * of its NAL unit after the start code, in the buffer given to
 * h265_stream_input, in decoding order (dependent segments included).
 * access_unit and access_unit_size are the bytes of the whole access
 * unit from the start code of its first NAL unit.  index counts the
 * pictures returned since h265_stream_init (from 0).
 *
 * About 12 KiB; the caller allocates it.
 */
struct h265_picture {
	unsigned nal_type;
	unsigned temporal_id;
	unsigned vps_id;
	unsigned sps_id;
	unsigned pps_id;
	unsigned generation;
	int irap;
	int idr;
	int no_rasl_output;
	int first;
	int after_eos;
	int no_output_of_prior_pics;
	int32_t poc;
	struct h265sw_rps_pocs rps;
	int pic_output;
	struct h265sw_slice_header header;
	unsigned slice_count;
	size_t slice_offset[H265SW_MAX_SLICE_SEGMENTS];
	size_t slice_size[H265SW_MAX_SLICE_SEGMENTS];
	size_t access_unit;
	size_t access_unit_size;
	uint64_t index;
};

/*
 * A stream being read.
 *
 * data, size and cursor: the borrowed buffer and the position of the next
 * NAL unit to read (the start of its start code).  store: the parameter
 * sets seen.  poc: the order count state (struct h265sw_poc_state).
 * started is 0 until an IRAP picture is returned after h265_stream_init
 * or h265_stream_reset; first_pending is 1 until a picture is returned
 * after them; eos_pending is 1 from an end of sequence NAL unit to the
 * next picture returned; cra_as_bla is 1 after h265_stream_reset until
 * the next IRAP picture (HandleCraAsBlaFlag); rasl_skip is
 * NoRaslOutputFlag of the last IRAP picture returned (its RASL pictures
 * are dropped when 1).  active_sps and active_generation: the SPS of the
 * coded video sequence (activated by its IRAP picture with
 * NoRaslOutputFlag 1, 7.4.3.2.1; a later picture of the sequence that
 * names another SPS, or whose SPS content changed, is broken), valid when
 * started is 1.  counts: what was not returned.  reason: a short static
 * text for the last picture or parameter set not returned (for a log;
 * NULL when none).  returned: pictures returned so far.
 *
 * rbsp is the copy of one NAL unit's RBSP (H265_STREAM_RBSP_BYTES);
 * slice_rbsp, of slice_rbsp_capacity bytes, the copy of a whole slice
 * segment NAL unit whose header runs past rbsp: NULL until the first such
 * segment, then allocated (with malloc, up to H265_STREAM_SLICE_BYTES)
 * and kept until h265_stream_destroy.  independent is the header of the
 * last independent slice segment of the picture being read (dependent
 * segments copy it).
 *
 * About 700 KiB; the caller allocates it (calloc or static).
 */
struct h265_stream {
	const uint8_t *data;
	size_t size;
	size_t cursor;
	struct h265sw_ps_store store;
	struct h265sw_poc_state poc;
	int started;
	int first_pending;
	int eos_pending;
	int cra_as_bla;
	int rasl_skip;
	unsigned active_sps;
	unsigned active_generation;
	struct h265_stream_counts counts;
	const char *reason;
	uint64_t returned;
	struct h265sw_slice_header independent;
	uint8_t rbsp[H265_STREAM_RBSP_BYTES];
	uint8_t *slice_rbsp;
	size_t slice_rbsp_capacity;
};

/* Makes an empty reader: no buffer, no parameter sets, waiting for an IRAP picture. */
void h265_stream_init(struct h265_stream *stream);

/* Frees what the reader allocated (slice_rbsp); the reader may be initialized again. */
void h265_stream_destroy(struct h265_stream *stream);

/*
 * Gives the reader a new buffer of whole access units (data is NULL only
 * when size is 0) and moves the cursor to its start.  The parameter sets,
 * the order count state and the stream state carry over from the buffers
 * before (a container hands one access unit per packet).
 */
void h265_stream_input(struct h265_stream *stream, const uint8_t *data, size_t size);

/*
 * Reads the parameter sets of a buffer that holds no picture (a
 * container's hvcC parameter set arrays, converted to Annex B), up to the
 * first VCL NAL unit, into the store.  The buffer is not kept.  Returns 0;
 * EINVAL when a parameter set does not read (the others are still
 * stored; reason says which).
 */
int h265_stream_parameters(struct h265_stream *stream, const uint8_t *data, size_t size);

/*
 * Reads the next picture to decode from the buffer.
 *
 * Reads NAL units from the cursor: parameter sets are stored
 * (h265sw_ps_read_vps, _sps, _pps over the RBSP copy; one that does not
 * read is counted and the reader goes on), access unit delimiters, SEI
 * and filler data are skipped, an end of sequence NAL unit sets
 * eos_pending (and makes the next IRAP picture's NoRaslOutputFlag 1), and
 * the slice segments of the next picture are gathered: from a segment
 * with first_slice_segment_in_pic_flag 1 up to (not including) the next
 * NAL unit that starts an access unit (h265sw_nal_starts_access_unit, or
 * another segment with first_slice_segment_in_pic_flag 1), or the end of
 * the buffer.  The cursor stays at that NAL unit, so the parameter sets
 * of the next access unit are not stored before the picture is used.
 *
 * For the picture: the first segment's header is read with
 * h265sw_slice_read_start, the PPS and SPS found and checked
 * (h265sw_ps_check_pair; at an IRAP picture with NoRaslOutputFlag 1 the
 * SPS becomes active, else it must be the active one with the same
 * sps_generation), the rest read with h265sw_slice_read_rest, then
 * NoRaslOutputFlag, PicOrderCntVal (h265sw_poc_compute) and the RPS
 * order counts (h265sw_poc_rps).  Each later segment's header is read
 * the same way (independent ones replace independent) and must name the
 * same PPS and have the same nal_unit_type; a picture of more than
 * H265SW_MAX_SLICE_SEGMENTS segments is broken (Table A.8 allows no more
 * up to level 6.2); a missing VPS does not break a picture (the base
 * layer's decoding does not read it: a caller that needs it checks
 * store.vps_present[ vps_id ]); a segment with
 * first_slice_segment_in_pic_flag 0 before any first one is skipped with
 * the picture it belongs to (counted as broken).  A segment header is
 * read from a copy of the first H265_STREAM_RBSP_BYTES payload bytes
 * (h265sw_nal_parse_prefix) and, when that copy is too short, again from
 * a copy of the whole NAL unit.
 *
 * When the picture is returned, h265sw_poc_finish records it, started,
 * first_pending, eos_pending, cra_as_bla and rasl_skip are updated, and
 * index is set.  A picture not returned (see above) leaves the order
 * count state as it was and is counted; the reader goes on to the next.
 *
 * Returns 0 and *picture; ENOENT when the buffer holds no further picture
 * (the parameter sets after the last picture are stored); ENOMEM when
 * slice_rbsp cannot be allocated (the picture is lost, counted as
 * broken, and the reader may go on).  A broken or dropped picture is not
 * an error of this call.  *picture is written only on success.
 */
int h265_stream_next(struct h265_stream *stream, struct h265_picture *picture);

/*
 * Starts again from a random access point (a seek): the next picture
 * returned is the next IRAP picture (a CRA picture is handled as a BLA
 * picture, HandleCraAsBlaFlag 1, so its RASL pictures are dropped), the
 * order count state is reset, and that picture is first.  The parameter
 * sets are kept.
 */
void h265_stream_reset(struct h265_stream *stream);

#endif
