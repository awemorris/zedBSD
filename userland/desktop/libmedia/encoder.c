/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libmedia's video encoders: the boundary between the programs and the
 * encoding back ends.  An encoder is opened by the first back end of the
 * table that takes its whole configuration and keeps that back end until
 * it closes.  This file checks what the programs ask for, keeps the order
 * of the pictures and the drain, frames the back end's access units
 * (Annex B with the parameter sets before a key frame, or 4-byte lengths)
 * and builds the codec data for a container.  It also holds the parts every
 * back end shares: the account of failed pictures and the mapping of a
 * configuration to libmedia's software encoders.
 */

#include "h265sw-ps.h"
#include "media-private.h"
#include "vcodec-bitreader.h"
#include "vcodec-nal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The smallest side of a picture, and the highest frame rate, in frames a second. */
#define ENCODER_MIN_SIDE	16U
#define ENCODER_MAX_RATE	300U

/* The highest quantizer of H.264 and H.265 at 8 bits. */
#define ENCODER_MAX_QP		51U

/* The slowest and best speed of config.speed. */
#define ENCODER_MAX_SPEED	3U

/* The H.265 NAL unit types of the parameter sets (Table 7-1). */
#define ENCODER_HEVC_VPS	32U
#define ENCODER_HEVC_SPS	33U
#define ENCODER_HEVC_PPS	34U

/* The bytes of an HEVCDecoderConfigurationRecord before its arrays, and of each array's header (ISO/IEC 14496-15 8.3.3.1). */
#define ENCODER_HVCC_HEADER	23U
#define ENCODER_HVCC_ARRAY	3U

/* The bytes of general_profile_space through general_level_idc in an SPS's profile_tier_level (7.3.3). */
#define ENCODER_HVCC_PTL	12U

/* The bytes of a NAL unit's length in a length-prefixed packet and in an hvcC array. */
#define ENCODER_LENGTH_BYTES	4U
#define ENCODER_HVCC_NAL_BYTES	2U

/* The largest magnitude a back end's negative errno value is taken to have. */
#define ENCODER_ERRNO_MAX	4095

/*
 * An open encoder: the back end that opened it and that back end's state,
 * and what this file keeps around it.
 *
 * config is the configuration as the back end got it (checked and
 * resolved; set_rate keeps bitrate and max_bitrate current).  lost is 1
 * once the device was lost (every call but close answers
 * MEDIA_PROBLEM_LOST).  draining is 1 once the drain was sent.  timed is 1
 * once a picture was sent, last_time_us that picture's time.  sent and
 * packets count the pictures sent and the packets taken.  output and
 * output_room are the buffer a framed packet is built in, which the last
 * packet's bytes may point into.
 */
struct media_encoder {
	const struct media_encoder_ops *ops;
	void *state;
	struct media_encoder_config config;
	int lost;
	int draining;
	int timed;
	int64_t last_time_us;
	uint64_t sent;
	uint64_t packets;
	uint8_t *output;
	size_t output_room;
};

/*
 * The fields of an SPS an HEVCDecoderConfigurationRecord copies: the
 * general profile_tier_level bytes as they are, chroma_format_idc, the bit
 * depths, sps_max_sub_layers_minus1, sps_temporal_id_nesting_flag and the
 * VUI's min_spatial_segmentation_idc (0 without one).
 */
struct encoder_hvcc_sps {
	uint8_t ptl[ENCODER_HVCC_PTL];
	unsigned chroma_format_idc;
	unsigned bit_depth_luma;
	unsigned bit_depth_chroma;
	unsigned max_sub_layers_minus1;
	int temporal_id_nesting;
	unsigned min_spatial_segmentation_idc;
};

/*
 * The encoding back ends, tried in this order for each encoder (hardware
 * first, then software), ending at NULL.  None is built in yet: every
 * encoder is refused with MEDIA_PROBLEM_DEVICE until the first is added
 * before the NULL.
 */
static const struct media_encoder_ops *const encoder_backends[] = {
	&media_vkvideo_encoder_ops,
	&media_h264sw_encoder_ops,
	NULL,
};

static int encoder_answer(int status);
static int encoder_colour_check(const struct media_encoder_config *config);
static int encoder_uuid_named(const struct media_encoder_config *config);
static int encoder_stronger(int problem, int refusal);
static int encoder_picture_check(const struct media_encoder *encoder, const struct media_encoder_picture *picture);
static int encoder_frame(struct media_encoder *encoder, struct media_encoded *packet);
static int encoder_room(struct media_encoder *encoder, size_t size);
static int encoder_length_prefixed(const uint8_t *data, size_t size, uint8_t *output, size_t room, size_t *length);
static int encoder_hvcc(const uint8_t *sets, size_t size, uint8_t *output, size_t room, size_t *length);
static int encoder_hvcc_read_sps(const uint8_t *nal, size_t size, struct encoder_hvcc_sps *sps);
static size_t encoder_hvcc_array(const uint8_t *sets, size_t size, unsigned type, uint8_t *output);

/*
 * Lists the encoders of a codec over the built-in back ends.
 */
int
media_encoder_query(
	unsigned codec,
	struct media_encoder_info *infos,
	unsigned capacity,
	unsigned *count)
{
	int error;

	/* Asks every built-in back end for its rows. */
	error = media_encoding_query(encoder_backends, codec, infos, capacity, count);
	if (error != 0)
		return error;

	/* Succeeded: *count holds the number of all rows. */
	return 0;
}

/*
 * Opens an encoder with the first built-in back end that takes the configuration.
 */
int
media_encoder_create(
	const struct media_encoder_config *config,
	struct media_encoder **encoder)
{
	int error;

	/* Tries the built-in back ends in their order. */
	error = media_encoding_create(encoder_backends, config, encoder);
	if (error != 0)
		return error;

	/* Succeeded: *encoder is open. */
	return 0;
}

/*
 * Names the back end an encoder opened with.
 */
const char *
media_encoder_backend(
	const struct media_encoder *encoder)
{
	/* A missing encoder has no back end. */
	if (encoder == NULL)
		return "";

	/* The back end's name, fixed for the encoder's life. */
	return encoder->ops->name;
}

/*
 * Sends a picture in CPU memory, or the drain.
 */
