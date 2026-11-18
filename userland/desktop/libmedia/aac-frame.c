/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Original AAC-LC raw_data_block syntax, bounded by the transport or container packet. */
#include "aac-frame.h"
#include "aac-codec.h"
#include <errno.h>
#include <string.h>

/* Normative explicit frequencies accepted by the band tables, immutable for all decoders. */
static const uint32_t frame_rates[13] = {96000U, 88200U, 64000U, 48000U, 44100U, 32000U, 24000U, 22050U, 16000U, 12000U, 11025U, 8000U, 7350U};

static int frame_info(struct media_bits *bits, unsigned rate, struct media_aac_ics *info);
static int frame_channel(struct media_bits *bits, unsigned rate, int common, struct media_aac_channel *channel);
static int frame_sections(struct media_bits *bits, struct media_aac_channel *channel);
static int frame_scales(struct media_bits *bits, struct media_aac_channel *channel);
static int frame_tns(struct media_bits *bits, struct media_aac_channel *channel);
static int frame_spectrum(struct media_bits *bits, struct media_aac_channel *channel);
static int frame_pair(struct media_bits *bits, unsigned rate, struct media_aac_frame *frame, unsigned tag);
static int frame_fill(struct media_bits *bits);
static int frame_dynamic_range(struct media_bits *bits);
static int frame_layout(struct media_aac_frame *frame);

/*
 * Parse one complete raw data block before permitting any PCM or overlap-state update.
 */
int
media_aac_frame_parse(
	struct media_bits *bits,
	const struct media_aac_config *config,
	struct media_aac_frame *frame)
{
	struct media_aac_channel *channel;
	unsigned rate;
	unsigned type;
	unsigned tag;
	unsigned count;
	unsigned aligned;
	unsigned escape;
	int error;

	/* Parsing requires caller-owned storage and an established frequency configuration. */
	if (bits == NULL ||
	    config == NULL ||
	    frame == NULL)
		return EINVAL;

	/* Resolve an explicit rate only when a normative band table actually exists. */
	rate = config->rate_index;
	if (rate == 15U) {
		/* A matching table preserves the explicit frequency's exact intended rate. */
		for (rate = 0U; rate < 13U; rate++) {
			/* Stop at the matching normative frequency, rather than guessing a profile. */
			if (frame_rates[rate] == config->rate)
				break;
		}
	}

	/* Unknown explicit rates cannot use an unrelated scalefactor-band layout. */
	if (rate >= 13U)
		return ENOTSUP;

	/* Start fresh scratch syntax without mutating the decoder's overlap histories. */
	memset(frame, 0, sizeof(*frame));
	frame->config = *config;

	/* Each element is parsed to its exact boundary; END terminates this raw block. */
	for (;;) {
		type = media_bits_read(bits, 3U);
		if (bits->error != 0)
			return EINVAL;

		/* END aligns this block and permits the caller to begin the next ADTS block. */
		if (type == 7U)
			break;

		/* Coupling requires its own reconstruction tool, never silently omitted channel audio. */
		if (type == 2U)
			return ENOTSUP;

		/* FIL has its own count and carries implicit SBR signalling that must be refused. */
		if (type == 6U) {
			error = frame_fill(bits);
			if (error != 0)
				return error;
			continue;
		}

		/* PCE refines channel identities without being an output channel itself. */
		if (type == 5U) {
			error = media_aac_pce_parse(bits, &frame->config);
			if (error != 0)
				return error;
			continue;
		}

		/* SCE, CPE, LFE and DSE carry an element-instance tag. */
		tag = media_bits_read(bits, 4U);
		if (bits->error != 0)
			return EINVAL;

		/* Ancillary data has a bounded byte count and optional byte alignment. */
		if (type == 4U) {
			aligned = media_bits_read1(bits);
			count = media_bits_read(bits, 8U);
			if (bits->error != 0)
				return EINVAL;

			/* The maximum count introduces another eight-bit length contribution. */
			if (count == 255U) {
				escape = media_bits_read(bits, 8U);
				if (bits->error != 0)
					return EINVAL;
				count += escape;
			}

			/* Align only when the data stream element requests it. */
			if (aligned != 0U)
				media_bits_align(bits);

			/* Consume only the declared ancillary payload. */
			media_bits_skip(bits, (size_t)count * 8U);
			if (bits->error != 0)
				return EINVAL;
			continue;
		}

		/* A stereo pair parses common-window metadata once for both channels. */
		if (type == 1U) {
			error = frame_pair(bits, rate, frame, tag);
			if (error != 0)
				return error;
			continue;
		}

		/* All supported output channels must fit the decoder's explicit bound. */
		if (frame->channels == MEDIA_AAC_MAX_CHANNELS)
			return ENOTSUP;

		/* A single or LFE channel has its own individual-channel syntax. */
		channel = &frame->channel[frame->channels];
		channel->type = (uint8_t)type;
		channel->tag = (uint8_t)tag;
		error = frame_channel(bits, rate, 0, channel);
		if (error != 0)
			return error;
		frame->channels++;
	}

