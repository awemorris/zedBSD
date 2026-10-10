/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native H.264 uses standard Vulkan Video admission, logical reference marking and retained CPU display pictures. */
#include "media-private.h"
#include "vkvideo-runtime.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define VKVIDEO_PENDING 20U

/* One retained picture carries its presentation time and order within the current coded-video epoch. */
struct vkvideo_output {
	struct media_picture *picture;
	int32_t poc;
	int64_t time_us;
};

/* One native backend owns packet conversion, parser history, GPU retirement and a bounded display reorder queue. */
struct vkvideo_decoder {
	struct media_bitstream bitstream;
	struct h264_stream stream;
	struct h264_picture picture;
	struct h264_dpb dpb;
	struct vkvideo_runtime *runtime;
	struct media_picture_pool *pool;
	StdVideoH264SequenceParameterSet sequence;
	int32_t sequence_offsets[256];
	struct vkvideo_output pending[VKVIDEO_PENDING];
	unsigned count;
	unsigned depth;
	unsigned generation;
	unsigned bump;
	int have_picture;
	int configured;
	int draining;
	int failed;
	int64_t packet_time;
	int decode_order_times;
	struct media_picture *current;
	uint32_t aspect_num;
	uint32_t aspect_den;
	unsigned colour;
	int full_range;
};

static int vkvideo_load(void);
static const char *vkvideo_reason(void);
static int vkvideo_open(const struct media_track *track, void **result);
static const char *vkvideo_name(const void *state);
static int vkvideo_send(void *state, const struct media_packet *packet);
static int vkvideo_receive(void *state, int64_t *time_us);
static void *vkvideo_picture(void *state);
static size_t vkvideo_sound(void *state, int16_t *samples, size_t capacity, uint32_t rate);
static void vkvideo_flush(void *state);
static void vkvideo_close(void *state);
static int vkvideo_configure(struct vkvideo_decoder *decoder, unsigned sps_id);
static void vkvideo_pending_free(struct vkvideo_decoder *decoder);
static int vkvideo_pop(struct vkvideo_decoder *decoder, int64_t *time_us);
static int vkvideo_decode(struct vkvideo_decoder *decoder);
static int vkvideo_errno(int problem);
static int vkvideo_problem(int error);

/* The native library's video operation table owns no software-codec loader or vendor-specific pixel layout. */
const struct media_decoder_ops media_vkvideo_ops = {
	"vulkan-video", vkvideo_load, vkvideo_reason, vkvideo_open, vkvideo_name,
	vkvideo_send, vkvideo_receive, vkvideo_picture, vkvideo_sound,
	vkvideo_flush, vkvideo_close, media_picture_unref, media_picture_size,
	media_picture_scale, media_picture_scaler_free, NULL, NULL, media_picture_aspect};

/* Built-in dispatch is available; device/profile admission belongs to opening the selected track. */
static int
vkvideo_load(
	void)
{
	return 0;
}

/* Native video reports its reason with a typed admission result for each track. */
static const char *
vkvideo_reason(
	void)
{
	return "";
}

