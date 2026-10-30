/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The software H.264 decoder as a whole: NAL units in, frames out.
 */

#include "h264sw-decoder.h"

#include "h264sw-cabac.h"
#include "h264sw-deblock.h"
#include "h264sw-direct.h"
#include "h264sw-macroblock.h"
#include "h264sw-mb.h"
#include "h264sw-nal.h"
#include "h264sw-reflist.h"
#include "h264sw-transform.h"
#include "h264sw-weight.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The longest NAL unit taken: a larger one is refused rather than buffered. */
#define DECODER_MAX_NAL (64U * 1024U * 1024U)

/*
 * The values of the decoder's waiting field.
 *
 * DECODER_RUNNING: a picture has started decoding since the start or the
 * last reset.  DECODER_WAITING: none has, and no slice has been dropped
 * since.  DECODER_WAITING_DROPPED: none has, and previous holds the
 * header of the last slice dropped while waiting, so that the slices of
 * one dropped picture count it once.
 */
#define DECODER_RUNNING 0
#define DECODER_WAITING 1
#define DECODER_WAITING_DROPPED 2

/* The last nal_unit_type of the range 15..18 that begins an access unit (7.4.1.2.3). */
#define DECODER_NAL_RESERVED_18 18U

/* The sample value of a macroblock that nothing better can fill (the middle of the 8-bit range). */
#define DECODER_GREY 128

/* Static function declarations. */
static int decoder_grow_rbsp(struct h264sw_decoder *decoder, size_t size);
static int decoder_ends_access_unit(unsigned type);
static int decoder_parameter_set(struct h264sw_decoder *decoder, const struct h264sw_nal *nal);
static int decoder_slice(struct h264sw_decoder *decoder, const struct h264sw_nal *nal, int64_t time_us);
static int decoder_activate(struct h264sw_decoder *decoder, const struct h264sw_pps *pps);
static int decoder_size_arrays(struct h264sw_decoder *decoder);
static int decoder_start_picture(struct h264sw_decoder *decoder, const struct h264sw_pps *pps, int64_t time_us);
static int decoder_decode_slice(struct h264sw_decoder *decoder, struct vcodec_bitreader *bits);
static int decoder_finish_picture(struct h264sw_decoder *decoder);
static void decoder_conceal(struct h264sw_decoder *decoder);
static void decoder_conceal_samples(struct h264sw_frame *frame, unsigned x, unsigned y, const struct h264sw_frame *source);
static void decoder_drop_picture(struct h264sw_decoder *decoder);
static void decoder_retime(struct h264sw_decoder *decoder, struct h264sw_frame *taken);

/*
 * Makes a decoder in its start state.
 *
 * Every part starts empty: no parameter set, no sequence, no picture,
 * an empty DPB and output queue, waiting for a picture to start from.
 */
int
h264sw_decoder_create(
	struct h264sw_decoder **decoder)
{
	struct h264sw_decoder *made;

	/* Nothing can be returned without the place to return it. */
	if (decoder == NULL)
		return EINVAL;
	*decoder = NULL;

	/* Allocates the decoder with every field cleared. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Starts the parts that have a start state of their own. */
	h264sw_ps_init(&made->ps);
	h264sw_output_init(&made->output);
	h264sw_poc_init(&made->poc);

	/* No sequence yet: the DPB has no long-term index until an SPS is activated. */
	made->dpb.max_long_term_frame_idx = -1;

	/* The decoder waits for an IDR picture or an I slice. */
	made->waiting = DECODER_WAITING;

	/* Publishes the decoder. */
	*decoder = made;

	/* Succeeded: the caller owns the decoder. */
	return 0;
}

/*
 * Frees a decoder: the picture in progress is dropped, the DPB and the output queue cleared, the pool destroyed.
 *
 * The pool is only marked dead while the caller still holds frames;
 * those frames free it with their last unref.
 */
void
h264sw_decoder_destroy(
	struct h264sw_decoder *decoder)
{
	/* Nothing to free. */
	if (decoder == NULL)
		return;

	/* Releases every frame the decoder holds. */
	decoder_drop_picture(decoder);
	h264sw_dpb_clear(&decoder->dpb);
	h264sw_output_clear(&decoder->output);
	h264sw_frame_unref(decoder->last_output);
	decoder->last_output = NULL;

	/* Destroys the pool; frames the caller still holds keep it until their last unref. */
	h264sw_frame_pool_destroy(decoder->pool);
	decoder->pool = NULL;

	/* Frees the arrays and the decoder itself. */
	free(decoder->mbs);
	free(decoder->slices);
	free(decoder->rbsp);
	free(decoder);
}

/*
 * Decodes one NAL unit.
 *
 * A NAL unit that begins an access unit first finishes the picture in
 * progress; then the parameter sets are stored and the slices decoded,
 * and the others are ignored.
 */
