/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Original parsing of AAC AudioSpecificConfig, program configuration and
 * ADTS framing.  ISO/IEC 14496-3's GA syntax and ISO/IEC 13818-7's ADTS
 * syntax supply the fields.  Non-LC profiles and signalled SBR/PS are
 * unsupported, never a request to decode only an HE-AAC core.
 */

#include "aac-input.h"

#include <errno.h>
#include <string.h>

/* Standard sampling-frequency indices, immutable for the library lifetime. */
static const uint32_t aac_rates[13] = {
    96000U, 88200U, 64000U, 48000U, 44100U, 32000U, 24000U,
    22050U, 16000U, 12000U, 11025U, 8000U, 7350U};

/* Standard channel configurations, including eight channels for configuration seven. */
static const uint8_t aac_channels[8] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 8U};

static uint32_t aac_object_type(struct media_bits *bits);
static int aac_frequency(struct media_bits *bits, uint32_t *rate, uint8_t *index);
static int aac_sync_extension(struct media_bits *bits);
static int aac_pce_element(struct media_bits *bits, struct media_aac_config *config, unsigned group, unsigned type);

/*
 * Parse an AAC-LC AudioSpecificConfig, distinguishing unsupported profiles from bad input.
 */
int
media_aac_config_parse(
	const uint8_t *data,
	size_t size,
	struct media_aac_config *config)
{
	struct media_aac_config parsed;
	struct media_bits bits;
	uint32_t object;
	uint32_t channel_configuration;
	uint32_t short_frame;
	uint32_t core_dependency;
	uint32_t extension;
	uint32_t reserved;
	int error;

	/* Missing configuration has no inferred profile or sampling frequency. */
	if (config == NULL || data == NULL || size == 0U)
		return EINVAL;

	/* Parse into temporary storage so a refusal never publishes a partial configuration. */
	memset(&parsed, 0, sizeof(parsed));
	media_bits_init(&bits, data, size);
	object = aac_object_type(&bits);
	error = aac_frequency(&bits, &parsed.rate, &parsed.rate_index);
	if (error != 0)
		return error;
	channel_configuration = media_bits_read(&bits, 4U);
	if (bits.error != 0)
		return EINVAL;

	/* LC is the only implemented audio object type, including the escaped AOT case. */
	if (object != 2U)
		return ENOTSUP;

	/* Unknown channel layouts require another profile implementation. */
	if (channel_configuration > 7U)
		return ENOTSUP;
	parsed.channel_configuration = (uint8_t)channel_configuration;
	parsed.channels = aac_channels[channel_configuration];

	/* GA configuration flags precede a possible program configuration. */
	short_frame = media_bits_read1(&bits);
	core_dependency = media_bits_read1(&bits);
	if (core_dependency != 0U)
		media_bits_skip(&bits, 14U);
	extension = media_bits_read1(&bits);
	if (bits.error != 0)
		return EINVAL;

	/* The planned LC filterbank has 1024 samples and no dependent core coder. */
	if (short_frame != 0U)
		return ENOTSUP;

	/* A dependent core coder is outside this standalone LC decoder. */
	if (core_dependency != 0U)
		return ENOTSUP;

	/* Configuration zero obtains its channels and element identities from the PCE. */
	if (channel_configuration == 0U) {
		error = media_aac_pce_parse(&bits, &parsed);
		if (error != 0)
			return error;
	}

	/* LC's future extension flag has no defined payload that this decoder accepts. */
	if (extension != 0U) {
		reserved = media_bits_read1(&bits);
		if (bits.error != 0)
			return EINVAL;

		/* A nonzero future-extension flag enables unsupported syntax. */
		if (reserved != 0U)
			return ENOTSUP;
	}

	/* Backward-compatible SBR and PS signalling is still unsupported HE-AAC. */
	error = aac_sync_extension(&bits);
	if (error != 0)
		return error;

	/* Succeeded: the caller receives only a complete LC configuration. */
	*config = parsed;
	return 0;
}

/*
 * Parse a PCE at the current cursor and retain the transmitted channel identities.
 */
