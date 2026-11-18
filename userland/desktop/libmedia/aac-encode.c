/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AAC-LC encoder (media-aac-encoder.h, ws205-p003).
 *
 * Each frame: the last 2048 samples of each channel through the sine
 * window and the MDCT (folded into a DCT-IV of 1024, computed with a
 * complex FFT of 512); the energy of each scalefactor band below the
 * bit rate's bandwidth; mid/side for a band of a pair where the side is
 * small next to the left and right; then the rate loop: a noise offset
 * searched so that the frame's bits fit the budget (the bit rate and a
 * small reservoir), each band's scalefactor the coarsest whose
 * quantization noise stays under the band's allowed noise.  The allowed
 * noise is one level for every coefficient (the noise spread evenly, as
 * reverse water-filling does: the most signal-to-noise for the bits; a
 * band under the level is not sent).  The bands are
 * coded with the cheapest Huffman book each (a section is a run of one
 * book), the scalefactors as differences with book 0.
 *
 * Quantization and reconstruction are AAC's: q = int((|x| / g)^(3/4) +
 * 0.4054) and |x| = q^(4/3) g, g = 2^((sf - 100) / 4).  The powers come
 * from tables made once (|x|^(3/4) per coefficient, g^(-3/4) per
 * scalefactor, q^(4/3) per value).
 */

#include "userland/desktop/libmedia/media-aac-encoder.h"
#include "userland/desktop/libmedia/aac-bands.h"
#include "userland/desktop/libmedia/aac-huffman.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The coefficients of a long window, the MDCT's input, and the FFT's points. */
#define AAC_COEFFICIENTS	1024U
#define AAC_WINDOW		2048U
#define AAC_FFT			512U

/* The most channels, bands and scalefactor (offset 100), and the largest quantized value. */
#define AAC_CHANNELS_MAX	2U
#define AAC_BANDS_MAX		64U
#define AAC_SCALE_MAX		255
#define AAC_SCALE_OFFSET	100
#define AAC_QUANT_MAX		8191

/* The Huffman books: zero (a silent band), the escape book, and the most symbols of a book. */
#define AAC_BOOK_ZERO		0U
#define AAC_BOOK_ESCAPE		11U
#define AAC_BOOK_SYMBOLS	289U

/* The syntax's element ids and the most bits of one channel's frame. */
#define AAC_ID_SCE		0U
#define AAC_ID_CPE		1U
#define AAC_ID_END		7U
#define AAC_CHANNEL_BITS	6144L

/*
 * The rounding of AAC's quantizer, and the rate loop's tries and its
 * range of the noise level of one coefficient (dB over 1, the input's
 * 16-bit scale).
 */
#define AAC_ROUNDING		0.4054
#define AAC_LOOP_TRIES		16U
#define AAC_OFFSET_LOW		(-20.0)
#define AAC_OFFSET_HIGH		160.0

/* The constant pi. */
#define AAC_PI			3.14159265358979323846

/* One Huffman codeword. */
struct aac_code {
	uint32_t word;
	uint8_t length;
};

/*
 * A frame's channel as the rate loop makes it: the spectrum (mid/side
 * applied), its |x|^(3/4), each band's allowed noise at offset 0, the
 * chosen scalefactors and books, and the quantized values.
 */
struct aac_channel {
	double spectrum[AAC_COEFFICIENTS];
	double power[AAC_COEFFICIENTS];
	double allowed[AAC_BANDS_MAX];
	int scale[AAC_BANDS_MAX];
	unsigned book[AAC_BANDS_MAX];
	int quant[AAC_COEFFICIENTS];
};

/*
 * One stream's encoder: its rate, channels and bit rate, the band table
 * and the bands coded (below the bandwidth), the window, the FFT's and
 * the folding's twiddles, the previous frame's samples, the powers'
 * tables, the codewords by symbol, the reservoir's bits, and the frame's
 * channels.
 */
struct media_aac_encoder {
	unsigned rate;
	unsigned channels;
	unsigned bitrate;
	unsigned frequency_index;
	const struct media_aac_bands *bands;
	unsigned coded_bands;
	double window[AAC_WINDOW];
	double fft_cos[AAC_FFT / 2U];
	double fft_sin[AAC_FFT / 2U];
	double pre_cos[AAC_FFT];
	double pre_sin[AAC_FFT];
	double post_cos[AAC_FFT];
	double post_sin[AAC_FFT];
	double previous[AAC_CHANNELS_MAX][AAC_COEFFICIENTS];
	double gain34[AAC_SCALE_MAX + 1];
	double gain[AAC_SCALE_MAX + 1];
	double pow43[AAC_QUANT_MAX + 1];
	struct aac_code codes[12][AAC_BOOK_SYMBOLS];
	long reservoir;
	struct aac_channel frame[AAC_CHANNELS_MAX];
	unsigned ms[AAC_BANDS_MAX];
};

/* Bits written into a unit (MSB first). */
struct aac_writer {
	uint8_t *data;
	size_t size;
	size_t position;
};