int
media_encoder_send(
	struct media_encoder *encoder,
	const struct media_encoder_picture *picture,
	int64_t time_us,
	unsigned flags)
{
	int status;

	/* Refuses a missing encoder. */
	if (encoder == NULL)
		return EINVAL;

	/* A lost encoder takes nothing more. */
	if (encoder->lost)
		return MEDIA_PROBLEM_LOST;

	/* Nothing follows the drain, not even another drain. */
	if (encoder->draining)
		return EINVAL;

	/* A NULL picture asks the back end to finish the pictures it holds. */
	if (picture == NULL) {
		status = encoder->ops->send(encoder->state, NULL, 0, 0);
		status = encoder_answer(status);
		if (status == MEDIA_PROBLEM_LOST)
			encoder->lost = 1;
		if (status != 0)
			return status;

		/* From now on receive runs down to MEDIA_ENCODER_DRAINED. */
		encoder->draining = 1;

		/* Succeeded: the drain was sent. */
		return 0;
	}

	/* Refuses a flag media_encoder_send does not have. */
	if ((flags & ~MEDIA_ENCODE_FORCE_KEY) != 0)
		return EINVAL;

	/* Refuses a picture whose planes do not hold the configured form. */
	status = encoder_picture_check(encoder, picture);
	if (status != 0)
		return status;

	/* Pictures come in display order: each after the one before. */
	if (encoder->timed && time_us <= encoder->last_time_us)
		return EINVAL;

	/* Hands the picture to the back end, which copies it before it returns. */
	status = encoder->ops->send(encoder->state, picture, time_us, flags);
	status = encoder_answer(status);
	if (status == MEDIA_PROBLEM_LOST)
		encoder->lost = 1;
	if (status != 0)
		return status;

	/* Keeps the time the next picture must pass, and counts the picture. */
	encoder->timed = 1;
	encoder->last_time_us = time_us;
	encoder->sent++;

	/* Succeeded: the picture is in flight. */
	return 0;
}

/*
 * Describes the encoder's GPU input images.
 */
int
media_encoder_slots(
	struct media_encoder *encoder,
	struct media_encoder_slot *slots,
	unsigned capacity,
	unsigned *count)
{
	int status;

	/* Refuses missing arguments. */
	if (encoder == NULL || count == NULL)
		return EINVAL;
	if (slots == NULL && capacity != 0)
		return EINVAL;

	/* A lost encoder answers nothing more. */
	if (encoder->lost)
		return MEDIA_PROBLEM_LOST;

	/* Only an encoder opened for GPU input has images. */
	if (encoder->config.input != MEDIA_ENCODER_INPUT_GPU_NV12 || encoder->ops->slots == NULL)
		return EINVAL;

	/* Asks the back end for the images it made when it opened. */
	status = encoder->ops->slots(encoder->state, slots, capacity, count);
	status = encoder_answer(status);
	if (status == MEDIA_PROBLEM_LOST)
		encoder->lost = 1;
	if (status != 0)
		return status;

	/* Succeeded: *count holds the number of all images. */
	return 0;
}

/*
 * Takes a free GPU input image for the next picture.
 */
int
media_encoder_acquire(
	struct media_encoder *encoder,
	unsigned *index)
{
	int status;

	/* Refuses missing arguments. */
	if (encoder == NULL || index == NULL)
		return EINVAL;

	/* A lost encoder answers nothing more. */
	if (encoder->lost)
		return MEDIA_PROBLEM_LOST;

	/* Only an encoder opened for GPU input has images, and none is written after the drain. */
	if (encoder->config.input != MEDIA_ENCODER_INPUT_GPU_NV12 || encoder->ops->acquire == NULL)
		return EINVAL;
	if (encoder->draining)
		return EINVAL;

	/* Asks the back end for an image no picture uses. */
	status = encoder->ops->acquire(encoder->state, index);
	status = encoder_answer(status);
	if (status == MEDIA_PROBLEM_LOST)
		encoder->lost = 1;
	if (status != 0)
		return status;

	/* Succeeded: *index is the program's until it sends it. */
	return 0;
}

/*
 * Sends an acquired GPU input image, or gives it back unencoded.
 */
int
media_encoder_send_slot(
	struct media_encoder *encoder,
	unsigned index,
	int64_t time_us,
	unsigned flags)
{
	int discard;
	int status;

	/* Refuses a missing encoder. */
	if (encoder == NULL)
		return EINVAL;

	/* A lost encoder takes nothing more. */
	if (encoder->lost)
		return MEDIA_PROBLEM_LOST;

	/* Only an encoder opened for GPU input has images, and none is sent after the drain. */
	if (encoder->config.input != MEDIA_ENCODER_INPUT_GPU_NV12 || encoder->ops->send_slot == NULL)
		return EINVAL;
	if (encoder->draining)
		return EINVAL;

	/* Refuses a flag media_encoder_send_slot does not have. */
	if ((flags & ~(MEDIA_ENCODE_FORCE_KEY | MEDIA_ENCODE_DISCARD)) != 0)
		return EINVAL;

	/* A discarded image is no picture: it has no time to keep in order. */
	discard = 0;
	if ((flags & MEDIA_ENCODE_DISCARD) != 0)
		discard = 1;

	/* Pictures come in display order: each after the one before. */
	if (!discard && encoder->timed && time_us <= encoder->last_time_us)
		return EINVAL;

	/* Hands the image to the back end (it refuses an index not acquired). */
	status = encoder->ops->send_slot(encoder->state, index, time_us, flags);
	status = encoder_answer(status);
	if (status == MEDIA_PROBLEM_LOST)
		encoder->lost = 1;
	if (status != 0)
		return status;

	/* A discarded image is free again and nothing was encoded. */
	if (discard)
		return 0;

	/* Keeps the time the next picture must pass, and counts the picture. */
	encoder->timed = 1;
	encoder->last_time_us = time_us;
	encoder->sent++;

	/* Succeeded: the picture is in flight. */
	return 0;
}

/*
 * Takes the next packet, framed as the configuration says.
 */
int
media_encoder_receive(
	struct media_encoder *encoder,
	unsigned flags,
	struct media_encoded *packet)
{
	struct media_encoded unit;
	int status;

	/* Refuses missing arguments and a flag media_encoder_receive does not have. */
	if (encoder == NULL || packet == NULL)
		return EINVAL;
	if ((flags & ~MEDIA_RECEIVE_NOWAIT) != 0)
		return EINVAL;

	/* A lost encoder gives nothing more. */
	if (encoder->lost)
		return MEDIA_PROBLEM_LOST;

	/* Takes the back end's next access unit. */
	memset(&unit, 0, sizeof(unit));
	status = encoder->ops->receive(encoder->state, flags, &unit);
	status = encoder_answer(status);
	if (status == EAGAIN || status == MEDIA_ENCODER_DRAINED)
		return status;

	/*
	 * Any other failure leaves the stream without a picture the next ones
	 * may predict from, so the encoder cannot go on.
	 */
	if (status != 0) {
		encoder->lost = 1;
		return status;
	}

	/* Frames the unit; a packet that cannot be framed is a picture lost from the stream. */
	status = encoder_frame(encoder, &unit);
	if (status != 0) {
		encoder->lost = 1;
		return status;
	}

