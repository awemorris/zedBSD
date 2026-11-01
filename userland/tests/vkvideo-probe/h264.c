/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.264 elementary stream reader of vkvideo-probe (see h264.h).
 *
 * The syntax is Recommendation ITU-T H.264's: 7.3.1 (NAL unit), 7.3.2.1.1
 * (sequence parameter set), 7.3.2.1.1.1 (scaling list), 7.3.2.2 (picture
 * parameter set), 7.3.3 (slice header, up to the order count fields), and
 * the order counts of 8.2.1 (8.2.1.1, 8.2.1.2 and 8.2.1.3, with the
 * memory management operation 5's restart).
 */

#include "h264.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The bytes of a NAL unit's payload the reader looks at: every header the probe reads fits. */
#define H264_RBSP_BYTES		4096U

/* The NAL unit types the reader knows. */
#define H264_NAL_SLICE		1U
#define H264_NAL_IDR		5U
#define H264_NAL_SEI		6U
#define H264_NAL_SPS		7U
#define H264_NAL_PPS		8U
#define H264_NAL_DELIMITER	9U

/*
 * The raw bytes of one NAL unit's payload (emulation prevention removed,
 * at most H264_RBSP_BYTES) and a bit cursor over them.  `error` is set by
 * a read past the end or a code too long, and every later read is zero.
 */
struct h264_bits {
	uint8_t bytes[H264_RBSP_BYTES];
	size_t size;
	size_t bit;
	int error;
};

static void h264_bits_load(struct h264_bits *bits, const uint8_t *nal, size_t size);
static uint32_t h264_u(struct h264_bits *bits, unsigned count);
static uint32_t h264_ue(struct h264_bits *bits);
static int32_t h264_se(struct h264_bits *bits);
static int h264_more_data(const struct h264_bits *bits);
static void h264_scaling_list(struct h264_bits *bits, uint8_t *list, unsigned count, int *use_default);
static void h264_scaling_lists(struct h264_bits *bits, unsigned count, StdVideoH264ScalingLists *lists);
static int h264_parse_sps(struct h264_stream *stream, const uint8_t *nal, size_t size);
static int h264_parse_pps(struct h264_stream *stream, const uint8_t *nal, size_t size);
static StdVideoH264LevelIdc h264_level(uint32_t level_idc);
static int h264_high_profile(uint32_t profile_idc);
static int h264_slice_start(const uint8_t *nal, size_t size, uint32_t *first_mb, uint32_t *slice_type);
static const char *h264_slice_header(struct h264_stream *stream, const struct h264_nal *nal, struct h264_picture *picture);
static const char *h264_slice_rest(struct h264_bits *bits, const StdVideoH264PictureParameterSet *pps, const struct h264_nal *nal, uint32_t slice_type, struct h264_picture *picture);
static void h264_list_modification(struct h264_bits *bits);
static void h264_weight_table(struct h264_bits *bits, uint32_t count);
static const char *h264_marking(struct h264_bits *bits, const struct h264_nal *nal, struct h264_picture *picture);
static size_t h264_start_code_of(const uint8_t *data, size_t offset);
static void h264_order_counts(struct h264_stream *stream, const StdVideoH264SequenceParameterSet *sps, const struct h264_nal *nal, uint32_t lsb, const int32_t *deltas, struct h264_picture *picture);
static int32_t h264_expected_order(const StdVideoH264SequenceParameterSet *sps, const struct h264_nal *nal, uint32_t frame_offset, uint32_t frame_num);
static int h264_read_set(struct h264_stream *stream, const struct h264_nal *nal);
static void h264_note_needs(struct h264_stream *stream, const StdVideoH264SequenceParameterSet *sps);
static uint64_t h264_hash(const uint8_t *bytes, size_t size);

/*
 * Opens a stream: reads every sequence and picture parameter set in it for
 * what the whole stream needs (the first sequence set's profile, the
 * largest coded picture, the most references), then rewinds to the first
 * picture with no set in force: the pictures read the sets as they come.
 * Returns -1 when a parameter set cannot be read.
 */
int
h264_open(
	struct h264_stream *stream,
	const uint8_t *data,
	size_t size)
{
	struct h264_nal nal;
	size_t cursor;
	int found;
	int error;

	/* An empty reader over the bytes. */
	memset(stream, 0, sizeof(*stream));
	stream->data = data;
	stream->size = size;

	/* Every parameter set of the stream. */
	cursor = 0U;
	for (;;) {
		/* The next NAL unit, or the end. */
		found = h264_nal_next(data, size, &cursor, &nal);
		if (!found)
			break;

		/* A sequence or a picture parameter set is read; anything else waits for the pictures. */
		error = 0;
		if (nal.type == H264_NAL_SPS)
			error = h264_parse_sps(stream, data + nal.offset, nal.size);
		else if (nal.type == H264_NAL_PPS)
			error = h264_parse_pps(stream, data + nal.offset, nal.size);
		if (error != 0)
			return -1;

		/* What a sequence set needs. */
		if (nal.type == H264_NAL_SPS)
			h264_note_needs(stream, &stream->sps[stream->last_sps]);
	}

	/* Succeeded: the pictures start from the beginning, with no set in force. */
	memset(stream->has_sps, 0, sizeof(stream->has_sps));
	memset(stream->has_pps, 0, sizeof(stream->has_pps));
	memset(stream->sps_unit_size, 0, sizeof(stream->sps_unit_size));
	memset(stream->pps_unit_size, 0, sizeof(stream->pps_unit_size));
	stream->generation = 0U;
	stream->cursor = 0U;
	return 0;
}

/* Adds a sequence set's profile (the first one's), coded size and references to what the stream needs. */
static void
h264_note_needs(
	struct h264_stream *stream,
	const StdVideoH264SequenceParameterSet *sps)
{
	uint32_t width;
	uint32_t height;

	/* The first set's profile. */
	if (!stream->has_profile) {
		stream->has_profile = 1;
		stream->profile_idc = sps->profile_idc;
	}

	/* The largest coded picture and the most references. */
	width = (sps->pic_width_in_mbs_minus1 + 1U) * 16U;
	height = (sps->pic_height_in_map_units_minus1 + 1U) * 16U;
	if (width > stream->max_width)
		stream->max_width = width;
	if (height > stream->max_height)
		stream->max_height = height;
	if (sps->max_num_ref_frames > stream->max_references)
		stream->max_references = sps->max_num_ref_frames;
}

/*
 * Reads a parameter set met between the pictures into the sets in force;
 * the generation goes up when the set is new or its unit differs from the
 * one of its id before.  Returns -1 when the set cannot be read.
 */
static int
h264_read_set(
	struct h264_stream *stream,
	const struct h264_nal *nal)
{
	const uint8_t *unit;
	uint64_t hash;
	uint32_t id;
	int error;

	/* The unit and its hash. */
	unit = stream->data + nal->offset;
	hash = h264_hash(unit, nal->size);

	/* A sequence set, kept in its id's place. */
	if (nal->type == H264_NAL_SPS) {
		error = h264_parse_sps(stream, unit, nal->size);
		if (error != 0)
			return -1;
		id = stream->last_sps;
		if (stream->sps_unit_size[id] != nal->size || stream->sps_unit_hash[id] != hash)
			stream->generation++;
		stream->sps_unit_size[id] = nal->size;
		stream->sps_unit_hash[id] = hash;
		return 0;
	}

	/* A picture set, kept in its id's place. */
	error = h264_parse_pps(stream, unit, nal->size);
	if (error != 0)
		return -1;
	id = stream->last_pps;
	if (stream->pps_unit_size[id] != nal->size || stream->pps_unit_hash[id] != hash)
		stream->generation++;
	stream->pps_unit_size[id] = nal->size;
	stream->pps_unit_hash[id] = hash;

	/* Succeeded: the set is in force. */
	return 0;
}

/* The 64-bit FNV-1a hash of bytes, which tells a set sent again from one with other content. */
static uint64_t
h264_hash(
	const uint8_t *bytes,
	size_t size)
{
	uint64_t hash;
	size_t index;

	/* Byte after byte. */
	hash = 0xcbf29ce484222325ULL;
	for (index = 0U; index < size; index++) {
		hash ^= bytes[index];
		hash *= 0x100000001b3ULL;
	}

	/* Succeeded: the hash. */
	return hash;
}

/*
 * Finds the next NAL unit from a cursor: the bytes after a start code
 * (00 00 01) up to the next start code, less the zero bytes before it (a
 * four-byte start code's first byte, trailing zeros).  Returns 1 with the
 * unit and the cursor after it, 0 at the end of the stream.
 */
int
h264_nal_next(
	const uint8_t *data,
	size_t size,
	size_t *cursor,
	struct h264_nal *nal)
{
	size_t start;
	size_t end;

	/* The next start code. */
	start = *cursor;
	while (start + 3U <= size) {
		if (data[start] == 0U && data[start + 1U] == 0U && data[start + 2U] == 1U)
			break;
		start++;
	}
	if (start + 3U > size)
		return 0;
	start += 3U;

	/* The start code after it, or the stream's end. */
	end = start;
	while (end + 3U <= size) {
		if (data[end] == 0U && data[end + 1U] == 0U && data[end + 2U] == 1U)
			break;
		end++;
	}
	if (end + 3U > size)
		end = size;
	*cursor = end;

	/* The zeros before the next start code are not the unit's. */
	while (end > start && data[end - 1U] == 0U)
		end--;

	/* An empty unit is skipped. */
	if (end == start)
		return h264_nal_next(data, size, cursor, nal);

	/* Succeeded: the unit's header and bytes. */
	nal->type = data[start] & 0x1fU;
	nal->ref_idc = (data[start] >> 5) & 3U;
	nal->offset = start;
	nal->size = end - start;
	return 1;
}

/*
 * Reads the next picture: its slices (every slice NAL unit up to the next
 * one starting a picture at macroblock 0, or the next delimiter or
 * parameter set), its information from its first slice's header, and its
 * order counts.  Returns 1 with a picture, 0 at the end of the stream,
 * and -1 with the reason the probe cannot decode it.
 */
int
h264_next_picture(
	struct h264_stream *stream,
	struct h264_picture *picture,
	const char **reason)
{
	struct h264_nal nal;
	size_t before;
	uint32_t first_mb;
	uint32_t slice_type;
	int found;
	int error;

	/* Nothing yet; the access unit starts at the first unit read. */
	memset(picture, 0, sizeof(*picture));
	picture->intra = 1;
	picture->access_unit = (size_t)-1;
	*reason = NULL;

	/* The slice NAL units of one picture. */
	for (;;) {
		/* The next unit; the cursor stays before one that is the next picture's. */
		before = stream->cursor;
		found = h264_nal_next(stream->data, stream->size, &stream->cursor, &nal);
		if (!found)
			break;

		/* The picture's access unit starts at the start code of its first unit. */
		if (picture->access_unit == (size_t)-1)
			picture->access_unit = h264_start_code_of(stream->data, nal.offset);

		/* A delimiter or a parameter set after slices ends the picture; a set before them is in force from here. */
		if (nal.type != H264_NAL_SLICE && nal.type != H264_NAL_IDR) {
			if (picture->slice_count != 0U &&
			    (nal.type == H264_NAL_DELIMITER || nal.type == H264_NAL_SPS || nal.type == H264_NAL_PPS || nal.type == H264_NAL_SEI)) {
				stream->cursor = before;
				break;
			}
			if (nal.type == H264_NAL_SPS || nal.type == H264_NAL_PPS) {
				error = h264_read_set(stream, &nal);
				if (error != 0) {
					*reason = "a parameter set the probe cannot read";
					return -1;
				}
			}
			continue;
		}

		/* A slice at macroblock 0 starts a picture: the next one when this one has slices. */
		error = h264_slice_start(stream->data + nal.offset, nal.size, &first_mb, &slice_type);
		if (error != 0) {
			*reason = "unreadable slice header";
			return -1;
		}
		if (first_mb == 0U && picture->slice_count != 0U) {
			stream->cursor = before;
			break;
		}

		/* The first slice gives the picture's information. */
		if (picture->slice_count == 0U) {
			*reason = h264_slice_header(stream, &nal, picture);
			if (*reason != NULL)
				return -1;
		}

		/* The slice joins the picture. */
		if (picture->slice_count >= H264_MAX_SLICES) {
			*reason = "more than 256 slices";
			return -1;
		}
		picture->slice_offsets[picture->slice_count] = nal.offset;
		picture->slice_sizes[picture->slice_count] = nal.size;
		picture->slice_count++;

		/* A slice that is not intra makes the picture not intra. */
		if (slice_type != H264_SLICE_I && slice_type != H264_SLICE_SI) {
			picture->intra = 0;
			picture->info.flags.is_intra = 0;
		}
	}

	/* The end of the stream. */
	if (picture->slice_count == 0U)
		return 0;

	/* Succeeded: a picture. */
	return 1;
}

/* Copies a NAL unit's payload into the reader, without its emulation prevention bytes. */
static void
h264_bits_load(
	struct h264_bits *bits,
	const uint8_t *nal,
	size_t size)
{
	size_t index;
	unsigned zeros;

	/* Byte by byte: an 03 after two zeros is dropped. */
	bits->size = 0U;
	bits->bit = 0U;
	bits->error = 0;
	zeros = 0U;
	for (index = 0U; index < size && bits->size < H264_RBSP_BYTES; index++) {
		if (zeros >= 2U && nal[index] == 3U) {
			zeros = 0U;
			continue;
		}
		if (nal[index] == 0U)
			zeros++;
		else
			zeros = 0U;
		bits->bytes[bits->size] = nal[index];
		bits->size++;
	}
}

/* Reads `count` bits (at most 32), the first the most significant: u(n). */
static uint32_t
h264_u(
	struct h264_bits *bits,
	unsigned count)
{
	uint32_t value;
	unsigned index;
	unsigned bit;

	/* Bit after bit; past the end every bit is zero and the reader is in error. */
	value = 0U;
	for (index = 0U; index < count; index++) {
		bit = 0U;
		if (bits->bit >= bits->size * 8U) {
			bits->error = 1;
		} else {
			bit = (bits->bytes[bits->bit / 8U] >> (7U - bits->bit % 8U)) & 1U;
			bits->bit++;
		}
		value = (value << 1) | bit;
	}

	/* Succeeded: the bits as a number. */
	return value;
}

/* Reads an unsigned Exp-Golomb code: ue(v). */
static uint32_t
h264_ue(
	struct h264_bits *bits)
{
	unsigned zeros;
	uint32_t bit;
	uint32_t rest;

	/* The leading zeros, at most 31. */
	zeros = 0U;
	for (;;) {
		bit = h264_u(bits, 1U);
		if (bit != 0U || bits->error != 0)
			break;
		zeros++;
		if (zeros > 31U) {
			bits->error = 1;
			return 0U;
		}
	}

	/* 2^zeros - 1 plus the bits after the one. */
	rest = h264_u(bits, zeros);
	return (uint32_t)((1ULL << zeros) - 1U + rest);
}

/* Reads a signed Exp-Golomb code: se(v). */
static int32_t
h264_se(
	struct h264_bits *bits)
{
	uint32_t code;

	/* Odd codes are positive, even ones negative. */
	code = h264_ue(bits);
	if ((code & 1U) != 0U)
		return (int32_t)((code + 1U) / 2U);

	/* Succeeded: a negative or zero value. */
	return -(int32_t)(code / 2U);
}

/* Reports whether syntax follows before the RBSP's stop bit: more_rbsp_data(). */
static int
h264_more_data(
	const struct h264_bits *bits)
{
	size_t last;
	size_t stop;
	unsigned bit;

	/* The last nonzero byte holds the stop bit, its lowest set bit. */
	last = bits->size;
	while (last > 0U && bits->bytes[last - 1U] == 0U)
		last--;
	if (last == 0U)
		return 0;
	bit = 0U;
	while (((bits->bytes[last - 1U] >> bit) & 1U) == 0U)
		bit++;
	stop = (last - 1U) * 8U + (7U - bit);

	/* More data while the cursor is before the stop bit. */
	if (bits->bit < stop)
		return 1;

	/* Succeeded: only the stop bit is left. */
	return 0;
}

/*
 * Reads one scaling list in its scan order (7.3.2.1.1.1): a list whose
 * first delta makes the next scale zero uses the default list; a later zero
 * repeats the last scale to the end.
 */
static void
h264_scaling_list(
	struct h264_bits *bits,
	uint8_t *list,
	unsigned count,
	int *use_default)
{
	int32_t last_scale;
	int32_t next_scale;
	int32_t delta;
	unsigned index;

	/* Each coefficient from the one before. */
	last_scale = 8;
	next_scale = 8;
	*use_default = 0;
	for (index = 0U; index < count; index++) {
		if (next_scale != 0) {
			delta = h264_se(bits);
			next_scale = (last_scale + delta + 256) % 256;
			if (index == 0U && next_scale == 0)
				*use_default = 1;
		}
		if (next_scale != 0)
			last_scale = next_scale;
		list[index] = (uint8_t)last_scale;
	}
}

/* Reads the scaling lists of a set: `count` present flags, each followed by its list when set. */
static void
h264_scaling_lists(
	struct h264_bits *bits,
	unsigned count,
	StdVideoH264ScalingLists *lists)
{
	uint32_t present;
	unsigned index;
	int use_default;

	/* The six 4x4 lists, then the 8x8 ones. */
	memset(lists, 0, sizeof(*lists));
	for (index = 0U; index < count; index++) {
		present = h264_u(bits, 1U);
		if (present == 0U)
			continue;
		lists->scaling_list_present_mask |= (uint16_t)(1U << index);
		if (index < 6U)
			h264_scaling_list(bits, lists->ScalingList4x4[index], 16U, &use_default);
		else
			h264_scaling_list(bits, lists->ScalingList8x8[index - 6U], 64U, &use_default);
		if (use_default)
			lists->use_default_scaling_matrix_mask |= (uint16_t)(1U << index);
	}
}

/* Reads a sequence parameter set (7.3.2.1.1) into its id's place. */
static int
h264_parse_sps(
	struct h264_stream *stream,
	const uint8_t *nal,
	size_t size)
{
	static struct h264_bits bits;
	StdVideoH264SequenceParameterSet sps;
	StdVideoH264ScalingLists lists;
	int32_t offsets[255];
	uint32_t profile_idc;
	int high;
	uint32_t constraints;
	uint32_t level_idc;
	uint32_t id;
	uint32_t value;
	uint32_t index;

	/* The payload after the NAL header. */
	h264_bits_load(&bits, nal + 1, size - 1U);
	memset(&sps, 0, sizeof(sps));
	memset(&lists, 0, sizeof(lists));
	memset(offsets, 0, sizeof(offsets));

	/* The profile, its constraint flags, the level and the id. */
	profile_idc = h264_u(&bits, 8U);
	constraints = h264_u(&bits, 8U);
	level_idc = h264_u(&bits, 8U);
	id = h264_ue(&bits);
	if (id >= H264_SPS_IDS)
		return -1;
	sps.profile_idc = (StdVideoH264ProfileIdc)profile_idc;
	sps.level_idc = h264_level(level_idc);
	sps.seq_parameter_set_id = (uint8_t)id;
	sps.flags.constraint_set0_flag = (constraints >> 7) & 1U;
	sps.flags.constraint_set1_flag = (constraints >> 6) & 1U;
	sps.flags.constraint_set2_flag = (constraints >> 5) & 1U;
	sps.flags.constraint_set3_flag = (constraints >> 4) & 1U;
	sps.flags.constraint_set4_flag = (constraints >> 3) & 1U;
	sps.flags.constraint_set5_flag = (constraints >> 2) & 1U;

	/* The format and the scaling matrix of the High profiles; 4:2:0 8-bit otherwise. */
	sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
	high = h264_high_profile(profile_idc);
	if (high) {
		value = h264_ue(&bits);
		sps.chroma_format_idc = (StdVideoH264ChromaFormatIdc)value;
		if (value == 3U)
			sps.flags.separate_colour_plane_flag = h264_u(&bits, 1U);
		sps.bit_depth_luma_minus8 = (uint8_t)h264_ue(&bits);
		sps.bit_depth_chroma_minus8 = (uint8_t)h264_ue(&bits);
		sps.flags.qpprime_y_zero_transform_bypass_flag = h264_u(&bits, 1U);
		sps.flags.seq_scaling_matrix_present_flag = h264_u(&bits, 1U);
		if (sps.flags.seq_scaling_matrix_present_flag) {
			if (value == 3U)
				h264_scaling_lists(&bits, 12U, &lists);
			else
				h264_scaling_lists(&bits, 8U, &lists);
		}
	}

	/* The frame number and the order count. */
	sps.log2_max_frame_num_minus4 = (uint8_t)h264_ue(&bits);
	value = h264_ue(&bits);
	sps.pic_order_cnt_type = (StdVideoH264PocType)value;
	if (value == 0U) {
		sps.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)h264_ue(&bits);
	} else if (value == 1U) {
		sps.flags.delta_pic_order_always_zero_flag = h264_u(&bits, 1U);
		sps.offset_for_non_ref_pic = h264_se(&bits);
		sps.offset_for_top_to_bottom_field = h264_se(&bits);
		value = h264_ue(&bits);
		if (value > 255U)
			return -1;
		sps.num_ref_frames_in_pic_order_cnt_cycle = (uint8_t)value;
		for (index = 0U; index < value; index++)
			offsets[index] = h264_se(&bits);
	}

	/* The references, the extent, the frame structure and the cropping. */
	sps.max_num_ref_frames = (uint8_t)h264_ue(&bits);
	sps.flags.gaps_in_frame_num_value_allowed_flag = h264_u(&bits, 1U);
	sps.pic_width_in_mbs_minus1 = h264_ue(&bits);
	sps.pic_height_in_map_units_minus1 = h264_ue(&bits);
	sps.flags.frame_mbs_only_flag = h264_u(&bits, 1U);
	if (!sps.flags.frame_mbs_only_flag)
		sps.flags.mb_adaptive_frame_field_flag = h264_u(&bits, 1U);
	sps.flags.direct_8x8_inference_flag = h264_u(&bits, 1U);
	sps.flags.frame_cropping_flag = h264_u(&bits, 1U);
	if (sps.flags.frame_cropping_flag) {
		sps.frame_crop_left_offset = h264_ue(&bits);
		sps.frame_crop_right_offset = h264_ue(&bits);
		sps.frame_crop_top_offset = h264_ue(&bits);
		sps.frame_crop_bottom_offset = h264_ue(&bits);
	}

	/* The VUI is not read and not given to the decoder. */
	sps.flags.vui_parameters_present_flag = 0;
	if (bits.error != 0)
		return -1;

	/* Keeps the set, its lists and its offsets in the id's place. */
	stream->sps[id] = sps;
	stream->sps_scaling[id] = lists;
	memcpy(stream->sps_offsets[id], offsets, sizeof(offsets[0]) * sps.num_ref_frames_in_pic_order_cnt_cycle);
	stream->sps[id].pScalingLists = &stream->sps_scaling[id];
	stream->sps[id].pOffsetForRefFrame = stream->sps_offsets[id];
	stream->sps[id].pSequenceParameterSetVui = NULL;
	stream->has_sps[id] = 1;
	stream->last_sps = id;

	/* Succeeded: the set is kept. */
	return 0;
}