static void aac_tables(struct media_aac_encoder *encoder);
static void aac_mdct(const struct media_aac_encoder *encoder, const double *input, double *output);
static void aac_fft(const struct media_aac_encoder *encoder, double *real, double *imaginary);
static void aac_analyse(struct media_aac_encoder *encoder, const int16_t *pcm);
static void aac_stereo(struct media_aac_encoder *encoder);
static long aac_choose(struct media_aac_encoder *encoder, double offset);
static void aac_band_scale(struct media_aac_encoder *encoder, struct aac_channel *channel, unsigned band, double allowed);
static double aac_band_noise(const struct media_aac_encoder *encoder, const struct aac_channel *channel, unsigned band, int scale, int *overflow);
static void aac_band_quantize(const struct media_aac_encoder *encoder, struct aac_channel *channel, unsigned band);
static void aac_band_book(struct media_aac_encoder *encoder, struct aac_channel *channel, unsigned band);
static long aac_band_bits(const struct media_aac_encoder *encoder, const int *quant, unsigned count, unsigned book);
static void aac_scale_limits(struct media_aac_encoder *encoder, struct aac_channel *channel, unsigned bands);
static unsigned aac_max_band(const struct media_aac_encoder *encoder);
static long aac_channel_bits(const struct media_aac_encoder *encoder, const struct aac_channel *channel, unsigned bands);
static long aac_frame_bits(const struct media_aac_encoder *encoder, unsigned bands);
static void aac_write_channel(const struct media_aac_encoder *encoder, struct aac_writer *writer, const struct aac_channel *channel, unsigned bands, int common);
static void aac_write_info(struct aac_writer *writer, unsigned bands);
static void aac_write_band(const struct media_aac_encoder *encoder, struct aac_writer *writer, const int *quant, unsigned count, unsigned book);
static void aac_put(struct aac_writer *writer, uint32_t value, unsigned count);
static unsigned aac_escape_bits(int magnitude);
static void aac_escape_put(struct aac_writer *writer, int magnitude);
static int aac_global_gain(const struct aac_channel *channel, unsigned bands);

/* The books' tuples: their dimension, their radix, the offset of a signed book (0: unsigned), and their largest magnitude. */
static const unsigned aac_book_dimension[12] = {0U, 4U, 4U, 4U, 4U, 2U, 2U, 2U, 2U, 2U, 2U, 2U};
static const unsigned aac_book_radix[12] = {0U, 3U, 3U, 3U, 3U, 9U, 9U, 8U, 8U, 13U, 13U, 17U};
static const int aac_book_offset[12] = {0, 1, 1, 0, 0, 4, 4, 0, 0, 0, 0, 0};
static const int aac_book_largest[12] = {0, 1, 1, 2, 2, 4, 4, 7, 7, 12, 12, 16};

/* The rates the encoder takes, and their sampling frequency indices. */
static const unsigned aac_rates[3] = {48000U, 44100U, 32000U};
static const unsigned aac_rate_indices[3] = {3U, 4U, 5U};

/*
 * Makes an encoder of a rate (32000, 44100 or 48000), one or two
 * channels and a bit rate (16 kbit/s a channel up to 256 kbit/s).
 * Returns 0, EINVAL, or ENOMEM.
 */
int
media_aac_encoder_create(
	unsigned rate,
	unsigned channels,
	unsigned bitrate,
	struct media_aac_encoder **encoder)
{
	struct media_aac_encoder *made;
	unsigned index;
	unsigned found;

	/* The settings the encoder takes. */
	*encoder = NULL;
	found = 3U;
	for (index = 0U; index < 3U; index++) {
		if (aac_rates[index] == rate)
			found = index;
	}

	/* A rate it does not take. */
	if (found == 3U)
		return EINVAL;
	if (channels < 1U || channels > AAC_CHANNELS_MAX)
		return EINVAL;
	if (bitrate < 16000U * channels || bitrate > MEDIA_AAC_BITRATE_MAX)
		return EINVAL;

	/* The encoder. */
	made = calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->rate = rate;
	made->channels = channels;
	made->bitrate = bitrate;
	made->frequency_index = aac_rate_indices[found];
	made->bands = &media_aac_bands[made->frequency_index];
	aac_tables(made);

	/* Succeeded: the encoder, its history silent. */
	*encoder = made;
	return 0;
}

/*
 * Encodes one frame (1024 samples of each channel, interleaved) into a
 * raw access unit.  Returns 0 with *length, ENOSPC when size is too small
 * (MEDIA_AAC_UNIT_MAX is always enough).
 */