	/* Gives the packet and counts it. */
	*packet = unit;
	encoder->packets++;

	/* Succeeded: *packet is the encoder's until the next receive. */
	return 0;
}

/*
 * Changes the bit rate of an encoder opened with CBR or VBR.
 */
int
media_encoder_set_rate(
	struct media_encoder *encoder,
	uint32_t bitrate,
	uint32_t max_bitrate)
{
	uint32_t highest;
	int status;

	/* Refuses a missing encoder. */
	if (encoder == NULL)
		return EINVAL;

	/* A lost encoder takes nothing more. */
	if (encoder->lost)
		return MEDIA_PROBLEM_LOST;

	/* A back end without the change cannot do it whatever the rate control. */
	if (encoder->ops->set_rate == NULL)
		return ENOTSUP;

	/* Only a bit rate the encoder keeps can change. */
	if (encoder->config.rate_control != MEDIA_ENCODER_RATE_CBR && encoder->config.rate_control != MEDIA_ENCODER_RATE_VBR)
		return EINVAL;
	if (bitrate == 0)
		return EINVAL;

	/* Resolves the highest rate: CBR's is its rate, VBR's defaults to half again as much. */
	highest = bitrate;
	if (encoder->config.rate_control == MEDIA_ENCODER_RATE_VBR) {
		if (max_bitrate == 0) {
			/* Half again as much, saturated. */
			highest = bitrate + bitrate / 2U;
			if (highest < bitrate)
				highest = UINT32_MAX;
		} else {
			/* A peak under the average cannot be kept. */
			if (max_bitrate < bitrate)
				return EINVAL;
			highest = max_bitrate;
		}
	}

	/* Asks the back end to keep the new rate from the next picture. */
	status = encoder->ops->set_rate(encoder->state, bitrate, highest);
	status = encoder_answer(status);
	if (status == MEDIA_PROBLEM_LOST)
		encoder->lost = 1;
	if (status != 0)
		return status;

	/* Keeps the configuration as the back end now has it. */
	encoder->config.bitrate = bitrate;
	encoder->config.max_bitrate = highest;

	/* Succeeded: the next picture sent has the new rate. */
	return 0;
}

/*
 * Writes the codec data of a form for a container.
 */
int
media_encoder_codec_data(
	const struct media_encoder *encoder,
	unsigned form,
	uint8_t *buffer,
	size_t size,
	size_t *length)
{
	const uint8_t *sets;
	size_t sets_size;
	int status;

	/* Refuses missing arguments. */
	if (encoder == NULL || length == NULL)
		return EINVAL;
	if (buffer == NULL && size != 0)
		return EINVAL;

	/* A lost encoder answers nothing more. */
	if (encoder->lost)
		return MEDIA_PROBLEM_LOST;

	/* Refuses a form that does not exist, then one the codec has no such data in. */
	if (form != MEDIA_CODEC_DATA_PARAMETER_SETS && form != MEDIA_CODEC_DATA_HVCC && form != MEDIA_CODEC_DATA_AVCC)
		return EINVAL;
	if (form == MEDIA_CODEC_DATA_HVCC && encoder->config.codec != MEDIA_CODEC_HEVC)
		return ENOTSUP;

	/* The AVCDecoderConfigurationRecord is built with the first H.264 back end. */
	if (form == MEDIA_CODEC_DATA_AVCC)
		return ENOTSUP;

	/* Takes the parameter sets the back end made when it opened. */
	sets = NULL;
	sets_size = 0;
	status = encoder->ops->parameter_sets(encoder->state, &sets, &sets_size);
	status = encoder_answer(status);
	if (status != 0)
		return status;

	/* An HEVCDecoderConfigurationRecord is built from the parameter sets. */
	if (form == MEDIA_CODEC_DATA_HVCC) {
		status = encoder_hvcc(sets, sets_size, buffer, size, length);
		if (status != 0)
			return status;

		/* Succeeded: the record is in buffer. */
		return 0;
	}

	/* The parameter sets go as they are, when they fit. */
	*length = sets_size;
	if (sets_size > size)
		return ENOBUFS;
	if (sets_size != 0)
		memcpy(buffer, sets, sets_size);

	/* Succeeded: the parameter sets are in buffer. */
	return 0;
}

/*
 * Reads what an encoder did since it opened.
 */
void
media_encoder_stats(
	const struct media_encoder *encoder,
	struct media_encoder_stats *stats)
{
	/* Nothing to read into, or nothing to read from. */
	if (stats == NULL)
		return;
	memset(stats, 0, sizeof(*stats));
	if (encoder == NULL)
		return;

	/* The back end counts the failures, the drops and the recoveries. */
	if (encoder->ops->stats != NULL)
		encoder->ops->stats(encoder->state, stats);

	/* This file counts what passed through it. */
	stats->sent = encoder->sent;
	stats->packets = encoder->packets;
}

/*
 * Closes an encoder.
 */
void
media_encoder_close(
	struct media_encoder *encoder)
{
	/* Closing nothing is allowed. */
	if (encoder == NULL)
		return;

	/* The back end waits for its pictures in flight and frees its state. */
	encoder->ops->close(encoder->state);

	/* Frees the framing buffer and the encoder. */
	free(encoder->output);
	free(encoder);
}

/*
 * Checks a configuration against the ranges every back end shares.
 *
 * Returns 0, or EINVAL for the first value out of its range.
 */
