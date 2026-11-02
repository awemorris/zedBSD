/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Native H.265 decode through standard Vulkan Video (the Main profile,
 * 8-bit 4:2:0), on vkvideo-runtime.c's contexts.
 *
 * The stream's side is libmedia's H.265 parse: h265-stream splits the
 * packets into pictures in decoding order (with their order counts and
 * reference picture sets), h265-dpb keeps the decoded picture buffer (the
 * reference marking, the slot each picture is decoded into, the output
 * order of C.5.2) and h265-std makes the StdVideoH265 structures.  Each
 * picture is decoded into the slot the DPB takes for it, into a picture of
 * the pool (read back, or held by the GPU), and given out when the DPB
 * outputs it, so the pictures come out in the order the standard does.
 *
 * A slot whose image is still in use (a picture in flight, or one the
 * program holds on the GPU) is busy for the DPB.  A picture the DPB empties
 * without output (PicOutputFlag 0, or prior pictures dropped at an IRAP
 * picture) is let go when its slot leaves the DPB.  An end of sequence
 * does not empty the DPB: the IRAP picture after it does, as the standard
 * has it (h265_dpb_begin); only the end of the stream flushes it.  A new
 * sequence whose size, profile or DPB differs gets a new context, after
 * the pictures before it are given out.
 */

#include "h265-dpb.h"
#include "h265-std.h"
#include "h265-stream.h"
#include "media-private.h"
#include "vkvideo-runtime.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most slots a context has (vkvideo-runtime.c), and the pictures waiting to be given out at most. */
#define H265VK_SLOTS MEDIA_GPU_SLOTS
#define H265VK_QUEUE 40U

/* How many pictures whose decode failed may be skipped in a row where the device skips them (as vkvideo.c). */
#define H265VK_MAX_SKIPPED 8U

/* A picture waiting to be given out: the pool's picture, its time and its ticket (0 once its pixels are in). */
struct h265vk_output {
	struct media_picture *picture;
	int64_t time_us;
	uint32_t ticket;
};

/*
 * One decoder: its codec (MEDIA_CODEC_HEVC, the member vkvideo.c tells
 * the two codecs' decoders apart by), the packets' conversion, the reader and its picture, the
 * DPB and its plan, the context and its pool, the sequence the context was
 * made for, the parameter sets' generation the context's parameters hold,
 * each slot's picture (decoded and not yet given to the queue) with its
 * ticket and time, the slots the session holds active, the queue of
 * pictures to give out, and the StdVideo forms of the sets.
 */
struct h265vk_decoder {
	unsigned codec;
	struct media_bitstream bitstream;
	struct h265_stream *stream;
	struct h265_picture *picture;
	struct h265_dpb dpb;
	struct h265_dpb_plan plan;
	struct h265_dpb_output outputs[H265_DPB_MAX_OUTPUT];
	struct vkvideo_runtime *runtime;
	struct media_picture_pool *pool;
	struct vkvideo_shape shape;
	uint32_t crop[4];
	int configured;
	int has_parameters;
	unsigned generation;
	uint32_t slots;
	uint32_t active;
	struct media_picture *slot_picture[H265VK_SLOTS];
	uint32_t slot_ticket[H265VK_SLOTS];
	int64_t slot_time[H265VK_SLOTS];
	struct h265vk_output queue[H265VK_QUEUE];
	unsigned count;
	int have_picture;
	int begun;
	int draining;
	int flushed;
	int failed;
	unsigned skipped;
	int64_t packet_time;
	int decode_order_times;
	struct media_picture *current;
	uint32_t aspect_num;
	uint32_t aspect_den;
	unsigned colour;
	int full_range;
	struct h265_std_vps std_vps[H265SW_VPS_IDS];
	struct h265_std_sps std_sps[H265SW_SPS_IDS];
	struct h265_std_pps std_pps[H265SW_PPS_IDS];
	StdVideoH265VideoParameterSet vps[H265SW_VPS_IDS];
	StdVideoH265SequenceParameterSet sps[H265SW_SPS_IDS];
	StdVideoH265PictureParameterSet pps[H265SW_PPS_IDS];
};

