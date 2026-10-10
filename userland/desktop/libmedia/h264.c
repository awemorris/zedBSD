/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.264 elementary stream reader of libmedia (derived from the repository's original Zlib WS083 probe) (see h264.h).
 *
 * The syntax is Recommendation ITU-T H.264's: 7.3.1 (NAL unit), 7.3.2.1.1
 * (sequence parameter set), 7.3.2.1.1.1 (scaling list), 7.3.2.2 (picture
 * parameter set), 7.3.3 (slice header, up to the order count fields), and
 * the order counts of 8.2.1.1 and 8.2.1.3.
 */

#include "h264.h"
#include "bits.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

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
static void h264_list_modification(struct h264_bits *bits, struct h264_list *list);
static void h264_weight_table(struct h264_bits *bits, uint32_t count);
static const char *h264_marking(struct h264_bits *bits, const struct h264_nal *nal, struct h264_picture *picture);
static size_t h264_start_code_of(const uint8_t *data, size_t offset);
static void h264_vui(struct h264_bits *bits, StdVideoH264SequenceParameterSetVui *vui, StdVideoH264HrdParameters *hrd);
static void h264_hrd(struct h264_bits *bits, StdVideoH264HrdParameters *hrd);

/*
 * Opens a stream: reads every sequence and picture parameter set in it
 * (the last one of an id is kept), then rewinds to the first picture.
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

	/* Extradata contains parameter sets; stop before the first coded picture so future sets cannot replace its state. */
	cursor = 0U;
	for (;;) {
		found = h264_nal_next(data, size, &cursor, &nal);
		if (!found)
			break;
		if (found < 0) {
			stream->error = EINVAL;
			return -1;
		}
		if (nal.type == H264_NAL_SLICE || nal.type == H264_NAL_IDR)
			break;
		error = 0;
		if (nal.type == H264_NAL_SPS)
			error = h264_parse_sps(stream, data + nal.offset, nal.size);
		else if (nal.type == H264_NAL_PPS)
			error = h264_parse_pps(stream, data + nal.offset, nal.size);
		if (error != 0) {
			if (stream->error == 0)
				stream->error = EINVAL;
			return -1;
		}
	}

	/* Succeeded: the pictures start from the beginning. */
	stream->cursor = 0U;
	return 0;
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

	/* Advance over empty units without growing the call stack. */
	for (;;) {
		start = *cursor;
		while (start + 3U <= size) {
			if (data[start] == 0U && data[start + 1U] == 0U && data[start + 2U] == 1U)
				break;
			start++;
		}
		if (start + 3U > size)
			return 0;
		start += 3U;
		end = start;
		while (end + 3U <= size) {
			if (data[end] == 0U && data[end + 1U] == 0U && data[end + 2U] == 1U)
				break;
			end++;
		}
		if (end + 3U > size)
			end = size;
		*cursor = end;
		while (end > start && data[end - 1U] == 0U)
			end--;
		if (end != start)
			break;
	}
	if ((data[start] & 0x80U) != 0U)
		return -1;

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
	struct h264_picture *next;
	unsigned index;

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
		if (found < 0) {
			stream->error = EINVAL;
			*reason = "invalid NAL header";
			return -1;
		}

		/* The picture's access unit starts at the start code of its first unit. */
		if (picture->access_unit == (size_t)-1)
			picture->access_unit = h264_start_code_of(stream->data, nal.offset);

		/* A delimiter or a parameter set after slices ends the picture. */
		if (nal.type != H264_NAL_SLICE && nal.type != H264_NAL_IDR) {
			if (picture->slice_count != 0U &&
			    (nal.type == H264_NAL_DELIMITER || nal.type == H264_NAL_SPS || nal.type == H264_NAL_PPS || nal.type == H264_NAL_SEI)) {
				stream->cursor = before;
				break;
			}
			error = 0;
			if (nal.type == H264_NAL_SPS)
				error = h264_parse_sps(stream, stream->data + nal.offset, nal.size);
			else if (nal.type == H264_NAL_PPS)
				error = h264_parse_pps(stream, stream->data + nal.offset, nal.size);
			else if (nal.type >= 2U && nal.type <= 4U)
				error = -1;
			if (error != 0) {
				if (stream->error == 0)
					stream->error = EINVAL;
				*reason = "unsupported or invalid parameter set";
				return -1;
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

		/* Every slice contributes its active reference counts and modifications. */
		if (picture->slice_count >= H264_MAX_SLICES) {
			stream->error = ENOTSUP;
			*reason = "more than 256 slices";
			return -1;
		}
		if (picture->slice_count == 0U) {
			if (first_mb != 0U) {
				stream->error = ENOTSUP;
				*reason = "slice order starts after macroblock zero";
				return -1;
			}
			*reason = h264_slice_header(stream, &nal, picture);
		} else {
			next = calloc(1U, sizeof(*next));
			if (next == NULL) {
				stream->error = ENOMEM;
				*reason = "slice metadata allocation failed";
				return -1;
			}
			*reason = h264_slice_header(stream, &nal, next);
			if (*reason == NULL) {
				if (first_mb <= picture->slices[picture->slice_count - 1U].first_mb)
					*reason = "arbitrary or overlapping slice order";
				if (memcmp(&picture->info, &next->info, sizeof(next->info)) != 0)
					*reason = "inconsistent picture identity across slices";
				if (picture->poc_lsb != next->poc_lsb || memcmp(picture->poc_delta, next->poc_delta, sizeof(next->poc_delta)) != 0)
					*reason = "inconsistent picture order across slices";
				if (picture->mmco_count != next->mmco_count || picture->adaptive_marking != next->adaptive_marking)
					*reason = "inconsistent reference marking across slices";
				if (picture->long_term_reference != next->long_term_reference || picture->no_output_prior != next->no_output_prior)
					*reason = "inconsistent IDR marking across slices";
				for (index = 0U; index < picture->mmco_count; index++) {
					if (memcmp(&picture->mmco[index], &next->mmco[index], sizeof(next->mmco[index])) != 0)
						*reason = "inconsistent memory management across slices";
				}
				picture->slices[picture->slice_count] = next->slices[0];
			}
			free(next);
		}
		if (*reason != NULL) {
			if (stream->error == 0)
				stream->error = EINVAL;
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

/*
 * Sets the next complete access unit without discarding parameter sets or order history.
 */
void
h264_input(
	struct h264_stream *stream,
	const uint8_t *data,
	size_t size)
{
	/* Packet bytes remain owned by the decoder until every slice has been submitted. */
	stream->data = data;
	stream->size = size;
	stream->cursor = 0U;
	stream->error = 0;
}

/*
 * Resets picture history while retaining reusable sequence and picture parameter sets.
 */
void
h264_flush(
	struct h264_stream *stream)
{
	/* A seek starts a new order epoch and a new reference frame-number chain. */
	stream->previous_msb = 0;
	stream->previous_lsb = 0;
	stream->previous_frame_num = 0U;
	stream->previous_frame_offset = 0U;
	stream->previous_reference_frame_num = 0U;
	stream->reference_started = 0;
	stream->error = 0;
	stream->cursor = stream->size;
}

/*
 * Calculates progressive picture order after any inferred gap frames have updated history.
 *
 * The submitted picture retains its order during decoding. MMCO 5 changes the saved history
 * to the post-decode order, which reference admission and the next picture use.
 */
int
h264_picture_order(
	struct h264_stream *stream,
	const StdVideoH264SequenceParameterSet *sps,
	struct h264_picture *picture)
{
	int64_t top;
	int64_t bottom;
	int64_t msb;
	int64_t cycle_delta;
	int64_t expected;
	int64_t minimum;
	uint64_t frame_offset;
	uint64_t absolute;
	uint64_t cycles;
	uint32_t frame_maximum;
	uint32_t poc_maximum;
	unsigned index;
	unsigned remainder;

	/* An IDR resets all previous frame and order bases before the current syntax is used. */
	if (picture->info.flags.IdrPicFlag) {
		stream->previous_msb = 0;
		stream->previous_lsb = 0;
		stream->previous_frame_num = 0U;
		stream->previous_frame_offset = 0U;
	}
	frame_maximum = 1U << (sps->log2_max_frame_num_minus4 + 4U);
	frame_offset = stream->previous_frame_offset;
	if (stream->previous_frame_num > picture->info.frame_num)
		frame_offset += frame_maximum;
	if (frame_offset > UINT32_MAX)
		return EINVAL;
	top = 0;
	bottom = 0;
	msb = 0;

	/* Type zero unwraps only against the previous reference picture. */
	if (sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_0) {
		poc_maximum = 1U << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4U);
		msb = stream->previous_msb;
		if ((int64_t)picture->poc_lsb < stream->previous_lsb && stream->previous_lsb - (int64_t)picture->poc_lsb >= poc_maximum / 2U)
			msb += poc_maximum;
		else if ((int64_t)picture->poc_lsb > stream->previous_lsb && (int64_t)picture->poc_lsb - stream->previous_lsb > poc_maximum / 2U)
			msb -= poc_maximum;
		top = msb + picture->poc_lsb;
		bottom = top + picture->poc_delta[1];
	} else if (sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_1) {
		/* A non-reference picture consumes the preceding reference cycle position. */
		absolute = 0U;
		if (sps->num_ref_frames_in_pic_order_cnt_cycle != 0U)
			absolute = frame_offset + picture->info.frame_num;
		if (!picture->info.flags.is_reference && absolute != 0U)
			absolute--;
		expected = 0;
		if (absolute != 0U) {
			cycle_delta = 0;
			for (index = 0U; index < sps->num_ref_frames_in_pic_order_cnt_cycle; index++)
				cycle_delta += sps->pOffsetForRefFrame[index];
			cycles = (absolute - 1U) / sps->num_ref_frames_in_pic_order_cnt_cycle;
			remainder = (unsigned)((absolute - 1U) % sps->num_ref_frames_in_pic_order_cnt_cycle);
			if (cycles != 0U) {
				if (cycle_delta > INT64_MAX / (int64_t)cycles || cycle_delta < INT64_MIN / (int64_t)cycles)
					return EINVAL;
			}
			expected = cycle_delta * (int64_t)cycles;
			/* Bound the intermediate cycle result before adding at most 255 signed offsets. */
			if (expected < -1099511627776LL || expected > 1099511627776LL)
				return EINVAL;
			for (index = 0U; index <= remainder; index++)
				expected += sps->pOffsetForRefFrame[index];
		}
		if (!picture->info.flags.is_reference)
			expected += sps->offset_for_non_ref_pic;
		top = expected + picture->poc_delta[0];
		bottom = top + sps->offset_for_top_to_bottom_field + picture->poc_delta[1];
	} else {
		/* Type two uses every previous picture, including non-reference and inferred frames. */
		if (!picture->info.flags.IdrPicFlag) {
			top = 2 * (int64_t)(frame_offset + picture->info.frame_num);
			if (!picture->info.flags.is_reference)
				top--;
		}
		bottom = top;
	}
	if (top < INT32_MIN || top > INT32_MAX)
		return EINVAL;
	if (bottom < INT32_MIN || bottom > INT32_MAX)
		return EINVAL;
	picture->info.PicOrderCnt[0] = (int32_t)top;
	picture->info.PicOrderCnt[1] = (int32_t)bottom;

	/* MMCO 5 is applied after this picture is decoded, including frame-number history. */
	stream->previous_frame_num = picture->info.frame_num;
	stream->previous_frame_offset = frame_offset;
	if (picture->mmco5) {
		stream->previous_frame_num = 0U;
		stream->previous_frame_offset = 0U;
	}
	if (picture->info.flags.is_reference) {
		stream->previous_msb = (int32_t)msb;
		stream->previous_lsb = (int32_t)picture->poc_lsb;
		stream->previous_reference_frame_num = picture->info.frame_num;
		stream->reference_started = 1;
		if (picture->mmco5) {
			minimum = top;
			if (bottom < minimum)
				minimum = bottom;
			if (top - minimum > INT32_MAX)
				return EINVAL;
			stream->previous_msb = 0;
			stream->previous_lsb = (int32_t)(top - minimum);
			stream->previous_reference_frame_num = 0U;
		}
	}

	/* The saved state and submitted StdVideo order now represent their distinct decode phases. */
	return 0;
}

/*
 * Reports the bounded display reorder depth from VUI or the normative level DPB limit.
 */
unsigned
h264_reorder_depth(
	const struct h264_stream *stream,
	unsigned sps_id)
{
	/* Table A-1 facts indexed by the Khronos level enumeration; immutable for the process. */
	static const uint32_t maximum_mbs[] = {396U, 900U, 2376U, 2376U, 2376U, 4752U, 8100U, 8100U, 18000U, 20480U, 32768U, 32768U, 34816U, 110400U, 184320U, 184320U, 696320U, 696320U, 696320U};
	const StdVideoH264SequenceParameterSet *sps;
	unsigned level;
	unsigned count;
	unsigned mbs;

	/* Only a published set may be used to derive an output bound. */
	if (sps_id >= H264_SPS_IDS)
		return 0U;
	if (!stream->has_sps[sps_id])
		return 0U;
	sps = &stream->sps[sps_id];
	if (sps->flags.vui_parameters_present_flag && stream->vui[sps_id].flags.bitstream_restriction_flag)
		return stream->vui[sps_id].max_num_reorder_frames;
	if (sps->profile_idc == STD_VIDEO_H264_PROFILE_IDC_BASELINE)
		return 0U;
	level = (unsigned)sps->level_idc;
	if (level >= sizeof(maximum_mbs) / sizeof(maximum_mbs[0]))
		return 16U;
	mbs = (sps->pic_width_in_mbs_minus1 + 1U) * (sps->pic_height_in_map_units_minus1 + 1U);
	count = maximum_mbs[level] / mbs;
	if (count > 16U)
		count = 16U;

	/* The no-VUI limit trades bounded latency for complete standards-based output ordering. */
	return count;
}

/*
 * Reports sample aspect ratio without depending on a particular container.
 */
void
h264_aspect(
	const struct h264_stream *stream,
	unsigned sps_id,
	uint32_t *num,
	uint32_t *den)
{
	/* Table E-1 facts for the fixed aspect-ratio identifiers; immutable for the process. */
	static const uint8_t ratios[17][2] = {{1, 1}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11}, {20, 11}, {32, 11}, {80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99}, {4, 3}, {3, 2}, {2, 1}};
	const StdVideoH264SequenceParameterSetVui *vui;
	unsigned id;

	/* Unspecified and malformed metadata default to square pixels. */
	*num = 1U;
	*den = 1U;
	if (sps_id >= H264_SPS_IDS)
		return;
	if (!stream->sps[sps_id].flags.vui_parameters_present_flag)
		return;
	vui = &stream->vui[sps_id];
	id = (unsigned)vui->aspect_ratio_idc;
	if (id < 17U) {
		*num = ratios[id][0];
		*den = ratios[id][1];
	} else if (id == 255U && vui->sar_width != 0U && vui->sar_height != 0U) {
		*num = vui->sar_width;
		*den = vui->sar_height;
	}
}

/* Read a bounded payload prefix, validating every emulation-prevention byte encountered. */
static void
h264_bits_load(
	struct h264_bits *bits,
	const uint8_t *nal,
	size_t size)
{
	size_t index;
	unsigned zeros;

	/* Slice bodies remain compressed; only the finite header prefix is copied here. */
	memset(bits, 0, sizeof(*bits));
	zeros = 0U;
	for (index = 0U; index < size && bits->size < H264_RBSP_BYTES; index++) {
		/* A protected byte must actually have a following protected value. */
		if (zeros >= 2U && nal[index] == 3U) {
			if (index + 1U == size) {
				bits->error = EINVAL;
				return;
			}
			if (nal[index + 1U] > 3U) {
				bits->error = EINVAL;
				return;
			}
			zeros = 0U;
			continue;
		}

		/* Forbidden unescaped start-code values are not valid RBSP payload. */
		if (zeros >= 2U && nal[index] <= 2U) {
			bits->error = EINVAL;
			return;
		}
		if (nal[index] == 0U) {
			zeros++;
		} else {
			zeros = 0U;
		}
		bits->bytes[bits->size] = nal[index];
		bits->size++;
	}
	return;
}

/* Use the common sticky-error bit reader for fixed-width H.264 syntax. */
static uint32_t
h264_u(
	struct h264_bits *bits,
	unsigned count)
{
	struct media_bits reader;
	uint32_t value;

	/* A syntax field with zero width contributes zero without changing the cursor. */
	if (count == 0U)
		return 0U;
	media_bits_init(&reader, bits->bytes, bits->size);
	reader.position = bits->bit;
	reader.error = bits->error;
	value = media_bits_read(&reader, count);
	bits->bit = reader.position;
	bits->error = reader.error;
	return value;
}

/* Read a complete unsigned Exp-Golomb value without truncating its 32-bit endpoint. */
static uint32_t
h264_ue(
	struct h264_bits *bits)
{
	struct media_bits reader;
	uint32_t value;

	/* The shared reader preserves failure before any dependent parameter-set publication. */
	media_bits_init(&reader, bits->bytes, bits->size);
	reader.position = bits->bit;
	reader.error = bits->error;
	value = media_bits_ue(&reader);
	bits->bit = reader.position;
	bits->error = reader.error;
	return value;
}

/* Read a signed Exp-Golomb value through the same validated integer-domain helper. */
static int32_t
h264_se(
	struct h264_bits *bits)
{
	struct media_bits reader;
	int32_t value;

	/* Signed endpoints are checked before a later field narrows their representation. */
	media_bits_init(&reader, bits->bytes, bits->size);
	reader.position = bits->bit;
	reader.error = bits->error;
	value = media_bits_se(&reader);
	bits->bit = reader.position;
	bits->error = reader.error;
	return value;
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
			if (delta < -128 || delta > 127) {
				bits->error = EINVAL;
				return;
			}
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
	struct h264_bits bits;
	StdVideoH264SequenceParameterSet sps;
	StdVideoH264ScalingLists lists;
	int32_t offsets[255];
	StdVideoH264SequenceParameterSetVui vui;
	StdVideoH264HrdParameters hrd[2];
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
	memset(&vui, 0, sizeof(vui));
	memset(hrd, 0, sizeof(hrd));
	if (size > H264_RBSP_BYTES) {
		stream->error = ENOTSUP;
		return -1;
	}

	/* The profile, its constraint flags, the level and the id. */
	profile_idc = h264_u(&bits, 8U);
	constraints = h264_u(&bits, 8U);
	level_idc = h264_u(&bits, 8U);
	id = h264_ue(&bits);
	if (id >= H264_SPS_IDS)
		return -1;
	if (profile_idc != 66U && profile_idc != 77U && profile_idc != 100U) {
		stream->error = ENOTSUP;
		return -1;
	}
	if ((constraints & 3U) != 0U)
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
		value = h264_ue(&bits);
		if (value != 0U) {
			stream->error = ENOTSUP;
			return -1;
		}
		value = h264_ue(&bits);
		if (value != 0U) {
			stream->error = ENOTSUP;
			return -1;
		}
		value = (uint32_t)sps.chroma_format_idc;
		if (value != 1U) {
			stream->error = ENOTSUP;
			return -1;
		}
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
	value = h264_ue(&bits);
	if (value > 12U)
		return -1;
	sps.log2_max_frame_num_minus4 = (uint8_t)value;
	value = h264_ue(&bits);
	if (value > 2U)
		return -1;
	sps.pic_order_cnt_type = (StdVideoH264PocType)value;
	if (value == 0U) {
		value = h264_ue(&bits);
		if (value > 12U)
			return -1;
		sps.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)value;
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
	value = h264_ue(&bits);
	if (value > 16U)
		return -1;
	sps.max_num_ref_frames = (uint8_t)value;
	sps.flags.gaps_in_frame_num_value_allowed_flag = h264_u(&bits, 1U);
	sps.pic_width_in_mbs_minus1 = h264_ue(&bits);
	sps.pic_height_in_map_units_minus1 = h264_ue(&bits);
	sps.flags.frame_mbs_only_flag = h264_u(&bits, 1U);
	if (!sps.flags.frame_mbs_only_flag)
		sps.flags.mb_adaptive_frame_field_flag = h264_u(&bits, 1U);
	if (!sps.flags.frame_mbs_only_flag) {
		stream->error = ENOTSUP;
		return -1;
	}
	if (sps.pic_width_in_mbs_minus1 >= 512U || sps.pic_height_in_map_units_minus1 >= 512U)
		return -1;
	sps.flags.direct_8x8_inference_flag = h264_u(&bits, 1U);
	sps.flags.frame_cropping_flag = h264_u(&bits, 1U);
	if (sps.flags.frame_cropping_flag) {
		sps.frame_crop_left_offset = h264_ue(&bits);
		sps.frame_crop_right_offset = h264_ue(&bits);
		sps.frame_crop_top_offset = h264_ue(&bits);
		sps.frame_crop_bottom_offset = h264_ue(&bits);
	}

	/* A truncated optional VUI is discarded; core SPS syntax must already be complete. */
	if (bits.error != 0)
		return -1;
	sps.flags.vui_parameters_present_flag = h264_u(&bits, 1U);
	if (bits.error != 0)
		return -1;
	if (sps.flags.vui_parameters_present_flag) {
		h264_vui(&bits, &vui, hrd);
		if (bits.error != 0) {
			sps.flags.vui_parameters_present_flag = 0;
			bits.error = 0;
		}
	}
	if (sps.frame_crop_left_offset + (uint64_t)sps.frame_crop_right_offset >= (sps.pic_width_in_mbs_minus1 + 1U) * 8U)
		return -1;
	if (sps.frame_crop_top_offset + (uint64_t)sps.frame_crop_bottom_offset >= (sps.pic_height_in_map_units_minus1 + 1U) * 8U)
		return -1;
	if (bits.error != 0)
		return -1;

	/* Keeps the set, its lists and its offsets in the id's place. */
	stream->sps[id] = sps;
	stream->sps_scaling[id] = lists;
	memcpy(stream->sps_offsets[id], offsets, sizeof(offsets[0]) * sps.num_ref_frames_in_pic_order_cnt_cycle);
	stream->sps[id].pScalingLists = &stream->sps_scaling[id];
	stream->sps[id].pOffsetForRefFrame = stream->sps_offsets[id];
	stream->vui[id] = vui;
	memcpy(stream->hrd[id], hrd, sizeof(hrd));
	stream->vui[id].pHrdParameters = &stream->hrd[id][1];
	if (vui.flags.nal_hrd_parameters_present_flag)
		stream->vui[id].pHrdParameters = &stream->hrd[id][0];
	stream->sps[id].pSequenceParameterSetVui = NULL;
	if (sps.flags.vui_parameters_present_flag)
		stream->sps[id].pSequenceParameterSetVui = &stream->vui[id];
	stream->parameter_generation++;
	stream->has_sps[id] = 1;

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
	struct h264_bits bits;
	StdVideoH264PictureParameterSet pps;
	StdVideoH264ScalingLists lists;
	uint32_t id;
	uint32_t sps_id;
	uint32_t groups;
	unsigned count;
	int more;
	uint32_t value;
	int32_t signed_value;

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
	if (groups != 0U) {
		stream->error = ENOTSUP;
		return -1;
	}

	/* The default references, the weighting, the quantizers and the slice header's flags. */
	value = h264_ue(&bits);
	if (value >= 32U)
		return -1;
	pps.num_ref_idx_l0_default_active_minus1 = (uint8_t)value;
	value = h264_ue(&bits);
	if (value >= 32U)
		return -1;
	pps.num_ref_idx_l1_default_active_minus1 = (uint8_t)value;
	pps.flags.weighted_pred_flag = h264_u(&bits, 1U);
	value = h264_u(&bits, 2U);
	if (value > 2U)
		return -1;
	pps.weighted_bipred_idc = (StdVideoH264WeightedBipredIdc)value;
	signed_value = h264_se(&bits);
	if (signed_value < -26 || signed_value > 25)
		return -1;
	pps.pic_init_qp_minus26 = (int8_t)signed_value;
	signed_value = h264_se(&bits);
	if (signed_value < -26 || signed_value > 25)
		return -1;
	pps.pic_init_qs_minus26 = (int8_t)signed_value;
	signed_value = h264_se(&bits);
	if (signed_value < -12 || signed_value > 12)
		return -1;
	pps.chroma_qp_index_offset = (int8_t)signed_value;
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
		signed_value = h264_se(&bits);
	if (signed_value < -12 || signed_value > 12)
		return -1;
	pps.second_chroma_qp_index_offset = (int8_t)signed_value;
	}
	if (bits.error != 0)
		return -1;

	/* Keeps the set and its lists in the id's place. */
	stream->pps[id] = pps;
	stream->pps_scaling[id] = lists;
	stream->pps[id].pScalingLists = &stream->pps_scaling[id];
	stream->has_pps[id] = 1;
	stream->parameter_generation++;

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
	return STD_VIDEO_H264_LEVEL_IDC_INVALID;
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
	struct h264_bits bits;

	/* The first two codes after the NAL header. */
	h264_bits_load(&bits, nal + 1, size - 1U);
	*first_mb = h264_ue(&bits);
	*slice_type = h264_ue(&bits);
	if (*slice_type > 9U)
		return -1;
	*slice_type %= 5U;
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
	struct h264_bits bits;
	const StdVideoH264SequenceParameterSet *sps;
	const StdVideoH264PictureParameterSet *pps;
	uint32_t slice_type;
	uint32_t pps_id;
	uint32_t lsb;
	int32_t bottom_delta;
	uint32_t value;
	const char *reason;

	/* The slice's macroblock, type and picture set. */
	h264_bits_load(&bits, stream->data + nal->offset + 1U, nal->size - 1U);
	picture->slices[0].first_mb = h264_ue(&bits);
	value = h264_ue(&bits);
	if (value > 9U)
		return "invalid slice type";
	slice_type = value % 5U;
	if (slice_type == H264_SLICE_SP || slice_type == H264_SLICE_SI) {
		stream->error = ENOTSUP;
		return "switching slices are unsupported";
	}
	picture->slices[0].type = slice_type;
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
		{
		value = h264_ue(&bits);
		if (value > 65535U)
			return "invalid IDR picture identifier";
		picture->info.idr_pic_id = (uint16_t)value;
	}

	/* The order count fields of type 0; type 2 has none; type 1 is not handled. */
	lsb = 0U;
	bottom_delta = 0;
	if (sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_0) {
		lsb = h264_u(&bits, sps->log2_max_pic_order_cnt_lsb_minus4 + 4U);
		if (pps->flags.bottom_field_pic_order_in_frame_present_flag)
			bottom_delta = h264_se(&bits);
	} else if (sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_1 && !sps->flags.delta_pic_order_always_zero_flag) {
		picture->poc_delta[0] = h264_se(&bits);
		if (pps->flags.bottom_field_pic_order_in_frame_present_flag)
			picture->poc_delta[1] = h264_se(&bits);
	}
	picture->poc_lsb = lsb;
	if (sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_0)
		picture->poc_delta[1] = bottom_delta;

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

	/* POC is computed after the caller has inserted inferred gap references. */

	/* Succeeded: the picture can be decoded. */
	return NULL;
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
			l0 = h264_ue(bits);
			if (l0 >= 32U)
				return "invalid active reference count";
			l0++;
			if (bi) {
				l1 = h264_ue(bits);
				if (l1 >= 32U)
					return "invalid active reference count";
				l1++;
			}
		}
	}
	if (l0 > 32U || l1 > 32U)
		return "more than 32 active references";

	/* The list modifications of each list read. */
	if (predicted)
		h264_list_modification(bits, &picture->slices[0].list[0]);
	if (bi)
		h264_list_modification(bits, &picture->slices[0].list[1]);

	if (predicted)
		picture->slices[0].list[0].active = l0;
	if (bi)
		picture->slices[0].list[1].active = l1;

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