int
media_encoding_check(
	const struct media_encoder_config *config)
{
	uint64_t highest_rate;
	int status;

	/* Refuses a missing configuration and a codec that is not encoded. */
	if (config == NULL)
		return EINVAL;
	if (config->codec != MEDIA_CODEC_H264 && config->codec != MEDIA_CODEC_HEVC)
		return EINVAL;

	/* The visible size is even (4:2:0) and at least the smallest side of both codecs' encoders. */
	if (config->width < ENCODER_MIN_SIDE || config->height < ENCODER_MIN_SIDE)
		return EINVAL;
	if ((config->width & 1U) != 0 || (config->height & 1U) != 0)
		return EINVAL;

	/* The frame rate is a fraction of two positive numbers, at most the highest rate. */
	if (config->rate_num == 0 || config->rate_den == 0)
		return EINVAL;
	highest_rate = (uint64_t)ENCODER_MAX_RATE * config->rate_den;
	if (config->rate_num > highest_rate)
		return EINVAL;

	/* Refuses an unknown rate control and the values it reads out of their ranges. */
	if (config->rate_control > MEDIA_ENCODER_RATE_VBR)
		return EINVAL;
	if (config->rate_control == MEDIA_ENCODER_RATE_CQP && config->qp > ENCODER_MAX_QP)
		return EINVAL;
	if (config->rate_control == MEDIA_ENCODER_RATE_CBR && config->bitrate == 0)
		return EINVAL;
	if (config->rate_control == MEDIA_ENCODER_RATE_VBR) {
		/* VBR keeps an average under a peak. */
		if (config->bitrate == 0)
			return EINVAL;
		if (config->max_bitrate != 0 && config->max_bitrate < config->bitrate)
			return EINVAL;
	}

	/* Refuses an unknown speed and purpose. */
	if (config->speed > ENCODER_MAX_SPEED)
		return EINVAL;
	if (config->purpose != MEDIA_ENCODER_RECORDING && config->purpose != MEDIA_ENCODER_LOW_LATENCY)
		return EINVAL;

	/* A low-latency stream has no B-frames: each would hold its picture back. */
	if (config->purpose == MEDIA_ENCODER_LOW_LATENCY && config->b_frames != 0)
		return EINVAL;

	/* The input is exactly one form. */
	if (config->input != MEDIA_ENCODER_INPUT_NV12 &&
	    config->input != MEDIA_ENCODER_INPUT_I420 &&
	    config->input != MEDIA_ENCODER_INPUT_GPU_NV12)
		return EINVAL;

	/* Refuses an unknown framing. */
	if (config->output != MEDIA_ENCODER_ANNEX_B && config->output != MEDIA_ENCODER_LENGTH_PREFIXED)
		return EINVAL;

	/* Refuses a colour description that cannot be signalled. */
	status = encoder_colour_check(config);
	if (status != 0)
		return status;

	/* Refuses a kind of back end that does not exist. */
	if ((config->allow & ~(MEDIA_ENCODER_HARDWARE | MEDIA_ENCODER_SOFTWARE)) != 0)
		return EINVAL;

	/* Succeeded: every back end may be asked for the configuration. */
	return 0;
}

/*
 * Resolves the defaults of a checked configuration as every back end gets it.
 *
 * key_interval 0 becomes two seconds' worth of frames (at least 1); CBR's
 * max_bitrate becomes its bitrate and VBR's 0 becomes 3 / 2 of bitrate
 * (saturated); the values a rate control does not read become 0 (DEFAULT
 * reads neither qp nor the bit rates, CQP no bit rate, CBR and VBR no
 * qp); a stream without a colour description has full_range 0 and the
 * code points 2 (unspecified).
 */
void
media_encoding_resolve(
	const struct media_encoder_config *config,
	struct media_encoder_config *resolved)
{
	uint64_t frames;
	uint32_t highest;

	/* Starts from the configuration as it was asked for. */
	*resolved = *config;

	/* Two seconds of the nominal frame rate, rounded up (at most 600 by the checked rate). */
	if (resolved->key_interval == 0) {
		frames = ((uint64_t)resolved->rate_num * 2U + resolved->rate_den - 1U) / resolved->rate_den;
		if (frames == 0)
			frames = 1;
		resolved->key_interval = (uint32_t)frames;
	}

	/* Clears or completes what each rate control reads. */
	switch (resolved->rate_control) {
	case MEDIA_ENCODER_RATE_DEFAULT:
		resolved->bitrate = 0;
		resolved->max_bitrate = 0;
		resolved->qp = 0;
		break;
	case MEDIA_ENCODER_RATE_CQP:
		resolved->bitrate = 0;
		resolved->max_bitrate = 0;
		break;
	case MEDIA_ENCODER_RATE_CBR:
		resolved->max_bitrate = resolved->bitrate;
		resolved->qp = 0;
		break;
	default:
		/* VBR: the peak defaults to half again the average, saturated. */
		if (resolved->max_bitrate == 0) {
			highest = resolved->bitrate + resolved->bitrate / 2U;
			if (highest < resolved->bitrate)
				highest = UINT32_MAX;
			resolved->max_bitrate = highest;
		}
		resolved->qp = 0;
		break;
	}

	/* A stream without a colour description signals nothing of it. */
	if (resolved->colour_description == 0) {
		resolved->full_range = 0;
		resolved->primaries = MEDIA_ENCODING_COLOUR_UNSPECIFIED;
		resolved->transfer = MEDIA_ENCODING_COLOUR_UNSPECIFIED;
		resolved->matrix = MEDIA_ENCODING_COLOUR_UNSPECIFIED;
	}
}

/*
 * Lists the encoders of a codec over a table of back ends.
 *
 * Each back end writes its rows after those of the back ends before it,
 * as many as capacity leaves room for, and *count is the number of all.
 * Returns 0; EINVAL for a codec that is not video or missing arguments;
 * ENOMEM or another back end's failure as it answered.
 */
int
media_encoding_query(
	const struct media_encoder_ops *const *backends,
	unsigned codec,
	struct media_encoder_info *infos,
	unsigned capacity,
	unsigned *count)
{
	struct media_encoder_info *room;
	unsigned room_count;
	unsigned written;
	unsigned total;
	unsigned rows;
	size_t index;
	int status;

	/* Refuses a codec that is not encoded and missing arguments. */
	if (codec != MEDIA_CODEC_H264 && codec != MEDIA_CODEC_HEVC)
		return EINVAL;
	if (backends == NULL || count == NULL)
		return EINVAL;
	if (infos == NULL && capacity != 0)
		return EINVAL;

	/* Asks every back end in the table's order, each into the room left. */
	written = 0;
	total = 0;
	for (index = 0; backends[index] != NULL; index++) {
		/* The rows still free, or none. */
		room = NULL;
		room_count = capacity - written;
		if (room_count != 0)
			room = infos + written;

		/* Asks the back end for its rows (no device is no row). */
		rows = 0;
		status = backends[index]->query(codec, room, room_count, &rows);
		status = encoder_answer(status);
		if (status != 0)
			return status;

		/* Counts every row and keeps those that fitted. */
		if (rows > room_count) {
			written += room_count;
		} else {
			written += rows;
		}
		total += rows;
	}

	/* Succeeded: the rows that fitted are in infos. */
	*count = total;
	return 0;
}

/*
 * Opens an encoder with the first back end of a table that takes the configuration.
 *
 * A back end of a kind config.allow leaves out is not asked, nor a software
 * back end when config names a device.  A back end that refuses
 * (MEDIA_PROBLEM_BUSY, _CONFIG or _DEVICE) passes the configuration on;
 * when every one refused, the strongest refusal is answered (BUSY above
 * CONFIG above DEVICE; DEVICE when none was asked).  ENOMEM and any other
 * failure end the search.
 */
int
media_encoding_create(
	const struct media_encoder_ops *const *backends,
	const struct media_encoder_config *config,
	struct media_encoder **encoder)
{
	const struct media_encoder_ops *ops;
	struct media_encoder *opened;
	unsigned allow;
	int named;
	int problem;
	size_t index;
	void *state;
	int status;