static int h265vk_load(void);
static const char *h265vk_reason(void);
static int h265vk_open(const struct media_track *track, void **result);
static const char *h265vk_name(const void *state);
static int h265vk_send(void *state, const struct media_packet *packet);
static int h265vk_receive(void *state, int64_t *time_us);
static void *h265vk_picture(void *state);
static size_t h265vk_sound(void *state, int16_t *samples, size_t capacity, uint32_t rate);
static void h265vk_flush(void *state);
static void h265vk_close(void *state);
static int h265vk_shape(const struct h265sw_sps *sps, struct vkvideo_shape *shape);
static int h265vk_differs(const struct h265vk_decoder *decoder, const struct h265sw_sps *sps);
static int h265vk_configure(struct h265vk_decoder *decoder, const struct h265sw_sps *sps);
static int h265vk_parameters(struct h265vk_decoder *decoder);
static int h265vk_step(struct h265vk_decoder *decoder);
static int h265vk_decode(struct h265vk_decoder *decoder, const struct h265sw_sps *sps);
static int h265vk_output(struct h265vk_decoder *decoder, const struct h265_dpb_output *outputs, unsigned count);
static int h265vk_drop_unheld(struct h265vk_decoder *decoder);
static int h265vk_pop(struct h265vk_decoder *decoder, int64_t *time_us);
static int h265vk_settle(struct h265vk_decoder *decoder, uint32_t *ticket, struct media_picture **picture);
static void h265vk_forget(struct h265vk_decoder *decoder);
static void h265vk_dpb_reset(struct h265vk_decoder *decoder);
static int h265vk_problem(int error);

/*
 * The native H.265 operations, which media_vkvideo_ops (vkvideo.c) calls
 * for an H.265 track: the same picture operations as the H.264 back end's.
 */
const struct media_decoder_ops media_vkvideo_h265_ops = {
	"vulkan-video", h265vk_load, h265vk_reason, h265vk_open, h265vk_name,
	h265vk_send, h265vk_receive, h265vk_picture, h265vk_sound,
	h265vk_flush, h265vk_close, media_picture_unref, media_picture_size,
	media_picture_scale, media_picture_scaler_free, NULL, NULL, media_picture_aspect,
	media_picture_gpu};

/* Built-in dispatch is available; device and profile admission belong to opening a track. */
static int
h265vk_load(
	void)
{
	return 0;
}

/* Native video reports its reason with a typed admission result for each track. */
static const char *
h265vk_reason(
	void)
{
	return "";
}

/*
 * Opens an H.265 track: the container's parameter sets read, the first
 * SPS's context made (the device's admission of its profile, level and
 * size), or without one a small Main context made and closed again (the
 * device's admission of H.265 at all).
 */
static int
h265vk_open(
	const struct media_track *track,
	void **result)
{
	struct h265vk_decoder *decoder;
	struct vkvideo_runtime *runtime;
	struct vkvideo_shape probe;
	unsigned index;
	int error;

	/* Another codec is a normal backend-selection refusal. */
	*result = NULL;
	if (track->codec != MEDIA_CODEC_HEVC)
		return MEDIA_PROBLEM_FORMAT;
	decoder = calloc(1U, sizeof(*decoder));
	if (decoder == NULL)
		return ENOMEM;
	decoder->codec = MEDIA_CODEC_HEVC;
	decoder->stream = calloc(1U, sizeof(*decoder->stream));
	decoder->picture = calloc(1U, sizeof(*decoder->picture));
	if (decoder->stream == NULL || decoder->picture == NULL) {
		h265vk_close(decoder);
		return ENOMEM;
	}
	h265_stream_init(decoder->stream);
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

	/* The container's codec initialization as Annex B parameter sets, read into the store. */
	error = media_bitstream_open(&decoder->bitstream, track->codec, track->private_data, track->private_size);
	if (error != 0) {
		h265vk_close(decoder);
		return error;
	}
	if (decoder->bitstream.prefix_size != 0U)
		(void)h265_stream_parameters(decoder->stream, decoder->bitstream.prefix, decoder->bitstream.prefix_size);

	/* The first SPS's context, which the device admits or refuses. */
	for (index = 0U; index < H265SW_SPS_IDS; index++) {
		if (!decoder->stream->store.sps_present[index])
			continue;
		error = h265vk_configure(decoder, &decoder->stream->store.sps[index]);
		if (error != 0) {
			error = h265vk_problem(error);
			h265vk_close(decoder);
			return error;
		}
		break;
	}

	/* Without one, a small Main context asks whether the device decodes H.265 at all. */
	if (!decoder->configured) {
		memset(&probe, 0, sizeof(probe));
		probe.profile = STD_VIDEO_H265_PROFILE_IDC_MAIN;
		probe.level = STD_VIDEO_H265_LEVEL_IDC_3_0;
		probe.width = 64U;
		probe.height = 64U;
		probe.references = 1U;
		error = media_vkvideo_runtime_open_h265(&probe, 0U, &runtime);
		if (error != 0) {
			h265vk_close(decoder);
			return error;
		}
		media_vkvideo_runtime_close(runtime);
	}

	/* Succeeded: the device admits the track. */
	*result = decoder;
	return 0;
}