int
h264sw_decoder_nal(
	struct h264sw_decoder *decoder,
	const uint8_t *nal,
	size_t size,
	int64_t time_us)
{
	struct h264sw_nal unit;
	int boundary;
	int error;

	/* Refuses the calls that cannot work. */
	if (decoder == NULL)
		return EINVAL;
	if (nal == NULL)
		return EINVAL;
	if (size == 0U)
		return EINVAL;

	/* The caller takes the frames already output before it gives more. */
	if (decoder->output.count != 0U)
		return EBUSY;

	/* Refuses a NAL unit too large to buffer; the decoder is unchanged. */
	if (size > DECODER_MAX_NAL)
		return EINVAL;

	/* Makes room for the RBSP, which is never longer than the NAL unit. */
	error = decoder_grow_rbsp(decoder, size);
	if (error != 0)
		return error;

	/* Reads the NAL unit header and removes the emulation prevention bytes (7.3.1, 7.4.1). */
	error = h264sw_nal_parse(nal, size, decoder->rbsp, decoder->rbsp_capacity, &unit);
	if (error != 0)
		return EINVAL;

	/* The extension NAL units of SVC and MVC streams sit among the base layer's slices and are not read (7.4.1.2.3 NOTE 1). */
	if (unit.type == H264SW_NAL_PREFIX ||
	    unit.type == H264SW_NAL_SLICE_EXTENSION ||
	    unit.type == H264SW_NAL_SLICE_DEPTH)
		return 0;

	/* Data partitioning is outside the decoder's range. */
	if (unit.type == H264SW_NAL_PARTITION_A ||
	    unit.type == H264SW_NAL_PARTITION_B ||
	    unit.type == H264SW_NAL_PARTITION_C) {
		decoder->stats.unsupported++;
		return ENOTSUP;
	}

	/* Takes a slice, which finds out itself whether it starts a new picture (7.4.1.2.4). */
	if (unit.type == H264SW_NAL_SLICE || unit.type == H264SW_NAL_IDR) {
		error = decoder_slice(decoder, &unit, time_us);
		if (error != 0)
			return error;

		/* Succeeded: the slice is decoded, or dropped as the flow says. */
		return 0;
	}

	/* Finishes the picture in progress at a NAL unit that starts an access unit or ends a sequence (7.4.1.2.3). */
	boundary = decoder_ends_access_unit(unit.type);
	if (boundary) {
		error = decoder_finish_picture(decoder);
		if (error != 0)
			return error;
	}

	/* Stores a parameter set; every other NAL unit carries nothing the decoder reads. */
	if (unit.type == H264SW_NAL_SPS || unit.type == H264SW_NAL_PPS) {
		error = decoder_parameter_set(decoder, &unit);
		if (error != 0)
			return error;
	}

	/* Succeeded: the NAL unit is taken. */
	return 0;
}

/*
 * Finishes the picture in progress, for a caller that knows the access unit has ended, so the picture does not wait for the next NAL unit.
 *
 * Refuses with EBUSY, like h264sw_decoder_nal, while output frames are
 * waiting to be taken.
 */
int
h264sw_decoder_end_access_unit(
	struct h264sw_decoder *decoder)
{
	int error;

	/* Refuses a call without a decoder. */
	if (decoder == NULL)
		return EINVAL;

	/* The caller takes the frames already output first. */
	if (decoder->output.count != 0U)
		return EBUSY;

	/* Finishes the picture; nothing happens when none is in progress. */
	error = decoder_finish_picture(decoder);
	if (error != 0)
		return error;

	/* Succeeded: no picture is in progress. */
	return 0;
}

/*
 * Ends the stream: finishes the picture in progress and outputs every frame still waiting in the DPB (h264sw_dpb_flush), in output order.
 *
 * Refuses with EBUSY while output frames are waiting to be taken, so
 * the output queue has room for the whole DPB.  Afterwards the order
 * count state is cleared and the decoder waits for a new start.
 */
int
h264sw_decoder_flush(
	struct h264sw_decoder *decoder)
{
	int error;

	/* Refuses a call without a decoder. */
	if (decoder == NULL)
		return EINVAL;

	/* The caller takes the frames already output first. */
	if (decoder->output.count != 0U)
		return EBUSY;

	/* Finishes the picture in progress, which stores it in the DPB. */
	error = decoder_finish_picture(decoder);
	if (error != 0)
		return error;

	/* Outputs every frame the DPB still holds for output and empties it (C.4.5.3). */
	error = h264sw_dpb_flush(&decoder->dpb, &decoder->output);
	if (error != 0)
		return error;

	/* Starts the order counts and the long-term indices afresh, as after a reset. */
	h264sw_poc_init(&decoder->poc);
	decoder->dpb.max_long_term_frame_idx = -1;

	/* Waits for a picture that can start decoding. */
	decoder->waiting = DECODER_WAITING;
	decoder->leading = 0;

	/* Succeeded: every frame is in the output queue. */
	return 0;
}

/*
 * Takes the next output frame with its reference (the caller unrefs it), or NULL when none is ready.
 *
 * The queue's reference passes to the caller; the decoder takes a
 * reference of its own for last_output.
 */
