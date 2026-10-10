/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Original AAC-LC backend: packet admission is atomic before any overlap or audible output changes. */
#include "media-private.h"
#include "aac-frame.h"
#include "aac-synth.h"
#include "sound.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AAC_PACKET_BLOCKS 64U

/* One admitted raw data block; the decoder owns it until reconstruction consumes its syntax. */
struct aac_block {
	struct aac_block *next;
	struct media_aac_frame frame;
	int64_t time_us;
};

/* One channel identity's retained overlap, independent of transmitted element ordering. */
struct aac_identity {
	struct media_aac_history history;
	unsigned type;
	unsigned tag;
	unsigned side;
	int valid;
};

/* One native audio decoder; all mutable transform, queued syntax, timing and noise state are private. */
struct aac_decoder {
	struct media_aac_config config;
	struct aac_block *head;
	struct aac_block *tail;
	struct aac_identity channels[MEDIA_AAC_MAX_CHANNELS];
	struct media_aac_transform transform;
	struct media_pcm sound;
	float planar[MEDIA_AAC_MAX_CHANNELS][1024];
	float stereo[2048];
	uint32_t random;
	int64_t end_us;
	int configured;
	int draining;
	int failed;
	int preroll;
};

static int aac_load(void);
static const char *aac_reason(void);
static int aac_open(const struct media_track *track, void **result);
static const char *aac_name(const void *state);
static int aac_send(void *state, const struct media_packet *packet);
static int aac_receive(void *state, int64_t *time_us);
static void *aac_picture(void *state);
static size_t aac_sound(void *state, int16_t *samples, size_t capacity, uint32_t rate);
static void aac_flush(void *state);
static void aac_close(void *state);
static int aac_trim(void *state, int64_t before_us);
static int64_t aac_frame_us(const void *state);
static void aac_blocks_free(struct aac_block *block);
static int aac_packet(struct aac_decoder *decoder, const struct media_packet *packet, struct aac_block **first, struct aac_block **last);
static int aac_synthesize(struct aac_decoder *decoder, struct aac_block *block);
static void aac_mix(struct aac_decoder *decoder, const struct media_aac_frame *frame);
static int aac_identity_slot(struct aac_decoder *decoder, const struct media_aac_frame *frame, unsigned channel);

/* Native operations never load or select libavcodec; pictures are absent for this audio-only backend. */
const struct media_decoder_ops media_aac_ops = {
    "libmedia",
    aac_load,
    aac_reason,
    aac_open,
    aac_name,
    aac_send,
    aac_receive,
    aac_picture,
    aac_sound,
    aac_flush,
    aac_close,
    NULL,
    NULL,
    NULL,
    NULL,
    aac_trim,
    aac_frame_us};

/* Built-in AAC has no runtime library-loading prerequisite. */
static int
aac_load(
	void)
{
	return 0;
}

/* Only codec/profile admission can make the built-in decoder unavailable. */
static const char *
aac_reason(
	void)
{
	return "";
}

/* Admit LC ASC immediately, or defer ADTS configuration until the first complete packet. */
static int
aac_open(
	const struct media_track *track,
	void **result)
{
	struct aac_decoder *decoder;
	int error;

	/* Backend selection must not mistake another codec for malformed AAC. */
	*result = NULL;
	if (track->codec != MEDIA_CODEC_AAC)
		return MEDIA_PROBLEM_FORMAT;
	decoder = calloc(1U, sizeof(*decoder));
	if (decoder == NULL)
		return ENOMEM;
	decoder->random = 1U;
	decoder->end_us = track->duration_us;

	/* ASC signalling is authoritative, including explicit and backwards-compatible SBR. */
	if (track->private_size != 0U) {
		error = media_aac_config_parse(track->private_data, track->private_size, &decoder->config);
		if (error != 0) {
			free(decoder);
			if (error == ENOTSUP)
				return MEDIA_PROBLEM_PROFILE;
			return error;
		}

		/* The validated rate establishes the common converter's source clock. */
		error = media_pcm_init(&decoder->sound, decoder->config.rate, decoder->end_us);
		if (error != 0) {
			free(decoder);
			return MEDIA_PROBLEM_PROFILE;
		}

		/* All subsequent raw packets must agree with this admitted program. */
		decoder->configured = 1;
	}

	/* Publish the decoder only after its declared profile has been accepted. */
	*result = decoder;

	/* Succeeded: first send will also refuse unsupported implicit FIL extensions atomically. */
	return 0;
}