/* Reads a picture parameter set (7.3.2.2) into its id's place; its sequence set must be known. */
static int
h264_parse_pps(
	struct h264_stream *stream,
	const uint8_t *nal,
	size_t size)
{
	static struct h264_bits bits;
	StdVideoH264PictureParameterSet pps;
	StdVideoH264ScalingLists lists;
	uint32_t id;
	uint32_t sps_id;
	uint32_t groups;
	unsigned count;
	int more;

	/* The payload after the NAL header. */
	h264_bits_load(&bits, nal + 1, size - 1U);
	memset(&pps, 0, sizeof(pps));
	memset(&lists, 0, sizeof(lists));

	/* The ids. */
	id = h264_ue(&bits);
	sps_id = h264_ue(&bits);
	if (id >= H264_PPS_IDS || sps_id >= H264_SPS_IDS || !stream->has_sps[sps_id])
		return -1;
	pps.pic_parameter_set_id = (uint8_t)id;
	pps.seq_parameter_set_id = (uint8_t)sps_id;

	/* The entropy coder, the field order flag and one slice group only. */
	pps.flags.entropy_coding_mode_flag = h264_u(&bits, 1U);
	pps.flags.bottom_field_pic_order_in_frame_present_flag = h264_u(&bits, 1U);
	groups = h264_ue(&bits);
	if (groups != 0U)
		return -1;

	/* The default references, the weighting, the quantizers and the slice header's flags. */
	pps.num_ref_idx_l0_default_active_minus1 = (uint8_t)h264_ue(&bits);
	pps.num_ref_idx_l1_default_active_minus1 = (uint8_t)h264_ue(&bits);
	pps.flags.weighted_pred_flag = h264_u(&bits, 1U);
	pps.weighted_bipred_idc = (StdVideoH264WeightedBipredIdc)h264_u(&bits, 2U);
	pps.pic_init_qp_minus26 = (int8_t)h264_se(&bits);
	pps.pic_init_qs_minus26 = (int8_t)h264_se(&bits);
	pps.chroma_qp_index_offset = (int8_t)h264_se(&bits);
	pps.flags.deblocking_filter_control_present_flag = h264_u(&bits, 1U);
	pps.flags.constrained_intra_pred_flag = h264_u(&bits, 1U);
	pps.flags.redundant_pic_cnt_present_flag = h264_u(&bits, 1U);

	/* The High syntax, when it is there: the 8x8 transform, a scaling matrix, the second chroma offset. */
	pps.second_chroma_qp_index_offset = pps.chroma_qp_index_offset;
	more = h264_more_data(&bits);
	if (more) {
		pps.flags.transform_8x8_mode_flag = h264_u(&bits, 1U);
		pps.flags.pic_scaling_matrix_present_flag = h264_u(&bits, 1U);
		if (pps.flags.pic_scaling_matrix_present_flag) {
			count = 6U;
			if (pps.flags.transform_8x8_mode_flag) {
				count += 2U;
				if (stream->sps[sps_id].chroma_format_idc == STD_VIDEO_H264_CHROMA_FORMAT_IDC_444)
					count += 4U;
			}
			h264_scaling_lists(&bits, count, &lists);
		}
		pps.second_chroma_qp_index_offset = (int8_t)h264_se(&bits);
	}
	if (bits.error != 0)
		return -1;

	/* Keeps the set and its lists in the id's place. */
	stream->pps[id] = pps;
	stream->pps_scaling[id] = lists;
	stream->pps[id].pScalingLists = &stream->pps_scaling[id];
	stream->has_pps[id] = 1;
	stream->last_pps = id;

	/* Succeeded: the set is kept. */
	return 0;
}