int
media_aac_encode(
	struct media_aac_encoder *encoder,
	const int16_t *pcm,
	uint8_t *unit,
	size_t size,
	size_t *length)
{
	struct aac_writer writer;
	double low;
	double high;
	double middle;
	double offset;
	long budget;
	long limit;
	long used;
	long frame_bits;
	unsigned tries;
	unsigned bands;
	unsigned band;
	unsigned channel;

	/* Room for the largest unit. */
	*length = 0U;
	if (size < MEDIA_AAC_UNIT_MAX)
		return ENOSPC;

	/* The spectra and each band's allowed noise, then mid/side. */
	aac_analyse(encoder, pcm);
	aac_stereo(encoder);

	/* The bits of this frame: its share of the bit rate and what the reservoir holds, within the syntax's limit. */
	frame_bits = (long)((double)encoder->bitrate * (double)MEDIA_AAC_FRAME_SAMPLES / (double)encoder->rate);
	budget = frame_bits + encoder->reservoir;
	limit = AAC_CHANNEL_BITS * (long)encoder->channels;
	if ((long)(MEDIA_AAC_UNIT_MAX * 8U) < limit)
		limit = (long)(MEDIA_AAC_UNIT_MAX * 8U);
	if (budget > limit)
		budget = limit;

	/* The noise offset: the least (the best) whose bits fit, by halving its range. */
	low = AAC_OFFSET_LOW;
	high = AAC_OFFSET_HIGH;
	for (tries = 0U; tries < AAC_LOOP_TRIES; tries++) {
		middle = (low + high) / 2.0;
		used = aac_choose(encoder, middle);
		if (used <= budget) {
			high = middle;
		} else {
			low = middle;
		}
	}

	/* The offset found, and its choice made again. */
	offset = high;
	used = aac_choose(encoder, offset);

	/* Still too large (the coarsest offset): the highest bands go silent until it fits. */
	bands = aac_max_band(encoder);
	while (used > limit && bands > 0U) {
		bands--;
		for (channel = 0U; channel < encoder->channels; channel++) {
			for (band = bands; band < encoder->coded_bands; band++)
				encoder->frame[channel].book[band] = AAC_BOOK_ZERO;
		}

		/* The bits at the fewer bands. */
		used = aac_frame_bits(encoder, bands);
	}

	/* The bands sent. */
	bands = aac_max_band(encoder);

	/* The unit: one element, END, whole bytes. */
	memset(unit, 0, size);
	writer.data = unit;
	writer.size = size;
	writer.position = 0U;
	if (encoder->channels == 1U) {
		aac_put(&writer, AAC_ID_SCE, 3U);
		aac_put(&writer, 0U, 4U);
		aac_write_channel(encoder, &writer, &encoder->frame[0], bands, 0);
	} else {
		aac_put(&writer, AAC_ID_CPE, 3U);
		aac_put(&writer, 0U, 4U);
		aac_put(&writer, 1U, 1U);
		aac_write_info(&writer, bands);
		aac_put(&writer, 1U, 2U);
		for (band = 0U; band < bands; band++)
			aac_put(&writer, encoder->ms[band], 1U);
		aac_write_channel(encoder, &writer, &encoder->frame[0], bands, 1);
		aac_write_channel(encoder, &writer, &encoder->frame[1], bands, 1);
	}

	/* The end of the unit, then whole bytes. */
	aac_put(&writer, AAC_ID_END, 3U);
	writer.position = (writer.position + 7U) & ~(size_t)7U;

	/* The reservoir keeps what this frame did not use, up to the syntax's limit. */
	encoder->reservoir += frame_bits - (long)writer.position;
	if (encoder->reservoir > limit - frame_bits)
		encoder->reservoir = limit - frame_bits;
	if (encoder->reservoir < 0)
		encoder->reservoir = 0;

	/* Succeeded: one access unit. */
	*length = writer.position / 8U;
	return 0;
}

/*
 * Writes the 7 bytes of an ADTS header (MPEG-4, AAC-LC, no CRC) for an
 * access unit of length bytes.  Returns 0, or EINVAL for a unit too long.
 */
int
media_aac_adts_header(
	const struct media_aac_encoder *encoder,
	size_t length,
	uint8_t header[MEDIA_AAC_ADTS_BYTES])
{
	struct aac_writer writer;
	size_t total;

	/* The frame's length in its 13 bits. */
	total = length + MEDIA_AAC_ADTS_BYTES;
	if (total > 8191U)
		return EINVAL;

	/* Syncword, MPEG-4, layer 0, no CRC, LC, the rate's index, private 0, the channels, four zero flags, the length, fullness 0x7FF, one block. */
	memset(header, 0, MEDIA_AAC_ADTS_BYTES);
	writer.data = header;
	writer.size = MEDIA_AAC_ADTS_BYTES;
	writer.position = 0U;
	aac_put(&writer, 0xfffU, 12U);
	aac_put(&writer, 0U, 1U);
	aac_put(&writer, 0U, 2U);
	aac_put(&writer, 1U, 1U);
	aac_put(&writer, 1U, 2U);
	aac_put(&writer, encoder->frequency_index, 4U);
	aac_put(&writer, 0U, 1U);
	aac_put(&writer, encoder->channels, 3U);
	aac_put(&writer, 0U, 4U);
	aac_put(&writer, (uint32_t)total, 13U);
	aac_put(&writer, 0x7ffU, 11U);
	aac_put(&writer, 0U, 2U);

	/* Succeeded: the header. */
	return 0;
}

/*
 * Writes the 2 bytes of the AudioSpecificConfig (AAC-LC, the rate's
 * index, the channels, 1024-sample frames).  Returns 0.
 */
int
media_aac_audio_specific_config(
	const struct media_aac_encoder *encoder,
	uint8_t config[MEDIA_AAC_CONFIG_BYTES])
{
	struct aac_writer writer;

	/* Object type 2, the index, the channel configuration, then frameLengthFlag, dependsOnCoreCoder and extensionFlag 0. */
	memset(config, 0, MEDIA_AAC_CONFIG_BYTES);
	writer.data = config;
	writer.size = MEDIA_AAC_CONFIG_BYTES;
	writer.position = 0U;
	aac_put(&writer, 2U, 5U);
	aac_put(&writer, encoder->frequency_index, 4U);
	aac_put(&writer, encoder->channels, 4U);
	aac_put(&writer, 0U, 3U);

	/* Succeeded: the configuration. */
	return 0;
}

/*
 * Frees an encoder.
 */
void
media_aac_encoder_destroy(
	struct media_aac_encoder *encoder)
{
	/* The memory, which holds nothing else. */
	free(encoder);
}

/* Makes the window, the twiddles, the powers' tables, the codewords and the bandwidth. */
static void
aac_tables(
	struct media_aac_encoder *encoder)
{
	const struct media_aac_codebook *book;
	double cutoff;
	double per_channel;
	unsigned index;
	unsigned number;
	unsigned band;

	/* The sine window. */
	for (index = 0U; index < AAC_WINDOW; index++)
		encoder->window[index] = sin(AAC_PI * ((double)index + 0.5) / (double)AAC_WINDOW);

	/* The FFT's twiddles, and the DCT-IV's twiddles before and after it. */
	for (index = 0U; index < AAC_FFT / 2U; index++) {
		encoder->fft_cos[index] = cos(2.0 * AAC_PI * (double)index / (double)AAC_FFT);
		encoder->fft_sin[index] = -sin(2.0 * AAC_PI * (double)index / (double)AAC_FFT);
	}

