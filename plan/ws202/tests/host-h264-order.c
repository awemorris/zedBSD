/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Independent metadata vectors exercise ITU-T H.264 order and missing-reference rules, without GPU execution. */
#include "userland/desktop/libmedia/h264-dpb.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void host_picture(struct h264_picture *picture, unsigned frame, unsigned poc, int reference);
static void host_order(struct h264_stream *stream, struct h264_picture *picture);
static void host_lists(struct h264_stream *stream, struct h264_picture *picture);

/*
 * Checks hand-calculated picture order, MMCO-5 reset and per-slice reference admission.
 */
int
main(
	void)
{
	struct h264_stream *stream;
	struct h264_picture *picture;

	/* Large parser and slice metadata live on the heap as they do in the native decoder. */
	stream = calloc(1U, sizeof(*stream));
	assert(stream != NULL);
	picture = calloc(1U, sizeof(*picture));
	assert(picture != NULL);
	host_order(stream, picture);
	host_lists(stream, picture);
	free(picture);
	free(stream);
	puts("H264 order/list PASS (types 0/1/2, wrap, MMCO-5, gaps, every slice)");
	return 0;
}

/* Initialize only transmitted picture metadata; order counts remain production outputs. */
static void
host_picture(
	struct h264_picture *picture,
	unsigned frame,
	unsigned poc,
	int reference)
{
	/* One progressive P picture requests a single short-term reference by default. */
	memset(picture, 0, sizeof(*picture));
	picture->info.frame_num = (uint16_t)frame;
	picture->info.flags.is_reference = reference;
	picture->poc_lsb = poc;
	picture->slice_count = 1U;
	picture->slice_type = H264_SLICE_P;
	picture->slices[0].type = H264_SLICE_P;
	picture->slices[0].list[0].active = 1U;
}

/* Compare production POC outputs with equations evaluated independently for small syntax values. */
static void
host_order(
	struct h264_stream *stream,
	struct h264_picture *picture)
{
	StdVideoH264SequenceParameterSet *sps;
	int32_t offsets[2];
	int error;

	/* Type zero wraps at half the 16-count range and MMCO-5 preserves current decode order. */
	memset(stream, 0, sizeof(*stream));
	sps = &stream->sps[0];
	sps->pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
	host_picture(picture, 0U, 0U, 1);
	picture->info.flags.IdrPicFlag = 1;
	error = h264_picture_order(stream, sps, picture);
	assert(error == 0 && picture->info.PicOrderCnt[0] == 0);
	host_picture(picture, 1U, 7U, 1);
	error = h264_picture_order(stream, sps, picture);
	assert(error == 0 && picture->info.PicOrderCnt[0] == 7);
	host_picture(picture, 2U, 14U, 1);
	error = h264_picture_order(stream, sps, picture);
	assert(error == 0 && picture->info.PicOrderCnt[0] == 14);
	host_picture(picture, 3U, 2U, 1);
	picture->mmco5 = 1;
	picture->poc_delta[1] = -2;
	error = h264_picture_order(stream, sps, picture);
	assert(error == 0 && picture->info.PicOrderCnt[0] == 18);
	assert(picture->info.PicOrderCnt[1] == 16);
	assert(stream->previous_lsb == 2 && stream->previous_msb == 0);
	assert(stream->previous_frame_num == 0U);
	host_picture(picture, 1U, 4U, 1);
	error = h264_picture_order(stream, sps, picture);
	assert(error == 0 && picture->info.PicOrderCnt[0] == 4);

