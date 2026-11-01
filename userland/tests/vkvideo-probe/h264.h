/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.264 elementary stream reader of vkvideo-probe.
 *
 * Vulkan Video leaves the bitstream to the application: it hands the
 * decoder the parameter sets and each picture's information as StdVideo
 * structures, and the slices as offsets into a buffer.  This reader splits
 * an Annex B stream into its NAL units, parses the sequence and picture
 * parameter sets into StdVideo form as they come (a set may be sent again
 * with other content under its id), groups the slices into pictures and
 * works out each picture's frame number and order counts from its first
 * slice header.  It covers what the probe decodes: progressive frames of
 * 4:2:0 8-bit Baseline, Main and High streams, one slice group, the three
 * order count types and the memory management operation 5.
 */

#ifndef VKVIDEO_PROBE_H264_H
#define VKVIDEO_PROBE_H264_H

#include <stddef.h>
#include <stdint.h>

/* The StdVideo H.264 structures, through the Vulkan header (zedBSD's and Khronos' both carry them). */
#include <vulkan/vulkan.h>

/* The parameter set id ranges of H.264. */
#define H264_SPS_IDS		32U
#define H264_PPS_IDS		256U

/* The most slices of a picture the probe hands the decoder. */
#define H264_MAX_SLICES		256U

/* The slice types of a picture the probe decodes: I and SI only (P and B are ws083-p006's). */
#define H264_SLICE_P		0U
#define H264_SLICE_B		1U
#define H264_SLICE_I		2U
#define H264_SLICE_SP		3U
#define H264_SLICE_SI		4U

/* The most memory management operations one slice header carries that the reader keeps. */
#define H264_MAX_MMCO		32U

/* The memory management control operations (7.4.3.3). */
#define H264_MMCO_END			0U
#define H264_MMCO_SHORT_UNUSED		1U
#define H264_MMCO_LONG_UNUSED		2U
#define H264_MMCO_SHORT_TO_LONG		3U
#define H264_MMCO_MAX_LONG_INDEX	4U
#define H264_MMCO_ALL_UNUSED		5U
#define H264_MMCO_CURRENT_TO_LONG	6U

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
 * slice's reference picture marking.  After a memory management operation
 * 5 the picture's order counts and frame number start over (8.2.1): later
 * pictures see it with the counts less the smaller of them and frame_num
 * 0; reference_poc holds the counts later pictures see, and display_poc
 * the smaller of them, its place in the display order.
 */
struct h264_picture {
	StdVideoDecodeH264PictureInfo info;
	int32_t reference_poc[2];
	int32_t display_poc;
	int restart;
	int intra;
	uint32_t slice_type;
	uint32_t slice_count;
	size_t slice_offsets[H264_MAX_SLICES];
	size_t slice_sizes[H264_MAX_SLICES];
	size_t access_unit;

	/* The marking: an IDR picture's long-term flag, or the adaptive operations. */
	int long_term_reference;
	int adaptive_marking;
	uint32_t mmco_count;
	struct h264_mmco mmco[H264_MAX_MMCO];
};

/*
 * A stream being read: the bytes; what h264_open found over all of its
 * parameter sets (the first sequence set's profile, the widest and
 * tallest coded picture, the most references); the parameter sets in
 * force at the cursor (the last of each id read so far), each with the
 * length and a hash of the unit it came from, and a count bumped whenever
 * one of them is new or changed; and the order count state the pictures
 * carry over.
 */
struct h264_stream {
	const uint8_t *data;
	size_t size;
	size_t cursor;

	/* The whole stream's needs. */
	int has_profile;
	uint32_t profile_idc;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t max_references;

	/* How often the sets in force changed, the id of the set read last of each kind, and each set's unit. */
	uint32_t generation;
	uint32_t last_sps;
	uint32_t last_pps;
	size_t sps_unit_size[H264_SPS_IDS];
	uint64_t sps_unit_hash[H264_SPS_IDS];
	size_t pps_unit_size[H264_PPS_IDS];
	uint64_t pps_unit_hash[H264_PPS_IDS];

	/* The sequence parameter sets, with the lists their pointers name. */
	int has_sps[H264_SPS_IDS];
	StdVideoH264SequenceParameterSet sps[H264_SPS_IDS];
	StdVideoH264ScalingLists sps_scaling[H264_SPS_IDS];
	int32_t sps_offsets[H264_SPS_IDS][255];

	/* The picture parameter sets, with the lists their pointers name. */
	int has_pps[H264_PPS_IDS];
	StdVideoH264PictureParameterSet pps[H264_PPS_IDS];
	StdVideoH264ScalingLists pps_scaling[H264_PPS_IDS];

	/* The order count state: the last reference picture's MSB and LSB (type 0), the last picture's frame number and offset (types 1 and 2). */
	int32_t previous_msb;
	int32_t previous_lsb;
	uint32_t previous_frame_num;
	uint32_t previous_frame_offset;
};

int h264_open(struct h264_stream *stream, const uint8_t *data, size_t size);
int h264_next_picture(struct h264_stream *stream, struct h264_picture *picture, const char **reason);
int h264_nal_next(const uint8_t *data, size_t size, size_t *cursor, struct h264_nal *nal);

#endif