/* Turns a level_idc into StdVideoH264LevelIdc; an unknown one is level 1.0. */
static StdVideoH264LevelIdc
h264_level(
	uint32_t level_idc)
{
	static const uint8_t levels[] = {
		10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62
	};
	unsigned index;

	/* The enumerants are the levels in order. */
	for (index = 0U; index < sizeof(levels); index++) {
		if (levels[index] == level_idc)
			return (StdVideoH264LevelIdc)index;
	}

	/* Not a level the table names. */
	return STD_VIDEO_H264_LEVEL_IDC_1_0;
}

/* Reports whether a profile's sequence sets carry the format and scaling syntax (7.3.2.1.1). */
static int
h264_high_profile(
	uint32_t profile_idc)
{
	/* The High, the scalable and the multiview profiles. */
	switch (profile_idc) {
	case 100:
	case 110:
	case 122:
	case 244:
	case 44:
	case 83:
	case 86:
	case 118:
	case 128:
	case 138:
	case 139:
	case 134:
	case 135:
		return 1;
	default:
		/* Succeeded: Baseline, Main, Extended and others. */
		return 0;
	}
}

/* Reads a slice's first_mb_in_slice and slice_type (0 to 4). */
static int
h264_slice_start(
	const uint8_t *nal,
	size_t size,
	uint32_t *first_mb,
	uint32_t *slice_type)
{
	static struct h264_bits bits;

	/* The first two codes after the NAL header. */
	h264_bits_load(&bits, nal + 1, size - 1U);
	*first_mb = h264_ue(&bits);
	*slice_type = h264_ue(&bits) % 5U;
	if (bits.error != 0)
		return -1;

	/* Succeeded: where the slice starts and its type. */
	return 0;
}