	/* Type one uses complete cycles and the previous non-reference picture for frame-number wrap. */
	memset(stream, 0, sizeof(*stream));
	sps = &stream->sps[0];
	sps->pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_1;
	offsets[0] = 2;
	offsets[1] = 3;
	sps->pOffsetForRefFrame = offsets;
	sps->num_ref_frames_in_pic_order_cnt_cycle = 2U;
	sps->offset_for_non_ref_pic = -1;
	sps->offset_for_top_to_bottom_field = 1;
	host_picture(picture, 3U, 0U, 1);
	error = h264_picture_order(stream, sps, picture);
	assert(error == 0 && picture->info.PicOrderCnt[0] == 7);
	assert(picture->info.PicOrderCnt[1] == 8);
	host_picture(picture, 15U, 0U, 0);
	error = h264_picture_order(stream, sps, picture);
	assert(error == 0 && picture->info.PicOrderCnt[0] == 34);
	host_picture(picture, 0U, 0U, 1);
	error = h264_picture_order(stream, sps, picture);
	assert(error == 0 && picture->info.PicOrderCnt[0] == 40);

	/* Type two unwraps every picture, including a non-reference frame before zero. */
	memset(stream, 0, sizeof(*stream));
	sps = &stream->sps[0];
	sps->pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2;
	host_picture(picture, 15U, 0U, 0);
	error = h264_picture_order(stream, sps, picture);
	assert(error == 0 && picture->info.PicOrderCnt[0] == 29);
	host_picture(picture, 0U, 0U, 1);
	error = h264_picture_order(stream, sps, picture);
	assert(error == 0 && picture->info.PicOrderCnt[0] == 32);
}

/* Check inferred type-zero exclusion and an explicit missing reference in a subsequent slice. */
static void
host_lists(
	struct h264_stream *stream,
	struct h264_picture *picture)
{
	struct h264_dpb dpb;
	struct h264_dpb_plan plan;
	StdVideoH264SequenceParameterSet *sps;
	unsigned index;
	unsigned inferred;
	int error;

	/* An intra reference at frame zero establishes a real image for subsequent B lists. */
	memset(stream, 0, sizeof(*stream));
	sps = &stream->sps[0];
	sps->max_num_ref_frames = 4U;
	sps->pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
	h264_dpb_init(&dpb, 4U);
	host_picture(picture, 0U, 0U, 1);
	picture->intra = 1;
	picture->info.flags.IdrPicFlag = 1;
	picture->slice_type = H264_SLICE_I;
	picture->slices[0].type = H264_SLICE_I;
	picture->slices[0].list[0].active = 0U;
	error = h264_dpb_prepare(&dpb, stream, picture, &plan);
	assert(error == 0 && plan.decode == 1);
	error = h264_dpb_mark(&dpb, sps, picture, &plan);
	assert(error == 0);

	/* Missing frame numbers one and two have no type-zero POC and cannot enter an initial B list. */
	host_picture(picture, 3U, 2U, 0);
	picture->slice_type = H264_SLICE_B;
	picture->slices[0].type = H264_SLICE_B;
	picture->slices[0].list[1].active = 1U;
	error = h264_dpb_prepare(&dpb, stream, picture, &plan);
	assert(error == 0 && plan.decode == 1);
	assert(plan.reference_count == 1U);
	inferred = 0U;
	for (index = 0U; index < H264_DPB_SLOTS; index++) {
		if (dpb.entry[index].inferred)
			inferred++;
	}

	/* Require both skipped frame numbers to remain logical-only inferred references. */
	assert(inferred == 2U);

	/* Both P slices explicitly select the only real reference before the second selects an inferred one. */
	host_picture(picture, 3U, 4U, 0);
	picture->slice_count = 2U;
	picture->slices[0].list[0].count = 1U;
	picture->slices[0].list[0].operation[0] = 0U;
	picture->slices[0].list[0].argument[0] = 2U;
	picture->slices[1] = picture->slices[0];
	picture->slices[1].first_mb = 1U;
	error = h264_dpb_prepare(&dpb, stream, picture, &plan);
	assert(error == 0 && plan.decode == 1);
	picture->slices[1].list[0].argument[0] = 0U;
	error = h264_dpb_prepare(&dpb, stream, picture, &plan);
	assert(error == 0 && plan.decode == 0);
}