/* Identifies the codec, apart from the back end's name. */
static const char *
h265vk_name(
	const void *state)
{
	(void)state;
	return "h265";
}

/*
 * Takes one packet: its bytes converted to Annex B and given to the reader,
 * which borrows them until every picture of the packet is decoded (EAGAIN
 * until then).  NULL starts the drain.
 */
static int
h265vk_send(
	void *state,
	const struct media_packet *packet)
{
	struct h265vk_decoder *decoder;
	const uint8_t *data;
	size_t size;
	int error;

	/* A packet's pictures are decoded before the next packet's bytes replace them. */
	decoder = state;
	if (decoder->failed != 0)
		return decoder->failed;
	if (decoder->have_picture || decoder->stream->cursor < decoder->stream->size)
		return EAGAIN;
	if (packet == NULL) {
		decoder->draining = 1;
		return 0;
	}
	if (decoder->draining)
		return EINVAL;

	/* The packet as Annex B, the reader on it. */
	error = media_bitstream_convert(&decoder->bitstream, packet->data, packet->size, packet->keyframe, &data, &size);
	if (error != 0)
		return error;
	h265_stream_input(decoder->stream, data, size);
	decoder->packet_time = packet->pts_us;

	/* Succeeded: the packet's pictures are read by receive. */
	return 0;
}

/*
 * Gives the next picture in output order: 1 with its time, 0 when another
 * packet is needed (or the drain ended), or a negative errno.
 */
static int
h265vk_receive(
	void *state,
	int64_t *time_us)
{
	struct h265vk_decoder *decoder;
	int error;

	/* The previous receive's picture is let go. */
	decoder = state;
	if (decoder->current != NULL) {
		media_picture_unref(decoder->current);
		decoder->current = NULL;
	}

	/* A decoding error stays until a flush. */
	if (decoder->failed != 0)
		return -decoder->failed;
	for (;;) {
		/* A picture waiting is given out first. */
		if (decoder->count != 0U) {
			error = h265vk_pop(decoder, time_us);
			if (error < 0) {
				decoder->failed = -error;
				return error;
			}
			if (error > 0)
				return 1;
			continue;
		}

		/* The next step: a picture read, planned or decoded; 0 asks for a packet. */
		error = h265vk_step(decoder);
		if (error == EAGAIN)
			continue;
		if (error == ENOENT)
			return 0;
		if (error != 0) {
			decoder->failed = error;
			return -error;
		}
	}
}

/*
 * One step of the decode: reads the next picture (at the end of the
 * packets, flushes the DPB once while draining), plans it (h265_dpb_begin;
 * its outputs join the queue), makes a new context when its sequence needs
 * one (after the queue is empty), and decodes it.  EAGAIN when the queue
 * has pictures to give out first, ENOENT when a packet is needed (or the
 * drain ended), 0 after a decode.
 */
static int
h265vk_step(
	struct h265vk_decoder *decoder)
{
	const struct h265sw_sps *sps;
	unsigned count;
	int irap;
	int error;

	/* The next picture, or the end of the packets. */
	if (!decoder->have_picture) {
		error = h265_stream_next(decoder->stream, decoder->picture);
		if (error == ENOENT) {
			if (!decoder->draining || decoder->flushed)
				return ENOENT;
			decoder->flushed = 1;
			error = h265_dpb_flush(&decoder->dpb, decoder->outputs, &count);
			if (error == 0)
				error = h265vk_output(decoder, decoder->outputs, count);
			if (error == 0)
				error = h265vk_drop_unheld(decoder);
			if (error != 0)
				return error;
			return EAGAIN;
		}
		if (error != 0)
			return error;
		decoder->have_picture = 1;
		decoder->begun = 0;
	}
	sps = &decoder->stream->store.sps[decoder->picture->sps_id];
	irap = decoder->picture->irap != 0 && decoder->picture->no_rasl_output != 0;

	/* A sequence the context was not made for: the pictures before it go out, then a new context. */
	if (!decoder->begun && (!decoder->configured || h265vk_differs(decoder, sps))) {
		if (!irap) {
			decoder->have_picture = 0;
			return EAGAIN;
		}
		if (decoder->configured && decoder->dpb.started) {
			if (decoder->picture->no_output_of_prior_pics) {
				h265_dpb_clear(&decoder->dpb);
				count = 0;
			} else {
				error = h265_dpb_flush(&decoder->dpb, decoder->outputs, &count);
				if (error != 0)
					return error;
			}
			error = h265vk_output(decoder, decoder->outputs, count);
			if (error == 0)
				error = h265vk_drop_unheld(decoder);
			if (error != 0)
				return error;
			return EAGAIN;
		}
		if (decoder->count != 0U)
			return EAGAIN;
		error = h265vk_configure(decoder, sps);
		if (error != 0)
			return error;
	}