	/* An ended frame must have valid alignment before its identities are admitted. */
	media_bits_align(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Match the transmitted output elements to the configured channel layout. */
	error = frame_layout(frame);
	if (error != 0)
		return error;

	/* Succeeded: the complete LC syntax can proceed to reconstruction tools. */
	return 0;
}

/* Read the window sequence and grouping, rejecting prediction outside the LC profile. */
static int
frame_info(
	struct media_bits *bits,
	unsigned rate,
	struct media_aac_ics *info)
{
	unsigned reserved;
	unsigned grouping;
	unsigned index;
	unsigned predicted;

	/* The reserved bit, sequence and window shape establish the following field widths. */
	reserved = media_bits_read1(bits);
	info->sequence = (uint8_t)media_bits_read(bits, 2U);
	info->shape = (uint8_t)media_bits_read1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* A reserved ICS bit is malformed LC syntax. */
	if (reserved != 0U)
		return EINVAL;

	/* Eight-short syntax groups consecutive windows with one grouping bit per boundary. */
	info->groups = 1U;
	info->group_length[0] = 1U;
	if (info->sequence == 2U) {
		info->windows = 8U;
		info->band_count = media_aac_bands[rate].short_count;
		info->tns_limit = media_aac_bands[rate].short_tns;
		info->offsets = media_aac_bands[rate].short_offsets;
		info->max_sfb = (uint8_t)media_bits_read(bits, 4U);
		grouping = media_bits_read(bits, 7U);
		if (bits->error != 0)
			return EINVAL;

		/* A set grouping bit joins the current window to its predecessor. */
		for (index = 0U; index < 7U; index++) {
			/* Start a new group when the bit marks a window boundary. */
			if ((grouping & (1U << (6U - index))) == 0U) {
				info->group_length[info->groups] = 1U;
				info->groups++;
			} else {
				info->group_length[info->groups - 1U]++;
			}
		}
	} else {
		info->windows = 1U;
		info->band_count = media_aac_bands[rate].long_count;
		info->tns_limit = media_aac_bands[rate].long_tns;
		info->offsets = media_aac_bands[rate].long_offsets;
		info->max_sfb = (uint8_t)media_bits_read(bits, 6U);
		predicted = media_bits_read1(bits);
		if (bits->error != 0)
			return EINVAL;

		/* Prediction is not an LC reconstruction tool. */
		if (predicted != 0U)
			return ENOTSUP;
	}

	/* A band count cannot index beyond the frequency's normative offsets. */
	if (info->max_sfb > info->band_count)
		return EINVAL;

	/* Succeeded: grouping covers exactly one long or eight short windows. */
	return 0;
}

/* Parse one channel's side information, spectrum and optional pulse corrections. */
static int
frame_channel(
	struct media_bits *bits,
	unsigned rate,
	int common,
	struct media_aac_channel *channel)
{
	unsigned present;
	unsigned pulses;
	unsigned start_band;
	unsigned position;
	unsigned index;
	unsigned offsets[4];
	unsigned amplitudes[4];
	int error;
	int corrected;

	/* Global gain belongs to each channel, even for shared ICS information. */
	channel->gain = (uint8_t)media_bits_read(bits, 8U);
	if (bits->error != 0)
		return EINVAL;

	/* Independent channels carry their own window metadata immediately after gain. */
	if (common == 0) {
		error = frame_info(bits, rate, &channel->info);
		if (error != 0)
			return error;
	}

	/* Section books and differential scalefactors precede optional reconstruction tools. */
	error = frame_sections(bits, channel);
	if (error != 0)
		return error;

	/* Resolve spectral, noise and intensity scalefactors independently. */
	error = frame_scales(bits, channel);
	if (error != 0)
		return error;