struct h264sw_frame *
h264sw_decoder_output(
	struct h264sw_decoder *decoder)
{
	struct h264sw_frame *frame;

	/* No decoder, no frame. */
	if (decoder == NULL)
		return NULL;

	/* Takes the oldest frame of the output queue. */
	frame = h264sw_output_take(&decoder->output);
	if (frame == NULL)
		return NULL;

	/* Gives the frame the earliest time when the caller's times are in decoding order. */
	if (decoder->decode_order_times)
		decoder_retime(decoder, frame);

	/* The frame becomes the concealment's fall-back: the decoder's own reference moves to it. */
	h264sw_frame_ref(frame);
	h264sw_frame_unref(decoder->last_output);
	decoder->last_output = frame;
	decoder->stats.output++;

	/* Succeeded: the caller holds the frame. */
	return frame;
}

/*
 * Drops the picture in progress, the DPB, the output queue and the order count state, for a seek; the parameter sets and the frame pool are kept.
 *
 * The active sequence is kept too, so a picture of the same sequence
 * starts without a new pool.
 */
void
h264sw_decoder_reset(
	struct h264sw_decoder *decoder)
{
	/* Nothing to reset. */
	if (decoder == NULL)
		return;

	/* Drops every frame of the old position in the stream. */
	decoder_drop_picture(decoder);
	h264sw_dpb_clear(&decoder->dpb);
	h264sw_output_clear(&decoder->output);

	/* Starts the order counts and the long-term indices afresh. */
	h264sw_poc_init(&decoder->poc);
	decoder->dpb.max_long_term_frame_idx = -1;

	/* Waits for a picture that can start decoding. */
	decoder->waiting = DECODER_WAITING;
	decoder->leading = 0;
}

/*
 * Makes the RBSP buffer at least size bytes long.
 */
static int
decoder_grow_rbsp(
	struct h264sw_decoder *decoder,
	size_t size)
{
	uint8_t *grown;

	/* The buffer is already large enough. */
	if (decoder->rbsp_capacity >= size)
		return 0;

	/* Replaces the buffer; its old contents are not needed. */
	grown = malloc(size);
	if (grown == NULL)
		return ENOMEM;
	free(decoder->rbsp);
	decoder->rbsp = grown;
	decoder->rbsp_capacity = size;

	/* Succeeded: the buffer holds size bytes. */
	return 0;
}

/*
 * Reports whether a NAL unit type other than a slice starts a new access unit or ends a sequence (7.4.1.2.3).
 */
static int
decoder_ends_access_unit(
	unsigned type)
{
	/* SEI, SPS, PPS and the access unit delimiter come before the slices of their access unit. */
	if (type == H264SW_NAL_SEI)
		return 1;
	if (type == H264SW_NAL_SPS)
		return 1;
	if (type == H264SW_NAL_PPS)
		return 1;
	if (type == H264SW_NAL_AUD)
		return 1;

	/* An end of sequence closes the access unit it is in. */
	if (type == H264SW_NAL_END_SEQUENCE)
		return 1;

	/* Types 15 to 18 (the subset SPS, the depth parameter set and two reserved types) begin an access unit too. */
	if (type >= H264SW_NAL_SUBSET_SPS && type <= DECODER_NAL_RESERVED_18)
		return 1;

	/* The others (filler, end of stream, the SPS extension, auxiliary slices) lie inside an access unit. */
	return 0;
}

/*
 * Reads an SPS or a PPS into the store.
 */
static int
decoder_parameter_set(
	struct h264sw_decoder *decoder,
	const struct h264sw_nal *nal)
{
	struct vcodec_bitreader bits;
	int error;

	/* Reads the set from its RBSP; a set that fails leaves the store as it was. */
	vcodec_bitreader_init(&bits, nal->rbsp, nal->rbsp_size);
	if (nal->type == H264SW_NAL_SPS) {
		error = h264sw_ps_read_sps(&decoder->ps, &bits);
	} else {
		error = h264sw_ps_read_pps(&decoder->ps, &bits);
	}

	/* Counts a set outside the decoder's range, so the caller can tell why the stream is refused. */
	if (error == ENOTSUP)
		decoder->stats.unsupported++;

	/* Reports a set that could not be stored. */
	if (error != 0)
		return error;

	/* Succeeded: the set is stored. */
	return 0;
}

/*
 * Takes one slice: finishes the picture before it when the slice starts a new one, starts the picture, and decodes the slice.
 */