/* Keep complete bounded list modifications; a missing terminator is invalid syntax. */
static void
h264_list_modification(
	struct h264_bits *bits,
	struct h264_list *list)
{
	uint32_t flag;
	uint32_t operation;
	uint32_t argument;

	/* A terminator is required after at most the retained operation count. */
	flag = h264_u(bits, 1U);
	if (flag == 0U)
		return;
	for (;;) {
		operation = h264_ue(bits);
		if (bits->error != 0 || operation == 3U)
			break;
		if (operation > 2U || list->count >= H264_MAX_MODIFICATIONS) {
			bits->error = EINVAL;
			break;
		}
		argument = h264_ue(bits);
		list->operation[list->count] = operation;
		list->argument[list->count] = argument;
		list->count++;
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
		picture->no_output_prior = (int)h264_u(bits, 1U);
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
		if (operation == H264_MMCO_ALL_UNUSED)
			picture->mmco5 = 1;
		if (operation == H264_MMCO_SHORT_UNUSED || operation == H264_MMCO_SHORT_TO_LONG)
			mmco->difference_of_pic_nums_minus1 = h264_ue(bits);
		if (operation == H264_MMCO_LONG_UNUSED)
			mmco->long_term_pic_num = h264_ue(bits);
		if (operation == H264_MMCO_SHORT_TO_LONG || operation == H264_MMCO_CURRENT_TO_LONG)
			mmco->long_term_frame_idx = h264_ue(bits);
		if (operation == H264_MMCO_MAX_LONG_INDEX)
			mmco->max_long_term_frame_idx_plus1 = h264_ue(bits);
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

/* Parse a complete HRD syntax record without borrowing its parsing from another decoder. */
static void
h264_hrd(
	struct h264_bits *bits,
	StdVideoH264HrdParameters *hrd)
{
	uint32_t count;
	unsigned index;

	/* Each CPB entry contributes its normative rate, size and constant-rate flag. */
	count = h264_ue(bits);
	if (count >= 32U) {
		bits->error = EINVAL;
		return;
	}
	hrd->cpb_cnt_minus1 = (uint8_t)count;
	hrd->bit_rate_scale = (uint8_t)h264_u(bits, 4U);
	hrd->cpb_size_scale = (uint8_t)h264_u(bits, 4U);
	for (index = 0U; index <= count; index++) {
		hrd->bit_rate_value_minus1[index] = h264_ue(bits);
		hrd->cpb_size_value_minus1[index] = h264_ue(bits);
		hrd->cbr_flag[index] = (uint8_t)h264_u(bits, 1U);
	}

	/* Timing widths are retained for the standard SPS structure. */
	hrd->initial_cpb_removal_delay_length_minus1 = h264_u(bits, 5U);
	hrd->cpb_removal_delay_length_minus1 = h264_u(bits, 5U);
	hrd->dpb_output_delay_length_minus1 = h264_u(bits, 5U);
	hrd->time_offset_length = h264_u(bits, 5U);
}

/* Parse optional display, colour, timing and reorder metadata after the complete core SPS. */
static void
h264_vui(
	struct h264_bits *bits,
	StdVideoH264SequenceParameterSetVui *vui,
	StdVideoH264HrdParameters *hrd)
{
	uint32_t syntax;

	/* SAR and overscan precede the colour description. */
	vui->flags.aspect_ratio_info_present_flag = h264_u(bits, 1U);
	if (vui->flags.aspect_ratio_info_present_flag) {
		vui->aspect_ratio_idc = (StdVideoH264AspectRatioIdc)h264_u(bits, 8U);
		if (vui->aspect_ratio_idc == STD_VIDEO_H264_ASPECT_RATIO_IDC_EXTENDED_SAR) {
			vui->sar_width = (uint16_t)h264_u(bits, 16U);
			vui->sar_height = (uint16_t)h264_u(bits, 16U);
		}
	}
	vui->flags.overscan_info_present_flag = h264_u(bits, 1U);
	if (vui->flags.overscan_info_present_flag)
		vui->flags.overscan_appropriate_flag = h264_u(bits, 1U);
	vui->flags.video_signal_type_present_flag = h264_u(bits, 1U);
	if (vui->flags.video_signal_type_present_flag) {
		vui->video_format = (uint8_t)h264_u(bits, 3U);
		vui->flags.video_full_range_flag = h264_u(bits, 1U);
		vui->flags.color_description_present_flag = h264_u(bits, 1U);
		if (vui->flags.color_description_present_flag) {
			vui->colour_primaries = (uint8_t)h264_u(bits, 8U);
			vui->transfer_characteristics = (uint8_t)h264_u(bits, 8U);
			vui->matrix_coefficients = (uint8_t)h264_u(bits, 8U);
		}
	}

	/* Chroma siting and rational picture timing remain optional metadata. */
	vui->flags.chroma_loc_info_present_flag = h264_u(bits, 1U);
	if (vui->flags.chroma_loc_info_present_flag) {
		syntax = h264_ue(bits);
		if (syntax > 5U)
			bits->error = EINVAL;
		vui->chroma_sample_loc_type_top_field = (uint8_t)syntax;
		syntax = h264_ue(bits);
		if (syntax > 5U)
			bits->error = EINVAL;
		vui->chroma_sample_loc_type_bottom_field = (uint8_t)syntax;
	}
	vui->flags.timing_info_present_flag = h264_u(bits, 1U);
	if (vui->flags.timing_info_present_flag) {
		vui->num_units_in_tick = h264_u(bits, 32U);
		vui->time_scale = h264_u(bits, 32U);
		vui->flags.fixed_frame_rate_flag = h264_u(bits, 1U);
	}

	/* Both HRD variants must be consumed even though StdVideo exposes a single HRD pointer. */
	vui->flags.nal_hrd_parameters_present_flag = h264_u(bits, 1U);
	if (vui->flags.nal_hrd_parameters_present_flag)
		h264_hrd(bits, &hrd[0]);
	vui->flags.vcl_hrd_parameters_present_flag = h264_u(bits, 1U);
	if (vui->flags.vcl_hrd_parameters_present_flag)
		h264_hrd(bits, &hrd[1]);
	if (vui->flags.nal_hrd_parameters_present_flag || vui->flags.vcl_hrd_parameters_present_flag)
		(void)h264_u(bits, 1U);
	(void)h264_u(bits, 1U);
	vui->flags.bitstream_restriction_flag = h264_u(bits, 1U);
	if (vui->flags.bitstream_restriction_flag) {
		(void)h264_u(bits, 1U);
		(void)h264_ue(bits);
		(void)h264_ue(bits);
		(void)h264_ue(bits);
		(void)h264_ue(bits);
		syntax = h264_ue(bits);
		if (syntax > 16U)
			bits->error = EINVAL;
		vui->max_num_reorder_frames = (uint8_t)syntax;
		syntax = h264_ue(bits);
		if (syntax > 16U || syntax < vui->max_num_reorder_frames)
			bits->error = EINVAL;
		vui->max_dec_frame_buffering = (uint8_t)syntax;
	}
}