int
media_aac_pce_parse(
	struct media_bits *bits,
	struct media_aac_config *config)
{
	struct media_aac_config parsed;
	uint32_t counts[3];
	uint32_t lfe;
	uint32_t associated;
	uint32_t coupling;
	uint32_t object;
	uint32_t rate_index;
	uint32_t present;
	uint32_t group;
	uint32_t element;
	uint32_t pair;
	uint32_t comments;
	int error;

	/* A PCE refines an existing sampling-frequency configuration. */
	if (bits == NULL || config == NULL)
		return EINVAL;
	parsed = *config;
	parsed.channels = 0U;
	parsed.element_count = 0U;
	parsed.matrix_present = 0U;
	parsed.matrix_index = 0U;
	parsed.pseudo_surround = 0U;
	memset(parsed.elements, 0, sizeof(parsed.elements));

	/* The header identifies the program and counts each group of syntax elements. */
	parsed.program_tag = (uint8_t)media_bits_read(bits, 4U);
	object = media_bits_read(bits, 2U) + 1U;
	rate_index = media_bits_read(bits, 4U);
	counts[0] = media_bits_read(bits, 4U);
	counts[1] = media_bits_read(bits, 4U);
	counts[2] = media_bits_read(bits, 4U);
	lfe = media_bits_read(bits, 2U);
	associated = media_bits_read(bits, 3U);
	coupling = media_bits_read(bits, 4U);
	if (bits->error != 0)
		return EINVAL;

	/* The program must be LC at the configured sampling frequency. */
	if (object != 2U)
		return ENOTSUP;

	/* Reserved frequency indices cannot describe a PCE program. */
	if (rate_index >= 13U)
		return EINVAL;

	/* A PCE cannot silently change the outer configuration's sampling frequency. */
	if (aac_rates[rate_index] != parsed.rate)
		return EINVAL;

	/* Mono and stereo mixdown selectors precede the optional matrix specification. */
	present = media_bits_read1(bits);
	if (present != 0U)
		media_bits_skip(bits, 4U);
	present = media_bits_read1(bits);
	if (present != 0U)
		media_bits_skip(bits, 4U);
	present = media_bits_read1(bits);
	if (present != 0U) {
		parsed.matrix_present = 1U;
		parsed.matrix_index = (uint8_t)media_bits_read(bits, 2U);
		parsed.pseudo_surround = (uint8_t)media_bits_read1(bits);
	}

	/* Front, side and back elements remain in their signalled order. */
	for (group = 0U; group < 3U; group++) {
		/* A pair contributes two channels but still has only one element tag. */
		for (element = 0U; element < counts[group]; element++) {
			pair = media_bits_read1(bits);
			error = aac_pce_element(bits, &parsed, group, pair);
			if (error != 0)
				return error;
		}
	}

	/* LFE elements occupy their own identity namespace. */
	for (element = 0U; element < lfe; element++) {
		error = aac_pce_element(bits, &parsed, 3U, 3U);
		if (error != 0)
			return error;
	}

	/* Associated-data and coupling declarations do not themselves add output channels. */
	media_bits_skip(bits, (size_t)associated * 4U);
	media_bits_skip(bits, (size_t)coupling * 5U);
	media_bits_align(bits);
	comments = media_bits_read(bits, 8U);
	media_bits_skip(bits, (size_t)comments * 8U);
	if (bits->error != 0)
		return EINVAL;

	/* A program with no output channels cannot produce PCM. */
	if (parsed.channels == 0U)
		return EINVAL;

	/* Succeeded: the entire PCE and its comment have been consumed. */
	*config = parsed;
	return 0;
}

/*
 * Parse one ADTS header and frame boundary without decoding or checking CRC values.
 */
int
media_aac_adts_parse(
	const uint8_t *data,
	size_t size,
	struct media_aac_adts *frame)
{
	struct media_aac_adts parsed;
	struct media_bits bits;
	uint32_t sync;
	uint32_t layer;
	uint32_t absent;
	uint32_t object;
	uint32_t rate_index;
	uint32_t channels;
	uint32_t position;
	unsigned block;
	size_t previous;

	/* A complete fixed and variable header occupies seven bytes. */
	if (data == NULL || frame == NULL || size < 7U)
		return EINVAL;