	/* The DCT-IV's twiddles before and after the FFT. */
	for (index = 0U; index < AAC_FFT; index++) {
		encoder->pre_cos[index] = cos(AAC_PI * ((double)index + 0.25) / (double)AAC_COEFFICIENTS);
		encoder->pre_sin[index] = -sin(AAC_PI * ((double)index + 0.25) / (double)AAC_COEFFICIENTS);
		encoder->post_cos[index] = cos(AAC_PI * (double)index / (double)AAC_COEFFICIENTS);
		encoder->post_sin[index] = -sin(AAC_PI * (double)index / (double)AAC_COEFFICIENTS);
	}

	/* The powers: g^(-3/4) and g for each scalefactor, q^(4/3) for each value. */
	for (index = 0U; index <= (unsigned)AAC_SCALE_MAX; index++) {
		encoder->gain[index] = pow(2.0, 0.25 * ((double)index - AAC_SCALE_OFFSET));
		encoder->gain34[index] = pow(encoder->gain[index], -0.75);
	}

	/* q^(4/3) for each value. */
	for (index = 0U; index <= (unsigned)AAC_QUANT_MAX; index++)
		encoder->pow43[index] = pow((double)index, 4.0 / 3.0);

	/* The codewords by symbol, from the decoder's normative books. */
	for (number = 0U; number < 12U; number++) {
		book = &media_aac_huffman_books[number];
		for (index = 0U; index < book->count; index++) {
			if (book->words[index].symbol >= AAC_BOOK_SYMBOLS)
				continue;
			encoder->codes[number][book->words[index].symbol].word = book->words[index].word;
			encoder->codes[number][book->words[index].symbol].length = book->words[index].length;
		}
	}

	/* The bandwidth by the bit rate of a channel, and the bands below it. */
	per_channel = (double)encoder->bitrate / (double)encoder->channels;
	cutoff = 12000.0;
	if (per_channel >= 56000.0)
		cutoff = 15000.0;
	if (per_channel >= 80000.0)
		cutoff = 17000.0;
	if (per_channel >= 112000.0)
		cutoff = 20000.0;
	encoder->coded_bands = encoder->bands->long_count;
	for (band = 0U; band < encoder->bands->long_count; band++) {
		if ((double)encoder->bands->long_offsets[band] * (double)encoder->rate / (double)AAC_WINDOW >= cutoff) {
			encoder->coded_bands = band;
			break;
		}
	}
}

/*
 * Computes the MDCT of 2048 windowed samples into 1024 coefficients (AAC's,
 * twice the plain transform's): folded into a DCT-IV of 1024, which is computed with a complex FFT of
 * 512 between two twiddles.
 */
static void
aac_mdct(
	const struct media_aac_encoder *encoder,
	const double *input,
	double *output)
{
	double folded[AAC_COEFFICIENTS];
	double real[AAC_FFT];
	double imaginary[AAC_FFT];
	double a;
	double b;
	unsigned half;
	unsigned index;

	/* The folding: (a, b, c, d) quarters into (-c' - d, a - b'). */
	half = AAC_COEFFICIENTS / 2U;
	for (index = 0U; index < half; index++)
		folded[index] = -input[3U * half - 1U - index] - input[3U * half + index];
	for (index = half; index < AAC_COEFFICIENTS; index++)
		folded[index] = input[index - half] - input[3U * half - 1U - index];

	/* The pairs, turned before the FFT. */
	for (index = 0U; index < AAC_FFT; index++) {
		a = folded[2U * index];
		b = folded[AAC_COEFFICIENTS - 1U - 2U * index];
		real[index] = a * encoder->pre_cos[index] - b * encoder->pre_sin[index];
		imaginary[index] = a * encoder->pre_sin[index] + b * encoder->pre_cos[index];
	}

	/* The FFT. */
	aac_fft(encoder, real, imaginary);

	/* Turned after it: the even coefficients from the real parts, the odd from the imaginary, counted down. */
	for (index = 0U; index < AAC_FFT; index++) {
		a = real[index] * encoder->post_cos[index] - imaginary[index] * encoder->post_sin[index];
		b = real[index] * encoder->post_sin[index] + imaginary[index] * encoder->post_cos[index];
		output[2U * index] = 2.0 * a;
		output[AAC_COEFFICIENTS - 1U - 2U * index] = -2.0 * b;
	}
}

/* Computes a complex FFT of 512 points in place (radix 2, the bits reversed first). */
static void
aac_fft(
	const struct media_aac_encoder *encoder,
	double *real,
	double *imaginary)
{
	double temporary;
	double twiddle_real;
	double twiddle_imaginary;
	double product_real;
	double product_imaginary;
	unsigned index;
	unsigned reversed;
	unsigned bit;
	unsigned size;
	unsigned step;
	unsigned start;
	unsigned offset;

	/* The order with the index's bits reversed. */
	reversed = 0U;
	for (index = 0U; index < AAC_FFT; index++) {
		if (index < reversed) {
			temporary = real[index];
			real[index] = real[reversed];
			real[reversed] = temporary;
			temporary = imaginary[index];
			imaginary[index] = imaginary[reversed];
			imaginary[reversed] = temporary;
		}

		/* The next index with its bits reversed (a carry from the top). */
		bit = AAC_FFT >> 1;
		while ((reversed & bit) != 0U) {
			reversed &= ~bit;
			bit >>= 1;
		}

		/* Its new top bit. */
		reversed |= bit;
	}

	/* The butterflies, from pairs up to the whole. */
	for (size = 2U; size <= AAC_FFT; size <<= 1) {
		step = AAC_FFT / size;
		for (start = 0U; start < AAC_FFT; start += size) {
			for (offset = 0U; offset < size / 2U; offset++) {
				twiddle_real = encoder->fft_cos[offset * step];
				twiddle_imaginary = encoder->fft_sin[offset * step];
				index = start + offset + size / 2U;
				product_real = real[index] * twiddle_real - imaginary[index] * twiddle_imaginary;
				product_imaginary = real[index] * twiddle_imaginary + imaginary[index] * twiddle_real;
				real[index] = real[start + offset] - product_real;
				imaginary[index] = imaginary[start + offset] - product_imaginary;
				real[start + offset] += product_real;
				imaginary[start + offset] += product_imaginary;
			}
		}
	}
}

