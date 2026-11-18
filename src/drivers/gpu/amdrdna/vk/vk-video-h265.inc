/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's H.265 side of Vulkan Video (included by vk-video.inc,
 * after its structures): the readers of the H.265 StdVideo records
 * libvulkan sends, the H.265 sets of a parameters object, and the checks
 * of an H.265 decode.
 *
 * Each reader reads the fields in the order libvulkan writes them
 * (userland/desktop/libvulkan/video.c: video_encode_h265_vps,
 * video_encode_h265_sps, video_encode_h265_pps and the structures they
 * point to, video_encode_h265_std_picture and
 * video_encode_h265_std_reference) and keeps what the video engine's
 * decode message needs, in the driver's own structures (amdrdna-vcn.h).
 * A field that does not decode marks the reader's error; the caller
 * checks it.  The video parameter sets are kept as their ids only: the
 * decode reads nothing of them.
 */

/*
 * The parameter set id ranges of H.265 (a sequence set is keyed by its
 * video set's id and its own, a picture set by those and its own).
 *
 * Values referenced: Recommendation ITU-T H.265 (7.4.3.1, 7.4.3.2.1 and
 * 7.4.3.3.1, the ranges of vps_video_parameter_set_id,
 * sps_seq_parameter_set_id and pps_pic_parameter_set_id).
 */
#define VIDEO_H265_VPS_IDS			16U
#define VIDEO_H265_SPS_IDS			16U
#define VIDEO_H265_PPS_IDS			64U

/*
 * The sizes of the H.265 structures' fixed arrays as the wire sends them:
 * the sub-layers, the coded picture buffers of a sub-layer's HRD, the
 * short-term reference picture sets of a sequence, its long-term
 * pictures, a short-term set's pictures, the palette's entries and the
 * chroma offset lists.
 *
 * Values referenced: include/libc/vulkan/vk_video/vulkan_video_codec_h265std.h
 * (STD_VIDEO_H265_SUBLAYERS_LIST_SIZE, STD_VIDEO_H265_CPB_CNT_LIST_SIZE,
 * STD_VIDEO_H265_MAX_SHORT_TERM_REF_PIC_SETS,
 * STD_VIDEO_H265_MAX_LONG_TERM_REF_PICS_SPS, STD_VIDEO_H265_MAX_DPB_SIZE,
 * STD_VIDEO_H265_PREDICTOR_PALETTE_COMPONENTS_LIST_SIZE times
 * STD_VIDEO_H265_PREDICTOR_PALETTE_COMP_ENTRIES_LIST_SIZE,
 * STD_VIDEO_H265_CHROMA_QP_OFFSET_LIST_SIZE).
 */
#define VIDEO_H265_SUB_LAYERS			7U
#define VIDEO_H265_CPB_COUNT			32U
#define VIDEO_H265_SHORT_TERM_SETS		64U
#define VIDEO_H265_LONG_TERM_PICS		32U
#define VIDEO_H265_SET_PICTURES			16U
#define VIDEO_H265_PALETTE_ENTRIES		384U
#define VIDEO_H265_CHROMA_OFFSETS		6U

/*
 * The words of an H.265 record the readers pass over: a VPS's reserved
 * and timing words, an SPS's reserved, screen content and window words, a
 * PPS's range extension and screen content words, a profile, tier and
 * level, an HRD's flags and sizes and its reserved words, a VUI's fields,
 * a short-term set's fields.
 */
#define VIDEO_H265_VPS_SKIP			6U
#define VIDEO_H265_SPS_SKIP			10U
#define VIDEO_H265_PPS_RANGE_WORDS		3U
#define VIDEO_H265_PPS_RANGE_TAIL		2U
#define VIDEO_H265_PPS_SCC_WORDS		6U
#define VIDEO_H265_PTL_WORDS			3U
#define VIDEO_H265_HRD_WORDS			13U
#define VIDEO_H265_HRD_RESERVED			3U
#define VIDEO_H265_VUI_WORDS			25U
#define VIDEO_H265_SHORT_TERM_WORDS		12U