	/* The parameters of the sets the stream has published. */
	if (!decoder->has_parameters || decoder->generation != decoder->stream->store.generation) {
		error = h265vk_parameters(decoder);
		if (error != 0)
			return error;
	}

	/* The marking and bumping before the picture (C.5.2.2); a picture the DPB cannot hold is skipped. */
	if (!decoder->begun) {
		error = h265_dpb_begin(&decoder->dpb, sps, &decoder->picture->rps, irap, decoder->picture->no_output_of_prior_pics, &decoder->plan);
		if (error == EINVAL) {
			error = h265vk_output(decoder, decoder->plan.output, decoder->plan.output_count);
			if (error == 0)
				error = h265vk_drop_unheld(decoder);
			decoder->have_picture = 0;
			if (error != 0)
				return error;
			return EAGAIN;
		}
		if (error != 0)
			return error;
		decoder->begun = 1;
		error = h265vk_output(decoder, decoder->plan.output, decoder->plan.output_count);
		if (error == 0)
			error = h265vk_drop_unheld(decoder);
		if (error != 0)
			return error;
		if (decoder->count != 0U)
			return EAGAIN;
	}

	/* The decode, and the outputs after it. */
	error = h265vk_decode(decoder, sps);
	decoder->have_picture = 0;
	decoder->begun = 0;
	return error;
}

/* Gives out the next picture of the queue, its pixels in first: 1, 0 for a skipped one, or a negative errno. */
static int
h265vk_pop(
	struct h265vk_decoder *decoder,
	int64_t *time_us)
{
	struct h265vk_output *first;
	unsigned index;
	int error;

	/* The first picture, completed. */
	first = &decoder->queue[0];
	error = h265vk_settle(decoder, &first->ticket, &first->picture);
	if (error != 0)
		return -error;
	decoder->current = first->picture;
	*time_us = first->time_us;
	for (index = 1U; index < decoder->count; index++)
		decoder->queue[index - 1U] = decoder->queue[index];
	decoder->count--;

	/* A skipped picture gives nothing. */
	if (decoder->current == NULL)
		return 0;
	return 1;
}

/*
 * Completes a picture still in flight: 0 with its pixels in; a failed
 * decode is skipped (*picture let go and NULL) where the device skips them,
 * a few in a row; else EIO.
 */
static int
h265vk_settle(
	struct h265vk_decoder *decoder,
	uint32_t *ticket,
	struct media_picture **picture)
{
	int error;

	/* Nothing in flight. */
	if (*ticket == 0U)
		return 0;
	error = media_vkvideo_runtime_finish(decoder->runtime, *ticket);
	*ticket = 0U;

	/* A failed decode where the device skips them. */
	if (error == EBADMSG && media_vkvideo_runtime_skips_failed(decoder->runtime) && decoder->skipped < H265VK_MAX_SKIPPED) {
		decoder->skipped++;
		if (*picture != NULL)
			media_picture_unref(*picture);
		*picture = NULL;
		return 0;
	}
	if (error == EBADMSG)
		error = EIO;
	if (error != 0)
		return error;

	/* Succeeded: the pixels are in. */
	decoder->skipped = 0;
	return 0;
}

/*
 * Moves the DPB's outputs into the queue in their order, each with its
 * time: the picture's own, or with no container times the smallest of the
 * pictures decoded and not given out yet.  Each slot is released at once:
 * a slot's image stays busy for the DPB while the context uses it.
 */