	/* Read the fixed fields before indexing either normative table. */
	memset(&parsed, 0, sizeof(parsed));
	media_bits_init(&bits, data, size);
	sync = media_bits_read(&bits, 12U);
	media_bits_skip(&bits, 1U);
	layer = media_bits_read(&bits, 2U);
	absent = media_bits_read1(&bits);
	object = media_bits_read(&bits, 2U) + 1U;
	rate_index = media_bits_read(&bits, 4U);
	media_bits_skip(&bits, 1U);
	channels = media_bits_read(&bits, 3U);
	media_bits_skip(&bits, 4U);
	parsed.frame_size = media_bits_read(&bits, 13U);
	media_bits_skip(&bits, 11U);
	parsed.blocks = (uint8_t)(media_bits_read(&bits, 2U) + 1U);
	if (bits.error != 0)
		return EINVAL;

	/* Sync, layer and reserved rate indices distinguish malformed transport. */
	if (sync != 0xfffU)
		return EINVAL;

	/* AAC's ADTS layer field is zero. */
	if (layer != 0U)
		return EINVAL;

	/* Reserved frequency indices have no ADTS sampling rate. */
	if (rate_index >= 13U)
		return EINVAL;

	/* A non-LC object cannot be treated as LC based only on its transport. */
	if (object != 2U)
		return ENOTSUP;
	parsed.config.rate = aac_rates[rate_index];
	parsed.config.rate_index = (uint8_t)rate_index;
	parsed.config.channel_configuration = (uint8_t)channels;
	parsed.config.channels = aac_channels[channels];
	parsed.header_size = 7U;

	/* Protected multi-block frames have positions before the header CRC. */
	if (absent == 0U) {
		parsed.protected_frame = 1U;
		parsed.header_size = 9U + (size_t)(parsed.blocks - 1U) * 2U;

		/* The packet must contain every protected-header field. */
		if (parsed.header_size > size)
			return EINVAL;

		/* Each relative position starts a subsequent raw data block, after the preceding CRC. */
		for (block = 1U; block < parsed.blocks; block++) {
			position = media_bits_read(&bits, 16U);
			parsed.block_offset[block] = parsed.header_size + position;
		}

		/* CRC words are framed here; actual raw data block syntax follows in the frame parser. */
		media_bits_skip(&bits, 16U);
		if (bits.error != 0)
			return EINVAL;
	}

	/* Refuse an incomplete frame or a length too small for all raw data blocks. */
	if (parsed.frame_size > size)
		return EINVAL;

	/* Each announced raw block requires at least its own END byte. */
	if (parsed.frame_size < parsed.header_size + parsed.blocks)
		return EINVAL;

	/* The first block follows the complete transport header. */
	parsed.block_offset[0] = parsed.header_size;

	/* Protected multi-block offsets must leave an END byte and CRC per block. */
	if (parsed.protected_frame != 0U && parsed.blocks > 1U) {
		previous = parsed.header_size;
		for (block = 1U; block < parsed.blocks; block++) {
			/* Offsets increase and remain within the declared frame. */
			if (parsed.block_offset[block] <= previous)
				return EINVAL;

			/* Every preceding block includes at least an END byte and a CRC. */
			if (parsed.block_offset[block] - previous < 3U)
				return EINVAL;

			/* A later block must begin before the declared frame ends. */
			if (parsed.block_offset[block] > parsed.frame_size)
				return EINVAL;

			/* Compare the next position with this now-validated block start. */
			previous = parsed.block_offset[block];
		}

		/* The final block also includes its own trailing CRC word. */
		if (parsed.frame_size - previous < 3U)
			return EINVAL;
	}

	/* Succeeded: the caller may consume one frame even when the packet contains several. */
	*frame = parsed;
	return 0;
}

/* Read an ordinary or escaped MPEG-4 audio object type. */
static uint32_t
aac_object_type(
	struct media_bits *bits)
{
	uint32_t object;

	/* Thirty-one introduces a six-bit extension to the object type. */
	object = media_bits_read(bits, 5U);
	if (object == 31U)
		object = 32U + media_bits_read(bits, 6U);