/* Report the original codec rather than an external decoder's branding. */
static const char *
aac_name(
	const void *state)
{
	(void)state;
	return "aac-lc";
}

/* Validate every raw block before publishing any part of a packet to reconstruction. */
static int
aac_send(
	void *state,
	const struct media_packet *packet)
{
	struct aac_decoder *decoder;
	struct aac_block *first;
	struct aac_block *last;
	int error;

	/* A fatal error is sticky until flush, keeping later input from concealing an unsupported tool. */
	decoder = state;
	if (decoder->failed != 0)
		return decoder->failed;

	/* NULL marks the real stream boundary where filter lookahead may become silence. */
	if (packet == NULL) {
		decoder->draining = 1;
		return 0;
	}

	/* Audio already marked as ended needs flush before another compressed packet. */
	if (decoder->draining != 0)
		return EINVAL;
	if (decoder->head != NULL)
		return EAGAIN;
	first = NULL;
	last = NULL;
	error = aac_packet(decoder, packet, &first, &last);
	if (error != 0) {
		aac_blocks_free(first);
		decoder->failed = error;
		return error;
	}

	/* Deferred ADTS obtains its rate and channel program from the first validated raw block. */
	if (decoder->configured == 0) {
		error = media_pcm_init(&decoder->sound, first->frame.config.rate, decoder->end_us);
		if (error != 0) {
			aac_blocks_free(first);
			decoder->failed = ENOTSUP;
			return ENOTSUP;
		}

		/* Later packets now have an authoritative LC source rate. */
		decoder->configured = 1;
	}

	/* Publish the complete packet chain and its final program configuration together. */
	decoder->config = last->frame.config;
	decoder->head = first;
	decoder->tail = last;

	/* Succeeded: no part of a rejected SBR/corrupt packet has reached overlap or PCM state. */
	return 0;
}

/* Decode enough admitted blocks to expose one timestamped source block with genuine lookahead. */
static int
aac_receive(
	void *state,
	int64_t *time_us)
{
	struct aac_decoder *decoder;
	struct aac_block *block;
	int error;
	int ready;

	/* Runtime failures remain distinguishable from an ordinary need for another packet. */
	decoder = state;
	if (decoder->failed != 0)
		return -decoder->failed;

	/* Receive first ends the prior block's conversion lifetime, even when sound was never requested. */
	ready = media_pcm_receive(&decoder->sound, time_us);
	if (ready != 0)
		return ready;

	/* A source block waits until its following real samples are available for the filter. */
	while (decoder->head != NULL) {
		block = decoder->head;
		error = aac_synthesize(decoder, block);
		if (error != 0) {
			decoder->failed = error;
			return -error;
		}

		/* Retire syntax only after its complete reconstruction has succeeded. */
		decoder->head = block->next;
		if (decoder->head == NULL)
			decoder->tail = NULL;
		free(block);
		ready = media_pcm_receive(&decoder->sound, time_us);
		if (ready != 0)
			return ready;
	}

	/* Only after all admitted syntax is consumed may the final convolution extend with silence. */
	if (decoder->draining != 0) {
		decoder->sound.drained = 1;
		ready = media_pcm_receive(&decoder->sound, time_us);
		if (ready != 0)
			return ready;
	}

	/* No ready block remains; additional input can supply real filter lookahead. */
	return 0;
}

/* AAC never owns or publishes a video picture. */
static void *
aac_picture(
	void *state)
{
	(void)state;
	return NULL;
}

/* The common converter enforces output capacity and the received block's lifetime. */
static size_t
aac_sound(
	void *state,
	int16_t *samples,
	size_t capacity,
	uint32_t rate)
{
	struct aac_decoder *decoder;
	size_t frames;

	/* Convert using this decoder's source queue and retained fractional clock. */
	decoder = state;
	frames = media_pcm_read(&decoder->sound, samples, capacity, rate);
	return frames;
}