	/* Refuses missing arguments. */
	if (backends == NULL || encoder == NULL)
		return EINVAL;
	*encoder = NULL;

	/* Refuses a configuration out of the shared ranges. */
	status = media_encoding_check(config);
	if (status != 0)
		return status;

	/* Allocates the encoder, which keeps the resolved configuration the back end reads. */
	opened = calloc(1, sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;
	media_encoding_resolve(config, &opened->config);

	/* The kinds of back end asked: none named is both. */
	allow = opened->config.allow;
	if (allow == 0)
		allow = MEDIA_ENCODER_HARDWARE | MEDIA_ENCODER_SOFTWARE;

	/* A configuration that names a device leaves the software back ends out. */
	named = encoder_uuid_named(&opened->config);

	/* Asks each back end in the table's order until one opens. */
	problem = MEDIA_PROBLEM_DEVICE;
	for (index = 0; backends[index] != NULL; index++) {
		/* Skips a kind the caller left out, and software for a named device. */
		ops = backends[index];
		if ((ops->kind & allow) == 0)
			continue;
		if (named && (ops->kind & MEDIA_ENCODER_HARDWARE) == 0)
			continue;

		/* Asks the back end to take the whole configuration. */
		state = NULL;
		status = ops->open(&opened->config, &state);
		status = encoder_answer(status);
		if (status == 0) {
			opened->ops = ops;
			opened->state = state;
			break;
		}

		/* A refusal passes the configuration on; anything else ends the search. */
		if (status == MEDIA_PROBLEM_BUSY || status == MEDIA_PROBLEM_CONFIG || status == MEDIA_PROBLEM_DEVICE) {
			problem = encoder_stronger(problem, status);
			continue;
		}
		free(opened);
		return status;
	}

	/* Reports the strongest refusal when no back end opened. */
	if (opened->ops == NULL) {
		free(opened);
		return problem;
	}

	/* Succeeded: the encoder keeps this back end until it closes. */
	*encoder = opened;
	return 0;
}

/*
 * Marks a picture a back end is about to send.
 *
 * marks has MEDIA_ENCODING_PICTURE_KEY when the back end's own key frame
 * interval or MEDIA_ENCODE_FORCE_KEY makes the picture a key frame.  After
 * a failure, the first picture sent is made a key frame to recover
 * (KEY and RECOVERY).  The back end makes a key frame of every picture
 * whose answer has KEY, and gives the answer to media_encoding_recovery_done.
 */
unsigned
media_encoding_recovery_sent(
	struct media_encoding_recovery *recovery,
	unsigned marks)
{
	/* The first picture after a failure recovers the stream. */
	if (recovery->key_pending) {
		marks |= MEDIA_ENCODING_PICTURE_KEY | MEDIA_ENCODING_PICTURE_RECOVERY;
		recovery->key_pending = 0;
	}

	/* The marks the back end keeps with the picture. */
	return marks;
}

/*
 * Accounts for a picture the device is done with, in the order sent.
 *
 * marks are the picture's from media_encoding_recovery_sent; failed is 1
 * when the device failed it.  Returns MEDIA_ENCODING_PACKET for a picture
 * whose packet the back end gives, MEDIA_ENCODING_NO_PACKET for one that
 * failed or predicts from a lost picture, and MEDIA_ENCODING_LOST when the
 * recovering key frames have failed MEDIA_ENCODING_KEY_FAILURES_MAX
 * times in a row.
 */
unsigned
media_encoding_recovery_done(
	struct media_encoding_recovery *recovery,
	unsigned marks,
	int failed)
{
	/*
	 * A failed picture gives no packet; the pictures predicted from it
	 * have lost their reference, and the next picture sent recovers.
	 */
	if (failed) {
		recovery->failed++;
		recovery->broken = 1;
		recovery->key_pending = 1;

		/* A recovering key frame that fails again counts towards giving up. */
		if ((marks & MEDIA_ENCODING_PICTURE_RECOVERY) != 0) {
			recovery->key_failures++;
			if (recovery->key_failures >= MEDIA_ENCODING_KEY_FAILURES_MAX)
				return MEDIA_ENCODING_LOST;
		}

		/* The picture is gone; the stream goes on. */
		return MEDIA_ENCODING_NO_PACKET;
	}

	/* A predicted picture behind a failure predicts from a picture the decoder never gets. */
	if ((marks & MEDIA_ENCODING_PICTURE_KEY) == 0 && recovery->broken) {
		recovery->dropped++;
		return MEDIA_ENCODING_NO_PACKET;
	}

	/* A key frame that came out ends the run of failures; one made to recover is counted. */
	if ((marks & MEDIA_ENCODING_PICTURE_KEY) != 0) {
		recovery->broken = 0;
		recovery->key_failures = 0;
		if ((marks & MEDIA_ENCODING_PICTURE_RECOVERY) != 0)
			recovery->recovered++;
	}

	/* The picture's packet goes to the program. */
	return MEDIA_ENCODING_PACKET;
}

/*
 * Maps a resolved configuration to libmedia's software encoders' configuration.
 *
 * The fields of the same names keep their numbers.  key_interval
 * UINT32_MAX (only the first and forced key frames) becomes the
 * encoders' longest interval.  LOW_LATENCY has no B-frames and the
 * fastest speed unless one was named.  Without a colour description the
 * code points are 2 (unspecified, not sent).  The input form becomes the
 * encoders' picture format (0 for GPU input, which they refuse).
 */
void
media_encoding_software(
	const struct media_encoder_config *config,
	struct media_encoding_software *software)
{
	/* Copies the fields both sides name the same way. */
	memset(software, 0, sizeof(*software));
	software->width = config->width;
	software->height = config->height;
	software->rate_num = config->rate_num;
	software->rate_den = config->rate_den;
	software->profile = config->profile;
	software->level_idc = config->level;
	software->rate_control = config->rate_control;
	software->bitrate = config->bitrate;
	software->max_bitrate = config->max_bitrate;
	software->qp = config->qp;
	software->key_interval = config->key_interval;
	software->b_frames = config->b_frames;
	software->speed = config->speed;
	software->threads = config->threads;
	software->thread_count = config->thread_count;

	/* DEFAULT is the software encoders' own default, which reads qp and bitrate: they get zero. */
	if (config->rate_control == MEDIA_ENCODER_RATE_DEFAULT) {
		software->bitrate = 0;
		software->max_bitrate = 0;
		software->qp = 0;
	}

	/* Only the first and forced key frames: the longest interval the encoders keep. */
	if (config->key_interval == UINT32_MAX)
		software->key_interval = MEDIA_ENCODING_MAX_KEY_INTERVAL;

	/* A low-latency stream has no B-frames and is made fast unless a speed was named. */
	if (config->purpose == MEDIA_ENCODER_LOW_LATENCY) {
		software->b_frames = 0;
		if (software->speed == 0)
			software->speed = MEDIA_ENCODING_SPEED_FAST;
	}