/* Open an H.264 track and query its signalled profile before an application commits to the native backend. */
static int
vkvideo_open(
	const struct media_track *track,
	void **result)
{
	struct vkvideo_decoder *decoder;
	StdVideoH264SequenceParameterSet probe;
	struct vkvideo_runtime *runtime;
	unsigned index;
	int error;

	/* Another codec is a normal backend-selection refusal. */
	*result = NULL;
	if (track->codec != MEDIA_CODEC_H264)
		return MEDIA_PROBLEM_FORMAT;
	decoder = calloc(1U, sizeof(*decoder));
	if (decoder == NULL)
		return ENOMEM;
	decoder->decode_order_times = track->decode_order_times;
	decoder->aspect_num = track->aspect_num;
	decoder->aspect_den = track->aspect_den;
	decoder->full_range = track->full_range;
	decoder->colour = MEDIA_COLOUR_601;
	if (track->width >= 1280U || track->height > 576U)
		decoder->colour = MEDIA_COLOUR_709;
	if (track->colour_present) {
		if (track->colour_matrix == 1U)
			decoder->colour = MEDIA_COLOUR_709;
		else if (track->colour_matrix == 9U || track->colour_matrix == 10U)
			decoder->colour = MEDIA_COLOUR_2020;
		else
			decoder->colour = MEDIA_COLOUR_601;
	}

	/* Convert the container's codec initialization into native parameter-set bytes. */
	error = media_bitstream_open(&decoder->bitstream, track->codec, track->private_data, track->private_size);
	if (error != 0) {
		vkvideo_close(decoder);
		return error;
	}

	/* Read parameter sets before allocating any profile-specific GPU resources. */
	error = h264_open(&decoder->stream, decoder->bitstream.prefix, decoder->bitstream.prefix_size);
	if (error != 0) {
		error = decoder->stream.error;
		vkvideo_close(decoder);
		if (error == ENOTSUP)
			return MEDIA_PROBLEM_PROFILE;
		return error;
	}

	/* Admit the first available sequence through standard capability queries. */
	for (index = 0U; index < H264_SPS_IDS; index++) {
		if (decoder->stream.has_sps[index]) {
			error = vkvideo_configure(decoder, index);
			if (error != 0) {
				error = vkvideo_problem(error);
				vkvideo_close(decoder);
				return error;
			}

			break;
		}
	}

	/* Without container SPS, query a small Baseline context now and admit actual sequence syntax on the first packet. */
	if (!decoder->configured) {
		memset(&probe, 0, sizeof(probe));
		probe.profile_idc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
		probe.pic_width_in_mbs_minus1 = 7U;
		probe.pic_height_in_map_units_minus1 = 7U;
		probe.max_num_ref_frames = 1U;
		error = media_vkvideo_runtime_open(&probe, &runtime);
		if (error != 0) {
			vkvideo_close(decoder);
			return error;
		}

		/* Close the temporary capability probe before deferring actual sequence allocation. */
		media_vkvideo_runtime_close(runtime);
	}

	/* Extradata is configuration, not a pending packet whose bytes must be received first. */
	decoder->stream.cursor = decoder->stream.size;

	/* Succeeded: all library and signalled-profile admission requirements are satisfied. */
	*result = decoder;
	return 0;
}

/* Identify the codec separately from the backend name reported by the public decoder API. */
static const char *
vkvideo_name(
	const void *state)
{
	(void)state;
	return "h264";
}

/* Admit one complete packet while keeping its converted bytes stable until every access unit has been consumed. */
static int
vkvideo_send(
	void *state,
	const struct media_packet *packet)
{
	struct vkvideo_decoder *decoder;
	const uint8_t *data;
	size_t size;
	const char *reason;
	int found;
	int error;

	/* Receive consumes each pending packet before the converter can overwrite its backing storage. */
	decoder = state;
	if (decoder->failed != 0)
		return decoder->failed;
	if (decoder->have_picture || decoder->stream.cursor < decoder->stream.size)
		return EAGAIN;
	if (packet == NULL) {
		decoder->draining = 1;
		return 0;
	}

	/* Do not accept compressed packets after end-of-stream drain began. */
	if (decoder->draining)
		return EINVAL;
	error = media_bitstream_convert(&decoder->bitstream, packet->data, packet->size, packet->keyframe, &data, &size);
	if (error != 0)
		return error;
	h264_input(&decoder->stream, data, size);
	decoder->packet_time = packet->pts_us;
	found = h264_next_picture(&decoder->stream, &decoder->picture, &reason);
	if (found < 0) {
		error = decoder->stream.error;
		if (error == 0)
			error = EINVAL;
		return error;
	}

	/* Retain a parsed first picture so its parameter admission is not repeated or lost. */
	if (found != 0) {
		decoder->have_picture = 1;
		if (!decoder->configured) {
			error = vkvideo_configure(decoder, decoder->picture.info.seq_parameter_set_id);
			if (error != 0) {
				return error;
			}
		}
	}

	/* Succeeded: actual sequence and first-picture syntax have been admitted before the packet is accepted. */
	return 0;
}