static uint32_t video_h265_read_vps(struct i915_wire_reader *reader);
static void video_h265_read_sps(struct i915_wire_reader *reader, struct amdrdna_vcn_h265_sps *sps);
static void video_h265_read_pps(struct i915_wire_reader *reader, struct amdrdna_vcn_h265_pps *pps);
static void video_h265_read_buffering(struct i915_wire_reader *reader, uint32_t sub_layers, uint32_t *buffering);
static void video_h265_read_hrd(struct i915_wire_reader *reader, uint32_t sub_layers);
static void video_h265_read_sub_layers(struct i915_wire_reader *reader, uint32_t sub_layers);
static void video_h265_read_scaling(struct i915_wire_reader *reader, struct amdrdna_vcn_h265_scaling *scaling);
static void video_h265_read_vui(struct i915_wire_reader *reader, uint32_t sub_layers);
static void video_h265_read_picture(struct i915_wire_reader *reader, struct amdrdna_vcn_h265_picture *picture);
static void video_h265_read_reference(struct i915_wire_reader *reader, struct amdrdna_vcn_h265_reference *reference);
static void video_h265_read_add(struct i915_wire_reader *reader, struct video_chain *chain);
static void video_h265_words(struct i915_wire_reader *reader, uint32_t *words, uint32_t expected);
static void video_h265_bytes(struct i915_wire_reader *reader, void *bytes, uint32_t expected);
static void video_h265_skip(struct i915_wire_reader *reader, uint32_t count);
static void video_h265_chain_free(struct video_chain *chain);
static bool video_h265_sizes_fit(const struct video_parameters *parameters, const struct video_chain *chain);
static int video_h265_take(struct video_parameters *parameters, struct video_chain *chain, bool replace);
static int video_h265_copy(struct video_parameters *destination, const struct video_parameters *source);
static void video_h265_free(struct video_parameters *parameters);
static const struct amdrdna_vcn_h265_sps *video_h265_find_sps(const struct video_parameters *parameters, uint32_t vps_id, uint32_t sps_id);
static const struct amdrdna_vcn_h265_pps *video_h265_find_pps(const struct video_parameters *parameters, uint32_t vps_id, uint32_t sps_id, uint32_t pps_id);
static const char *video_h265_check(const struct video_record *video, const struct video_session *state, const struct video_parameters *parameters, struct amdrdna_vcn_h265_decode *h265);
static const char *video_h265_check_sets(const struct amdrdna_vcn_h265_sps *sps, const struct amdrdna_vcn_h265_pps *pps, const struct video_session *state);
static const char *video_h265_current(const uint8_t *slots, const struct amdrdna_vcn_h265_decode *h265, uint8_t *places);

/*
 * Reads one H.265 video parameter set: [flags][id][max sub-layers minus 1]
 * [two reserved words][three timing words][reserved][present]{buffering}
 * [present]{HRD}[present]{profile, tier and level}.  Reports its id; the
 * decode needs nothing else of it.
 */
static uint32_t
video_h265_read_vps(
	struct i915_wire_reader *reader)
{
	uint64_t present;
	uint32_t id;
	uint32_t sub_layers;
	uint32_t buffering;

	/* The flags, the id and the sub-layers. */
	(void)drv_i915_wire_read_u32(reader);
	id = drv_i915_wire_read_u32(reader);
	sub_layers = drv_i915_wire_read_u32(reader) + 1U;
	if (sub_layers > VIDEO_H265_SUB_LAYERS)
		reader->error = 1;

	/* The reserved words and the timing. */
	video_h265_skip(reader, VIDEO_H265_VPS_SKIP);

	/* The buffering limits, the HRD and the profile, tier and level, which the decode does not use. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_h265_read_buffering(reader, sub_layers, &buffering);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_h265_read_hrd(reader, sub_layers);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_h265_skip(reader, VIDEO_H265_PTL_WORDS);

	/* Succeeded: the set's id. */
	return id;
}

/*
 * Reads one H.265 sequence parameter set: its fields in declared order,
 * then the structures it points to.  The decode keeps its key, its
 * format, its sizes, its tools, its reference set counts, its highest
 * sub-layer's DPB size and its own scaling lists.
 */
static void
video_h265_read_sps(
	struct i915_wire_reader *reader,
	struct amdrdna_vcn_h265_sps *sps)
{
	uint64_t present;
	uint64_t count;
	uint64_t item;
	uint32_t sub_layers;

	/* The flags, the sampling and the size. */
	kern_memset(sps, 0, sizeof(*sps));
	sps->flags = drv_i915_wire_read_u32(reader);
	sps->chroma_format_idc = drv_i915_wire_read_u32(reader);
	sps->width = drv_i915_wire_read_u32(reader);
	sps->height = drv_i915_wire_read_u32(reader);

	/* The key, the sub-layers and the depths. */
	sps->vps_id = drv_i915_wire_read_u32(reader);
	sub_layers = drv_i915_wire_read_u32(reader) + 1U;
	if (sub_layers > VIDEO_H265_SUB_LAYERS)
		reader->error = 1;
	sps->max_sub_layers_minus1 = sub_layers - 1U;
	sps->id = drv_i915_wire_read_u32(reader);
	sps->bit_depth_luma_minus8 = drv_i915_wire_read_u32(reader);
	sps->bit_depth_chroma_minus8 = drv_i915_wire_read_u32(reader);

	/* The order count's size and the block sizes. */
	sps->log2_max_pic_order_cnt_lsb_minus4 = drv_i915_wire_read_u32(reader);
	sps->log2_min_luma_coding_block_size_minus3 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_max_min_luma_coding_block_size = drv_i915_wire_read_u32(reader);
	sps->log2_min_luma_transform_block_size_minus2 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_max_min_luma_transform_block_size = drv_i915_wire_read_u32(reader);
	sps->max_transform_hierarchy_depth_inter = drv_i915_wire_read_u32(reader);
	sps->max_transform_hierarchy_depth_intra = drv_i915_wire_read_u32(reader);

	/* The reference picture sets the sequence signals. */
	sps->num_short_term_ref_pic_sets = drv_i915_wire_read_u32(reader);
	sps->num_long_term_ref_pics_sps = drv_i915_wire_read_u32(reader);

