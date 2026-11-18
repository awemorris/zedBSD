/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SBC (media-sbc.h, ws205-p002), written from A2DP 1.4.1 appendix B.
 *
 * The filters are the specification's polyphase filterbanks written
 * directly from their impulse responses: subband m of the analysis is
 * hp(n) cos((m + 1/2)(n - M/2) pi / M) and of the synthesis -M hp(n)
 * cos((m + 1/2)(n + M/2) pi / M), n = 0 .. 10 M - 1, where hp is the
 * prototype of table B.8 (the table carries the sign of every other
 * 2 M coefficients, as MPEG-1's window does; hp is the table with those
 * signs undone; the synthesis's sign makes the decoded samples those of
 * the specification's figure 8.3, as other decoders give them).  The
 * encoder keeps the last 10 M samples of each channel, the decoder the
 * last 10 blocks of subband samples; every output is a sum over them (80
 * products a sample at 8 subbands).
 *
 * The bit allocation, the reconstruction, the joint stereo and the CRC
 * follow B.6 word for word.  The encoder chooses joint coding per subband
 * by B.7.3's criterion (the sum and difference's scale factors smaller
 * than the left and right's).
 */

#include "userland/desktop/libmedia/media-sbc.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The syncwords of an SBC frame and of an mSBC frame. */
#define SBC_SYNC		0x9cU
#define SBC_MSBC_SYNC		0xadU

/* The most channels, subbands and blocks, and the filters' length (10 M). */
#define SBC_CHANNELS_MAX	2U
#define SBC_SUBBANDS_MAX	8U
#define SBC_BLOCKS_MAX		16U
#define SBC_TAPS_MAX		80U
#define SBC_HISTORY		10U

/* The CRC-8's generator (x^8 + x^4 + x^3 + x^2 + 1) and its first state. */
#define SBC_CRC_POLYNOMIAL	0x1dU
#define SBC_CRC_START		0x0fU

/* The constant pi, which C11's <math.h> does not name. */
#define SBC_PI			3.14159265358979323846

/*
 * One stream's codec: whether it encodes, the settings (the encoder's
 * fixed, the decoder's those of the last frame), the subbands and
 * channels its filters and history are made for (0 before the first
 * frame), the filters (subband by tap), and the history: the encoder's
 * last 10 M samples of each channel (the newest first), the decoder's
 * last 10 blocks of subband samples (the newest first).
 */
struct media_sbc {
	int encoder;
	struct media_sbc_config config;
	unsigned made_subbands;
	unsigned made_channels;
	double analysis[SBC_SUBBANDS_MAX][SBC_TAPS_MAX];
	double synthesis[SBC_SUBBANDS_MAX][SBC_TAPS_MAX];
	double input[SBC_CHANNELS_MAX][SBC_TAPS_MAX];
	double blocks[SBC_CHANNELS_MAX][SBC_HISTORY][SBC_SUBBANDS_MAX];
};

/* A frame's fields between the header and the samples. */
struct sbc_frame {
	unsigned join[SBC_SUBBANDS_MAX];
	unsigned scale[SBC_CHANNELS_MAX][SBC_SUBBANDS_MAX];
	unsigned bits[SBC_CHANNELS_MAX][SBC_SUBBANDS_MAX];
	double samples[SBC_BLOCKS_MAX][SBC_CHANNELS_MAX][SBC_SUBBANDS_MAX];
};

/* Bits written into a frame (MSB first), the room counted in bits. */
struct sbc_writer {
	uint8_t *data;
	size_t room;
	size_t position;
};

/* Bits read from a frame (MSB first). */
struct sbc_reader {
	const uint8_t *data;
	size_t size;
	size_t position;
};

static void sbc_filters(struct media_sbc *codec, unsigned subbands);
static void sbc_allocate(const struct media_sbc_config *config, const struct sbc_frame *frame, unsigned bits[SBC_CHANNELS_MAX][SBC_SUBBANDS_MAX]);
static void sbc_allocate_need(const struct media_sbc_config *config, unsigned scale, unsigned subband, int *need);
static unsigned sbc_crc(const struct media_sbc_config *config, const uint8_t *frame, const struct sbc_frame *fields);
static unsigned sbc_crc_add(unsigned crc, uint32_t value, unsigned count);
static unsigned sbc_scale(double largest);
static void sbc_analyse(struct media_sbc *codec, const int16_t *pcm, struct sbc_frame *frame);
static void sbc_join(const struct media_sbc_config *config, struct sbc_frame *frame);
static void sbc_synthesise(struct media_sbc *codec, unsigned channel, const double *samples, int16_t *pcm, unsigned stride);
static int sbc_header(const uint8_t *frame, struct media_sbc_config *config);
static void sbc_put(struct sbc_writer *writer, uint32_t value, unsigned count);
static uint32_t sbc_get(struct sbc_reader *reader, unsigned count);

/* The prototype of four subbands (table B.8, row-wise, with its signs). */
static const double sbc_proto4[40] = {
	0.00000000E+00, 5.36548976E-04, 1.49188357E-03, 2.73370904E-03,
	3.83720193E-03, 3.89205149E-03, 1.86581691E-03, -3.06012286E-03,
	1.09137620E-02, 2.04385087E-02, 2.88757392E-02, 3.21939290E-02,
	2.58767811E-02, 6.13245186E-03, -2.88217274E-02, -7.76463494E-02,
	1.35593274E-01, 1.94987841E-01, 2.46636662E-01, 2.81828203E-01,
	2.94315332E-01, 2.81828203E-01, 2.46636662E-01, 1.94987841E-01,
	-1.35593274E-01, -7.76463494E-02, -2.88217274E-02, 6.13245186E-03,
	2.58767811E-02, 3.21939290E-02, 2.88757392E-02, 2.04385087E-02,
	-1.09137620E-02, -3.06012286E-03, 1.86581691E-03, 3.89205149E-03,
	3.83720193E-03, 2.73370904E-03, 1.49188357E-03, 5.36548976E-04
};

/* The prototype of eight subbands (table B.8, row-wise, with its signs). */
static const double sbc_proto8[80] = {
	0.00000000E+00, 1.56575398E-04, 3.43256425E-04, 5.54620202E-04,
	8.23919506E-04, 1.13992507E-03, 1.47640169E-03, 1.78371725E-03,
	2.01182542E-03, 2.10371989E-03, 1.99454554E-03, 1.61656283E-03,
	9.02154502E-04, -1.78805361E-04, -1.64973098E-03, -3.49717454E-03,
	5.65949473E-03, 8.02941163E-03, 1.04584443E-02, 1.27472335E-02,
	1.46525263E-02, 1.59045603E-02, 1.62208471E-02, 1.53184106E-02,
	1.29371806E-02, 8.85757540E-03, 2.92408442E-03, -4.91578024E-03,
	-1.46404076E-02, -2.61098752E-02, -3.90751381E-02, -5.31873032E-02,
	6.79989431E-02, 8.29847578E-02, 9.75753918E-02, 1.11196689E-01,
	1.23264548E-01, 1.33264415E-01, 1.40753505E-01, 1.45389847E-01,
	1.46955068E-01, 1.45389847E-01, 1.40753505E-01, 1.33264415E-01,
	1.23264548E-01, 1.11196689E-01, 9.75753918E-02, 8.29847578E-02,
	-6.79989431E-02, -5.31873032E-02, -3.90751381E-02, -2.61098752E-02,
	-1.46404076E-02, -4.91578024E-03, 2.92408442E-03, 8.85757540E-03,
	1.29371806E-02, 1.53184106E-02, 1.62208471E-02, 1.59045603E-02,
	1.46525263E-02, 1.27472335E-02, 1.04584443E-02, 8.02941163E-03,
	-5.65949473E-03, -3.49717454E-03, -1.64973098E-03, -1.78805361E-04,
	9.02154502E-04, 1.61656283E-03, 1.99454554E-03, 2.10371989E-03,
	2.01182542E-03, 1.78371725E-03, 1.47640169E-03, 1.13992507E-03,
	8.23919506E-04, 5.54620202E-04, 3.43256425E-04, 1.56575398E-04
};

/* The loudness offsets of four subbands, by the rate's field (table B.8). */
static const int sbc_offset4[4][4] = {
	{-1, 0, 0, 0},
	{-2, 0, 0, 1},
	{-2, 0, 0, 1},
	{-2, 0, 0, 1}
};

/* The loudness offsets of eight subbands, by the rate's field (table B.8). */
static const int sbc_offset8[4][8] = {
	{-2, 0, 0, 0, 0, 0, 0, 1},
	{-3, 0, 0, 0, 0, 0, 1, 2},
	{-4, 0, 0, 0, 0, 0, 1, 2},
	{-4, 0, 0, 0, 0, 0, 1, 2}
};

/* The rates by the frame's two bits. */
static const unsigned sbc_rates[4] = {16000U, 32000U, 44100U, 48000U};

/*
 * Fills a configuration with mSBC's fixed settings.
 */
void
media_sbc_msbc_config(
	struct media_sbc_config *config)
{
	/* 16 kHz, 15 blocks, mono, loudness, 8 subbands, bitpool 26. */
	memset(config, 0, sizeof(*config));
	config->rate = 16000U;
	config->blocks = 15U;
	config->channel_mode = MEDIA_SBC_MONO;
	config->allocation = MEDIA_SBC_LOUDNESS;
	config->subbands = 8U;
	config->bitpool = 26U;
	config->msbc = 1U;
}

/*
 * Checks a configuration.  Returns 0, or EINVAL for settings SBC does not
 * have (or mSBC's not its fixed ones).
 */
int
media_sbc_check(
	const struct media_sbc_config *config)
{
	unsigned channels;
	unsigned largest;

	/* mSBC: its fixed settings only. */
	if (config->msbc) {
		if (config->rate != 16000U || config->blocks != 15U)
			return EINVAL;
		if (config->channel_mode != MEDIA_SBC_MONO || config->subbands != 8U)
			return EINVAL;
		if (config->allocation != MEDIA_SBC_LOUDNESS || config->bitpool != 26U)
			return EINVAL;
		return 0;
	}

	/* A rate, blocks, subbands, a mode and a method of the frame's fields. */
	if (config->rate != 16000U &&
	    config->rate != 32000U &&
	    config->rate != 44100U &&
	    config->rate != 48000U)
		return EINVAL;
	if (config->blocks != 4U &&
	    config->blocks != 8U &&
	    config->blocks != 12U &&
	    config->blocks != 16U)
		return EINVAL;
	if (config->subbands != 4U && config->subbands != 8U)
		return EINVAL;
	if (config->channel_mode > MEDIA_SBC_JOINT || config->allocation > MEDIA_SBC_SNR)
		return EINVAL;

	/* The bitpool: 2 at least, at most 16 (one channel's) or 32 (two together) per subband, and the field's 255. */
	channels = media_sbc_channels(config);
	largest = 16U * config->subbands;
	if (channels == 2U && config->channel_mode >= MEDIA_SBC_STEREO)
		largest = 32U * config->subbands;
	if (largest > 250U)
		largest = 250U;
	if (config->bitpool < 2U || config->bitpool > largest)
		return EINVAL;

	/* Succeeded: settings of SBC. */
	return 0;
}

/*
 * Tells the bytes of a frame of these settings (B.9, rounded up to whole
 * bytes as the padding does).
 */
size_t
media_sbc_frame_bytes(
	const struct media_sbc_config *config)
{
	size_t bits;
	unsigned channels;

	/* The header, the join flags, the scale factors. */
	channels = media_sbc_channels(config);
	bits = 32U + 4U * (size_t)config->subbands * channels;
	if (config->channel_mode == MEDIA_SBC_JOINT)
		bits += config->subbands;

	/* The samples: a bitpool a block, of each channel apart or of both together. */
	if (config->channel_mode >= MEDIA_SBC_STEREO) {
		bits += (size_t)config->blocks * config->bitpool;
	} else {
		bits += (size_t)config->blocks * channels * config->bitpool;
	}

	/* Succeeded: whole bytes. */
	return (bits + 7U) / 8U;
}

/*
 * Tells a configuration's channels: 1 for mono, 2 for the others.
 */
unsigned
media_sbc_channels(
	const struct media_sbc_config *config)
{
	/* Mono has one. */
	if (config->channel_mode == MEDIA_SBC_MONO)
		return 1U;

	/* Succeeded: two. */
	return 2U;
}

/*
 * Makes an encoder of fixed settings.  Returns 0, EINVAL for settings
 * SBC does not have, or ENOMEM.
 */
int
media_sbc_encoder_create(
	const struct media_sbc_config *config,
	struct media_sbc **codec)
{
	struct media_sbc *made;
	int error;

	/* Settings of SBC. */
	*codec = NULL;
	error = media_sbc_check(config);
	if (error != 0)
		return error;

	/* The codec, its history silent. */
	made = calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->encoder = 1;
	made->config = *config;
	made->made_channels = media_sbc_channels(config);
	sbc_filters(made, config->subbands);

	/* Succeeded: the encoder. */
	*codec = made;
	return 0;
}

/*
 * Encodes one frame: blocks x subbands samples of each channel,
 * interleaved, into frame (size bytes, *length set to the frame's).
 * Returns 0, EINVAL for a decoder, or ENOSPC when the frame does not fit.
 */
int
media_sbc_encode(
	struct media_sbc *codec,
	const int16_t *pcm,
	uint8_t *frame,
	size_t size,
	size_t *length)
{
	struct sbc_frame *fields;
	struct sbc_writer writer;
	const struct media_sbc_config *config;
	double value;
	double levels;
	unsigned channels;
	unsigned channel;
	unsigned subband;
	unsigned block;
	unsigned header;
	long quantized;
	size_t bytes;

	/* An encoder, and room for its frame. */
	*length = 0U;
	if (!codec->encoder)
		return EINVAL;
	config = &codec->config;
	bytes = media_sbc_frame_bytes(config);
	if (size < bytes)
		return ENOSPC;

	/* The fields, too large for the stack of a small thread. */
	fields = calloc(1U, sizeof(*fields));
	if (fields == NULL)
		return ENOMEM;

	/* The subband samples, their scale factors, the joint coding and the bits of each subband. */
	sbc_analyse(codec, pcm, fields);
	sbc_join(config, fields);
	sbc_allocate(config, fields, fields->bits);

	/* The header: the syncword, the settings (mSBC's two reserved bytes) and the CRC's place. */
	memset(frame, 0, bytes);
	writer.data = frame;
	writer.room = bytes * 8U;
	writer.position = 0U;
	if (config->msbc) {
		sbc_put(&writer, SBC_MSBC_SYNC, 8U);
		sbc_put(&writer, 0U, 16U);
	} else {
		header = 0U;
		while (sbc_rates[header] != config->rate)
			header++;
		header = header << 6;
		header |= ((config->blocks / 4U) - 1U) << 4;
		header |= config->channel_mode << 2;
		header |= config->allocation << 1;
		header |= config->subbands == 8U;
		sbc_put(&writer, SBC_SYNC, 8U);
		sbc_put(&writer, header, 8U);
		sbc_put(&writer, config->bitpool, 8U);
	}

	/* The CRC's byte, written when the fields it covers are known. */
	sbc_put(&writer, 0U, 8U);

	/* The join flags (the last always 0, the RFU bit). */
	channels = media_sbc_channels(config);
	if (config->channel_mode == MEDIA_SBC_JOINT) {
		for (subband = 0U; subband < config->subbands; subband++)
			sbc_put(&writer, fields->join[subband], 1U);
	}

	/* The scale factors. */
	for (channel = 0U; channel < channels; channel++) {
		for (subband = 0U; subband < config->subbands; subband++)
			sbc_put(&writer, fields->scale[channel][subband], 4U);
	}

	/* The samples, each scaled to -1 .. 1 and quantized to its levels (B.7.5). */
	for (block = 0U; block < config->blocks; block++) {
		for (channel = 0U; channel < channels; channel++) {
			for (subband = 0U; subband < config->subbands; subband++) {
				/* A subband without bits carries nothing. */
				if (fields->bits[channel][subband] == 0U)
					continue;

				/* The level, kept within the quantizer's range. */
				levels = (double)((1U << fields->bits[channel][subband]) - 1U);
				value = fields->samples[block][channel][subband] / ldexp(1.0, (int)fields->scale[channel][subband] + 1);
				quantized = (long)floor((value + 1.0) * levels / 2.0);
				if (quantized < 0L)
					quantized = 0L;
				if (quantized > (long)levels - 1L)
					quantized = (long)levels - 1L;
				sbc_put(&writer, (uint32_t)quantized, fields->bits[channel][subband]);
			}
		}
	}

	/* The CRC over the settings, the join flags and the scale factors. */
	frame[3] = (uint8_t)sbc_crc(config, frame, fields);
	free(fields);

	/* Succeeded: one frame, padded with zero bits. */
	*length = bytes;
	return 0;
}

/*
 * Makes a decoder; it takes each frame's settings from the frame.
 * Returns 0 or ENOMEM.
 */
int
media_sbc_decoder_create(
	struct media_sbc **codec)
{
	struct media_sbc *made;

	/* The codec, without filters until its first frame. */
	*codec = NULL;
	made = calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Succeeded: the decoder. */
	*codec = made;
	return 0;
}

/*
 * Decodes the frame at the start of frame (size bytes): *used is the
 * frame's bytes, the samples go to pcm (capacity values, interleaved;
 * *count set to the samples of one channel) and its settings to *config.
 * Returns 0; EAGAIN when size holds less than the frame (nothing used);
 * EBADMSG when it is not a frame (*used 1, to look for the next
 * syncword) or its CRC is wrong (*used the frame, its samples not given:
 * the caller conceals); ENOSPC when pcm is too small; EINVAL for an
 * encoder.
 */
int
media_sbc_decode(
	struct media_sbc *codec,
	const uint8_t *frame,
	size_t size,
	size_t *used,
	int16_t *pcm,
	size_t capacity,
	size_t *count,
	struct media_sbc_config *config)
{
	struct media_sbc_config found;
	struct sbc_frame *fields;
	struct sbc_reader reader;
	double scale;
	double levels;
	double left;
	double right;
	unsigned channels;
	unsigned channel;
	unsigned subband;
	unsigned block;
	unsigned crc;
	uint32_t quantized;
	size_t bytes;
	int error;

	/* Nothing yet. */
	*used = 0U;
	*count = 0U;
	if (codec->encoder)
		return EINVAL;

	/* The header, whole. */
	if (size < 4U)
		return EAGAIN;
	error = sbc_header(frame, &found);
	if (error != 0) {
		*used = 1U;
		return EBADMSG;
	}

	/* The frame, whole, and room for its samples. */
	bytes = media_sbc_frame_bytes(&found);
	if (size < bytes)
		return EAGAIN;
	channels = media_sbc_channels(&found);
	if (capacity < (size_t)found.blocks * found.subbands * channels)
		return ENOSPC;

	/* The fields, too large for the stack of a small thread. */
	fields = calloc(1U, sizeof(*fields));
	if (fields == NULL)
		return ENOMEM;

	/* The join flags. */
	reader.data = frame;
	reader.size = bytes;
	reader.position = 32U;
	if (found.channel_mode == MEDIA_SBC_JOINT) {
		for (subband = 0U; subband < found.subbands; subband++)
			fields->join[subband] = sbc_get(&reader, 1U);
	}

	/* The scale factors. */
	for (channel = 0U; channel < channels; channel++) {
		for (subband = 0U; subband < found.subbands; subband++)
			fields->scale[channel][subband] = sbc_get(&reader, 4U);
	}

	/* A frame whose CRC is wrong is skipped whole. */
	crc = sbc_crc(&found, frame, fields);
	if (crc != frame[3]) {
		free(fields);
		*used = bytes;
		return EBADMSG;
	}

	/* The bits of each subband, and the samples reconstructed (B.6.4). */
	sbc_allocate(&found, fields, fields->bits);
	for (block = 0U; block < found.blocks; block++) {
		for (channel = 0U; channel < channels; channel++) {
			for (subband = 0U; subband < found.subbands; subband++) {
				/* A subband without bits is silent. */
				if (fields->bits[channel][subband] == 0U)
					continue;

				/* The sample at the middle of its step. */
				quantized = sbc_get(&reader, fields->bits[channel][subband]);
				levels = (double)((1U << fields->bits[channel][subband]) - 1U);
				scale = ldexp(1.0, (int)fields->scale[channel][subband] + 1);
				fields->samples[block][channel][subband] = scale * (((double)quantized * 2.0 + 1.0) / levels - 1.0);
			}
		}

		/* The joint subbands back to left and right (B.6.5). */
		for (subband = 0U; subband < found.subbands; subband++) {
			if (found.channel_mode != MEDIA_SBC_JOINT || !fields->join[subband])
				continue;
			left = fields->samples[block][0][subband] + fields->samples[block][1][subband];
			right = fields->samples[block][0][subband] - fields->samples[block][1][subband];
			fields->samples[block][0][subband] = left;
			fields->samples[block][1][subband] = right;
		}
	}

	/* New subbands or channels start the filters over. */
	if (codec->made_subbands != found.subbands || codec->made_channels != channels) {
		sbc_filters(codec, found.subbands);
		codec->made_channels = channels;
		memset(codec->blocks, 0, sizeof(codec->blocks));
	}

	/* The samples of each block and channel, through the synthesis. */
	for (block = 0U; block < found.blocks; block++) {
		for (channel = 0U; channel < channels; channel++) {
			sbc_synthesise(codec, channel, fields->samples[block][channel],
			    pcm + ((size_t)block * found.subbands * channels) + channel, channels);
		}
	}

	/* The fields are no longer needed. */
	free(fields);

	/* Succeeded: one frame. */
	codec->config = found;
	*config = found;
	*used = bytes;
	*count = (size_t)found.blocks * found.subbands;
	return 0;
}

/*
 * Frees a codec.
 */
void
media_sbc_destroy(
	struct media_sbc *codec)
{
	/* The memory, which holds nothing else. */
	free(codec);
}

/* Makes the analysis and synthesis filters of a number of subbands. */
static void
sbc_filters(
	struct media_sbc *codec,
	unsigned subbands)
{
	const double *proto;
	double prototype;
	double half;
	unsigned taps;
	unsigned tap;
	unsigned subband;

	/* The prototype of the subbands. */
	proto = sbc_proto8;
	if (subbands == 4U)
		proto = sbc_proto4;
	taps = 10U * subbands;
	half = (double)subbands / 2.0;

	/* Each tap: the table's sign undone in every other 2 M taps, then each subband's cosine. */
	for (tap = 0U; tap < taps; tap++) {
		prototype = proto[tap];
		if (((tap / (2U * subbands)) & 1U) != 0U)
			prototype = -prototype;
		for (subband = 0U; subband < subbands; subband++) {
			codec->analysis[subband][tap] = prototype *
			    cos(((double)subband + 0.5) * ((double)tap - half) * SBC_PI / (double)subbands);
			codec->synthesis[subband][tap] = -(double)subbands * prototype *
			    cos(((double)subband + 0.5) * ((double)tap + half) * SBC_PI / (double)subbands);
		}
	}

	/* Made for these subbands. */
	codec->made_subbands = subbands;
}

/* Computes each subband's bits from the scale factors (B.6.3). */
static void
sbc_allocate(
	const struct media_sbc_config *config,
	const struct sbc_frame *frame,
	unsigned bits[SBC_CHANNELS_MAX][SBC_SUBBANDS_MAX])
{
	int need[SBC_CHANNELS_MAX][SBC_SUBBANDS_MAX];
	unsigned channels;
	unsigned groups;
	unsigned group;
	unsigned width;
	unsigned first;
	unsigned channel;
	unsigned subband;
	int most;
	int slice;
	int bitcount;
	int slicecount;
	int pool;

	/*
	 * Two channels of stereo and joint stereo share one pool (one group
	 * of both channels); mono and dual channel allocate each channel
	 * apart (a group of one).
	 */
	channels = media_sbc_channels(config);
	groups = channels;
	width = 1U;
	if (config->channel_mode >= MEDIA_SBC_STEREO) {
		groups = 1U;
		width = 2U;
	}

	/* The pool of each group. */
	pool = (int)config->bitpool;

	/* Each group. */
	for (group = 0U; group < groups; group++) {
		first = group;

		/* The bitneed of each subband, and the largest. */
		most = 0;
		for (channel = first; channel < first + width; channel++) {
			for (subband = 0U; subband < config->subbands; subband++) {
				sbc_allocate_need(config, frame->scale[channel][subband], subband, &need[channel][subband]);
				if (need[channel][subband] > most)
					most = need[channel][subband];
			}
		}

		/* How many bitslices fit into the bitpool. */
		bitcount = 0;
		slicecount = 0;
		slice = most + 1;
		do {
			slice--;
			bitcount += slicecount;
			slicecount = 0;
			for (channel = first; channel < first + width; channel++) {
				for (subband = 0U; subband < config->subbands; subband++) {
					if (need[channel][subband] > slice + 1 && need[channel][subband] < slice + 16) {
						slicecount++;
					} else if (need[channel][subband] == slice + 1) {
						slicecount += 2;
					}
				}
			}
		} while (bitcount + slicecount < pool);

		/* An exact fit takes the last slice too. */
		if (bitcount + slicecount == pool) {
			bitcount += slicecount;
			slice--;
		}

		/* The bits down to the last slice. */
		for (channel = first; channel < first + width; channel++) {
			for (subband = 0U; subband < config->subbands; subband++) {
				bits[channel][subband] = 0U;
				if (need[channel][subband] >= slice + 2) {
					bits[channel][subband] = (unsigned)(need[channel][subband] - slice);
					if (bits[channel][subband] > 16U)
						bits[channel][subband] = 16U;
				}
			}
		}

		/* The bits left, from subband 0 (of both channels in turn when they share the pool). */
		channel = first;
		subband = 0U;
		while (bitcount < pool && subband < config->subbands) {
			if (bits[channel][subband] >= 2U && bits[channel][subband] < 16U) {
				bits[channel][subband]++;
				bitcount++;
			} else if (need[channel][subband] == slice + 1 && pool > bitcount + 1) {
				bits[channel][subband] = 2U;
				bitcount += 2;
			}

			/* The next: the other channel of the subband, or the next subband. */
			if (width == 2U && channel == first) {
				channel = first + 1U;
			} else {
				channel = first;
				subband++;
			}
		}

		/* Then one more each while bits are left. */
		channel = first;
		subband = 0U;
		while (bitcount < pool && subband < config->subbands) {
			if (bits[channel][subband] < 16U) {
				bits[channel][subband]++;
				bitcount++;
			}

			/* The next, as above. */
			if (width == 2U && channel == first) {
				channel = first + 1U;
			} else {
				channel = first;
				subband++;
			}
		}
	}
}

/* Computes one subband's bitneed from its scale factor (B.6.3). */
static void
sbc_allocate_need(
	const struct media_sbc_config *config,
	unsigned scale,
	unsigned subband,
	int *need)
{
	unsigned field;
	int loudness;

	/* SNR: the scale factor itself. */
	if (config->allocation == MEDIA_SBC_SNR) {
		*need = (int)scale;
		return;
	}

	/* Loudness: a silent subband needs nothing. */
	if (scale == 0U) {
		*need = -5;
		return;
	}

	/* The scale factor less the rate's offset for the subband. */
	field = 0U;
	while (sbc_rates[field] != config->rate)
		field++;
	if (config->subbands == 4U) {
		loudness = (int)scale - sbc_offset4[field][subband];
	} else {
		loudness = (int)scale - sbc_offset8[field][subband];
	}

	/* Half of a positive loudness. */
	*need = loudness;
	if (loudness > 0)
		*need = loudness / 2;
}

/*
 * Computes a frame's CRC (B.6.1.1): the header's two bytes after the
 * syncword (mSBC's reserved bytes as they are), the join flags and the
 * scale factors.
 */
static unsigned
sbc_crc(
	const struct media_sbc_config *config,
	const uint8_t *frame,
	const struct sbc_frame *fields)
{
	unsigned channels;
	unsigned channel;
	unsigned subband;
	unsigned crc;

	/* The header's settings. */
	crc = SBC_CRC_START;
	crc = sbc_crc_add(crc, frame[1], 8U);
	crc = sbc_crc_add(crc, frame[2], 8U);

	/* The join flags. */
	if (config->channel_mode == MEDIA_SBC_JOINT) {
		for (subband = 0U; subband < config->subbands; subband++)
			crc = sbc_crc_add(crc, fields->join[subband], 1U);
	}

	/* The scale factors. */
	channels = media_sbc_channels(config);
	for (channel = 0U; channel < channels; channel++) {
		for (subband = 0U; subband < config->subbands; subband++)
			crc = sbc_crc_add(crc, fields->scale[channel][subband], 4U);
	}

	/* Succeeded: the CRC. */
	return crc;
}

/* Shifts count bits of value (the highest first) into the CRC-8. */
static unsigned
sbc_crc_add(
	unsigned crc,
	uint32_t value,
	unsigned count)
{
	unsigned bit;
	unsigned feedback;

	/* Each bit. */
	while (count != 0U) {
		count--;
		bit = (unsigned)((value >> count) & 1U);
		feedback = ((crc >> 7) ^ bit) & 1U;
		crc = (crc << 1) & 0xffU;
		if (feedback != 0U)
			crc ^= SBC_CRC_POLYNOMIAL;
	}

	/* Succeeded: the new state. */
	return crc;
}

/* Finds the scale factor of a subband's largest magnitude: the least x with largest < 2^(x+1). */
static unsigned
sbc_scale(
	double largest)
{
	unsigned scale;
	double limit;

	/* Up from 0 while the largest reaches 2^(x+1), at most 15. */
	scale = 0U;
	limit = 2.0;
	while (scale < 15U && largest >= limit) {
		scale++;
		limit *= 2.0;
	}

	/* Succeeded: the scale factor. */
	return scale;
}

/* Splits one frame's samples into subbands, and finds each subband's scale factor. */
static void
sbc_analyse(
	struct media_sbc *codec,
	const int16_t *pcm,
	struct sbc_frame *frame)
{
	const struct media_sbc_config *config;
	double largest[SBC_CHANNELS_MAX][SBC_SUBBANDS_MAX];
	double sum;
	double magnitude;
	unsigned channels;
	unsigned channel;
	unsigned subband;
	unsigned block;
	unsigned taps;
	unsigned tap;
	unsigned index;

	/* Nothing found yet. */
	config = &codec->config;
	channels = media_sbc_channels(config);
	taps = 10U * config->subbands;
	memset(largest, 0, sizeof(largest));

	/* Each block of each channel: M new samples in (the newest first), then each subband's sum. */
	for (block = 0U; block < config->blocks; block++) {
		for (channel = 0U; channel < channels; channel++) {
			memmove(&codec->input[channel][config->subbands], &codec->input[channel][0], (taps - config->subbands) * sizeof(double));
			for (index = 0U; index < config->subbands; index++) {
				codec->input[channel][config->subbands - 1U - index] =
				    (double)pcm[((size_t)block * config->subbands + index) * channels + channel];
			}

			/* Each subband. */
			for (subband = 0U; subband < config->subbands; subband++) {
				sum = 0.0;
				for (tap = 0U; tap < taps; tap++)
					sum += codec->analysis[subband][tap] * codec->input[channel][tap];
				frame->samples[block][channel][subband] = sum;
				magnitude = fabs(sum);
				if (magnitude > largest[channel][subband])
					largest[channel][subband] = magnitude;
			}
		}
	}

	/* The scale factors. */
	for (channel = 0U; channel < channels; channel++) {
		for (subband = 0U; subband < config->subbands; subband++)
			frame->scale[channel][subband] = sbc_scale(largest[channel][subband]);
	}
}

/*
 * Chooses joint coding for each subband but the last where the sum and
 * difference's scale factors are smaller than the left and right's
 * (B.7.3), and replaces those subbands' samples and scale factors.
 */
static void
sbc_join(
	const struct media_sbc_config *config,
	struct sbc_frame *frame)
{
	double largest_sum;
	double largest_difference;
	double sum;
	double difference;
	unsigned subband;
	unsigned block;
	unsigned scale_sum;
	unsigned scale_difference;

	/* Joint stereo only. */
	if (config->channel_mode != MEDIA_SBC_JOINT)
		return;

	/* Each subband but the last. */
	for (subband = 0U; subband + 1U < config->subbands; subband++) {
		/* The sum's and the difference's largest. */
		largest_sum = 0.0;
		largest_difference = 0.0;
		for (block = 0U; block < config->blocks; block++) {
			sum = fabs((frame->samples[block][0][subband] + frame->samples[block][1][subband]) / 2.0);
			difference = fabs((frame->samples[block][0][subband] - frame->samples[block][1][subband]) / 2.0);
			if (sum > largest_sum)
				largest_sum = sum;
			if (difference > largest_difference)
				largest_difference = difference;
		}

		/* Left and right stay when they cost no more. */
		scale_sum = sbc_scale(largest_sum);
		scale_difference = sbc_scale(largest_difference);
		if (frame->scale[0][subband] + frame->scale[1][subband] <= scale_sum + scale_difference)
			continue;

		/* The sum and the difference take their place. */
		frame->join[subband] = 1U;
		frame->scale[0][subband] = scale_sum;
		frame->scale[1][subband] = scale_difference;
		for (block = 0U; block < config->blocks; block++) {
			sum = (frame->samples[block][0][subband] + frame->samples[block][1][subband]) / 2.0;
			difference = (frame->samples[block][0][subband] - frame->samples[block][1][subband]) / 2.0;
			frame->samples[block][0][subband] = sum;
			frame->samples[block][1][subband] = difference;
		}
	}
}

/*
 * Makes one block's M samples of a channel from its subband samples: the
 * block joins the history (the newest first), and each output is the sum
 * over the last 10 blocks.  The samples go to pcm, stride values apart.
 */
static void
sbc_synthesise(
	struct media_sbc *codec,
	unsigned channel,
	const double *samples,
	int16_t *pcm,
	unsigned stride)
{
	unsigned subbands;
	unsigned subband;
	unsigned output;
	unsigned age;
	double sum;
	long rounded;

	/* The block into the history, the oldest leaving it. */
	subbands = codec->made_subbands;
	for (age = SBC_HISTORY - 1U; age > 0U; age--)
		memcpy(codec->blocks[channel][age], codec->blocks[channel][age - 1U], sizeof(codec->blocks[channel][age]));
	memcpy(codec->blocks[channel][0], samples, subbands * sizeof(double));

	/* Each output of the block: its sum over the history. */
	for (output = 0U; output < subbands; output++) {
		sum = 0.0;
		for (age = 0U; age < SBC_HISTORY; age++) {
			for (subband = 0U; subband < subbands; subband++)
				sum += codec->synthesis[subband][age * subbands + output] * codec->blocks[channel][age][subband];
		}

		/* Rounded, within 16 bits. */
		rounded = lrint(sum);
		if (rounded > 32767L)
			rounded = 32767L;
		if (rounded < -32768L)
			rounded = -32768L;
		pcm[(size_t)output * stride] = (int16_t)rounded;
	}
}

/*
 * Reads a frame's header into a configuration.  Returns 0, or EINVAL
 * for a byte that is no syncword or settings SBC does not have.
 */
static int
sbc_header(
	const uint8_t *frame,
	struct media_sbc_config *config)
{
	int error;

	/* mSBC: its fixed settings. */
	if (frame[0] == SBC_MSBC_SYNC) {
		media_sbc_msbc_config(config);
		return 0;
	}

	/* No other syncword. */
	if (frame[0] != SBC_SYNC)
		return EINVAL;

	/* The settings. */
	memset(config, 0, sizeof(*config));
	config->rate = sbc_rates[frame[1] >> 6];
	config->blocks = (((unsigned)frame[1] >> 4) & 3U) * 4U + 4U;
	config->channel_mode = ((unsigned)frame[1] >> 2) & 3U;
	config->allocation = ((unsigned)frame[1] >> 1) & 1U;
	config->subbands = 4U;
	if ((frame[1] & 1U) != 0U)
		config->subbands = 8U;
	config->bitpool = frame[2];

	/* Settings SBC has. */
	error = media_sbc_check(config);
	if (error != 0)
		return error;

	/* Succeeded: the frame's settings. */
	return 0;
}

/* Writes count bits of value (the highest first); bits past the room are not written. */
static void
sbc_put(
	struct sbc_writer *writer,
	uint32_t value,
	unsigned count)
{
	unsigned bit;

	/* Each bit. */
	while (count != 0U) {
		count--;
		bit = (unsigned)((value >> count) & 1U);
		if (writer->position < writer->room && bit != 0U)
			writer->data[writer->position / 8U] |= (uint8_t)(0x80U >> (writer->position % 8U));
		writer->position++;
	}
}

/* Reads count bits (the highest first); bits past the end read as 0. */
static uint32_t
sbc_get(
	struct sbc_reader *reader,
	unsigned count)
{
	uint32_t value;
	unsigned bit;

	/* Each bit. */
	value = 0U;
	while (count != 0U) {
		count--;
		bit = 0U;
		if (reader->position < reader->size * 8U)
			bit = (reader->data[reader->position / 8U] >> (7U - reader->position % 8U)) & 1U;
		value = (value << 1) | bit;
		reader->position++;
	}

	/* Succeeded: the bits. */
	return value;
}