/*
 * Makes each channel's spectrum from the previous and this frame's
 * samples, and each band's allowed noise at offset 0 (its width: a level
 * of 1 a coefficient).
 */
static void
aac_analyse(
	struct media_aac_encoder *encoder,
	const int16_t *pcm)
{
	struct aac_channel *channel;
	double samples[AAC_WINDOW];
	unsigned number;
	unsigned index;
	unsigned band;
	unsigned start;
	unsigned end;

	/* Each channel. */
	for (number = 0U; number < encoder->channels; number++) {
		channel = &encoder->frame[number];

		/* The window over the previous frame and this one, this one kept for the next. */
		for (index = 0U; index < AAC_COEFFICIENTS; index++) {
			samples[index] = encoder->previous[number][index] * encoder->window[index];
			encoder->previous[number][index] = (double)pcm[index * encoder->channels + number];
			samples[AAC_COEFFICIENTS + index] = encoder->previous[number][index] * encoder->window[AAC_COEFFICIENTS + index];
		}

		/* The spectrum. */
		aac_mdct(encoder, samples, channel->spectrum);

		/* Each band's allowed noise: its width. */
		for (band = 0U; band < encoder->coded_bands; band++) {
			start = encoder->bands->long_offsets[band];
			end = encoder->bands->long_offsets[band + 1U];
			channel->allowed[band] = (double)(end - start);
		}
	}
}

/*
 * Chooses mid/side for each band of a pair where the side is small next
 * to left and right (M S < L R / 8, a correlated band), and puts the
 * mid and side in their place (both allowed the lesser noise).  Then the
 * powers |x|^(3/4) of every coefficient.
 */
static void
aac_stereo(
	struct media_aac_encoder *encoder)
{
	struct aac_channel *left;
	struct aac_channel *right;
	struct aac_channel *channel;
	double left_energy;
	double right_energy;
	double mid_energy;
	double side_energy;
	double mid;
	double side;
	double allowed;
	unsigned band;
	unsigned index;
	unsigned number;
	unsigned start;
	unsigned end;

	/* A pair only. */
	memset(encoder->ms, 0, sizeof(encoder->ms));
	if (encoder->channels == 2U) {
		left = &encoder->frame[0];
		right = &encoder->frame[1];
		for (band = 0U; band < encoder->coded_bands; band++) {
			/* The energies of the four signals. */
			start = encoder->bands->long_offsets[band];
			end = encoder->bands->long_offsets[band + 1U];
			left_energy = 0.0;
			right_energy = 0.0;
			mid_energy = 0.0;
			side_energy = 0.0;
			for (index = start; index < end; index++) {
				mid = (left->spectrum[index] + right->spectrum[index]) / 2.0;
				side = (left->spectrum[index] - right->spectrum[index]) / 2.0;
				left_energy += left->spectrum[index] * left->spectrum[index];
				right_energy += right->spectrum[index] * right->spectrum[index];
				mid_energy += mid * mid;
				side_energy += side * side;
			}

			/* Left and right stay unless they are correlated. */
			if (mid_energy * side_energy >= left_energy * right_energy / 8.0)
				continue;

			/* Mid and side, each allowed the lesser noise. */
			encoder->ms[band] = 1U;
			allowed = left->allowed[band];
			if (right->allowed[band] < allowed)
				allowed = right->allowed[band];
			left->allowed[band] = allowed;
			right->allowed[band] = allowed;
			for (index = start; index < end; index++) {
				mid = (left->spectrum[index] + right->spectrum[index]) / 2.0;
				side = (left->spectrum[index] - right->spectrum[index]) / 2.0;
				left->spectrum[index] = mid;
				right->spectrum[index] = side;
			}
		}
	}

	/* The powers the quantizer uses. */
	for (number = 0U; number < encoder->channels; number++) {
		channel = &encoder->frame[number];
		for (index = 0U; index < AAC_COEFFICIENTS; index++)
			channel->power[index] = pow(fabs(channel->spectrum[index]), 0.75);
	}
}

/*
 * Chooses each band's scalefactor, values and book at a noise offset
 * (dB on the allowed noise).  Returns the frame's bits.
 */
static long
aac_choose(
	struct media_aac_encoder *encoder,
	double offset)
{
	struct aac_channel *channel;
	double factor;
	unsigned number;
	unsigned band;

	/* Each band of each channel. */
	factor = pow(10.0, offset / 10.0);
	for (number = 0U; number < encoder->channels; number++) {
		channel = &encoder->frame[number];
		for (band = 0U; band < encoder->coded_bands; band++)
			aac_band_scale(encoder, channel, band, channel->allowed[band] * factor);

		/* The differences the syntax can code, then the values and the books. */
		aac_scale_limits(encoder, channel, encoder->coded_bands);
		for (band = 0U; band < encoder->coded_bands; band++) {
			if (channel->book[band] == AAC_BOOK_ZERO)
				continue;
			aac_band_quantize(encoder, channel, band);
			aac_band_book(encoder, channel, band);
		}
	}

	/* Succeeded: the bits. */
	return aac_frame_bits(encoder, aac_max_band(encoder));
}