/* Receive display-order pictures, decoding additional accepted access units only when reorder depth requires them. */
static int
vkvideo_receive(
	void *state,
	int64_t *time_us)
{
	struct vkvideo_decoder *decoder;
	const char *reason;
	int found;
	int error;

	/* The previous receive's picture stays available until this receive replaces it. */
	decoder = state;
	if (decoder->current != NULL) {
		media_picture_unref(decoder->current);
		decoder->current = NULL;
	}

	/* Keep a runtime decoding error sticky until the application flushes for a seek. */
	if (decoder->failed != 0)
		return -decoder->failed;
	for (;;) {
		if (
			decoder->count > decoder->depth ||
			decoder->bump != 0U ||
			decoder->draining) {
			if (decoder->count != 0U) {
				found = vkvideo_pop(decoder, time_us);
				if (found != 0)
					return found;
				continue;
			}
		}

		/* Parse the next picture only after pending presentation output has been serviced. */
		if (!decoder->have_picture) {
			found = h264_next_picture(&decoder->stream, &decoder->picture, &reason);
			if (found < 0) {
				decoder->failed = decoder->stream.error;
				if (decoder->failed == 0)
					decoder->failed = EINVAL;
				return -decoder->failed;
			}

			/* Request another compressed packet when no complete picture remains. */
			if (found == 0)
				return 0;
			decoder->have_picture = 1;
		}

		/* Flush the preceding coded-video epoch before inserting an IDR or MMCO-5 picture. */
		if (decoder->picture.info.flags.IdrPicFlag || decoder->picture.mmco5) {
			if (decoder->picture.no_output_prior)
				vkvideo_pending_free(decoder);
			if (decoder->count != 0U) {
				decoder->bump = decoder->count;
				continue;
			}
		}

		/* Decode the admitted picture after preceding epoch output has been emptied. */
		error = vkvideo_decode(decoder);
		if (error == EAGAIN) {
			decoder->bump = decoder->count;
			continue;
		}

		/* Emit MMCO-5 output before any picture from the reset order epoch. */
		if (error == 0 && decoder->picture.mmco5)
			decoder->bump = decoder->count;
		decoder->have_picture = 0;
		if (error != 0) {
			decoder->failed = error;
			return -error;
		}
	}
}

/* Retain the current CPU picture without extending the lifetime of any GPU session. */
static void *
vkvideo_picture(
	void *state)
{
	struct vkvideo_decoder *decoder;

	/* Exported picture references belong to the caller, independently of the next receive. */
	decoder = state;
	if (decoder->current == NULL)
		return NULL;
	media_picture_ref(decoder->current);
	return decoder->current;
}

/* H.264 video has no audio output. */
static size_t
vkvideo_sound(
	void *state,
	int16_t *samples,
	size_t capacity,
	uint32_t rate)
{
	(void)state;
	(void)samples;
	(void)capacity;
	(void)rate;
	return 0U;
}

/* Clear packet, display and reference history while retaining reusable decoder allocations. */
static void
vkvideo_flush(
	void *state)
{
	struct vkvideo_decoder *decoder;

	/* Seek cannot expose queued pre-seek CPU frames or retain active logical references. */
	decoder = state;
	vkvideo_pending_free(decoder);
	if (decoder->current != NULL) {
		media_picture_unref(decoder->current);
		decoder->current = NULL;
	}

	/* Reset parser, logical references and reusable coding state together. */
	h264_flush(&decoder->stream);
	h264_dpb_init(&decoder->dpb, decoder->sequence.max_num_ref_frames);
	if (decoder->runtime != NULL)
		media_vkvideo_runtime_reset(decoder->runtime);
	decoder->have_picture = 0;
	decoder->draining = 0;
	decoder->failed = 0;
}

/* Retire the Vulkan owner before releasing backend state; exported CPU pictures retain only their own pool. */
static void
vkvideo_close(
	void *state)
{
	struct vkvideo_decoder *decoder;

	/* Partial opens share the same unwind path as completed decoders. */
	decoder = state;
	if (decoder == NULL)
		return;
	media_vkvideo_runtime_close(decoder->runtime);
	vkvideo_pending_free(decoder);
	if (decoder->current != NULL)
		media_picture_unref(decoder->current);
	media_picture_pool_close(decoder->pool);
	media_bitstream_close(&decoder->bitstream);
	free(decoder);
}