	/* Pulse data is legal only for a long window, and has at most four positions. */
	pulses = 0U;
	position = 0U;
	present = media_bits_read1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* A pulse starts at one scalefactor band and advances by transmitted offsets. */
	if (present != 0U) {
		/* Eight-short windows cannot carry pulse data. */
		if (channel->info.sequence == 2U)
			return EINVAL;
		pulses = media_bits_read(bits, 2U) + 1U;
		start_band = media_bits_read(bits, 6U);
		if (bits->error != 0)
			return EINVAL;

		/* The initial pulse band must exist in the full normative layout. */
		if (start_band >= channel->info.band_count)
			return EINVAL;
		position = channel->info.offsets[start_band];

		/* Keep all pulse fields until the quantized spectrum has been decoded. */
		for (index = 0U; index < pulses; index++) {
			offsets[index] = media_bits_read(bits, 5U);
			amplitudes[index] = media_bits_read(bits, 4U);
			if (bits->error != 0)
				return EINVAL;
		}
	}

	/* TNS is parsed before the spectral Huffman payload. */
	present = media_bits_read1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Only a signalled tool contributes a TNS payload. */
	if (present != 0U) {
		error = frame_tns(bits, channel);
		if (error != 0)
			return error;
	}

	/* Gain control belongs to other audio profiles and cannot be ignored. */
	present = media_bits_read1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* A signalled gain-control payload would require another reconstruction path. */
	if (present != 0U)
		return ENOTSUP;

	/* Decode grouped bands directly into per-window spectral positions. */
	error = frame_spectrum(bits, channel);
	if (error != 0)
		return error;

	/* Pulse magnitudes extend their original sign, with zero taking the negative branch. */
	for (index = 0U; index < pulses; index++) {
		position += offsets[index];

		/* Pulse positions cannot reach outside the long spectrum. */
		if (position >= 1024U)
			return EINVAL;
		corrected = channel->quantized[position];

		/* Positive coefficients grow positive; zero and negative coefficients grow negative. */
		if (corrected > 0) {
			corrected += (int)amplitudes[index];
		} else {
			corrected -= (int)amplitudes[index];
		}

		/* AAC's quantized spectrum remains within its normative escape range. */
		if (corrected < -8191 || corrected > 8191)
			return EINVAL;
		channel->quantized[position] = (int16_t)corrected;
	}

	/* Succeeded: channel syntax is complete before any synthesis history changes. */
	return 0;
}

/* Expand each section length into per-group band codebooks. */
static int
frame_sections(
	struct media_bits *bits,
	struct media_aac_channel *channel)
{
	unsigned width;
	unsigned escape;
	unsigned group;
	unsigned band;
	unsigned book;
	unsigned length;
	unsigned increment;
	unsigned index;

	/* Short sections use three-bit length increments, long sections five. */
	width = 5U;
	if (channel->info.sequence == 2U)
		width = 3U;
	escape = (1U << width) - 1U;

	/* Every section must consume at least one band inside the signalled max_sfb. */
	for (group = 0U; group < channel->info.groups; group++) {
		/* Decode successive section boundaries for this group. */
		band = 0U;
		while (band < channel->info.max_sfb) {
			book = media_bits_read(bits, 4U);
			if (bits->error != 0)
				return EINVAL;

			/* Codebook twelve is reserved rather than a spectral or pseudo book. */
			if (book == 12U)
				return EINVAL;

			/* Accumulate escaped lengths without crossing the remaining bands. */
			length = 0U;
			for (;;) {
				increment = media_bits_read(bits, width);
				if (bits->error != 0)
					return EINVAL;
				length += increment;

				/* A section cannot name bands beyond its current group. */
				if (length > channel->info.max_sfb - band)
					return EINVAL;

				/* A nonescape increment terminates this section's length. */
				if (increment != escape)
					break;
			}

			/* A zero-length section would make no progress through the band syntax. */
			if (length == 0U)
				return EINVAL;

			/* All bands in the section use its transmitted book. */
			for (index = 0U; index < length; index++)
				channel->books[group][band + index] = (uint8_t)book;
			band += length;
		}
	}

	/* Succeeded: every signalled band has one codebook. */
	return 0;
}

/* Decode independent spectral-gain, intensity-position and noise-energy accumulators. */
static int
frame_scales(
	struct media_bits *bits,
	struct media_aac_channel *channel)
{
	unsigned group;
	unsigned band;
	unsigned book;
	int spectral;
	int noise;
	int intensity;
	int first_noise;
	int difference;
	int error;