static int
decoder_slice(
	struct h264sw_decoder *decoder,
	const struct h264sw_nal *nal,
	int64_t time_us)
{
	struct vcodec_bitreader bits;
	struct h264sw_slice_header *header;
	const struct h264sw_pps *pps;
	const struct h264sw_sps *named_sps;
	int new_picture;
	int error;

	/* Reads the slice header into the decoder's header of the slice being decoded. */
	header = &decoder->header;
	vcodec_bitreader_init(&bits, nal->rbsp, nal->rbsp_size);
	error = h264sw_slice_read(&bits, nal, &decoder->ps, header);
	if (error == ENOTSUP) {
		decoder->stats.unsupported++;
		return ENOTSUP;
	}
	if (error != 0) {
		decoder->stats.slice_errors++;
		return EINVAL;
	}

	/* A slice of a redundant picture is dropped: the primary picture is decoded instead. */
	if (header->redundant_pic_cnt > 0U)
		return 0;

	/* The header was read, so its PPS and that PPS's SPS are present. */
	pps = &decoder->ps.pps[header->pps_id];
	named_sps = &decoder->ps.sps[pps->sps_id];

	/* Finishes the picture in progress when this slice is the first of the next one (7.4.1.2.4). */
	if (decoder->in_picture) {
		new_picture = h264sw_slice_new_picture(&decoder->previous, header, named_sps);
		if (new_picture) {
			error = decoder_finish_picture(decoder);
			if (error != 0)
				return error;
		}
	}

	/* Starts a picture with its first slice. */
	if (!decoder->in_picture) {
		error = decoder_start_picture(decoder, pps, time_us);
		if (error != 0)
			return error;

		/* A picture dropped while waiting for a start has nothing to decode. */
		if (!decoder->in_picture)
			return 0;
	}

	/* Decodes the slice from the start of its slice_data(). */
	error = decoder_decode_slice(decoder, &bits);
	if (error != 0)
		return error;

	/* Succeeded: the slice is decoded. */
	return 0;
}

/*
 * Activates the SPS a starting picture's PPS names.
 *
 * When the SPS is another one than the active one (another id, or the
 * same id stored again with other contents), the old sequence's frames
 * are output (or dropped by no_output_of_prior_pics_flag) and the DPB is
 * prepared for the new SPS.  At an IDR picture every old frame is output
 * before the new picture in any case (C.4.4), so doing it here gives the
 * same order; C.4.4 NOTE 3 asks for the output even when the size
 * changes.  A new size, cropping window or DPB size also makes a new
 * pool and new macroblock arrays.
 */
static int
decoder_activate(
	struct h264sw_decoder *decoder,
	const struct h264sw_pps *pps)
{
	const struct h264sw_sps *sps;
	unsigned generation;
	int resize;
	int error;

	/* Takes the SPS the PPS names (present: the slice header was read against it). */
	sps = &decoder->ps.sps[pps->sps_id];
	generation = decoder->ps.sps_generation[pps->sps_id];

	/* The active SPS stays when it is the same one. */
	if (decoder->has_sequence &&
	    decoder->sps.id == pps->sps_id &&
	    decoder->sps_generation == generation)
		return 0;

	/* Outputs the old sequence's frames, or drops them when the IDR picture says no_output_of_prior_pics_flag (C.4.4). */
	if (decoder->has_sequence) {
		if (decoder->header.idr && decoder->header.no_output_of_prior_pics) {
			h264sw_dpb_clear(&decoder->dpb);
		} else {
			error = h264sw_dpb_flush(&decoder->dpb, &decoder->output);
			if (error != 0)
				return error;
		}
	}

	/* Decides whether the frames of the old pool still fit the new sequence. */
	resize = 0;
	if (decoder->pool == NULL) {
		/* No sequence was active yet, or the last activation failed. */
		resize = 1;
	} else if (sps->width_mbs != decoder->sps.width_mbs ||
		   sps->height_mbs != decoder->sps.height_mbs ||
		   sps->dpb_frames != decoder->sps.dpb_frames) {
		/* Another size of frame or of DPB (the pool's limit follows the DPB). */
		resize = 1;
	} else if (sps->crop_left != decoder->sps.crop_left ||
		   sps->crop_right != decoder->sps.crop_right ||
		   sps->crop_top != decoder->sps.crop_top ||
		   sps->crop_bottom != decoder->sps.crop_bottom) {
		/* Another cropping window, which the pool gives its frames. */
		resize = 1;
	}

	/* Copies the SPS, so a later SPS of the same id does not change it under a picture. */
	decoder->sps = *sps;
	decoder->sps_generation = generation;
	decoder->has_sequence = 0;

	/* Makes a pool and arrays for the new size. */
	if (resize) {
		error = decoder_size_arrays(decoder);
		if (error != 0)
			return error;
	}

	/* Prepares the empty DPB for the new sequence. */
	h264sw_dpb_init(&decoder->dpb, &decoder->sps);
	decoder->has_sequence = 1;

	/* Succeeded: the SPS is active. */
	return 0;
}

/*
 * Makes the frame pool and the macroblock and slice arrays for the active SPS.
 *
 * The old pool is destroyed (frames still held elsewhere keep it until
 * their last unref).  A failure leaves no pool, so the next activation
 * makes everything again.
 */
static int
decoder_size_arrays(
	struct h264sw_decoder *decoder)
{
	const struct h264sw_sps *sps;
	struct h264sw_mb *mbs;
	struct h264sw_slice_params *slices;
	unsigned width;
	unsigned height;
	unsigned crop_width;
	unsigned crop_height;
	unsigned limit;
	int error;