/*
 * Reads the first slice header of a picture (7.3.3) up to its order count
 * fields, and fills the picture's information.  Returns NULL, or the
 * reason the probe cannot decode the picture.
 */
static const char *
h264_slice_header(
	struct h264_stream *stream,
	const struct h264_nal *nal,
	struct h264_picture *picture)
{
	static struct h264_bits bits;
	const StdVideoH264SequenceParameterSet *sps;
	const StdVideoH264PictureParameterSet *pps;
	uint32_t slice_type;
	uint32_t pps_id;
	uint32_t lsb;
	int32_t deltas[2];
	const char *reason;

	/* The slice's macroblock, type and picture set. */
	h264_bits_load(&bits, stream->data + nal->offset + 1U, nal->size - 1U);
	(void)h264_ue(&bits);
	slice_type = h264_ue(&bits) % 5U;
	pps_id = h264_ue(&bits);
	if (pps_id >= H264_PPS_IDS || !stream->has_pps[pps_id])
		return "unknown picture parameter set";
	pps = &stream->pps[pps_id];
	sps = &stream->sps[pps->seq_parameter_set_id];

	/* The probe decodes progressive 4:2:0 frames without a separate colour plane. */
	if (sps->flags.separate_colour_plane_flag || sps->chroma_format_idc != STD_VIDEO_H264_CHROMA_FORMAT_IDC_420)
		return "not 4:2:0";
	if (!sps->flags.frame_mbs_only_flag)
		return "not frames only";