	/* The noise offset is normative; intensity position begins at zero independently of gain. */
	spectral = channel->gain;
	noise = (int)channel->gain - 90;
	intensity = 0;
	first_noise = 1;

	/* Read only nonzero books, preserving each accumulator across groups. */
	for (group = 0U; group < channel->info.groups; group++) {
		/* Each book selects its own scalefactor syntax and interpretation. */
		for (band = 0U; band < channel->info.max_sfb; band++) {
			book = channel->books[group][band];

			/* Zero bands carry no scalefactor or spectral bits. */
			if (book == 0U)
				continue;

			/* The first noise band uses an unsigned nine-bit difference biased by 256. */
			if (book == 13U && first_noise != 0) {
				difference = (int)media_bits_read(bits, 9U) - 256;
				if (bits->error != 0)
					return EINVAL;
				first_noise = 0;
			} else {
				error = media_aac_scalefactor(bits, &difference);
				if (error != 0)
					return error;
			}

			/* Intensity belongs only to the right member of a channel pair. */
			if (book >= 14U) {
				/* Another channel cannot reconstruct an intensity band from an absent left partner. */
				if (channel->type != 1U || channel->side != 1U)
					return EINVAL;

				/* Intensity reconstruction requires a shared window and band layout. */
				if (channel->common == 0U)
					return EINVAL;
				intensity += difference;

				/* Bound logarithmic positions before evaluating reconstruction powers. */
				if (intensity < -255 || intensity > 255)
					return EINVAL;
				channel->scales[group][band] = intensity;
			} else if (book == 13U) {
				noise += difference;

				/* A noise energy outside the finite reconstruction domain is malformed. */
				if (noise < -255 || noise > 255)
					return EINVAL;
				channel->scales[group][band] = noise;
			} else {
				spectral += difference;

				/* Spectral gains use the normative eight-bit absolute scale domain. */
				if (spectral < 0 || spectral > 255)
					return EINVAL;
				channel->scales[group][band] = spectral;
			}
		}
	}

	/* Succeeded: differential syntax is resolved into per-band absolute scales. */
	return 0;
}

/* Read all window-local TNS filters with LC order limits and signed coefficient indices. */
static int
frame_tns(
	struct media_bits *bits,
	struct media_aac_channel *channel)
{
	struct media_aac_tns *filter;
	unsigned window;
	unsigned index;
	unsigned coefficient;
	unsigned resolution;
	unsigned compressed;
	unsigned width;
	unsigned sign;
	unsigned order_limit;
	unsigned length_width;
	unsigned order_width;
	unsigned count_width;
	int decoded;

	/* Long and short windows transmit different count, length and order widths. */
	length_width = 6U;
	order_width = 5U;
	order_limit = 12U;
	count_width = 2U;
	if (channel->info.sequence == 2U) {
		length_width = 4U;
		order_width = 3U;
		order_limit = 7U;
		count_width = 1U;
	}

	/* Filters apply independently to each transform window. */
	for (window = 0U; window < channel->info.windows; window++) {
		channel->tns_count[window] = (uint8_t)media_bits_read(bits, count_width);
		if (bits->error != 0)
			return EINVAL;

		/* A window without filters carries no resolution flag. */
		if (channel->tns_count[window] == 0U)
			continue;
		resolution = media_bits_read1(bits) + 3U;
		if (bits->error != 0)
			return EINVAL;

		/* Each filter has its own length, order and optional direction/coefficients. */
		for (index = 0U; index < channel->tns_count[window]; index++) {
			filter = &channel->tns[window][index];
			filter->length = (uint8_t)media_bits_read(bits, length_width);
			filter->order = (uint8_t)media_bits_read(bits, order_width);
			filter->resolution = (uint8_t)resolution;
			if (bits->error != 0)
				return EINVAL;

			/* LC cannot reconstruct an order above its profile limit. */
			if (filter->order > order_limit)
				return EINVAL;

			/* A zero-order region transmits no direction or coefficient payload. */
			if (filter->order == 0U)
				continue;
			filter->direction = (uint8_t)media_bits_read1(bits);
			compressed = media_bits_read1(bits);
			if (bits->error != 0)
				return EINVAL;
			width = resolution - compressed;
			sign = 1U << (width - 1U);

			/* Two's-complement indices retain the uncompressed resolution for reflection conversion. */
			for (coefficient = 0U; coefficient < filter->order; coefficient++) {
				decoded = (int)media_bits_read(bits, width);
				if (bits->error != 0)
					return EINVAL;

				/* Extend the transmitted signed index before narrowing it into frame storage. */
				if (((unsigned)decoded & sign) != 0U)
					decoded -= (int)(1U << width);
				filter->coefficients[coefficient] = (int8_t)decoded;
			}
		}
	}