	/* Lets the old pool go: the frames the output queue and the caller hold keep it alive. */
	sps = &decoder->sps;
	h264sw_frame_pool_destroy(decoder->pool);
	decoder->pool = NULL;

	/* Grows the macroblock array to one entry per macroblock. */
	if (decoder->mbs_capacity < sps->size_mbs) {
		mbs = malloc((size_t)sps->size_mbs * sizeof(*mbs));
		if (mbs == NULL)
			return ENOMEM;
		free(decoder->mbs);
		decoder->mbs = mbs;
		decoder->mbs_capacity = sps->size_mbs;
	}

	/* Grows the slice array: a slice has at least one macroblock. */
	if (decoder->slices_capacity < sps->size_mbs) {
		slices = malloc((size_t)sps->size_mbs * sizeof(*slices));
		if (slices == NULL)
			return ENOMEM;
		free(decoder->slices);
		decoder->slices = slices;
		decoder->slices_capacity = sps->size_mbs;
	}

	/* The cropping window in luma samples; the SPS reader checked that it is not empty. */
	width = sps->width_mbs * 16U;
	height = sps->height_mbs * 16U;
	crop_width = width - sps->crop_left - sps->crop_right;
	crop_height = height - sps->crop_top - sps->crop_bottom;

	/*
	 * The frames the DPB holds, as many again bumped at once, the
	 * picture being decoded, the last output, the frame the caller
	 * copies out, and a spare (struct h264sw_frame_pool).
	 */
	limit = 2U * sps->dpb_frames + 4U;
	if (limit > H264SW_FRAME_POOL_MAX)
		limit = H264SW_FRAME_POOL_MAX;

	/* Makes the pool of the new size, last, so that its presence says everything is made. */
	error = h264sw_frame_pool_create(sps->width_mbs, sps->height_mbs, sps->crop_left, sps->crop_top, crop_width, crop_height, limit, &decoder->pool);
	if (error != 0) {
		decoder->pool = NULL;
		return error;
	}

	/* Succeeded: the pool and the arrays fit the sequence. */
	return 0;
}

/*
 * Starts a picture with the slice just read.
 *
 * While waiting for a start, a picture that is neither IDR nor begun by
 * an I slice is dropped (in_picture stays 0).  Otherwise the SPS is
 * activated or checked, the gaps in frame_num are filled, the order
 * count derived, a frame taken and the picture context prepared.
 */