	/* The colour description, or the code points that send none. */
	if (config->colour_description) {
		software->full_range = config->full_range;
		software->primaries = config->primaries;
		software->transfer = config->transfer;
		software->matrix = config->matrix;
	} else {
		software->full_range = 0;
		software->primaries = MEDIA_ENCODING_COLOUR_UNSPECIFIED;
		software->transfer = MEDIA_ENCODING_COLOUR_UNSPECIFIED;
		software->matrix = MEDIA_ENCODING_COLOUR_UNSPECIFIED;
	}

	/* The picture format of the CPU input forms. */
	if (config->input == MEDIA_ENCODER_INPUT_NV12) {
		software->format = MEDIA_ENCODING_FORMAT_NV12;
	} else if (config->input == MEDIA_ENCODER_INPUT_I420) {
		software->format = MEDIA_ENCODING_FORMAT_I420;
	}
}

/*
 * Maps a software encoder's receive answer to media_encoder_receive's.
 *
 * 1 (a packet) is 0; 0 (none) is MEDIA_ENCODER_DRAINED after the drain
 * (drained 1) and EAGAIN before it; a negative errno value is the errno
 * value (an internal failure, after which media_encoder_receive makes the
 * encoder lost).  Any other answer is EIO.
 */
int
media_encoding_software_answer(
	int answer,
	int drained)
{
	/* A packet was taken. */
	if (answer == 1)
		return 0;

	/* No packet: the drain is over, or more pictures are needed. */
	if (answer == 0) {
		if (drained)
			return MEDIA_ENCODER_DRAINED;
		return EAGAIN;
	}

	/* An internal failure the encoder answered as a negative errno value. */
	if (answer < 0 && answer >= -ENCODER_ERRNO_MAX)
		return -answer;

	/* An answer the software encoders do not give. */
	return EIO;
}

/* Makes a back end's negative errno value positive. */
static int
encoder_answer(
	int status)
{
	/* A negative errno value of a back end that follows the software encoders' convention. */
	if (status < 0 && status >= -ENCODER_ERRNO_MAX)
		return -status;

	/* A negative value no errno can be. */
	if (status < 0)
		return EIO;

	/* The answer as the back end gave it. */
	return status;
}

/* Checks the colour description against the code points H.264 and H.265 do not reserve. */
static int
encoder_colour_check(
	const struct media_encoder_config *config)
{
	/* Without a description nothing else is read. */
	if (config->colour_description == 0)
		return 0;

	/* The description is either sent or not, and the range is full or not. */
	if (config->colour_description != 1)
		return EINVAL;
	if (config->full_range != 0 && config->full_range != 1)
		return EINVAL;

	/* colour_primaries: 1, 2, 4 to 12 and 22 (H.265 Table E.3, H.264 Table E-3); the rest is reserved. */
	if (config->primaries == 0 || config->primaries == 3)
		return EINVAL;
	if (config->primaries > 12 && config->primaries != 22)
		return EINVAL;

	/* transfer_characteristics: 1, 2 and 4 to 18 (Table E.4, E-4). */
	if (config->transfer == 0 || config->transfer == 3 || config->transfer > 18)
		return EINVAL;

	/* matrix_coefficients: 0 to 2 and 4 to 14 (Table E.5, E-5). */
	if (config->matrix == 3 || config->matrix > 14)
		return EINVAL;

	/* Succeeded: every code point can be signalled. */
	return 0;
}

/* Reports whether a configuration names a device (either UUID not all zero). */
static int
encoder_uuid_named(
	const struct media_encoder_config *config)
{
	size_t index;

	/* Any byte not zero names a device. */
	for (index = 0; index < sizeof(config->device_uuid); index++) {
		if (config->device_uuid[index] != 0)
			return 1;
		if (config->driver_uuid[index] != 0)
			return 1;
	}

	/* Both all zero: any device. */
	return 0;
}

/* Picks the stronger of two refusals (BUSY above CONFIG above DEVICE). */
static int
encoder_stronger(
	int problem,
	int refusal)
{
	/* Busy says the configuration would open later, the most useful answer. */
	if (problem == MEDIA_PROBLEM_BUSY || refusal == MEDIA_PROBLEM_BUSY)
		return MEDIA_PROBLEM_BUSY;

	/* A back end that has the device but not the configuration says more than one without the device. */
	if (problem == MEDIA_PROBLEM_CONFIG || refusal == MEDIA_PROBLEM_CONFIG)
		return MEDIA_PROBLEM_CONFIG;

	/* No device for the configuration. */
	return MEDIA_PROBLEM_DEVICE;
}

/* Checks that a picture in CPU memory has the planes of the configured form. */
static int
encoder_picture_check(
	const struct media_encoder *encoder,
	const struct media_encoder_picture *picture)
{
	size_t width;
	size_t half;

	/* Only a CPU input form is sent this way. */
	if (encoder->config.input != MEDIA_ENCODER_INPUT_NV12 && encoder->config.input != MEDIA_ENCODER_INPUT_I420)
		return EINVAL;

	/* The luma plane holds a whole row of the visible width. */
	width = encoder->config.width;
	half = width / 2U;
	if (picture->plane[0] == NULL || picture->stride[0] < width)
		return EINVAL;
	if (picture->plane[1] == NULL)
		return EINVAL;

	/* NV12's chroma rows interleave Cb and Cr: the same bytes as a luma row. */
	if (encoder->config.input == MEDIA_ENCODER_INPUT_NV12) {
		if (picture->stride[1] < width)
			return EINVAL;
		return 0;
	}

	/* I420's Cb and Cr rows are half a luma row each. */
	if (picture->plane[2] == NULL)
		return EINVAL;
	if (picture->stride[1] < half || picture->stride[2] < half)
		return EINVAL;

	/* Succeeded: the planes hold the picture. */
	return 0;
}

/*
 * Frames a back end's access unit as the configuration says.
 *
 * Annex B: a key frame gets the parameter sets before it (built in the
 * encoder's buffer), any other unit goes as it is.  Length-prefixed:
 * every NAL unit gets its 4-byte length instead of its start code.
 */
static int
encoder_frame(
	struct media_encoder *encoder,
	struct media_encoded *packet)
{
	const uint8_t *sets;
	size_t sets_size;
	size_t length;
	int status;