	/* A truncated type is left to the parser's input-error path. */
	if (bits->error != 0)
		return 0U;

	/* Succeeded: the profile's object type is available without interpretation. */
	return object;
}

/* Read a sampling frequency without indexing a reserved table entry. */
static int
aac_frequency(
	struct media_bits *bits,
	uint32_t *rate,
	uint8_t *index)
{
	uint32_t rate_index;
	uint32_t frequency;

	/* Fifteen carries an explicit frequency rather than a table index. */
	rate_index = media_bits_read(bits, 4U);
	frequency = 0U;
	if (rate_index == 15U) {
		frequency = media_bits_read(bits, 24U);
	} else if (rate_index < 13U) {
		frequency = aac_rates[rate_index];
	}

	/* Truncation and reserved indices have no valid sample rate. */
	if (bits->error != 0)
		return EINVAL;

	/* Zero and reserved indices do not represent a usable sampling frequency. */
	if (frequency == 0U)
		return EINVAL;

	/* Succeeded: preserve whether the rate was signalled explicitly. */
	*rate = frequency;
	*index = (uint8_t)rate_index;
	return 0;
}

/* Refuse backward-compatible extension tools instead of silently decoding their core. */
static int
aac_sync_extension(
	struct media_bits *bits)
{
	struct media_bits probe;
	size_t left;
	uint32_t sync;
	uint32_t object;
	uint32_t present;
	uint32_t padding;

	/* Look ahead without turning an ordinary short padding tail into a failed read. */
	left = media_bits_left(bits);
	if (left >= 11U) {
		probe = *bits;
		sync = media_bits_read(&probe, 11U);

		/* The SBR sync marker is followed by an object type and presence flag. */
		if (sync == 0x2b7U) {
			object = aac_object_type(&probe);
			present = media_bits_read1(&probe);
			if (probe.error != 0)
				return EINVAL;

			/* Other extension object types need their own decoder syntax. */
			if (object != 5U)
				return ENOTSUP;

			/* SBR presence is HE-AAC even when the outer audio object says LC. */
			if (present != 0U)
				return ENOTSUP;
			*bits = probe;
		}
	}

	/* Only zero fill follows LC; unknown nonzero extension syntax is unsupported. */
	left = media_bits_left(bits);
	while (left != 0U) {
		padding = media_bits_read1(bits);
		if (bits->error != 0)
			return EINVAL;

		/* PS or another extension can never become unnoticed LC padding. */
		if (padding != 0U)
			return ENOTSUP;
		left--;
	}

	/* Succeeded: no extension tool was enabled. */
	return 0;
}

/* Append one PCE element, preserving its class-specific tag and output group. */
static int
aac_pce_element(
	struct media_bits *bits,
	struct media_aac_config *config,
	unsigned group,
	unsigned type)
{
	uint32_t tag;
	unsigned index;
	unsigned channels;

	/* Each declared element carries a four-bit tag in its own type's namespace. */
	tag = media_bits_read(bits, 4U);
	if (bits->error != 0)
		return EINVAL;
	channels = 1U;
	if (type == 1U)
		channels = 2U;

	/* Larger programs are unsupported, without overflowing the fixed configuration. */
	if (config->channels + channels > MEDIA_AAC_MAX_CHANNELS)
		return ENOTSUP;

	/* All declared elements must fit the fixed identity list as well as its channel bound. */
	if (config->element_count == MEDIA_AAC_MAX_CHANNELS)
		return ENOTSUP;

	/* Reusing the same type and tag for two output elements is inconsistent. */
	for (index = 0U; index < config->element_count; index++) {
		/* A matching class and tag identifies the same channel element twice. */
		if (config->elements[index].type == type && config->elements[index].tag == tag)
			return EINVAL;
	}

	/* Publish the element and its contribution to the program's output channel count. */
	config->elements[config->element_count].type = (uint8_t)type;
	config->elements[config->element_count].tag = (uint8_t)tag;
	config->elements[config->element_count].group = (uint8_t)group;
	config->element_count++;
	config->channels = (uint8_t)(config->channels + channels);

	/* Succeeded: the complete element is part of the temporary configuration. */
	return 0;
}