/* Flush syntax, noise and channel overlaps; the first decoded seek frame is pre-roll only. */
static void
aac_flush(
	void *state)
{
	struct aac_decoder *decoder;

	/* Seek invalidates every compressed block and previously reconstructed channel overlap. */
	decoder = state;
	aac_blocks_free(decoder->head);
	decoder->head = NULL;
	decoder->tail = NULL;
	memset(decoder->channels, 0, sizeof(decoder->channels));
	media_pcm_reset(&decoder->sound);
	decoder->random = 1U;
	decoder->draining = 0;
	decoder->failed = 0;
	decoder->preroll = 1;
	return;
}

/* Release only decoder-local resources; no external runtime participates in native AAC ownership. */
static void
aac_close(
	void *state)
{
	struct aac_decoder *decoder;

	/* Closing the decoder retires both queued syntax and convolution resources. */
	decoder = state;
	aac_blocks_free(decoder->head);
	media_pcm_close(&decoder->sound);
	free(decoder);
	return;
}

/* Apply a seek boundary after reconstructing pre-roll, preserving sample accuracy inside a frame. */
static int
aac_trim(
	void *state,
	int64_t before_us)
{
	struct aac_decoder *decoder;

	/* Trimming changes presentation without destroying reconstructed pre-roll history. */
	decoder = state;
	media_pcm_trim(&decoder->sound, before_us);
	return 0;
}

/* Audio frame length informs app-side seek pre-roll without an AAC-specific app constant. */
static int64_t
aac_frame_us(
	const void *state)
{
	const struct aac_decoder *decoder;
	int64_t duration;

	/* An unconfigured ADTS stream cannot yet promise a seek pre-roll duration. */
	decoder = state;
	if (decoder->config.rate == 0U)
		return 0;
	duration = (1024000000LL + decoder->config.rate - 1U) / decoder->config.rate;
	return duration;
}

/* Free an unpublished or flushed chain without changing successful packet history. */
static void
aac_blocks_free(
	struct aac_block *block)
{
	struct aac_block *next;

	/* Release every block in the caller-owned chain, including partially admitted packets. */
	while (block != NULL) {
		next = block->next;
		free(block);
		block = next;
	}

	/* The chain contains no surviving syntax allocation. */
	return;
}

/* Parse ASC raw blocks or complete ADTS frames, with protected multi-block boundaries honored. */
static int
aac_packet(
	struct aac_decoder *decoder,
	const struct media_packet *packet,
	struct aac_block **first,
	struct aac_block **last)
{
	struct media_aac_config config;
	struct media_aac_adts transport;
	struct media_bits bits;
	struct aac_block *block;
	size_t offset;
	size_t begin;
	size_t end;
	size_t consumed;
	unsigned count;
	unsigned index;
	int adts;
	int error;

	/* Empty packets are not successful audio blocks, and syntax admission has a finite bound. */
	if (packet->data == NULL)
		return EINVAL;
	if (packet->size == 0U)
		return EINVAL;
	config = decoder->config;
	offset = 0U;
	count = 0U;
	adts = 0;
	if (packet->size >= 2U) {
		if (packet->data[0] == 0xffU && (packet->data[1] & 0xf0U) == 0xf0U)
			adts = 1;
	}

	/* Unframed AAC requires authoritative ASC rather than guessing a profile from channel counts. */
	if (adts == 0 && decoder->configured == 0)
		return ENOTSUP;