static int
decoder_start_picture(
	struct h264sw_decoder *decoder,
	const struct h264sw_pps *pps,
	int64_t time_us)
{
	struct h264sw_slice_header *header;
	struct h264sw_picture_ctx *picture;
	struct h264sw_scaling scaling;
	struct h264sw_frame *frame;
	unsigned addr;
	int starting;
	int new_picture;
	int same_picture;
	int error;

	/* Takes the header of the picture's first slice. */
	header = &decoder->header;
	starting = 0;

	/* While waiting, only an IDR picture or a picture whose first slice is an I slice can start. */
	if (decoder->waiting != DECODER_RUNNING) {
		if (!header->idr && header->slice_type != H264SW_SLICE_I) {
			/* Counts each dropped picture once, by comparing with the last slice dropped (7.4.1.2.4). */
			same_picture = 0;
			if (decoder->waiting == DECODER_WAITING_DROPPED) {
				new_picture = h264sw_slice_new_picture(&decoder->previous, header, &decoder->ps.sps[pps->sps_id]);
				if (!new_picture)
					same_picture = 1;
			}
			if (!same_picture)
				decoder->stats.dropped++;
			decoder->previous = *header;
			decoder->waiting = DECODER_WAITING_DROPPED;
			return 0;
		}
		starting = 1;
	}

	/* An IDR picture or the start picture activates its SPS; any other picture must use the active one (7.4.1.2.1). */
	if (header->idr || starting) {
		error = decoder_activate(decoder, pps);
		if (error != 0) {
			decoder->stats.slice_errors++;
			return error;
		}
	} else if (!decoder->has_sequence) {
		/* No sequence is active (an activation failed): the picture has nowhere to go. */
		decoder->stats.slice_errors++;
		return EINVAL;
	} else if (pps->sps_id != decoder->sps.id ||
		   decoder->ps.sps_generation[pps->sps_id] != decoder->sps_generation) {
		/* Another SPS can only start at an IDR picture: nothing sized by the active SPS is indexed by its values. */
		decoder->stats.slice_errors++;
		return EINVAL;
	}

	/* A slice outside the active picture size is dropped. */
	if (header->first_mb >= decoder->sps.size_mbs) {
		decoder->stats.slice_errors++;
		return EINVAL;
	}

	/* Stores the frames that stand for a gap in frame_num (8.2.5.2); the start picture has no earlier frame_num to compare with. */
	if (!starting) {
		error = h264sw_dpb_gaps(&decoder->dpb, &decoder->sps, header, &decoder->poc, decoder->pool, &decoder->output);
		if (error != 0)
			return error;
	}

	/* Derives the picture's order counts (8.2.1). */
	error = h264sw_poc_compute(&decoder->poc, &decoder->sps, header, &decoder->picture_poc);
	if (error != 0) {
		decoder->stats.slice_errors++;
		return EINVAL;
	}

	/* Takes the frame the picture is decoded into. */
	frame = h264sw_frame_get(decoder->pool);
	if (frame == NULL)
		return ENOMEM;

	/* Records what the picture leaves for later pictures and how the sequence says to show it. */
	frame->poc = decoder->picture_poc.poc;
	frame->top_poc = decoder->picture_poc.top;
	frame->bottom_poc = decoder->picture_poc.bottom;
	frame->frame_num = header->frame_num;
	frame->idr = header->idr;
	frame->time_us = time_us;
	frame->sar_width = decoder->sps.sar_width;
	frame->sar_height = decoder->sps.sar_height;
	frame->signal_present = decoder->sps.signal_present;
	frame->full_range = decoder->sps.full_range;
	frame->colour_present = decoder->sps.colour_present;
	frame->matrix_coefficients = decoder->sps.matrix_coefficients;

	/* Resolves the picture's scaling lists into its scaling functions (8.5.9). */
	picture = &decoder->picture;
	h264sw_ps_scaling(&decoder->sps, pps, &scaling);
	h264sw_levelscale_init(&picture->levelscale, &scaling);

	/* Prepares the picture context for the active size. */
	picture->frame = frame;
	picture->sps = &decoder->sps;
	picture->pps = pps;
	picture->mbs = decoder->mbs;
	picture->width_mbs = decoder->sps.width_mbs;
	picture->height_mbs = decoder->sps.height_mbs;
	picture->size_mbs = decoder->sps.size_mbs;
	picture->slices = decoder->slices;
	picture->slice_count = 0U;
	picture->slice_capacity = decoder->slices_capacity;
	picture->decoded_mbs = 0U;
	picture->chroma_qp_offset[0] = pps->chroma_qp_offset;
	picture->chroma_qp_offset[1] = pps->second_chroma_qp_offset;

	/* Marks every macroblock as not decoded yet. */
	for (addr = 0U;
	     addr < picture->size_mbs;
	     addr++)
		h264sw_mb_reset(&picture->mbs[addr], -1);

	/*
	 * A start at a non-IDR picture decodes the open GOP's leading
	 * pictures without output until the next IDR picture or operation
	 * 5; an IDR picture ends that.
	 */
	if (header->idr) {
		decoder->leading = 0;
	} else if (starting) {
		decoder->leading = 1;
		decoder->start_poc = decoder->picture_poc.poc;
	}

	/* The picture is in progress from its first slice. */
	decoder->first = *header;
	decoder->time_us = time_us;
	decoder->in_picture = 1;
	decoder->waiting = DECODER_RUNNING;

	/* Succeeded: the picture is ready for its slices. */
	return 0;
}

/*
 * Decodes the slice just read into the picture in progress.
 *
 * The slice's parameters are recorded for the deblocking, its lists,
 * weights and direct prediction prepared, CABAC started when used, and
 * its macroblocks decoded.  An error ends the slice; the macroblocks it
 * left are concealed when the picture finishes.
 */
static int
decoder_decode_slice(
	struct h264sw_decoder *decoder,
	struct vcodec_bitreader *bits)
{
	struct h264sw_picture_ctx *picture;
	struct h264sw_slice_ctx *slice;
	struct h264sw_slice_header *header;
	struct h264sw_slice_params *params;
	int error;

	/* Takes the picture and the slice header. */
	picture = &decoder->picture;
	slice = &decoder->slice;
	header = &decoder->header;

	/* Refuses a slice beyond the slices a picture can have (each has at least one macroblock). */
	if (picture->slice_count >= picture->slice_capacity) {
		decoder->stats.slice_errors++;
		return EINVAL;
	}

	/* Refuses a slice outside the active picture size. */
	if (header->first_mb >= picture->size_mbs) {
		decoder->stats.slice_errors++;
		return EINVAL;
	}

	/* Records the slice's deblocking parameters under its number (8.7). */
	params = &picture->slices[picture->slice_count];
	params->disable_deblocking = header->disable_deblocking;
	params->filter_offset_a = header->filter_offset_a;
	params->filter_offset_b = header->filter_offset_b;
	params->slice_type = header->slice_type;

	/* Prepares the slice context: its number, its header, the first QPY (SliceQPY). */
	slice->picture = picture;
	slice->header = header;
	slice->slice_num = (int32_t)picture->slice_count;
	slice->qp = header->qp;
	slice->concealed = 0U;
	picture->slice_count++;