/*
 * Chooses a band's scalefactor: the coarsest (the largest) whose noise
 * stays under the allowed noise, between the finest that does not pass
 * the largest value and the coarsest, by halving the range (the noise
 * grows with the scalefactor); the finest when none is good enough.  A
 * band whose energy is under the allowed noise is silent (book zero).
 */
static void
aac_band_scale(
	struct media_aac_encoder *encoder,
	struct aac_channel *channel,
	unsigned band,
	double allowed)
{
	double noise;
	int low;
	int high;
	int middle;
	int overflow;

	/* Silent when nothing need be sent. */
	noise = aac_band_noise(encoder, channel, band, AAC_SCALE_MAX, &overflow);
	if (noise <= allowed) {
		channel->book[band] = AAC_BOOK_ZERO;
		channel->scale[band] = 0;
		return;
	}

	/* The finest scalefactor whose values fit (the coarsest always fits). */
	low = 0;
	high = AAC_SCALE_MAX;
	while (high - low > 1) {
		middle = (low + high) / 2;
		(void)aac_band_noise(encoder, channel, band, middle, &overflow);
		if (overflow) {
			low = middle;
		} else {
			high = middle;
		}
	}

	/* The finest found, or the next when it overflows. */
	(void)aac_band_noise(encoder, channel, band, low, &overflow);
	if (overflow)
		low = high;

	/* The finest is not good enough: it is the best there is. */
	noise = aac_band_noise(encoder, channel, band, low, &overflow);
	channel->book[band] = 1U;
	if (noise > allowed) {
		channel->scale[band] = low;
		return;
	}

	/* The coarsest still good enough, between the finest (good) and the coarsest (not). */
	high = AAC_SCALE_MAX;
	while (high - low > 1) {
		middle = (low + high) / 2;
		noise = aac_band_noise(encoder, channel, band, middle, &overflow);
		if (noise <= allowed) {
			low = middle;
		} else {
			high = middle;
		}
	}

	/* Chosen (its book comes after quantizing). */
	channel->scale[band] = low;
}

/* Measures a band's quantization noise at a scalefactor; *overflow when a value passes the largest. */
static double
aac_band_noise(
	const struct media_aac_encoder *encoder,
	const struct aac_channel *channel,
	unsigned band,
	int scale,
	int *overflow)
{
	double error;
	double noise;
	double inverse;
	double gain;
	unsigned index;
	long value;

	/* Each coefficient quantized and rebuilt. */
	*overflow = 0;
	inverse = encoder->gain34[scale];
	gain = encoder->gain[scale];
	noise = 0.0;
	for (index = encoder->bands->long_offsets[band]; index < encoder->bands->long_offsets[band + 1U]; index++) {
		value = (long)(channel->power[index] * inverse + AAC_ROUNDING);
		if (value > AAC_QUANT_MAX) {
			*overflow = 1;
			value = AAC_QUANT_MAX;
		}

		/* The error of its rebuilt magnitude. */
		error = fabs(channel->spectrum[index]) - encoder->pow43[value] * gain;
		noise += error * error;
	}

	/* Succeeded: the noise. */
	return noise;
}

/* Quantizes a band's coefficients at its scalefactor (signed values). */
static void
aac_band_quantize(
	const struct media_aac_encoder *encoder,
	struct aac_channel *channel,
	unsigned band)
{
	double inverse;
	unsigned index;
	long value;

	/* Each coefficient. */
	inverse = encoder->gain34[channel->scale[band]];
	for (index = encoder->bands->long_offsets[band]; index < encoder->bands->long_offsets[band + 1U]; index++) {
		value = (long)(channel->power[index] * inverse + AAC_ROUNDING);
		if (value > AAC_QUANT_MAX)
			value = AAC_QUANT_MAX;
		channel->quant[index] = (int)value;
		if (channel->spectrum[index] < 0.0)
			channel->quant[index] = -(int)value;
	}
}

/* Chooses the cheapest book for a band's values (book zero when they are all zero). */
static void
aac_band_book(
	struct media_aac_encoder *encoder,
	struct aac_channel *channel,
	unsigned band)
{
	const int *quant;
	unsigned count;
	unsigned book;
	unsigned best;
	unsigned index;
	long bits;
	long least;
	int largest;
	int magnitude;

	/* The band's largest magnitude. */
	quant = &channel->quant[encoder->bands->long_offsets[band]];
	count = encoder->bands->long_offsets[band + 1U] - encoder->bands->long_offsets[band];
	largest = 0;
	for (index = 0U; index < count; index++) {
		magnitude = abs(quant[index]);
		if (magnitude > largest)
			largest = magnitude;
	}

	/* All zero: silent. */
	if (largest == 0) {
		channel->book[band] = AAC_BOOK_ZERO;
		return;
	}

	/* The cheapest book that can hold the largest (the escape book holds any). */
	best = AAC_BOOK_ESCAPE;
	least = aac_band_bits(encoder, quant, count, AAC_BOOK_ESCAPE);
	for (book = 1U; book < AAC_BOOK_ESCAPE; book++) {
		if (aac_book_largest[book] < largest)
			continue;
		bits = aac_band_bits(encoder, quant, count, book);
		if (bits < least) {
			least = bits;
			best = book;
		}
	}

	/* Chosen. */
	channel->book[band] = best;
}