	/* Succeeded: every TNS filter is represented independently of synthesis arithmetic. */
	return 0;
}

/* Deinterleave grouped spectral tuples into each transform window's original frequency order. */
static int
frame_spectrum(
	struct media_bits *bits,
	struct media_aac_channel *channel)
{
	unsigned group;
	unsigned band;
	unsigned first_window;
	unsigned window;
	unsigned window_size;
	unsigned start;
	unsigned end;
	unsigned position;
	unsigned book;
	unsigned count;
	unsigned index;
	int16_t tuple[4];
	int error;

	/* Each short transform owns 128 coefficients; a long transform owns 1024. */
	window_size = 1024U;
	if (channel->info.sequence == 2U)
		window_size = 128U;

	/* Transmitted order is group, band, grouped window, then tuple within that band. */
	first_window = 0U;
	for (group = 0U; group < channel->info.groups; group++) {
		/* Non-spectral pseudo books have no Huffman coefficient payload. */
		for (band = 0U; band < channel->info.max_sfb; band++) {
			book = channel->books[group][band];

			/* Zero, noise and intensity produce their coefficients during reconstruction. */
			if (book == 0U || book >= 13U)
				continue;
			start = channel->info.offsets[band];
			end = channel->info.offsets[band + 1U];

			/* Every grouped window contributes its own complete band width. */
			for (window = 0U; window < channel->info.group_length[group]; window++) {
				/* Decode pairs or quads until this window's band is filled. */
				position = start;
				while (position < end) {
					error = media_aac_spectral(bits, book, tuple, &count);
					if (error != 0)
						return error;

					/* The tuple must fit the band's remaining frequency positions. */
					if (count > end - position)
						return EINVAL;

					/* Publish quantized values only into this band's own transform window. */
					for (index = 0U; index < count; index++)
						channel->quantized[(first_window + window) * window_size + position + index] = tuple[index];
					position += count;
				}
			}
		}

		/* Advance past every window represented by the completed group. */
		first_window += channel->info.group_length[group];
	}

	/* Succeeded: all spectral Huffman syntax has been consumed without padded read-ahead. */
	return 0;
}

/* Parse a channel pair, preserving common-window M/S information on its left channel. */
static int
frame_pair(
	struct media_bits *bits,
	unsigned rate,
	struct media_aac_frame *frame,
	unsigned tag)
{
	struct media_aac_channel *left;
	struct media_aac_channel *right;
	unsigned common;
	unsigned mode;
	unsigned group;
	unsigned band;
	int error;

	/* A complete pair requires two available output-channel slots. */
	if (frame->channels > MEDIA_AAC_MAX_CHANNELS - 2U)
		return ENOTSUP;

	/* Give both members the same element identity and distinguish their pair side. */
	left = &frame->channel[frame->channels];
	right = &frame->channel[frame->channels + 1U];
	left->type = 1U;
	right->type = 1U;
	left->tag = (uint8_t)tag;
	right->tag = (uint8_t)tag;
	right->side = 1U;
	common = media_bits_read1(bits);
	if (bits->error != 0)
		return EINVAL;
	left->common = (uint8_t)common;
	right->common = (uint8_t)common;

	/* Common-window information precedes either channel's global gain. */
	if (common != 0U) {
		error = frame_info(bits, rate, &left->info);
		if (error != 0)
			return error;
		right->info = left->info;
		mode = media_bits_read(bits, 2U);
		if (bits->error != 0)
			return EINVAL;

		/* M/S mode three is reserved in ordinary AAC-LC. */
		if (mode == 3U)
			return EINVAL;
		left->ms_mode = (uint8_t)mode;

		/* Mode one carries explicit masks; mode two enables all bands. */
		for (group = 0U; group < left->info.groups; group++) {
			/* Stereo side information follows group and band order. */
			for (band = 0U; band < left->info.max_sfb; band++) {
				/* Only explicit masks consume a bit for each band. */
				if (mode == 1U) {
					left->ms[group][band] = (uint8_t)media_bits_read1(bits);
					if (bits->error != 0)
						return EINVAL;
				} else if (mode == 2U) {
					left->ms[group][band] = 1U;
				}
			}
		}
	}

	/* Decode the left channel before advancing to the right channel's own gain and spectrum. */
	error = frame_channel(bits, rate, (int)common, left);
	if (error != 0)
		return error;

	/* The right member retains common-window metadata but has independent scalefactors. */
	error = frame_channel(bits, rate, (int)common, right);
	if (error != 0)
		return error;
	frame->channels += 2U;

	/* Succeeded: both members and their complete masks belong to the frame. */
	return 0;
}

/* Parse bounded fill extensions, refusing SBR instead of reconstructing only its LC core. */
static int
frame_fill(
	struct media_bits *bits)
{
	struct media_bits payload;
	unsigned count;
	unsigned extra;
	unsigned extension;
	unsigned version;
	unsigned length;
	unsigned increment;
	int error;