	/* Builds the reference picture lists from the DPB as marked before the picture (8.2.4). */
	error = h264sw_reflist_build(&decoder->dpb, header, decoder->first.frame_num, decoder->sps.max_frame_num, decoder->picture_poc.poc, decoder->sps.poc_type, &slice->lists);
	if (error != 0) {
		decoder->previous = *header;
		decoder->stats.slice_errors++;
		return EINVAL;
	}

	/* Prepares the weights and, for a B slice, the direct prediction (8.4.3, 8.4.1.2). */
	h264sw_weights_setup(&slice->weights, picture->pps, header, &slice->lists, decoder->picture_poc.poc);
	memset(&slice->direct, 0, sizeof(slice->direct));
	if (header->slice_type == H264SW_SLICE_B)
		h264sw_direct_setup(&slice->direct, &slice->lists, decoder->picture_poc.poc);

	/* Positions the readers at slice_data(): CAVLC reads from bits, CABAC starts its engine there (9.3.1). */
	slice->bits = *bits;
	slice->cabac_mode = picture->pps->cabac;
	if (slice->cabac_mode) {
		error = h264sw_cabac_start(&slice->cabac, &slice->bits, header->slice_type, header->cabac_init_idc, header->qp);
		if (error != 0) {
			decoder->previous = *header;
			decoder->stats.slice_errors++;
			return EINVAL;
		}
	}

	/* Decodes the macroblocks; the stand-ins used for missing references count as concealed. */
	error = h264sw_slice_data(slice);
	picture->frame->concealed += slice->concealed;
	decoder->previous = *header;
	if (error != 0) {
		decoder->stats.slice_errors++;
		return error;
	}

	/* Succeeded: the slice's macroblocks are in the picture. */
	return 0;
}

/*
 * Finishes the picture in progress.
 *
 * Conceals the macroblocks no slice decoded, deblocks, copies the motion
 * into the frame, carries the order count past the picture, and marks
 * and stores it in the DPB (which may output frames).  The picture is
 * over even when the DPB reports an error, so the decoder can take the
 * next picture.
 */
static int
decoder_finish_picture(
	struct h264sw_decoder *decoder)
{
	struct h264sw_picture_ctx *picture;
	struct h264sw_frame *frame;
	unsigned marking;
	uint32_t long_idx;
	unsigned addr;
	int output;
	int error;

	/* Nothing to finish. */
	if (!decoder->in_picture)
		return 0;

	/* Fills the macroblocks that no slice decoded. */
	picture = &decoder->picture;
	frame = picture->frame;
	decoder_conceal(decoder);

	/* Filters the block edges of the whole picture (8.7). */
	h264sw_deblock_picture(picture);

	/* Copies each macroblock's motion into the frame, for the direct prediction of later pictures (8.4.1.2). */
	for (addr = 0U;
	     addr < picture->size_mbs;
	     addr++)
		frame->motion[addr] = picture->mbs[addr].motion;

	/*
	 * Decides the output before operation 5 changes the order count: a
	 * leading picture of the start (an order count below the start
	 * picture's) is decoded but not output.
	 */
	output = 1;
	if (decoder->leading && decoder->picture_poc.poc < decoder->start_poc) {
		output = 0;
		decoder->stats.dropped++;
	}

	/* Carries the order count state past the picture; operation 5 resets its counts (8.2.1). */
	h264sw_poc_finish(&decoder->poc, &decoder->sps, &decoder->first, &decoder->picture_poc);
	frame->poc = decoder->picture_poc.poc;
	frame->top_poc = decoder->picture_poc.top;
	frame->bottom_poc = decoder->picture_poc.bottom;

	/* Marks the references and removes the frames no longer needed (C.4.4, 8.2.5). */
	marking = H264SW_REF_UNUSED;
	long_idx = 0U;
	error = h264sw_dpb_remove(&decoder->dpb, &decoder->sps, &decoder->first, frame, 0, &marking, &long_idx, &decoder->output);

	/* After operation 5 the picture's frame_num is taken as 0 (7.4.3), and the leading pictures of a start are over. */
	if (decoder->first.has_mmco5) {
		frame->frame_num = 0U;
		decoder->leading = 0;
	}

	/* Stores the picture (C.4.5); the DPB takes its own reference. */
	if (error == 0)
		error = h264sw_dpb_store(&decoder->dpb, frame, marking, long_idx, output, &decoder->output);

	/* The picture is over whatever the DPB said: the decoder's reference goes. */
	h264sw_frame_unref(frame);
	picture->frame = NULL;
	decoder->in_picture = 0;
	decoder->stats.pictures++;

	/* Reports a DPB that could not take the picture. */
	if (error != 0)
		return error;

	/* Succeeded: the picture is in the DPB or output. */
	return 0;
}

/*
 * Fills the macroblocks no slice decoded.
 *
 * Each is copied from the same place of RefPicList0[ 0 ] of the last
 * slice when it had one, else of the frame output last, else made grey,
 * and its entry is set as an intra macroblock of the last slice, which
 * the deblocking filter then treats like any other.
 */