	/* The PCM depths and sizes. */
	sps->pcm_sample_bit_depth_luma_minus1 = drv_i915_wire_read_u32(reader);
	sps->pcm_sample_bit_depth_chroma_minus1 = drv_i915_wire_read_u32(reader);
	sps->log2_min_pcm_luma_coding_block_size_minus3 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_max_min_pcm_luma_coding_block_size = drv_i915_wire_read_u32(reader);

	/* Two reserved words, the screen content sizes and the conformance window, which the decode does not use. */
	video_h265_skip(reader, VIDEO_H265_SPS_SKIP);

	/* The profile, tier and level, which the session's profile stands for. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_h265_skip(reader, VIDEO_H265_PTL_WORDS);

	/* The buffering limits: the DPB size of the highest sub-layer. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		sps->has_buffering = true;
		video_h265_read_buffering(reader, sub_layers, &sps->max_dec_pic_buffering_minus1);
	}

	/* The sequence's own scaling lists. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		sps->has_scaling = true;
		video_h265_read_scaling(reader, &sps->scaling);
	}

	/* The short-term sets, as many as the sequence counts, which the engine reads from the stream. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		count = drv_i915_wire_read_u64(reader);
		if (count != sps->num_short_term_ref_pic_sets || count > VIDEO_H265_SHORT_TERM_SETS)
			reader->error = 1;
		for (item = 0U; item < count && reader->error == 0; item++) {
			video_h265_skip(reader, VIDEO_H265_SHORT_TERM_WORDS);
			video_h265_words(reader, NULL, VIDEO_H265_SET_PICTURES);
			video_h265_words(reader, NULL, VIDEO_H265_SET_PICTURES);
		}
	}

	/* The long-term pictures, which the engine reads from the stream. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		video_h265_skip(reader, 1U);
		video_h265_words(reader, NULL, VIDEO_H265_LONG_TERM_PICS);
	}

	/* The VUI and the palette's initializers, which the decode does not use. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_h265_read_vui(reader, sub_layers);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_h265_words(reader, NULL, VIDEO_H265_PALETTE_ENTRIES);
}

/*
 * Reads one H.265 picture parameter set: its fields in declared order,
 * then the structures it points to.  The decode keeps its key, its
 * flags, its counts and offsets, its tiles and its own scaling lists.
 */
static void
video_h265_read_pps(
	struct i915_wire_reader *reader,
	struct amdrdna_vcn_h265_pps *pps)
{
	uint8_t offsets[VIDEO_H265_CHROMA_OFFSETS];
	uint64_t present;

	/* The flags and the key. */
	kern_memset(pps, 0, sizeof(*pps));
	pps->flags = drv_i915_wire_read_u32(reader);
	pps->id = drv_i915_wire_read_u32(reader);
	pps->sps_id = drv_i915_wire_read_u32(reader);
	pps->vps_id = drv_i915_wire_read_u32(reader);

	/* The slice header's extra bits and the default reference counts. */
	pps->num_extra_slice_header_bits = drv_i915_wire_read_u32(reader);
	pps->num_ref_idx_l0_default_active_minus1 = drv_i915_wire_read_u32(reader);
	pps->num_ref_idx_l1_default_active_minus1 = drv_i915_wire_read_u32(reader);

	/* The quantizer, the deblocking offsets and the merge level, the signed ones widened with their sign. */
	pps->init_qp_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->diff_cu_qp_delta_depth = drv_i915_wire_read_u32(reader);
	pps->pps_cb_qp_offset = (int32_t)drv_i915_wire_read_u32(reader);
	pps->pps_cr_qp_offset = (int32_t)drv_i915_wire_read_u32(reader);
	pps->pps_beta_offset_div2 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->pps_tc_offset_div2 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->log2_parallel_merge_level_minus2 = drv_i915_wire_read_u32(reader);

	/* The range extension's fields and chroma offset lists, which Main does not use. */
	video_h265_skip(reader, VIDEO_H265_PPS_RANGE_WORDS);
	video_h265_bytes(reader, offsets, VIDEO_H265_CHROMA_OFFSETS);
	video_h265_bytes(reader, offsets, VIDEO_H265_CHROMA_OFFSETS);
	video_h265_skip(reader, VIDEO_H265_PPS_RANGE_TAIL);

	/* The screen content fields, which Main does not use. */
	video_h265_skip(reader, VIDEO_H265_PPS_SCC_WORDS);

	/* The tiles: their counts, two reserved words, each column's width and row's height minus 1, a reserved word. */
	pps->num_tile_columns_minus1 = drv_i915_wire_read_u32(reader);
	pps->num_tile_rows_minus1 = drv_i915_wire_read_u32(reader);
	video_h265_skip(reader, 2U);
	video_h265_words(reader, pps->column_width_minus1, AMDRDNA_VCN_H265_COLUMN_WIDTHS);
	video_h265_words(reader, pps->row_height_minus1, AMDRDNA_VCN_H265_ROW_HEIGHTS);
	video_h265_skip(reader, 1U);