	/* Fifteen count bytes introduce an eight-bit extension with a bias of fourteen. */
	count = media_bits_read(bits, 4U);
	if (bits->error != 0)
		return EINVAL;

	/* The extended count belongs to the FIL header, not to its payload. */
	if (count == 15U) {
		extra = media_bits_read(bits, 8U);
		if (bits->error != 0)
			return EINVAL;
		count = 14U + extra;
	}

	/* A fill payload is bounded even when it begins at a non-byte-aligned cursor. */
	payload = *bits;
	media_bits_skip(bits, (size_t)count * 8U);
	if (bits->error != 0)
		return EINVAL;
	payload.count = bits->position;

	/* Iterate extension boundaries rather than searching arbitrary data for a false SBR marker. */
	while (payload.position < payload.count) {
		extension = media_bits_read(&payload, 4U);
		if (payload.error != 0)
			return EINVAL;

		/* Both SBR payload types, with and without CRC, are unsupported HE-AAC. */
		if (extension == 13U || extension == 14U)
			return ENOTSUP;

		/* Fill and fill-data occupy the remainder of this declared fill element. */
		if (extension == 0U || extension == 1U)
			break;

		/* Dynamic-range metadata can precede a later SBR extension in the same payload. */
		if (extension == 11U) {
			error = frame_dynamic_range(&payload);
			if (error != 0)
				return error;
			continue;
		}

		/* The data-element version zero has a length-prefixed byte payload. */
		if (extension != 2U)
			return ENOTSUP;
		version = media_bits_read(&payload, 4U);
		if (payload.error != 0)
			return EINVAL;

		/* Future data versions have no known extension boundary here. */
		if (version != 0U)
			return ENOTSUP;
		length = 0U;

		/* Byte-sized length increments terminate at the first value below 255. */
		for (;;) {
			increment = media_bits_read(&payload, 8U);
			if (payload.error != 0)
				return EINVAL;
			length += increment;

			/* The length cannot exceed the entire bounded fill payload. */
			if (length > count)
				return EINVAL;

			/* A final length contribution below 255 ends the extended count. */
			if (increment != 255U)
				break;
		}

		/* Consume ancillary bytes without interpreting their bit patterns as codec signalling. */
		media_bits_skip(&payload, (size_t)length * 8U);
		if (payload.error != 0)
			return EINVAL;
	}

	/* Succeeded: no implicit SBR tool was enabled in a parsed fill extension. */
	return 0;
}

/* Consume dynamic-range metadata to find the exact boundary of the next fill extension. */
static int
frame_dynamic_range(
	struct media_bits *bits)
{
	unsigned present;
	unsigned additional;
	unsigned bands;

	/* Optional PCE identity occupies a full byte after its presence flag. */
	present = media_bits_read1(bits);
	if (present != 0U)
		media_bits_skip(bits, 8U);

	/* Excluded-channel groups use seven mask bits and one continuation bit. */
	present = media_bits_read1(bits);
	if (present != 0U) {
		/* Continue until the excluded-channel mask explicitly ends. */
		for (;;) {
			media_bits_skip(bits, 7U);
			additional = media_bits_read1(bits);
			if (bits->error != 0)
				return EINVAL;

			/* A zero continuation ends the channel exclusion groups. */
			if (additional == 0U)
				break;
		}
	}

