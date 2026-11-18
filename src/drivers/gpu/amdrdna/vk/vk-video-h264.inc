/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's readers of the H.264 StdVideo records libvulkan sends
 * (included by vk-video.inc): the sequence and picture parameter sets,
 * their scaling lists, a picture's information and a reference's.  Each
 * reads the fields in the order libvulkan writes them
 * (userland/desktop/libvulkan/video.c: video_encode_sps, video_encode_pps,
 * video_encode_scaling and the picture and reference encoders) and keeps
 * what the video engine's decode message needs, in the driver's own
 * structures (amdrdna-vcn.h).  A field that does not decode marks the
 * reader's error; the caller checks it.
 */

/* The most entries of a sequence set's order count cycle (num_ref_frames_in_pic_order_cnt_cycle). */
#define VIDEO_H264_CYCLE_MAX		255U

static void video_h264_read_scaling(struct i915_wire_reader *reader, struct amdrdna_vcn_h264_scaling *scaling);

/*
 * Reads a sequence parameter set: [flags][profile_idc][level_idc]
 * [chroma_format_idc][id][two bit depths][log2_max_frame_num_minus4]
 * [pic_order_cnt_type][two order count offsets]
 * [log2_max_pic_order_cnt_lsb_minus4][cycle length][max_num_ref_frames]
 * [reserved][width][height][four crop offsets][reserved][count][cycle]
 * [present]{scaling lists}[present]{VUI, never sent}.
 */
static void
video_h264_read_sps(
	struct i915_wire_reader *reader,
	struct amdrdna_vcn_h264_sps *sps)
{
	uint64_t offsets;
	uint64_t present;
	uint32_t index;

	/* The flags, the profile, the level, the chroma format and the id. */
	kern_memset(sps, 0, sizeof(*sps));
	sps->flags = drv_i915_wire_read_u32(reader);
	sps->profile_idc = drv_i915_wire_read_u32(reader);
	sps->level_idc = drv_i915_wire_read_u32(reader);
	sps->chroma_format_idc = drv_i915_wire_read_u32(reader);
	sps->id = drv_i915_wire_read_u32(reader);

	/* The bit depths, the frame number's size and the order count's type. */
	sps->bit_depth_luma_minus8 = drv_i915_wire_read_u32(reader);
	sps->bit_depth_chroma_minus8 = drv_i915_wire_read_u32(reader);
	sps->log2_max_frame_num_minus4 = drv_i915_wire_read_u32(reader);
	sps->pic_order_cnt_type = drv_i915_wire_read_u32(reader);

	/* The two order count offsets the engine does not take, then the order count's size. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	sps->log2_max_pic_order_cnt_lsb_minus4 = drv_i915_wire_read_u32(reader);

	/* The cycle's length (sent again with the cycle), the reference frames, a reserved word. */
	(void)drv_i915_wire_read_u32(reader);
	sps->max_num_ref_frames = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The size in macroblocks and map units. */
	sps->pic_width_in_mbs_minus1 = drv_i915_wire_read_u32(reader);
	sps->pic_height_in_map_units_minus1 = drv_i915_wire_read_u32(reader);

	/* The four crop offsets and a reserved word, which the engine does not take. */
	for (index = 0U; index < 5U; index++)
		(void)drv_i915_wire_read_u32(reader);

	/* The order count cycle's offsets, which the engine does not take. */
	offsets = drv_i915_wire_read_u64(reader);
	if (offsets > VIDEO_H264_CYCLE_MAX)
		reader->error = 1;
	for (index = 0U; index < (uint32_t)offsets && reader->error == 0; index++)
		(void)drv_i915_wire_read_u32(reader);

	/* The scaling lists. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		sps->has_scaling = true;
		video_h264_read_scaling(reader, &sps->scaling);
	}

	/* The VUI, which libvulkan never sends. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		reader->error = 1;
}

/*
 * Reads a picture parameter set: [flags][sps id][pps id][two default
 * active counts][weighted_bipred_idc][pic_init_qp_minus26]
 * [pic_init_qs_minus26][two chroma offsets][present]{scaling lists}.
 */
static void
video_h264_read_pps(
	struct i915_wire_reader *reader,
	struct amdrdna_vcn_h264_pps *pps)
{
	uint64_t present;