	/* Each transport frame may contain several independently aligned raw data blocks. */
	while (offset < packet->size) {
		memset(&transport, 0, sizeof(transport));
		transport.blocks = 1U;
		transport.frame_size = packet->size - offset;
		if (adts != 0) {
			error = media_aac_adts_parse(packet->data + offset, packet->size - offset, &transport);
			if (error != 0)
				return error;
			/* Channel configuration zero retains the last PCE until an in-band PCE replaces it. */
			if (transport.config.channel_configuration == 0U &&
			    config.channel_configuration == 0U &&
			    config.rate == transport.config.rate &&
			    config.channels != 0U) {
				transport.config = config;
			}

			/* Use the transport program after retaining any applicable in-band PCE. */
			config = transport.config;
		}

		/* Begin after the complete transport header, including its optional CRC fields. */
		begin = transport.header_size;

		/* Decode each block without consuming an adjacent CRC as raw channel syntax. */
		for (index = 0U; index < transport.blocks; index++) {
			if (count == AAC_PACKET_BLOCKS)
				return ENOTSUP;
			end = transport.frame_size;
			if (transport.protected_frame != 0U && transport.blocks > 1U) {
				if (index + 1U < transport.blocks)
					end = transport.block_offset[index + 1U];
				if (end < begin + 2U)
					return EINVAL;
				end -= 2U;
			}

			/* Each raw block gets private syntax storage until the whole packet is admitted. */
			block = calloc(1U, sizeof(*block));
			if (block == NULL)
				return ENOMEM;

			/* Retain every allocated block for one complete cleanup path if later syntax fails. */
			if (*last == NULL) {
				*first = block;
			} else {
				(*last)->next = block;
			}

			/* The cleanup chain owns the block before any fallible channel parsing. */
			*last = block;
			media_bits_init(&bits, packet->data + offset + begin, end - begin);
			error = media_aac_frame_parse(&bits, &config, &block->frame);
			if (error != 0)
				return error;
			consumed = bits.position / 8U;

			/* Indexed protected blocks and final unprotected blocks have exact payload endpoints. */
			if (transport.protected_frame != 0U || index + 1U == transport.blocks) {
				if (consumed != end - begin)
					return EINVAL;
			}

			/* An in-band PCE can refine identities but cannot silently change the decoder's source clock. */
			config = block->frame.config;
			if (decoder->configured != 0) {
				if (config.rate != decoder->config.rate)
					return ENOTSUP;
				if (config.channels != decoder->config.channels)
					return ENOTSUP;
			}

			/* Every block in this one packet must use the same reconstruction program. */
			if (count != 0U) {
				if (config.rate != (*first)->frame.config.rate)
					return ENOTSUP;
				if (config.channels != (*first)->frame.config.channels)
					return ENOTSUP;
			}

			/* Keep block timestamps on one rational packet timeline, avoiding accumulated rounding. */
			block->time_us = packet->pts_us + (int64_t)count * 1024000000LL / config.rate;
			count++;
			begin += consumed;
			if (transport.protected_frame != 0U && transport.blocks > 1U)
				begin += 2U;
		}

		/* The next transport frame begins only after every raw block and CRC boundary. */
		offset += transport.frame_size;
	}

	/* Every raw block is ready for atomic publication by send. */
	return 0;
}

/* Reconstruct one admitted block using overlap state identified by class, tag and pair side. */
static int
aac_synthesize(
	struct aac_decoder *decoder,
	struct aac_block *block)
{
	unsigned channel;
	int slot;
	int error;

	/* Reconstruct frequency-domain tools before any retained time-domain overlap changes. */
	error = media_aac_reconstruct(&block->frame, &decoder->random);
	if (error != 0)
		return error;
	for (channel = 0U; channel < block->frame.channels; channel++) {
		slot = aac_identity_slot(decoder, &block->frame, channel);
		if (slot < 0)
			return EINVAL;
		error = media_aac_synthesize(&block->frame.channel[channel], &decoder->channels[slot].history, &decoder->transform, decoder->planar[channel]);
		if (error != 0)
			return error;
	}

	/* Seek pre-roll rebuilds overlap only; it never supplies audible output or a false origin. */
	if (decoder->preroll != 0) {
		decoder->preroll = 0;
		return 0;
	}

	/* Only audible frames enter the shared stereo conversion queue. */
	aac_mix(decoder, &block->frame);
	error = media_pcm_push(&decoder->sound, decoder->stereo, 1024U, block->time_us);
	if (error != 0)
		return error;
	return 0;
}