static int
h265vk_output(
	struct h265vk_decoder *decoder,
	const struct h265_dpb_output *outputs,
	unsigned count)
{
	struct h265vk_output *entry;
	unsigned index;
	unsigned slot;
	unsigned other;
	unsigned earliest;
	int64_t time;

	/* Each output in order. */
	for (index = 0U; index < count; index++) {
		if (outputs[index].slot < 0 || (unsigned)outputs[index].slot >= H265VK_SLOTS)
			continue;
		slot = (unsigned)outputs[index].slot;
		h265_dpb_release(&decoder->dpb, outputs[index].slot);
		if (decoder->slot_picture[slot] == NULL)
			continue;
		if (decoder->count >= H265VK_QUEUE)
			return EOVERFLOW;

		/* Its time: its own, or the smallest still waiting (swapped with it). */
		time = decoder->slot_time[slot];
		if (decoder->decode_order_times) {
			earliest = slot;
			for (other = 0U; other < H265VK_SLOTS; other++) {
				if (decoder->slot_picture[other] != NULL && decoder->slot_time[other] < decoder->slot_time[earliest])
					earliest = other;
			}
			time = decoder->slot_time[earliest];
			decoder->slot_time[earliest] = decoder->slot_time[slot];
		}

		/* The picture joins the queue; the slot no longer has it. */
		entry = &decoder->queue[decoder->count];
		entry->picture = decoder->slot_picture[slot];
		entry->ticket = decoder->slot_ticket[slot];
		entry->time_us = time;
		decoder->count++;
		decoder->slot_picture[slot] = NULL;
		decoder->slot_ticket[slot] = 0U;
	}

	/* Succeeded: the outputs wait in the queue. */
	return 0;
}

/*
 * Lets go of the pictures of slots the DPB no longer holds that were never
 * given out (PicOutputFlag 0, or dropped prior pictures), once their
 * decodes have completed.
 */
static int
h265vk_drop_unheld(
	struct h265vk_decoder *decoder)
{
	unsigned slot;
	unsigned index;
	int held;
	int error;

	/* Each slot with a picture. */
	for (slot = 0U; slot < H265VK_SLOTS; slot++) {
		if (decoder->slot_picture[slot] == NULL)
			continue;
		held = 0;
		for (index = 0U; index < H265_DPB_PICTURES; index++) {
			if (decoder->dpb.entry[index].used && decoder->dpb.entry[index].slot == (int)slot)
				held = 1;
		}
		if ((int)slot == decoder->dpb.current)
			held = 1;
		if (held)
			continue;

		/* Its decode completes before its picture goes back to the pool. */
		error = 0;
		if (decoder->slot_ticket[slot] != 0U)
			error = media_vkvideo_runtime_finish(decoder->runtime, decoder->slot_ticket[slot]);
		decoder->slot_ticket[slot] = 0U;
		if (decoder->slot_picture[slot] != NULL)
			media_picture_unref(decoder->slot_picture[slot]);
		decoder->slot_picture[slot] = NULL;
		if (error != 0 && error != EBADMSG && error != ECANCELED)
			return error;
	}

	/* Succeeded: only the DPB's slots keep pictures. */
	return 0;
}

/*
 * Decodes the planned picture: a free submission, the slot (the context's
 * busy slots and those past its slots busy for the DPB), the StdVideo
 * records, the job, the DPB's storage after it (C.5.2.3) and its outputs.
 */