	/* The picture's own scaling lists. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		pps->has_scaling = true;
		video_h265_read_scaling(reader, &pps->scaling);
	}

	/* The palette's initializers, which Main does not use. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_h265_words(reader, NULL, VIDEO_H265_PALETTE_ENTRIES);
}

/*
 * Reads H.265 buffering limits: [7]{latency}[7 bytes]{buffering minus 1}
 * [7 bytes]{reordering}; *buffering gets the highest of sub_layers
 * sub-layers' DPB size minus 1.
 */
static void
video_h265_read_buffering(
	struct i915_wire_reader *reader,
	uint32_t sub_layers,
	uint32_t *buffering)
{
	uint8_t sizes[VIDEO_H265_SUB_LAYERS];
	uint8_t reordering[VIDEO_H265_SUB_LAYERS];

	/* The latency words, then the two byte arrays. */
	kern_memset(sizes, 0, sizeof(sizes));
	video_h265_words(reader, NULL, VIDEO_H265_SUB_LAYERS);
	video_h265_bytes(reader, sizes, VIDEO_H265_SUB_LAYERS);
	video_h265_bytes(reader, reordering, VIDEO_H265_SUB_LAYERS);

	/* The highest sub-layer's DPB size (a malformed count is the reader's error). */
	*buffering = 0U;
	if (sub_layers >= 1U && sub_layers <= VIDEO_H265_SUB_LAYERS)
		*buffering = sizes[sub_layers - 1U];
}

/*
 * Reads H.265 HRD parameters: the one-bit flags' word and the three
 * per-sub-layer masks, nine lengths and scales, the buffer counts, the
 * durations, three reserved words, then the NAL and VCL sub-layer arrays
 * of sub_layers entries.
 */
static void
video_h265_read_hrd(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint8_t counts[VIDEO_H265_SUB_LAYERS];
	uint64_t present;

	/* The flags and the masks, the lengths and the scales. */
	video_h265_skip(reader, VIDEO_H265_HRD_WORDS);

	/* The buffer counts, the durations and the reserved words. */
	video_h265_bytes(reader, counts, VIDEO_H265_SUB_LAYERS);
	video_h265_words(reader, NULL, VIDEO_H265_SUB_LAYERS);
	video_h265_words(reader, NULL, VIDEO_H265_HRD_RESERVED);

	/* The NAL and the VCL sub-layers. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_h265_read_sub_layers(reader, sub_layers);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_h265_read_sub_layers(reader, sub_layers);
}

/* Reads an array of H.265 sub-layer HRD parameters: [count]{four arrays of 32 words and the CBR mask}. */
static void
video_h265_read_sub_layers(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint64_t count;
	uint64_t item;

	/* One entry for each sub-layer. */
	count = drv_i915_wire_read_u64(reader);
	if (count != sub_layers)
		reader->error = 1;

	/* Each entry's rates and sizes, then its CBR mask. */
	for (item = 0U; item < count && reader->error == 0; item++) {
		video_h265_words(reader, NULL, VIDEO_H265_CPB_COUNT);
		video_h265_words(reader, NULL, VIDEO_H265_CPB_COUNT);
		video_h265_words(reader, NULL, VIDEO_H265_CPB_COUNT);
		video_h265_words(reader, NULL, VIDEO_H265_CPB_COUNT);
		video_h265_skip(reader, 1U);
	}
}

/* Reads H.265 scaling lists: the 4x4, 8x8, 16x16 and 32x32 lists and the 16x16 and 32x32 DC coefficients, each a byte array. */
static void
video_h265_read_scaling(
	struct i915_wire_reader *reader,
	struct amdrdna_vcn_h265_scaling *scaling)
{
	/* The lists, list after list, then the DC coefficients. */
	video_h265_bytes(reader, scaling->list4, sizeof(scaling->list4));
	video_h265_bytes(reader, scaling->list8, sizeof(scaling->list8));
	video_h265_bytes(reader, scaling->list16, sizeof(scaling->list16));
	video_h265_bytes(reader, scaling->list32, sizeof(scaling->list32));
	video_h265_bytes(reader, scaling->dc16, sizeof(scaling->dc16));
	video_h265_bytes(reader, scaling->dc32, sizeof(scaling->dc32));
}

/* Reads an H.265 VUI: 25 words, then its HRD of sub_layers. */
static void
video_h265_read_vui(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint64_t present;

	/* The flags, the aspect, the signal, the window, the timing and the restrictions. */
	video_h265_skip(reader, VIDEO_H265_VUI_WORDS);

	/* The HRD. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_h265_read_hrd(reader, sub_layers);
}

/*
 * Reads the H.265 information of the picture a decode writes: [flags]
 * [vps][sps][pps][NumDeltaPocsOfRefRpsIdx][PicOrderCntVal]
 * [NumBitsForSTRefPicSetInSlice][reserved] and the three sets of eight
 * slots.
 */
static void
video_h265_read_picture(
	struct i915_wire_reader *reader,
	struct amdrdna_vcn_h265_picture *picture)
{
	/* The flags and the sets the picture names. */
	kern_memset(picture, 0, sizeof(*picture));
	picture->flags = drv_i915_wire_read_u32(reader);
	picture->vps_id = drv_i915_wire_read_u32(reader);
	picture->sps_id = drv_i915_wire_read_u32(reader);
	picture->pps_id = drv_i915_wire_read_u32(reader);

	/* The slice header's own short-term set's size, the order count, its size in bits, a reserved word. */
	picture->num_delta_pocs_of_ref_rps_idx = drv_i915_wire_read_u32(reader);
	picture->poc = (int32_t)drv_i915_wire_read_u32(reader);
	picture->num_bits_for_st_ref_pic_set_in_slice = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The slots of the three current sets. */
	video_h265_bytes(reader, picture->before, AMDRDNA_VCN_H265_SET_ENTRIES);
	video_h265_bytes(reader, picture->after, AMDRDNA_VCN_H265_SET_ENTRIES);
	video_h265_bytes(reader, picture->long_term, AMDRDNA_VCN_H265_SET_ENTRIES);
}

/* Reads the H.265 information of a reference picture: [flags][PicOrderCntVal].  Its tag is the caller's. */
static void
video_h265_read_reference(
	struct i915_wire_reader *reader,
	struct amdrdna_vcn_h265_reference *reference)
{
	/* The flags and the order count. */
	kern_memset(reference, 0, sizeof(*reference));
	reference->flags = drv_i915_wire_read_u32(reader);
	reference->poc = (int32_t)drv_i915_wire_read_u32(reader);
}

/*
 * Reads an H.265 add record's fields: [count][array]{VPS}[count][array]
 * {SPS}[count][array]{PPS}.  The video sets are kept as their ids, the
 * others as new allocations on the chain; a set the executor cannot keep
 * marks add_error.
 */
static void
video_h265_read_add(
	struct i915_wire_reader *reader,
	struct video_chain *chain)
{
	struct video_h265_sps_node scratch_sps;
	struct video_h265_pps_node scratch_pps;
	struct video_h265_sps_node *sps;
	struct video_h265_pps_node *pps;
	uint64_t count;
	uint64_t item;
	uint32_t id;

	/* The video parameter sets: only their ids are kept. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < count && reader->error == 0; item++) {
		/* The set's id; one outside H.265 marks the add malformed. */
		id = video_h265_read_vps(reader);
		if (id >= VIDEO_H265_VPS_IDS) {
			chain->add_error = EINVAL;
			continue;
		}

		/* The id joins the chain's. */
		chain->new_h265_vps |= 1U << id;
	}

	/* The sequence parameter sets. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < count && reader->error == 0; item++) {
		/* A set that cannot be allocated is read into scratch. */
		sps = kern_calloc(1U, sizeof(*sps));
		if (sps == NULL) {
			sps = &scratch_sps;
			chain->add_error = ENOMEM;
		}

		/* The set, kept on the chain; ids outside H.265 mark the add malformed. */
		video_h265_read_sps(reader, &sps->sps);
		if (sps->sps.vps_id >= VIDEO_H265_VPS_IDS || sps->sps.id >= VIDEO_H265_SPS_IDS)
			chain->add_error = EINVAL;
		if (sps != &scratch_sps) {
			sps->next = chain->new_h265_sps;
			chain->new_h265_sps = sps;
		}
	}

	/* The picture parameter sets. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < count && reader->error == 0; item++) {
		/* A set that cannot be allocated is read into scratch. */
		pps = kern_calloc(1U, sizeof(*pps));
		if (pps == NULL) {
			pps = &scratch_pps;
			chain->add_error = ENOMEM;
		}

		/* The set, kept on the chain; ids outside H.265 mark the add malformed. */
		video_h265_read_pps(reader, &pps->pps);
		if (pps->pps.vps_id >= VIDEO_H265_VPS_IDS ||
		    pps->pps.sps_id >= VIDEO_H265_SPS_IDS ||
		    pps->pps.id >= VIDEO_H265_PPS_IDS)
			chain->add_error = EINVAL;
		if (pps != &scratch_pps) {
			pps->next = chain->new_h265_pps;
			chain->new_h265_pps = pps;
		}
	}
}

/* Reads an array of words that must have expected elements: [count][words]; into words when it is not NULL. */
static void
video_h265_words(
	struct i915_wire_reader *reader,
	uint32_t *words,
	uint32_t expected)
{
	uint64_t count;
	uint32_t index;
	uint32_t value;

	/* The count must be the array's. */
	count = drv_i915_wire_read_u64(reader);
	if (count != expected) {
		reader->error = 1;
		return;
	}

	/* Each word. */
	for (index = 0U; index < expected; index++) {
		value = drv_i915_wire_read_u32(reader);
		if (words != NULL)
			words[index] = value;
	}
}

/* Reads a byte array that must have expected bytes: [count][bytes]. */
static void
video_h265_bytes(
	struct i915_wire_reader *reader,
	void *bytes,
	uint32_t expected)
{
	uint64_t count;

	/* The count must be the array's. */
	count = drv_i915_wire_read_u64(reader);
	if (count != expected) {
		reader->error = 1;
		return;
	}

	/* The bytes. */
	i915_vkc_read_bytes(reader, bytes, expected);
}

/* Reads and drops count words the decode does not use. */
static void
video_h265_skip(
	struct i915_wire_reader *reader,
	uint32_t count)
{
	uint32_t index;

	/* Each word. */
	for (index = 0U; index < count; index++)
		(void)drv_i915_wire_read_u32(reader);
}

/* Frees the H.265 sets a chain read and nobody took. */
static void
video_h265_chain_free(
	struct video_chain *chain)
{
	struct video_h265_sps_node *sps;
	struct video_h265_pps_node *pps;