/* Match the actual sequence extent and reference capacity before replacing any GPU context. */
static int
vkvideo_configure(
	struct vkvideo_decoder *decoder,
	unsigned sps_id)
{
	const StdVideoH264SequenceParameterSet *sps;
	struct vkvideo_runtime *runtime;
	struct media_picture_pool *pool;
	uint32_t width;
	uint32_t height;
	int change;
	int comparison;
	int error;

	/* Changes affecting image allocation or profile require a fresh session; scaling/POC changes require a new reference epoch. */
	sps = &decoder->stream.sps[sps_id];
	change = 0;
	if (!decoder->configured)
		change = 1;
	if (sps->profile_idc != decoder->sequence.profile_idc || sps->max_num_ref_frames != decoder->sequence.max_num_ref_frames)
		change = 1;
	if (sps->pic_width_in_mbs_minus1 != decoder->sequence.pic_width_in_mbs_minus1 || sps->pic_height_in_map_units_minus1 != decoder->sequence.pic_height_in_map_units_minus1)
		change = 1;
	if (sps->frame_crop_left_offset != decoder->sequence.frame_crop_left_offset || sps->frame_crop_right_offset != decoder->sequence.frame_crop_right_offset)
		change = 1;
	if (sps->frame_crop_top_offset != decoder->sequence.frame_crop_top_offset || sps->frame_crop_bottom_offset != decoder->sequence.frame_crop_bottom_offset)
		change = 1;
	if (sps->log2_max_frame_num_minus4 != decoder->sequence.log2_max_frame_num_minus4 || sps->log2_max_pic_order_cnt_lsb_minus4 != decoder->sequence.log2_max_pic_order_cnt_lsb_minus4)
		change = 1;
	if (sps->pic_order_cnt_type != decoder->sequence.pic_order_cnt_type || sps->num_ref_frames_in_pic_order_cnt_cycle != decoder->sequence.num_ref_frames_in_pic_order_cnt_cycle)
		change = 1;
	if (sps->offset_for_non_ref_pic != decoder->sequence.offset_for_non_ref_pic || sps->offset_for_top_to_bottom_field != decoder->sequence.offset_for_top_to_bottom_field)
		change = 1;
	/* The saved offset cycle owns its bytes because in-band SPS updates reuse parser storage. */
	if (sps->num_ref_frames_in_pic_order_cnt_cycle == decoder->sequence.num_ref_frames_in_pic_order_cnt_cycle && sps->num_ref_frames_in_pic_order_cnt_cycle != 0U) {
		comparison = memcmp(sps->pOffsetForRefFrame, decoder->sequence_offsets, sps->num_ref_frames_in_pic_order_cnt_cycle * sizeof(int32_t));
		if (comparison != 0)
			change = 1;
	}

	/* Replace sequence resources after every preceding retained output has been emitted. */
	if (change) {
		if (decoder->count != 0U)
			return EAGAIN;
		error = media_vkvideo_runtime_open(sps, &runtime);
		if (error != 0) {
			error = vkvideo_errno(error);
			return error;
		}

		/* Allocate CPU picture storage for the new visible crop independently of the GPU image. */
		width = (sps->pic_width_in_mbs_minus1 + 1U) * 16U - (sps->frame_crop_left_offset + sps->frame_crop_right_offset) * 2U;
		height = (sps->pic_height_in_map_units_minus1 + 1U) * 16U - (sps->frame_crop_top_offset + sps->frame_crop_bottom_offset) * 2U;
		pool = media_picture_pool_create(width, height, decoder->depth + 4U);
		if (pool == NULL) {
			media_vkvideo_runtime_close(runtime);
			return ENOMEM;
		}

		/* Retire the old GPU session and decoder-owned picture pool before publishing replacements. */
		media_vkvideo_runtime_close(decoder->runtime);
		media_picture_pool_close(decoder->pool);
		decoder->runtime = runtime;
		decoder->pool = pool;
		decoder->sequence = *sps;
		if (sps->num_ref_frames_in_pic_order_cnt_cycle != 0U)
			memcpy(decoder->sequence_offsets, sps->pOffsetForRefFrame, sps->num_ref_frames_in_pic_order_cnt_cycle * sizeof(int32_t));
		decoder->sequence.pOffsetForRefFrame = decoder->sequence_offsets;
		decoder->stream.previous_msb = 0;
		decoder->stream.previous_lsb = 0;
		decoder->stream.previous_frame_num = 0U;
		decoder->stream.previous_frame_offset = 0U;
		decoder->stream.reference_started = 0;
		h264_dpb_init(&decoder->dpb, sps->max_num_ref_frames);
		decoder->configured = 1;
		decoder->generation = 0U;
	}

	/* Update the reorder bound and parameters for the now-admitted sequence. */
	decoder->depth = h264_reorder_depth(&decoder->stream, sps_id);
	if (decoder->generation != decoder->stream.parameter_generation) {
		error = media_vkvideo_runtime_parameters(decoder->runtime, &decoder->stream);
		if (error != 0)
			return error;
		decoder->generation = decoder->stream.parameter_generation;
	}

	/* Succeeded: current parameter sets and image ownership match the actual sequence. */
	return 0;
}