	/* Length-prefixed packets carry no parameter sets: they are in the codec data. */
	if (encoder->config.output == MEDIA_ENCODER_LENGTH_PREFIXED) {
		/* Measures the packet. */
		status = encoder_length_prefixed(packet->data, packet->size, NULL, 0, &length);
		if (status != 0)
			return status;

		/* Makes room for it. */
		status = encoder_room(encoder, length);
		if (status != 0)
			return status;

		/* Writes it. */
		status = encoder_length_prefixed(packet->data, packet->size, encoder->output, encoder->output_room, &length);
		if (status != 0)
			return status;
		packet->data = encoder->output;
		packet->size = length;

		/* Succeeded: the packet is in the encoder's buffer. */
		return 0;
	}

	/* An Annex B unit that is not a key frame goes as it is. */
	if ((packet->flags & MEDIA_ENCODED_KEY) == 0)
		return 0;

	/* Takes the parameter sets to put before the key frame. */
	sets = NULL;
	sets_size = 0;
	status = encoder->ops->parameter_sets(encoder->state, &sets, &sets_size);
	status = encoder_answer(status);
	if (status != 0)
		return status;

	/* Makes room for the parameter sets and the unit. */
	if (sets_size > SIZE_MAX - packet->size)
		return ENOMEM;
	status = encoder_room(encoder, sets_size + packet->size);
	if (status != 0)
		return status;

	/* Writes the parameter sets, then the unit. */
	if (sets_size != 0)
		memcpy(encoder->output, sets, sets_size);
	if (packet->size != 0)
		memcpy(encoder->output + sets_size, packet->data, packet->size);
	packet->data = encoder->output;
	packet->size += sets_size;

	/* Succeeded: the key frame starts with the parameter sets. */
	return 0;
}

/* Grows the encoder's framing buffer to hold size bytes. */
static int
encoder_room(
	struct media_encoder *encoder,
	size_t size)
{
	uint8_t *output;

	/* The buffer already holds it. */
	if (size <= encoder->output_room)
		return 0;

	/* Grows the buffer (the last packet's bytes are not needed any more). */
	output = realloc(encoder->output, size);
	if (output == NULL)
		return ENOMEM;
	encoder->output = output;
	encoder->output_room = size;

	/* Succeeded: the buffer holds size bytes. */
	return 0;
}

/*
 * Writes an Annex B access unit with a 4-byte length before each NAL unit.
 *
 * With output NULL (or room too small) only *length is computed.  Returns
 * 0; EINVAL for a unit without a NAL unit or one too long for its length.
 */
static int
encoder_length_prefixed(
	const uint8_t *data,
	size_t size,
	uint8_t *output,
	size_t room,
	size_t *length)
{
	const uint8_t *nal;
	size_t nal_size;
	size_t cursor;
	size_t written;
	unsigned units;
	int found;

	/* Walks the unit's NAL units, writing each after its length while they fit. */
	cursor = 0;
	written = 0;
	units = 0;
	for (;;) {
		/* Finds the next NAL unit; none is the end of the unit. */
		found = vcodec_nal_find(data, size, &cursor, &nal, &nal_size);
		if (!found)
			break;

		/* The length must fit its four bytes. */
		if (nal_size > UINT32_MAX)
			return EINVAL;

		/* Writes the big-endian length and the NAL unit when the room holds them. */
		if (output != NULL && written + ENCODER_LENGTH_BYTES + nal_size <= room) {
			output[written] = (uint8_t)(nal_size >> 24);
			output[written + 1U] = (uint8_t)(nal_size >> 16);
			output[written + 2U] = (uint8_t)(nal_size >> 8);
			output[written + 3U] = (uint8_t)nal_size;
			memcpy(output + written + ENCODER_LENGTH_BYTES, nal, nal_size);
		}
		written += ENCODER_LENGTH_BYTES + nal_size;
		units++;
	}

	/* An access unit has at least one NAL unit. */
	if (units == 0)
		return EINVAL;

	/* Succeeded: *length is the framed size. */
	*length = written;
	return 0;
}

/*
 * Builds an HEVCDecoderConfigurationRecord (ISO/IEC 14496-15 8.3.3.1) from
 * the parameter sets in Annex B.
 *
 * The profile, tier, compatibility, constraint and level bytes are the
 * SPS's profile_tier_level bytes as they are (read again they would lose
 * the constraint flags the SPS parse does not keep).  The chroma format,
 * bit depths, sub-layers, nesting and min_spatial_segmentation_idc come
 * from the SPS read; parallelismType, avgFrameRate and constantFrameRate
 * are 0 (unknown); lengthSizeMinusOne is 3.  The arrays hold the VPS, the
 * SPS and the PPS NAL units in that order, each complete.
 *
 * Returns 0 and *length; ENOBUFS with *length the size needed when room is
 * too small; EINVAL for parameter sets without a VPS, an SPS and a PPS,
 * or with an SPS that does not read; ENOMEM.
 */
static int
encoder_hvcc(
	const uint8_t *sets,
	size_t size,
	uint8_t *output,
	size_t room,
	size_t *length)
{
	struct encoder_hvcc_sps sps;
	const uint8_t *first_sps;
	const uint8_t *nal;
	size_t first_sps_size;
	size_t nal_size;
	size_t cursor;
	size_t needed;
	unsigned counts[3];
	unsigned type;
	size_t written;
	int found;
	int status;

	/* Counts the parameter sets of each kind and measures the record. */
	memset(counts, 0, sizeof(counts));
	first_sps = NULL;
	first_sps_size = 0;
	needed = ENCODER_HVCC_HEADER;
	cursor = 0;
	for (;;) {
		/* Finds the next NAL unit; none is the end of the parameter sets. */
		found = vcodec_nal_find(sets, size, &cursor, &nal, &nal_size);
		if (!found)
			break;

		/* A NAL unit has its two header bytes, and an array entry's length is 16 bits. */
		if (nal_size < 2U || nal_size > UINT16_MAX)
			return EINVAL;

		/* Only the VPS, the SPS and the PPS go into the record. */
		type = (nal[0] >> 1) & 0x3fU;
		if (type < ENCODER_HEVC_VPS || type > ENCODER_HEVC_PPS)
			continue;
		counts[type - ENCODER_HEVC_VPS]++;
		needed += ENCODER_HVCC_NAL_BYTES + nal_size;

		/* The first SPS describes the stream. */
		if (type == ENCODER_HEVC_SPS && first_sps == NULL) {
			first_sps = nal;
			first_sps_size = nal_size;
		}
	}

	/* Every kind is there, each in an array of its own. */
	if (counts[0] == 0 || counts[1] == 0 || counts[2] == 0)
		return EINVAL;
	needed += 3U * ENCODER_HVCC_ARRAY;

	/* Reports the size the record needs when it does not fit. */
	*length = needed;
	if (output == NULL || room < needed)
		return ENOBUFS;

	/* Reads the SPS fields the record copies. */
	status = encoder_hvcc_read_sps(first_sps, first_sps_size, &sps);
	if (status != 0)
		return status;

	/* configurationVersion 1, then the general profile_tier_level bytes as the SPS has them. */
	output[0] = 1;
	memcpy(output + 1, sps.ptl, ENCODER_HVCC_PTL);

	/* reserved '1111'b and min_spatial_segmentation_idc. */
	output[13] = (uint8_t)(0xf0U | ((sps.min_spatial_segmentation_idc >> 8) & 0x0fU));
	output[14] = (uint8_t)sps.min_spatial_segmentation_idc;

	/* reserved '111111'b and parallelismType 0 (unknown). */
	output[15] = 0xfc;

	/* reserved '111111'b and chromaFormat, then reserved '11111'b and each bit depth minus 8. */
	output[16] = (uint8_t)(0xfcU | (sps.chroma_format_idc & 0x03U));
	output[17] = (uint8_t)(0xf8U | ((sps.bit_depth_luma - 8U) & 0x07U));
	output[18] = (uint8_t)(0xf8U | ((sps.bit_depth_chroma - 8U) & 0x07U));

	/* avgFrameRate 0 (unknown). */
	output[19] = 0;
	output[20] = 0;

	/* constantFrameRate 0, numTemporalLayers, temporalIdNested and lengthSizeMinusOne 3. */
	output[21] = (uint8_t)((((sps.max_sub_layers_minus1 + 1U) & 0x07U) << 3) | 0x03U);
	if (sps.temporal_id_nesting)
		output[21] |= 0x04U;

	/* numOfArrays: the VPS, the SPS and the PPS. */
	output[22] = 3;

	/* The arrays, in the order the decoders read them. */
	written = ENCODER_HVCC_HEADER;
	written += encoder_hvcc_array(sets, size, ENCODER_HEVC_VPS, output + written);
	written += encoder_hvcc_array(sets, size, ENCODER_HEVC_SPS, output + written);
	written += encoder_hvcc_array(sets, size, ENCODER_HEVC_PPS, output + written);

	/* The two walks agree on every byte. */
	if (written != needed)
		return EINVAL;

	/* Succeeded: the record is in output. */
	return 0;
}

/*
 * Reads the fields an HEVCDecoderConfigurationRecord copies from an SPS NAL unit.
 *
 * The profile_tier_level bytes are taken from the RBSP (emulation
 * prevention removed) after sps_video_parameter_set_id,
 * sps_max_sub_layers_minus1 and sps_temporal_id_nesting_flag (one byte);
 * the rest from libmedia's SPS parse.  Returns 0; EINVAL for an SPS that
 * does not read; ENOMEM.
 */
static int
encoder_hvcc_read_sps(
	const uint8_t *nal,
	size_t size,
	struct encoder_hvcc_sps *sps)
{
	struct h265sw_ps_store *store;
	struct vcodec_bitreader bits;
	const struct h265sw_sps *read;
	uint8_t *rbsp;
	size_t rbsp_size;
	size_t index;
	int status;