	/* The sequence sets. */
	while (chain->new_h265_sps != NULL) {
		sps = chain->new_h265_sps;
		chain->new_h265_sps = sps->next;
		kern_free(sps);
	}

	/* The picture sets. */
	while (chain->new_h265_pps != NULL) {
		pps = chain->new_h265_pps;
		chain->new_h265_pps = pps->next;
		kern_free(pps);
	}
}

/*
 * Reports whether every new H.265 sequence set of a chain fits its
 * parameters object's session: its picture no wider and no taller than
 * the session's largest picture on 16-sample boundaries.
 */
static bool
video_h265_sizes_fit(
	const struct video_parameters *parameters,
	const struct video_chain *chain)
{
	const struct video_session *state;
	const struct video_h265_sps_node *sps;

	/* The session's largest picture. */
	state = parameters->session->payload;

	/* Each new set's picture. */
	for (sps = chain->new_h265_sps; sps != NULL; sps = sps->next) {
		if (sps->sps.width > video_mbs(state->width) * VIDEO_MB || sps->sps.height > video_mbs(state->height) * VIDEO_MB)
			return false;
	}

	/* Succeeded: every set fits. */
	return true;
}

/*
 * Moves a chain's new H.265 sets into a parameters object.
 *
 * With replace, a set replaces the object's of the same key (a create);
 * without, a key already held refuses the whole add (an update) and
 * nothing is taken.  The object's capacity bounds the sets.
 */
static int
video_h265_take(
	struct video_parameters *parameters,
	struct video_chain *chain,
	bool replace)
{
	struct video_h265_sps_node *sps;
	struct video_h265_sps_node **sps_link;
	struct video_h265_sps_node *old_sps;
	struct video_h265_pps_node *pps;
	struct video_h265_pps_node **pps_link;
	struct video_h265_pps_node *old_pps;
	const struct amdrdna_vcn_h265_sps *held_sps;
	const struct amdrdna_vcn_h265_pps *held_pps;
	uint32_t vps_count;
	uint32_t sps_count;
	uint32_t pps_count;
	uint32_t id;

	/* An update must not repeat a key the object holds. */
	if (!replace) {
		/* Nor a video set id. */
		if ((parameters->h265_vps & chain->new_h265_vps) != 0U)
			return EINVAL;

		/* Nor a sequence set key. */
		for (sps = chain->new_h265_sps; sps != NULL; sps = sps->next) {
			held_sps = video_h265_find_sps(parameters, sps->sps.vps_id, sps->sps.id);
			if (held_sps != NULL)
				return EINVAL;
		}

		/* Nor a picture set key. */
		for (pps = chain->new_h265_pps; pps != NULL; pps = pps->next) {
			held_pps = video_h265_find_pps(parameters, pps->pps.vps_id, pps->pps.sps_id, pps->pps.id);
			if (held_pps != NULL)
				return EINVAL;
		}
	}

	/* The video sets held and new. */
	vps_count = 0U;
	for (id = 0U; id < VIDEO_H265_VPS_IDS; id++) {
		if (((parameters->h265_vps | chain->new_h265_vps) & (1U << id)) != 0U)
			vps_count++;
	}

	/* The sequence sets held, and the new ones of new keys. */
	sps_count = 0U;
	for (sps = parameters->h265_sps; sps != NULL; sps = sps->next)
		sps_count++;
	for (sps = chain->new_h265_sps; sps != NULL; sps = sps->next) {
		held_sps = video_h265_find_sps(parameters, sps->sps.vps_id, sps->sps.id);
		if (held_sps == NULL)
			sps_count++;
	}

	/* The picture sets held, and the new ones of new keys. */
	pps_count = 0U;
	for (pps = parameters->h265_pps; pps != NULL; pps = pps->next)
		pps_count++;
	for (pps = chain->new_h265_pps; pps != NULL; pps = pps->next) {
		held_pps = video_h265_find_pps(parameters, pps->pps.vps_id, pps->pps.sps_id, pps->pps.id);
		if (held_pps == NULL)
			pps_count++;
	}

	/* Refuses sets beyond the object's capacity. */
	if (vps_count > parameters->max_vps || sps_count > parameters->max_sps || pps_count > parameters->max_pps)
		return EINVAL;

	/* The video sets' ids. */
	parameters->h265_vps |= chain->new_h265_vps;
	chain->new_h265_vps = 0U;

	/* The sequence sets replace any of the same key. */
	while (chain->new_h265_sps != NULL) {
		sps = chain->new_h265_sps;
		chain->new_h265_sps = sps->next;

		/* Drops the sequence sets of the same key the object holds. */
		sps_link = &parameters->h265_sps;
		while (*sps_link != NULL) {
			if ((*sps_link)->sps.vps_id == sps->sps.vps_id && (*sps_link)->sps.id == sps->sps.id) {
				old_sps = *sps_link;
				*sps_link = old_sps->next;
				kern_free(old_sps);
				continue;
			}

			/* Moves on to the next sequence set. */
			sps_link = &(*sps_link)->next;
		}

		/* The set joins the object's list. */
		sps->next = parameters->h265_sps;
		parameters->h265_sps = sps;
	}

	/* The picture sets replace any of the same key. */
	while (chain->new_h265_pps != NULL) {
		pps = chain->new_h265_pps;
		chain->new_h265_pps = pps->next;

		/* Drops the picture sets of the same key the object holds. */
		pps_link = &parameters->h265_pps;
		while (*pps_link != NULL) {
			if ((*pps_link)->pps.vps_id == pps->pps.vps_id &&
			    (*pps_link)->pps.sps_id == pps->pps.sps_id &&
			    (*pps_link)->pps.id == pps->pps.id) {
				old_pps = *pps_link;
				*pps_link = old_pps->next;
				kern_free(old_pps);
				continue;
			}

			/* Moves on to the next picture set. */
			pps_link = &(*pps_link)->next;
		}

		/* The set joins the object's list. */
		pps->next = parameters->h265_pps;
		parameters->h265_pps = pps;
	}

	/* Succeeded: the sets are the object's. */
	return 0;
}

/* Copies a template's H.265 sets into a new parameters object. */
static int
video_h265_copy(
	struct video_parameters *destination,
	const struct video_parameters *source)
{
	const struct video_h265_sps_node *sps;
	const struct video_h265_pps_node *pps;
	struct video_h265_sps_node *sps_copy;
	struct video_h265_pps_node *pps_copy;

	/* The video sets' ids. */
	destination->h265_vps = source->h265_vps;

	/* The sequence sets. */
	for (sps = source->h265_sps; sps != NULL; sps = sps->next) {
		sps_copy = kern_calloc(1U, sizeof(*sps_copy));
		if (sps_copy == NULL)
			return ENOMEM;
		sps_copy->sps = sps->sps;
		sps_copy->next = destination->h265_sps;
		destination->h265_sps = sps_copy;
	}

	/* The picture sets. */
	for (pps = source->h265_pps; pps != NULL; pps = pps->next) {
		pps_copy = kern_calloc(1U, sizeof(*pps_copy));
		if (pps_copy == NULL)
			return ENOMEM;
		pps_copy->pps = pps->pps;
		pps_copy->next = destination->h265_pps;
		destination->h265_pps = pps_copy;
	}

	/* Succeeded: the template's sets are copied. */
	return 0;
}

/* Frees a parameters object's H.265 sets. */
static void
video_h265_free(
	struct video_parameters *parameters)
{
	struct video_h265_sps_node *sps;
	struct video_h265_pps_node *pps;

