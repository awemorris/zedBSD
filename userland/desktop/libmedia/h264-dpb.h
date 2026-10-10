/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Logical H.264 reference state is independent of the Vulkan slots holding decoded images. */
#ifndef LIBMEDIA_H264_DPB_H
#define LIBMEDIA_H264_DPB_H

#include "h264.h"

#define H264_DPB_REFERENCES 16U
#define H264_DPB_SLOTS 17U
#define H264_DPB_UNUSED 0
#define H264_DPB_SHORT 1
#define H264_DPB_LONG 2

/* One logical reference lives until marking removes it, even when no decoded image exists. */
struct h264_dpb_entry {
	int reference;
	int slot;
	int inferred;
	uint32_t frame_num;
	uint32_t long_index;
	int32_t poc[2];
};

/* One decoder owns its logical references and the separate device-slot activation history. */
struct h264_dpb {
	unsigned max_references;
	unsigned slots;
	int max_long_index;
	int started;
	int device_started;
	int seek;
	int32_t seek_poc;
	int active[H264_DPB_SLOTS];
	struct h264_dpb_entry entry[H264_DPB_SLOTS];
};

/* One prepared picture owns these standard submission descriptors until marking commits it. */
struct h264_dpb_plan {
	int decode;
	int reset;
	int32_t setup;
	StdVideoDecodeH264ReferenceInfo setup_info;
	unsigned deactivate_count;
	int32_t deactivate[H264_DPB_SLOTS];
	unsigned reference_count;
	int32_t references[H264_DPB_REFERENCES];
	StdVideoDecodeH264ReferenceInfo info[H264_DPB_REFERENCES];
};

void h264_dpb_init(struct h264_dpb *dpb, unsigned max_references);
int h264_dpb_prepare(struct h264_dpb *dpb, struct h264_stream *stream, struct h264_picture *picture, struct h264_dpb_plan *plan);
int h264_dpb_mark(struct h264_dpb *dpb, const StdVideoH264SequenceParameterSet *sps, const struct h264_picture *picture, const struct h264_dpb_plan *plan);

#endif