static void
decoder_conceal(
	struct h264sw_decoder *decoder)
{
	struct h264sw_picture_ctx *picture;
	struct h264sw_slice_ctx *slice;
	const struct h264sw_frame *source;
	struct h264sw_mb *entry;
	int32_t last_slice;
	unsigned addr;
	unsigned x;
	unsigned y;

	/* Nothing to do when every macroblock was decoded. */
	picture = &decoder->picture;
	slice = &decoder->slice;
	if (picture->decoded_mbs >= picture->size_mbs)
		return;

	/* Chooses the frame the samples come from: the last slice's first reference, else the last output. */
	source = NULL;
	if (slice->lists.count[0] > 0U)
		source = slice->lists.list[0][0].frame;
	if (source == NULL)
		source = decoder->last_output;

	/* A frame of another size has nothing at the same place. */
	if (source != NULL &&
	    (source->width != picture->frame->width ||
	     source->height != picture->frame->height))
		source = NULL;

	/* The filled macroblocks belong to the last slice (a picture starts with one). */
	last_slice = 0;
	if (picture->slice_count > 0U)
		last_slice = (int32_t)picture->slice_count - 1;

	/* Fills each macroblock left without a slice. */
	for (addr = 0U;
	     addr < picture->size_mbs;
	     addr++) {
		entry = &picture->mbs[addr];
		if (entry->slice != -1)
			continue;

		/* Copies or greys the samples at the macroblock's place. */
		x = addr % picture->width_mbs;
		y = addr / picture->width_mbs;
		decoder_conceal_samples(picture->frame, x, y, source);

		/* Records the macroblock as intra, with the last macroblock's QPY for the deblocking filter. */
		h264sw_mb_reset(entry, last_slice);
		entry->kind = H264SW_MB_I_16X16;
		entry->qp = (uint8_t)slice->qp;
		entry->motion.intra = 1U;

		/* Counts the macroblock as concealed. */
		picture->frame->concealed++;
		decoder->stats.concealed_mbs++;
	}
}

/*
 * Copies one macroblock's samples from a frame of the same size, or makes them grey when there is none.
 */
static void
decoder_conceal_samples(
	struct h264sw_frame *frame,
	unsigned x,
	unsigned y,
	const struct h264sw_frame *source)
{
	uint8_t *to;
	const uint8_t *from;
	unsigned plane;
	unsigned size;
	unsigned row;

	/* Fills the luma 16x16 block, then the two chroma 8x8 blocks. */
	for (plane = 0U;
	     plane < 3U;
	     plane++) {
		/* The block's side in this plane. */
		size = 8U;
		if (plane == 0U)
			size = 16U;

		/* Writes each row of the block. */
		for (row = 0U;
		     row < size;
		     row++) {
			/* Copies the row from the same place of the source, or greys it without one. */
			to = frame->plane[plane] + (size_t)(y * size + row) * frame->stride[plane] + (size_t)x * size;
			if (source != NULL) {
				from = source->plane[plane] + (size_t)(y * size + row) * source->stride[plane] + (size_t)x * size;
				memcpy(to, from, size);
			} else {
				memset(to, DECODER_GREY, size);
			}
		}
	}
}

/*
 * Drops the picture in progress without storing it.
 */
static void
decoder_drop_picture(
	struct h264sw_decoder *decoder)
{
	/* Nothing is in progress. */
	if (!decoder->in_picture)
		return;

	/* Releases the picture's frame. */
	h264sw_frame_unref(decoder->picture.frame);
	decoder->picture.frame = NULL;
	decoder->in_picture = 0;
}

/*
 * Gives a frame taken for output the smallest of the waiting times.
 *
 * The candidates are the frame taken, the frames still in the output
 * queue and the DPB frames still needed for output; the frame that had
 * the smallest time gets the taken frame's, so the set of times is kept.
 */
static void
decoder_retime(
	struct h264sw_decoder *decoder,
	struct h264sw_frame *taken)
{
	struct h264sw_frame *earliest;
	struct h264sw_frame *frame;
	struct h264sw_dpb_entry *entry;
	unsigned index;
	int64_t time_us;

	/* Starts with the frame taken itself. */
	earliest = taken;

	/* Looks through the frames still in the output queue. */
	for (index = 0U;
	     index < decoder->output.count;
	     index++) {
		frame = decoder->output.frame[(decoder->output.head + index) % H264SW_OUTPUT_MAX];
		if (frame != NULL && frame->time_us < earliest->time_us)
			earliest = frame;
	}

	/* Looks through the DPB frames still waiting for output. */
	for (index = 0U;
	     index < H264SW_MAX_DPB_FRAMES;
	     index++) {
		entry = &decoder->dpb.entry[index];
		if (entry->frame == NULL)
			continue;
		if (!entry->output_needed)
			continue;
		if (entry->frame->time_us < earliest->time_us)
			earliest = entry->frame;
	}

	/* Swaps the two times, so the taken frame shows at the earliest one. */
	if (earliest != taken) {
		time_us = earliest->time_us;
		earliest->time_us = taken->time_us;
		taken->time_us = time_us;
	}
}