	/* The flags and the two ids. */
	kern_memset(pps, 0, sizeof(*pps));
	pps->flags = drv_i915_wire_read_u32(reader);
	pps->sps_id = drv_i915_wire_read_u32(reader);
	pps->pps_id = drv_i915_wire_read_u32(reader);

	/* The default active references and the weighted bi-prediction. */
	pps->num_ref_idx_l0_default_active_minus1 = drv_i915_wire_read_u32(reader);
	pps->num_ref_idx_l1_default_active_minus1 = drv_i915_wire_read_u32(reader);
	pps->weighted_bipred_idc = drv_i915_wire_read_u32(reader);

	/* The quantizers, signed. */
	pps->pic_init_qp_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->pic_init_qs_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->chroma_qp_index_offset = (int32_t)drv_i915_wire_read_u32(reader);
	pps->second_chroma_qp_index_offset = (int32_t)drv_i915_wire_read_u32(reader);

	/* The scaling lists. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		pps->has_scaling = true;
		video_h264_read_scaling(reader, &pps->scaling);
	}
}

/*
 * Reads the H.264 information of the picture a decode writes: [flags]
 * [sps id][pps id][two reserved words][frame_num][idr_pic_id][count 2]
 * [two order counts].
 */
static void
video_h264_read_picture(
	struct i915_wire_reader *reader,
	struct amdrdna_vcn_h264_picture *picture)
{
	uint64_t count;

	/* The flags and the sets the picture names. */
	kern_memset(picture, 0, sizeof(*picture));
	picture->flags = drv_i915_wire_read_u32(reader);
	picture->sps_id = drv_i915_wire_read_u32(reader);
	picture->pps_id = drv_i915_wire_read_u32(reader);

	/* Two reserved words, the frame number, and the IDR picture's id the engine reads from the slices. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	picture->frame_num = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The two order counts. */
	count = drv_i915_wire_read_u64(reader);
	if (count != 2U)
		reader->error = 1;
	picture->poc[0] = (int32_t)drv_i915_wire_read_u32(reader);
	picture->poc[1] = (int32_t)drv_i915_wire_read_u32(reader);
}

/*
 * Reads the H.264 information of a reference picture: [flags][FrameNum]
 * [reserved][count 2][two order counts].  Its tag is the caller's.
 */
static void
video_h264_read_reference(
	struct i915_wire_reader *reader,
	struct amdrdna_vcn_h264_reference *reference)
{
	uint64_t count;

	/* The flags and the frame number (or long-term index). */
	kern_memset(reference, 0, sizeof(*reference));
	reference->flags = drv_i915_wire_read_u32(reader);
	reference->frame_num = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The two order counts. */
	count = drv_i915_wire_read_u64(reader);
	if (count != 2U)
		reader->error = 1;
	reference->poc[0] = (int32_t)drv_i915_wire_read_u32(reader);
	reference->poc[1] = (int32_t)drv_i915_wire_read_u32(reader);
}

/* Reads H.264 scaling lists: [present mask][default mask][96]{4x4}[384]{8x8}. */
static void
video_h264_read_scaling(
	struct i915_wire_reader *reader,
	struct amdrdna_vcn_h264_scaling *scaling)
{
	uint64_t count;

	/* The two masks. */
	scaling->present_mask = drv_i915_wire_read_u32(reader);
	scaling->default_mask = drv_i915_wire_read_u32(reader);

	/* The six 4x4 lists, then the six 8x8 lists, each array whole. */
	count = drv_i915_wire_read_u64(reader);
	if (count != sizeof(scaling->list4))
		reader->error = 1;
	i915_vkc_read_bytes(reader, scaling->list4, sizeof(scaling->list4));
	count = drv_i915_wire_read_u64(reader);
	if (count != sizeof(scaling->list8))
		reader->error = 1;
	i915_vkc_read_bytes(reader, scaling->list8, sizeof(scaling->list8));
}