	/* The frame number and the IDR picture's id. */
	picture->info.frame_num = (uint16_t)h264_u(&bits, sps->log2_max_frame_num_minus4 + 4U);
	if (nal->type == H264_NAL_IDR)
		picture->info.idr_pic_id = (uint16_t)h264_ue(&bits);

	/* The order count fields: type 0's LSB and bottom delta, type 1's two deltas, none of type 2. */
	lsb = 0U;
	deltas[0] = 0;
	deltas[1] = 0;
	if (sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_0) {
		lsb = h264_u(&bits, sps->log2_max_pic_order_cnt_lsb_minus4 + 4U);
		if (pps->flags.bottom_field_pic_order_in_frame_present_flag)
			deltas[1] = h264_se(&bits);
	} else if (sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_1 && !sps->flags.delta_pic_order_always_zero_flag) {
		deltas[0] = h264_se(&bits);
		if (pps->flags.bottom_field_pic_order_in_frame_present_flag)
			deltas[1] = h264_se(&bits);
	}

	/* The rest of the header up to the reference marking. */
	reason = h264_slice_rest(&bits, pps, nal, slice_type, picture);
	if (reason != NULL)
		return reason;
	if (bits.error != 0)
		return "unreadable slice header";
	picture->slice_type = slice_type;