static int
h265vk_decode(
	struct h265vk_decoder *decoder,
	const struct h265sw_sps *sps)
{
	StdVideoDecodeH265PictureInfo info;
	StdVideoDecodeH265ReferenceInfo references[H265_DPB_PICTURES];
	StdVideoDecodeH265ReferenceInfo setup;
	int32_t reference_slots[H265_DPB_PICTURES];
	int32_t deactivate[H265VK_SLOTS];
	struct vkvideo_job job;
	struct media_picture *output;
	const struct h265sw_vui *vui;
	uint32_t ticket;
	uint32_t kept;
	uint32_t mask;
	unsigned count;
	unsigned index;
	int slot;
	int error;

	/* A free submission first, then the slot no image in use is. */
	error = media_vkvideo_runtime_reserve(decoder->runtime);
	if (error != 0)
		return error;
	mask = 0U;
	if (decoder->dpb.slots > decoder->slots)
		mask = ~0U << decoder->slots;
	decoder->dpb.busy = media_vkvideo_runtime_busy(decoder->runtime) | mask;
	error = h265_dpb_take_slot(&decoder->dpb, &decoder->plan);
	decoder->dpb.busy = 0U;
	if (error != 0) {
		h265_dpb_abort(&decoder->dpb);
		return error;
	}
	slot = decoder->plan.setup_slot;

	/* An old picture of the slot (never given out) goes once its decode is done. */
	if (decoder->slot_picture[slot] != NULL) {
		if (decoder->slot_ticket[slot] != 0U)
			(void)media_vkvideo_runtime_finish(decoder->runtime, decoder->slot_ticket[slot]);
		media_picture_unref(decoder->slot_picture[slot]);
		decoder->slot_picture[slot] = NULL;
		decoder->slot_ticket[slot] = 0U;
	}

	/* The StdVideo records of the picture, its references and its slot. */
	error = h265_std_picture(decoder->picture, &decoder->plan, &info);
	if (error != 0) {
		h265_dpb_abort(&decoder->dpb);
		return error;
	}
	h265_std_references(&decoder->plan, references);
	h265_std_setup(decoder->picture->poc, &setup);

	/* The scope keeps the DPB's references active and lets the other active slots go (a reset: none is active). */
	if (decoder->plan.reset)
		decoder->active = 0U;
	kept = 0U;
	for (index = 0U; index < decoder->plan.reference_count; index++) {
		reference_slots[index] = decoder->plan.reference_slot[index];
		kept |= 1U << (unsigned)decoder->plan.reference_slot[index];
	}
	count = 0U;
	for (index = 0U; index < decoder->slots && index < H265VK_SLOTS; index++) {
		if ((decoder->active & ~kept & (1U << index)) != 0U) {
			deactivate[count] = (int32_t)index;
			count++;
		}
	}

	/* The pool's picture, with the sequence's display information. */
	output = media_picture_pool_get(decoder->pool);
	if (output == NULL) {
		h265_dpb_abort(&decoder->dpb);
		return ENOMEM;
	}
	output->aspect_num = decoder->aspect_num;
	output->aspect_den = decoder->aspect_den;
	output->colour = decoder->colour;
	output->full_range = decoder->full_range;
	if (sps->vui_present) {
		vui = &sps->vui;
		if (vui->aspect_ratio_present && vui->sar_width != 0U && vui->sar_height != 0U) {
			output->aspect_num = vui->sar_width;
			output->aspect_den = vui->sar_height;
		}
		if (vui->signal_present)
			output->full_range = vui->full_range;
		if (vui->colour_present) {
			if (vui->matrix_coeffs == 1U)
				output->colour = MEDIA_COLOUR_709;
			else if (vui->matrix_coeffs == 9U || vui->matrix_coeffs == 10U)
				output->colour = MEDIA_COLOUR_2020;
		}
	}

	/* The job. */
	memset(&job, 0, sizeof(job));
	job.data = decoder->stream->data;
	job.slice_count = decoder->picture->slice_count;
	job.slice_offsets = decoder->picture->slice_offset;
	job.slice_sizes = decoder->picture->slice_size;
	job.setup = slot;
	job.reset = decoder->plan.reset;
	job.reference_count = decoder->plan.reference_count;
	job.references = reference_slots;
	job.reference_info = references;
	job.deactivate_count = count;
	job.deactivate = deactivate;
	job.setup_info = &setup;
	job.picture_info = &info;
	job.width = sps->width;
	job.height = sps->height;
	job.crop_x = sps->crop_x;
	job.crop_y = sps->crop_y;
	error = media_vkvideo_runtime_submit_h265(decoder->runtime, &job, output, &ticket);
	if (error != 0) {
		media_picture_unref(output);
		h265_dpb_abort(&decoder->dpb);
		return error;
	}
	decoder->active = kept | (1U << (unsigned)slot);
	decoder->slot_picture[slot] = output;
	decoder->slot_ticket[slot] = ticket;
	decoder->slot_time[slot] = decoder->packet_time;

	/* The picture in the DPB (C.5.2.3), and what is given out after it. */
	error = h265_dpb_end(&decoder->dpb, &decoder->plan, decoder->picture->poc, decoder->picture->pic_output, decoder->picture->index,
			     decoder->outputs, &count);
	if (error != 0)
		return error;
	error = h265vk_output(decoder, decoder->outputs, count);
	if (error == 0)
		error = h265vk_drop_unheld(decoder);
	return error;
}

/* The context's shape of an SPS: Main, 8-bit 4:2:0; ENOTSUP for another. */
static int
h265vk_shape(
	const struct h265sw_sps *sps,
	struct vkvideo_shape *shape)
{
	StdVideoH265ProfileTierLevel ptl;
	int error;

	/* The profile StdVideo names, which must be Main. */
	error = h265_std_ptl(&sps->ptl, &ptl);
	if (error != 0 || ptl.general_profile_idc != STD_VIDEO_H265_PROFILE_IDC_MAIN)
		return ENOTSUP;
	if (sps->chroma_format_idc != 1U || sps->bit_depth_luma != 8U || sps->bit_depth_chroma != 8U)
		return ENOTSUP;

	/* The level, the coded size, the DPB's references. */
	memset(shape, 0, sizeof(*shape));
	shape->codec = VKVIDEO_H265;
	shape->profile = (uint32_t)ptl.general_profile_idc;
	shape->level = (uint32_t)ptl.general_level_idc;
	shape->width = sps->width;
	shape->height = sps->height;
	shape->references = sps->max_dec_pic_buffering_minus1[sps->max_sub_layers_minus1];
	if (shape->references == 0U)
		shape->references = 1U;
	return 0;
}