	/* The sequence sets. */
	while (parameters->h265_sps != NULL) {
		sps = parameters->h265_sps;
		parameters->h265_sps = sps->next;
		kern_free(sps);
	}

	/* The picture sets. */
	while (parameters->h265_pps != NULL) {
		pps = parameters->h265_pps;
		parameters->h265_pps = pps->next;
		kern_free(pps);
	}
	parameters->h265_vps = 0U;
}

/* Finds the H.265 sequence set of a key in a parameters object. */
static const struct amdrdna_vcn_h265_sps *
video_h265_find_sps(
	const struct video_parameters *parameters,
	uint32_t vps_id,
	uint32_t sps_id)
{
	const struct video_h265_sps_node *sps;

	/* Walks the list for the key. */
	for (sps = parameters->h265_sps; sps != NULL; sps = sps->next) {
		if (sps->sps.vps_id == vps_id && sps->sps.id == sps_id)
			return &sps->sps;
	}

	/* No set of the key. */
	return NULL;
}

/* Finds the H.265 picture set of a key in a parameters object. */
static const struct amdrdna_vcn_h265_pps *
video_h265_find_pps(
	const struct video_parameters *parameters,
	uint32_t vps_id,
	uint32_t sps_id,
	uint32_t pps_id)
{
	const struct video_h265_pps_node *pps;

	/* Walks the list for the key. */
	for (pps = parameters->h265_pps; pps != NULL; pps = pps->next) {
		if (pps->pps.vps_id == vps_id && pps->pps.sps_id == sps_id && pps->pps.id == pps_id)
			return &pps->pps;
	}

	/* No set of the key. */
	return NULL;
}

/*
 * Checks an H.265 decode against what the engine takes and the
 * session's limits, and fills what the H.265 body is made from.  Returns
 * NULL when it can run, else why the picture is not run.
 */
static const char *
video_h265_check(
	const struct video_record *video,
	const struct video_session *state,
	const struct video_parameters *parameters,
	struct amdrdna_vcn_h265_decode *h265)
{
	const struct amdrdna_vcn_h265_sps *sps;
	const struct amdrdna_vcn_h265_pps *pps;
	const struct video_slot *slot;
	const char *reason;
	uint32_t index;
	uint32_t other;