/* Release only backend-held display references, preserving pictures already exported to applications. */
static void
vkvideo_pending_free(
	struct vkvideo_decoder *decoder)
{
	unsigned index;

	/* Each queued CPU frame owns one pool reference. */
	for (index = 0U; index < decoder->count; index++)
		media_picture_unref(decoder->pending[index].picture);
	decoder->count = 0U;
	decoder->bump = 0U;
}

/* Extract the earliest picture order from a bounded output epoch. */
static int
vkvideo_pop(
	struct vkvideo_decoder *decoder,
	int64_t *time_us)
{
	unsigned earliest;
	unsigned time_index;
	unsigned index;

	/* Stable selection preserves arrival order when two pictures carry an equal order count. */
	earliest = 0U;
	for (index = 1U; index < decoder->count; index++) {
		if (decoder->pending[index].poc < decoder->pending[earliest].poc)
			earliest = index;
	}

	/* Transfer the earliest retained picture from pending ownership to the current receive result. */
	decoder->current = decoder->pending[earliest].picture;
	*time_us = decoder->pending[earliest].time_us;

	/* Without composition offsets, consume the earliest clock position independently of the earliest picture POC. */
	if (decoder->decode_order_times) {
		time_index = 0U;
		for (index = 1U; index < decoder->count; index++) {
			if (decoder->pending[index].time_us < decoder->pending[time_index].time_us)
				time_index = index;
		}

		/* Keep the remaining timestamp multiset intact when its minimum came from another pending picture. */
		*time_us = decoder->pending[time_index].time_us;
		decoder->pending[time_index].time_us = decoder->pending[earliest].time_us;
	}

	/* Compact the pending queue after transferring one picture and clock position. */
	for (index = earliest + 1U; index < decoder->count; index++)
		decoder->pending[index - 1U] = decoder->pending[index];
	decoder->count--;
	if (decoder->bump != 0U)
		decoder->bump--;

	/* A missing-reference placeholder consumes its clock position without publishing a picture. */
	if (decoder->current == NULL)
		return 0;

	/* Succeeded: the current picture owns the removed queue reference until the next receive. */
	return 1;
}

/* Execute and mark one prepared picture, copying only admitted output into the display queue. */
static int
vkvideo_decode(
	struct vkvideo_decoder *decoder)
{
	const StdVideoH264SequenceParameterSet *sps;
	const StdVideoH264SequenceParameterSetVui *vui;
	struct h264_dpb_plan plan;
	struct media_picture *output;
	int32_t poc;
	int error;

	/* Missing-reference drops still update the complete logical DPB and order history. */
	sps = &decoder->stream.sps[decoder->picture.info.seq_parameter_set_id];
	error = vkvideo_configure(decoder, sps->seq_parameter_set_id);
	if (error != 0)
		return error;

	/* Prepare normative order and every slice's available reference set before GPU submission. */
	error = h264_dpb_prepare(&decoder->dpb, &decoder->stream, &decoder->picture, &plan);
	if (error != 0)
		return error;
	if (!plan.decode) {
		error = h264_dpb_mark(&decoder->dpb, sps, &decoder->picture, &plan);
		if (error != 0)
			return error;

		/* A known missing picture still consumes a presentation clock position after POC reordering. */
		if (decoder->decode_order_times && decoder->dpb.started) {
			if (decoder->count >= VKVIDEO_PENDING)
				return EOVERFLOW;
			poc = decoder->picture.info.PicOrderCnt[0];
			if (decoder->picture.info.PicOrderCnt[1] < poc)
				poc = decoder->picture.info.PicOrderCnt[1];
			decoder->pending[decoder->count].picture = NULL;
			decoder->pending[decoder->count].poc = poc;
			decoder->pending[decoder->count].time_us = decoder->packet_time;
			decoder->count++;
		}

		/* The dropped picture supplies no decoded pixel ownership. */
		return 0;
	}