	/* The picture's information. */
	picture->info.seq_parameter_set_id = pps->seq_parameter_set_id;
	picture->info.pic_parameter_set_id = (uint8_t)pps_id;
	picture->info.flags.IdrPicFlag = 0;
	if (nal->type == H264_NAL_IDR)
		picture->info.flags.IdrPicFlag = 1;
	picture->info.flags.is_reference = 0;
	if (nal->ref_idc != 0U)
		picture->info.flags.is_reference = 1;
	picture->intra = 0;
	if (slice_type == H264_SLICE_I || slice_type == H264_SLICE_SI)
		picture->intra = 1;
	picture->info.flags.is_intra = (uint32_t)picture->intra;

	/* Its order counts. */
	h264_order_counts(stream, sps, nal, lsb, deltas, picture);

	/* Succeeded: the picture can be decoded. */
	return NULL;
}

/*
 * Works out a frame's order counts (8.2.1): type 0 from the LSB and the
 * last reference picture's MSB and LSB (8.2.1.1), type 1 from the frame
 * number, its wrap-around offset and the sequence's cycle (8.2.1.2), type
 * 2 from the frame number and its offset (8.2.1.3); an IDR picture starts
 * them over.  A picture with the memory management operation 5 starts
 * them over after itself: later pictures see its counts less the smaller
 * of them, its frame number as 0 and no offset.
 */
static void
h264_order_counts(
	struct h264_stream *stream,
	const StdVideoH264SequenceParameterSet *sps,
	const struct h264_nal *nal,
	uint32_t lsb,
	const int32_t *deltas,
	struct h264_picture *picture)
{
	int32_t maximum;
	int32_t msb;
	int32_t top;
	int32_t bottom;
	int32_t smaller;
	uint32_t frame_offset;
	uint32_t frame_maximum;