/* Counts the bits of a band's values in a book (codewords, signs, escapes). */
static long
aac_band_bits(
	const struct media_aac_encoder *encoder,
	const int *quant,
	unsigned count,
	unsigned book)
{
	unsigned dimension;
	unsigned index;
	unsigned part;
	unsigned symbol;
	long bits;
	int value;
	int magnitude;

	/* Each tuple. */
	dimension = aac_book_dimension[book];
	bits = 0;
	for (index = 0U; index < count; index += dimension) {
		symbol = 0U;
		for (part = 0U; part < dimension; part++) {
			value = quant[index + part];
			magnitude = abs(value);

			/* A signed book's index holds the sign; an unsigned book's sign is a bit after the codeword. */
			if (aac_book_offset[book] != 0) {
				symbol = symbol * aac_book_radix[book] + (unsigned)(value + aac_book_offset[book]);
				continue;
			}

			/* An unsigned book: a sign bit for a value not zero. */
			if (magnitude != 0)
				bits++;

			/* The escape book's large magnitudes. */
			if (book == AAC_BOOK_ESCAPE && magnitude >= 16) {
				bits += (long)aac_escape_bits(magnitude);
				magnitude = 16;
			}

			/* Its digit in the index. */
			symbol = symbol * aac_book_radix[book] + (unsigned)magnitude;
		}

		/* The tuple's codeword. */
		bits += encoder->codes[book][symbol].length;
	}

	/* Succeeded: the bits. */
	return bits;
}

/*
 * Keeps each scalefactor within 60 of the previous coded band's (the
 * differences book 0 codes); a band moved requantizes later.
 */
static void
aac_scale_limits(
	struct media_aac_encoder *encoder,
	struct aac_channel *channel,
	unsigned bands)
{
	unsigned band;
	int previous;
	int first;

	/* Each coded band after the first. */
	(void)encoder;
	first = 1;
	previous = 0;
	for (band = 0U; band < bands; band++) {
		if (channel->book[band] == AAC_BOOK_ZERO)
			continue;
		if (!first) {
			if (channel->scale[band] > previous + 60)
				channel->scale[band] = previous + 60;
			if (channel->scale[band] < previous - 60)
				channel->scale[band] = previous - 60;
		}

		/* The next band compares with this one. */
		first = 0;
		previous = channel->scale[band];
	}
}

/* Finds the bands to send: one past the highest band not silent in any channel. */
static unsigned
aac_max_band(
	const struct media_aac_encoder *encoder)
{
	unsigned number;
	unsigned band;
	unsigned bands;

	/* The highest band with a book. */
	bands = 0U;
	for (number = 0U; number < encoder->channels; number++) {
		for (band = 0U; band < encoder->coded_bands; band++) {
			if (encoder->frame[number].book[band] != AAC_BOOK_ZERO && band + 1U > bands)
				bands = band + 1U;
		}
	}

	/* Succeeded: the count. */
	return bands;
}

/* Counts the bits of one channel's stream (its global gain, sections, scalefactors, flags, values). */
static long
aac_channel_bits(
	const struct media_aac_encoder *encoder,
	const struct aac_channel *channel,
	unsigned bands)
{
	const int *quant;
	unsigned band;
	unsigned length;
	unsigned count;
	long bits;
	int previous;
	int difference;

	/* The global gain, the three absent tools' flags. */
	bits = 8 + 3;

	/* The sections: each run of one book, its book and its length in 5-bit steps. */
	band = 0U;
	while (band < bands) {
		length = 1U;
		while (band + length < bands && channel->book[band + length] == channel->book[band])
			length++;
		bits += 4 + 5 * (long)(length / 31U + 1U);
		band += length;
	}

	/* The scalefactors' differences, the first against the global gain. */
	previous = aac_global_gain(channel, bands);
	for (band = 0U; band < bands; band++) {
		if (channel->book[band] == AAC_BOOK_ZERO)
			continue;
		difference = channel->scale[band] - previous;
		bits += encoder->codes[0][difference + 60].length;
		previous = channel->scale[band];
	}

	/* The values. */
	for (band = 0U; band < bands; band++) {
		if (channel->book[band] == AAC_BOOK_ZERO)
			continue;
		quant = &channel->quant[encoder->bands->long_offsets[band]];
		count = encoder->bands->long_offsets[band + 1U] - encoder->bands->long_offsets[band];
		bits += aac_band_bits(encoder, quant, count, channel->book[band]);
	}

	/* Succeeded: the bits. */
	return bits;
}

/* Counts a frame's bits at a number of bands: the element, its channels, END, the last byte's padding at most. */
static long
aac_frame_bits(
	const struct media_aac_encoder *encoder,
	unsigned bands)
{
	unsigned number;
	long bits;

	/* The element's id and tag, END and the padding. */
	bits = 3 + 4 + 3 + 7;

	/* A pair's common window, its window's information and its mid/side flags; a single channel's information. */
	if (encoder->channels == 2U) {
		bits += 1 + 11 + 2 + (long)bands;
	} else {
		bits += 11;
	}

	/* Each channel. */
	for (number = 0U; number < encoder->channels; number++)
		bits += aac_channel_bits(encoder, &encoder->frame[number], bands);

	/* Succeeded: the bits. */
	return bits;
}