/* Reports whether an SPS needs another context than the one made (its shape or crop differ, or it is refused). */
static int
h265vk_differs(
	const struct h265vk_decoder *decoder,
	const struct h265sw_sps *sps)
{
	struct vkvideo_shape shape;
	int error;

	/* Another shape, or one not taken. */
	error = h265vk_shape(sps, &shape);
	if (error != 0)
		return 1;
	if (shape.profile != decoder->shape.profile || shape.level > decoder->shape.level || shape.width != decoder->shape.width ||
	    shape.height != decoder->shape.height || shape.references > decoder->shape.references)
		return 1;

	/* Another crop makes another pool. */
	if (sps->crop_x != decoder->crop[0] || sps->crop_y != decoder->crop[1] || sps->crop_width != decoder->crop[2] ||
	    sps->crop_height != decoder->crop[3])
		return 1;
	return 0;
}

/*
 * Makes the context and the pool for an SPS (the queue is empty), in place
 * of the ones before, with an empty DPB.
 */
static int
h265vk_configure(
	struct h265vk_decoder *decoder,
	const struct h265sw_sps *sps)
{
	struct vkvideo_shape shape;
	struct vkvideo_runtime *runtime;
	struct media_picture_pool *pool;
	unsigned reorder;
	unsigned pictures;
	int error;

	/* The shape the device must take. */
	error = h265vk_shape(sps, &shape);
	if (error != 0)
		return error;
	reorder = sps->max_num_reorder_pics[sps->max_sub_layers_minus1];
	error = media_vkvideo_runtime_open_h265(&shape, reorder, &runtime);
	if (error != 0) {
		if (error == MEDIA_PROBLEM_DEVICE)
			return ENODEV;
		if (error == MEDIA_PROBLEM_PROFILE)
			return ENOTSUP;
		if (error == MEDIA_PROBLEM_BUSY)
			return EBUSY;
		return error;
	}

	/* The pool of the crop: the DPB's pictures, those in flight and a few more. */
	pictures = shape.references + 4U + media_vkvideo_runtime_flights(runtime);
	if (media_vkvideo_runtime_gpu(runtime))
		pool = media_picture_pool_create_gpu(sps->crop_width, sps->crop_height, pictures);
	else
		pool = media_picture_pool_create(sps->crop_width, sps->crop_height, pictures);
	if (pool == NULL) {
		media_vkvideo_runtime_close(runtime);
		return ENOMEM;
	}

	/* The old context's pictures go first, then the old context and pool. */
	if (decoder->runtime != NULL)
		media_vkvideo_runtime_discard(decoder->runtime);
	h265vk_forget(decoder);
	media_vkvideo_runtime_close(decoder->runtime);
	media_picture_pool_close(decoder->pool);
	decoder->runtime = runtime;
	decoder->pool = pool;
	decoder->shape = shape;
	decoder->crop[0] = sps->crop_x;
	decoder->crop[1] = sps->crop_y;
	decoder->crop[2] = sps->crop_width;
	decoder->crop[3] = sps->crop_height;
	decoder->slots = media_vkvideo_runtime_slots(runtime);
	decoder->configured = 1;
	decoder->has_parameters = 0;
	h265vk_dpb_reset(decoder);

	/* Succeeded: a context for the sequence, its parameters made before its first decode. */
	return 0;
}

/* Makes the context's parameters from every set the store holds that StdVideo can name. */
static int
h265vk_parameters(
	struct h265vk_decoder *decoder)
{
	VkVideoDecodeH265SessionParametersAddInfoKHR add;
	const struct h265sw_ps_store *store;
	uint32_t vps_count;
	uint32_t sps_count;
	uint32_t pps_count;
	unsigned index;
	int error;

	/* Each set made where it lives, then copied (shallow) into the add's arrays. */
	store = &decoder->stream->store;
	vps_count = 0U;
	for (index = 0U; index < H265SW_VPS_IDS; index++) {
		if (!store->vps_present[index] || h265_std_vps(&store->vps[index], &decoder->std_vps[index]) != 0)
			continue;
		decoder->vps[vps_count] = decoder->std_vps[index].vps;
		vps_count++;
	}
	sps_count = 0U;
	for (index = 0U; index < H265SW_SPS_IDS; index++) {
		if (!store->sps_present[index] || h265_std_sps(&store->sps[index], &decoder->std_sps[index]) != 0)
			continue;
		decoder->sps[sps_count] = decoder->std_sps[index].sps;
		sps_count++;
	}
	pps_count = 0U;
	for (index = 0U; index < H265SW_PPS_IDS; index++) {
		if (!store->pps_present[index] || !store->sps_present[store->pps[index].sps_id])
			continue;
		if (h265_std_pps(&store->pps[index], &store->sps[store->pps[index].sps_id], &decoder->std_pps[index]) != 0)
			continue;
		decoder->pps[pps_count] = decoder->std_pps[index].pps;
		pps_count++;
	}