/* Normalize a stereo downmix before filtering and final saturation; LFE is deliberately excluded. */
static void
aac_mix(
	struct aac_decoder *decoder,
	const struct media_aac_frame *frame)
{
	double weights[MEDIA_AAC_MAX_CHANNELS][2];
	double sums[2];
	double surround;
	double normalization;
	double value;
	unsigned channel;
	unsigned side;
	unsigned sample;

	/* Ordinary multichannel mixing follows center/surround attenuation; PCE can select the surround matrix. */
	memset(weights, 0, sizeof(weights));
	sums[0] = 0.0;
	sums[1] = 0.0;
	surround = sqrt(0.5);
	if (frame->config.matrix_present != 0U) {
		surround = pow(0.5, 0.5 * (double)(frame->config.matrix_index + 1U));
		if (frame->config.matrix_index == 3U)
			surround = 0.0;
	}

	/* Each output identity contributes by its configured speaker group, not by a presumed array slot. */
	for (channel = 0U; channel < frame->channels; channel++) {
		if (frame->channel[channel].group == 3U)
			continue;
		if (frame->channels == 1U) {
			weights[channel][0] = 1.0;
			weights[channel][1] = 1.0;
		} else if (frame->channel[channel].type == 1U) {
			side = frame->channel[channel].side;
			weights[channel][side] = 1.0;
			if (frame->channel[channel].group != 0U)
				weights[channel][side] = surround;
		} else {
			weights[channel][0] = sqrt(0.5);
			weights[channel][1] = sqrt(0.5);
			if (frame->channel[channel].group != 0U) {
				weights[channel][0] = surround * sqrt(0.5);
				weights[channel][1] = surround * sqrt(0.5);
			}
		}

		/* Matrix pseudo-surround carries the rear contribution with opposite right-channel polarity. */
		if (frame->config.pseudo_surround != 0U && frame->channel[channel].group != 0U)
			weights[channel][1] = -weights[channel][1];
		sums[0] += fabs(weights[channel][0]);
		sums[1] += fabs(weights[channel][1]);
	}

	/* Bound either output's sum of absolute gains before accumulating channel samples. */
	normalization = sums[0];
	if (sums[1] > normalization)
		normalization = sums[1];
	if (normalization < 1.0)
		normalization = 1.0;

	/* Accumulate in the native float domain so downmix never clips an intermediate channel. */
	for (sample = 0U; sample < 1024U; sample++) {
		for (side = 0U; side < 2U; side++) {
			value = 0.0;
			for (channel = 0U; channel < frame->channels; channel++)
				value += decoder->planar[channel][sample] * weights[channel][side];
			decoder->stereo[sample * 2U + side] = (float)(value / normalization);
		}
	}

	/* The normalized float downmix is ready for continuous conversion. */
	return;
}

/* Locate a channel's previous overlap, recycling only identities absent from the current complete program. */
static int
aac_identity_slot(
	struct aac_decoder *decoder,
	const struct media_aac_frame *frame,
	unsigned channel)
{
	const struct media_aac_channel *current;
	unsigned slot;
	unsigned other;
	int reusable;

	/* Match the signalled identity before allocating fresh overlap history. */
	current = &frame->channel[channel];
	for (slot = 0U; slot < MEDIA_AAC_MAX_CHANNELS; slot++) {
		if (decoder->channels[slot].valid == 0)
			continue;
		if (decoder->channels[slot].type != current->type)
			continue;
		if (decoder->channels[slot].tag != current->tag)
			continue;
		if (decoder->channels[slot].side != current->side)
			continue;
		return (int)slot;
	}

	/* An identity not used by this frame may supply fresh zero overlap for a newly signalled element. */
	for (slot = 0U; slot < MEDIA_AAC_MAX_CHANNELS; slot++) {
		reusable = 1;
		for (other = 0U; other < frame->channels; other++) {
			if (decoder->channels[slot].valid != 0 &&
			    decoder->channels[slot].type == frame->channel[other].type &&
			    decoder->channels[slot].tag == frame->channel[other].tag &&
			    decoder->channels[slot].side == frame->channel[other].side) {
				reusable = 0;
				break;
			}
		}

		/* Only a channel absent from this complete frame may lose its old history. */
		if (reusable == 0)
			continue;
		memset(&decoder->channels[slot].history, 0, sizeof(decoder->channels[slot].history));
		decoder->channels[slot].type = current->type;
		decoder->channels[slot].tag = current->tag;
		decoder->channels[slot].side = current->side;
		decoder->channels[slot].valid = 1;
		return (int)slot;
	}

	/* The configured channel bound leaves no reusable identity slot. */
	return -1;
}