/* Writes one channel's stream (its window's information too when it is not a pair's common window). */
static void
aac_write_channel(
	const struct media_aac_encoder *encoder,
	struct aac_writer *writer,
	const struct aac_channel *channel,
	unsigned bands,
	int common)
{
	const int *quant;
	unsigned band;
	unsigned length;
	unsigned rest;
	unsigned count;
	int previous;
	int difference;

	/* The global gain, and a single channel's window. */
	previous = aac_global_gain(channel, bands);
	aac_put(writer, (uint32_t)previous, 8U);
	if (!common)
		aac_write_info(writer, bands);

	/* The sections. */
	band = 0U;
	while (band < bands) {
		length = 1U;
		while (band + length < bands && channel->book[band + length] == channel->book[band])
			length++;
		aac_put(writer, channel->book[band], 4U);
		rest = length;
		while (rest >= 31U) {
			aac_put(writer, 31U, 5U);
			rest -= 31U;
		}

		/* The last step, under 31. */
		aac_put(writer, rest, 5U);
		band += length;
	}

	/* The scalefactors. */
	for (band = 0U; band < bands; band++) {
		if (channel->book[band] == AAC_BOOK_ZERO)
			continue;
		difference = channel->scale[band] - previous;
		aac_put(writer, encoder->codes[0][difference + 60].word, encoder->codes[0][difference + 60].length);
		previous = channel->scale[band];
	}

	/* No pulse, TNS or gain control. */
	aac_put(writer, 0U, 3U);

	/* The values. */
	for (band = 0U; band < bands; band++) {
		if (channel->book[band] == AAC_BOOK_ZERO)
			continue;
		quant = &channel->quant[encoder->bands->long_offsets[band]];
		count = encoder->bands->long_offsets[band + 1U] - encoder->bands->long_offsets[band];
		aac_write_band(encoder, writer, quant, count, channel->book[band]);
	}
}

/* Writes a long window's information: reserved, only long, the sine shape, the bands, no prediction. */
static void
aac_write_info(
	struct aac_writer *writer,
	unsigned bands)
{
	/* The fields. */
	aac_put(writer, 0U, 1U);
	aac_put(writer, 0U, 2U);
	aac_put(writer, 0U, 1U);
	aac_put(writer, bands, 6U);
	aac_put(writer, 0U, 1U);
}

/* Writes a band's values in its book: each tuple's codeword, its sign bits, then its escapes. */
static void
aac_write_band(
	const struct media_aac_encoder *encoder,
	struct aac_writer *writer,
	const int *quant,
	unsigned count,
	unsigned book)
{
	const struct aac_code *code;
	unsigned dimension;
	unsigned index;
	unsigned part;
	unsigned symbol;
	int value;
	int magnitude;

	/* Each tuple. */
	dimension = aac_book_dimension[book];
	for (index = 0U; index < count; index += dimension) {
		/* Its index. */
		symbol = 0U;
		for (part = 0U; part < dimension; part++) {
			value = quant[index + part];
			magnitude = abs(value);
			if (aac_book_offset[book] != 0) {
				symbol = symbol * aac_book_radix[book] + (unsigned)(value + aac_book_offset[book]);
			} else {
				if (book == AAC_BOOK_ESCAPE && magnitude > 16)
					magnitude = 16;
				symbol = symbol * aac_book_radix[book] + (unsigned)magnitude;
			}
		}

		/* The codeword. */
		code = &encoder->codes[book][symbol];
		aac_put(writer, code->word, code->length);

		/* An unsigned book's signs, 1 for a negative value. */
		if (aac_book_offset[book] != 0)
			continue;
		for (part = 0U; part < dimension; part++) {
			if (quant[index + part] != 0)
				aac_put(writer, quant[index + part] < 0, 1U);
		}

		/* The escape book's large magnitudes. */
		if (book != AAC_BOOK_ESCAPE)
			continue;
		for (part = 0U; part < dimension; part++) {
			magnitude = abs(quant[index + part]);
			if (magnitude >= 16)
				aac_escape_put(writer, magnitude);
		}
	}
}

/* Writes count bits of value, the highest first (bits past the room are not written). */
static void
aac_put(
	struct aac_writer *writer,
	uint32_t value,
	unsigned count)
{
	unsigned bit;

	/* Each bit. */
	while (count != 0U) {
		count--;
		bit = (unsigned)((value >> count) & 1U);
		if (writer->position < writer->size * 8U && bit != 0U)
			writer->data[writer->position / 8U] |= (uint8_t)(0x80U >> (writer->position % 8U));
		writer->position++;
	}
}

/* Counts an escape's bits: N - 4 ones, a zero, N bits (N the magnitude's highest bit). */
static unsigned
aac_escape_bits(
	int magnitude)
{
	unsigned width;

	/* The magnitude's highest bit, at least 4. */
	width = 4U;
	while ((magnitude >> (width + 1U)) != 0)
		width++;

	/* Succeeded: prefix, separator, suffix. */
	return (width - 4U) + 1U + width;
}

/* Writes an escape: N - 4 ones, a zero, then the magnitude less 2^N in N bits. */
static void
aac_escape_put(
	struct aac_writer *writer,
	int magnitude)
{
	unsigned width;
	unsigned index;

	/* The magnitude's highest bit, at least 4. */
	width = 4U;
	while ((magnitude >> (width + 1U)) != 0)
		width++;

	/* The prefix, the separator, the rest. */
	for (index = 4U; index < width; index++)
		aac_put(writer, 1U, 1U);
	aac_put(writer, 0U, 1U);
	aac_put(writer, (uint32_t)magnitude - (1U << width), width);
}

/* Gives a channel's global gain: its first coded band's scalefactor (100 when none is coded). */
static int
aac_global_gain(
	const struct aac_channel *channel,
	unsigned bands)
{
	unsigned band;

	/* The first coded band. */
	for (band = 0U; band < bands; band++) {
		if (channel->book[band] != AAC_BOOK_ZERO)
			return channel->scale[band];
	}

	/* Succeeded: none coded. */
	return AAC_SCALE_OFFSET;
}