	/* The parameters object, then the generation it holds. */
	memset(&add, 0, sizeof(add));
	add.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR;
	add.stdVPSCount = vps_count;
	add.pStdVPSs = decoder->vps;
	add.stdSPSCount = sps_count;
	add.pStdSPSs = decoder->sps;
	add.stdPPSCount = pps_count;
	add.pStdPPSs = decoder->pps;
	error = media_vkvideo_runtime_parameters_h265(decoder->runtime, &add);
	if (error != 0)
		return error;
	decoder->generation = store->generation;
	decoder->has_parameters = 1;
	return 0;
}

/* Gives the current picture to the caller, who holds it apart from the next receive. */
static void *
h265vk_picture(
	void *state)
{
	struct h265vk_decoder *decoder;

	decoder = state;
	if (decoder->current == NULL)
		return NULL;
	media_picture_ref(decoder->current);
	return decoder->current;
}

/* H.265 video has no sound. */
static size_t
h265vk_sound(
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

/* A seek: the pictures in flight retire, every picture goes, the reader waits for an IRAP picture. */
static void
h265vk_flush(
	void *state)
{
	struct h265vk_decoder *decoder;

	decoder = state;
	if (decoder->runtime != NULL)
		media_vkvideo_runtime_discard(decoder->runtime);
	h265vk_forget(decoder);
	if (decoder->current != NULL) {
		media_picture_unref(decoder->current);
		decoder->current = NULL;
	}
	h265_stream_reset(decoder->stream);
	h265vk_dpb_reset(decoder);
	if (decoder->runtime != NULL)
		media_vkvideo_runtime_reset(decoder->runtime);
	decoder->have_picture = 0;
	decoder->begun = 0;
	decoder->draining = 0;
	decoder->flushed = 0;
	decoder->failed = 0;
}

/* Closes the decoder: the context retires before the pictures and the pool go. */
static void
h265vk_close(
	void *state)
{
	struct h265vk_decoder *decoder;

	decoder = state;
	if (decoder == NULL)
		return;
	if (decoder->runtime != NULL)
		media_vkvideo_runtime_discard(decoder->runtime);
	h265vk_forget(decoder);
	media_vkvideo_runtime_close(decoder->runtime);
	if (decoder->current != NULL)
		media_picture_unref(decoder->current);
	media_picture_pool_close(decoder->pool);
	media_bitstream_close(&decoder->bitstream);
	if (decoder->stream != NULL)
		h265_stream_destroy(decoder->stream);
	free(decoder->stream);
	free(decoder->picture);
	free(decoder);
}

/* Lets go of every picture the slots and the queue hold (their decodes discarded or done). */
static void
h265vk_forget(
	struct h265vk_decoder *decoder)
{
	unsigned index;

	for (index = 0U; index < H265VK_SLOTS; index++) {
		if (decoder->slot_picture[index] != NULL)
			media_picture_unref(decoder->slot_picture[index]);
		decoder->slot_picture[index] = NULL;
		decoder->slot_ticket[index] = 0U;
	}
	for (index = 0U; index < decoder->count; index++) {
		if (decoder->queue[index].picture != NULL)
			media_picture_unref(decoder->queue[index].picture);
	}
	decoder->count = 0U;
	decoder->active = 0U;
}

/* An empty DPB over the context's slots (the DPB counts sixteen at least; the ones past the context's stay busy). */
static void
h265vk_dpb_reset(
	struct h265vk_decoder *decoder)
{
	h265_dpb_init(&decoder->dpb, decoder->slots);
	decoder->active = 0U;
}

/* The open's problem of an errno (MEDIA_PROBLEM_*), others as they are. */
static int
h265vk_problem(
	int error)
{
	if (error == ENODEV || error == EIO)
		return MEDIA_PROBLEM_DEVICE;
	if (error == ENOTSUP)
		return MEDIA_PROBLEM_PROFILE;
	if (error == EBUSY)
		return MEDIA_PROBLEM_BUSY;
	return error;
}