	/* The picture's information and its sets. */
	if (!video->has_h265_picture)
		return "no H.265 picture information";
	if (parameters == NULL)
		return "no parameter sets";
	sps = video_h265_find_sps(parameters, video->h265_picture.vps_id, video->h265_picture.sps_id);
	pps = video_h265_find_pps(parameters, video->h265_picture.vps_id, video->h265_picture.sps_id, video->h265_picture.pps_id);
	if (sps == NULL || pps == NULL)
		return "a parameter set the session does not hold";

	/* What the sets ask of the engine. */
	reason = video_h265_check_sets(sps, pps, state);
	if (reason != NULL)
		return reason;

	/* The bitstream and the output. */
	reason = video_check_bitstream(video);
	if (reason == NULL)
		reason = video_check_output(video, state, sps->width, sps->height);
	if (reason != NULL)
		return reason;

	/* The picture's tag: its setup slot, or the loose tag without one. */
	h265->sps = sps;
	h265->pps = pps;
	h265->picture = video->h265_picture;
	h265->tag = AMDRDNA_VCN_H265_LOOSE_TAG;
	if (video->has_setup)
		h265->tag = (uint32_t)video->setup.index;

	/* The references: within the session's, each of a slot that holds a picture, no slot twice. */
	if (video->slot_count > state->references)
		return "more references than the session's";
	for (index = 0U; index < video->slot_count; index++) {
		/* A slot of the session's that holds a picture, with its H.265 information. */
		slot = &video->slots[index];
		if (slot->index < 0 || slot->index >= (int32_t)state->slots)
			return "a reference outside the slots";
		if (!slot->has_h265_reference)
			return "a reference without its H.265 information";
		if (!state->slot_valid[slot->index])
			return "a reference to a slot that holds no picture";

		/* A slot named once. */
		for (other = 0U; other < index; other++) {
			if (video->slots[other].index == slot->index)
				return "a slot named twice among the references";
		}

		/* The reference under its slot's tag. */
		h265->references[index] = slot->h265_reference;
		h265->references[index].tag = (uint32_t)slot->index;
	}
	h265->reference_count = video->slot_count;