	/* Allocates the RBSP, never longer than the NAL unit's payload. */
	rbsp = malloc(size);
	if (rbsp == NULL)
		return ENOMEM;

	/* Removes the emulation prevention bytes from the payload after the two header bytes. */
	status = vcodec_nal_unescape(nal + 2, size - 2U, rbsp, size, &rbsp_size);
	if (status != 0) {
		free(rbsp);
		return EINVAL;
	}

	/* The general profile_tier_level follows the first byte. */
	if (rbsp_size < 1U + ENCODER_HVCC_PTL) {
		free(rbsp);
		return EINVAL;
	}
	memset(sps, 0, sizeof(*sps));
	memcpy(sps->ptl, rbsp + 1, ENCODER_HVCC_PTL);

	/* Allocates a parameter set store for the parse (too large for the stack). */
	store = malloc(sizeof(*store));
	if (store == NULL) {
		free(rbsp);
		return ENOMEM;
	}
	h265sw_ps_init(store);

	/* Reads the SPS. */
	vcodec_bitreader_init(&bits, rbsp, rbsp_size);
	status = h265sw_ps_read_sps(store, &bits);
	if (status != 0)
		goto done;

	/* Finds the set the read stored (the only one). */
	read = NULL;
	for (index = 0; index < H265SW_SPS_IDS; index++) {
		if (store->sps_present[index]) {
			read = &store->sps[index];
			break;
		}
	}
	if (read == NULL) {
		status = EINVAL;
		goto done;
	}

	/* Copies the fields the record holds. */
	sps->chroma_format_idc = read->chroma_format_idc;
	sps->bit_depth_luma = read->bit_depth_luma;
	sps->bit_depth_chroma = read->bit_depth_chroma;
	sps->max_sub_layers_minus1 = read->max_sub_layers_minus1;
	sps->temporal_id_nesting = read->temporal_id_nesting;
	if (read->vui_present)
		sps->min_spatial_segmentation_idc = read->vui.min_spatial_segmentation_idc;

done:
	/* Frees the parse's memory whatever it found. */
	free(store);
	free(rbsp);

	/* An SPS that did not read cannot describe the stream. */
	if (status == ENOMEM)
		return ENOMEM;
	if (status != 0)
		return EINVAL;

	/* Succeeded: *sps holds the fields. */
	return 0;
}

/*
 * Writes the hvcC array of one NAL unit type: array_completeness 1, the
 * type, the number of units, and each unit after its 16-bit length.
 * The caller made room for it.  Returns the bytes written.
 */
static size_t
encoder_hvcc_array(
	const uint8_t *sets,
	size_t size,
	unsigned type,
	uint8_t *output)
{
	const uint8_t *nal;
	size_t nal_size;
	size_t cursor;
	size_t written;
	unsigned count;
	int found;

	/* Writes every unit of the type after the array's header. */
	written = ENCODER_HVCC_ARRAY;
	count = 0;
	cursor = 0;
	for (;;) {
		/* Finds the next NAL unit; none is the end of the parameter sets. */
		found = vcodec_nal_find(sets, size, &cursor, &nal, &nal_size);
		if (!found)
			break;

		/* Units of other types go into other arrays. */
		if (((nal[0] >> 1) & 0x3fU) != type)
			continue;

		/* The unit after its big-endian length. */
		output[written] = (uint8_t)(nal_size >> 8);
		output[written + 1U] = (uint8_t)nal_size;
		memcpy(output + written + ENCODER_HVCC_NAL_BYTES, nal, nal_size);
		written += ENCODER_HVCC_NAL_BYTES + nal_size;
		count++;
	}

	/* array_completeness 1 (no unit of the type is in the packets), reserved 0, the type and the count. */
	output[0] = (uint8_t)(0x80U | (type & 0x3fU));
	output[1] = (uint8_t)(count >> 8);
	output[2] = (uint8_t)count;

	/* The bytes of the array. */
	return written;
}