	/* Optional band boundaries specify how many control bytes follow. */
	bands = 1U;
	present = media_bits_read1(bits);
	if (present != 0U) {
		bands += media_bits_read(bits, 4U);
		media_bits_skip(bits, 4U + (size_t)bands * 8U);
	}

	/* Program reference level has its own presence flag and seven-bit field plus reserved bit. */
	present = media_bits_read1(bits);
	if (present != 0U)
		media_bits_skip(bits, 8U);

	/* One signed dynamic-range control byte belongs to each declared band. */
	media_bits_skip(bits, (size_t)bands * 8U);
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the cursor points to the next extension rather than to a control coefficient. */
	return 0;
}

/* Validate configured element identities and assign each output channel its speaker group. */
static int
frame_layout(
	struct media_aac_frame *frame)
{
	struct media_aac_channel *channel;
	unsigned index;
	unsigned previous;
	unsigned element;
	unsigned single;
	unsigned pair;
	unsigned low;
	unsigned configuration;
	int found;

	/* An absent program configuration cannot be inferred from arbitrary output elements. */
	if (frame->config.channels == 0U)
		return ENOTSUP;

	/* Every configured output channel must be present exactly once in this frame. */
	if (frame->channels != frame->config.channels)
		return EINVAL;
	single = 0U;
	pair = 0U;
	low = 0U;
	configuration = frame->config.channel_configuration;

	/* PCE identities are explicit; ordinary layouts retain their transmitted element order. */
	for (index = 0U; index < frame->channels; index++) {
		channel = &frame->channel[index];

		/* A repeated class/tag/side would synthesize the same configured channel twice. */
		for (previous = 0U; previous < index; previous++) {
			/* Pair members share a tag but have distinct side identities. */
			if (frame->channel[previous].type == channel->type && frame->channel[previous].tag == channel->tag) {
				/* Only the opposite side of a stereo pair may share this identity. */
				if (frame->channel[previous].side == channel->side)
					return EINVAL;
			}
		}

		/* LFE has a separate speaker group even for ordinary channel configurations. */
		channel->group = 0U;
		if (channel->type == 3U)
			channel->group = 3U;

		/* Standard configurations place one front pair before rear or side pairs. */
		if (configuration != 0U) {
			if (channel->type == 1U) {
				if (pair != 0U)
					channel->group = 2U;
				if (configuration == 7U && pair == 1U)
					channel->group = 1U;
				if (channel->side == 1U)
					pair++;
			} else if (channel->type == 0U) {
				if (configuration == 4U && single == 1U)
					channel->group = 2U;
				single++;
			} else if (channel->type == 3U) {
				low++;
			}
		}

		/* Explicit PCE groups override the default ordering with class-specific tags. */
		if (frame->config.channel_configuration == 0U) {
			found = 0;

			/* Match one transmitted element against the program's declared identities. */
			for (element = 0U; element < frame->config.element_count; element++) {
				/* Each configured identity is a class and instance-tag pair. */
				if (frame->config.elements[element].type == channel->type && frame->config.elements[element].tag == channel->tag) {
					channel->group = frame->config.elements[element].group;
					found = 1;
					break;
				}
			}

			/* A channel outside the signalled program is not valid configured output. */
			if (found == 0)
				return EINVAL;
		}
	}

	/* Channel counts alone cannot admit a wrong collection of single, paired or LFE elements. */
	if (configuration == 1U &&
	    (single != 1U ||
	    pair != 0U ||
	    low != 0U))
		return EINVAL;
	if (configuration == 2U &&
	    (single != 0U ||
	    pair != 1U ||
	    low != 0U))
		return EINVAL;
	if (configuration == 3U &&
	    (single != 1U ||
	    pair != 1U ||
	    low != 0U))
		return EINVAL;
	if (configuration == 4U &&
	    (single != 2U ||
	    pair != 1U ||
	    low != 0U))
		return EINVAL;
	if (configuration == 5U &&
	    (single != 1U ||
	    pair != 2U ||
	    low != 0U))
		return EINVAL;
	if (configuration == 6U &&
	    (single != 1U ||
	    pair != 2U ||
	    low != 1U))
		return EINVAL;
	if (configuration == 7U &&
	    (single != 1U ||
	    pair != 3U ||
	    low != 1U))
		return EINVAL;

	/* Succeeded: all output identities belong to the configured LC program. */
	return 0;
}