	/* An IDR picture restarts the counts. */
	if (nal->type == H264_NAL_IDR) {
		stream->previous_msb = 0;
		stream->previous_lsb = 0;
		stream->previous_frame_num = 0U;
		stream->previous_frame_offset = 0U;
	}

	/* Types 1 and 2: the frame number's offset grows by MaxFrameNum when the number wraps around. */
	frame_maximum = 1U << (sps->log2_max_frame_num_minus4 + 4U);
	frame_offset = stream->previous_frame_offset;
	if (nal->type != H264_NAL_IDR && stream->previous_frame_num > picture->info.frame_num)
		frame_offset += frame_maximum;

	/* The counts of the picture's type. */
	msb = 0;
	if (sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_2) {
		/* Twice the frame's number from the start, one less for a non-reference picture. */
		top = 0;
		if (nal->type != H264_NAL_IDR) {
			top = (int32_t)(2U * (frame_offset + picture->info.frame_num));
			if (nal->ref_idc == 0U)
				top--;
		}
		bottom = top;
	} else if (sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_1) {
		/* The cycle's expected count, the slice's deltas and the field offset. */
		top = h264_expected_order(sps, nal, frame_offset, picture->info.frame_num) + deltas[0];
		bottom = top + sps->offset_for_top_to_bottom_field + deltas[1];
	} else {
		/* The MSB follows the LSB's wrap around from the last reference picture's. */
		maximum = (int32_t)(1U << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4U));
		msb = stream->previous_msb;
		if ((int32_t)lsb < stream->previous_lsb && stream->previous_lsb - (int32_t)lsb >= maximum / 2)
			msb = stream->previous_msb + maximum;
		else if ((int32_t)lsb > stream->previous_lsb && (int32_t)lsb - stream->previous_lsb > maximum / 2)
			msb = stream->previous_msb - maximum;
		top = msb + (int32_t)lsb;
		bottom = top + deltas[1];
	}
	picture->info.PicOrderCnt[0] = top;
	picture->info.PicOrderCnt[1] = bottom;

	/* The counts later pictures see, and the place in the display order: the smaller count. */
	smaller = top < bottom ? top : bottom;
	picture->reference_poc[0] = top;
	picture->reference_poc[1] = bottom;
	if (picture->restart) {
		picture->reference_poc[0] = top - smaller;
		picture->reference_poc[1] = bottom - smaller;
	}
	picture->display_poc = picture->reference_poc[0] < picture->reference_poc[1] ? picture->reference_poc[0] : picture->reference_poc[1];

	/* The numbering of the next picture: from 0 after a restart. */
	stream->previous_frame_num = picture->info.frame_num;
	stream->previous_frame_offset = frame_offset;
	if (picture->restart) {
		stream->previous_frame_num = 0U;
		stream->previous_frame_offset = 0U;
	}

	/* A reference picture is type 0's next base; after a restart, MSB 0 and its top count as the LSB. */
	if (nal->ref_idc != 0U) {
		stream->previous_msb = msb;
		stream->previous_lsb = (int32_t)lsb;
		if (picture->restart) {
			stream->previous_msb = 0;
			stream->previous_lsb = picture->reference_poc[0];
		}
	}
}

/*
 * Reports type 1's expected order count of a frame (8.2.1.2): the cycles
 * of the sequence's reference frame offsets before the frame and the
 * offsets into its own cycle, with the offset of a non-reference picture.
 */
static int32_t
h264_expected_order(
	const StdVideoH264SequenceParameterSet *sps,
	const struct h264_nal *nal,
	uint32_t frame_offset,
	uint32_t frame_num)
{
	uint32_t cycle_length;
	uint32_t absolute;
	uint32_t cycles;
	uint32_t in_cycle;
	uint32_t index;
	int32_t per_cycle;
	int32_t expected;

	/* The frame's number from the start, counted among reference frames. */
	cycle_length = sps->num_ref_frames_in_pic_order_cnt_cycle;
	absolute = 0U;
	if (cycle_length != 0U)
		absolute = frame_offset + frame_num;
	if (nal->ref_idc == 0U && absolute > 0U)
		absolute--;

	/* The whole cycles before it and the offsets into its own. */
	expected = 0;
	if (absolute > 0U) {
		per_cycle = 0;
		for (index = 0U; index < cycle_length; index++)
			per_cycle += sps->pOffsetForRefFrame[index];
		cycles = (absolute - 1U) / cycle_length;
		in_cycle = (absolute - 1U) % cycle_length;
		expected = (int32_t)cycles * per_cycle;
		for (index = 0U; index <= in_cycle; index++)
			expected += sps->pOffsetForRefFrame[index];
	}

	/* A non-reference picture's own offset. */
	if (nal->ref_idc == 0U)
		expected += sps->offset_for_non_ref_pic;

	/* Succeeded: the expected count. */
	return expected;
}

/*
 * Reads the slice header from after the order count fields to the end of
 * the reference picture marking (7.3.3): the redundant picture count, the
 * direct mode flag, the active reference counts, the list modifications,
 * the weight table and the marking, which the probe needs for its DPB.
 * Returns NULL, or why the probe cannot follow the header.
 */
static const char *
h264_slice_rest(
	struct h264_bits *bits,
	const StdVideoH264PictureParameterSet *pps,
	const struct h264_nal *nal,
	uint32_t slice_type,
	struct h264_picture *picture)
{
	uint32_t l0;
	uint32_t l1;
	uint32_t override;
	int predicted;
	int bi;
	int weighted;
	const char *reason;

	/* The redundant picture count. */
	if (pps->flags.redundant_pic_cnt_present_flag)
		(void)h264_ue(bits);