	/* Bound pending display storage before obtaining another CPU picture. */
	if (decoder->count >= VKVIDEO_PENDING)
		return EOVERFLOW;
	output = media_picture_pool_get(decoder->pool);
	if (output == NULL)
		return ENOMEM;
	error = media_vkvideo_runtime_decode(decoder->runtime, &decoder->stream, &decoder->picture, &plan, output);
	if (error != 0) {
		media_picture_unref(output);
		return error;
	}

	/* Commit reference marking only after GPU output completed successfully. */
	error = h264_dpb_mark(&decoder->dpb, sps, &decoder->picture, &plan);
	if (error != 0) {
		media_picture_unref(output);
		return error;
	}

	/* Display metadata accompanies the retained CPU picture rather than the Vulkan image. */
	h264_aspect(&decoder->stream, sps->seq_parameter_set_id, &output->aspect_num, &output->aspect_den);
	output->colour = decoder->colour;
	output->full_range = decoder->full_range;
	vui = &decoder->stream.vui[sps->seq_parameter_set_id];
	if (!sps->flags.vui_parameters_present_flag || !vui->flags.aspect_ratio_info_present_flag) {
		if (decoder->aspect_num != 0U && decoder->aspect_den != 0U) {
			output->aspect_num = decoder->aspect_num;
			output->aspect_den = decoder->aspect_den;
		}
	}

	/* Apply stream VUI colour metadata after the container's defaults. */
	if (sps->flags.vui_parameters_present_flag) {
		vui = &decoder->stream.vui[sps->seq_parameter_set_id];
		if (vui->flags.video_signal_type_present_flag)
			output->full_range = (int)vui->flags.video_full_range_flag;
		if (vui->matrix_coefficients == 1U)
			output->colour = MEDIA_COLOUR_709;
		else if (vui->matrix_coefficients == 9U || vui->matrix_coefficients == 10U)
			output->colour = MEDIA_COLOUR_2020;
	}

	/* Use the earlier field order as the progressive picture's presentation sort key. */
	poc = decoder->picture.info.PicOrderCnt[0];
	if (decoder->picture.info.PicOrderCnt[1] < poc)
		poc = decoder->picture.info.PicOrderCnt[1];
	decoder->pending[decoder->count].picture = output;
	decoder->pending[decoder->count].poc = poc;
	decoder->pending[decoder->count].time_us = decoder->packet_time;
	decoder->count++;

	/* Succeeded: the decoded CPU frame waits for its presentation position. */
	return 0;
}

/* Translate typed open problems to ordinary runtime errno without ambiguous positive media-problem values. */
static int
vkvideo_errno(
	int problem)
{
	/* Opening and packet admission use different result domains. */
	if (problem == MEDIA_PROBLEM_DEVICE)
		return ENODEV;
	if (problem == MEDIA_PROBLEM_PROFILE)
		return ENOTSUP;
	if (problem == MEDIA_PROBLEM_BUSY)
		return EBUSY;

	/* Other native implementation errors preserve their original errno. */
	return problem;
}

/* Translate internal errno only at track open, keeping packet and decode errors in their original domain. */
static int
vkvideo_problem(
	int error)
{
	/* Device admission does not imply a decodable native profile. */
	if (error == ENODEV || error == EIO)
		return MEDIA_PROBLEM_DEVICE;

	/* Unsupported sequence tools allow application software fallback. */
	if (error == ENOTSUP)
		return MEDIA_PROBLEM_PROFILE;

	/* A context-capacity refusal is distinct from an allocation failure. */
	if (error == EBUSY)
		return MEDIA_PROBLEM_BUSY;

	/* Invalid input and allocation failures remain ordinary errors. */
	return error;
}
