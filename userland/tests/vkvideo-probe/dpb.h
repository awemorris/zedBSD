/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoded picture buffer of vkvideo-probe (ws083-p006a).
 *
 * Vulkan Video leaves the DPB to the application: which pictures are
 * references, in which slots, and which slot each new picture is written
 * into.  The probe gives every slot its own picture (slot n is image n),
 * keeps H.264's reference marking (8.2.5: the sliding window, the memory
 * management operations 1 to 6 and the frames a gap in frame_num implies,
 * for frames), and plans each decode:
 * the references and their StdVideoDecodeH264ReferenceInfo, a free slot
 * for the new picture, and the slots the decoder still holds that no
 * longer hold a reference (deactivated in the coding scope's begin).
 */

#ifndef VKVIDEO_PROBE_DPB_H
#define VKVIDEO_PROBE_DPB_H

#include "h264.h"

#include <stdint.h>

/* The most slots: sixteen references and the picture being decoded. */
#define DPB_SLOTS		17U
#define DPB_REFERENCES		16U

/* What a slot holds for H.264. */
#define DPB_UNUSED		0
#define DPB_SHORT		1
#define DPB_LONG		2

/*
 * One slot: its reference marking, whether it is a frame a gap in
 * frame_num implies (it has no picture, is never a reference given to the
 * decoder, and only takes its place in the marking), its numbers and order
 * counts, and whether the decoder holds its picture.
 */
struct dpb_entry {
	int reference;
	int missing;
	uint32_t frame_num;
	uint32_t long_term_frame_idx;
	int32_t poc[2];
	int device_active;
};

/*
 * The DPB of a decode session: the slots it uses (the sequence's
 * max_num_ref_frames and one), the largest long-term index (-1 for none),
 * the last reference picture's frame_num, and whether the decoder was reset.
 */
struct dpb {
	uint32_t slots;
	uint32_t max_references;
	int32_t max_long_term_frame_idx;
	uint32_t previous_reference_frame_num;
	int started;
	struct dpb_entry entry[DPB_SLOTS];
};

/* One decode's plan: a reset, the slot written, the slots deactivated, and the references. */
struct dpb_plan {
	int reset;
	int32_t setup;
	StdVideoDecodeH264ReferenceInfo setup_info;
	uint32_t deactivate_count;
	int32_t deactivate[DPB_SLOTS];
	uint32_t reference_count;
	int32_t references[DPB_REFERENCES];
	StdVideoDecodeH264ReferenceInfo info[DPB_REFERENCES];
};

void dpb_init(struct dpb *dpb, uint32_t max_references);
const char *dpb_plan(struct dpb *dpb, const StdVideoH264SequenceParameterSet *sps, const struct h264_picture *picture, struct dpb_plan *plan);
const char *dpb_mark(struct dpb *dpb, const StdVideoH264SequenceParameterSet *sps, const struct h264_picture *picture, const struct dpb_plan *plan);

#endif