	/* A P, SP or B slice reads references; a B slice from two lists, after its direct mode flag. */
	predicted = 0;
	if (slice_type == H264_SLICE_P || slice_type == H264_SLICE_SP || slice_type == H264_SLICE_B)
		predicted = 1;
	bi = 0;
	if (slice_type == H264_SLICE_B) {
		bi = 1;
		(void)h264_u(bits, 1U);
	}

	/* The active reference counts: the picture set's, or the slice's own. */
	l0 = pps->num_ref_idx_l0_default_active_minus1 + 1U;
	l1 = pps->num_ref_idx_l1_default_active_minus1 + 1U;
	if (predicted) {
		override = h264_u(bits, 1U);
		if (override != 0U) {
			l0 = h264_ue(bits) + 1U;
			if (bi)
				l1 = h264_ue(bits) + 1U;
		}
	}
	if (l0 > 32U || l1 > 32U)
		return "more than 32 active references";

	/* The list modifications of each list read. */
	if (predicted)
		h264_list_modification(bits);
	if (bi)
		h264_list_modification(bits);

	/* The weight table of an explicitly weighted slice. */
	weighted = 0;
	if (pps->flags.weighted_pred_flag && (slice_type == H264_SLICE_P || slice_type == H264_SLICE_SP))
		weighted = 1;
	if (pps->weighted_bipred_idc == STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_EXPLICIT && bi)
		weighted = 1;
	if (weighted) {
		(void)h264_ue(bits);
		(void)h264_ue(bits);
		h264_weight_table(bits, l0);
		if (bi)
			h264_weight_table(bits, l1);
	}

	/* The marking of a reference picture. */
	if (nal->ref_idc != 0U) {
		reason = h264_marking(bits, nal, picture);
		if (reason != NULL)
			return reason;
	}

	/* Succeeded: the header is read to its marking. */
	return NULL;
}

/* Skips one list's reference picture list modification (7.3.3.1). */
static void
h264_list_modification(
	struct h264_bits *bits)
{
	uint32_t flag;
	uint32_t operation;
	unsigned count;

	/* The flag, then operations up to 3 (at most 33 of them). */
	flag = h264_u(bits, 1U);
	if (flag == 0U)
		return;
	for (count = 0U; count < 33U && bits->error == 0; count++) {
		operation = h264_ue(bits);
		if (operation == 3U)
			break;
		(void)h264_ue(bits);
	}
}

/* Skips one list's prediction weights (7.3.3.2, 4:2:0): a luma and a chroma flag, each with its weights. */
static void
h264_weight_table(
	struct h264_bits *bits,
	uint32_t count)
{
	uint32_t index;
	uint32_t flag;

	/* Each active reference. */
	for (index = 0U; index < count && bits->error == 0; index++) {
		/* The luma weight and offset. */
		flag = h264_u(bits, 1U);
		if (flag != 0U) {
			(void)h264_se(bits);
			(void)h264_se(bits);
		}

		/* The two chroma weights and offsets. */
		flag = h264_u(bits, 1U);
		if (flag != 0U) {
			(void)h264_se(bits);
			(void)h264_se(bits);
			(void)h264_se(bits);
			(void)h264_se(bits);
		}
	}
}

/* Reads a reference picture's marking (7.3.3.3). */
static const char *
h264_marking(
	struct h264_bits *bits,
	const struct h264_nal *nal,
	struct h264_picture *picture)
{
	struct h264_mmco *mmco;
	uint32_t operation;

	/* An IDR picture: no output of prior pictures, and whether it is long-term. */
	if (nal->type == H264_NAL_IDR) {
		(void)h264_u(bits, 1U);
		picture->long_term_reference = (int)h264_u(bits, 1U);
		return NULL;
	}

	/* Another picture: the sliding window, or the operations. */
	picture->adaptive_marking = (int)h264_u(bits, 1U);
	if (!picture->adaptive_marking)
		return NULL;
	for (;;) {
		operation = h264_ue(bits);
		if (operation == H264_MMCO_END || bits->error != 0)
			break;
		if (operation > H264_MMCO_CURRENT_TO_LONG)
			return "unknown memory management operation";
		if (picture->mmco_count >= H264_MAX_MMCO)
			return "too many memory management operations";
		mmco = &picture->mmco[picture->mmco_count];
		memset(mmco, 0, sizeof(*mmco));
		mmco->operation = operation;
		if (operation == H264_MMCO_SHORT_UNUSED || operation == H264_MMCO_SHORT_TO_LONG)
			mmco->difference_of_pic_nums_minus1 = h264_ue(bits);
		if (operation == H264_MMCO_LONG_UNUSED)
			mmco->long_term_pic_num = h264_ue(bits);
		if (operation == H264_MMCO_SHORT_TO_LONG || operation == H264_MMCO_CURRENT_TO_LONG)
			mmco->long_term_frame_idx = h264_ue(bits);
		if (operation == H264_MMCO_MAX_LONG_INDEX)
			mmco->max_long_term_frame_idx_plus1 = h264_ue(bits);
		if (operation == H264_MMCO_ALL_UNUSED)
			picture->restart = 1;
		picture->mmco_count++;
	}

	/* Succeeded: the operations are kept. */
	return NULL;
}

/* Reports where the start code of the unit at an offset starts: three bytes before it, four with a leading zero. */
static size_t
h264_start_code_of(
	const uint8_t *data,
	size_t offset)
{
	size_t start;

	/* The three bytes 00 00 01, and a zero before them. */
	start = offset - 3U;
	if (start > 0U && data[start - 1U] == 0U)
		start--;

	/* Succeeded: the start code's first byte. */
	return start;
}
