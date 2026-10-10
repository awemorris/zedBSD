/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.264 elementary stream reader of libmedia (derived from the repository's original Zlib WS083 probe).
 *
 * Vulkan Video leaves the bitstream to the application: it hands the
 * decoder the parameter sets and each picture's information as StdVideo
 * structures, and the slices as offsets into a buffer.  This reader splits
 * an Annex B stream into its NAL units, parses the sequence and picture
 * parameter sets into StdVideo form, groups the slices into pictures and
 * works out each picture's frame number and order counts from its first
 * slice header.  It covers what the decoder decodes: progressive frames of
 * 4:2:0 8-bit Baseline, Main and High streams, one slice group, the order
 * count types 0, 1 and 2.
 */

#ifndef LIBMEDIA_H264_H
#define LIBMEDIA_H264_H

#include <stddef.h>
#include <stdint.h>

/* The StdVideo H.264 structures, through the Vulkan header (zedBSD's and Khronos' both carry them). */
#include <vulkan/vulkan.h>

/* The parameter set id ranges of H.264. */
#define H264_SPS_IDS 32U
#define H264_PPS_IDS 256U

/* The most slices of a picture the decoder hands the decoder. */
#define H264_MAX_SLICES 256U

/* The slice types of a picture the decoder decodes: progressive I, P and B. */
#define H264_SLICE_P 0U
#define H264_SLICE_B 1U
#define H264_SLICE_I 2U
#define H264_SLICE_SP 3U
#define H264_SLICE_SI 4U

/* The most memory management operations one slice header carries that the reader keeps. */
#define H264_MAX_MMCO 32U
#define H264_MAX_MODIFICATIONS 32U

/* The memory management control operations (7.4.3.3). */
#define H264_MMCO_END 0U
#define H264_MMCO_SHORT_UNUSED 1U
#define H264_MMCO_LONG_UNUSED 2U
#define H264_MMCO_SHORT_TO_LONG 3U
#define H264_MMCO_MAX_LONG_INDEX 4U
#define H264_MMCO_ALL_UNUSED 5U
#define H264_MMCO_CURRENT_TO_LONG 6U

/* One slice list's transmitted modifications, retained for independent missing-reference admission. */
struct h264_list {
	uint32_t active;
	uint32_t count;
	uint32_t operation[H264_MAX_MODIFICATIONS];
	uint32_t argument[H264_MAX_MODIFICATIONS];
};

/* One slice's reference requirements; its lifetime is the containing access unit's decode. */
struct h264_slice {
	uint32_t first_mb;
	uint32_t type;
	struct h264_list list[2];
};

/* One NAL unit of the stream: its type, its nal_ref_idc and its bytes after the start code. */
struct h264_nal {
	uint32_t type;
	uint32_t ref_idc;
	size_t offset;
	size_t size;
};

/* One memory management control operation: the operation and its two numbers (7.3.3.3). */
struct h264_mmco {
	uint32_t operation;
	uint32_t difference_of_pic_nums_minus1;
	uint32_t long_term_pic_num;
	uint32_t long_term_frame_idx;
	uint32_t max_long_term_frame_idx_plus1;
};

/*
 * One picture: its StdVideoDecodeH264PictureInfo, whether all its slices
 * are intra, its first slice's type, its slices (the NAL units after their
 * start codes), where its access unit starts in the stream, and its first
 * slice's reference picture marking.
 */
struct h264_picture {
	StdVideoDecodeH264PictureInfo info;
	int intra;
	uint32_t slice_type;
	uint32_t slice_count;
	size_t slice_offsets[H264_MAX_SLICES];
	size_t slice_sizes[H264_MAX_SLICES];
	struct h264_slice slices[H264_MAX_SLICES];
	size_t access_unit;
	uint32_t poc_lsb;
	int32_t poc_delta[2];
	int no_output_prior;
	int mmco5;

	/* The marking: an IDR picture's long-term flag, or the adaptive operations. */
	int long_term_reference;
	int adaptive_marking;
	uint32_t mmco_count;
	struct h264_mmco mmco[H264_MAX_MMCO];
};

/*
 * A stream being read: the bytes, the parameter sets seen (the last of
 * each id), and the order count state the pictures carry over.
 */
struct h264_stream {
	const uint8_t *data;
	size_t size;
	size_t cursor;

	/* The sequence parameter sets, with the lists their pointers name. */
	int has_sps[H264_SPS_IDS];
	StdVideoH264SequenceParameterSet sps[H264_SPS_IDS];
	StdVideoH264ScalingLists sps_scaling[H264_SPS_IDS];
	int32_t sps_offsets[H264_SPS_IDS][255];
	StdVideoH264SequenceParameterSetVui vui[H264_SPS_IDS];
	StdVideoH264HrdParameters hrd[H264_SPS_IDS][2];

	/* The picture parameter sets, with the lists their pointers name. */
	int has_pps[H264_PPS_IDS];
	StdVideoH264PictureParameterSet pps[H264_PPS_IDS];
	StdVideoH264ScalingLists pps_scaling[H264_PPS_IDS];

	/* The order count state: the last reference picture's MSB and LSB (type 0), frame number and offset (type 2). */
	int32_t previous_msb;
	int32_t previous_lsb;
	uint32_t previous_frame_num;
	uint64_t previous_frame_offset;
	uint32_t previous_reference_frame_num;
	int reference_started;
	int error;
	unsigned parameter_generation;
};

int h264_open(struct h264_stream *stream, const uint8_t *data, size_t size);
int h264_next_picture(struct h264_stream *stream, struct h264_picture *picture, const char **reason);
int h264_nal_next(const uint8_t *data, size_t size, size_t *cursor, struct h264_nal *nal);
void h264_input(struct h264_stream *stream, const uint8_t *data, size_t size);
void h264_flush(struct h264_stream *stream);
int h264_picture_order(struct h264_stream *stream, const StdVideoH264SequenceParameterSet *sps, struct h264_picture *picture);
unsigned h264_reorder_depth(const struct h264_stream *stream, unsigned sps_id);
void h264_aspect(const struct h264_stream *stream, unsigned sps_id, uint32_t *num, uint32_t *den);

#endif