	/* The three current sets, as places in the references. */
	reason = video_h265_current(video->h265_picture.before, h265, h265->before);
	if (reason == NULL)
		reason = video_h265_current(video->h265_picture.after, h265, h265->after);
	if (reason == NULL)
		reason = video_h265_current(video->h265_picture.long_term, h265, h265->long_term);
	if (reason != NULL)
		return reason;

	/* Succeeded: the decode can run. */
	return NULL;
}

/*
 * Checks a picture's H.265 sets: an 8-bit 4:2:0 picture without the
 * tools of the range and screen content extensions, with its buffering
 * limits, within the session's largest picture, whose tiles the engine
 * takes.  Returns NULL when the engine takes them, else why not.
 */
static const char *
video_h265_check_sets(
	const struct amdrdna_vcn_h265_sps *sps,
	const struct amdrdna_vcn_h265_pps *pps,
	const struct video_session *state)
{
	uint32_t columns[AMDRDNA_VCN_H265_TILE_COLUMNS];
	uint32_t rows[AMDRDNA_VCN_H265_TILE_ROWS];
	int error;

	/* An 8-bit 4:2:0 picture of Main's tools. */
	if (sps->chroma_format_idc != 1U ||
	    sps->bit_depth_luma_minus8 != 0U ||
	    sps->bit_depth_chroma_minus8 != 0U ||
	    (sps->flags & AMDRDNA_VCN_H265_SPS_SEPARATE_PLANES) != 0U)
		return "not 8-bit 4:2:0";
	if ((sps->flags & AMDRDNA_VCN_H265_SPS_EXTENSION_TOOLS) != 0U || (pps->flags & AMDRDNA_VCN_H265_PPS_EXTENSION_TOOLS) != 0U)
		return "a tool of the range or screen content extensions";
	if (!sps->has_buffering)
		return "a sequence set without its buffering limits";

	/* Within the session's largest picture. */
	if (sps->width == 0U || sps->height == 0U)
		return "a picture of no size";
	if (sps->width > video_mbs(state->width) * VIDEO_MB || sps->height > video_mbs(state->height) * VIDEO_MB)
		return "a picture larger than the session's";

	/* Tiles the picture and the message hold. */
	error = amdrdna_vcn_h265_tiles(sps, pps, columns, rows);
	if (error != 0)
		return "tiles the engine does not take";

	/* Succeeded: the engine takes the sets. */
	return NULL;
}

/*
 * Turns one of a picture's current sets (slots, the no-reference mark
 * past the last) into places in its references.  Returns NULL, or why an
 * entry cannot be placed.
 */
static const char *
video_h265_current(
	const uint8_t *slots,
	const struct amdrdna_vcn_h265_decode *h265,
	uint8_t *places)
{
	uint32_t entry;
	uint32_t index;

	/* Each entry: none stays none, a slot becomes its reference's place. */
	for (entry = 0U; entry < AMDRDNA_VCN_H265_SET_ENTRIES; entry++) {
		/* No reference. */
		places[entry] = AMDRDNA_VCN_H265_NO_REFERENCE;
		if (slots[entry] == AMDRDNA_VCN_H265_NO_REFERENCE)
			continue;

		/* The reference of the slot. */
		for (index = 0U; index < h265->reference_count; index++) {
			if (h265->references[index].tag == slots[entry]) {
				places[entry] = (uint8_t)index;
				break;
			}
		}

		/* A slot that is none of the references. */
		if (places[entry] == AMDRDNA_VCN_H265_NO_REFERENCE)
			return "a current reference set names a slot that is not a reference";
	}

	/* Succeeded: every entry is placed. */
	return NULL;
}
